#pragma once
// leveldp -- exact-representative layered reachability solver (cube + ship).
//
// First target: lv1 (Stereo Madness): cube -> ship -> cube -> ship, no pads,
// no orbs, no speed changes, nothing grouped/moving.
//
// Architecture: forward layered search over ticks. States are EXACT
// (double y, vy + mode/held/grounded); a per-layer hash keyed by quantized
// (y, vy) only DEDUPES (first representative wins) -- it never snaps state.
// Surviving paths are therefore exactly simulatable input plans; the DP is a
// candidate generator and the final proof is a plain GD replay of the witness
// (project rule).
//
// Sound under-approximations:
//   - ship: any solid contact excludes the branch (no floor/ceiling rides)
//   - cube: solids can only be stood on from above; inner-box side contact
//     kills; ceilings kill
//
// Measured constants (2026-07-30):
//   cube: gravity -0.216/tick, jump vy=11.18 (jump tick: vy set, y frozen),
//         terminal -15, rest at surface top + 15, floor clamp y=105
//   ship: ShipModel::stepVy (bit-exact), box 30x30
//   both: y += 0.225 * vy, dx = 1.29825 (speed 0.9), input latency +2 ticks
//
// usage: leveldp <objrects.csv> [--out plan.txt]
//
// This is the command-line front end as a FUNCTION. It is compiled once (dp/src/dpcore.cpp),
// the mod links that compiled code and calls dp::cliMain in-process, so there is exactly one
// solver entry point: whatever the CLI does with a level, the mod does with it too, in the same
// order and with the same code. leveldp.exe is a three-line main() around this.
#include "dp/reset.hpp"
#include "dp/clearance.hpp"
#include "dp/refwatch.hpp"
#include "dp/search_census.hpp"
#include "dp/model.hpp"
#include "dp/statefile.hpp"
#include "dp/search_dp.hpp"
#include "dp/switch_model.hpp"
#include "dp/removed_flags.gen.hpp"
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>   // _mm_getcsr: see the fpenv line at the top of cliMain
#endif

namespace dp {

// (PlanEdge, planEdges: search_dp.hpp)

// Seconds from cliMain's entry to the end of each part of the preparation a call pays before it
// searches or replays anything: 0 arguments, 1 group recordings, 2 touch/auto triggers, 3 the
// level, 4 the rest of the tables (to the --replay branch or the first layer), 5 the object
// position table inside 2. -1 = not reached. Written on every call, read by the mod after it (cfg
// dpphaseprof); nothing prints it here.
inline double g_prepMark[6] = {-1, -1, -1, -1, -1, -1};

// ---- --walkresume: a ladder attempt's witness walk taken up from an earlier attempt's ----
// Every attempt of a --capladder ends with the witness walk of its plan from t0 (the rollout after
// "reconstructing plan"), and an attempt --ladderprefix resumed near the end of the call still walks
// the whole call: measured on a custom level's 14,432-tick call, the walk was 5-9 s of each later
// attempt against 0.3-0.8 s of search. Under the flag a walk keeps, every g_walkResumeEvery ticks,
// everything its next tick reads and everything the lines after it report (WalkCheckpoint), and a
// later attempt of the SAME ladder whose walk starts from the same world (walkWorldSig) with the
// same plan up to a kept tick takes the walk from there. Anything else walks from t0 as before.
// The cache lives inside one ladder (cliMain sets it up and empties it on the way out); a call
// outside a ladder never sees it.
struct WalkCheckpoint {
    size_t i = 0;                        // lvl[0..i) walked: `s` is the state after tick t0 + i
    State s{};
    uint64_t objRefs[12] = {};           // s's Obj pointers (Model::encodeObj), as --ladderprefix
    std::vector<double> carry;           // frame 0's moving geometry (Dynamics::saveCarry); a walk
                                         // through gameplay rotation is not taken up at all
    std::vector<double> autoLockLastX;   // the one autonomous-trigger field the walk writes
    // the walk's own accumulators, which the lines after it and g_outcome read
    long long resimDead = 0, resimTicks = 0, dieRecon = 0;
    int resimFirst = -1, resimLast = -1, resimUid = -1, resimFrame = -1;
    const char* resimWhy = nullptr;
    uint8_t resimKind = 0;   // the first death's DIE kind, for g_outcome.resimKind only
    float resimPX = 0.f, resimObjX = 0.f, resimObjY = 0.f;
    TouchMask resimTrig{};
    std::vector<std::pair<const char*, long long>> dieReconWhy;
    const char* deadWhy = "";
    uint64_t deadObjRef = 0;
    float deadCx = 0.f, deadCy = 0.f;
    size_t traceLen = 0;                // bytes of the trace rows through tick t0 + i
    uint64_t sticky = 0;                 // frame 0's turnedBox latch here (Dynamics::foldSticky)
    long long fixupHitsWalk = 0;         // g_fixupHits the walk added up to here (the search's
                                         // share differs between attempts, so it is a delta)
    // The spider's pre-warp y (frames.hpp g_preBtnY): per step, read only in the step that wrote
    // it, but left behind for the next; the walk's last write if it has made one.
    bool preBtnSeen = false;
    double preBtnY = 0.0;
    // bands.hpp's camera verdict, computed (and printed, once) on its first use: a walk taken up
    // past that use computes it again, so the line it printed comes out as it did unbroken.
    int bandTrackCam = -1;
};
struct WalkResumeCache {
    bool active = false;                 // --walkresume, inside a ladder (cliMain) and only there
    long long t0 = -1;
    uint64_t worldSig = 0;               // the world the kept walk started from
    std::vector<uint8_t> lvl;            // the plan it walked
    std::string trace;                   // its trace rows (the header is written apart)
    std::vector<uint8_t> modeAt;
    std::vector<WalkCheckpoint> cps;
};
inline WalkResumeCache g_wrc;
inline int g_walkResumeEvery = 256;
// --walkresumecheck (print only): after every witness walk, a fingerprint of what it left (the
// trace rows, modeAt, and every global resetInvocationState covers), and where a walk was taken
// up from; at every call's entry, whether a cache is left over from another call. With and
// without --walkresume, the two runs' lines must agree.
inline bool g_walkResumeCheck = false;
inline uint64_t walkFnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t k = 0; k < n; ++k) { h ^= b[k]; h *= 1099511628211ull; }
    return h;
}
// The world a witness walk starts from, as far as the search that ran before it can have left it
// differently from another attempt's: the anchor and the start state, the autonomous and rotation
// triggers' fire state, the rejoin, and each bound frame's moving geometry (where it was last
// placed and turnedBox's latch). `ok` goes false when the start state holds a pointer encodeObj
// cannot name; such a walk is neither taken up nor kept.
inline uint64_t walkWorldSig(const Model& model, long long t0, const State& init, float rDxF, bool& ok) {
    uint64_t h = 1469598103934665603ull;
    h = walkFnv(h, &t0, sizeof t0);
    h = walkFnv(h, &rDxF, sizeof rDxF);
    State z = init;
    uint64_t refs[12];
    refs[0] = model.encodeObj(z.snapObj, ok);
    refs[1] = model.encodeObj(z.usedOrb, ok);
    refs[2] = model.encodeObj(z.snapObj2, ok);
    refs[3] = model.encodeObj(z.usedOrb2, ok);
    for (int k = 0; k < 4; ++k) refs[4 + k] = model.encodeObj(z.usedPad[k], ok);
    for (int k = 0; k < 4; ++k) refs[8 + k] = model.encodeObj(z.usedPad2[k], ok);
    z.snapObj = z.usedOrb = z.snapObj2 = z.usedOrb2 = nullptr;
    for (int k = 0; k < 4; ++k) z.usedPad[k] = z.usedPad2[k] = nullptr;
    h = walkFnv(h, &z, sizeof z);
    h = walkFnv(h, refs, sizeof refs);
    for (const AutoTrig& A : g_autoTrig) {
        h = walkFnv(h, &A.fireT, sizeof A.fireT);
        h = walkFnv(h, &A.fireX, sizeof A.fireX);
        h = walkFnv(h, &A.fireSpreadT, sizeof A.fireSpreadT);
        h = walkFnv(h, &A.lockLastX, sizeof A.lockLastX);
    }
    for (const RotTrig& R : g_rotTrig) {
        h = walkFnv(h, &R.firedT, sizeof R.firedT);
        h = walkFnv(h, &R.revT, sizeof R.revT);
    }
    h = walkFnv(h, &g_rjJoinT, sizeof g_rjJoinT);
    const int bandCam = g_bandTrackCam.load();
    h = walkFnv(h, &bandCam, sizeof bandCam);   // computed yet or not (bands.hpp)
    for (int f = 0; f < 4; ++f) {
        const uint8_t on = model.frameBound(f) ? 1 : 0;
        h = walkFnv(h, &on, 1);
        if (on) model.levelOf(f).dyn.foldSticky(h);
    }
    return h;
}

// ---- parse caches for the in-process loop ----
// The mod calls cliMain hundreds of times a level -- the ladder's rungs, the fixup recorder's
// replays -- and every call parsed the same level text twice more (the object type and position
// tables) and every group recording again: on SubZero 4003 about 0.13-0.2 s of a 0.25-0.3 s call,
// most of a call that then searches nothing. A table is reused only when the bytes it came from are
// the same bytes, so a hit hands back exactly what parsing them would have. The CLI makes one call
// per process and never hits.
struct LevelTextCache {
    std::string text;
    // The text pass loadLevelFrom does before it reads a row (level_loader.hpp CsvRows): parsed once per level text.
    // --no-csvrows is the off arm, --csvrowscheck loads each level both ways and compares them.
    bool haveRows = false;
    CsvRows rows;
    bool haveTypes = false, havePos = false, haveSpawnX = false;
    std::unordered_map<int, int> types;
    std::unordered_map<int, std::pair<double, double>> pos;
    // --spawnlag's spawn-triggered x (loadObjSpawnX): parsed once per level text like `pos`.
    // It was parsed afresh on every call -- 0.15-0.44 s of each on a custom level's table.
    std::unordered_map<int, double> spawnX;
};
// (namespace scope, not a function's static, so releaseCrossCallCaches below can empty it)
inline LevelTextCache g_levelTextCache;
inline bool g_csvRows = true;         // --no-csvrows: read the level's text pass afresh each call, as before
inline bool g_csvRowsCheck = false;   // --csvrowscheck (debug): a level loaded both ways, compared (cliMain)
inline LevelTextCache& levelTextFor(const std::string& text) {
    LevelTextCache& c = g_levelTextCache;
    if (c.text.size() != text.size() || c.text != text) {
        c = LevelTextCache{};
        c.text = text;
    }
    return c;
}
// The moving-geometry timeline a call's --groups files make: each loaded, the later ones laid over
// the first (overlayGroupTimeline), and g_groupInit set -- kept as a whole, not per file. A call
// used to take its first layer as a COPY out of a per-file cache so the overlay could write into
// it, and that copy of a 1.5M-sample recording, with the character-at-a-time read before it, was
// ~1 s of every call on a custom level (lv20: ~0.6 s). The whole result depends on the files'
// bytes and the two hold settings and nothing else, so a hit on all of them hands back the
// timeline itself, and what the loads and overlays printed is printed again.
//
// A miss re-reads only the files whose bytes changed. The loop replaces the live recording
// (dp_groups.txt) whenever a replay goes deeper, which is nearly every round, while the bootstrap
// and the deep recording under it stay as they are for the whole level -- and re-parsing those was
// most of such a call: 2.4 s of 4.2 s on a long custom level with an 81 MB bootstrap, 4.2 s of 5.9 s with a deep
// recording as large under it (measured 2026-09-30). Each file's parse is kept as it came off the
// file, so the first layer is a copy of it, as it was before the merged cache existed.
struct GroupFileParse {
    std::string path, bytes;
    std::string sig;      // the caller's signature of these bytes (g_fileSigHint), "" when none
    GroupTimeline one;
    long long end = -1;
    std::unordered_map<int, uint8_t> init;
    std::string line;     // the line its load printed
};
struct GroupLayersCache {
    bool valid = false;
    std::vector<GroupFileParse> files;   // the last call's files, in its order
    bool holdDeath = false;
    GroupTimeline merged;
    std::unordered_map<int, uint8_t> init;
    std::string report;   // the lines the loads and overlays printed, in order
    long long endT = -1;  // the largest `end,<tick>` among the files (g_groupsEndT)
};
// The caller's content signature per file path (the mod's fileSig of a recording it wrote, taken
// while writing it), set before each in-process call. A groups file whose signature matches the
// one its cached parse was made from is not read again: reading and comparing every byte was the
// whole cost of a cache hit, 0.3-0.7 s per call on a 293 MB boot recording. Without a signature
// (the CLI, any caller that does not set one) the bytes are read and compared as before.
inline std::unordered_map<std::string, std::string> g_fileSigHint;
// --groupsigcheck (debug, off): on a signature hit, read and compare the bytes as well, and say
// so when the two disagree (the parse is then rebuilt from the bytes).
inline bool g_groupSigCheck = false;
// Bumped every time groupLayersFor builds the timeline instead of handing back the one it has,
// so a table made from the timeline can tell that it is still the same one.
inline unsigned long long g_groupLayersGen = 0;
inline GroupLayersCache g_groupLayersCache;   // (namespace scope: releaseCrossCallCaches empties it)
inline const GroupTimeline& groupLayersFor(const std::vector<std::string>& normalPaths) {
    GroupLayersCache& c = g_groupLayersCache;
    static const GroupTimeline kNone;
    g_groupInit.clear();
    g_groupsEndT = -1;
    // --bootauto: the recordings named by --groupsauto are parsed and cached with the others (the same file signatures, the same
    // byte comparison) and stay out of the timeline: they are the tail of `paths`, which the overlay loop below does not lay down.
    std::vector<std::string> paths = normalPaths;
    const size_t nNormal = paths.size();
    if (g_bootAuto)
        for (const std::string& p : g_groupAutoPaths) paths.push_back(p);
    g_groupAutoTL.clear();
    auto publishAuto = [&]() {
        for (size_t i = nNormal; i < c.files.size(); ++i) g_groupAutoTL.push_back(&c.files[i].one);
    };
    if (paths.empty()) return kNone;
    std::vector<std::string> hint(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto h = g_fileSigHint.find(paths[i]);
        if (h != g_fileSigHint.end()) hint[i] = h->second;
    }
    bool sigHit = c.valid && c.holdDeath == g_groupHoldDeath && c.files.size() == paths.size();
    for (size_t i = 0; i < paths.size() && sigHit; ++i)
        sigHit = !hint[i].empty() && c.files[i].path == paths[i] && c.files[i].sig == hint[i];
    if (sigHit && !g_groupSigCheck) {
        std::fputs(c.report.c_str(), stdout);
        g_groupInit = c.init;
        g_groupsEndT = c.endT;
        publishAuto();
        return c.merged;
    }
    // A file whose signature matches a cached parse of the same path is not read at all (the parse
    // is taken by signature below); the others are read and compared byte for byte as before.
    // --groupsigcheck reads every file, so its byte comparison has something to compare.
    auto cachedBySig = [&](size_t i) -> GroupFileParse* {
        if (hint[i].empty() || g_groupSigCheck) return nullptr;
        for (GroupFileParse& old : c.files)
            if (old.path == paths[i] && old.sig == hint[i]) return &old;
        return nullptr;
    };
    std::vector<std::string> bytes(paths.size());
    std::vector<uint8_t> bySig(paths.size(), 0);
    bool readable = true;
    for (size_t i = 0; i < paths.size() && readable; ++i) {
        if (cachedBySig(i)) { bySig[i] = 1; continue; }
        readable = readFileBytes(paths[i], bytes[i]);
    }
    auto sameFiles = [&]() {
        if (c.files.size() != paths.size()) return false;
        for (size_t i = 0; i < paths.size(); ++i) {
            if (c.files[i].path != paths[i]) return false;
            if (bySig[i] ? c.files[i].sig != hint[i] : c.files[i].bytes != bytes[i]) return false;
        }
        return true;
    };
    const bool byteHit = readable && c.valid && c.holdDeath == g_groupHoldDeath && sameFiles();
    // --groupsigcheck: say what the signature hit was worth, on stdout (the in-process copy's
    // stdout reaches the game's log; stderr does not)
    if (sigHit)
        std::printf("groupsigcheck: %s (%s)\n",
                    byteHit ? "signature hit, bytes agree" : "MISMATCH - a signature hit whose bytes differ",
                    paths[0].c_str());
    if (byteHit) {
        for (size_t i = 0; i < paths.size(); ++i)
            if (!hint[i].empty()) c.files[i].sig = hint[i];
        std::fputs(c.report.c_str(), stdout);
        g_groupInit = c.init;
        g_groupsEndT = c.endT;
        publishAuto();
        return c.merged;
    }
    // A miss: the loop this replaces, printing as it goes -- each file's line, then its overlay's.
    ++g_groupLayersGen;
    std::vector<GroupFileParse> files(paths.size());
    GroupTimeline gt;
    std::string report;
    for (size_t gi = 0; gi < paths.size(); ++gi) {
        GroupFileParse& f = files[gi];
        GroupFileParse* kept = nullptr;
        if (readable && bySig[gi]) {
            kept = cachedBySig(gi);
        } else if (readable) {
            for (GroupFileParse& old : c.files)
                if (old.path == paths[gi] && !old.bytes.empty() && old.bytes == bytes[gi]) {
                    kept = &old;
                    break;
                }
        }
        if (kept) {   // the same bytes as last time: the same parse, and the line it printed
            f = std::move(*kept);
            f.sig = hint[gi];
            kept->path.clear();   // taken; a path given twice parses its second copy anew
            std::fputs(f.line.c_str(), stdout);
        } else {
            f.path = paths[gi];
            f.sig = hint[gi];
            if (readable) {
                f.bytes = std::move(bytes[gi]);
                f.one = parseGroupTimeline(f.bytes, paths[gi], &f.end, &f.init);
            } else {  // an unreadable file says so on stderr and loads as nothing
                f.one = loadGroupTimeline(paths[gi], &f.end, &f.init);
            }
            long long rows = 0;
            for (const auto& kv : f.one) rows += (long long)kv.second.size();
            char b[768];
            std::snprintf(b, sizeof b, "groups: %lld samples for %zu objects (%s)\n", rows,
                          f.one.size(), paths[gi].c_str());
            f.line = b;
        }
        // With a signature the bytes are never compared again (the parse is found by signature),
        // so the copy -- as large as the recording, 293 MB for a custom level's boot -- is let go.
        if (!f.sig.empty() && !g_groupSigCheck) std::string().swap(f.bytes);
        report += f.line;
        if (gi >= nNormal) continue;   // --bootauto: kept apart, not laid over the timeline
        if (g_groupInit.empty()) g_groupInit = f.init;
        if (gi == 0) {
            gt = f.one;
        } else {
            std::string ov;
            overlayGroupTimeline(gt, f.one, g_groupHoldDeath && gi + 1 == files.size(), f.end,
                                 &ov);
            std::fputs(ov.c_str(), stdout);
            report += ov;
        }
    }
    long long endT = -1;
    for (size_t fi = 0; fi < nNormal; ++fi) endT = std::max(endT, files[fi].end);
    g_groupsEndT = endT;
    c = GroupLayersCache{};
    c.merged = std::move(gt);
    c.endT = endT;
    if (readable) {   // an unreadable file is not a state worth remembering
        c.valid = true;
        c.files = std::move(files);
        c.holdDeath = g_groupHoldDeath;
        c.init = g_groupInit;
        c.report = std::move(report);
        publishAuto();
    }
    return c.merged;
}

// The level, shared by the attempts of one --capladder. The ladder (cliMain) runs cliMainOnce
// again for every attempt with the same arguments but for --capmap and --gridmap, which only the
// layer loop reads (capAtX, gridAtX), so every attempt rebuilt the same level. In the panel on
// a custom level (2026-09-25, 32 minutes) that was 257 attempts for 74 searches, each preparing
// for ~1.3 s, the level ~1 s of it. An attempt after the first takes the level the first one
// built: the Level as loadLevel returned it, the globals loadLevelFrom writes (LoadLevelWrites)
// and what it printed.
//
// Only inside one ladder, where the arguments, the input files and the level text are the same
// by construction, and resetInvocationState has put every other global back where the first
// attempt found it. The argument list and the moving-geometry timeline are still compared, as a
// guard rather than as the key. cliMain empties it when the ladder ends: it holds a copy of the
// level, recordings included, and has no use after that.
struct LadderLevelCache {
    unsigned long long ladder = 0;   // the ladder it was built in; 0 = empty
    std::vector<std::string> args;   // that attempt's arguments without --capmap / --gridmap
    unsigned long long groupsGen = 0;
    Level level;
    LoadLevelWrites writes;
    std::string printed;
};
inline unsigned long long g_ladder = 0;   // the ladder in progress (cliMain); 0 = none
// --capladder with another search (--search): that search answers the call once, without the
// ladder (which is the layer DP's own), and a call it declines comes back here
// (g_declinedToLadder) for the ladder to answer with the layer DP.
inline bool g_declineToLadder = false;
inline bool g_declinedToLadder = false;
// (g_ladderBestT, g_ladderStop: search_dp.hpp)
inline LadderLevelCache g_ladderLevel;
// --levelcachecheck (debug, off): a hash of a built level's whole state -- every data member of
// Level and Dynamics, the member list generated from their source -- taken when ReplayLevelCache
// stores a level and again at each reuse (the cached level, and the copy handed to the call), so
// a pass that wrote into the level it was given shows up as a mismatch.
inline bool g_levelCacheCheck = false;
struct LevelHash {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    }
};
template <class T> struct LvhAlwaysFalse : std::false_type {};
template <class T> inline void lvh(LevelHash& H, const T& v) {
    static_assert(std::is_trivially_copyable<T>::value || LvhAlwaysFalse<T>::value,
                  "levelHash: a member type that is neither plain data nor a vector/string");
    H.bytes(&v, sizeof v);
}
inline void lvh(LevelHash& H, const std::string& s) {
    const size_t n = s.size();
    H.bytes(&n, sizeof n);
    H.bytes(s.data(), n);
}
template <class T> inline void lvh(LevelHash& H, const std::vector<T>& v) {
    const size_t n = v.size();
    H.bytes(&n, sizeof n);
    if constexpr (std::is_trivially_copyable<T>::value) {
        if (n) H.bytes(v.data(), n * sizeof(T));
    } else {
        for (const T& x : v) lvh(H, x);
    }
}
inline void levelHashDyn(LevelHash& H, const Dynamics& d) {
    lvh(H, d.objs);
    lvh(H, d.samples);
    lvh(H, d.everRot);
    lvh(H, d.obbBc0);
    lvh(H, d.obbBs0);
    lvh(H, d.shiftSaid);
    lvh(H, d.cur);
    lvh(H, d.bucket);
    lvh(H, d.formula);
    lvh(H, d.prevCy);
    lvh(H, d.lastStep1);
    lvh(H, d.baseCy);
    lvh(H, d.baseSy0);
    lvh(H, d.baseSy1);
    lvh(H, d.on);
    lvh(H, d.onEv);
    lvh(H, d.onInit);
    lvh(H, d.offBox);
    lvh(H, d.trigMask);
    lvh(H, d.recAuto);
    lvh(H, d.trigDx);
    lvh(H, d.trigDy);
    lvh(H, d.trigDur);
    lvh(H, d.trigEase);
    lvh(H, d.trigErate);
    lvh(H, d.trigRecFire);
    lvh(H, d.boxRecEntry);
    lvh(H, d.recLag);
    lvh(H, d.autoLag);
    lvh(H, d.autoClosed);
    lvh(H, d.autoAnchor);
    lvh(H, d.anchorUnrec);
    lvh(H, d.recAnchorX);   // only under the flag: the off arm hashes as before
    // the same
    lvh(H, d.areaOf);
    lvh(H, d.areaBaseX);
    lvh(H, d.areaBaseY);
    lvh(H, d.areaBaseHa);
    lvh(H, d.areaBaseHb);
    lvh(H, d.areaBaseRot);
    lvh(H, d.areaArmed);
    lvh(H, d.areaAdd);
    if (g_tapDisarm) {                          // the same
        lvh(H, d.disarmEv);
        lvh(H, d.disarmInit);
    }
    lvh(H, d.armDep);             // the same
    lvh(H, d.obbLatchT);     // the same
    // the same
    lvh(H, d.tailOnEv);
    lvh(H, d.tailOnBox);
    lvh(H, d.tailRecEnd);
    for (const Dynamics::RigTail& r : d.rigTail) {
        lvh(H, r.t0); lvh(H, r.lockA); lvh(H, r.lockB);
        lvh(H, r.x); lvh(H, r.y); lvh(H, r.r);
    }
    lvh(H, d.recSelfFire);
    if (g_touchRecLeads) lvh(H, d.touchLeads);   // only under the flag: the off arm hashes as before
    lvh(H, d.autoReach);
    lvh(H, d.autoDx);
    lvh(H, d.autoDy);
    lvh(H, d.autoDur);
    lvh(H, d.autoEase);
    lvh(H, d.autoErate);
    lvh(H, d.autoParts);
    lvh(H, d.rotSplit);
    lvh(H, d.touchParts);
    lvh(H, d.autoLock);
    lvh(H, d.autoLockTrig);
    lvh(H, d.touchLock);
    lvh(H, d.anyTrig);
    lvh(H, d.anyAuto);
    lvh(H, d.lastT);
    lvh(H, d.seekEpoch);
    lvh(H, d.pureEpoch);
    lvh(H, d.pureT);
    lvh(H, d.pureFire);
    lvh(H, d.byBucket);
    lvh(H, d.anyUidM1);
    lvh(H, d.nNonPure);
}
inline uint64_t levelHashOf(const Level& L) {
    LevelHash H;
    lvh(H, L.objs);
    lvh(H, L.portals);
    lvh(H, L.pads);
    lvh(H, L.orbs);
    lvh(H, L.speeds);
    lvh(H, L.slopes);
    lvh(H, L.coins);
    lvh(H, L.coinDyn);
    levelHashDyn(H, L.dyn);
    lvh(H, L.topAhead);
    lvh(H, L.bucketX0);
    lvh(H, L.maxX);
    lvh(H, L.maxY);
    lvh(H, L.unsupported);
    lvh(H, L.gpHandoff);
    return H.h;
}
// The level one --replay call built, for the next --replay call whose inputs are the same: the
// recorder flies the same plan from the same anchor two to four times, adding fixups between the
// passes, and the level does not read the fixups (level_loader.hpp has no use of them). The key
// is the whole argument list with the --fixups value left out, the moving-geometry timeline
// (g_groupLayersGen), the in-process level text, and the bytes of every other file argument --
// compared, not assumed. One level at a time, and emptied by any call that is not a --replay, so
// a copy of a large level does not stay beside the searches'.
struct ReplayLevelCache {
    bool valid = false;
    std::vector<std::string> args;
    unsigned long long groupsGen = 0;
    std::string csv;
    std::vector<std::pair<std::string, std::string>> files;   // path, bytes
    Level level;
    LoadLevelWrites writes;
    std::string printed;
    uint64_t hash = 0;   // --levelcachecheck: levelHashOf(level) when stored
};
inline ReplayLevelCache g_replayLevel;

// The tables above, kept from one call to the next on purpose, let go of -- for the mod, when the
// solving is over (dpcore releaseCaches). Each is handed out only when the bytes it was made from
// come again, so emptying one costs the next call a parse and changes nothing it returns; but
// nothing else ever empties them, and after a solve they hold the level's recordings parsed (the
// merged timeline and each file's own parse -- a custom level's boot recording runs to 293 MB of
// text) for the rest of the game. Each is moved out into a local that is then destroyed, so the
// memory goes with it whatever the containers' assignments keep: `x = {}` on a vector, a string or
// an unordered_map is the initializer-list assignment, which empties it and keeps its buffer (or
// bucket array) as clear() does. Never while a call is running: the caller asks between calls.
// The CLI makes one call and never asks.
template <class T> inline void dropHeld(T& x) {
    T gone = std::move(x);   // destroyed at the end of this scope, its buffers with it
    x = T{};
}
inline void releaseCrossCallCaches() {
    dropHeld(g_levelTextCache);
    dropHeld(g_groupLayersCache);
    dropHeld(g_trigRowsCache);
    dropHeld(g_groupMembersCache);
    dropHeld(g_replayLevel);
    dropHeld(g_fileSigHint);
    dropHeld(g_levelCsv);   // the caller's, cleared after every call but its capacity kept
}
// What the tables above hold allocated, approximately (string and timeline capacities; a map's
// nodes and buckets and the replay level's own tables are not counted), for the mod's line that
// says what releaseCrossCallCaches gave back.
inline size_t crossCallCacheBytes() {
    auto tl = [](const GroupTimeline& g) {
        size_t n = 0;
        for (const auto& kv : g) n += kv.second.capacity() * sizeof(DynSample);
        return n;
    };
    size_t n = g_levelTextCache.text.capacity() + g_levelTextCache.rows.filtered.capacity()
               + g_levelTextCache.rows.places.capacity() * sizeof(CsvRows::Place)
               + g_levelCsv.capacity() + tl(g_groupLayersCache.merged)
               + g_groupLayersCache.report.capacity() + g_trigRowsCache.bytes.capacity()
               + g_groupMembersCache.bytes.capacity() + g_replayLevel.csv.capacity()
               + g_replayLevel.printed.capacity();
    for (const GroupFileParse& f : g_groupLayersCache.files) n += f.bytes.capacity() + tl(f.one);
    for (const auto& f : g_replayLevel.files) n += f.second.capacity();
    for (const auto& kv : g_groupMembersCache.byGroup) n += kv.second.capacity() * sizeof(int);
    return n;
}
// ...the part of it that is the moving-geometry recordings (the merged timeline and each file's parse and
// bytes), for the mod's memory census (cfg memcensus).
inline size_t crossCallGroupBytes() {
    auto tl = [](const GroupTimeline& g) {
        size_t n = 0;
        for (const auto& kv : g) n += kv.second.capacity() * sizeof(DynSample);
        return n;
    };
    size_t n = tl(g_groupLayersCache.merged);
    for (const GroupFileParse& f : g_groupLayersCache.files) n += f.bytes.capacity() + tl(f.one);
    return n;
}

inline std::vector<std::string> ladderArgs(int argc, char** argv) {
    std::vector<std::string> a;
    for (int i = 0; i < argc; ++i) {
        // (--search: the level does not depend on it, and a call another search declined hands
        // its level to the ladder that answers it with the layer DP -- cliMain)
        if (i + 1 < argc
            && (!std::strcmp(argv[i], "--capmap") || !std::strcmp(argv[i], "--gridmap")
                || !std::strcmp(argv[i], "--search"))) {
            ++i;
            continue;
        }
        a.push_back(argv[i]);
    }
    return a;
}

// --phaseprof / --incplacecheck: what the incremental placement did in this call, printed whichever way the call ends
// (a search, a replay, a refusal)
struct IncReport {
    ~IncReport() {
        if (!(g_phaseProf || g_incCheck)) return;
        const IncStats& s = g_incStats;
        std::printf("incplace: seeks full=%lld inc=%lld (placed %lld of %lld object-ticks) applyTriggers lists=%lld "
                    "(%lld objects) plain=%lld builds=%lld (%.1f ms) failed=%lld | check: %lld compared, %lld disagree\n",
                    s.seekFull, s.seekInc, s.procInc, s.objsInc, s.atLists, s.atListObjs, s.atFull, s.builds,
                    s.buildMs, s.buildFail, s.checked, s.mismatch);
        if (g_incAnchor)
            std::printf("incanchor: slots/tick=%.0f placed/tick=%.0f (%.2f%%) over %lld ticks; wake calls %lld, %lld objects woken\n",
                        s.pTicks ? (double)s.pSlots / (double)s.pTicks : 0.0, s.pTicks ? (double)s.pPlaced / (double)s.pTicks : 0.0,
                        s.pSlots ? 100.0 * (double)s.pPlaced / (double)s.pSlots : 0.0, s.pTicks, s.pWakes, s.pWokeObjs);
    }
};

inline int cliMainOnce(int argc, char** argv) {
    // --phaseprof's `prep=`: from here to the first layer -- reading the level and every input
    // file and building the tables -- which a call pays whether or not it then searches at all.
    const auto cliT0 = std::chrono::steady_clock::now();
    const IncReport incReport;
    for (double& m : g_prepMark) m = -1;
    g_groupSigCheck = false;   // --groupsigcheck is per call (set again by the argument loop below)
    g_levelCacheCheck = false;   // --levelcachecheck likewise
    g_csvRows = true;            // --no-csvrows / --csvrowscheck likewise
    g_csvRowsCheck = false;
    g_rotFitCheck = false;       // --rotfitcheck likewise (level_loader.hpp)
    g_rotFitChecked = g_rotFitMismatch = 0;
    g_incPlace = true;           // --no-incplace / --incplacecheck likewise (dynamics.hpp)
    g_incCheck = false;
    g_incAnchor = true;          // --no-incanchor is the off arm (dynamics.hpp)
    g_stateCross = true;         // --no-statecross is the off arm (search_key.hpp)
    g_crossMinLag = 3;
    g_crossLagUnit = 4;
    g_crossCxHeads = false;
    g_crossBehind = true;
    g_crossNoted = 0;
    g_crossOver = 0;
    g_incStats = IncStats{};
    ++g_dynPlaceGen;             // a cached Level's incremental index is rebuilt for each call
    g_walkResumeCheck = false;   // --walkresumecheck likewise
    g_needSizeT = -1;            // --needsize (search_dp.hpp), per call
    g_needSpeedX = -1.0;         // --needspeed likewise
    g_needSpeedNot = false;
    g_dumpStateT = -1;           // --dumpstate / --start-state likewise (statefile.hpp)
    g_dumpStatePath.clear();
    g_startStatePath.clear();
    g_planPrevHeld = -1;
    auto prepMark = [&](int k) {
        g_prepMark[k] = std::chrono::duration<double>(std::chrono::steady_clock::now() - cliT0)
                            .count();
    };
    // Whatever the last call concluded must not be readable as this one's answer. Cleared here
    // rather than at the search, so an early return (bad arguments, unreadable level) also
    // leaves "FAILED, nothing measured" behind instead of the previous run's verdict
    g_outcome.reset();
    // ...and neither must anything else the last call left behind. This is a process's worth of
    // state, and only the mod ever runs two solves in one process -- see dp/reset.hpp for what
    // that was costing and how it was measured.
    resetInvocationState();
    // THE FLOATING-POINT ENVIRONMENT IS NOT IN THE OBJECT FILE. The mod's dp
    // core and this CLI are built from these same headers, and on lv22 they
    // disagree -- the run's own walk dies at 1813, the CLI's at 1837, from an
    // argv that has now been checked token for token and a level whose bytes
    // dpselftest says are the same file. The candidates on the table (the
    // mod-only defines, the PCH force-include) are both compile-time stories.
    // This one is not: SSE rounding lives in MXCSR, a per-thread register, and
    // the mod runs inside a process that cocos2d, fmod, the audio backend and
    // the graphics driver have all initialised. Any of them may leave
    // flush-to-zero / denormals-are-zero set, and every arithmetic result in the
    // search would then round differently with byte-identical code. A bisect of
    // defines and PCH cannot see it, so it has to be READ rather than inferred.
    //
    // Printed unconditionally: quick_regress compares the emitted plans and
    // sends stdout to DEVNULL (py/quick_regress.py:717), so this changes no
    // acceptance. `--mxcsr <hex>` then sets it, which is what turns the reading
    // into an experiment -- without the flag nothing here alters a single bit.
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    std::printf("fpenv: mxcsr=0x%04x\n", (unsigned)_mm_getcsr());
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--mxcsr")) {
            _mm_setcsr((unsigned)std::strtoul(argv[i + 1], nullptr, 0));
            std::printf("fpenv: mxcsr set to 0x%04x\n", (unsigned)_mm_getcsr());
        }
#elif defined(__aarch64__)
    // AArch64 keeps flush-to-zero and the rounding mode in FPCR. Read only: --mxcsr is x86's.
    {
        unsigned long long fpcr = 0;
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        std::printf("fpenv: fpcr=0x%08llx\n", fpcr);
    }
#endif
    // --eval-padgate: evaluate orientedHit() -- the SHIPPED pad predicate the
    // step.hpp pad loop calls -- on cases read from stdin, and exit. No level,
    // no search.
    //
    // It exists because py/gdtas/padgate.py is a TRANSCRIPTION of that function,
    // so py/test_leaf_padgate.py's Q1 ("does the predicate return GD's answer?")
    // is green whatever the C++ says: editing object.hpp to disagree with the
    // copy leaves the test passing. `check_transcription` only notices that the
    // copied text moved, which is a staleness alarm, not a proof. Feeding the
    // same cases through here compares the copy against the real thing.
    //
    // The object's rc/rs are taken RAW rather than derived from a rotation,
    // because deriving them here would be a second transcription of the loader
    // and would put the thing under test on both sides of the comparison.
    //
    // `--eval-padgate <cases.txt>`: one case per line, 13 comma- or
    // space-separated numbers
    //   cx,cy,hw,hh,ohw,ohh,rc,rs,oriented,px,py,half,prot
    // stdout: one line per case, `hit=0` or `hit=1`, in order. A line that does
    // not parse gets `hit=?` rather than being skipped, so the caller can never
    // silently line up N inputs against fewer outputs. Blank lines are ignored.
    //
    // A FILE rather than stdin: agent harnesses commonly run with stdin on the
    // null device, and a stdin interface then reads as "the mode is broken"
    // while actually being untestable. A path is testable everywhere.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--eval-padgate")) continue;
        if (i + 1 >= argc) {
            std::printf("--eval-padgate needs a case file\n");
            return 2;
        }
        std::FILE* f = std::fopen(argv[i + 1], "r");
        if (!f) {
            std::printf("--eval-padgate: cannot read %s\n", argv[i + 1]);
            return 2;
        }
        char buf[512];
        while (std::fgets(buf, sizeof buf, f)) {
            std::string line(buf);
            bool blank = true;
            for (char& ch : line) {
                if (ch == ',') ch = ' ';
                else if (!std::isspace((unsigned char)ch)) blank = false;
            }
            if (blank) continue;
            std::istringstream is(line);
            double v[13];
            int n = 0;
            while (n < 13 && (is >> v[n])) ++n;
            if (n < 13) { std::printf("hit=?\n"); continue; }
            Obj o{};
            o.cx = v[0];  o.cy = v[1];  o.hw = v[2];  o.hh = v[3];
            o.ohw = v[4]; o.ohh = v[5]; o.rc = v[6];  o.rs = v[7];
            o.oriented = (v[8] != 0.0);
            std::printf("hit=%d\n",
                        orientedHit(o, v[9], v[10], v[11], v[12]) ? 1 : 0);
        }
        std::fclose(f);
        return 0;
    }
    if (argc < 2) {
        std::printf("usage: leveldp <objrects.csv> [--out plan.txt]\n");
        return 2;
    }
    // SWITCHES REMOVED IN A RELEASE CLEAN-UP. Each chose between a behaviour that had been on by default and
    // the one it replaced (the --no- form is the off arm, the plain name the on spelling that had become a
    // no-op), or was an opt-in / A/B switch whose other path went. The table is generated from
    // dp/removed_flags.txt by tools/gen_removed_flags.py: the spelling, the commit that removed it and the
    // commit the refusal names. A caller that still passes one is refused by name rather than ignored. A
    // spelling ending in '=' takes its value in the same token and is matched as a prefix.
    {
        for (int i = 1; i < argc; ++i)
            for (const auto& r : kRemovedFlags) {
                const size_t n = std::strlen(r[0]);
                const bool hit = r[0][n - 1] == '='
                    ? !std::strncmp(argv[i], r[0], n) : !std::strcmp(argv[i], r[0]);
                if (hit) {
                    std::printf("leveldp: %s was removed in %s; the behaviour is fixed "
                                "(since %s)\n", argv[i], r[1], r[2]);
                    std::fprintf(stderr, "leveldp: %s was removed in %s; the behaviour is "
                                 "fixed (since %s)\n", argv[i], r[1], r[2]);
                    return 2;
                }
            }
    }
    std::string outPath = "leveldp_plan.txt";
    // --replay <plan.txt>: DIAGNOSTIC ONLY. Re-simulate a fixed input plan
    // (the driver's own "input=tick,level" format) against the model and
    // report where the model dies, writing the usual .trace.csv for gd_diff.
    // Never feeds the search (cold rule): the frontier machinery is bypassed
    // entirely. Purpose: given a plan KNOWN to clear in GD, decide whether the
    // wall is model fidelity (this dies where GD lives) or search/witness
    // policy (this survives -- the route exists in the model but the search
    // never keeps it).
    std::string replayPath;
    // --start t0,x0,y,vy,mode,grounded,held : re-anchor mid-level from a GD
    // dump state. x0 matters: GD's x is NOT a pure clock -- the stair snap
    // shifts it by up to +/-threshold per stair (see StairParams), so each
    // re-anchor also re-anchors x.
    // The dump now carries m_objectSnappedTo's uid and m_snapDistance (the MOD's
    // `snapuid` / `snapdist` columns), and --start takes them as its 19th/20th
    // fields; see the note there. Anchors from an older dump pass -1 and get the
    // old behaviour (no snapObj, one missed stair).
    long long t0 = 0;
    long long startSnapUid = -1;
    double startSnapDist = 0.0;
    int startFrame = 0;   // --start's 21st field (gframe); gates the pad seeding
    double x0 = -kDx;  // so that x(1) = 0
    // --start's world x, kept before a rotated frame maps x0 into its travel
    // coordinate (see g_trigWinNear).
    double x0World = -1e18;
    int dbgLayers = 0;
    std::string snapLogPath;
    std::vector<std::string> groupsPaths;
    std::string trigPath, grpPath, obbPath, setPath, rotQPath, itemPath;
    std::string areasPath;   // --areas (with --areamove)
    double endPortalArg = 0.0;   // --endportalx (with --goalgd)
    // y, vy, mode, held, grounded, flip (everything after that is zeroed)
    State init{(float)kFloorY, 0.f, 0, 0, 1, 0};
    // value-less flags get their own loop: the one below stops at argc-1 (every
    // option there reads argv[i+1]), so a flag passed LAST would never be seen.
    for (int i = 2; i < argc; ++i) {
        // On by default since v0.1.4: the plain names still parse (and change
        // nothing), the --no- forms are the off arm.
        // value-less, so in this loop and not the argc-1 one below: passed LAST (the mod appends cfg dparg flags last) a flag there is never seen
        if (!std::strcmp(argv[i], "--bootauto")) g_bootAuto = true;   // groups.hpp
        if (!std::strcmp(argv[i], "--no-bootauto")) g_bootAuto = false;
        if (!std::strcmp(argv[i], "--fixreach")) g_fixNearReach = true;   // fixup.hpp
        if (!std::strcmp(argv[i], "--no-fixreach")) g_fixNearReach = false;
        if (!std::strcmp(argv[i], "--needtrig-unseen")) g_needUnseen = true;   // (moved here from the argc-1 loop: value-less)
        if (!std::strcmp(argv[i], "--needtrigrev")) g_needTrigRev = true;
        if (!std::strcmp(argv[i], "--no-needtrigrev")) g_needTrigRev = false;
        if (!std::strcmp(argv[i], "--dualorblate")) g_dualOrbLate = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-dualorblate")) g_dualOrbLate = false;
        if (!std::strcmp(argv[i], "--ringflipride")) g_ringFlipRide = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-ringflipride")) g_ringFlipRide = false;
        if (!std::strcmp(argv[i], "--landtapspent")) g_landTapSpent = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-landtapspent")) g_landTapSpent = false;
        if (!std::strcmp(argv[i], "--flyframesup")) g_flyFrameSup = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-flyframesup")) g_flyFrameSup = false;
        if (!std::strcmp(argv[i], "--autotrigframe")) g_autoTrigFrame = true;   // dynamics.hpp
        if (!std::strcmp(argv[i], "--no-autotrigframe")) g_autoTrigFrame = false;
        if (!std::strcmp(argv[i], "--wavepadsink")) g_wavePadSink = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-wavepadsink")) g_wavePadSink = false;
        if (!std::strcmp(argv[i], "--spawnremap")) g_spawnRemap = true;
        if (!std::strcmp(argv[i], "--no-spawnremap")) g_spawnRemap = false;
        // --touchvisual: a Pulse or an Alpha ends a chain walk (triggers.hpp)
        // --recleads: an autonomous object whose recording moves first is read as it (dynamics.hpp)
        // --touchrecleads: a touch object no box moves, whose recording moves first, is read as it (dynamics.hpp)
        if (!std::strcmp(argv[i], "--touchrecleads")) g_touchRecLeads = true;
        if (!std::strcmp(argv[i], "--no-touchrecleads")) g_touchRecLeads = false;
        if (!std::strcmp(argv[i], "--tplatchexact")) g_tpLatchExact = true;
        // --padrefill: a pad the body was already inside is taken as fired (constants.hpp)
        // --spiderorbface: a spider orb jumps from its own facing's gravity (constants.hpp)
        // --dashground: a dash lasts through a landing while the button is held (constants.hpp)
        if (!std::strcmp(argv[i], "--fixgravdual")) g_fixGravDual = true;
        if (!std::strcmp(argv[i], "--no-fixgravdual")) g_fixGravDual = false;
        if (!std::strcmp(argv[i], "--expinzero")) g_expInZero = true;
        if (!std::strcmp(argv[i], "--no-expinzero")) g_expInZero = false;
        if (!std::strcmp(argv[i], "--gravtoggle")) g_gravToggle = true;
        if (!std::strcmp(argv[i], "--touchoff")) g_touchOwnOff = true;
        if (!std::strcmp(argv[i], "--no-touchoff")) g_touchOwnOff = false;
        if (!std::strcmp(argv[i], "--areaunrec")) g_areaUnrec = true;
        if (!std::strcmp(argv[i], "--no-areaunrec")) g_areaUnrec = false;
        if (!std::strcmp(argv[i], "--rotparent")) g_rotParent = true;
        if (!std::strcmp(argv[i], "--no-rotparent")) g_rotParent = false;
        if (!std::strcmp(argv[i], "--recendheldrot")) g_recEndHeldRot = true;
        if (!std::strcmp(argv[i], "--no-recendheldrot")) g_recEndHeldRot = false;
        if (!std::strcmp(argv[i], "--tapdisarm")) g_tapDisarm = true;
        if (!std::strcmp(argv[i], "--no-tapdisarm")) g_tapDisarm = false;
        if (!std::strcmp(argv[i], "--touchobb")) g_touchObb = true;
        if (!std::strcmp(argv[i], "--no-touchobb")) g_touchObb = false;
        if (!std::strcmp(argv[i], "--touchobbrect")) g_touchObbRect = true;
        if (!std::strcmp(argv[i], "--no-touchobbrect")) g_touchObbRect = false;
        if (!std::strcmp(argv[i], "--flipbonk")) g_flipBonk = true;
        if (!std::strcmp(argv[i], "--no-flipbonk")) g_flipBonk = false;
        if (!std::strcmp(argv[i], "--spiderseatsolid")) g_spiderSeatSolid = true;
        if (!std::strcmp(argv[i], "--no-spiderseatsolid")) g_spiderSeatSolid = false;
        if (!std::strcmp(argv[i], "--spiderslopeexit")) g_spiderSlopeExit = true;
        if (!std::strcmp(argv[i], "--crushhithead")) g_crushHitHead = true;
        if (!std::strcmp(argv[i], "--no-crushhithead")) g_crushHitHead = false;
        if (!std::strcmp(argv[i], "--no-spiderslopeexit")) g_spiderSlopeExit = false;
        // --spiderslopeline: the spider's teleport meets a ramp facing it at its line (constants.hpp)
        if (!std::strcmp(argv[i], "--spiderrectall")) g_spiderRectAll = true;
        if (!std::strcmp(argv[i], "--no-spiderrectall")) g_spiderRectAll = false;
        if (!std::strcmp(argv[i], "--rotboost")) g_rotBoost = true;
        if (!std::strcmp(argv[i], "--no-rotboost")) g_rotBoost = false;
        if (!std::strcmp(argv[i], "--rotspeed")) g_rotSpeed = true;
        if (!std::strcmp(argv[i], "--no-rotspeed")) g_rotSpeed = false;
        if (!std::strcmp(argv[i], "--bonkouter")) g_bonkOuter = true;
        if (!std::strcmp(argv[i], "--no-bonkouter")) g_bonkOuter = false;
        if (!std::strcmp(argv[i], "--flyfree")) g_flyFree = true;
        if (!std::strcmp(argv[i], "--camfree")) g_camFree = true;
        if (!std::strcmp(argv[i], "--no-camfree")) g_camFree = false;
        if (!std::strcmp(argv[i], "--no-flyfree")) g_flyFree = false;
        if (!std::strcmp(argv[i], "--groundbandzoom")) g_groundBandZoom = true;
        if (!std::strcmp(argv[i], "--no-groundbandzoom")) g_groundBandZoom = false;
        if (!std::strcmp(argv[i], "--lingerhold")) g_lingerHold = true;
        if (!std::strcmp(argv[i], "--no-lingerhold")) g_lingerHold = false;
        if (!std::strcmp(argv[i], "--warpfly")) g_warpFly = true;
        if (!std::strcmp(argv[i], "--no-warpfly")) g_warpFly = false;
        if (!std::strcmp(argv[i], "--warpflyufo")) g_warpFlyUfo = true;
        if (!std::strcmp(argv[i], "--warpslowonly")) g_warpSlowOnly = true;
        if (!std::strcmp(argv[i], "--no-warpslowonly")) g_warpSlowOnly = false;
        if (!std::strcmp(argv[i], "--no-warpflyufo")) g_warpFlyUfo = false;
        // --warpqueue: a touch- or spawn-triggered time warp is left out (modifiers.hpp)
        if (!std::strcmp(argv[i], "--warpcap")) g_warpCap = true;
        if (!std::strcmp(argv[i], "--no-warpcap")) g_warpCap = false;
        if (!std::strcmp(argv[i], "--wavewarp")) g_waveWarp = true;
        if (!std::strcmp(argv[i], "--no-wavewarp")) g_waveWarp = false;
        // --ctrltouch: a touch-triggered Options trigger's controls switch is fired by the state (modifiers.hpp)
        if (!std::strcmp(argv[i], "--ctrlwintrig")) g_ctrlWinTrig = true;
        if (!std::strcmp(argv[i], "--no-ctrlwintrig")) g_ctrlWinTrig = false;
        if (!std::strcmp(argv[i], "--ctrlwavenow")) g_ctrlWaveNow = true;
        if (!std::strcmp(argv[i], "--no-ctrlwavenow")) g_ctrlWaveNow = false;
        if (!std::strcmp(argv[i], "--ctrlliftlag")) g_ctrlLiftLag = true;
        if (!std::strcmp(argv[i], "--no-ctrlliftlag")) g_ctrlLiftLag = false;
        if (!std::strcmp(argv[i], "--ctrlwinlag")) g_ctrlWinLag = true;
        if (!std::strcmp(argv[i], "--no-ctrlwinlag")) g_ctrlWinLag = false;
        if (!std::strcmp(argv[i], "--recstallwide")) g_recStallWide = true;
        if (!std::strcmp(argv[i], "--no-recstallwide")) g_recStallWide = false;
        if (!std::strcmp(argv[i], "--recinterpsize")) g_recInterpSize = true;
        if (!std::strcmp(argv[i], "--no-recinterpsize")) g_recInterpSize = false;
        if (!std::strcmp(argv[i], "--recinterpslow")) g_recInterpSlow = true;
        if (!std::strcmp(argv[i], "--no-recinterpslow")) g_recInterpSlow = false;
        if (!std::strcmp(argv[i], "--flipgracedyn")) g_flipGraceDyn = true;
        if (!std::strcmp(argv[i], "--no-flipgracedyn")) g_flipGraceDyn = false;
        if (!std::strcmp(argv[i], "--camlockx")) g_camLockX = true;
        if (!std::strcmp(argv[i], "--no-camlockx")) g_camLockX = false;
        if (!std::strcmp(argv[i], "--portseenx")) g_portSeenX = true;
        if (!std::strcmp(argv[i], "--no-portseenx")) g_portSeenX = false;
        // --spdsameonce: a second speed portal of one speed on a tick is not marked (speed.hpp)
        if (!std::strcmp(argv[i], "--p1soloend")) g_p1SoloEnd = true;
        if (!std::strcmp(argv[i], "--no-p1soloend")) g_p1SoloEnd = false;
        if (!std::strcmp(argv[i], "--flydashring")) g_flyDashRing = true;
        if (!std::strcmp(argv[i], "--no-flydashring")) g_flyDashRing = false;
        if (!std::strcmp(argv[i], "--flypadhead")) g_flyPadHead = true;
        if (!std::strcmp(argv[i], "--no-flypadhead")) g_flyPadHead = false;
        if (!std::strcmp(argv[i], "--dashframe3")) g_dashFrame3 = true;
        if (!std::strcmp(argv[i], "--no-dashframe3")) g_dashFrame3 = false;
        if (!std::strcmp(argv[i], "--crossworldx")) g_crossWorldX = true;
        if (!std::strcmp(argv[i], "--no-crossworldx")) g_crossWorldX = false;
        if (!std::strcmp(argv[i], "--ringmemrot")) g_ringMemRot = true;
        if (!std::strcmp(argv[i], "--no-ringmemrot")) g_ringMemRot = false;
        if (!std::strcmp(argv[i], "--ringrotbox")) g_ringRotBox = true;
        if (!std::strcmp(argv[i], "--no-ringrotbox")) g_ringRotBox = false;
        if (!std::strcmp(argv[i], "--ringrectgate")) g_ringRectGate = true;
        if (!std::strcmp(argv[i], "--no-ringrectgate")) g_ringRectGate = false;
        if (!std::strcmp(argv[i], "--ringflipunturned")) g_ringFlipUnturned = true;
        if (!std::strcmp(argv[i], "--no-ringflipunturned")) g_ringFlipUnturned = false;
        if (!std::strcmp(argv[i], "--ringheldnow")) g_ringHeldNow = true;
        if (!std::strcmp(argv[i], "--spiderpadafter")) g_spiderPadAfter = true;
        if (!std::strcmp(argv[i], "--no-spiderpadafter")) g_spiderPadAfter = false;
        if (!std::strcmp(argv[i], "--spiderorbpass")) g_spiderOrbPass = true;
        if (!std::strcmp(argv[i], "--no-spiderorbpass")) g_spiderOrbPass = false;
        if (!std::strcmp(argv[i], "--dashstopdead")) g_dashStopDead = true;
        if (!std::strcmp(argv[i], "--no-dashstopdead")) g_dashStopDead = false;
        if (!std::strcmp(argv[i], "--tppinvy")) g_tpPinVy = true;
        if (!std::strcmp(argv[i], "--no-tppinvy")) g_tpPinVy = false;
        if (!std::strcmp(argv[i], "--robothoverwarp")) g_robotHoverWarp = true;
        if (!std::strcmp(argv[i], "--no-robothoverwarp")) g_robotHoverWarp = false;
        if (!std::strcmp(argv[i], "--portalunseat")) g_portalUnseat = true;
        if (!std::strcmp(argv[i], "--no-portalunseat")) g_portalUnseat = false;
        if (!std::strcmp(argv[i], "--pinwarp")) g_pinWarp = true;
        if (!std::strcmp(argv[i], "--no-pinwarp")) g_pinWarp = false;
        if (!std::strcmp(argv[i], "--dashwarp")) g_dashWarp = true;
        if (!std::strcmp(argv[i], "--no-dashwarp")) g_dashWarp = false;
        if (!std::strcmp(argv[i], "--countactgrp")) g_countActGrp = true;
        if (!std::strcmp(argv[i], "--no-countactgrp")) g_countActGrp = false;
        if (!std::strcmp(argv[i], "--swingheldash")) g_swingHelDash = true;
        if (!std::strcmp(argv[i], "--no-swingheldash")) g_swingHelDash = false;
        if (!std::strcmp(argv[i], "--ringride")) g_ringRide = true;
        if (!std::strcmp(argv[i], "--no-ringride")) g_ringRide = false;
        if (!std::strcmp(argv[i], "--snapfacebelow")) g_snapFaceBelow = true;
        if (!std::strcmp(argv[i], "--no-snapfacebelow")) g_snapFaceBelow = false;
        if (!std::strcmp(argv[i], "--no-ringheldnow")) g_ringHeldNow = false;
        if (!std::strcmp(argv[i], "--ringturn")) g_ringTurn = true;
        if (!std::strcmp(argv[i], "--no-ringturn")) g_ringTurn = false;
        if (!std::strcmp(argv[i], "--ringrotground")) g_ringRotGround = true;
        if (!std::strcmp(argv[i], "--no-ringrotground")) g_ringRotGround = false;
        if (!std::strcmp(argv[i], "--crushhead")) g_crushHead = true;
        if (!std::strcmp(argv[i], "--no-crushhead")) g_crushHead = false;
        if (!std::strcmp(argv[i], "--sametick")) g_sameTick = true;
        if (!std::strcmp(argv[i], "--no-sametick")) g_sameTick = false;
        if (!std::strcmp(argv[i], "--forcecircle")) g_forceCircle = true;
        if (!std::strcmp(argv[i], "--no-forcecircle")) g_forceCircle = false;
        if (!std::strcmp(argv[i], "--forcelive")) g_forceLive = true;
        if (!std::strcmp(argv[i], "--no-forcelive")) g_forceLive = false;
        if (!std::strcmp(argv[i], "--ringpresseat")) g_ringPressEat = true;
        if (!std::strcmp(argv[i], "--no-ringpresseat")) g_ringPressEat = false;
        if (!std::strcmp(argv[i], "--spawnringon")) g_spawnRingOn = true;
        if (!std::strcmp(argv[i], "--no-spawnringon")) g_spawnRingOn = false;
        if (!std::strcmp(argv[i], "--offafter")) g_offAfter = true;
        if (!std::strcmp(argv[i], "--no-offafter")) g_offAfter = false;
        if (!std::strcmp(argv[i], "--acthazoff")) g_actHazOff = true;
        if (!std::strcmp(argv[i], "--no-acthazoff")) g_actHazOff = false;
        if (!std::strcmp(argv[i], "--actlookchain")) g_actLookChain = true;
        if (!std::strcmp(argv[i], "--no-actlookchain")) g_actLookChain = false;
        if (!std::strcmp(argv[i], "--flystepup")) g_flyStepUp = true;
        if (!std::strcmp(argv[i], "--no-flystepup")) g_flyStepUp = false;
        if (!std::strcmp(argv[i], "--ringpobb")) g_ringPobb = true;
        if (!std::strcmp(argv[i], "--no-ringpobb")) g_ringPobb = false;
        if (!std::strcmp(argv[i], "--tporbown")) g_tpOrbOwn = true;
        if (!std::strcmp(argv[i], "--no-tporbown")) g_tpOrbOwn = false;
        if (!std::strcmp(argv[i], "--dashrot")) g_dashRot = true;
        if (!std::strcmp(argv[i], "--no-dashrot")) g_dashRot = false;
        if (!std::strcmp(argv[i], "--dashsprite")) g_dashSprite = true;
        if (!std::strcmp(argv[i], "--no-dashsprite")) g_dashSprite = false;
        if (!std::strcmp(argv[i], "--dashspriteonce")) g_dashSpriteOnce = true;
        if (!std::strcmp(argv[i], "--no-dashspriteonce")) g_dashSpriteOnce = false;
        if (!std::strcmp(argv[i], "--robotrot")) g_robotRot = true;
        if (!std::strcmp(argv[i], "--no-robotrot")) g_robotRot = false;
        if (!std::strcmp(argv[i], "--spiderrot")) g_spiderRot = true;
        if (!std::strcmp(argv[i], "--no-spiderrot")) g_spiderRot = false;
        if (!std::strcmp(argv[i], "--waveslopemini") && i + 1 < argc) g_waveSlopeMini = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--waveslopeminik") && i + 1 < argc)
            std::sscanf(argv[i + 1], "%lf,%lf,%lf", &g_wsMiniR, &g_wsMiniL, &g_wsMiniY);
        if (!std::strcmp(argv[i], "--no-waveslopeminik")) g_wsMiniR = g_wsMiniL = g_wsMiniY = -1.0;
        if (!std::strcmp(argv[i], "--waveringminipobb")) g_waveRingMiniPobb = true;
        if (!std::strcmp(argv[i], "--sectionround")) g_sectionRound = true;
        if (!std::strcmp(argv[i], "--dropa1c")) g_dropA1c = true;
        if (!std::strcmp(argv[i], "--sectionuid")) g_sectionUid = true;
        if (!std::strcmp(argv[i], "--no-sectionuid")) g_sectionUid = false;
        if (!std::strcmp(argv[i], "--no-dropa1c")) g_dropA1c = false;
        if (!std::strcmp(argv[i], "--robotheldmove")) g_robotHeldMove = true;
        if (!std::strcmp(argv[i], "--no-robotheldmove")) g_robotHeldMove = false;
        if (!std::strcmp(argv[i], "--robotbufjump")) g_robotBufJump = true;
        if (!std::strcmp(argv[i], "--no-robotbufjump")) g_robotBufJump = false;
        if (!std::strcmp(argv[i], "--robotheldspent")) g_robotHeldSpent = true;
        if (!std::strcmp(argv[i], "--no-robotheldspent")) g_robotHeldSpent = false;
        if (!std::strcmp(argv[i], "--ceilrobotjump")) g_ceilRobotJump = true;
        if (!std::strcmp(argv[i], "--no-ceilrobotjump")) g_ceilRobotJump = false;
        if (!std::strcmp(argv[i], "--p2touchbox")) g_p2TouchBox = true;
        if (!std::strcmp(argv[i], "--no-p2touchbox")) g_p2TouchBox = false;
        if (!std::strcmp(argv[i], "--spentglobal")) g_spentGlobal = true;
        if (!std::strcmp(argv[i], "--no-spentglobal")) g_spentGlobal = false;
        if (!std::strcmp(argv[i], "--slopelagplain")) g_slopeLagPlain = true;
        if (!std::strcmp(argv[i], "--no-slopelagplain")) g_slopeLagPlain = false;
        if (!std::strcmp(argv[i], "--startrev") && i + 1 < argc) {   // R,C: GD's m_maybeReverseSpeed and m_maybeReverseAcceleration at the anchor
            const char* v = argv[i + 1];
            g_startRevOff = std::atof(v);
            const char* comma = std::strchr(v, ',');
            g_startRevCarry = comma ? std::atof(comma + 1) : 0.0;
        }
        if (!std::strcmp(argv[i], "--dualinlat")) g_dualInLat = true;
        if (!std::strcmp(argv[i], "--no-dualinlat")) g_dualInLat = false;
        if (!std::strcmp(argv[i], "--areaoutsame")) g_areaOutSame = true;
        if (!std::strcmp(argv[i], "--no-areaoutsame")) g_areaOutSame = false;
        if (!std::strcmp(argv[i], "--spiderpadlatch")) g_spiderPadLatch = true;
        if (!std::strcmp(argv[i], "--no-spiderpadlatch")) g_spiderPadLatch = false;
        if (!std::strcmp(argv[i], "--no-sectionround")) g_sectionRound = false;
        if (!std::strcmp(argv[i], "--no-waveringminipobb")) g_waveRingMiniPobb = false;
        if (!std::strcmp(argv[i], "--balldashrot")) g_ballDashRot = true;
        if (!std::strcmp(argv[i], "--no-balldashrot")) g_ballDashRot = false;
        if (!std::strcmp(argv[i], "--stopjump")) g_stopJump = true;
        if (!std::strcmp(argv[i], "--no-stopjump")) g_stopJump = false;
        if (!std::strcmp(argv[i], "--reversetrig")) g_reverseTrig = true;
        if (!std::strcmp(argv[i], "--no-reversetrig")) g_reverseTrig = false;
        if (!std::strcmp(argv[i], "--lawunderflip")) g_lawUnderFlip = true;
        if (!std::strcmp(argv[i], "--no-lawunderflip")) g_lawUnderFlip = false;
        if (!std::strcmp(argv[i], "--teleporttrig")) g_teleportTrig = true;
        if (!std::strcmp(argv[i], "--no-teleporttrig")) g_teleportTrig = false;
        // --tpsideways: a teleport trigger whose target is ahead in x is loaded too (constants.hpp)
        // --tpportalx: a 2902-family portal whose exit is ahead in x moves x too (constants.hpp)
        // --widescan: an object wider than the x-window scan's margin is not stepped over (constants.hpp)
        if (!std::strcmp(argv[i], "--widescan")) g_wideScan = true;
        if (!std::strcmp(argv[i], "--no-widescan")) g_wideScan = false;
        // --padundoflip: a ball's tap a pad undoes is undone for the landing passes too (speed.hpp)
        // --p2xoff: the second dual body keeps its own x offset (constants.hpp); --startp2dx seeds it
        if (!std::strcmp(argv[i], "--startp2dx") && i + 1 < argc) g_startP2dx = std::atof(argv[i + 1]);
        // --dualendrot: a wave that ends the dual as the second body hands over its angle (constants.hpp)
        if (!std::strcmp(argv[i], "--autostop")) g_autoStop = true;
        if (!std::strcmp(argv[i], "--no-autostop")) g_autoStop = false;
        // --rotnospawn: a Rotate's walk does not enter its triggers' targets (dynamics.hpp)
        // --formularec: a formula-driven object reads its recording while its boxes agree (dynamics.hpp)
        // --recprov rec,anchor,reach: whose attempt the recording and the anchor's rows are (dynamics.hpp)
        if (!std::strcmp(argv[i], "--recprov") && i + 1 < argc) {
            long long a = -1, b = -1, c = -1;
            if (std::sscanf(argv[i + 1], "%lld,%lld,%lld", &a, &b, &c) == 3) {
                g_recProvGiven = true;
                g_recProvRec = a;
                g_recProvAnchor = b;
                g_recProvReach = c;
            }
        }
        if (!std::strcmp(argv[i], "--dynobbrot")) g_dynObbRot = true;
        if (!std::strcmp(argv[i], "--no-dynobbrot")) g_dynObbRot = false;
        if (!std::strcmp(argv[i], "--reclagup")) g_recLagUp = true;
        if (!std::strcmp(argv[i], "--no-reclagup")) g_recLagUp = false;
        if (!std::strcmp(argv[i], "--reclag0")) g_recLag0 = true;   // dynamics.hpp
        if (!std::strcmp(argv[i], "--no-reclag0")) g_recLag0 = false;
        if (!std::strcmp(argv[i], "--forceraw")) g_forceRaw = true;   // modifiers.hpp
        if (!std::strcmp(argv[i], "--no-forceraw")) g_forceRaw = false;
        if (!std::strcmp(argv[i], "--movingwallup")) g_movingWallUp = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-movingwallup")) g_movingWallUp = false;
        if (!std::strcmp(argv[i], "--ballcontride")) g_ballContRide = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-ballcontride")) g_ballContRide = false;
        if (!std::strcmp(argv[i], "--stickuphill")) g_stickUphill = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-stickuphill")) g_stickUphill = false;
        if (!std::strcmp(argv[i], "--waveceilend")) g_waveCeilEnd = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-waveceilend")) g_waveCeilEnd = false;
        if (!std::strcmp(argv[i], "--gravspawn")) g_gravSpawn = true;
        if (!std::strcmp(argv[i], "--no-gravspawn")) g_gravSpawn = false;
        // --gravchain / --tptrigspawn: a spawned gravity / teleport trigger at the x its chain fires it (modifiers.hpp)
        if (!std::strcmp(argv[i], "--gravchain")) g_gravChain = true;
        if (!std::strcmp(argv[i], "--no-gravchain")) g_gravChain = false;
        if (!std::strcmp(argv[i], "--tptrigspawn")) g_tpTrigSpawn = true;
        if (!std::strcmp(argv[i], "--no-tptrigspawn")) g_tpTrigSpawn = false;
        // --sdelaychains: the old chain walks read the Spawn delays (triggers.hpp chainDelayS)
        if (!std::strcmp(argv[i], "--sdelaychains")) g_spawnDelayChains = true;
        if (!std::strcmp(argv[i], "--no-sdelaychains")) g_spawnDelayChains = false;
        // --gravtouch: a touch-triggered gravity trigger fired by the touching state; --startgmod V seeds it
        if (!std::strcmp(argv[i], "--gravtouch")) g_gravTouch = true;
        if (!std::strcmp(argv[i], "--no-gravtouch")) g_gravTouch = false;
        if (!std::strcmp(argv[i], "--startgmod") && i + 1 < argc) g_startGmod = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--tptouch")) g_tpTouch = true;
        if (!std::strcmp(argv[i], "--no-tptouch")) g_tpTouch = false;
        if (!std::strcmp(argv[i], "--tptrigforce")) g_tpTrigForce = true;
        if (!std::strcmp(argv[i], "--no-tptrigforce")) g_tpTrigForce = false;
        // --tpback: a teleport whose exit is behind moves x back; --startxhi X seeds the farthest x (modifiers.hpp)
        // --groundforce: a force that outpulls gravity lifts a grounded body (modifiers.hpp)
        if (!std::strcmp(argv[i], "--groundforce")) g_groundForce = true;
        if (!std::strcmp(argv[i], "--no-groundforce")) g_groundForce = false;
        if (!std::strcmp(argv[i], "--eventtrig")) g_eventTrig = true;
        if (!std::strcmp(argv[i], "--no-eventtrig")) g_eventTrig = false;
        if (!std::strcmp(argv[i], "--forceboost")) g_forceBoost = true;
        if (!std::strcmp(argv[i], "--no-forceboost")) g_forceBoost = false;
        if (!std::strcmp(argv[i], "--waveportalslope")) g_wavePortalSlope = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-waveportalslope")) g_wavePortalSlope = false;
        if (!std::strcmp(argv[i], "--dashaway")) g_dashAway = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-dashaway")) g_dashAway = false;
        if (!std::strcmp(argv[i], "--spiderorbrot")) g_spiderOrbRot = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-spiderorbrot")) g_spiderOrbRot = false;
        if (!std::strcmp(argv[i], "--bandholdzoom")) g_bandHoldZoom = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-bandholdzoom")) g_bandHoldZoom = false;
        if (!std::strcmp(argv[i], "--tpback")) g_tpBack = true;
        if (!std::strcmp(argv[i], "--no-tpback")) g_tpBack = false;
        if (!std::strcmp(argv[i], "--tptrigonce")) g_tpTrigOnce = true;   // modifiers.hpp
        if (!std::strcmp(argv[i], "--no-tptrigonce")) g_tpTrigOnce = false;
        if (!std::strcmp(argv[i], "--tpjumpnext")) g_tpJumpNext = true;
        if (!std::strcmp(argv[i], "--no-tpjumpnext")) g_tpJumpNext = false;
        if (!std::strcmp(argv[i], "--tptrigoff")) g_tpTrigOff = true;
        if (!std::strcmp(argv[i], "--no-tptrigoff")) g_tpTrigOff = false;
        if (!std::strcmp(argv[i], "--startxhi") && i + 1 < argc) g_startXhi = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--tppush")) g_tpPush = true;
        if (!std::strcmp(argv[i], "--no-tppush")) g_tpPush = false;
        if (!std::strcmp(argv[i], "--tpexitrot")) g_tpExitRot = true;
        if (!std::strcmp(argv[i], "--no-tpexitrot")) g_tpExitRot = false;
        if (!std::strcmp(argv[i], "--tpexitmove")) g_tpExitMove = true;
        if (!std::strcmp(argv[i], "--no-tpexitmove")) g_tpExitMove = false;
        if (!std::strcmp(argv[i], "--tpkeepy")) g_tpKeepY = true;
        if (!std::strcmp(argv[i], "--no-tpkeepy")) g_tpKeepY = false;
        if (!std::strcmp(argv[i], "--nocolone")) g_noColOneWay = true;
        if (!std::strcmp(argv[i], "--no-nocolone")) g_noColOneWay = false;
        if (!std::strcmp(argv[i], "--nocolstatic")) g_noColStatic = true;
        if (!std::strcmp(argv[i], "--no-nocolstatic")) g_noColStatic = false;
        // --slambox: a slam step is skipped by the recorded attempt's own box (constants.hpp)
        // --rectouch uid:tick,... ("-" = none): the boxes GD fired in the recorded attempt (dynamics.hpp)
        if (!std::strcmp(argv[i], "--rectouch") && i + 1 < argc) {
            g_recTouchGiven = true;
            g_recTouch.clear();
            const std::string v = argv[i + 1];
            size_t p = 0;
            while (p < v.size()) {
                size_t q = v.find(',', p);
                if (q == std::string::npos) q = v.size();
                const std::string tok = v.substr(p, q - p);
                const int u = std::atoi(tok.c_str());
                const size_t c = tok.find(':');
                if (u > 0) g_recTouch[u] = (c != std::string::npos) ? std::atoi(tok.c_str() + c + 1) : -1;
                p = q + 1;
            }
        }
        if (!std::strcmp(argv[i], "--moverextendvel")) g_moverExtendVel = true;
        if (!std::strcmp(argv[i], "--recendquiet")) g_recEndQuiet = true;
        if (!std::strcmp(argv[i], "--no-recendquiet")) g_recEndQuiet = false;
        if (!std::strcmp(argv[i], "--ufodownflap")) g_ufoDownFlap = true;
        if (!std::strcmp(argv[i], "--no-ufodownflap")) g_ufoDownFlap = false;
        if (!std::strcmp(argv[i], "--exitjumpseat")) g_exitJumpSeat = true;
        if (!std::strcmp(argv[i], "--no-exitjumpseat")) g_exitJumpSeat = false;
        if (!std::strcmp(argv[i], "--spiderheadx")) g_spiderHeadX = true;
        if (!std::strcmp(argv[i], "--no-spiderheadx")) g_spiderHeadX = false;
        if (!std::strcmp(argv[i], "--lawcubelaunch")) g_lawCubeLaunch = true;
        if (!std::strcmp(argv[i], "--no-lawcubelaunch")) g_lawCubeLaunch = false;
        if (!std::strcmp(argv[i], "--dualbornlinked")) g_dualBornLinked = true;   // frames.hpp
        if (!std::strcmp(argv[i], "--no-dualbornlinked")) g_dualBornLinked = false;
        if (!std::strcmp(argv[i], "--p2held")) g_p2Held = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-p2held")) g_p2Held = false;
        if (!std::strcmp(argv[i], "--p2shared")) g_p2Shared = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-p2shared")) g_p2Shared = false;
        if (!std::strcmp(argv[i], "--seatpast")) g_seatPast = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-seatpast")) g_seatPast = false;
        if (!std::strcmp(argv[i], "--flyridedown")) g_flyRideDown = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-flyridedown")) g_flyRideDown = false;
        if (!std::strcmp(argv[i], "--p2soloafter")) g_p2SoloAfter = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-p2soloafter")) g_p2SoloAfter = false;
        if (!std::strcmp(argv[i], "--ballorbtap")) g_ballOrbTap = true;   // speed.hpp
        if (!std::strcmp(argv[i], "--no-ballorbtap")) g_ballOrbTap = false;
        if (!std::strcmp(argv[i], "--dualjumpbuf")) g_dualJumpBuf = true;
        if (!std::strcmp(argv[i], "--no-dualjumpbuf")) g_dualJumpBuf = false;
        // --ghostlevels <file> / --swapbodies / --bodybit <1|2>: one body of a two-player level against the other's plan (frames.hpp g_ghostOn)
        if (!std::strcmp(argv[i], "--ghostlevels") && i + 1 < argc) {
            std::ifstream gf(argv[i + 1]);
            if (!gf) {
                std::printf("ghostlevels: cannot open %s\n", argv[i + 1]);
                return 2;
            }
            std::vector<std::pair<long long, int>> rows;
            std::string gl;
            while (std::getline(gf, gl)) {
                long long gt;
                int gv;
                if (std::sscanf(gl.c_str(), "%lld,%d", &gt, &gv) == 2) rows.emplace_back(gt, gv);
            }
            g_ghostOn = true;
            g_ghostLvl.clear();
            if (!rows.empty()) {
                long long lo = rows.front().first, hi = rows.front().first;
                for (const auto& r : rows) { lo = std::min(lo, r.first); hi = std::max(hi, r.first); }
                g_ghostBase = lo;
                g_ghostLvl.assign((size_t)(hi - lo + 1), 0);
                for (const auto& r : rows) g_ghostLvl[(size_t)(r.first - lo)] = (uint8_t)(r.second != 0);
            }
        }
        if (!std::strcmp(argv[i], "--swapbodies")) g_swapBodies = true;
        if (!std::strcmp(argv[i], "--startact2") && i + 1 < argc) g_startAct2 = std::atoi(argv[i + 1]) != 0 ? 1 : 0;
        if (!std::strcmp(argv[i], "--bodybit") && i + 1 < argc) g_bodyBit = std::atoi(argv[i + 1]) == 2 ? 2 : 1;
        if (!std::strcmp(argv[i], "--dualbandportal")) g_dualBandPortal = true;
        if (!std::strcmp(argv[i], "--no-dualbandportal")) g_dualBandPortal = false;
        // --mustrelease t0:t1,...: the searched body's button is 0 on t0..t1 (frames.hpp g_mustPress, level 0)
        if (!std::strcmp(argv[i], "--mustrelease") && i + 1 < argc) {
            for (const char* p = argv[i + 1]; *p; ) {
                const long long a0 = std::atoll(p);
                const char* col = std::strchr(p, ':');
                const long long a1 = col ? std::atoll(col + 1) : a0;
                for (long long q = a0; q <= a1; ++q) g_mustPress.push_back({q, 0});
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
            std::sort(g_mustPress.begin(), g_mustPress.end());
            g_mustPress.erase(std::unique(g_mustPress.begin(), g_mustPress.end(),
                                          [](const auto& a, const auto& b) { return a.first == b.first; }), g_mustPress.end());
        }
        if (!std::strcmp(argv[i], "--p2tappress")) g_p2TapPress = true;
        if (!std::strcmp(argv[i], "--no-p2tappress")) g_p2TapPress = false;
        // --mustpress t0:t1,t0:t1: the searched body's button is 1 on t0..t1 and 0 on t1+1 (frames.hpp)
        if (!std::strcmp(argv[i], "--mustpress") && i + 1 < argc) {
            for (const char* p = argv[i + 1]; *p; ) {
                const long long a0 = std::atoll(p);
                const char* col = std::strchr(p, ':');
                const long long a1 = col ? std::atoll(col + 1) : a0;
                for (long long q = a0; q <= a1; ++q) g_mustPress.push_back({q, 1});
                g_mustPress.push_back({a1 + 1, 0});
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
            std::sort(g_mustPress.begin(), g_mustPress.end());
        }
        if (!std::strcmp(argv[i], "--p2taps")) g_p2Taps = true;
        if (!std::strcmp(argv[i], "--no-p2taps")) g_p2Taps = false;
        if (!std::strcmp(argv[i], "--p2secx")) g_p2SecX = true;
        if (!std::strcmp(argv[i], "--no-p2secx")) g_p2SecX = false;
        if (!std::strcmp(argv[i], "--sticktie0")) g_stickTie0 = true;
        if (!std::strcmp(argv[i], "--no-sticktie0")) g_stickTie0 = false;
        if (!std::strcmp(argv[i], "--spidertptouch")) g_spiderTpTouch = true;
        if (!std::strcmp(argv[i], "--no-spidertptouch")) g_spiderTpTouch = false;
        if (!std::strcmp(argv[i], "--rottailmove")) g_rotTailMove = true;
        if (!std::strcmp(argv[i], "--no-rottailmove")) g_rotTailMove = false;
        if (!std::strcmp(argv[i], "--slidelive")) g_slideLive = true;
        if (!std::strcmp(argv[i], "--no-slidelive")) g_slideLive = false;
        if (!std::strcmp(argv[i], "--slideon")) g_slideOn = true;
        if (!std::strcmp(argv[i], "--no-slideon")) g_slideOn = false;
        if (!std::strcmp(argv[i], "--waveflipramp")) g_waveFlipRamp = true;
        if (!std::strcmp(argv[i], "--tpmark")) g_tpMark = true;
        if (!std::strcmp(argv[i], "--no-tpmark")) g_tpMark = false;
        if (!std::strcmp(argv[i], "--tpmarkseed")) g_tpMarkSeed = true;
        if (!std::strcmp(argv[i], "--no-tpmarkseed")) g_tpMarkSeed = false;
        if (!std::strcmp(argv[i], "--no-waveflipramp")) g_waveFlipRamp = false;
        if (!std::strcmp(argv[i], "--lockx")) g_lockX = true;
        if (!std::strcmp(argv[i], "--no-lockx")) g_lockX = false;
        if (!std::strcmp(argv[i], "--locknegdur")) g_lockNegDur = true;
        if (!std::strcmp(argv[i], "--no-locknegdur")) g_lockNegDur = false;
        if (!std::strcmp(argv[i], "--anchormove")) g_anchorMove = true;
        if (!std::strcmp(argv[i], "--no-anchormove")) g_anchorMove = false;
        if (!std::strcmp(argv[i], "--spiderbandground")) g_spiderBandGround = true;
        if (!std::strcmp(argv[i], "--no-spiderbandground")) g_spiderBandGround = false;
        // ...on by default since 2026-10; these are the off arms
        if (!std::strcmp(argv[i], "--no-gravtoggle")) g_gravToggle = false;
        if (!std::strcmp(argv[i], "--no-portalstarty")) g_portalStartY = false;
        if (!std::strcmp(argv[i], "--no-boxoff")) g_boxOff = false;
        if (!std::strcmp(argv[i], "--bandowntrack")) g_bandOwnTrack = true;
        if (!std::strcmp(argv[i], "--no-bandowntrack")) g_bandOwnTrack = false;
        // --bandsrc: a recorded portal band holds a flyer from either side (bands.hpp)
        if (!std::strcmp(argv[i], "--boxoff")) g_boxOff = true;
        // --boxoffown: an off box's objects keep the recording's "gone" only once it fired (dynamics.hpp)
        // --ringtaplink: a ring's gravity turn reaches the partner past a same-tick tap (constants.hpp)
        // --flyheldbox: a ship or UFO holding into a toggle block does not fire it (triggers.hpp)
        if (!std::strcmp(argv[i], "--portalstarty")) g_portalStartY = true;
        if (!std::strcmp(argv[i], "--cubempush")) g_cubeMpush = true;
        // --rampslablift: a face climbing into a ramp rider puts it on top at its speed (constants.hpp)
        // --headpushhold: cubempush's head side holds a body at vy 0 under a face that keeps coming down
        if (!std::strcmp(argv[i], "--mpusharm")) g_mPushArm = true;
        if (!std::strcmp(argv[i], "--no-mpusharm")) g_mPushArm = false;
        // --hazportalonce: a body taking a box-changing mode portal is hazard-tested once, after
        // the portal pass (constants.hpp)
        if (!std::strcmp(argv[i], "--modeslopeorder")) g_modeSlopeOrder = true;
        if (!std::strcmp(argv[i], "--no-modeslopeorder")) g_modeSlopeOrder = false;
        if (!std::strcmp(argv[i], "--spiderportalfirst")) g_spiderPortalFirst = true;
        if (!std::strcmp(argv[i], "--no-spiderportalfirst")) g_spiderPortalFirst = false;
        if (!std::strcmp(argv[i], "--spiderportalcube")) g_spiderPortalCube = true;
        if (!std::strcmp(argv[i], "--no-spiderportalcube")) g_spiderPortalCube = false;
        if (!std::strcmp(argv[i], "--padbandin")) g_padBandIn = true;
        if (!std::strcmp(argv[i], "--no-padbandin")) g_padBandIn = false;
        if (!std::strcmp(argv[i], "--tpmove")) g_tpMove = true;
        if (!std::strcmp(argv[i], "--no-tpmove")) g_tpMove = false;
        if (!std::strcmp(argv[i], "--flipridepast")) g_flipRidePast = true;
        if (!std::strcmp(argv[i], "--no-flipridepast")) g_flipRidePast = false;
        if (!std::strcmp(argv[i], "--portalobb")) g_portalObb = true;
        if (!std::strcmp(argv[i], "--no-portalobb")) g_portalObb = false;
        if (!std::strcmp(argv[i], "--padwaveold")) g_padWaveOld = true;
        if (!std::strcmp(argv[i], "--no-padwaveold")) g_padWaveOld = false;
        if (!std::strcmp(argv[i], "--rideoverspeed")) g_rideOverSpeed = true;
        if (!std::strcmp(argv[i], "--no-rideoverspeed")) g_rideOverSpeed = false;
        if (!std::strcmp(argv[i], "--rideoverspeedunder")) g_rideOverSpeedUnder = true;
        if (!std::strcmp(argv[i], "--no-rideoverspeedunder")) g_rideOverSpeedUnder = false;
        if (!std::strcmp(argv[i], "--ringspeedfirst")) g_ringSpeedFirst = true;
        if (!std::strcmp(argv[i], "--no-ringspeedfirst")) g_ringSpeedFirst = false;
        if (!std::strcmp(argv[i], "--tpportalforce")) g_tpPortalForce = true;
        if (!std::strcmp(argv[i], "--no-tpportalforce")) g_tpPortalForce = false;
        if (!std::strcmp(argv[i], "--forceminifly")) g_forceMiniFly = true;
        if (!std::strcmp(argv[i], "--no-forceminifly")) g_forceMiniFly = false;
        if (!std::strcmp(argv[i], "--lawfrombelowship")) g_lawFromBelowShip = true;
        if (!std::strcmp(argv[i], "--no-lawfrombelowship")) g_lawFromBelowShip = false;
        if (!std::strcmp(argv[i], "--tpsaveoff")) g_tpSaveOff = true;
        if (!std::strcmp(argv[i], "--no-tpsaveoff")) g_tpSaveOff = false;
        if (!std::strcmp(argv[i], "--waverampend")) g_waveRampEnd = true;
        if (!std::strcmp(argv[i], "--no-waverampend")) g_waveRampEnd = false;
        if (!std::strcmp(argv[i], "--waveportalrot")) g_wavePortalRot = true;
        if (!std::strcmp(argv[i], "--no-waveportalrot")) g_wavePortalRot = false;
        if (!std::strcmp(argv[i], "--ceilfarbox")) g_ceilFarBox = true;
        if (!std::strcmp(argv[i], "--no-ceilfarbox")) g_ceilFarBox = false;
        if (!std::strcmp(argv[i], "--waveflipcap")) g_waveFlipCap = true;
        if (!std::strcmp(argv[i], "--no-waveflipcap")) g_waveFlipCap = false;
        if (!std::strcmp(argv[i], "--ballseatground")) g_ballSeatGround = true;
        if (!std::strcmp(argv[i], "--no-ballseatground")) g_ballSeatGround = false;
        if (!std::strcmp(argv[i], "--ridesupgrace")) g_rideSupGrace = true;
        if (!std::strcmp(argv[i], "--no-ridesupgrace")) g_rideSupGrace = false;
        if (!std::strcmp(argv[i], "--robotslopenohover")) g_robotSlopeNoHover = true;
        if (!std::strcmp(argv[i], "--no-robotslopenohover")) g_robotSlopeNoHover = false;
        if (!std::strcmp(argv[i], "--waveflipcapfloor")) g_waveFlipCapFloor = true;
        if (!std::strcmp(argv[i], "--no-waveflipcapfloor")) g_waveFlipCapFloor = false;
        if (!std::strcmp(argv[i], "--waveflipceilup")) g_waveFlipCeilUp = true;
        if (!std::strcmp(argv[i], "--no-waveflipceilup")) g_waveFlipCeilUp = false;
        if (!std::strcmp(argv[i], "--waveportalrotmini")) g_wavePortalRotMini = true;
        if (!std::strcmp(argv[i], "--modeswitchkeep")) g_modeSwitchKeep = true;
        if (!std::strcmp(argv[i], "--no-modeswitchkeep")) g_modeSwitchKeep = false;
        if (!std::strcmp(argv[i], "--waveportalsize")) g_wavePortalSize = true;
        if (!std::strcmp(argv[i], "--no-waveportalsize")) g_wavePortalSize = false;
        if (!std::strcmp(argv[i], "--obbfloat")) g_obbFloat = true;
        if (!std::strcmp(argv[i], "--no-waveportalrotmini")) g_wavePortalRotMini = false;
        if (!std::strcmp(argv[i], "--rampseatall")) g_rampSeatAll = true;
        if (!std::strcmp(argv[i], "--no-rampseatall")) g_rampSeatAll = false;
        if (!std::strcmp(argv[i], "--portalobbmode")) g_portalObbMode = true;
        if (!std::strcmp(argv[i], "--no-portalobbmode")) g_portalObbMode = false;
        if (!std::strcmp(argv[i], "--ceilowner")) g_ceilOwner = true;
        if (!std::strcmp(argv[i], "--no-ceilowner")) g_ceilOwner = false;
        if (!std::strcmp(argv[i], "--ceilheldnoland")) g_ceilHeldNoLand = true;
        if (!std::strcmp(argv[i], "--no-ceilheldnoland")) g_ceilHeldNoLand = false;
        if (!std::strcmp(argv[i], "--nocolceil")) g_noColCeil = true;
        if (!std::strcmp(argv[i], "--no-nocolceil")) g_noColCeil = false;
        if (!std::strcmp(argv[i], "--slideunderdir")) g_slideUnderDir = true;
        if (!std::strcmp(argv[i], "--no-slideunderdir")) g_slideUnderDir = false;
        if (!std::strcmp(argv[i], "--dynseamlen")) g_dynSeamLen = true;
        if (!std::strcmp(argv[i], "--no-dynseamlen")) g_dynSeamLen = false;
        if (!std::strcmp(argv[i], "--seedchain")) g_seedChain = true;
        if (!std::strcmp(argv[i], "--no-seedchain")) g_seedChain = false;
        if (!std::strcmp(argv[i], "--waveseathaz")) g_waveSeatHaz = true;
        if (!std::strcmp(argv[i], "--no-waveseathaz")) g_waveSeatHaz = false;
        if (!std::strcmp(argv[i], "--sizeexit")) g_sizeExit = true;
        if (!std::strcmp(argv[i], "--no-sizeexit")) g_sizeExit = false;
        if (!std::strcmp(argv[i], "--sizeseat")) g_sizeSeat = true;
        if (!std::strcmp(argv[i], "--no-sizeseat")) g_sizeSeat = false;
        if (!std::strcmp(argv[i], "--portalrot")) g_portalRot = true;
        if (!std::strcmp(argv[i], "--no-portalrot")) g_portalRot = false;
        if (!std::strcmp(argv[i], "--tpramplaunch")) g_tpRampLaunch = true;
        if (!std::strcmp(argv[i], "--no-tpramplaunch")) g_tpRampLaunch = false;
        if (!std::strcmp(argv[i], "--rottouch")) g_rotTouch = true;
        if (!std::strcmp(argv[i], "--no-rottouch")) g_rotTouch = false;
        if (!std::strcmp(argv[i], "--rotqreplay")) g_rotQReplay = true;
        if (!std::strcmp(argv[i], "--rotqtouch")) g_rotQTouch = true;
        if (!std::strcmp(argv[i], "--waveramprot")) g_waveRampRot = true;
        if (!std::strcmp(argv[i], "--no-waveramprot")) g_waveRampRot = false;
        if (!std::strcmp(argv[i], "--portslopepre")) g_portSlopePre = true;
        if (!std::strcmp(argv[i], "--no-portslopepre")) g_portSlopePre = false;
        if (!std::strcmp(argv[i], "--growramporder")) g_growRampOrder = true;
        if (!std::strcmp(argv[i], "--no-growramporder")) g_growRampOrder = false;
        if (!std::strcmp(argv[i], "--spdtapwarp")) g_spdTapWarp = true;
        if (!std::strcmp(argv[i], "--no-spdtapwarp")) g_spdTapWarp = false;
        if (!std::strcmp(argv[i], "--ballfreshend")) g_ballFreshEnd = true;
        if (!std::strcmp(argv[i], "--no-ballfreshend")) g_ballFreshEnd = false;
        if (!std::strcmp(argv[i], "--tapwarpnocrush")) g_tapWarpNoCrush = true;
        if (!std::strcmp(argv[i], "--no-tapwarpnocrush")) g_tapWarpNoCrush = false;
        if (!std::strcmp(argv[i], "--no-rotqtouch")) g_rotQTouch = false;
        if (!std::strcmp(argv[i], "--no-rotqreplay")) g_rotQReplay = false;
        if (!std::strcmp(argv[i], "--waveholddead")) g_waveHoldDead = true;
        if (!std::strcmp(argv[i], "--no-waveholddead")) g_waveHoldDead = false;
        if (!std::strcmp(argv[i], "--touchfirstlat")) g_touchFirstLat = true;
        if (!std::strcmp(argv[i], "--no-touchfirstlat")) g_touchFirstLat = false;
        if (!std::strcmp(argv[i], "--downdyrev")) g_downDyRev = true;
        if (!std::strcmp(argv[i], "--no-downdyrev")) g_downDyRev = false;
        if (!std::strcmp(argv[i], "--wavebandzoom")) g_waveBandZoom = true;
        if (!std::strcmp(argv[i], "--no-wavebandzoom")) g_waveBandZoom = false;
        if (!std::strcmp(argv[i], "--flybandcentre")) g_flyBandCentre = true;
        if (!std::strcmp(argv[i], "--no-flybandcentre")) g_flyBandCentre = false;
        if (!std::strcmp(argv[i], "--spidernoextcol")) g_spiderNoExtCol = true;
        if (!std::strcmp(argv[i], "--no-spidernoextcol")) g_spiderNoExtCol = false;
        if (!std::strcmp(argv[i], "--autolockmoves")) g_autoLockMoves = true;
        if (!std::strcmp(argv[i], "--no-autolockmoves")) g_autoLockMoves = false;
        if (!std::strcmp(argv[i], "--areaadd")) g_areaAdd = true;
        if (!std::strcmp(argv[i], "--no-areaadd")) g_areaAdd = false;
        if (!std::strcmp(argv[i], "--followtouch")) g_followTouch = true;
        if (!std::strcmp(argv[i], "--no-followtouch")) g_followTouch = false;
        if (!std::strcmp(argv[i], "--chanmove")) g_chanMove = true;
        if (!std::strcmp(argv[i], "--no-chanmove")) g_chanMove = false;
        if (!std::strcmp(argv[i], "--tap2max") && i + 1 < argc) g_tap2Max = std::max(1, std::min(4, std::atoi(argv[i + 1])));
        if (!std::strcmp(argv[i], "--p2born")) g_p2Born = true;
        if (!std::strcmp(argv[i], "--no-p2born")) g_p2Born = false;
        if (!std::strcmp(argv[i], "--holdseed2")) g_holdSeed2 = true;
        if (!std::strcmp(argv[i], "--no-holdseed2")) g_holdSeed2 = false;
        if (!std::strcmp(argv[i], "--wavepdbg") && i + 1 < argc) g_wavePortalHalf = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--tpskipfly")) g_tpSkipFly = true;
        if (!std::strcmp(argv[i], "--no-tpskipfly")) g_tpSkipFly = false;
        if (!std::strcmp(argv[i], "--portreread")) g_portReRead = true;
        if (!std::strcmp(argv[i], "--no-portreread")) g_portReRead = false;
        // --armpost: the 1859 arm reads the post-move box (constants.hpp)
        // --fgpost: the 2866 arm the same way, and read on the touch tick (constants.hpp)
        // --exitjump: a cube that rode the ramp last tick jumps on the exit tick (constants.hpp)
        if (!std::strcmp(argv[i], "--no-cubempush")) g_cubeMpush = false;
        // --seatc6: a flyer on a moved top face writes c6 and is launched from it (constants.hpp)
        // --ridec6: a ground-mode rider on a moved face writes c6 too (constants.hpp)
        // --orbswing: the yellow / pink orb's swing x0.6 (constants.hpp)
        // --flyclampreach: a body the band ceiling pulled down is offered to a rising face again (constants.hpp)
        // --escdyn: escapee-prune also reads the moving objects' tops (search_key.hpp)
        // --goalspare: keep a body above the level that reaches the goal first (speed.hpp)
        if (!std::strcmp(argv[i], "--goalspare")) g_goalSpare = 1;
        if (!std::strcmp(argv[i], "--no-goalspare")) g_goalSpare = 0;   // on by default
        if (!std::strcmp(argv[i], "--goalgd")) g_goalGd = true;
        if (!std::strcmp(argv[i], "--no-goalgd")) g_goalGd = false;
        // --upsidecoyote: an upside-down cube jumps from the og GD leaves set (speed.hpp). On by
        // default since 2026-09-26; the plain name still parses, --no-upsidecoyote is the off arm.
        if (!std::strcmp(argv[i], "--upsidecoyote")) g_upsideCoyote = 1;
        if (!std::strcmp(argv[i], "--no-upsidecoyote")) g_upsideCoyote = 0;
        // --rawvalues: the cube branch's gravity step before the 0.001 grid (speed.hpp). On by
        // default with --padtable; the positive spellings still parse and --no- turns them off.
        if (!std::strcmp(argv[i], "--rawvalues")) g_rawValues = 1;
        if (!std::strcmp(argv[i], "--no-rawvalues")) g_rawValues = 0;
        // --portalunpin: a gravity portal off a block keeps the tick's step in y (speed.hpp)
        if (!std::strcmp(argv[i], "--portalunpin")) g_portalUnpin = 1;   // on by default
        if (!std::strcmp(argv[i], "--no-portalunpin")) g_portalUnpin = 0;
        // --hazendpoint: the cube's and flying body's hazard test at the tick's end only (speed.hpp)
        if (!std::strcmp(argv[i], "--hazendpoint")) g_hazEndpoint = 1;   // on by default
        if (!std::strcmp(argv[i], "--no-hazendpoint")) g_hazEndpoint = 0;
        // --portalpress: a press on a mode portal's tick belongs to the new mode (speed.hpp)
        if (!std::strcmp(argv[i], "--portalpress")) g_portalPress = 1;   // on by default
        if (!std::strcmp(argv[i], "--no-portalpress")) g_portalPress = 0;
        // --padtable: pad launches from GD's per-mode table (speed.hpp)
        if (!std::strcmp(argv[i], "--padtable")) g_padTable = 1;
        if (!std::strcmp(argv[i], "--no-padtable")) g_padTable = 0;
        // --ringonce, --portalonce, --ringorder, --holdlatch, --ringfirst, --padfirst,
        // --portalflap, --ringpresspin, --ballbuffer, --ringbuffer, --groundcubejump,
        // --gravlandtap, --gravpadchain, --onewayceil, --padsection, --solidorder, --ballceil,
        // --ballceilflip, --snaponeway, --flyreland, --padthenland, --faceclass, --portalceil,
        // --flipgrace, --padflipland, --flyrings, --sawexact, --snaptable, --sawexactship,
        // --groundgrow, --ringeither, --growaway, --sizepress, --ballgrowlag:
        // always on since 2026-09-26 (speed.hpp). The spellings are still accepted, doing
        // nothing, so a config written before keeps working.
        if (!std::strcmp(argv[i], "--ringonce") || !std::strcmp(argv[i], "--portalonce")
            || !std::strcmp(argv[i], "--ringorder") || !std::strcmp(argv[i], "--holdlatch")
            || !std::strcmp(argv[i], "--ringfirst") || !std::strcmp(argv[i], "--padfirst")
            || !std::strcmp(argv[i], "--portalflap") || !std::strcmp(argv[i], "--ringpresspin")
            || !std::strcmp(argv[i], "--ballbuffer") || !std::strcmp(argv[i], "--ringbuffer")
            || !std::strcmp(argv[i], "--groundcubejump")
            || !std::strcmp(argv[i], "--gravlandtap") || !std::strcmp(argv[i], "--gravpadchain")
            || !std::strcmp(argv[i], "--onewayceil") || !std::strcmp(argv[i], "--padsection")
            || !std::strcmp(argv[i], "--solidorder") || !std::strcmp(argv[i], "--ballceil")
            || !std::strcmp(argv[i], "--ballceilflip") || !std::strcmp(argv[i], "--snaponeway")
            || !std::strcmp(argv[i], "--flyreland") || !std::strcmp(argv[i], "--padthenland")
            || !std::strcmp(argv[i], "--faceclass") || !std::strcmp(argv[i], "--portalceil")
            || !std::strcmp(argv[i], "--flipgrace") || !std::strcmp(argv[i], "--padflipland")
            || !std::strcmp(argv[i], "--flyrings") || !std::strcmp(argv[i], "--sawexact")
            || !std::strcmp(argv[i], "--snaptable") || !std::strcmp(argv[i], "--sawexactship")
            || !std::strcmp(argv[i], "--groundgrow") || !std::strcmp(argv[i], "--ringeither")
            || !std::strcmp(argv[i], "--growaway") || !std::strcmp(argv[i], "--sizepress")
            || !std::strcmp(argv[i], "--ballgrowlag")) continue;
        // --heldall: State::held tracks the button in every mode (speed.hpp)
        if (!std::strcmp(argv[i], "--heldall")) g_heldAll = 1;
        if (!std::strcmp(argv[i], "--no-heldall")) g_heldAll = 0;
        // --heldcellcap: under --heldall the alive cap counts cells, not button twins (speed.hpp)
        if (!std::strcmp(argv[i], "--heldcellcap")) g_heldCellCap = 1;
        // --heldcelltwins N: ...and keeps at most N states of a sampled cell (speed.hpp)
        if (i + 1 < argc && !std::strcmp(argv[i], "--heldcelltwins"))
            g_heldCellTwins = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--fgarmlive")) g_fgArmLive = true;
        if (!std::strcmp(argv[i], "--trigdump")) g_trigDump = true;
        if (!std::strcmp(argv[i], "--stopdump")) g_stopDump = true;   // print only
        if (!std::strcmp(argv[i], "--trigeffect")) g_trigEffect = true;  // print only
        // ...and here as well, because the loop that used to own it stops at argc-1:
        // passed LAST on the command line it was read by nobody, which is how the
        // turned-hazard fixture came out "still dies" for the arm that re-applies the
        // box (py/test_haz_turned_stage.py). Parsing it twice is harmless -- it only
        // ever sets the flag true.
        if (!std::strcmp(argv[i], "--memstat")) g_memStat = true;
        // --phaseprof: wall time per phase of the layer loop, printed once at the end. Print only.
        if (!std::strcmp(argv[i], "--phaseprof")) g_phaseProf = true;
        if (!std::strcmp(argv[i], "--groupsigcheck")) g_groupSigCheck = true;
        if (!std::strcmp(argv[i], "--levelcachecheck")) g_levelCacheCheck = true;   // debug, ReplayLevelCache   // debug, groupLayersFor (cleared below each call)
        if (!std::strcmp(argv[i], "--no-csvrows")) g_csvRows = false;               // LevelTextCache::rows, on by default
        if (!std::strcmp(argv[i], "--csvrowscheck")) g_csvRowsCheck = true;         // debug, level_loader.hpp CsvRows
        if (!std::strcmp(argv[i], "--rotfitcheck")) g_rotFitCheck = true;           // debug, level_loader.hpp stage 1''
        if (!std::strcmp(argv[i], "--incplace")) g_incPlace = true;                 // dynamics.hpp, on by default
        if (!std::strcmp(argv[i], "--no-incplace")) g_incPlace = false;
        if (!std::strcmp(argv[i], "--incplacecheck")) g_incCheck = true;            // debug: also place the plain way, compare
        if (!std::strcmp(argv[i], "--statecross")) g_stateCross = true;   // search_key.hpp, on by default
        if (!std::strcmp(argv[i], "--no-statecross")) g_stateCross = false;
        if (!std::strcmp(argv[i], "--crosscx")) g_crossCxHeads = true;   // search_key.hpp
        if (!std::strcmp(argv[i], "--no-crosscx")) g_crossCxHeads = false;
        if (!std::strcmp(argv[i], "--crossbehind")) g_crossBehind = true;   // search_key.hpp
        if (!std::strcmp(argv[i], "--no-crossbehind")) g_crossBehind = false;
        if (!std::strcmp(argv[i], "--crossminlag") && i + 1 < argc) g_crossMinLag = std::max(1, std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--crosslagunit") && i + 1 < argc) g_crossLagUnit = std::max(1, std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--incanchor")) g_incAnchor = true;               // dynamics.hpp, on by default
        if (!std::strcmp(argv[i], "--no-incanchor")) g_incAnchor = false;
        // --firebcheck: count violations of fireB's invariant (a set bit with no
        // tick, or a tick with no bit) and print the totals at the end.
        if (!std::strcmp(argv[i], "--firebcheck")) g_fireBCheck = true;
        // --groupfire: split the speed groups by the fire tick of every box still moving
        // (search_key.hpp groupFireSig).
        if (!std::strcmp(argv[i], "--groupfire")) g_groupFire = true;
        // --groupxsplit: split a speed group where its parts' object windows cannot meet
        // (search_key.hpp)
        if (!std::strcmp(argv[i], "--groupxsplit")) g_groupXSplit = true;
        if (!std::strcmp(argv[i], "--no-groupxsplit")) g_groupXSplit = false;
        // --witnessauto (search_key.hpp): the witness fires the autonomous triggers itself.
        if (!std::strcmp(argv[i], "--witnessauto")) g_witnessAuto = true;
        if (!std::strcmp(argv[i], "--no-witnessauto")) g_witnessAuto = false;
        // --rotqueue: consume rotations from the queue (frames.hpp) instead of
        // the pre-queue selection. Opt-in until the anchor can seed the state.
        if (!std::strcmp(argv[i], "--rotqueue")) g_rotQueue = true;
        // --startrotq <chan>,<revHex>[,<uid>...]: seed the queue's per-state
        // values at an anchor (history at g_startRotChan's declaration).
        // Applied after the queue is built, because the uids have to be looked
        // up in it.
        if (!std::strcmp(argv[i], "--startrotq")) {
            const char* p = argv[i + 1];
            g_startRotChan = std::atoi(p);
            const char* c = std::strchr(p, ',');
            if (c) {
                g_startRotRev = (unsigned)std::strtoul(c + 1, nullptr, 16);
                p = std::strchr(c + 1, ',');
                while (p) {
                    g_startRotSpent.push_back(std::atoi(p + 1));
                    p = std::strchr(p + 1, ',');
                }
            }
        }
        // --seeddump <t>: the accumulated-field line, for the seeding check.
        if (!std::strcmp(argv[i], "--seeddump")) g_seedDump = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--seedevery")) g_seedEvery = std::atoi(argv[i + 1]);
        // --dumpstate <t>:<path> / --start-state <path> (statefile.hpp)
        if (!std::strcmp(argv[i], "--needsize") && i + 1 < argc) {   // <tick>:<0 normal | 1 mini>
            long long nt = -1;
            int nm = 0;
            if (std::sscanf(argv[i + 1], "%lld:%d", &nt, &nm) == 2) {
                g_needSizeT = nt;
                g_needSizeMini = nm ? 1 : 0;
            }
        }
        if (!std::strcmp(argv[i], "--needspeed") && i + 1 < argc) {   // <x>:<0.7 | 0.9 | 1.1 | 1.3 | 1.6>, or <x>:!<speed>
            double nx = -1.0, nm = 0.0;
            const char* colon = std::strchr(argv[i + 1], ':');
            const bool no = colon && colon[1] == '!';
            if (colon && std::sscanf(argv[i + 1], "%lf", &nx) == 1 && std::sscanf(colon + (no ? 2 : 1), "%lf", &nm) == 1
                && nm > 0.0) {
                g_needSpeedX = nx;
                g_needSpeedNot = no;
                g_needSpeedCode = (int)speedCodeForDx((double)dxForSpeedMul(nm));
            }
        }
        if (!std::strcmp(argv[i], "--dumpstate") && i + 1 < argc) {
            const char* a = argv[i + 1];
            const char* colon = std::strchr(a, ':');   // the tick's own; a drive letter's comes after it
            if (colon) {
                g_dumpStateT = std::atoll(a);
                g_dumpStatePath = std::string(colon + 1);
            }
        }
        if (!std::strcmp(argv[i], "--start-state") && i + 1 < argc) g_startStatePath = argv[i + 1];
        // --p2touch: count touch boxes the second player enters (diagnostic).
        if (!std::strcmp(argv[i], "--p2touch")) g_p2Touch = true;
        // --no-spentpad: the A/B arm for --spentpad (frames.hpp). The list is
        // still parsed and still reported, so the two arms differ in the
        // seeding alone and not in what the caller passed. Valueless, so it
        // belongs in THIS loop -- the one below stops at argc-1.
        if (!std::strcmp(argv[i], "--no-spentpad")) g_spentPadSeed = false;
        // --anchor-state / --seed-partial-ok: the anchor payload (frames.hpp).
        if (!std::strcmp(argv[i], "--anchor-state")) g_anchorState = argv[i + 1];
        if (!std::strcmp(argv[i], "--touchseed") && i + 1 < argc) g_touchSeedArg = argv[i + 1];
        // --xtrack <file>: GD's x per tick behind the anchor (g_xTrack).
        if (!std::strcmp(argv[i], "--xtrack") && i + 1 < argc) {
            g_xTrack.clear();
            g_xTrackW.clear();
            std::vector<std::array<double, 4>> camHere;
            std::ifstream xf(argv[i + 1]);
            std::string ln;
            int ct = 0, cx = 1, ccl = -1, ccw = -1, cyw = -1, cgf = -1;
            bool first = true;
            while (std::getline(xf, ln)) {
                std::vector<std::string> f;
                size_t p = 0;
                while (true) {
                    const size_t q = ln.find(',', p);
                    f.push_back(ln.substr(p, q == std::string::npos ? std::string::npos : q - p));
                    if (q == std::string::npos) break;
                    p = q + 1;
                }
                if (first) {
                    first = false;
                    bool hdr = false;
                    for (size_t k = 0; k < f.size(); ++k) {
                        if (f[k] == "tick") { ct = (int)k; hdr = true; }
                        if (f[k] == "x") { cx = (int)k; hdr = true; }
                        if (f[k] == "camL") ccl = (int)k;
                        if (f[k] == "camW") ccw = (int)k;
                        if (f[k] == "y") cyw = (int)k;
                        if (f[k] == "gframe") cgf = (int)k;
                    }
                    if (hdr) continue;
                }
                if ((int)f.size() <= std::max(ct, cx)) continue;
                g_xTrack.push_back({std::atoi(f[ct].c_str()), std::atof(f[cx].c_str())});
                // --autotrigframe: the row's world y and frame, kept beside it (dynamics.hpp g_xTrackW)
                if (cyw >= 0 && cgf >= 0 && (int)f.size() > std::max(cyw, cgf))
                    g_xTrackW.push_back({std::atof(f[cx].c_str()), std::atof(f[cyw].c_str()), (double)(std::atoi(f[cgf].c_str()) & 3)});
                if (ccl >= 0 && ccw >= 0 && (int)f.size() > std::max(ccl, ccw))
                    camHere.push_back({std::atof(f[cx].c_str()), std::atof(f[ccl].c_str()), std::atof(f[ccw].c_str())});
            }
            // the xtrack's camera columns stand in for --camtrack when that is not given (a track without them, or with gaps, has none)
            if (!g_camFromFile && camHere.size() == g_xTrack.size()) g_camXtrackRaw = camHere;
            std::printf("xtrack: %zu rows from %s\n", g_xTrack.size(), argv[i + 1]);
        }
        // --camtrack <file>: tick,x,camL,camW of one attempt (g_camRows).
        if (!std::strcmp(argv[i], "--camtrack") && i + 1 < argc) {
            std::ifstream cf(argv[i + 1]);
            std::string ln;
            std::getline(cf, ln);   // header
            std::vector<std::array<double, 4>> raw;
            while (std::getline(cf, ln)) {
                int tk = 0;
                double x = 0, cl = 0, cw = 0;
                if (std::sscanf(ln.c_str(), "%d,%lf,%lf,%lf", &tk, &x, &cl, &cw) == 4 && cw > 0) raw.push_back({x, cl, cw});
            }
            g_camXtrackRaw = raw;
            g_camFromFile = true;
            std::printf("camtrack: %zu rows from %s\n", raw.size(), argv[i + 1]);
        }
        // --touchentered uid[:tick],uid[:tick],... ("-" = none): see
        // g_touchEnteredGiven and g_touchEnteredT.
        if (!std::strcmp(argv[i], "--touchentered") && i + 1 < argc) {
            g_touchEnteredGiven = true;
            g_touchEntered.clear();
            g_touchEnteredT.clear();
            const std::string v = argv[i + 1];
            size_t p = 0;
            while (p < v.size()) {
                size_t q = v.find(',', p);
                if (q == std::string::npos) q = v.size();
                const std::string tok = v.substr(p, q - p);
                const int u = std::atoi(tok.c_str());
                if (u > 0) {
                    g_touchEntered.insert(u);
                    const size_t c = tok.find(':');
                    if (c != std::string::npos)
                        g_touchEnteredT[u] = std::atoi(tok.c_str() + c + 1);
                }
                p = q + 1;
            }
        }
        // --rectouchentered uid:tick,... ("-" = none): the recording's own attempt's entries
        // (dynamics.hpp g_recOwnEntry, filled by bit once the boxes are known)
        if (!std::strcmp(argv[i], "--rectouchentered") && i + 1 < argc) {
            g_recTouchEnteredGiven = true;
            g_recTouchEnteredT.clear();
            const std::string v = argv[i + 1];
            size_t p = 0;
            while (p < v.size()) {
                size_t q = v.find(',', p);
                if (q == std::string::npos) q = v.size();
                const std::string tok = v.substr(p, q - p);
                const int u = std::atoi(tok.c_str());
                const size_t c = tok.find(':');
                if (u > 0 && c != std::string::npos) g_recTouchEnteredT[u] = std::atoi(tok.c_str() + c + 1);
                p = q + 1;
            }
        }
        if (!std::strcmp(argv[i], "--touchonrec")) g_touchOnRec = true;
        if (!std::strcmp(argv[i], "--no-touchonrec")) g_touchOnRec = false;
        if (!std::strcmp(argv[i], "--seed-partial-ok")) g_seedPartialOk = true;
        // --shiftstat: one line per moving object saying which recorded row the
        // model reads for it (dynamics.hpp). Single-threaded paths only -- the
        // "said it already" flag it keeps is not synchronised, so use it on
        // --replay, not on a threaded search.
        if (!std::strcmp(argv[i], "--shiftstat")) g_shiftStat = true;
        // Instrumentation only (dp/clearance.hpp). Reads the children after they
        // are stepped and keyed; never feeds anything back into the search.
        if (!std::strcmp(argv[i], "--clearprobe")) g_clearProbe = true;
        // --refwatch <trace.csv>: follow that trajectory through the search and
        // name the gate that drops it (refwatch.hpp).
        if (!std::strcmp(argv[i], "--refwatch") && i + 1 < argc) {
            if (!loadRefTrace(argv[i + 1]))
                std::printf("refwatch: cannot read %s\n", argv[i + 1]);
            else {
                g_refWatch = true;
                std::printf("refwatch: %zu reference rows\n", g_refRows.size());
            }
        }
        // --rejoinwatch <trace.csv> / --rejoinafter <tick>: print only (refwatch.hpp).
        if (!std::strcmp(argv[i], "--rejoinwatch") && i + 1 < argc) {
            if (!loadTraceInto(argv[i + 1], g_rjRows))
                std::printf("rejoin: cannot read %s\n", argv[i + 1]);
            else
                g_rjOn = true;
        }
        if (!std::strcmp(argv[i], "--rejoinuse")) g_rjUse = true;
        if (!std::strcmp(argv[i], "--rejoinprobe") && i + 1 < argc) g_rejoinProbe = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--rejoinfull")) g_rjFull = true;
        if (!std::strcmp(argv[i], "--rejoinafter") && i + 1 < argc)
            g_rjAfter = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--refeps") && i + 1 < argc)
            g_refEps = std::atof(argv[i + 1]);
        // --no-padplayerrot: a rotated pad's contact is tested with the player
        // square forced AXIS-ALIGNED (the a874728 behaviour) rather than turned
        // by the player's own rotation.
        if (!std::strcmp(argv[i], "--no-padplayerrot")) g_noPadPlayerRot = true;
        // --no-satrotraw: the four oriented-contact sites advance s.rot by one
        // spin step before the SAT (the pre-2026-09-06 behaviour) instead of
        // passing it as it stands. See g_noSatRotRaw.
        if (!std::strcmp(argv[i], "--no-satrotraw")) g_noSatRotRaw = true;
        // Value-less, so it lives in this loop (see the note just below): the
        // argc-1 loop would drop it silently whenever it is passed last.
        if (!std::strcmp(argv[i], "--rotqtoggle")) g_rotQToggle = true;
        if (!std::strcmp(argv[i], "--touchwinahead")) g_touchWinAhead = true;
        if (!std::strcmp(argv[i], "--no-touchwinahead")) g_touchWinAhead = false;
        if (!std::strcmp(argv[i], "--touchcensus")) g_touchCensus = true;   // value-less, same reason
        if (!std::strcmp(argv[i], "--lagfit")) g_lagFitDbg = true;   // print only
        // --keycensus: count, per touch box, how often it divided the dedupe
        // key. Print-only; the tally goes out at the end of the run.
        if (!std::strcmp(argv[i], "--keycensus")) g_keyCensus = true;
        // --searchcensus: what the frontier is made of (search_census.hpp), every 16th layer.
        // Print only.
        if (!std::strcmp(argv[i], "--searchcensus")) g_searchCensus = 16;
        // --autospread: the autonomous triggers fired with states still behind them (model.hpp).
        // Print only.
        if (!std::strcmp(argv[i], "--autospread")) g_autoSpread = true;
        // --autoown: each group is placed by its own crossing of the autonomous triggers (dynamics.hpp)
        if (!std::strcmp(argv[i], "--autoownpre")) g_autoOwnPre = true;
        if (!std::strcmp(argv[i], "--no-autoownpre")) g_autoOwnPre = false;
        // --twinaudit (search_census.hpp): value-less.
        if (!std::strcmp(argv[i], "--twinaudit")) g_twinAudit = true;
        // --trigraw: autonomous triggers behind the anchor trust the
        // recording's tick (history at g_trigRaw's declaration). These three sat
        // in the argc-1 loop and were never read when passed last
        // (py/test_valueless_flags.py).
        if (!std::strcmp(argv[i], "--trigraw")) g_trigRaw = true;
        if (!std::strcmp(argv[i], "--closedpast")) g_closedPast = true;
        if (!std::strcmp(argv[i], "--no-closedpast")) g_closedPast = false;
        if (!std::strcmp(argv[i], "--closedpastall")) g_closedPastAll = true;
        if (!std::strcmp(argv[i], "--no-closedpastall")) g_closedPastAll = false;
        // --groupholddeath: the last --groups file ended on a death; hold its
        // last row one tick (groups.hpp). The mod appends it after the live
        // recording, i.e. last. Value-less, so it lives in this loop.
        if (!std::strcmp(argv[i], "--groupholddeath")) g_groupHoldDeath = true;
        if (!std::strcmp(argv[i], "--ceilpush")) g_ceilPush = true;   // value-less, same reason
        if (!std::strcmp(argv[i], "--dualslide")) g_dualSlide = true;   // value-less too
        if (!std::strcmp(argv[i], "--no-dualslide")) g_dualSlide = false;
        if (!std::strcmp(argv[i], "--anchorride")) g_anchorRide = true;   // on by default
        if (!std::strcmp(argv[i], "--no-anchorride")) g_anchorRide = false;
        if (i + 1 < argc && !std::strcmp(argv[i], "--flipcldbg"))
            g_flipLandDbgLeft = std::atoi(argv[++i]);   // print only
        // --bonkarm: always on since 2026-09-26 (constants.hpp); accepted, doing nothing
        if (!std::strcmp(argv[i], "--bonkarm")) {}
        if (!std::strcmp(argv[i], "--vetophys")) g_vetoPhys = true;   // value-less, same reason
        if (!std::strcmp(argv[i], "--resimladderpartial")) g_resimLadderPartial = true;
        if (!std::strcmp(argv[i], "--no-resimladderpartial")) g_resimLadderPartial = false;
        if (!std::strcmp(argv[i], "--rlbound") && i + 1 < argc) g_resimLadderBound = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--rlspread0")) g_resimLadderSpread0 = true;
        if (!std::strcmp(argv[i], "--no-rlspread0")) g_resimLadderSpread0 = false;
        if (!std::strcmp(argv[i], "--verdictinfo")) g_verdictInfo = true;   // value-less, same reason
        // Value-less too, and the mod's addWorldArgs can emit it LAST (nothing
        // after it when no boxes are dropped, obb.txt is missing and dpArgs is
        // empty -- the cold-restart JobFirstSolve path), where the loop below
        // never reads it. Parsed here as well so argv order cannot drop it;
        // the second parse at its old site is a harmless re-set.
        if (!std::strcmp(argv[i], "--needtrig-unseen")) g_needUnseen = true;
        if (!std::strcmp(argv[i], "--airpress")) g_airPress = true;   // value-less (g_airPress)
        if (!std::strcmp(argv[i], "--refadopt")) g_refAdopt = true;   // value-less (g_refAdopt)
        if (!std::strcmp(argv[i], "--refadoptmem")) g_refAdopt = g_refAdoptMem = true;   // value-less
        if (!std::strcmp(argv[i], "--tracestate")) g_traceState = true;   // value-less (refwatch.hpp)
        // --walkresume is the ladder's (cliMain reads it into g_wrc); --walkresumecheck is per call.
        if (!std::strcmp(argv[i], "--walkresumecheck")) g_walkResumeCheck = true;   // value-less
        if (!std::strcmp(argv[i], "--no-airpress")) g_airPress = false;
        if (!std::strcmp(argv[i], "--airpressnear")) g_airPressNear = true;
        if (!std::strcmp(argv[i], "--no-airpressnear")) g_airPressNear = false;
        // --replayon (diagnostic): the replay walks past its first death (g_replayOn).
        if (!std::strcmp(argv[i], "--replayon")) g_replayOn = true;
        // [2026-09-01] The three debug switches used to sit in the argc-1 loop
        // below, i.e. in the loop this very comment says value-less flags must
        // not be in: passed LAST they did nothing, silently. Measured the hard
        // way -- `--slopedbg` alone printed nothing at all, and adding a second
        // flag after it made 216 lines appear. A diagnostic that is silently
        // off is worse than one that is missing, because the empty output reads
        // as an answer ("the model never even looks at this object").
        if (!std::strcmp(argv[i], "--banddbg")) g_bandDbg = true;
        if (!std::strcmp(argv[i], "--slopedbg")) g_slopeDbg = true;
            if (!std::strcmp(argv[i], "--slopereldbg")) g_slopeRelDbg = true;
            if (!std::strcmp(argv[i], "--vywriter") && i + 1 < argc) g_vyWatchT = std::atoll(argv[++i]);
            if (!std::strcmp(argv[i], "--vywriter2") && i + 1 < argc) g_vyWatchT2 = std::atoll(argv[++i]);
            if (!std::strcmp(argv[i], "--fxwatch") && i + 1 < argc) g_fxWatchT = std::atoll(argv[++i]);
            if (!std::strcmp(argv[i], "--rotwatch") && i + 1 < argc
                && std::sscanf(argv[++i], "%lld,%lld", &g_rotWatchLo, &g_rotWatchHi) != 2)
                g_rotWatchLo = g_rotWatchHi = -1;
            if (!std::strcmp(argv[i], "--qfoldwatch") && i + 1 < argc
                && std::sscanf(argv[++i], "%lld,%lld", &g_qfoldLo, &g_qfoldHi) != 2)
                g_qfoldLo = g_qfoldHi = -1;
        if (!std::strcmp(argv[i], "--dcydbg")) g_dcyDbg = true;
        if (!std::strcmp(argv[i], "--supdbg")) g_supDbg = true;
        // --spddbg: one line per speed-portal candidate per tick, plus the
        // window size, plus which of the three gates rejected it.
        if (!std::strcmp(argv[i], "--spddbg")) g_spdDbg = true;
        // --rotcheck: replay the computed orbit against every recorded rotated
        // object at load and print the residual. Diagnostic only -- it does not
        // place anything (see RotSpec).
        if (!std::strcmp(argv[i], "--rotcheck")) g_rotCheck = true;
        // --no-rotcompute: back to the recording (or to standing still) for
        // every turned object, for A/B against the computed orbit.
        if (!std::strcmp(argv[i], "--no-rotcompute")) g_rotCompute = false;
        // --no-rotsplit: keep the recording for turned objects instead of the
        // computed orbit (stage 1' A/B).
        if (!std::strcmp(argv[i], "--no-rotsplit")) g_rotSplit = false;
        // --stripvetoorder: a ramp's strip is vetoed only by the ramps GD's slope map holds (off by
        // default: on the model-only check set it breaks more than it fixes; g_stripVetoOrder says why).
        // --no-stripvetoorder still parses, for the A/B arms recorded while it was on.
        if (!std::strcmp(argv[i], "--stripvetoorder")) g_stripVetoOrder = true;
        if (!std::strcmp(argv[i], "--no-stripvetoorder")) g_stripVetoOrder = false;
        // --ringspendflap: a ring that takes the press takes the portal's buffered flap too (frames.hpp)
        // --ballportalflap: a ball's unspent press flaps the UFO a portal makes of it (frames.hpp)
        // --bvar9kill: a UFO meeting the other side's ramp after a ride dies on it (frames.hpp)
        // --spiderringfirst: a grounded spider's press inside any ring goes to the ring (frames.hpp)
        // --flipceilball: a flipped ball rides a ceiling ramp's line on as the cube does (frames.hpp)
        // --locknodx: a lockToPlayerX Move's x offset is not added to the object (dynamics.hpp)
        if (!std::strcmp(argv[i], "--ladderovercap")) g_ladderOverCap = true;
        if (!std::strcmp(argv[i], "--no-ladderovercap")) g_ladderOverCap = false;
        if (!std::strcmp(argv[i], "--ladderovercapstop")) g_ladderOverCapStop = true;
        if (!std::strcmp(argv[i], "--no-ladderovercapstop")) g_ladderOverCapStop = false;
        // --capenvelope: a thinned cap class keeps its lowest and highest state (g_capEnvelope).
        if (!std::strcmp(argv[i], "--capenvelope")) g_capEnvelope = true;
        // --coins: collect every coin in the level as well as reaching the end.
        // Value-less, so it belongs HERE -- put in the loop below it was read
        // only when something else followed it on the command line, and the
        // first run passed it last and silently solved the level as usual.
        if (!std::strcmp(argv[i], "--coins")) g_coinRoute = true;
        // --worldgates: the item gates are part of the world without --coins too (g_worldGates).
        if (!std::strcmp(argv[i], "--worldgates")) g_worldGates = true;
    }
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--threads")) g_threads = std::atoi(argv[i + 1]);
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--dodgemin"))
            g_portalDodgeMin = std::atof(argv[i + 1]);
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--speeddodge"))
            g_speedDodgeMin = std::atof(argv[i + 1]);
    // --glitchportal <px> / --glitchwave <px>: the glitch-avoid mode (thread_pool.hpp)
    for (int i = 2; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--glitchportal")) g_glitchPortal = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--glitchwave")) g_glitchWave = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--glitchembed")) g_glitchEmbed = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--wavecentre")) g_waveCentre = std::atof(argv[i + 1]);   // the wave-behaviour option
        if (!std::strcmp(argv[i], "--wavetidy")) g_waveTidy = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--wavetoggle")) g_waveToggle = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--startedge")) g_startEdge = std::atoi(argv[i + 1]);
    }
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "--glitchdeco")) g_glitchDeco = true;
    // --coinwin <yBin>,<vyBin>,<len> (thread_pool.hpp). Anything but three positive numbers is off.
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--coinwin")
            && (std::sscanf(argv[i + 1], "%lf,%lf,%lf", &g_coinWinY, &g_coinWinVy, &g_coinWinLen) != 3
                || !(g_coinWinY > 0.0 && g_coinWinVy > 0.0 && g_coinWinLen > 0.0)))
            g_coinWinY = g_coinWinVy = g_coinWinLen = 0.0;
    if (g_coinWinY > 0.0)
        std::printf("coinwin: cap cells y/%.1f vy/%.2f over %.0f px before each coin still lacked\n",
                    g_coinWinY, g_coinWinVy, g_coinWinLen);
    for (int i = 2; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--out")) outPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--dbg")) dbgLayers = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--cap")) g_aliveCap = (size_t)std::atoll(argv[i + 1]);
        // --search dp|beam, --beam B (search_key.hpp): which search answers, and the beam's width.
        if (!std::strcmp(argv[i], "--search")) {
            if (!std::strcmp(argv[i + 1], "beam")) g_searchMethod = 1;
            else if (!std::strcmp(argv[i + 1], "montecarlo")) g_searchMethod = 2;
            else if (!std::strcmp(argv[i + 1], "dp")) g_searchMethod = 0;
            else std::printf("search: unknown method '%s', using dp\n", argv[i + 1]);
        }
        if (!std::strcmp(argv[i], "--beam")) g_beamWidth = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--mcrollouts")) g_mcRollouts = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--mcwindow")) g_mcWindow = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--mctoggles")) g_mcToggles = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--mcsteps")) g_mcSteps = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--mcsafety")) g_mcSafetyS = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--mcstall")) g_mcStall = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--mccall")) g_mcCall = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--seed")) g_searchSeed = std::atoll(argv[i + 1]);
        // --capmap x0:c0,x1:c1,... (thread_pool.hpp). Entries must be in x order.
        if (!std::strcmp(argv[i], "--capmap")) {
            g_capMap.clear();
            for (const char* p = argv[i + 1]; *p;) {
                char* e = nullptr;
                const double x = std::strtod(p, &e);
                if (e == p || *e != ':') break;
                p = e + 1;
                const long long c = std::strtoll(p, &e, 10);
                if (e == p || c <= 0) break;
                g_capMap.emplace_back(x, (size_t)c);
                p = (*e == ',') ? e + 1 : e;
            }
        }
        // --gridmap x0:k0,x1:k1,... (search_census.hpp gridAtX). Entries must be in x order;
        // k = 0 means "--inputgrid's value", 1 means no grid.
        if (!std::strcmp(argv[i], "--gridmap")) {
            g_gridMap.clear();
            for (const char* p = argv[i + 1]; *p;) {
                char* e = nullptr;
                const double x = std::strtod(p, &e);
                if (e == p || *e != ':') break;
                p = e + 1;
                const long k = std::strtol(p, &e, 10);
                if (e == p || k < 0) break;
                g_gridMap.emplace_back(x, (int)k);
                p = (*e == ',') ? e + 1 : e;
            }
        }
        // --coinmask <bits>: with --coins, the coins already collected at the
        // --start anchor (bit i = L.coins[i], x order). See the seed below.
        if (!std::strcmp(argv[i], "--coinmask")) g_coinMaskSeed = std::atoi(argv[i + 1]);
        // --coinskip <bits>: coins to leave out of the goal (same bit order).
        // Kept apart from --coinmask because the loop passes that one itself,
        // at every anchor, and the last occurrence would win.
        if (!std::strcmp(argv[i], "--coinskip")) g_coinSkip = std::atoi(argv[i + 1]);
        // --hazdbg <uid>: see g_hazDbgUid. Takes a value, so it is parsed with
        // the same argv[i+1] form as --cap and not with the valueless shape at
        // the top of this file, which is silently off when it is given one.
        if (!std::strcmp(argv[i], "--hazdbg")) g_hazDbgUid = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--hazdbgwin") && i + 2 < argc) { g_hazDbgFrom = std::atoll(argv[i + 1]); g_hazDbgTo = std::atoll(argv[i + 2]); }
        // --trigdbg <tick>: one line per group at that tick, naming the mask the
        // group applied. Reads --hazdbg for WHICH object to locate, so the two
        // flags are given together.
        if (!std::strcmp(argv[i], "--trigdbg")) g_trigDbgT = std::atoll(argv[i + 1]);
        // --fbforce <box>:<tick>: see g_fbForceBox (triggers.hpp).
        if (!std::strcmp(argv[i], "--fbforce")) {
            const char* v = argv[i + 1];
            g_fbForceBox = std::atoi(v);
            const char* c = std::strchr(v, ':');
            g_fbForceTick = c ? std::atoi(c + 1) : 0;
        }
        // Where the model put each coin at that tick, against the mod's own
        // `coinlive:` line for the same tick (see the declaration).
        if (!std::strcmp(argv[i], "--coindbg")) g_coinDbgT = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--gcnodes")) g_gcNodes = (size_t)std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--memlimit")) g_memLimitMiB = (size_t)std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--shipyq")) g_shipYq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--keyxq")) g_keyXq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--keyxqfly")) g_keyXqFly = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--minpulse")) g_minPulse = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--inputgrid")) g_inputGrid = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--wavepulse")) g_wavePulse = std::atoi(argv[i + 1]);   // the wave-behaviour option
        if (!std::strcmp(argv[i], "--shipvq")) g_shipVq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--snaplog")) snapLogPath = argv[i + 1];
        // [2026-08-31] --shipceil / --ceil / --flyfloor / --ufoceil ARE GONE.
        // Not the machinery -- g_shipCeil, g_playerCeils / playerCeilAt,
        // g_flyFloor and g_ufoCeil are all still read where they always were --
        // only the way to SET them from the command line. An audit found that
        // nothing passes any of the four: not addWorldArgs (the full list the
        // in-game loop hands the solver is level dumps plus what the run itself
        // produced), not cold_regress.py (one identical cfg for all 22 levels),
        // not fidelity_diff, not the lab. They are per-LEVEL knobs in a solver
        // whose whole claim is that every level gets the same one, and leaving a
        // way to set them meant the claim held only because nobody happened to.
        // Their comments still describe values "measured per level" (lv9 = 330,
        // lv14 = 420@13100:13600) as though they were in force; they are not,
        // and both levels solve without them. See the notes at the globals.
        // Dropping the flags makes that structural instead of accidental, and
        // leaves the readers as plain dead code for whoever deletes them.
        // --groups <file>: the MOD's grouptrace dump. Without it every level
        // from lv19 on is planned against geometry that is in the wrong place.
        if (!std::strcmp(argv[i], "--groups")) groupsPaths.push_back(argv[i + 1]);
        if (!std::strcmp(argv[i], "--groupsauto")) g_groupAutoPaths.push_back(argv[i + 1]);   // groups.hpp g_bootAuto
        // --triggers / --objgroups: the MOD's trigger -> group -> uid map. Both
        // or neither; without them a door that only opens on contact reads as a
        // solid wall (see TouchTrig).
        // --supporttol X: gap tolerance for staying supported (default 0.01,
        // and the surface's own |dcy| is added on top -- see kSupportTol's
        // note)
        if (!std::strcmp(argv[i], "--supporttol"))
            kSupportTol = std::atof(argv[i + 1]);
        // --landtol X: penetration a landing allows (default 10.5, see
        // kLandTol's note)
        if (!std::strcmp(argv[i], "--landtol"))
            kLandTol = std::atof(argv[i + 1]);
        // --stickgap X: gap tolerance for continuing a ride (default 4.03, see
        // kStickGap's note)
        if (!std::strcmp(argv[i], "--stickgap"))
            kStickGap = std::atof(argv[i + 1]);
        // --rotperp X: how far off a 2900 the player may be on the perpendicular
        // axis and still fire it (default 150, see kRotPerpWin's note)
        if (!std::strcmp(argv[i], "--rotperp"))
            kRotPerpWin = std::atof(argv[i + 1]);
        // --stepdepth X: step depth that can be grabbed mid-ride (default
        // 3.15, see kStepDepth's note)
        if (!std::strcmp(argv[i], "--stepdepth"))
            kStepDepth = std::atof(argv[i + 1]);
        // --ctrlwin t0:t1,t0:t1,...: controls-disabled windows (both ends
        // inclusive). The caller builds them from the ctrlOff column of GD's
        // dump (history at g_ctrlWin's declaration).
        // --ctrlwin2: the second body's, from the dump's p2ctrlOff (read under --ctrlwinp2)
        if (!std::strcmp(argv[i], "--ctrlwin") || !std::strcmp(argv[i], "--ctrlwin2")) {
            auto& wins = argv[i][9] ? g_ctrlWin2 : g_ctrlWin;
            for (const char* p = argv[i + 1]; *p; ) {
                long long a0 = std::atoll(p);
                const char* col = std::strchr(p, ':');
                long long a1 = col ? std::atoll(col + 1) : a0;
                wins.push_back({a0, a1});
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        // --pinrows <file>: the ticks GD holds player 1 on a spot (frames.hpp g_pinRows)
        if (!std::strcmp(argv[i], "--pinrows")) {
            std::ifstream pf(argv[i + 1]);
            if (!pf) {
                std::printf("pinrows: cannot open %s\n", argv[i + 1]);
            } else {
                std::string ln;
                while (std::getline(pf, ln)) {
                    long long pt;
                    double px, py;
                    if (std::sscanf(ln.c_str(), "%lld,%lf,%lf", &pt, &px, &py) == 3) g_pinRows.push_back({pt, (float)px, (float)py});
                }
                std::sort(g_pinRows.begin(), g_pinRows.end(), [](const PinRow& a, const PinRow& b) { return a.t < b.t; });
                std::printf("pinrows: %zu pinned ticks from %s\n", g_pinRows.size(), argv[i + 1]);
            }
        }
        // --spentrot uid,uid,...: 2900s already fired before the anchor
        // (history at g_spentRot's declaration). Applied after loadLevel (once
        // g_rotTrig has been filled).
        if (!std::strcmp(argv[i], "--spentrot")) {
            for (const char* p = argv[i + 1]; *p; ) {
                g_spentRot.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        // --spentpad uid,uid,...: pads already fired before the anchor
        // (history at g_spentPad's declaration). Applied at the anchor's pad
        // seeding, after loadLevel.
        if (!std::strcmp(argv[i], "--spentpad")) {
            for (const char* p = argv[i + 1]; *p; ) {
                g_spentPad.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        // --solidorddbg t0:t1: print the solids the collision loops visit, in order (speed.hpp)
        if (!std::strcmp(argv[i], "--solidorddbg")) {
            g_solidOrdDbgT0 = std::atoll(argv[i + 1]);
            const char* c = std::strchr(argv[i + 1], ':');
            g_solidOrdDbgT1 = c ? std::atoll(c + 1) : g_solidOrdDbgT0;
        }
        // --spentorb uid,uid,...: rings already fired before the anchor, oldest first (speed.hpp)
        if (!std::strcmp(argv[i], "--spentorb")) {
            for (const char* p = argv[i + 1]; *p; ) {
                g_spentOrb.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        // --spentorb2 uid,uid,...: the second body's (read under --spentp2, speed.hpp)
        if (!std::strcmp(argv[i], "--spentorb2")) {
            for (const char* p = argv[i + 1]; *p; ) {
                g_spentOrb2.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        if (!std::strcmp(argv[i], "--dyndbg")) g_dynDbg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--geodump") && i + 4 < argc) {
            if (g_geoDumpF) std::fclose(g_geoDumpF);
            g_geoDumpF = std::fopen(argv[i + 1], "w");
            if (g_geoDumpF) std::fprintf(g_geoDumpF, "t,uid,cx,cy,hw,hh,on\n");
            g_geoT0 = std::atoll(argv[i + 2]);
            g_geoT1 = std::atoll(argv[i + 3]);
            g_geoStride = std::max(1LL, (long long)std::atoll(argv[i + 4]));
        }
        if (!std::strcmp(argv[i], "--triggers")) trigPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--objgroups")) grpPath = argv[i + 1];
        // --items <path>: the MOD's items.txt, the pickups a Count trigger
        // counts. Read only under --coins (nothing else looks at a counter).
        if (!std::strcmp(argv[i], "--items")) itemPath = argv[i + 1];
        // --itembase "<item>:<n>,...": the counters GD already holds at the
        // anchor (see g_itemBase).
        // --coinmargin <px>: how far inside a coin the plan has to be (see
        // kCoinMargin). 0 restores "the boundary counts", which is GD's own
        // rule and the fragile one to plan against.
        if (!std::strcmp(argv[i], "--coinmargin")) kCoinMargin = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--itembase")) {
            g_itemBase.clear();
            std::stringstream ss(argv[i + 1]);
            std::string ent;
            while (std::getline(ss, ent, ',')) {
                int it = 0, n = 0;
                if (std::sscanf(ent.c_str(), "%d:%d", &it, &n) == 2 && it != 0)
                    g_itemBase.push_back({it, n});
            }
        }
        // --rotgameplay <path>: the MOD's dump of the 2899/2900 queue (channel,
        // ord, the switch fields). Absent on 21 of 22 levels, which have no
        // such object at all.
        if (!std::strcmp(argv[i], "--rotgameplay")) rotQPath = argv[i + 1];
        // --areas <path>: the MOD's dump of the area triggers (areas.txt), read under --areamove.
        if (!std::strcmp(argv[i], "--areas")) areasPath = argv[i + 1];
        // --levelsettings <file>: the level's LevelSettingsObject flags. Only
        // lv22 has any of them set among the 22 official levels, and only
        // fixRadiusCollision has a reader (hazardHit). Without the flag the
        // loader looks for the file beside the objrects dump, which covers
        // every caller that passes a real path; the in-process caller passes
        // the level in memory, so it has to name the file.
        if (!std::strcmp(argv[i], "--levelsettings")) setPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--endportalx")) endPortalArg = std::atof(argv[i + 1]);   // --goalgd
        // --obb <file>: GD's own corners for the turned objects (see loadObb).
        // Optional -- without it a turned hazard keeps the bound it has always
        // had, so a level with no obb dump cannot move.
        if (!std::strcmp(argv[i], "--obb")) obbPath = argv[i + 1];
        // --forceids <file>: force block IDs (see g_forceIdsPath)
        if (!std::strcmp(argv[i], "--forceids")) g_forceIdsPath = argv[i + 1];
        // --bandtrack <file>: the recorded band (history at g_bandTrack's
        // declaration)
        if (!std::strcmp(argv[i], "--bandtrack")) loadBandTrack(argv[i + 1]);
        // --bandtrackend <t>: where the recording behind --bandtrack stops (bands.hpp)
        if (!std::strcmp(argv[i], "--bandtrackend")) g_bandTrackEnd = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--startband")) {
            // Three different facts used to leave through the same line: a
            // collapsed band, a pair the wrong way round, and a string that is
            // not a pair at all. They are separated here because only the first
            // says anything about the level.
            double f = 0, c = 0;
            if (std::sscanf(argv[i + 1], "%lf,%lf", &f, &c) != 2) {
                g_startBandKind = BandKind::Unknown;
                std::fprintf(stderr, "startband: `%s` is not a readable f,c pair\n",
                             argv[i + 1]);
            } else if (c > f) {
                g_startBandKind = BandKind::Interval;
                g_startBandFloor = f;
                g_startBandCeil = c;
            } else if (c == f) {
                g_startBandKind = BandKind::Degenerate;
                g_startBandFloor = g_startBandCeil = f;
                std::fprintf(stderr, "startband: `%s` is a collapsed band\n",
                             argv[i + 1]);
            } else {
                g_startBandKind = BandKind::Unknown;
                std::fprintf(stderr, "startband: `%s` has the ceiling below the "
                             "floor\n", argv[i + 1]);
            }
        }
        // --needtrig <n>: repeatable. See g_needTrig.
        if (!std::strcmp(argv[i], "--needtrig"))
            g_needTrig |= touchBit(std::atoi(argv[i + 1]));
        // --needtrig-uid <uid>: repeatable. See g_needTrigUids.
        if (!std::strcmp(argv[i], "--needtrig-uid"))
            g_needTrigUids.push_back(std::atoi(argv[i + 1]));
        // --needtrig-skip <n>: repeatable. See g_needSkip.
        if (!std::strcmp(argv[i], "--needtrig-skip"))
            g_needSkip |= touchBit(std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--bands")) g_bandPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--replay")) replayPath = argv[i + 1];
        // --fixups <file>: divergence-driven local overrides recorded by the
        // driver DURING THIS RUN (see Fixup above). The driver clears the file
        // at run start -- records never carry across runs, so a cold solve
        // stays cold; the permanent archive lives in fixups_log_lv*.txt which
        // nothing here ever reads.
        if (!std::strcmp(argv[i], "--fixups")) {
            std::ifstream ff(argv[i + 1]);
            std::string ln;
            while (std::getline(ff, ln)) {
                if (ln.size() >= 3 && (unsigned char)ln[0] == 0xEF &&
                    (unsigned char)ln[1] == 0xBB && (unsigned char)ln[2] == 0xBF)
                    ln.erase(0, 3);
                Fixup f{};
                double x, y, vy, dy, dvy;
                int in, mode, mini, flip, g, g2, kill = 0;
                const int n = std::sscanf(
                    ln.c_str(),
                    "x=%lf,in=%d,mode=%d,mini=%d,flip=%d,g=%d,"
                    "y=%lf,vy=%lf,dy=%lf,dvy=%lf,g2=%d,kill=%d",
                    &x, &in, &mode, &mini, &flip, &g, &y, &vy, &dy, &dvy,
                    &g2, &kill);
                if (n < 11) continue;
                f.x = (float)x; f.y = (float)y; f.vy = (float)vy;
                f.dy = (float)dy; f.dvy = (float)dvy;
                f.in = (uint8_t)in; f.mode = (uint8_t)mode;
                f.mini = (uint8_t)mini; f.flip = (uint8_t)flip;
                f.g = (uint8_t)g; f.gAfter = (uint8_t)g2;
                f.kill = (uint8_t)(kill != 0);
                // The second body, if this record was measured on a pair. Read
                // from a named tail rather than more positional fields, so a
                // file written before duals were recordable parses unchanged
                // and lands on dual=0 -- which is what it always meant.
                if (const char* d = std::strstr(ln.c_str(), ",dual2=")) {
                    double y2, vy2, dy2, dvy2;
                    int g2b;
                    if (std::sscanf(d, ",dual2=%lf,%lf,%lf,%lf,%d",
                                    &y2, &vy2, &dy2, &dvy2, &g2b) == 5) {
                        f.dual = 1;
                        f.y2 = (float)y2; f.vy2 = (float)vy2;
                        f.dy2 = (float)dy2; f.dvy2 = (float)dvy2;
                        f.gAfter2 = (uint8_t)g2b;
                    }
                }
                // ...and the second body's button, when the record named it (a two-player level)
                if (const char* d2 = std::strstr(ln.c_str(), ",in2=")) {
                    int v2;
                    if (std::sscanf(d2, ",in2=%d", &v2) == 1 && (v2 == 0 || v2 == 1)) f.in2 = (uint8_t)v2;
                }
                f.ord = (int)g_fixups.size();
                g_fixups.push_back(f);
            }
            for (const Fixup& fx : g_fixups)
                (fx.kill ? g_fixupKills : g_fixupDeltas).push_back(fx);
            auto byX = [](const Fixup& a, const Fixup& b) { return a.x < b.x; };
            std::stable_sort(g_fixupKills.begin(), g_fixupKills.end(), byX);
            std::stable_sort(g_fixupDeltas.begin(), g_fixupDeltas.end(), byX);
            // CONTRADICTORY delta pairs: the NEWER record wins. Two records
            // the matcher cannot tell apart (their key windows overlap) whose
            // outcomes disagree are two worldlines' answers written on one
            // key -- the lv22 lift at x=8,185 holds dvy=+3.426 from plans
            // where it was rising and 0.000 from plans where it sank, and
            // some of these transitions (the grounded ball's tap-flip phase
            // on a mover) are decided inside GD's collision pass, below the
            // model's resolution -- no rule can carry them, only the current
            // run's own measurement. The file is append-only within a run, so
            // the larger ord is the measurement of the LATEST worldline (the
            // one the converging plan actually lives in); the stale side is
            // dropped. Records that agree (adjacent-tick drift stays under
            // 0.5) coexist, and there is no blanket dyn gate on deltas.
            // DISABLED (2026-08-26): neither the drop-both nor the
            // newest-wins form ever produced a corridor breakout, while both
            // no-filter builds did (see the note at stepBoth's dyn gate).
            // Kept compiled behind the flag for the next measurement.
            if (g_fixupConflictFilter) {
                std::vector<char> drop(g_fixupDeltas.size(), 0);
                for (size_t a = 0; a < g_fixupDeltas.size(); ++a)
                    for (size_t b = a + 1;
                         b < g_fixupDeltas.size()
                         && g_fixupDeltas[b].x <= g_fixupDeltas[a].x + 2.4f;
                         ++b) {
                        const Fixup &p = g_fixupDeltas[a], &q = g_fixupDeltas[b];
                        if (p.in != q.in || p.mode != q.mode || p.mini != q.mini
                            || p.flip != q.flip || p.g != q.g || p.dual != q.dual)
                            continue;
                        if (std::fabs(p.y - q.y) > 8.0f
                            || std::fabs(p.vy - q.vy) > 2.0f)
                            continue;
                        if (std::fabs(p.dy - q.dy) > 0.5f
                            || std::fabs(p.dvy - q.dvy) > 0.5f)
                            drop[p.ord < q.ord ? a : b] = 1;
                    }
                size_t w = 0, gone = 0;
                for (size_t a = 0; a < g_fixupDeltas.size(); ++a) {
                    if (drop[a]) { ++gone; continue; }
                    g_fixupDeltas[w++] = g_fixupDeltas[a];
                }
                g_fixupDeltas.resize(w);
                if (gone)
                    std::printf("fixups: %zu contradictory delta records "
                                "dropped\n", gone);
            }
            if (!g_fixups.empty())
                std::printf("fixups: %zu transition overrides loaded\n",
                            g_fixups.size());
        }
        if (!std::strcmp(argv[i], "--dynhazpad")) g_dynHazPad = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--dynseamgap")) g_dynSeamGap = std::atoi(argv[i + 1]);
        // --histstat <file>: the fixed-length histories' fill after a replay (speed.hpp)
        if (!std::strcmp(argv[i], "--histstat")) {
            g_histStatOn = true;
            g_histStatPath = argv[i + 1];
        }
        if (!std::strcmp(argv[i], "--maxplayy")) g_maxPlayY = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--offboard") && i + 1 < argc)
            g_offBoardMargin = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--shiftdbg")) g_shiftDbgUid = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--deadband")) {
            double a0 = 0, a1 = 0, b0 = -1e18, b1 = 1e18; int md = -1;
            const int n = std::sscanf(argv[i + 1], "%lf,%lf,%d,%lf,%lf",
                                      &a0, &a1, &md, &b0, &b1);
            if (n >= 2 && a1 > a0)
                g_deadBands.push_back({a0, a1, n >= 3 ? md : -1,
                                       n >= 4 ? b0 : -1e18,
                                       n >= 5 ? b1 : 1e18});
        }
        if (!std::strcmp(argv[i], "--cubeyq")) g_cubeYq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--cubevq")) g_cubeVq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--start")) {
            // 30 fields (21st=frame, 22nd=rev, 23rd/24th=sprite rotation,
            // 25th=boost, 26th/27th=the second body's mode and size,
            // 28th=ticks since the last gravity flip, 29th=ticks since the
            // last id-1859 ceiling arm, 30th=rotStep). The mod's producer
            // (repair.hpp startArg) writes the first 27 only, so 28-30 reach
            // this parser from the offline instruments and one-off scripts
            // alone. New per-body history does not get a field here: it goes
            // through --anchor-state owns=hist (frames.hpp, kHistNames).
            // FORGET TO GROW THE SIZE AND
            // sscanf WRITES PAST THE ARRAY: when rev was added it was left at
            // 21, and it showed up as rev=0/1 not changing the result by a
            // single bit.
            // -1 in the last three = "the caller did not say", which is not the
            // same as 0 (a real mode / a flip on this very tick) -- see the
            // notes at the dual block and at State::flipT.
            // 31st-33rd (--rot2): the second body's sprite angle, spin sign and spin size;
            // -9999 = "not said" (the pair then starts from p1's, as it always did).
            double a[33] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, -1, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, 0, -9999, 0, 0};
            // 8th field = flip. It used to be absent entirely, so every
            // re-anchor taken while the player was upside down restarted the
            // solve in NORMAL gravity -- the tail was then solved for a world
            // that mirrors the real one. Levels that flip a lot (lv10) can only
            // re-anchor correctly with it. Older callers passing 7 fields still
            // work: a[7] stays 0.
            // 9th field = mini (from the dump's vsize: 1.0 normal, 0.6 mini).
            // Same class of bug as the flip field above -- re-anchoring past a
            // size portal without it restarts the tail at the wrong half extent.
            // 10th..14th = the SECOND player: dual, y2, vy2, flip2, grounded2.
            // Without them a re-anchor taken inside a dual section restarted
            // the solve as a single player, and the tail was then planned for a
            // world with half the bodies in it. lv16's dual runs from x=10,551
            // to x=15,103 and its wall at x=13,982 sits inside it, so EVERY
            // anchor the loop took there was wrong. Same class of bug as the
            // missing flip / mini / ufo fields before it.
            // 15th field = GD's OWN speed multiplier at the anchor (the dump's
            // `speed` column). The model used to re-derive the section's speed
            // by replaying every speed portal whose x is behind the anchor --
            // which is wrong exactly when the player did not TOUCH one, the case
            // the note above advanceX predicted. lv16 is that case: its id 201
            // ("0.9") portal sits at x=17,403 cy=561 (y=533..589) and the mini
            // UFO flies UNDER it at y~515, so GD keeps 1.1 for the rest of the
            // level while the model dropped to 0.9 -- a 0.3145 px/tick x error,
            // 24% slow, from x=17,403 all the way to the end. Every tail solved
            // past that point was timed for a level moving at the wrong speed,
            // which is why they came back SOLVED and then died in GD.
            // GD's measurement is exact and needs no rule, so prefer it.
            // 16th field = the ROBOT's remaining hover budget (rHover).
            // Without it every re-anchor taken mid-hover restarts with the
            // budget at 0, so the model falls at the mode's gravity through a
            // stretch where GD holds vy dead flat -- and the driver, whose fixup
            // comparison IS an anchored resim, records that as a divergence on
            // every single tick.
            // Measured on lv21 (2026-08-10): 45 of the 63 fixups at the x=18,705
            // wall were this, all `m5/mini1` with the same edy=0.0873 /
            // edvy=0.194, which reads exactly like one wrong constant. It is
            // not: anchored BEFORE the jump, so the model arms the budget
            // itself, it holds vy=4.472 and matches GD bit for bit
            // (y = 203.337 / 204.343 / 205.349 against GD's 203.336731 /
            // 204.342926 / 205.349121).
            // Worse than a wasted record: those 45 go back in through --fixups
            // and overwrite correct physics next to the wall.
            // Same family as the documented snapObj gap, and the same list still
            // has holes -- dashing / dashSlope / ringHold / usedOrb are all
            // still dropped by a re-anchor. lv21 holds one dash for 300+
            // ticks, so that one will bite next.
            // `usedPad` came off that list on 2026-09-06: the overlap rule below
            // covers a contact still in progress and --spentpad covers the pads
            // fired earlier in the attempt (g_spentPad's declaration). usedOrb
            // has no such producer -- an orb is consumed by a PRESS, so the
            // recording's x,y cannot say which ones were taken.
            // Callers passing 15 fields still work: a[15] stays 0, which is what
            // every non-robot anchor wants anyway.
            // 17th/18th = DASH state (dashing, dashSlope). Same family as the
            // 16th and the one that bites hardest: a dash is held for hundreds
            // of ticks (lv21 holds one at x=2,445 for 300+, and another at
            // x=18,105 for 69), so a re-anchor taken inside one restarts in free
            // fall through the whole thing.
            // Found the long way (2026-08-10): the hover-budget inference was
            // credited for lv21's x=18,105 stretch and the mini-ship fixups
            // collapsed 14 -> 2, which looked like a win. It was a COINCIDENCE.
            // The object at (18105,255) is a dash ring (type 37), not a hover:
            // GD sets vy := 0 and freezes y, and a hover AT vy=0 draws the same
            // line, so the wrong mechanism produced the right trajectory. It
            // only holds while the ring's rot is 0 (all of lv21's are) and while
            // the dash is shorter than the hover's 67-tick budget -- that
            // stretch is 69, so the last two ticks were already wrong.
            // 19th/20th = the STAIR SNAP state (m_objectSnappedTo's uid and
            // m_snapDistance), the "documented snapObj gap" the note above
            // names. An anchor without it restarts with snapObj = null, so the
            // first stair after the anchor cannot match a pattern (the gate
            // needs a PREVIOUS object) and the nudge is skipped -- the model
            // then runs 1 px behind GD for the whole rest of that simulation.
            // Measured 2026-08-10 with the segment harness (quick_regress's own
            // anchors, 400 ticks apart, lv1-20): 64 segments diverge with
            // dy = dvy = 0 and dx = -1.0000 exactly, all mode 0 on a block
            // surface, and every one of them sits EARLIER than the same level's
            // un-anchored replay first diverges -- i.e. they are artefacts of
            // the anchor, not of the physics. -1 = "no object" (the default).
            // 22nd = reverse (a same-frame firing of id 2900). Without it an
            // anchor taken after the reversal restarts with rev=0 and runs the
            // opposite way to GD.
            // 23rd/24th = THE PLAYER'S SPRITE ROTATION (the dump's `rot`
            // column) and its spin direction (rotNeg; the caller derives it
            // from the sign of rot(t0) - rot(t0-1)). State::rot is the only
            // angle used for "hitbox of a turned object", and an anchor
            // without it re-accumulates from rot=0, so mid-section it drifts
            // tens of degrees from GD. A different angle FLIPS THE VERDICT OF
            // THE TURNED-BOX TEST: measured on lv20 t=7,281 against the -46
            // degree teleport portal uid7021:
            //   GD:    rot=-420.303 -> 14.3 degrees off the portal axis -> no
            //          fire; fires next tick (rot=-422.033, 16.0 degrees off)
            //   model: rot re-accumulated from the anchor, effectively near 45
            //          degrees = the angle where the player's projection is
            //          largest (21.21); fires 1 tick early
            // It showed up as the 192 px warp landing 1 tick off
            // (fixcensus `m0/mini0/g0/gdg0/sp0.9/air/in0`, edy=+192.106).
            // 25th = the velocity-limit exemption (State::boost), read
            // straight off GD's byte [player+0x952]. An anchor taken during a
            // boosted stretch (a fast slope exit, a red ring/pad) without it
            // re-clamps the swing at 8 while GD keeps accelerating.
            // 26th/27th = THE SECOND BODY'S OWN MODE AND SIZE. -1 = the caller
            // did not say, and gets the old behaviour (copy the first body's);
            // see the note where they are applied. 0 is a real mode, so the
            // sentinel cannot be 0 the way the earlier optional fields' is.
            // 28th = TICKS SINCE GRAVITY LAST FLIPPED (State::flipT), which
            // decides whether the side/crush kill is still inside GD's 0.1 s
            // grace. -1 = the caller did not say, read as "no grace"; 0 is a
            // real value (flipped on the anchor tick), so it cannot be the
            // sentinel. An anchor taken in the 24 ticks after a blue pad or a
            // gravity portal without it kills states GD spares.
            // 30th = the SIZE of the spin (State::rotStep), deg/tick. Default 0
            // = "not turning", which is what every caller written before this
            // meant and is correct for a cube and for a grounded ball; it is
            // wrong only for an anchor landing mid-air on a ball, which is the
            // case the field exists for.
            std::sscanf(argv[i + 1],
                        "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,"
                        "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,"
                        "%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                        &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7],
                        &a[8], &a[9], &a[10], &a[11], &a[12], &a[13], &a[14],
                        &a[15], &a[16], &a[17], &a[18], &a[19], &a[20], &a[21],
                        &a[22], &a[23], &a[24], &a[25], &a[26], &a[27], &a[28],
                        &a[29], &a[30], &a[31], &a[32]);
            const uint8_t startRev = (uint8_t)((int)a[21] & 1);
            startFrame = (int)a[20] & 3;
            startSnapUid = (long long)a[18];
            startSnapDist = a[19];
            g_startSpeedMul = a[14];
            t0 = (long long)a[0];
            x0 = a[1];
            x0World = a[1];
            init = State{(float)a[2], (float)a[3], (uint8_t)a[4], (uint8_t)a[6],
                         (uint8_t)a[5], (uint8_t)a[7]};
            init.mini = (uint8_t)a[8];
            // Ticks since the last gravity flip; absent or negative = no grace.
            init.flipT = (a[27] < 0.0)
                             ? (uint8_t)kFlipGraceTicks
                             : (uint8_t)std::min<int>(kFlipGraceTicks,
                                                      (int)a[27]);
            // ...and since the last id-1859 touch; absent or negative = not
            // armed, which is the conservative answer (the ceiling kills).
            init.armT = (a[28] < 0.0)
                            ? (uint8_t)kArmTicks
                            : (uint8_t)std::min<int>(kArmTicks, (int)a[28]);
            // 21st field = GD's gameplay rotation (the dump's gframe, 0..3).
            // Re-anchored INSIDE a rotated section, the model had no way to
            // know which way it was running (rotation triggers already passed
            // count as fired), restarted in frame 0 and fell within 1 tick
            // (the "follow 1 tick" at lv22 t=11,340). GD's x,y are world, so
            // map them into frame coordinates before handing them over. vy's
            // sign mirrors in frame 3 only (same convention as the trace
            // writer).
            // NOTE: `init = State{...}` above REBUILDS init, so the rev
            // assignment is lost unless it comes after it (that is why frame
            // worked while rev did not: the frame assignment was always after
            // the rebuild). Reverse can occur in frame 0 too, so it sits
            // OUTSIDE the frame block below.
            init.rev = startRev;
            // (after the rebuild, same as rev -- see the NOTE above)
            init.boost = ((int)a[24] != 0) ? 1 : 0;
            if (((int)a[20] & 3) != 0) {
                const int sf = (int)a[20] & 3;
                double u, v;
                toFrame(sf, x0, a[2], u, v);
                x0 = u;
                init.y = (float)v;
                // vy too is MIRRORED WHEN f&2, for the same reason as flip
                // (the vertical v uses the world axis reversed for f=2 and
                // f=3). Only frame 3 was being mapped, so a frame-2 anchor
                // TURNED A FALL INTO A RISE: at lv22 t=20,450 GD falls at
                // 8.2px/tick while the model rose at 1.7px/tick, and 87 ticks
                // later it was passing through a spot 42px too high.
                init.vy = (float)(a[3] * ((sf & 2) ? -1.0 : 1.0));
                init.frame = (uint8_t)sf;
                // ...and FLIP TOO IS MIRRORED IN FRAME 3 (same convention as
                // gdUpOf). Forget to map it here and the state says "standing
                // right there" while gravity points the other way, and the
                // re-anchor floats upward from tick 1 (dvy=-0.432 = exactly 2
                // ticks' worth at lv22 t=11,340).
                // The mirror applies when the frame's vertical v uses the
                // world axis REVERSED: toFrame has f=0 v=Y, f=1 v=X, f=2 v=-Y,
                // f=3 v=-X. That is the two with f&2 (180 and 270 degrees).
                // frame 3 is confirmed by measurement
                // ([[gd-lv22-rotated-section]]); frame 2 follows the same
                // logic and was backed up by the t=12,878 anchor failing to
                // reproduce GD's fall (only the model stands on a ledge 130px
                // higher).
                //
                // [correction 2026-08-18] THE FLIP MIRROR IS NOT APPLIED HERE.
                // The caller (py/mkstart.py's measured table -- checked
                // against all 2,069 ticks of lv22) already does the
                // `GD 3/up1 -> model 3/flip0` conversion, and stacking it here
                // double-flips back to the original. The caller rewrites
                // gframe 2 as (frame 0, rev 1), so a 2 never arrives here and
                // the "frame 2 follows the same logic" above is currently
                // reached from nowhere.
                //
                // Measured (--start at lv22 t=17,000, robot / gframe3 / up1):
                //   with the double flip (flip=1): the model's vy goes 3.160
                //     -> 2.965 -> ..., FALLING 0.195/tick, while GD goes
                //     3.550 -> 3.745 -> ..., rising
                //   without it (flip=0): the internal vy runs -3.355 -> -3.550
                //     and the writer's mirror gives +3.550 = matches GD as is
                // This is what fixcensus's `m5/mini0/g0/gdg0/sp1.4/air`
                // (lv22 x3, edy=0.000 / edvy=+0.390 = two gravities' worth)
                // really was.
                // The vy-side mirror STAYS: the trace writer negates in frame
                // 3 only, so entry and exit are paired.
                // [note 2026-08-16] This loop is DEAD CODE: g_rotTrig is
                // filled by loadLevel (much later than this), so it is always
                // empty here. The intent (treat passed triggers as fired) was
                // taken over by --spentrot.
                for (RotTrig& r : g_rotTrig)
                    if (r.frame == sf) r.firedT = (int)t0;
            }
            init.rHover = (uint8_t)a[15];   // see the 16th-field note above
            // ...and `action` as well as `held`, from the same 7th field. They
            // are the same fact ("was the button down coming into this tick")
            // and the anchor only ever carried one of them. Everything that
            // asks whether a hold is CONTINUING reads s.action, not s.held --
            // the robot's hover, the dash, the UFO's rising edge -- so an anchor
            // with action=0 makes the very first tick look like a fresh press
            // after a release. For the robot that is fatal and silent: the hover
            // branch is skipped once, the else-branch clears the budget, and no
            // value of the 16th field can bring it back.
            //
            // Gated on the hover budget, NOT applied to every anchor. Seeding it
            // unconditionally is what the semantics say, but it costs three
            // levels in quick_regress -- lv12, lv18 and lv20 each lose a segment
            // from 400 ticks of follow to 1 (lv19 gains 114), which is the first
            // tick going a different way. The obvious suspect is the UFO's
            // rising edge (`act = input && !s.action`), where a seeded hold
            // turns a flap into nothing; that is a separate bug and a separate
            // measurement. A non-zero 16th field only ever comes from an anchor
            // the driver has already identified as mid-hover, i.e. one where the
            // button is down by construction, so this gate is exact for the case
            // it was measured on and inert everywhere else.
            init.dashing = (uint8_t)a[16];
            // --dashclamp (speed.hpp): the mod's tan(-rot), held inside GD's +-70 line
            init.dashSlope = dashSlopeClamped((float)a[17], (int)init.frame);
            // ...and a dash continues on `s.action` exactly like the hover, so
            // it needs the same seed for the same reason.
            if (a[15] > 0 || a[16] > 0) init.action = (uint8_t)a[6];
            g_startHoldField = (int)a[6];   // --holdseed reads it with the hist payload
            // 23rd/24th (note above). A caller that omits them leaves 0 = the
            // old behaviour.
            init.rot = (float)a[22];
            init.rotNeg = (uint8_t)(a[23] != 0.0 ? 1 : 0);
            // 30th: the SIZE of the spin (State::rotStep). Rides in the same
            // seeding path as the two fields above rather than getting one of
            // its own, because it is the same quantity measured from the same
            // two reference rows: the caller reads `rot(t0) - rot(t0-1)`, sends
            // the sign as the 24th and the magnitude here. No factor of 240 --
            // see State::rotStep for why the name says "step" and not "rate".
            //
            // BALL ONLY, and seedcheck is why. The caller measures the turn
            // whatever mode it is in, but this field is a ball's stake: nothing
            // stakes it in any other mode, so a whole run leaves it at 0 there.
            // Seeded unconditionally it disagreed on 25 of 105 sampled ticks,
            // every one of them a CUBE holding the cube's own step (t=400:
            // whole 0.000000, seeded 1.730773 = 180/0.43333334/240). Copying an
            // observation into a field that means something narrower is how a
            // seed ends up describing a state the run can never be in.
            //
            // WHAT THE GATE CANNOT FIX, and exactly who it reaches: an anchor
            // taken in a NON-ball mode while a ball's stake is still live. The
            // stake outlives the mode -- nothing clears it -- so a run that was
            // a ball, changed mode, and comes back to ball in mid-air is still
            // spending the old step, and the reference has no column that says
            // so. Seeded from a non-ball row this reads 0 and the anchored run
            // stops turning. That is the whole population: every other anchor
            // either re-derives its spin per tick (cube) or is re-staked on its
            // first tick down (grounded ball). 21 of 105 sampled ticks on lv22,
            // and quick_regress is per-level identical to a0c4c8f, so no
            // section in the corpus is anchored inside that window -- which is
            // a fact about the corpus, not a bound on the defect.
            if ((int)a[4] == 2) init.rotStep = (float)std::fabs(a[29]);
            // --sloperot: the same turn, read for a CUBE, says whether a ramp's launch staked the
            // spin (State::rotBoost): boostPlayer's size is half the take-off's, 0.8653846 (1.125
            // mini) against 1.7307692 (2.25), and nothing else stakes that size. A grounded cube's
            // easing never lands on it within the tolerance by more than chance, and the gate
            // below only reads it off the ground.
            if ((int)a[4] == 0 && (int)a[5] == 0
                && std::fabs(std::fabs(a[29]) - (a[8] != 0.0 ? 1.125 : 0.8653846)) < 2e-3)
                init.rotBoost = 1;
            // State::hitG (--repela0c): GD's +0xa0c is not in the dump; a body on the ground at t0
            // holds it, which is all a row can say (one that walked off an edge still holds it).
            init.hitG = (uint8_t)(a[5] != 0.0 ? 1 : 0);
            init.dual = (uint8_t)a[9];
            if (init.dual) {
                init.y2 = (float)a[10];
                init.vy2 = (float)a[11];
                init.flip2 = (uint8_t)a[12];
                init.grounded2 = (uint8_t)a[13];
                init.hitG2 = init.grounded2 ? 1 : 0;
                // The second body's own mode and size (26th/27th fields).
                //
                // These used to be seeded from the first body unconditionally,
                // because the dump had one `mode` column for the pair and an
                // anchor could not tell the two apart. The justification given
                // for it -- "they differ for at most the tick between one
                // clearing a mode portal's window and the other reaching it"
                // -- was never measurable, precisely because of the thing it
                // was justifying, and it is FALSE. Measured 2026-08-28 on the
                // rig `dualmode` (py/mklevel.py), which separates the halves
                // with floor-height portals: 3,471 CONSECUTIVE ticks with the
                // modes differing (p1 cube / p2 ship, 50% of the run) and 4,676
                // with the sizes differing.
                //
                // It is true of the official corpus -- lv16's whole cold run is
                // 207,761 dual ticks and the halves never differ -- and false
                // wherever the corpus is not, which now includes levels this
                // solver clears. So an anchor that is told takes what it is
                // told; -1 (or an older caller passing 25 fields) keeps the
                // copy, which is what every official anchor resolves to anyway.
                init.mode2 = (a[25] >= 0.0) ? (uint8_t)a[25] : init.mode;
                init.mini2 = (a[26] >= 0.0) ? (uint8_t)(a[26] != 0.0)
                                            : init.mini;
                // --rot2: the second body's own angle and spin (31st-33rd). Not said, it
                // starts from p1's -- the angle the pair's second half used to read anyway.
                {
                    const bool told = a[30] > -9998.0;
                    init.rot2 = told ? (float)a[30] : init.rot;
                    init.rotNeg2 = told ? (uint8_t)(a[31] != 0.0 ? 1 : 0) : init.rotNeg;
                    init.rotStep2 = (init.mode2 == 2)
                                        ? (told ? (float)std::fabs(a[32]) : init.rotStep) : 0.f;
                }
            }
        }
    }
    // seed the per-state float accumulator with the anchor's absolute x. The
    // rounding depends on the magnitude, so this cannot be deferred (see advanceX)
    // --swapbodies: the searched body is the level's second player, so the call's x is ITS x (the anchor row's x plus the offset --startp2dx names) and the
    // ghost's offset from it is the other way round.
    double dx2Start = (init.dual && init.frame == 0) ? g_startP2dx : 0.0;
    if (g_swapBodies && replayPath.empty() && init.dual && init.frame == 0) {
        x0 += dx2Start;
        x0World += dx2Start;
        dx2Start = -dx2Start;
    }
    init.xAbs = (float)x0;
    // --p2xoff: the second body's x offset at the anchor (--startp2dx; GD's p2x - x). Frame 0 only.
    if (init.dual && init.frame == 0) init.dx2 = (float)dx2Start;

    // --horizon N: stop as soon as the frontier has survived N ticks past the
    // anchor and emit that prefix as the plan. The driver only ever uses the
    // part up to the next death, so solving all the way to the level end every
    // iteration is wasted work (17k ticks re-solved to use 300 of them).
    long long horizon = 0;
    long long planCut = 0;   // >0: emit only the plan up to this tick
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--horizon")) horizon = std::atoll(argv[i + 1]);

    prepMark(0);
    // Read only from here on: the level loader takes it by const pointer, and the rest look up.
    const GroupTimeline& gt = groupLayersFor(groupsPaths);   // ...and sets g_groupInit
    prepMark(1);
    if (!trigPath.empty() && !grpPath.empty()) {
        // The window stays off in general -- the note at the cap records a measured
        // failure with it ON at an early anchor (the removed --touch-from-anchor) -- but
        // there is one case where NOT windowing is strictly worse than anything:
        // an anchor so deep that the plain first-32 list lies entirely behind it.
        // There the model can see no door ahead at all, needUnseen has nothing to
        // force, and a section built on touch-doors is a dead end by construction
        // (lv22's shaft: four chained doors at x=20,081..20,141 against 155 touch
        // triggers in the level -- the roof at (20115,1215) could never open, and
        // every climb died under it while the machinery asked for capacity).
        // Auto-window exactly then: the levels below 33 triggers never trip this,
        // and early anchors keep the measured-safe plain list.
        g_touchRetimeFrom = t0;   // --touchretime re-times only boxes entered from here on
        // THE COIN SIDE FIRST, because the probe below decides the gate from a
        // list loadTouchTriggers builds, and both of these change that list: a
        // Count or Tap trigger is a root only when its chain reaches a coin
        // (g_coinUids), and a box is kept as a feeder only when its chain
        // reaches something items.txt says gives the item a coin's counter
        // reads (g_itemGiver, filled by loadCollectibles). Loaded after the
        // probe, the probe would be deciding on a population the run does not
        // use -- which is the fault --trigwinsel exists to name.
        if (g_coinRoute) {
            g_coinUids = coinUids(argv[1]);
            std::printf("coins: %zu coin uids for the trigger window\n",
                        g_coinUids.size());
        }
        if (gatesModelled() && !itemPath.empty()) {   // the pickups feed the gates
            loadCollectibles(itemPath);
            if (!g_collect.empty())
                std::printf("items: %zu pickups\n", g_collect.size());
            // ...and the item givers (every row that names an item, touchable or not), which the
            // touch window's feeder test and the gate tables read.
            std::printf("items: %zu item givers\n", g_itemGiver.size());
        }
        // The types the chains reach (the relevance filter reads them, --trigdump
        // prints them). Read here because loadLevel has not run yet and this is
        // the only place that knows the dump's path.
        // MOVED ABOVE THE GATE (2026-09-21) so the probe below can use it: see
        // g_trigWinSel.
        // In-process, argv[1] is a placeholder and the level is g_levelCsv (see
        // loadObjTypesFrom). --csvtypes reads it from there.
        // The cached tables are read in place rather than copied (117k entries each on a custom
        // level): nothing below writes to either.
        std::unordered_map<int, int> objTypesOwn;
        const std::unordered_map<int, int>* objTypesP = &objTypesOwn;
        if (!g_levelCsv.empty()) {
            LevelTextCache& lc = levelTextFor(g_levelCsv);
            if (!lc.haveTypes) {
                std::istringstream cs(g_levelCsv);
                lc.types = loadObjTypesFrom(cs);
                lc.haveTypes = true;
            }
            objTypesP = &lc.types;
        } else {
            objTypesOwn = loadObjTypes(argv[1]);
        }
        const std::unordered_map<int, int>& objTypes = *objTypesP;
        bool winTouch = false;   // the window is opened by the auto-window gate below (and --touchwinahead)
        // A rotated-frame anchor is placed by its WORLD x (the boxes' cx are
        // world), and windows around it (triggers.hpp, g_trigWinNear).
        const bool rotWin = startFrame != 0 && x0World > -1e17;
        const double xGate = rotWin ? x0World : x0;
        if (!winTouch && xGate > -1e17) {
            // --trigwinsel: probe the population this call will ACTUALLY load.
            // Without it the probe runs with the selection off while g_touch
            // below runs with it on (it has been on by default since 934c21e),
            // so the gate is decided on a list the run never uses: on lv22 the
            // probe's last box is at cx 2,283 and the loaded set reaches 3,675,
            // and the gate fires ~1,400 px of anchor too early. Early windowing
            // is the direction the note at the cap records a measured failure
            // for, so this is not a safe inconsistency -- but it does change
            // which anchors window, so it is off until measured.
            const std::vector<TouchTrig> plain =
                loadTouchTriggers(trigPath, grpPath, -1e18,
                                  &objTypes);
            // THE LAST BOX, NOT THE LAST ENTRY. With --coins the list ends with
            // the roots held outside the cap (Counts, Taps, Item Compares --
            // triggers.hpp), which have no x the player passes, and lv22's tap
            // at x=15,525 sat last in every probe: no anchor ever windowed, and
            // every coin run carried the boxes of x=511..3,674 all the way to
            // the end (the loop's own `[trigwin] win=0 maxKeptX=3674`). Without
            // coins nothing is held and the max is the last entry, as before.
            double lastBox = -1e18;
            for (const TouchTrig& e : plain)
                if (e.count < 0 && !e.tap) lastBox = std::max(lastBox, e.cx);
            if ((int)plain.size() >= kTouchBits && !plain.empty()
                && lastBox < xGate - 200.0) {
                winTouch = true;
                // ...and say how many, rather than "32": the cap is kTouchBits
                // and the probe's own count is what the test just used.
                std::printf("triggers: the plain %d all lie behind the anchor "
                            "(last %.0f < %.0f) - windowing from the anchor\n",
                            (int)plain.size(), lastBox, xGate);
            } else if (g_touchWinAhead && g_trigDroppedAhead > 0) {
                // --touchwinahead (triggers.hpp): the plain list drops a box this call can reach.
                // Read off the probe's own globals, before the load below overwrites them.
                const double dxReach = std::max((double)kDxF, g_startSpeedMul > 0.0
                                                ? (double)dxForSpeedMul(g_startSpeedMul) : 0.0);
                const double reach = (double)(horizon > 0 ? horizon : 3000) * dxReach;
                if (g_trigFirstAheadX < xGate + reach) {
                    winTouch = true;
                    std::printf("triggers: the plain %d drop a box ahead at x=%.0f within "
                                "the anchor's reach (%.0f + %.0f) - windowing from the "
                                "anchor\n", (int)plain.size(), g_trigFirstAheadX, xGate,
                                reach);
                }
            }
        }
        // --movetarget: object positions, for a target-mode Move's destination.
        // In-process the level is g_levelCsv, not argv[1] (a placeholder there).
        std::unordered_map<int, std::pair<double, double>> objPosOwn;
        const std::unordered_map<int, std::pair<double, double>>* objPosP = &objPosOwn;
        if (!g_levelCsv.empty()) {
            LevelTextCache& lc = levelTextFor(g_levelCsv);
            if (!lc.havePos) {
                std::istringstream cs(g_levelCsv);
                lc.pos = loadObjPos(cs);
                lc.havePos = true;
            }
            objPosP = &lc.pos;
        } else {
            std::ifstream fs(argv[1]);
            if (fs) objPosOwn = loadObjPos(fs);
        }
        const std::unordered_map<int, std::pair<double, double>>& objPos = *objPosP;
        prepMark(5);
        std::printf("movetarget: %zu object positions from %s\n", objPos.size(),
                    g_levelCsv.empty() ? "the dump" : "the in-process level table");
        if (rotWin && winTouch) g_trigWinNear = x0World;
        // --spawnlag: the spawn-triggered objects' x, from the same table as the positions.
        {
            if (!g_levelCsv.empty()) {
                LevelTextCache& lc = levelTextFor(g_levelCsv);
                if (!lc.haveSpawnX) {
                    std::istringstream cs(g_levelCsv);
                    lc.spawnX = loadObjSpawnX(cs);
                    lc.haveSpawnX = true;
                }
                g_spawnableX = lc.spawnX;
            } else {
                std::ifstream fs(argv[1]);
                if (fs) g_spawnableX = loadObjSpawnX(fs);
            }
            std::printf("spawnlag: %zu spawn-triggered objects\n", g_spawnableX.size());
        }
        g_touch = loadTouchTriggers(trigPath, grpPath, winTouch ? x0 : -1e18,
                                    &objTypes,
                                    &objPos);
        for (size_t b = 0; b < g_touch.size(); ++b) {
            int n = 0, lo = 1 << 30, hi = 0;
            for (const TrigCtl& c : g_touch[b].ctl)
                if (c.spawnLag > 0) {
                    ++n;
                    lo = std::min(lo, c.spawnLag);
                    hi = std::max(hi, c.spawnLag);
                }
            if (n > 0)
                std::printf("spawnlag: box %zu uid %d (x=%.0f) delays %d of %zu objects "
                            "by %d..%d ticks\n", b, g_touch[b].uid, g_touch[b].cx, n,
                            g_touch[b].ctl.size(), lo, hi);
        }
        g_trigWinNear = -1e18;
        // ...and publish what that decided, because nothing else can see it:
        // the numbers are printed and the mod has no pipe. See SolveOutcome.
        g_outcome.trigWinTouch = g_touch.empty() ? -1 : (winTouch ? 1 : 0);
        g_outcome.trigTotal = (long long)g_trigTotal;
        g_outcome.trigRelevantN = (long long)g_trigRelevantN;
        g_outcome.trigKept = (long long)g_trigKept;
        g_outcome.trigDroppedRelevant = (long long)g_trigDroppedRelevant;
        g_outcome.trigDroppedBehind = (long long)g_trigDroppedBehind;
        g_outcome.trigDroppedAhead = (long long)g_trigDroppedAhead;
        g_outcome.trigMaxKeptX = g_trigMaxKeptX;
        // FNV-1a over the uids in BIT ORDER. Order is the point -- the same
        // set of boxes in a different order is a different bit->uid map and
        // must hash differently, which a set hash would hide.
        {
            unsigned long long h = 1469598103934665603ull;
            for (const TouchTrig& t : g_touch) {
                unsigned long long u = (unsigned long long)(unsigned)t.uid;
                for (int k = 0; k < 8; ++k) {
                    h ^= (u >> (k * 8)) & 0xffull;
                    h *= 1099511628211ull;
                }
            }
            g_outcome.trigMapSig = g_touch.empty() ? 0ull : h;
        }
        buildTouchMoveTicks();   // the dedupe key's "is this box still moving"
        // ...and "is the button down near a toggle block" (g_pressWin). 15 is the largest
        // half any mode has, 20 more covers a tick's travel at the fastest speed, since the
        // box reads the PARENT's button and so the held parent has to survive the tick before.
        g_pressWin.clear();
        g_actMask = TouchMask{};
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            if (g_touch[b].press)
                g_pressWin.push_back({g_touch[b].cx - g_touch[b].hw - 35.0,
                                      g_touch[b].cx + g_touch[b].hw + 15.0, (int)b});
            if (g_touch[b].activator) g_actMask |= touchBit((int)b);
        }
        g_chanSwitch.clear();
        readChanSwitchers(rotQPath.empty() ? rotQPathBeside(argv[1]) : rotQPath);   // --chantog
        g_autoTrig = loadAutoTriggers(trigPath, grpPath);
        // --autostop: for every entry, the Stop entries that name its root Move.
        g_autoStopBy.assign(g_autoStop ? g_autoTrig.size() : 0, {});
        if (g_autoStop) {
            std::unordered_map<int, int> autoIdx;
            for (size_t a = 0; a < g_autoTrig.size(); ++a)
                if (g_autoTrig[a].stopMoves.empty()) autoIdx[g_autoTrig[a].uid] = (int)a;
            for (size_t s = 0; s < g_autoTrig.size(); ++s)
                for (const int mu : g_autoTrig[s].stopMoves) {
                    const auto f = autoIdx.find(mu);
                    if (f != autoIdx.end()) g_autoStopBy[(size_t)f->second].push_back((int)s);
                }
        }
        // --tapdisarm: tie the triggers each box's chain switches to their g_autoTrig entries.
        // --boxarm: the same ties, for the toggle blocks that switch an offInit Move back on (triggers.hpp).
        g_autoDisarm.assign(g_autoTrig.size(), {});
        {
            std::unordered_map<int, std::vector<int>> autoIdx;
            for (size_t a = 0; a < g_autoTrig.size(); ++a) autoIdx[g_autoTrig[a].uid].push_back((int)a);
            size_t nTied = 0;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
                for (const TouchTrig::TogEv& e : g_touch[b].trigTog) {
                    const auto f = autoIdx.find(e.uid);
                    if (f == autoIdx.end()) continue;
                    for (const int a : f->second) {
                        g_autoDisarm[(size_t)a].push_back({(int)b, e.on, e.delay});
                        ++nTied;
                        std::printf("tapdisarm: box %zu (uid %d) switches %s the x-crossing trigger uid %d "
                                    "(x=%.0f) +%.0f\n", b, g_touch[b].uid, e.on ? "on" : "off", e.uid,
                                    g_autoTrig[(size_t)a].cx, e.delay);
                    }
                }
            std::printf("tapdisarm: %zu switch(es) of x-crossing triggers by boxes\n", nTied);
        }
        // --touchoff: tie each box's own Toggles to their g_autoTrig entries (fireT). A Toggle
        // with no entry has no crossing tick here, so its box is left as it was.
        if (g_touchOwnOff) {
            std::unordered_map<int, int> autoIdx;
            for (size_t a = 0; a < g_autoTrig.size(); ++a) autoIdx[g_autoTrig[a].uid] = (int)a;
            for (TouchTrig& tb : g_touch) {
                if (tb.ownTog.empty()) continue;
                bool all = true;
                for (TouchTrig::OwnTog& e : tb.ownTog) {
                    const auto f = autoIdx.find(e.uid);
                    e.autoIdx = f == autoIdx.end() ? -1 : f->second;
                    if (e.autoIdx < 0) all = false;
                }
                std::printf("touchoff: box uid %d at (%.0f,%.0f) -", tb.uid, tb.cx, tb.cy);
                for (const TouchTrig::OwnTog& e : tb.ownTog)
                    std::printf(" %s at x=%.0f (Toggle uid %d)", e.on ? "on" : "off", e.cx, e.uid);
                std::printf("%s\n", all ? "" : " - a Toggle has no crossing tick, left as it was");
                if (!all) tb.ownTog.clear();
            }
        }
    }
    prepMark(2);
    if (!obbPath.empty()) g_obb = loadObb(obbPath);   // before loadLevel reads it
    // The flags decide the SHAPE of a circular hazard's test, so they have to
    // be in before anything asks hazardHit anything.
    if (setPath.empty()) setPath = settingsPathBeside(argv[1]);
    if (!setPath.empty() && loadLevelSettings(setPath) && g_fixRadiusCollision)
        std::printf("levelsettings: fixRadiusCollision=1 - circular hazards "
                    "use centre distance (branch A) in this level\n");
    // --endportalx <x>: the end portal's x for --goalgd where the dump has none (an older one), as GD
    // reported it; it wins over the dump's
    if (endPortalArg > 0.0) g_endPortalX = endPortalArg;
    // An attempt after a ladder's first takes the level that one built (LadderLevelCache); a
    // --replay whose inputs are the last --replay's takes that one's (ReplayLevelCache).
    Level L;
    {
        std::vector<std::string> la;
        if (g_ladder) la = ladderArgs(argc, argv);
        bool isReplay = false;
        for (int i = 2; i < argc && !isReplay; ++i) isReplay = !std::strcmp(argv[i], "--replay");
        isReplay = isReplay && !g_ladder;
        if (!isReplay) g_replayLevel = ReplayLevelCache{};
        std::vector<std::string> ra;
        std::vector<std::pair<std::string, std::string>> rf;
        bool replayKeyed = false;
        if (isReplay) {
            replayKeyed = true;
            for (int i = 0; i < argc; ++i) {
                const bool fixVal = i > 0 && !std::strcmp(argv[i - 1], "--fixups");
                ra.push_back(fixVal ? std::string() : std::string(argv[i]));
                if (fixVal || i < 2) continue;
                const std::string a = argv[i];
                if (a.empty() || a[0] == '-') continue;
                if (std::find(groupsPaths.begin(), groupsPaths.end(), a) != groupsPaths.end())
                    continue;   // the timeline, keyed by g_groupLayersGen
                // Any value that reads as a file is keyed by its bytes (one that does not -- a
                // number, a tick list, a missing file -- is in `ra` as text already).
                std::string bytes;
                if (readFileBytes(a, bytes)) rf.emplace_back(a, std::move(bytes));
            }
        }
        // --ctrlwintrig (frames.hpp): the crossings answer past the recording's reach; with recorded windows
        // and no reach they are taken as complete, and with neither (a call from the level's head) from the start
        g_ctrlTrigFrom = g_recProvGiven ? (long long)g_recProvReach
                                        : (g_ctrlWin.empty() ? -1LL : std::numeric_limits<long long>::max());
        if (g_ladder && g_ladderLevel.ladder == g_ladder
            && g_ladderLevel.groupsGen == g_groupLayersGen && g_ladderLevel.args == la) {
            L = g_ladderLevel.level;
            g_ladderLevel.writes.put();
            std::fputs(g_ladderLevel.printed.c_str(), stdout);
        } else if (replayKeyed && g_replayLevel.valid && g_replayLevel.args == ra
                   && g_replayLevel.groupsGen == g_groupLayersGen && g_replayLevel.csv == g_levelCsv
                   && g_replayLevel.files == rf) {
            L = g_replayLevel.level;
            g_replayLevel.writes.put();
            std::fputs(g_replayLevel.printed.c_str(), stdout);
            if (g_levelCacheCheck) {
                const uint64_t hc = levelHashOf(g_replayLevel.level), hl = levelHashOf(L);
                std::printf("levelcachecheck: %s (stored %016llx cached %016llx copy %016llx)\n",
                            (hc == g_replayLevel.hash && hl == g_replayLevel.hash) ? "reuse ok" : "MISMATCH",
                            (unsigned long long)g_replayLevel.hash, (unsigned long long)hc,
                            (unsigned long long)hl);
            }
        } else {
            std::string printed;
            // The level text's pass before its rows (CsvRows), made once per text and kept with it. Not with the
            // glitch-avoid mode, which adds rows before that pass (loadLevelFrom builds it as it always did).
            const CsvRows* rowsP = nullptr;
            if (g_csvRows && !g_levelCsv.empty() && !g_glitchDeco) {
                LevelTextCache& lc = levelTextFor(g_levelCsv);
                if (!lc.haveRows) {
                    lc.rows = buildCsvRows(g_levelCsv);
                    lc.haveRows = true;
                }
                rowsP = &lc.rows;
            }
            if (g_ladder || replayKeyed) g_loadPrinted = &printed;
            // --csvrowscheck: the kept pass against one made afresh from the text, before it is used (a stale one
            // would show here). The rows a call reads and the lists made from them are tested in dptest
            // (keptTextPass); a hash of two separately built levels is no comparison (the hash reads padding).
            if (g_csvRowsCheck && rowsP) {
                const CsvRows fresh = buildCsvRows(g_levelCsv);
                bool same = fresh.noDrop == rowsP->noDrop && fresh.maxUid == rowsP->maxUid
                            && fresh.filtered == rowsP->filtered && fresh.places.size() == rowsP->places.size();
                for (size_t i = 0; same && i < fresh.places.size(); ++i)
                    same = fresh.places[i].uid == rowsP->places[i].uid && fresh.places[i].cx == rowsP->places[i].cx
                           && fresh.places[i].cy == rowsP->places[i].cy;
                std::printf("csvrowscheck: %s (%zu places, max uid %d, %s)\n", same ? "same" : "MISMATCH",
                            rowsP->places.size(), rowsP->maxUid, rowsP->noDrop ? "nothing dropped" : "rows dropped");
            }
            L = loadLevel(argv[1], groupsPaths.empty() ? nullptr : &gt,
                          g_touch.empty() ? nullptr : &g_touch,
                          g_autoTrig.empty() ? nullptr : &g_autoTrig, rowsP);
            g_loadPrinted = nullptr;
            if (g_bootAuto) {
                size_t nb = 0;
                for (const uint8_t b : L.dyn.bootRows) nb += b;
                std::printf("bootauto: %zu object(s) read from the left-out recording\n", nb);
            }
            if (g_dualOrbLate) std::printf("dualorblate: on\n");   // the line that says the flag was read
            if (replayKeyed) {
                g_replayLevel = ReplayLevelCache{};
                g_replayLevel.valid = true;
                g_replayLevel.args = std::move(ra);
                g_replayLevel.groupsGen = g_groupLayersGen;
                g_replayLevel.csv = g_levelCsv;
                g_replayLevel.files = std::move(rf);
                g_replayLevel.level = L;
                g_replayLevel.writes.take();
                g_replayLevel.printed = printed;
                if (g_levelCacheCheck) g_replayLevel.hash = levelHashOf(g_replayLevel.level);
            }
            if (g_ladder) {
                g_ladderLevel.ladder = g_ladder;
                g_ladderLevel.args = std::move(la);
                g_ladderLevel.groupsGen = g_groupLayersGen;
                g_ladderLevel.level = L;
                g_ladderLevel.writes.take();
                g_ladderLevel.printed = std::move(printed);
            }
        }
    }
    prepMark(3);
    // --gravspawn (modifiers.hpp): each spawned gravity trigger an x-crossing Spawn (delay 0) fires goes on
    // the crossing list at that Spawn's x. After the load or the cache's put, which hold the list without them.
    // --gravchain / --tptrigspawn: the x a chain's spawn fires at. A delay of d seconds reaches the trigger
    // ceil(d*240) - 1 ticks after an immediate chain would (the queue fires in updateSpawnTriggers, before the
    // tick's move: --spawnlag's measurement), so the crossing x moves on by that many ticks of the speed at the
    // root. That holds only while x runs at that speed: a delay across a speed portal, a time warp or a teleport
    // that moves x gives NaN, and the trigger stays out (a custom level's Spawn at x 1,275 with 5.08 s fires its
    // teleport at x 2,546 in GD, past two teleports; the speed at the root put it at 2,858).
    auto spawnFireX = [&](double rootX, double delayS) {
        if (delayS <= 0.0) return rootX;
        double dxR = kDxF;
        for (const Obj& o : L.speeds) {
            if (o.cx - o.hw - kCubeHalf > rootX) break;
            dxR = dxForSpeedId(o.id);
        }
        const double lag = std::max(0.0, std::ceil(delayS * 240.0 - 1e-9) - 1.0);
        const double fx = rootX + lag * dxR;
        auto inside = [&](double x) { return x > rootX && x <= fx; };
        for (const Obj& o : L.speeds)
            if (inside(o.cx - o.hw - kCubeHalf)) return std::nan("");
        for (const TimeWarp& w : g_timeWarps)
            if (inside(w.cx)) return std::nan("");
        for (const TpTrig& t : g_tpTrigs)
            if (inside(t.cx) && !t.ignX) return std::nan("");
        for (const Obj& p : L.portals)
            if (p.type == 28 && (p.tpKeep & 4) && inside(p.cx)) return std::nan("");
        return fx;
    };
    if (g_gravSpawn && g_gravChain && !g_gravSpawned.empty() && !trigPath.empty()
        && !grpPath.empty()) {
        const auto* rows = trigRowsCached(trigPath);
        const auto* mem = groupMembersCached(grpPath);
        int added = 0;
        if (rows && mem) {
            std::unordered_map<int, double> want;
            for (const GravSpawned& p : g_gravSpawned) want.emplace(p.uid, p.cx);
            for (const SpawnFire& f : spawnFiresOf(want, *rows, *mem))
                for (const GravSpawned& p : g_gravSpawned) {
                    if (p.uid != f.uid) continue;
                    const double fx = spawnFireX(f.rootX, f.delayS);
                    if (std::isnan(fx)) {
                        std::printf("gravspawn: uid %d (root uid %d at x=%.0f, delay %.3fs) is delayed across a "
                                    "speed change, a warp or a teleport - NOT modelled\n", p.uid, f.rootUid,
                                    f.rootX, f.delayS);
                        continue;
                    }
                    g_gravTrigs.push_back({fx, p.mod, p.uid});
                    ++added;
                    std::printf("gravspawn: uid %d gravity x%.3f at x=%.0f (root uid %d at x=%.0f, delay %.3fs)\n",
                                p.uid, p.mod, fx, f.rootUid, f.rootX, f.delayS);
                }
        }
        if (added)
            std::stable_sort(g_gravTrigs.begin(), g_gravTrigs.end(),
                             [](const GravTrig& a, const GravTrig& b) { return a.cx < b.cx; });
    } else if (g_gravSpawn && !g_gravSpawned.empty() && !trigPath.empty() && !grpPath.empty()) {
        const auto* rows = trigRowsCached(trigPath);
        const auto* mem = groupMembersCached(grpPath);
        int added = 0;
        if (rows && mem) {
            std::vector<const TrigRow*> spawns;
            for (const auto& kv : *rows)
                if (kv.second.id == 1268 && !kv.second.touch && !kv.second.spawn && kv.second.target != 0
                    && kv.second.sdelay == 0.0)
                    spawns.push_back(&kv.second);
            std::sort(spawns.begin(), spawns.end(),
                      [](const TrigRow* a, const TrigRow* b) { return a->uid < b->uid; });
            for (const GravSpawned& p : g_gravSpawned)
                for (const TrigRow* S : spawns) {
                    const auto gi = mem->find(S->target);
                    if (gi == mem->end()
                        || std::find(gi->second.begin(), gi->second.end(), p.uid) == gi->second.end())
                        continue;
                    g_gravTrigs.push_back({S->cx, p.mod, p.uid});
                    ++added;
                    std::printf("gravspawn: uid %d gravity x%.3f at x=%.0f (Spawn uid %d)\n", p.uid, p.mod,
                                S->cx, S->uid);
                }
        }
        if (added)
            std::stable_sort(g_gravTrigs.begin(), g_gravTrigs.end(),
                             [](const GravTrig& a, const GravTrig& b) { return a.cx < b.cx; });
    }
    // --tptrigoff (modifiers.hpp): a crossing teleport trigger a Toggle may have switched off by the time it is
    // reached stays out -- the last x-crossing Toggle behind it turns its group off (offAtCrossing), or a spawned
    // Toggle-off does whose spawner stands at or behind it (what that spawner decides is not read here).
    if (g_teleportTrig && g_tpTrigOff && !g_tpTrigs.empty() && !trigPath.empty() && !grpPath.empty()) {
        const auto* rows = trigRowsCached(trigPath);
        const auto* mem = groupMembersCached(grpPath);
        if (rows && mem) {
            std::vector<TpTrig> keep;
            for (const TpTrig& t : g_tpTrigs) {
                const auto r = rows->find(t.uid);
                bool off = false;
                if (r != rows->end()) {
                    bool spawnOn = false;
                    off = offAtCrossing(r->second, *rows, *mem, &spawnOn) && !spawnOn;
                    if (!off)
                        forTogglesHolding(t.uid, *rows, *mem, [&](const TrigRow& X) {
                            if (!X.spawn || X.togon != 0 || off) return;
                            for (const auto& kv : *rows) {
                                const TrigRow& S = kv.second;
                                if (S.target == 0 || S.cx > t.cx) continue;
                                const auto gi = mem->find(S.target);
                                if (gi != mem->end()
                                    && std::find(gi->second.begin(), gi->second.end(), X.uid) != gi->second.end()) {
                                    off = true;
                                    return;
                                }
                            }
                        });
                }
                if (off) {
                    std::printf("teleporttrig: uid %d at x=%.0f is in a group a Toggle switches off before it is "
                                "reached - NOT modelled (--tptrigoff)\n", t.uid, t.cx);
                    continue;
                }
                keep.push_back(t);
            }
            g_tpTrigs.swap(keep);
        }
    }
    // --tptrigspawn (modifiers.hpp): each spawned teleport trigger an x-crossing chain fires goes on the crossing
    // list at the x it fires at, if its target is not behind that x (x is the search's clock).
    if (g_teleportTrig && g_tpTrigSpawn && !g_tpSpawned.empty() && !trigPath.empty() && !grpPath.empty()) {
        const auto* rows = trigRowsCached(trigPath);
        const auto* mem = groupMembersCached(grpPath);
        int added = 0;
        if (rows && mem) {
            std::unordered_map<int, double> want;
            for (const TpTrig& t : g_tpSpawned) want.emplace(t.uid, t.cx);
            for (const SpawnFire& f : spawnFiresOf(want, *rows, *mem))
                for (const TpTrig& t : g_tpSpawned) {
                    if (t.uid != f.uid) continue;
                    const double fx = spawnFireX(f.rootX, f.delayS);
                    if (std::isnan(fx)) {
                        std::printf("teleporttrig: uid %d (root uid %d at x=%.0f, delay %.3fs) is delayed across a "
                                    "speed change, a warp or a teleport - NOT modelled\n", t.uid, f.rootUid,
                                    f.rootX, f.delayS);
                        continue;
                    }
                    const bool back = !t.ignX && t.tx < fx - 0.5 && !g_tpBack;   // --tpback takes it
                    std::printf("teleporttrig: uid %d spawned at x=%.0f (root uid %d at x=%.0f, delay %.3fs) -> "
                                "(%.1f, %.1f)%s\n", t.uid, fx, f.rootUid, f.rootX, f.delayS, t.tx, t.ty,
                                back ? " moving x backwards - NOT modelled"
                                     : "");
                    if (back) continue;
                    TpTrig u = t;
                    u.cx = fx;
                    g_tpTrigs.push_back(u);
                    ++added;
                }
        }
        if (added)
            std::stable_sort(g_tpTrigs.begin(), g_tpTrigs.end(),
                             [](const TpTrig& a, const TpTrig& b) { return a.cx < b.cx; });
    }
    // --tptouch / --gravtouch (modifiers.hpp): ...and the spawned ones a touch-triggered Spawn or Toggle-on fires
    // with no delay, each as a box of that root's rect. Before the anchor's seed below, which goes last.
    const bool tpTouchChain = g_tpTouch && g_teleportTrig && !g_tpSpawned.empty();
    const bool gravTouchChain = g_gravTouch && !g_gravSpawned.empty();
    if ((tpTouchChain || gravTouchChain) && !trigPath.empty() && !grpPath.empty()) {
        const auto* rows = trigRowsCached(trigPath);
        const auto* mem = groupMembersCached(grpPath);
        if (rows && mem && tpTouchChain) {
            std::unordered_map<int, double> want;
            for (const TpTrig& t : g_tpSpawned) want.emplace(t.uid, t.cx);
            for (const SpawnFire& f : spawnFiresOf(want, *rows, *mem, true))
                for (const TpTrig& t : g_tpSpawned) {
                    if (t.uid != f.uid) continue;
                    const bool back = !t.ignX && t.tx < f.rootX - f.rootHw - 0.5 && !g_tpBack;   // --tpback takes it
                    std::printf("teleporttrig: uid %d spawned by the touch box uid %d at (%.0f,%.0f), delay %.3fs -> "
                                "(%.1f, %.1f)%s\n", t.uid, f.rootUid, f.rootX, f.rootY, f.delayS, t.tx, t.ty,
                                f.delayS > 0.0 ? " delayed - NOT modelled"
                                : back ? " moving x backwards - NOT modelled" : "");
                    if (f.delayS > 0.0 || back) continue;
                    TpTrig u = t;
                    u.cx = f.rootX;
                    g_tpTouchBoxes.push_back({f.rootX, f.rootY, f.rootHw, f.rootHh, u, f.rootUid});
                }
            std::stable_sort(g_tpTouchBoxes.begin(), g_tpTouchBoxes.end(),
                             [](const TpTouchBox& a, const TpTouchBox& b) { return a.cx < b.cx; });
            if (g_tpTouchBoxes.size() > 254) g_tpTouchBoxes.resize(254);
        }
        if (rows && mem && gravTouchChain) {
            std::unordered_map<int, double> want;
            for (const GravSpawned& p : g_gravSpawned) want.emplace(p.uid, p.cx);
            for (const SpawnFire& f : spawnFiresOf(want, *rows, *mem, true))
                for (const GravSpawned& p : g_gravSpawned) {
                    if (p.uid != f.uid) continue;
                    std::printf("gravtouch: uid %d gravity x%.3f spawned by the touch box uid %d at (%.0f,%.0f), "
                                "delay %.3fs%s\n", p.uid, p.mod, f.rootUid, f.rootX, f.rootY, f.delayS,
                                f.delayS > 0.0 ? " - NOT modelled" : "");
                    if (f.delayS > 0.0) continue;
                    g_gravTouchBoxes.push_back({f.rootX, f.rootY, f.rootHw, f.rootHh, p.mod, p.uid});
                }
            std::stable_sort(g_gravTouchBoxes.begin(), g_gravTouchBoxes.end(),
                             [](const GravTouchBox& a, const GravTouchBox& b) { return a.cx < b.cx; });
            if (g_gravTouchBoxes.size() > 253) g_gravTouchBoxes.resize(253);
        }
    }
    // --eventtrig (modifiers.hpp): the spawned gravity and teleport triggers an Event trigger on the crossing
    // queue reaches with no delay, as entries no overlap fires (appended after the boxes' sort, before the seed).
    g_eventFires.clear();
    if (g_eventTrig && (!g_gravSpawned.empty() || !g_tpSpawned.empty()) && !trigPath.empty() && !grpPath.empty()) {
        const auto* rows = trigRowsCached(trigPath);
        const auto* mem = groupMembersCached(grpPath);
        if (rows && mem) {
            std::vector<const TrigRow*> roots;
            for (const auto& kv : *rows)
                if (kv.second.id == 3604 && !kv.second.touch && !kv.second.spawn && kv.second.chan == 0
                    && kv.second.target != 0 && !kv.second.events.empty())
                    roots.push_back(&kv.second);
            std::sort(roots.begin(), roots.end(), [](const TrigRow* a, const TrigRow* b) { return a->uid < b->uid; });
            std::unordered_map<int, double> wantG, wantT;
            for (const GravSpawned& p : g_gravSpawned) wantG.emplace(p.uid, p.cx);
            for (const TpTrig& t : g_tpSpawned) wantT.emplace(t.uid, t.cx);
            for (const TrigRow* R : roots) {
                if (R->evx1 != 0 || R->evx2 != 0) {
                    std::printf("eventtrig: uid %d at x=%.0f filters on an extra ID (%d:%d) - NOT modelled\n", R->uid,
                                R->cx, R->evx1, R->evx2);
                    continue;
                }
                std::string evs;
                for (const int e : R->events) evs += (evs.empty() ? "" : ",") + std::to_string(e);
                if (!wantG.empty())
                    for (const SpawnFire& f : spawnFiresFromRoots({R}, wantG, *rows, *mem))
                        for (const GravSpawned& p : g_gravSpawned) {
                            if (p.uid != f.uid) continue;
                            std::printf("eventtrig: uid %d at x=%.0f on event(s) %s spawns gravity x%.3f (uid %d)%s\n",
                                        R->uid, R->cx, evs.c_str(), p.mod, p.uid,
                                        f.delayS > 0.0 ? " delayed - NOT modelled" : "");
                            if (f.delayS > 0.0 || g_gravTouchBoxes.size() >= 253) continue;
                            g_gravTouchBoxes.push_back({R->cx, -1e9, 0.0, 0.0, p.mod, -1});
                            g_eventFires.push_back({R->events, R->cx, 1, (int)g_gravTouchBoxes.size() - 1});
                        }
                if (g_teleportTrig && !wantT.empty())
                    for (const SpawnFire& f : spawnFiresFromRoots({R}, wantT, *rows, *mem))
                        for (const TpTrig& t : g_tpSpawned) {
                            if (t.uid != f.uid) continue;
                            std::printf("eventtrig: uid %d at x=%.0f on event(s) %s spawns the teleport uid %d -> "
                                        "(%.1f, %.1f)%s\n", R->uid, R->cx, evs.c_str(), t.uid, t.tx, t.ty,
                                        f.delayS > 0.0 ? " delayed - NOT modelled" : "");
                            if (f.delayS > 0.0 || g_tpTouchBoxes.size() >= 254) continue;
                            TpTrig u = t;
                            u.cx = R->cx;
                            g_tpTouchBoxes.push_back({R->cx, 0.0, 0.0, 0.0, u, R->uid, true});
                            g_eventFires.push_back({R->events, R->cx, 0, (int)g_tpTouchBoxes.size() - 1});
                        }
            }
        }
    }
    // --tpback (modifiers.hpp): every source of a jump back gets its farthest x in g_tpBackHi -- a crossing or
    // spawned trigger its own x, a touch box and a portal their left edge, an orb its x -- and the anchor's own
    // farthest x (--startxhi) one more, which the state starts on.
    g_tpBackHi.clear();
    g_tpBackObj.clear();
    if (g_tpBack) {
        int nBack = 0;
        for (TpTrig& t : g_tpTrigs)
            if (!t.ignX && (t.tx < t.cx - 0.5 || g_tpTrigOnce)) {   // --tptrigonce: every crossing trigger fires once (modifiers.hpp)
                t.backIdx = tpBackIdx(t.cx);
                ++nBack;
            }
        for (TpTouchBox& b : g_tpTouchBoxes)
            if (!b.t.ignX && b.t.tx < b.cx - b.hw - 0.5) {
                b.t.backIdx = tpBackIdx(b.cx - b.hw);
                ++nBack;
            }
        for (const Obj& p : L.portals)
            if (p.type == 28 && (p.tpKeep & 8) && p.uid >= 0) {
                g_tpBackObj[p.uid] = tpBackIdx(p.cx - p.hw);
                ++nBack;
            }
        for (const Obj& o : L.orbs)
            if (o.type == 46 && !(o.tpKeep & 1) && (o.tpEx != 0.0 || o.tpEy != 0.0) && o.tpEx < o.cx - 0.5
                && o.uid >= 0) {
                g_tpBackObj[o.uid] = tpBackIdx(o.cx);
                ++nBack;
            }
        if (nBack)
            std::printf("tpback: %d teleport(s) move x back; the crossings read the farthest x reached\n", nBack);
        // (not a reversed anchor: running back is not a jump, and the queue reads the other way there)
        if (!std::isnan(g_startXhi) && g_startXhi > x0 + 0.5 && init.frame == 0) {
            const int i = tpBackIdx(g_startXhi);
            if (i >= 0) init.tpBack = (uint8_t)(i + 1);
            std::printf("tpback: the anchor at x=%.1f has been as far as x=%.1f\n", x0, g_startXhi);
        }
    }
    // --startedge <ticks>: how long the button had been in the state the anchor starts in, so --wavepulse / --minpulse
    // hold across the join (State::edgeAge counts the ticks after the run's first, hence one less). Unsaid = 255, "long ago".
    if (g_startEdge >= 0) init.edgeAge = (uint8_t)std::min(254, g_startEdge);
    // --revcarry (speed.hpp): GD's reverse correction as the anchor holds it (--startrev), still to be paid back
    if (!std::isnan(g_startRevOff) && init.frame == 0) {
        init.revOff = (float)g_startRevOff;
        init.revCarry = (float)g_startRevCarry;
    }
    // --gravtouch (modifiers.hpp): GD's gravity value at the anchor (--startgmod) where the crossing triggers
    // give another -- a touched trigger before the anchor set it -- stands as an entry of its own at the anchor's
    // x, which the state starts on. After the gravspawn entries, which the crossing value includes.
    if (g_gravTouch && !std::isnan(g_startGmod) && init.frame == 0) {
        const double crossX = crossXOf(x0, g_tpBack ? init.tpBack : 0);
        const double cross = gravModOf(crossX, 0);
        if (std::fabs(g_startGmod - cross) > 1e-6) {
            g_gravTouchBoxes.push_back({crossX, -1e9, 0.0, 0.0, g_startGmod, -1});
            init.gravTouch = (uint8_t)g_gravTouchBoxes.size();
            std::printf("gravtouch: the anchor at x=%.1f starts at gravity x%.3f (the crossing triggers give x%.3f)\n",
                        x0, g_startGmod, cross);
        }
    }
    // A level this build cannot represent: say so and return 2 -- the exit code the
    // loader's old std::exit(2) gave the CLI, without ending the host process when
    // this runs inside the game (see Level::unsupported).
    if (!L.unsupported.empty()) {
        std::printf("unsupported: %s\n", L.unsupported.c_str());
        std::fprintf(stderr, "level unsupported: %s\n", L.unsupported.c_str());
        g_outcome.unsupported = L.unsupported;
        return 2;
    }
    g_baseLv = &L;   // see levelOfFrame / applyRotation's re-tap
    // --areamove: the Area Moves of the shape the formula covers, tied to the objects of
    // their target groups, and an object armed where the formula reproduces its recorded rows from the recorded
    // run's x (--xtrack) and its first row as the base.
    buildCamRows();   // --areascreen: the camera track (--camtrack, or the xtrack's camera columns) as a lookup by x
    g_areas.clear();
    L.dyn.areaOf.clear();
    L.dyn.areaArmed.clear();
    L.dyn.areaAdd.clear();
    // A call without --areas (a kit collected before the mod passed it): areas.txt beside the objrects dump, or in
    // the kit's own directory when that dump is in its snaps/ (oneoff lvl_areas.py writes one from the level).
    if (areasPath.empty() && argc > 1) {
        std::string dir = argv[1];
        const size_t sl = dir.find_last_of("/\\");
        dir = sl == std::string::npos ? std::string(".") : dir.substr(0, sl);
        std::vector<std::string> cand{dir + "/areas.txt"};
        const size_t sl2 = dir.find_last_of("/\\");
        if (sl2 != std::string::npos && dir.substr(sl2 + 1) == "snaps") cand.push_back(dir.substr(0, sl2) + "/areas.txt");
        for (const std::string& c : cand)
            if (std::ifstream(c)) {
                areasPath = c;
                std::printf("areamove: no --areas, read %s\n", c.c_str());
                break;
            }
    }
    // Said, not silent: a kit collected before the mod wrote areas.txt has none, and the flag then does nothing.
    if (areasPath.empty() || grpPath.empty())
        std::printf("areamove: nothing taken - no %s\n", areasPath.empty() ? "--areas file (areas.txt)"
                                                                          : "--objgroups file");
    else if (!std::ifstream(areasPath))
        std::printf("areamove: nothing taken - cannot read %s\n", areasPath.c_str());
    if (!areasPath.empty() && !grpPath.empty() && !L.dyn.objs.empty()) {
        std::ifstream af(areasPath);
        std::string line;
        std::vector<std::string> head;
        size_t nRead = 0;
        if (af && std::getline(af, line)) {
            std::stringstream hs(line);
            for (std::string c; std::getline(hs, c, ',');) head.push_back(c);
        }
        auto col = [&](const std::vector<std::string>& f, const char* name, double dflt) {
            for (size_t k = 0; k < head.size() && k < f.size(); ++k)
                if (head[k] == name) return f[k].empty() ? dflt : std::atof(f[k].c_str());
            return dflt;
        };
        while (af && std::getline(af, line)) {
            std::vector<std::string> f;
            std::stringstream ls(line);
            for (std::string c; std::getline(ls, c, ',');) f.push_back(c);
            ++nRead;
            AreaAct a;
            a.uid = (int)col(f, "uid", -1);
            a.id = (int)col(f, "id", 0);
            a.cx = col(f, "cx", 0);
            a.target = (int)col(f, "target", 0);
            a.special = (int)col(f, "special", 0);
            a.dirType = (int)col(f, "dirType", 0);
            a.xy = col(f, "xy", 0) != 0;
            a.inbound = col(f, "inbound", 0) != 0;
            a.len = col(f, "len", 0);
            a.off = col(f, "off", 0);
            a.modFront = col(f, "modFront", 1);
            a.modBack = col(f, "modBack", 1);
            if (col(f, "twoDir", 0) != 0) a.modFront = -std::fabs(a.modFront);   // processAreaEffects, +0x7f4
            a.deadzone = col(f, "deadzone", 0);
            a.moveX = col(f, "moveX", 0);
            a.moveY = col(f, "moveY", 0);
            a.eInType = (int)col(f, "eInType", 0);
            a.eInRate = col(f, "eInRate", 2);
            a.eInBuf = (int)col(f, "eInBuf", -3);
            a.moveDist = col(f, "moveDist", 0);
            a.angle = col(f, "angle", 0);
            a.scaleX = col(f, "scaleX", 1);
            a.scaleY = col(f, "scaleY", 1);
            a.rot = col(f, "rot", 0);
            // --areaadd: GD reads the pair the mode names (x / y, or angle / distance), so the other pair's variation is moot
            const bool modeXY = g_areaAdd && a.id == 3006 && a.xy, modeAD = g_areaAdd && a.id == 3006 && !a.xy;
            const bool varies = col(f, "lenV", 0) != 0 || col(f, "offV", 0) != 0 || col(f, "offYV", 0) != 0
                                || (!modeAD && (col(f, "moveXV", 0) != 0 || col(f, "moveYV", 0) != 0))
                                || (!modeXY && (col(f, "moveDistV", 0) != 0 || col(f, "angleV", 0) != 0))
                                || col(f, "scaleXV", 0) != 0 || col(f, "scaleYV", 0) != 0 || col(f, "rotV", 0) != 0;
            // --areaoutsame (speed.hpp): an ease-out trigger whose out curve is its in curve gives the same answer
            // whichever one GD's per-object state picks
            const bool outSame = col(f, "eOut", 0) == 0
                                 || (g_areaOutSame && col(f, "eOutType", 0) == col(f, "eInType", 0)
                                     && col(f, "eOutRate", 2) == col(f, "eInRate", 2)
                                     && col(f, "eOutBuf", -3) == col(f, "eInBuf", -3));
            // --areascreen: a centre that is a point of the screen (special -3 .. -11), when the track carries the camera
            if (a.special <= -3 && a.special >= -11 && !g_camRows.empty()) {
                a.screen = true;
                a.fx = (a.special == -4 || a.special == -5 || a.special == -6) ? 0.0
                     : (a.special == -9 || a.special == -10 || a.special == -11) ? 1.0 : 0.5;
                a.special = -1;
            }
            const bool shape = (a.id == 3006 || a.id == 3007 || a.id == 3008) && a.special == -1 && a.dirType == 1
                               && !varies && col(f, "relative", 0) == 0 && outSame && a.len > 0;
            if (shape) g_areas.push_back(a);
        }
        const auto* members = groupMembersCached(grpPath);
        Dynamics& D = L.dyn;
        const size_t N = D.objs.size();
        D.areaOf.assign(N, {});
        D.areaArmed.assign(N, 0);
        D.areaBaseX.assign(N, 0.0);
        D.areaBaseY.assign(N, 0.0);
        D.areaBaseHa.assign(N, 0.0);
        D.areaBaseHb.assign(N, 0.0);
        D.areaBaseRot.assign(N, 0.0);
        std::unordered_map<int, size_t> dynIdx;
        for (size_t i = 0; i < N; ++i) dynIdx[D.objs[i].uid] = i;
        if (members)
            for (size_t k = 0; k < g_areas.size(); ++k) {
                const auto gi = members->find(g_areas[k].target);
                if (gi == members->end()) continue;
                for (const int u : gi->second)
                    if (const auto di = dynIdx.find(u); di != dynIdx.end()) D.areaOf[di->second].push_back((int)k);
            }
        size_t nCand = 0, nArmed = 0;
        double worstArmed = 0.0;
        for (size_t i = 0; i < N; ++i) {
            // --areaunrec: an object with its base row alone (the recording ended before the action reached
            // it) and nothing else moving it is a candidate too; the check below is then the action's pose
            // at the track's last tick, which has to be the base the recording shows. A custom level's spike
            // uid 44875 (Area Move +60 y, uid 39982): the recording ended at 6,847, the rise starts at 6,892,
            // and the model, unarmed, held it at its base and killed the cube on it at t=6,975.
            const bool oneRow = g_areaUnrec && D.samples[i].size() == 1 && D.autoAnchor[i] < 0
                                && !D.trigMask[i].any() && D.partsAny[i].empty();
            if (D.areaOf[i].empty() || (D.samples[i].size() < 2 && !oneRow) || g_xTrack.empty()) continue;
            ++nCand;
            const auto& sm = D.samples[i];
            const double bx = sm[0].cx, by = sm[0].cy, r0 = sm[0].rot;
            bool before = true;   // the base row comes before every action runs
            for (const int k : D.areaOf[i])
                before = before && (xRefAt(std::max(1, sm[0].t - 1)) < g_areas[(size_t)k].cx || sm[0].t <= 1);   // --areastart
            // the unturned half sizes from the base row's rect: only at a right angle
            const double q = std::fmod(std::fabs(r0), 180.0);
            const bool right = q < 1e-3 || std::fabs(q - 90.0) < 1e-3 || std::fabs(q - 180.0) < 1e-3;
            const bool swap = std::fabs(q - 90.0) < 1e-3;
            const double ha = swap ? sm[0].hh : sm[0].hw, hb = swap ? sm[0].hw : sm[0].hh;
            double worst = before ? 0.0 : 1e9;
            for (size_t r = 1; r < sm.size() && worst <= kAreaArmTol; ++r) {
                const double prx = xRefAt(std::max(1, sm[r].t - 1));
                const AreaPose p = areaPoseOf(D.areaOf[i], bx, by, ha, hb, r0, prx, std::max(1, sm[r].t - 1));
                if (p.sized && !right) { worst = 1e9; break; }
                worst = std::max({worst, std::fabs(p.cx - (double)sm[r].cx), std::fabs(p.cy - (double)sm[r].cy)});
                if (p.sized)
                    worst = std::max({worst, std::fabs(p.hw - (double)sm[r].hw), std::fabs(p.hh - (double)sm[r].hh)});
            }
            if (oneRow && worst <= kAreaArmTol) {
                const AreaPose p = areaPoseOf(D.areaOf[i], bx, by, ha, hb, r0, xRefAt(g_xTrack.back().first), g_xTrack.back().first);
                if (p.sized && !right) worst = 1e9;
                else worst = std::max({worst, std::fabs(p.cx - bx), std::fabs(p.cy - by)});
                if (p.sized) worst = std::max({worst, std::fabs(p.hw - (double)sm[0].hw), std::fabs(p.hh - (double)sm[0].hh)});
            }
            if (g_dynDbg == D.objs[i].uid || g_dynDbg == -6)
                std::printf("areamove: uid %d, %zu action(s), base (%.3f,%.3f), worst %.4f px over %zu rows -> %s\n",
                            D.objs[i].uid, D.areaOf[i].size(), bx, by, worst, sm.size(),
                            worst <= kAreaArmTol ? "armed" : "not armed");
            if (worst <= kAreaArmTol) {
                D.areaArmed[i] = 1;
                D.areaBaseX[i] = bx;
                D.areaBaseY[i] = by;
                D.areaBaseHa[i] = ha;
                D.areaBaseHb[i] = hb;
                D.areaBaseRot[i] = r0;
                worstArmed = std::max(worstArmed, worst);
                ++nArmed;
                D.anyAuto = true;
            }
        }
        std::printf("areamove: %zu area trigger(s) read, %zu of the covered shape; %zu of %zu object(s) armed "
                    "(worst %.4f px)%s\n", nRead, g_areas.size(), nArmed, nCand, worstArmed,
                    g_xTrack.empty() ? " - no --xtrack, nothing armed" : "");
    }
    if (g_nanSlopeDropped)   // --nanslope (constants.hpp)
        std::printf("nanslope: %d ramp(s) with no line (scale 0, sy0/sy1 nan) left out of the level\n",
                    g_nanSlopeDropped);
    // --boxarm (constants.hpp): an object an offInit entry moves, anchors or locks is placed by the state's boxes,
    // so it is not a pure object (dynamics.hpp isPure).
    L.dyn.armDep.clear();
    {
        Dynamics& D = L.dyn;
        const size_t N = D.objs.size();
        D.armDep.assign(N, 0);
        auto offInit = [](int k) {
            return k >= 0 && (size_t)k < g_autoTrig.size() && g_autoTrig[(size_t)k].offInit != 0;
        };
        size_t nDep = 0;
        for (size_t i = 0; i < N; ++i) {
            bool dep = (i < D.autoAnchor.size() && offInit(D.autoAnchor[i]))
                       || (i < D.autoLockTrig.size() && offInit(D.autoLockTrig[i]));
            if (i < D.autoParts.size())
                for (const Dynamics::AutoPart& p : D.autoParts[i]) dep = dep || offInit(p.trig);
            if (i < D.partsAny.size())
                for (const Dynamics::AutoPart& p : D.partsAny[i]) dep = dep || offInit(p.trig);
            if (dep) {
                D.armDep[i] = 1;
                ++nDep;
            }
        }
        std::printf("boxarm: %zu moving object(s) take a Move that only a box switches back on\n", nDep);
    }
    ++g_dynPlaceGen;   // the purity-relevant vectors (areaArmed, armDep) are final for this call: see Dynamics::incBuild
    // --ctrltouch (modifiers.hpp): a recorded window that opens ahead of the anchor where the anchor's
    // x, run on at its own speed, is over a box that switches the controls off is that touch's -- the
    // state fires it or not now -- and goes. One the anchor is already inside stays: the anchored
    // state has no controls. The x is an estimate (a speed change on the way moves it).
    // ...and a box a held button switches off: its group is the target of a Touch trigger in hold
    // mode with toggle "off", crossed (not touch- or spawn-started) before the box (CtrlTouchBox::holdOff)
    if (!g_ctrlTouchBoxes.empty() && !trigPath.empty() && !grpPath.empty()) {
        const auto* rows = trigRowsCached(trigPath);
        const auto* mem = groupMembersCached(grpPath);
        if (rows && mem)
            for (const auto& kv : *rows) {
                const TrigRow& T = kv.second;
                if (T.id != 1595 || T.thold != 1 || T.ttog != 2 || T.touch || T.spawn) continue;
                const auto g = mem->find(T.target);
                if (g == mem->end()) continue;
                for (CtrlTouchBox& b : g_ctrlTouchBoxes)
                    if (!b.holdOff && T.cx < b.cx
                        && std::find(g->second.begin(), g->second.end(), b.uid) != g->second.end()) {
                        b.holdOff = 1;
                        std::printf("ctrltouch: uid %d is off while the button is held (Touch uid %d "
                                    "at x=%.0f)\n", b.uid, T.uid, T.cx);
                    }
            }
    }
    // --holdtap (object.hpp): the moving objects in the group of a Touch trigger in hold mode with toggle
    // "off", crossed on the x queue, are switched by the state's button from the trigger's x on
    if (!trigPath.empty() && !grpPath.empty() && !L.dyn.objs.empty()) {
        if (!g_heldAll)
            std::printf("holdtap: nothing taken - needs --heldall (the button in every mode)\n");
        const auto* rows = trigRowsCached(trigPath);
        const auto* mem = groupMembersCached(grpPath);
        if (g_heldAll && rows && mem)
            for (const auto& kv : *rows) {
                const TrigRow& T = kv.second;
                if (T.id != 1595 || T.thold != 1 || T.ttog != 2 || T.touch || T.spawn) continue;
                const auto g = mem->find(T.target);
                if (g == mem->end()) continue;
                int nHaz = 0, nOther = 0;
                double lo = 1e18, hi = -1e18;
                for (Obj& o : L.dyn.objs) {
                    if (std::find(g->second.begin(), g->second.end(), o.uid) == g->second.end()) continue;
                    if (o.type != 2 && o.type != 47) { ++nOther; continue; }   // hazards only
                    if (o.holdTapX <= 0.f || (float)T.cx < o.holdTapX) o.holdTapX = (float)T.cx;
                    lo = std::min(lo, o.cx - o.hw);
                    hi = std::max(hi, o.cx + o.hw);
                    ++nHaz;
                }
                if (nHaz) g_holdTapSpans.push_back({lo, hi});
                std::printf("holdtap: Touch uid %d at x=%.0f (group %d) switches %d moving hazard(s) off while "
                            "the button is held (x %.0f..%.0f)%s\n", T.uid, T.cx, T.target, nHaz, lo, hi,
                            nOther ? " - its other moving objects are left to the recording" : "");
            }
    }
    if (!g_ctrlTouchBoxes.empty() && !g_ctrlWin.empty()) {
        const double dxA = g_startSpeedMul > 0.0 ? (double)dxForSpeedMul(g_startSpeedMul) : (double)kDxF;
        std::vector<std::pair<long long, long long>> kept;
        for (const auto& w : g_ctrlWin) {
            const CtrlTouchBox* hit = nullptr;
            if (w.first > t0) {
                const double xw = x0 + (double)(w.first - t0) * dxA;
                for (const CtrlTouchBox& b : g_ctrlTouchBoxes)
                    if (b.on && std::fabs(xw - b.cx) <= b.hw + 15.0 + dxA) { hit = &b; break; }
            }
            if (hit)
                std::printf("ctrltouch: window %lld:%lld dropped (x ~%.1f at its start is over uid %d)\n",
                            w.first, w.second, x0 + (double)(w.first - t0) * dxA, hit->uid);
            else
                kept.push_back(w);
        }
        g_ctrlWin.swap(kept);
    }
    g_gpHandoff = L.gpHandoff;   // shared gravity-portal bits (prelude.hpp); empty on most levels
    if (!g_gpHandoff.empty())
        std::printf("gravity portals: %zu bits shared past %d, handed on at x=%.1f..%.1f\n",
                    g_gpHandoff.size(), kGravPortalBits, g_gpHandoff.front().x,
                    g_gpHandoff.back().x);
    // TouchTrig::followIdx: a box whose own group a Move displaces follows a recorded object
    // moved by exactly the same Moves -- the same set of Move-targeted groups, so neither can
    // travel without the other -- the nearest one to the box. The trigger itself has no row in
    // any recording. Printed per box, so a run says which boxes move and after what.
    // markTouched adds the object's WHOLE displacement, which is the box's own only when
    // everything that moves either of them is a translation of whole groups (Move in any
    // mode, Follow, Follow Player Y) and both carry the same set of such groups. A Rotate,
    // a Scale, an Area effect or an Advanced Follow moves each object by an amount that
    // depends on where it is, so a box under one is left unfollowed and says so -- lv22's
    // pulse boxes 197/277/334 turn with the arms of groups 38-40, at another radius than any
    // recorded object. (An Edit Area's target may be an effect id rather than a group; the
    // test then refuses more than it needs to, never less.)
    // --advfollow (speed.hpp): the objects an Advanced Follow trigger (3016) carries along the player's x are placed at the player's x of the tick before
    // once it has crossed the trigger (Dynamics::placeAdvFollow). What a 3016 follows is in its own save string (the triggers.txt column advp, GD's writing
    // of the properties it parsed): the trigger acts on Target Player 1 (138=1) along x only (306=1, disassembly: GJBaseGameLayer::processAdvancedFollowAction
    // zeroes the y part for the byte that key sets and the x part for 307's), with nothing else that changes the step -- no follow group (71), no Player 2 (200),
    // no mods (72/73), no speed, delay, offset or max speed (90/91/92/105), nothing from the keys measured on no level yet. Only the keys below are accepted
    // on the row; any other key leaves the trigger's group to its recording. A dump without the column acts on nothing.
    if (!trigPath.empty() && !grpPath.empty() && !L.dyn.objs.empty()) {
        const auto* rowsF = trigRowsCached(trigPath);
        const auto* memF = groupMembersCached(grpPath);
        if (rowsF && memF) {
            // keys that say where the trigger stands, which layer, which groups it is in, whether it fades: not what it does
            static const std::unordered_set<int> kNeutral = {1, 2, 3, 20, 24, 36, 51, 57, 64, 67, 96, 155};
            std::unordered_map<int, double> followX;      // uid -> the crossing x of its Advanced Follow
            std::unordered_set<int> otherMoved;           // uids some other transform acts on
            int nTrig = 0, nOk = 0, nNoCol = 0;
            for (const auto& kv : *rowsF) {
                const TrigRow& r = kv.second;
                if (r.target <= 0) continue;
                bool follow = false;
                if (r.id == 3016 && !r.touch && !r.spawn && r.center == 0) {
                    ++nTrig;
                    if (r.advp.empty()) ++nNoCol;
                    else {
                        std::unordered_map<int, std::string> keys;
                        for (size_t p = 0; p < r.advp.size();) {
                            const size_t e = r.advp.find('|', p);
                            const std::string pr = r.advp.substr(p, e == std::string::npos ? std::string::npos : e - p);
                            const size_t q = pr.find('=');
                            if (q != std::string::npos) keys[std::atoi(pr.c_str())] = pr.substr(q + 1);
                            if (e == std::string::npos) break;
                            p = e + 1;
                        }
                        bool ok = keys.count(138) && keys[138] == "1" && keys.count(306) && keys[306] == "1";
                        for (const auto& k : keys) if (!kNeutral.count(k.first) && k.first != 138 && k.first != 306) ok = false;
                        follow = ok;
                        if (ok) ++nOk;
                    }
                }
                const bool transform = r.id == 901 || r.id == 1346 || r.id == 1347 || r.id == 1814 || r.id == 2067
                                       || (r.id >= 3006 && r.id <= 3008) || (r.id >= 3011 && r.id <= 3013) || (r.id == 3016 && !follow);
                const auto it = memF->find(r.target);
                if (it == memF->end()) continue;
                for (const int uid : it->second) {
                    if (follow) {
                        const auto f = followX.find(uid);
                        if (f == followX.end()) followX[uid] = r.cx;
                        else otherMoved.insert(uid);   // two follows: not this rule
                    } else if (transform) otherMoved.insert(uid);
                }
            }
            L.dyn.advFollowX.assign(L.dyn.objs.size(), -1e30);
            int nF = 0, nSkip = 0;
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                const auto f = followX.find(L.dyn.objs[i].uid);
                if (f == followX.end()) continue;
                if (otherMoved.count(L.dyn.objs[i].uid)) { ++nSkip; continue; }
                L.dyn.advFollowX[i] = f->second;
                ++nF;
            }
            L.dyn.anyAdvFollow = nF > 0;
            if (nTrig)
                std::printf("advfollow: %d Advanced Follow trigger(s), %d on Player 1 along x only with no other key (%d without the advp column); %d object(s) follow the player (previous tick's x) from the trigger's crossing, %d left to their recording (another transform acts on them)\n",
                            nTrig, nOk, nNoCol, nF, nSkip);
        }
    }    if (!g_touch.empty() && !trigPath.empty() && !grpPath.empty() && !L.dyn.objs.empty()) {
        // (the trigger rows and the group members come from the parse caches, which parse a file again
        // only when its bytes change; this block used to parse both files afresh in every call)
        const auto* rowsP = trigRowsCached(trigPath);
        if (rowsP) {
            const std::unordered_map<int, TrigRow>& rows = *rowsP;
            std::unordered_set<int> moved;   // groups some Move displaces
            std::unordered_set<int> rigid;   // groups a whole-group translation acts on
            std::unordered_set<int> bent;    // groups a position-dependent transform acts on
            for (const auto& kv : rows) {
                const TrigRow& r = kv.second;
                if (r.target <= 0) continue;
                if (r.id == 901 && (r.ox != 0.0 || r.oy != 0.0)) moved.insert(r.target);
                if (r.id == 901 || r.id == 1347 || r.id == 1814) rigid.insert(r.target);
                if (r.id == 1346 || r.id == 2067 || (r.id >= 3006 && r.id <= 3008)
                    || (r.id >= 3011 && r.id <= 3013) || r.id == 3016)
                    bent.insert(r.target);
            }
            std::unordered_map<int, std::vector<int>> movedOf;   // uid -> its moved groups
            std::unordered_map<int, std::vector<int>> rigidOf;   // uid -> its translated groups
            std::unordered_set<int> bentUid;
            if (!moved.empty() || !rigid.empty() || !bent.empty()) {
                // each object's groups among the three sets, from the members of those groups: an object
                // listed twice in a group is pushed twice, as the line-by-line read it replaces pushed
                // a group it named twice; the vectors are sorted below, so the order they fill in is free
                if (const auto* mem = groupMembersCached(grpPath)) {
                    for (const int g : moved) {
                        const auto it = mem->find(g);
                        if (it != mem->end()) for (const int uid : it->second) movedOf[uid].push_back(g);
                    }
                    for (const int g : rigid) {
                        const auto it = mem->find(g);
                        if (it != mem->end()) for (const int uid : it->second) rigidOf[uid].push_back(g);
                    }
                    for (const int g : bent) {
                        const auto it = mem->find(g);
                        if (it != mem->end()) for (const int uid : it->second) bentUid.insert(uid);
                    }
                }
                for (auto& kv : movedOf) std::sort(kv.second.begin(), kv.second.end());
                for (auto& kv : rigidOf) std::sort(kv.second.begin(), kv.second.end());
            }
            const auto rigidSig = [&](int uid) {
                const auto it = rigidOf.find(uid);
                return it == rigidOf.end() ? std::vector<int>{} : it->second;
            };
            int nFollow = 0, nRefused = 0;
            for (TouchTrig& T : g_touch) {
                const auto want = movedOf.find(T.uid);
                if (want == movedOf.end() || want->second.empty()) continue;
                const std::vector<int> wantRigid = rigidSig(T.uid);
                int best = -1, nSameMoves = 0;
                double bestD = 1e18;
                for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                    const int u = L.dyn.objs[i].uid;
                    const auto have = movedOf.find(u);
                    if (have == movedOf.end() || have->second != want->second) continue;
                    ++nSameMoves;
                    if (bentUid.count(T.uid) || bentUid.count(u) || rigidSig(u) != wantRigid)
                        continue;
                    const double d = std::fabs(L.dyn.objs[i].cx - T.cx)
                                     + std::fabs(L.dyn.objs[i].cy - T.cy);
                    if (d < bestD) { bestD = d; best = (int)i; }
                }
                if (best < 0) {
                    if (nSameMoves) {
                        ++nRefused;
                        std::printf("touchfollow: box uid %d NOT followed: %s\n", T.uid,
                                    bentUid.count(T.uid)
                                        ? "a Rotate/Scale/Area/Advanced Follow moves its "
                                          "group, by an amount that depends on position"
                                        : "no recorded object shares all its transforms");
                    }
                    continue;
                }
                // load-time position: seek() has not run yet
                T.followIdx = best;
                T.followX0 = L.dyn.objs[(size_t)best].cx;
                T.followY0 = L.dyn.objs[(size_t)best].cy;
                ++nFollow;
                std::printf("touchfollow: box uid %d follows dynamic uid %d (%zu moved "
                            "group(s), %.0f px away)\n", T.uid,
                            L.dyn.objs[(size_t)best].uid, want->second.size(), bestD);
            }
            if (nFollow || nRefused)
                std::printf("touchfollow: %d box(es) follow, %d refused\n", nFollow, nRefused);
            if (nFollow) g_touchFollowObjs = &L.dyn.objs;
        }
    }
    // --boxmoves (speed.hpp): the touch boxes (and the touch gravity triggers) of a group a plain Move displaces are where the Move left them, and a touch Toggle that switches a touch gravity trigger's
    // group off switches it off.
    if ((!g_touch.empty() || !g_gravTouchBoxes.empty()) && !trigPath.empty() && !grpPath.empty()) {
        const auto* rowsP2 = trigRowsCached(trigPath);
        const auto* mem2 = groupMembersCached(grpPath);
        if (rowsP2 && mem2) {
            struct MoveRec { double cx, ox, oy, dur; };
            std::unordered_map<int, std::vector<MoveRec>> byGroup;   // group -> the autonomous Moves that displace it
            std::unordered_set<int> bentG;
            for (const auto& kv : *rowsP2) {
                const TrigRow& r = kv.second;
                if (r.target <= 0) continue;
                if (r.id == 901 && !r.touch && !r.spawn && !r.lockx && !r.locky && r.mvtgt == 0 && !r.lockcamx && !r.lockcamy
                    && (r.ox != 0.0 || r.oy != 0.0))
                    byGroup[r.target].push_back({r.cx, r.ox, r.oy, r.dur});
                if (r.id == 1346 || r.id == 2067 || (r.id >= 3006 && r.id <= 3008) || (r.id >= 3011 && r.id <= 3013) || r.id == 3016
                    || r.id == 901 || r.id == 1347 || r.id == 1814) {
                    // rigid translations are fine; only the position-dependent transforms bend a group
                    if (r.id != 901 && r.id != 1347 && r.id != 1814) bentG.insert(r.target);
                }
            }
            std::unordered_set<int> want;
            for (const TouchTrig& T : g_touch) want.insert(T.uid);
            for (const GravTouchBox& b : g_gravTouchBoxes) want.insert(b.uid);
            std::unordered_map<int, std::vector<int>> groupsOfUid;
            for (const auto& gm : *mem2)
                for (const int u : gm.second)
                    if (want.count(u)) groupsOfUid[u].push_back(gm.first);
            const auto shiftOf = [&](int uid, double boxCx, double& dx, double& dy) {
                dx = dy = 0.0;
                const auto gi = groupsOfUid.find(uid);
                if (gi == groupsOfUid.end()) return false;
                for (const int g : gi->second)
                    if (bentG.count(g)) { dx = dy = 0.0; return false; }
                bool any = false;
                for (const int g : gi->second) {
                    const auto mi = byGroup.find(g);
                    if (mi == byGroup.end()) continue;
                    for (const MoveRec& m : mi->second)
                        if (m.cx + 1300.0 * std::max(0.0, m.dur) < boxCx) { dx += m.ox; dy += m.oy; any = true; }
                }
                return any;
            };
            int nMoved = 0;
            // Only the touch Toggles that switch a touch gravity trigger's group off are displaced: every other touch box is left where it was dumped, because the objects such a box moves
            // are placed from the recording of the run that entered it (or did not), and a box that now fires in the model against a recording that holds it unfired replaces placements GD
            // had right (measured on the level above: 12 episodes that agreed with GD lost it when every displaced box was moved).
            std::unordered_set<size_t> providers;
            for (const GravTouchBox& b : g_gravTouchBoxes) {
                if (b.uid < 0) continue;
                for (size_t i = 0; i < g_touch.size(); ++i) {
                    const TouchTrig& T = g_touch[i];
                    if (T.id != 1049 || T.togOn != 0) continue;
                    const auto rt = rowsP2->find(T.uid);
                    if (rt == rowsP2->end()) continue;
                    const auto gm = mem2->find(rt->second.target);
                    if (gm != mem2->end() && std::find(gm->second.begin(), gm->second.end(), b.uid) != gm->second.end())
                        providers.insert(i);
                }
            }
            for (size_t i = 0; i < g_touch.size(); ++i) {
                TouchTrig& T = g_touch[i];
                if (T.followIdx >= 0 || !providers.count(i)) continue;
                double dx, dy;
                if (shiftOf(T.uid, T.cx, dx, dy)) {
                    std::printf("boxmoves: touch box uid %d (%.0f,%.0f) -> (%.0f,%.0f)\n", T.uid, T.cx, T.cy, T.cx + dx, T.cy + dy);
                    T.cx += dx; T.cy += dy;
                    ++nMoved;
                }
            }
            for (GravTouchBox& b : g_gravTouchBoxes) {
                double dx, dy;
                if (b.uid >= 0 && shiftOf(b.uid, b.cx, dx, dy)) {
                    std::printf("boxmoves: gravity trigger uid %d (%.0f,%.0f) -> (%.0f,%.0f)\n", b.uid, b.cx, b.cy, b.cx + dx, b.cy + dy);
                    b.cx += dx; b.cy += dy;
                    ++nMoved;
                }
                // a touch Toggle (togon 0) whose target group holds this trigger switches it off
                b.offBits.clear();
                if (b.uid < 0) continue;
                for (size_t i = 0; i < g_touch.size(); ++i) {
                    const TouchTrig& T = g_touch[i];
                    if (T.id != 1049 || T.togOn != 0) continue;
                    const auto rt = rowsP2->find(T.uid);
                    if (rt == rowsP2->end()) continue;
                    const auto gm = mem2->find(rt->second.target);
                    if (gm == mem2->end()) continue;
                    if (std::find(gm->second.begin(), gm->second.end(), b.uid) != gm->second.end()) {
                        b.offBits.push_back((int)i);
                        std::printf("boxmoves: gravity trigger uid %d is switched off by touch Toggle uid %d (box %zu)\n", b.uid, T.uid, i);
                    }
                }
            }
            if (nMoved) std::printf("boxmoves: %d box(es) displaced by their groups' Moves\n", nMoved);
        }
    }
    // --boxarm (constants.hpp): a toggle block that switches Moves back on is tested where the recording has it,
    // turned (TouchTrig::recBox). A custom level's corridor blocks ride up 30 px and spin with their groups, and
    // GD fired them exactly on the ticks a body's square met the turned box at the recorded centre -- the second
    // body's 2-tick press at the edge of one did, a 6-tick press 10 px off a turned corner did not.
    {
        size_t nRec = 0;
        for (TouchTrig& T : g_touch) {
            T.recBox = nullptr;
            if (!T.press || T.offSwitch) continue;
            bool arms = false;
            for (const TouchTrig::TogEv& e : T.trigTog) arms = arms || e.on;
            if (!arms) continue;
            const auto it = gt.find(T.uid);
            if (it == gt.end() || it->second.size() < 2) continue;
            T.recBox = &it->second;
            ++nRec;
        }
        std::printf("boxarm: %zu toggle block(s) tested where the recording has them\n", nRec);
    }
    // --touchobb (constants.hpp): a turned touch box is met as GD meets it, by its oriented box
    {
        size_t nObb = 0;
        for (TouchTrig& T : g_touch) {
            T.obb = false;
            if (!g_touchObb) continue;
            const auto it = g_obb.find(T.uid);
            if (it == g_obb.end()) continue;
            T.obb = true;
            T.obbHw = it->second.hw;
            T.obbHh = it->second.hh;
            T.obbRot = -std::atan2(it->second.s, it->second.c) * 180.0 / 3.14159265358979;
            ++nObb;
        }
        if (nObb) std::printf("touchobb: %zu touch box(es) met by their turned box\n", nObb);
    }
    // --followy: the Follow Player Y triggers (modifiers.hpp, FollowY) and their groups' objects,
    // recorded or not (a first iteration has no recording, and the group sits in the static
    // windows then). Each object's load-time y is where it closes on the player from.
    g_followYs.clear();
    g_followSecObjs.clear();
    if (!trigPath.empty() && !grpPath.empty()) {
        const auto* rowsP = trigRowsCached(trigPath);   // (parse caches, as the touchfollow block above)
        if (rowsP) {
            const std::unordered_map<int, TrigRow>& rows = *rowsP;
            std::vector<const TrigRow*> fy;
            for (const auto& kv : rows)
                if (kv.second.id == 1814 && kv.second.target > 0) fy.push_back(&kv.second);
            std::sort(fy.begin(), fy.end(),
                      [](const TrigRow* a, const TrigRow* b) { return a->cx < b->cx; });
            // Without a Follow Player Y trigger nothing below reads the group members or the objects'
            // load-time y (the loop over `fy` is empty), so neither is built: a level has none far more
            // often than not, and the y table is one entry per object.
            static const std::unordered_map<int, std::vector<int>> noMembers;
            const auto* memP = fy.empty() ? nullptr : groupMembersCached(grpPath);
            const std::unordered_map<int, std::vector<int>>& members = memP ? *memP : noMembers;   // group -> uids
            std::unordered_map<int, double> y0Of;   // uid -> load-time y, from every bucket
            if (!fy.empty())
                for (const std::vector<Obj>* v : {&L.objs, &L.portals, &L.pads, &L.orbs, &L.speeds,
                                                  &L.slopes, &L.dyn.objs})
                    for (const Obj& o : *v) y0Of.emplace(o.uid, (double)o.cy);
            // the fastest x advance the level can have: where a run is over by, at the latest
            double dxMax = (double)kDxF;
            for (const Obj& o : L.speeds) dxMax = std::max(dxMax, (double)dxForSpeedId(o.id));
            if (g_startSpeedMul > 0.0)
                dxMax = std::max(dxMax, (double)dxForSpeedMul(g_startSpeedMul));
            double slotFree[kFollowYSlots];
            for (double& v : slotFree) v = -1e300;
            for (const TrigRow* r : fy) {
                const char* why = nullptr;
                int slot = -1;
                for (int sl = 0; sl < kFollowYSlots; ++sl)
                    if (slotFree[sl] < r->cx && (slot < 0 || slotFree[sl] < slotFree[slot]))
                        slot = sl;
                if (!r->fyCols) why = "from a dump without the follow columns";
                else if (r->spawn || (r->touch && !g_followTouch)) why = "touch- or spawn-triggered";
                else if (r->fyDelay > 0.0) why = "delayed (a ring of past y a state cannot carry)";
                else if (r->fySpeed <= 0.0) why = "at speed 0";
                else if (slot < 0) why = "one more than a state's slots while the others run";
                FollowY f;
                f.slot = slot;
                f.endX = r->cx + r->dur * 240.0 * dxMax;
                f.cx = r->cx;
                if (r->touch) {   // --followtouch: owner from the box's left edge less the player's reach
                    f.touch = true;
                    f.tcx = r->cx; f.tcy = r->cy;
                    f.thw = r->w > 0.0 ? 0.5 * r->w : 15.0; f.thh = r->h > 0.0 ? 0.5 * r->h : 15.0;
                    f.cx = r->cx - f.thw - 30.0;
                }
                f.uid = r->uid;
                f.group = r->target;
                f.k = std::min(std::max(0.25 * r->fySpeed, 0.0), 1.0);
                f.offset = (double)r->fyOffset;
                f.maxStep = r->fyMax > 0.0 ? 0.25 * r->fyMax : 0.0;
                f.durTicks = r->dur * 240.0;
                f.refUid = -1;
                const auto mit = members.find(r->target);
                if (!why && mit != members.end())
                    for (int u : mit->second) {
                        const auto yit = y0Of.find(u);
                        if (yit == y0Of.end()) continue;   // a decoration or a trigger
                        if (f.refUid < 0) {
                            f.refUid = u;
                            f.y0ref = yit->second;
                        }
                        f.objs.push_back({u, yit->second});
                    }
                if (!why && f.objs.empty()) why = "its group holds nothing the model collides with";
                if (why) {
                    std::printf("followy: uid %d at x=%.0f (group %d) NOT modelled: %s\n",
                                r->uid, r->cx, r->target, why);
                    // --followsection: not placed, but bucketed all the same (modifiers.hpp)
                    if (mit != members.end()) {
                        size_t n0 = g_followSecObjs.size();
                        for (int u : mit->second) {
                            const auto yit = y0Of.find(u);
                            if (yit != y0Of.end()) g_followSecObjs.push_back({u, yit->second});
                        }
                        std::printf("followsection: group %d's %zu object(s) are bucketed at their "
                                    "load-time cells\n", r->target, g_followSecObjs.size() - n0);
                    }
                    continue;
                }
                std::printf("followy: uid %d at x=%.0f slot %d: group %d (%zu object(s), ref uid "
                            "%d at y=%.1f), k %.4f, offset %+.0f, max step %.3f, %.0f ticks "
                            "(over by x=%.0f)\n",
                            r->uid, r->cx, slot, f.group, f.objs.size(), f.refUid, f.y0ref, f.k,
                            f.offset, f.maxStep, f.durTicks, f.endX);
                slotFree[slot] = f.endX;
                g_followYs.push_back(std::move(f));
            }
        }
    }
    // --bigsection (constants.hpp): the static colliders that can reach the player from a section cell
    // the collision pass does not walk. A followed object is the follow pass's business already.
    g_bigSecObjs.clear();
    g_bigSecDyn.clear();
    {
        std::unordered_set<int> followed;
        for (const FollowY& f : g_followYs)
            for (const auto& m : f.objs) followed.insert(m.first);
        for (const auto& m : g_followSecObjs) followed.insert(m.first);
        // An object with Extended Collision (Obj::extCol) is not filed in a cell at all: addToSection puts
        // it in the always-checked list, which checkCollisions tests after the 3x3 cells every tick.
        for (const std::vector<Obj>* v : {&L.objs, &L.portals, &L.pads, &L.orbs, &L.speeds, &L.slopes})
            for (const Obj& o : *v)
                if ((o.hw > kBigSecHalf || o.hh > kBigSecHalf) && !o.extCol && !followed.count(o.uid))
                    g_bigSecObjs.push_back({o.uid, (double)o.cy});
        // ...and the grouped ones, which load into Dynamics whether or not anything moves them: the
        // game files them the same way and re-files one where a Move takes it, so their cell is read
        // from where the windows have them (step.hpp). A circle's reach is its radius.
        for (const Obj& o : L.dyn.objs)
            if ((o.hw > kBigSecHalf || o.hh > kBigSecHalf || o.radius > kBigSecHalf) && !o.extCol
                && !followed.count(o.uid))
                g_bigSecDyn.push_back({o.uid, (double)o.cy});
        std::printf("bigsection: %zu static and %zu grouped collider(s) are met only from their own "
                    "section cell:", g_bigSecObjs.size(), g_bigSecDyn.size());
        for (size_t i = 0; i < g_bigSecObjs.size() && i < 24; ++i)
            std::printf(" %d", g_bigSecObjs[i].first);
        if (!g_bigSecDyn.empty()) std::printf(" | grouped:");
        for (size_t i = 0; i < g_bigSecDyn.size() && i < 24; ++i)
            std::printf(" %d", g_bigSecDyn[i].first);
        std::printf("\n");
    }
    // ...and the starting state's values. A trigger at or behind the start fires on the first
    // tick, so a walk from the level's start begins with it primed (moves from the next tick).
    // An anchor past a trigger takes its reference object's recorded y at the anchor tick: the
    // recording is this lineage's replay, so that is where GD had it. Without a recorded row
    // the value stays unknown and the group stays where the recording puts it.
    if (!g_followYs.empty()) {
        if (t0 > 0) L.dyn.seek((int)t0);
        for (int sl = 0; sl < kFollowYSlots; ++sl) {
            const int own = followOwner(sl, x0World);   // ahead of x0: primed on its crossing
            if (own < 0) continue;
            FollowY& f = g_followYs[(size_t)own];
            const uint8_t bit = (uint8_t)(1u << sl);
            if (t0 <= 0) {
                f.tAct = t0;
                init.followF[sl] = (float)f.y0ref;
                init.followOk = (uint8_t)(init.followOk | bit);
                continue;
            }
            for (const Obj& o : L.dyn.objs)
                if (o.uid == f.refUid) {
                    init.followF[sl] = (float)o.cy;
                    init.followOk = (uint8_t)(init.followOk | bit);
                    break;
                }
            // --followtouch: a touch trigger the anchor is past counts from its box (the recording's run touched it)
            if (f.touch) {
                if (init.followOk & bit) {
                    const double dxA = g_startSpeedMul > 0.0 ? (double)dxForSpeedMul(g_startSpeedMul) : (double)kDxF;
                    init.followOwn[sl] = (uint8_t)(own + 1);
                    init.followN[sl] = (uint16_t)std::min(65535.0, std::max(0.0, std::floor((x0World - f.tcx) / dxA)));
                }
                continue;
            }
            // A walk that begins past the crossing never sees it: its tick is put back from the
            // distance at the anchor's speed (GD's own multiplier when the caller passed it), so
            // a run that is over by the anchor stays over.
            if (f.tAct < 0) {
                const double dxA = g_startSpeedMul > 0.0 ? (double)dxForSpeedMul(g_startSpeedMul)
                                                         : (double)kDxF;
                // the crossing tick is the first whose x reaches cx, so the distance past it
                // holds a whole number of ticks plus less than one
                f.tAct = t0 - (long long)std::floor((x0World - f.cx) / dxA);
            }
            std::printf("followy: uid %d (slot %d) at the anchor t=%lld: %s, crossed at t=%lld\n",
                        f.uid, sl, t0,
                        (init.followOk & bit) ? "seeded from the recording" : "no recorded row - "
                        "the group stays where the recording puts it", f.tAct);
        }
    }
    // Beside the dump when nobody said, the way levelsettings and rotgameplay are
    // (forceIdsPathBeside; it returns empty unless the file is really there).
    if (g_forceIdsPath.empty()) g_forceIdsPath = forceIdsPathBeside(argv[1]);
    if (!g_forceIdsPath.empty()) applyForceIds(g_forceIdsPath);
    // ...and build every frame the level can turn into, NOW. frameLevel() is a
    // lazy cache, and applyRotation's re-tap needs the NEW frame's geometry on
    // the tick it turns -- which for the first rotation is before anything else
    // has asked for it. Building it there would also be a data race: the search
    // calls applyRotation from the worker threads.
    for (const RotTrig& r : g_rotTrig) (void)frameLevel(L, r.frame);
    // --spentrot: burn the 2900s already fired before the anchor (history at
    // the declaration). firedT=0 means "fired at t=0" = skipped on every later
    // tick via shot != t. revT is left alone: all the driver can derive is the
    // gframe transitions = frame-changing firings; the consumption of a
    // reverse toggle does not show in the dump.
    if (!g_spentRot.empty()) {
        int hit = 0;
        for (int su : g_spentRot)
            for (RotTrig& r : g_rotTrig)
                if (r.uid == su && r.firedT < 0) { r.firedT = 0; ++hit; }
        std::printf("spentrot: %d/%zu triggers pre-fired\n",
                    hit, g_spentRot.size());
    }
    // Resolve the anchor's stair-snap object now that the level exists. Pointers
    // into L.objs are what the snap block compares against (`c.snapObj != o`),
    // so it has to be the SAME object, not a copy: only the identity matters for
    // the gate, and the pattern is measured from its cx/cy.
    // A MOVING solid lives in L.dyn.objs, not L.objs, and the player can be
    // standing on one -- both stores are stable across ticks by construction
    // (see the note on Dynamics::objs), which is what makes the raw pointer
    // legal here in the first place.
    if (startSnapUid >= 0) {
        for (const Obj& o : L.objs)
            if (o.uid == (int)startSnapUid) { init.snapObj = &o; break; }
        if (!init.snapObj)
            for (const Obj& o : L.dyn.objs)
                if (o.uid == (int)startSnapUid) { init.snapObj = &o; break; }
        if (init.snapObj) init.snapDist = (float)startSnapDist;
        else std::printf("start: snap uid %lld not found in the level\n",
                         startSnapUid);
    }
    // [2026-08-19] The anchor cannot carry CONSUMED PADS either (same family
    // of hole as rHover/dash/snapObj). A pad is consumed on contact (note in
    // stepOne's pad loop) and does not re-fire while the contact lasts. If the
    // player's box at t0 overlaps a pad's firing box, that contact began before
    // t0 and GD has already fired it -- start with it consumed.
    // Measured on lv17 t=14,601: GD had fired the yellow pad uid7254
    // (18,943,152) at t=14,577, yet the section anchor at t0=14,600 re-fired
    // it for vy=16 (census m4/orbnear/in1, edvy +10.807).
    // Orbs are CONSUMED ONLY BY A PRESS, so they cannot be seeded from
    // position alone (deferred). Anchors in a rotated frame are deferred too,
    // the coordinate system being different.
    // y overlap is checked as well: an x-only overlap (ducking past the side)
    // is unfired in GD too, so it is not seeded -- seeding there would err on
    // the side of killing a real firing.
    if (t0 > 0 && startFrame == 0) {
        const double sHalf = playerHalf(init.mode, init.mini != 0);
        int slot = 0;
        for (const Obj& o : L.pads) {
            if (slot >= 4) break;
            if (std::fabs(x0 - o.cx) >= o.hw + sHalf + kPadReach) continue;
            if (std::fabs((double)init.y - o.cy) >= o.hh + sHalf) continue;
            // The same shape the pad loop uses, or the seeder and the loop
            // would disagree about what "already in contact" means for a
            // rotated board -- see the padobb / padplayerrot note at the loop's
            // site in step.hpp. A type-8 pad's activation is judged with the
            // player's square TURNED BY ITS OWN ROTATION, so the anchor asks the
            // same question here, under the same two arms. (No one-step
            // advance of the angle. That used to be a DIFFERENCE from the loop,
            // justified by `init.rot` being a whole seeded state rather than the
            // mid-tick value the loop reconstructed from `s.rot`; since the
            // advance was deleted at the loop's site the two ask the same thing,
            // and `--no-satrotraw` reintroduces the difference along with it.)
            double sRotPad = (double)init.rot;
            if (o.type == 8) {
                if (g_noPadPlayerRot) sRotPad = 0.0;
                else if (!padPlayerRotMode(init.mode)) sRotPad = 0.0;
            }
            if (o.oriented
                && !orientedHit(o, x0, (double)init.y, sHalf, sRotPad))
                continue;
            init.usedPad[slot++] = &o;
        }
        // ...AND THE PADS THE RUN FIRED EARLIER AND STEPPED OFF (--spentpad,
        // history at g_spentPad's declaration). The block above only ever
        // catches a contact still in progress at t0, because it asks a question
        // about the CURRENT position. GD's latch is permanent for the attempt,
        // so a pad fired thirty ticks ago is just as dead -- and lv18's
        // x~25,000 zig-zag walks straight back into six of them.
        //
        // The two are kept apart rather than merged. The overlap rule needs no
        // caller and works for anyone who passes only --start -- the repair
        // loop's own anchors, an MCP probe, any hand-built section -- while the
        // list is the recording talking, and is only there when someone has
        // read a recording. Its one witness, lv17 t=14,601, is covered by both.
        //
        // THE X WINDOW IS DECIDED HERE and not by the producer, because it is
        // this side's rule: `usedPad` is released on x alone (the release loop
        // at the end of stepOne's pad section) and there are four slots for a
        // level with up to 82 pads, so "which of the fired pads can still
        // matter" has to be answered with the same expression the release uses.
        //
        // A PAD CARRIED BY MOVING GEOMETRY IS NOT SEEDED, and is counted
        // separately rather than folded into "unknown", which has to keep its
        // own meaning (a stale dump, the wrong level). Such a pad lives in
        // L.dyn.objs, where `cx` is its position at load time and not at t0 --
        // the x window below would be measured against the wrong place, and
        // the producer's own contact test has the same problem. Measured over
        // quick_regress's 1,116 sections: 406 named uids land here, all of them
        // on lv19/20/21/22, which are exactly the levels with grouped pads
        // (16/15/2/2 of them). lv1-18 name none.
        if (g_spentPadSeed && !g_spentPad.empty()) {
            for (int su : g_spentPad) {   // a pad carried by moving geometry as well
                for (Obj& o : L.pads)
                    if (o.uid == su && !o.multiAct) o.spentRun = 1;
                for (Obj& o : L.dyn.objs)
                    if (o.uid == su && !o.multiAct) o.spentRun = 1;
            }
            int seeded = 0, gone = 0, unknown = 0, overflow = 0, carried = 0;
            for (int su : g_spentPad) {
                const Obj* pad = nullptr;
                for (const Obj& o : L.pads)
                    if (o.uid == su) { pad = &o; break; }
                if (!pad) {
                    bool moving = false;
                    for (const Obj& o : L.dyn.objs)
                        if (o.uid == su) { moving = true; break; }
                    (moving ? carried : unknown) += 1;
                    continue;
                }
                if (std::fabs(x0 - pad->cx) >= pad->hw + sHalf + kPadReach) {
                    ++gone;      // released before t0 anyway -- see the release
                    continue;
                }
                bool have = false;
                for (int i = 0; i < slot; ++i)
                    if (init.usedPad[i] == pad) { have = true; break; }
                if (have) continue;             // the overlap rule got it first
                if (slot >= 4) { ++overflow; continue; }
                init.usedPad[slot++] = pad;
                ++seeded;
            }
            // Reported, not silent: "nothing happened" is this block's failure
            // mode, and a caller whose uids name no pad of this level (a stale
            // dump, the wrong level) would otherwise look exactly like a caller
            // with nothing to seed.
            std::printf("spentpad: %d/%zu pads pre-fired (%d already outside "
                        "the x window, %d carried by moving geometry, %d not a "
                        "pad in this level)\n",
                        seeded, g_spentPad.size(), gone, carried, unknown);
            if (overflow)
                std::printf("spentpad: %d DROPPED - all four usedPad slots are "
                            "taken at t0\n", overflow);
        }
        // --spentorb (speed.hpp): the rings the attempt fired before t0, replayed through
        // noteRingFired oldest first, so usedOrb / usedOrbOld (and the --ringonce history) hold
        // the most recent ones as they would have had the run started at the head.
        // --ringpresseat (speed.hpp): ...and a toggle ring in either list, which neither L.orbs nor dyn holds, goes
        // in by uid in its place in the order, as the step files it (notePressRingFired); a Multi Activate one never
        auto pressRingSpent = [](int su) {
            if (!g_ringPressEat) return false;
            for (const PressRing& r : g_pressRings)
                if (r.uid == su) return !r.multiAct;
            return false;
        };
        if (!g_spentOrb.empty()) {
            int seeded = 0, unknown = 0, carried = 0, pressed = 0;
            for (int su : g_spentOrb) {
                const Obj* ring = nullptr;
                for (const Obj& o : L.orbs)
                    if (o.uid == su) { ring = &o; break; }
                // --spentdyn: ...and a ring in dyn (a grouped ring is routed there and L.orbs
                // never holds it). Unlike a pad it needs no position: a ring is spent by uid
                // (usedOrbOld / the history), and usedOrb points at the same L.dyn.objs entry
                // the step collects. A custom level anchored at t0=16,064 named 57 rings GD had
                // fired and 34 of them were dropped as "not a ring"; the pink ring uid 8532, the
                // last GD fired, stayed live and the model's UFO press fired it a second time at
                // t=16,085 where GD's gave the plain jump.
                if (!ring)
                    for (size_t i = 0; i < L.dyn.objs.size(); ++i)
                        if (L.dyn.objs[i].uid == su && L.dyn.bucket[i] == Dynamics::ORB) {
                            ring = &L.dyn.objs[i];
                            ++carried;
                            break;
                        }
                if (!ring && pressRingSpent(su)) {
                    notePressRingFired(init, su);
                    if (g_spentGlobal) markSpentGlobal(g_spentMap1, su);   // --spentglobal (speed.hpp)
                    ++pressed;
                    continue;
                }
                if (!ring) { ++unknown; continue; }
                noteRingFired(init, ring);
                if (g_spentGlobal && !ring->multiAct) markSpentGlobal(g_spentMap1, su);   // --spentglobal (speed.hpp)
                ++seeded;
            }
            std::printf("spentorb: %d/%zu rings pre-fired (%d not a ring in this level)\n",
                        seeded, g_spentOrb.size(), unknown);
            std::printf("spentorb: %d of them carried by moving geometry\n", carried);
            if (pressed) std::printf("spentorb: %d toggle rings pre-fired (--ringpresseat)\n", pressed);
        }
        // --spentp2 (speed.hpp): ...and the second body's, into its own ring memory -- the halves the step
        // trades for that body's turn (swapHalves), so noteRingFired files them as its step would.
        if (!g_spentOrb2.empty()) {
            int seeded2 = 0, unknown2 = 0, pressed2 = 0;
            auto swapRings = [](State& s) {
                std::swap(s.usedOrb, s.usedOrb2);
                std::swap(s.usedOrbOld, s.usedOrbOld2);
                for (int i = 0; i < 3; ++i) std::swap(s.usedOrbHist[i], s.usedOrbHist2[i]);
            };
            for (int su : g_spentOrb2) {
                const Obj* ring = nullptr;
                for (const Obj& o : L.orbs)
                    if (o.uid == su) { ring = &o; break; }
                if (!ring)
                    for (size_t i = 0; i < L.dyn.objs.size(); ++i)
                        if (L.dyn.objs[i].uid == su && L.dyn.bucket[i] == Dynamics::ORB) {
                            ring = &L.dyn.objs[i];
                            break;
                        }
                if (!ring && pressRingSpent(su)) {
                    swapRings(init);
                    notePressRingFired(init, su);
                    swapRings(init);
                    if (g_spentGlobal) markSpentGlobal(g_spentMap2, su);   // --spentglobal (speed.hpp)
                    ++pressed2;
                    continue;
                }
                if (!ring) { ++unknown2; continue; }
                swapRings(init);
                noteRingFired(init, ring);
                swapRings(init);
                if (g_spentGlobal && !ring->multiAct) markSpentGlobal(g_spentMap2, su);   // --spentglobal (speed.hpp)
                ++seeded2;
            }
            std::printf("spentorb2: %d/%zu of the second body's rings pre-fired (%d not a ring in this level)\n",
                        seeded2, g_spentOrb2.size(), unknown2);
            if (pressed2) std::printf("spentorb2: %d toggle rings pre-fired (--ringpresseat)\n", pressed2);
        }
    }
    // A re-anchored solve starts mid-level, where the doors the earlier part of
    // the run already opened ARE open. That is not an assumption to make -- it
    // is in the recording, so read it from there: a controlled object that has
    // already travelled most of its offset by t0 means its trigger fired.
    // Without this the anchor is dropped inside a door GD has open and the
    // frontier dies on tick 0 (measured on lv19 --start 19900, x=27,721, which
    // sits exactly where gate uid 13395 was before it slid away).
    // ...unless GD told us directly. The recording can only answer for objects
    // it contains, which is why reading it left 163 differences on lv22; a
    // payload names the triggers GD saw fire and their ticks, so nothing has to
    // be inferred. It takes precedence over the block below and reports what it
    // set, including the boxes it named that this build has no bit for -- a
    // payload written against a wider window than this one's 32.
    std::vector<std::pair<int, int>> payloadTouch;
    std::vector<int> payloadPortal, payloadPortal2;
    std::vector<std::pair<std::string, int>> payloadHist;
    int histVersion = 1;
    bool havePayload = false;
    if (!g_anchorState.empty()) {
        std::vector<std::pair<std::string, std::string>> kv;
        std::string unknown;
        if (!parseAnchorState(g_anchorState, kv, unknown)) {
            std::printf("seed payload rejected: unknown key '%s'\n"
                        "  this exe knows:", unknown.c_str());
            for (const char* kk : kAnchorKeys) std::printf(" %s", kk);
            std::printf("\n  a payload naming something else was written by a "
                        "build that carries what this one would drop\n");
            return 2;
        }
        bool sawTouchKey = false, sawPortalKey = false, sawPortal2Key = false;
        bool sawHistKey = false;
        for (const auto& p : kv) {
            if (p.first == "owns") {
                if (p.second.find("touch") != std::string::npos)
                    g_ownsTouch = true;
                if (p.second.find("portal") != std::string::npos)
                    g_ownsPortal = true;
                if (p.second.find("hist") != std::string::npos)
                    g_ownsHist = true;
            } else if (p.first == "hist") {
                // version|count|name:value,... -- see kHistNames.
                const std::string& v = p.second;
                const size_t b1 = v.find('|');
                const size_t b2 = (b1 == std::string::npos) ? b1
                                                            : v.find('|', b1 + 1);
                if (b2 == std::string::npos
                    || (v.substr(0, b1) != "1" && v.substr(0, b1) != "2"
                        && v.substr(0, b1) != "3" && v.substr(0, b1) != "4"
                        && v.substr(0, b1) != "5" && v.substr(0, b1) != "6")) {
                    std::printf("seed payload rejected: hist '%s' is not version "
                                "1 to 6 (version|count|name:value,...)\n", v.c_str());
                    return 2;
                }
                histVersion = std::atoi(v.substr(0, b1).c_str());
                const int want = std::atoi(v.substr(b1 + 1, b2 - b1 - 1).c_str());
                int got = 0;
                size_t i = b2 + 1;
                while (i < v.size()) {
                    size_t c = v.find(',', i);
                    if (c == std::string::npos) c = v.size();
                    const std::string nv = v.substr(i, c - i);
                    i = c + 1;
                    if (nv.empty()) continue;
                    const size_t colon = nv.find(':');
                    const std::string name = nv.substr(0, colon);
                    bool known = false;
                    for (const char* hn : histNamesFor(histVersion))
                        if (hn && name == hn) known = true;
                    if (colon == std::string::npos || !known) {
                        std::printf("seed payload rejected: hist name '%s' is "
                                    "not one version %d knows:", name.c_str(),
                                    histVersion);
                        for (const char* hn : histNamesFor(histVersion))
                            if (hn) std::printf(" %s", hn);
                        std::printf("\n");
                        return 2;
                    }
                    payloadHist.emplace_back(name,
                                             std::atoi(nv.substr(colon + 1).c_str()));
                    ++got;
                }
                if (got != want) {
                    std::printf("seed payload rejected: hist says %d values and "
                                "carries %d\n", want, got);
                    return 2;
                }
                sawHistKey = true;
            } else if (p.first == "touch") {
                payloadTouch = parseTouchPayload(p.second);
                sawTouchKey = true;
            } else if (p.first == "portal") {
                payloadPortal = parseUidList(p.second);
                sawPortalKey = true;
            } else if (p.first == "portal2") {
                payloadPortal2 = parseUidList(p.second);
                sawPortal2Key = true;
            }
        }
        havePayload = g_ownsTouch;
        // A PAYLOAD REPLACES THE RECORDING-DERIVED SEED WHOLESALE, so a key it
        // does not carry is a value nobody sets -- not a value that falls back.
        // Found by this build's own first test: a payload with `touch` and no
        // `lockOff` seeded the fire ticks perfectly and left the platform's ride
        // at zero, and the replay died 45 ticks later. That is the exact silent
        // degradation this refusal exists for, so it is not optional.
        // Refusal is per DECLARED subsystem: a payload that claims `touch` and
        // carries no `touch` key is incomplete, while one that never claimed
        // the lock is not missing anything -- the lock simply keeps its own
        // seeding.
        std::string missing;
        if (g_ownsTouch && !sawTouchKey) missing = "touch";
        // Both halves, because the mask is per player: a payload that claims
        // the subsystem and names only p1 has left p2 to a default, which is
        // the shape of degradation this refusal exists for.
        if (g_ownsPortal && !sawPortalKey)
            missing += (missing.empty() ? "" : " ") + std::string("portal");
        if (g_ownsPortal && !sawPortal2Key)
            missing += (missing.empty() ? "" : " ") + std::string("portal2");
        // ...and a claimed hist must name every value version 1 knows: an
        // omitted one would be left at its default by nobody's decision.
        if (g_ownsHist) {
            if (!sawHistKey)
                missing += (missing.empty() ? "" : " ") + std::string("hist");
            for (const char* hn : histNamesFor(histVersion)) {
                if (!hn) continue;
                bool have = false;
                for (const auto& h : payloadHist) if (h.first == hn) have = true;
                if (sawHistKey && !have)
                    missing += (missing.empty() ? "" : " ") + std::string(hn);
            }
        }
        if (!missing.empty()) {
            if (!g_seedPartialOk) {
                // Say WHY this is fatal rather than a warning: the payload
                // replaces the recording-derived seed wholesale, so a key it
                // omits is not filled from the recording -- it is filled by
                // nobody. Without this line the next reader assumes the old
                // path still covers the gap, which is what the first test of
                // this code did: three fire ticks perfect, the ride at zero,
                // dead 45 ticks later.
                std::printf("seed payload incomplete: missing %s\n"
                            "  a payload REPLACES the recording-derived seed, "
                            "so an omitted key is set by nobody\n"
                            "  this exe wants:", missing.c_str());
                for (const char* kk : kAnchorKeys) std::printf(" %s", kk);
                std::printf("\n  payload carried:");
                for (const auto& p : kv) std::printf(" %s", p.first.c_str());
                std::printf("\n  (the shorter list is the older build; pass "
                            "--seed-partial-ok to run anyway)\n");
                return 2;
            }
            // Allowed -- but the result has to carry it, because a line on
            // stderr does not survive into the place results are compared.
            g_seedPartial = missing;
        }
    }
    if (t0 > 0 && havePayload) {
        int set = 0, unmapped = 0;
        for (const auto& ut : payloadTouch) {
            int bit = -1;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
                if (g_touch[b].uid == ut.first) { bit = (int)b; break; }
            if (bit < 0) { ++unmapped; continue; }
            init.trig |= touchBit(bit);
            init.fireB[bit] = (uint16_t)std::max(0, ut.second);
            ++set;
        }
        std::printf("seed payload: %d boxes set, %d named but not in this "
                    "build's window\n", set, unmapped);
        // Provenance, in the run's own output. Which side set a value is the
        // first question when two runs disagree, and stderr does not survive
        // into the place results are compared.
        std::printf("seed: touch=payload lock=recording\n");
        // The one diagnosis worth pre-writing, because the convention breaks
        // in exactly one shape: values are the state at t0, and a payload
        // written at t0+1 lands one tick of motion further on. If the first
        // compared tick is out by about one dx (1.615 px on lv19), that is
        // what happened -- not a physics difference.
        std::printf("seed payload: values are the state at t0; a first-tick "
                    "difference near one dx means the payload was written at "
                    "t0+1\n");
    }
    // --touchseed (--rotqtoggle's anchor seed): the touch boxes the anchored
    // attempt had already entered by t0, named by uid and mapped onto this
    // build's window exactly as the payload's touch list is above. OR-ed in, and
    // ownership is not claimed, so the recording-derived seeding below still runs
    // for every other box.
    if (t0 > 0 && !g_touchSeedArg.empty()) {
        int set = 0, unmapped = 0;
        for (const auto& ut : parseTouchPayload(g_touchSeedArg)) {
            int bit = -1;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
                if (g_touch[b].uid == ut.first) { bit = (int)b; break; }
            if (bit < 0) { ++unmapped; continue; }
            init.trig |= touchBit(bit);
            init.fireB[bit] = (uint16_t)std::max(0, ut.second);
            ++set;
        }
        std::printf("seed touchseed: %d boxes set, %d named but not in this "
                    "build's window\n", set, unmapped);
    }
    // WHICH GRAVITY PORTALS THE RUN HAD ALREADY SPENT before t0. Without this a
    // state handed to --start starts with an empty mask and re-fires every
    // portal the run has passed -- the same hole State::fireB, State::lockOff
    // and the rotation queue each fell into, and the reason the queue is still
    // opt-in. A portal is spent on OVERLAP, so the producer takes the passes
    // out of gdref's x,y rather than out of the firings.
    if (t0 > 0 && g_ownsPortal) {
        int set = 0, unmapped = 0;
        for (int half = 0; half < 2; ++half) {
            const std::vector<int>& ids = half ? payloadPortal2 : payloadPortal;
            GravLatch& mask = half ? init.portalLatch2 : init.portalLatch;
            for (int uid : ids) {
                int bit = -1;
                double px = 0.0;
                for (const Obj& p : L.portals)
                    if (p.uid == uid && p.gpBit >= 0) { bit = p.gpBit; px = p.cx; break; }
                // --tplatch: a teleport in dyn holds a bit too.
                if (bit < 0)
                    for (const Obj& p : L.dyn.objs)
                        if (p.uid == uid && p.gpBit >= 0) { bit = p.gpBit; px = p.cx; break; }
                if (bit < 0) { ++unmapped; continue; }
                // a shared bit already handed on past this portal belongs to the later one
                bool handedOn = false;
                for (const GpHandoff& h : g_gpHandoff)
                    handedOn = handedOn || (h.bit == bit && px < h.x && (double)init.xAbs >= h.x);
                if (handedOn) continue;
                // (this said `mask |= touchBit(bit)` -- the touch mask's helper on
                // the portal latch, which only agreed while both were 32 wide)
                mask.set(bit);
                ++set;
            }
        }
        std::printf("seed payload: %d gravity portals spent (%s/%s), %d "
                    "named but not a gravity portal in this level\n",
                    set, gravLatchHex(init.portalLatch).c_str(),
                    gravLatchHex(init.portalLatch2).c_str(), unmapped);
        std::printf("seed: portal=payload\n");
        // --tpmarkseed: the teleports with no latch bit that this run fired (the payload names them) and whose box still overlaps the anchor
        // pose -- the only ones that can fire a second time on the first step (State::tpMarkA, tpMarkB). The box is the portal's bound
        // against the full half 15, so a mini body may be marked for one it had cleared; the portal is in the payload either way. The
        // two slots are filled furthest-x first; more than two overlapping ones are said on stdout.
        if (g_tpMark && g_tpMarkSeed) {
            int uidA = -1, uidB = -1, nOver = 0;
            double cxA = -1e30, cxB = -1e30;
            auto consider = [&](const Obj& p) {
                if (p.type != 28 || p.gpBit >= 0 || p.uid < 0) return;
                if (std::find(payloadPortal.begin(), payloadPortal.end(), p.uid) == payloadPortal.end()) return;
                const double half = 15.0;
                if (std::fabs((double)init.xAbs - p.cx) > p.hw + half) return;
                if (std::fabs((double)init.y - p.cy) >= p.hh + half) return;
                ++nOver;
                if (p.cx > cxA) { uidB = uidA; cxB = cxA; uidA = p.uid; cxA = p.cx; }
                else if (p.cx > cxB) { uidB = p.uid; cxB = p.cx; }
            };
            for (const Obj& p : L.portals) consider(p);
            for (size_t k = 0; k < L.dyn.objs.size(); ++k)
                if (L.dyn.bucket[k] == Dynamics::PORT) consider(L.dyn.objs[k]);
            init.tpMarkA = uidA;
            init.tpMarkB = uidB;
            std::printf("seed: tpmark=%d,%d (%d fired teleports overlap the anchor pose)\n", uidA, uidB, nOver);
            if (nOver > 2)
                std::printf("seed: tpmark WARNING %d overlapping fired teleports, only two fit State::tpMarkA/B\n", nOver);
        }
    }
    // owns=hist: the per-body history values --start does not carry.
    if (t0 > 0 && g_ownsHist) {
        for (const auto& h : payloadHist) {
            if (h.first == "pressSpent") init.pressSpent = (uint8_t)(h.second != 0);
            else if (h.first == "pressSpent2")
                init.pressSpent2 = (uint8_t)(h.second != 0);
            else if (h.first == "freeMode" && g_freeModeCol)
                init.bandBranch = (uint8_t)((init.bandBranch & ~kBandFree)
                                            | kBandFreeKnown
                                            | (h.second != 0 ? kBandFree : 0));
            else if (h.first == "spiderJumpT" && h.second >= 0)
                init.spiderJumpT = (uint8_t)std::min(h.second, spiderJumpGraceTicks());
            // slopeOn .. slopeLanded (version 4) are seeded further down, under
            // --anchorride, once the geometric ride guess (rideAtAnchor) has run.
        }
        // Version 5: the second body's dash (frames.hpp). The slope is the ring's, built as
        // the dash's start builds it (step.hpp); the ring is looked up as --spentorb does, in
        // L.orbs and then among the moving rings.
        int dash2 = 0, dashRing2 = -1;
        for (const auto& h : payloadHist) {
            if (h.first == "dashing2") dash2 = h.second;
            else if (h.first == "dashRing2") dashRing2 = h.second;
        }
        if (init.dual && dash2 != 0) {
            const Obj* ring = nullptr;
            for (const Obj& o : L.orbs)
                if (o.uid == dashRing2) { ring = &o; break; }
            if (!ring)
                for (size_t i = 0; i < L.dyn.objs.size(); ++i)
                    if (L.dyn.objs[i].uid == dashRing2 && L.dyn.bucket[i] == Dynamics::ORB) {
                        ring = &L.dyn.objs[i];
                        break;
                    }
            init.dashing2 = 1;
            init.dashSlope2 = ring ? dashSlopeOf(ring, (int)init.frame) : 0.f;   // --dashclamp
            std::printf("seed: dashing2=1 ring uid %d%s slope %.6f\n", dashRing2,
                        ring ? "" : " NOT a ring in this level (slope 0)",
                        (double)init.dashSlope2);
        }
        // --spentaction: a UFO's or swing's flap fires on a fresh edge (`input && !s.action`), and
        // the anchor leaves action at 0, so a press GD had already consumed before t0 looked fresh
        // on the first tick. pressSpent is 0x985 && !0x986 -- the button held and its press used --
        // so the button was down: action 1 is the truth, and the edge is not there. A custom level
        // (custom level C, a cold run's attempt 16): a dual UFO pressing on 387 takes the pink rings
        // uid 392 / 324 on GD's row 388 (vy 4.696 / -4.696); the anchor at 388 says pressSpent 1,
        // and the model flapped again on 389 (6.871) where GD decays.
        if (init.pressSpent && (init.mode == 3 || init.mode == 7))
            init.action = 1;
        // --holdseed (speed.hpp): GD's +0x985 at 0 under a button the --start hold field says is
        // down is a dead hold (a ball's tap or a ring cleared it); the level stays 1, or the first
        // tick would read a fresh press and make the hold live again (step.hpp, the hold latch).
        if (histVersion >= 6 && g_startHoldField > 0) {
            int jb = -1, jb2 = -1;
            for (const auto& h : payloadHist) {
                if (h.first == "jumpBuf") jb = h.second;
                else if (h.first == "jumpBuf2") jb2 = h.second;
            }
            if (jb == 0) init.holdDead = 1;
            if (init.dual && jb2 == 0) init.holdDead2 = 1;
            if (init.holdDead || init.holdDead2) init.action = 1;
            std::printf("seed: holdseed jumpBuf=%d jumpBuf2=%d -> holdDead=%d holdDead2=%d\n", jb,
                        jb2, (int)init.holdDead, (int)init.holdDead2);
        } else if (g_holdSeed2 && histVersion >= 6 && g_twoPlayer && init.dual && g_startAct2 == 1) {
            // --holdseed2: the second player's own dead hold, whatever the first player's button is doing. The branch above
            // reads GD's second +0x985 only under a first-player hold, and a two-player anchor mostly has none.
            int jb2 = -1;
            for (const auto& h : payloadHist)
                if (h.first == "jumpBuf2") jb2 = h.second;
            if (jb2 == 0) init.holdDead2 = 1;
            std::printf("seed: holdseed2 jumpBuf2=%d -> holdDead2=%d\n", jb2, (int)init.holdDead2);
        }
        std::printf("seed: hist=payload v%d pressSpent=%d pressSpent2=%d "
                    "freeMode=%s spiderJumpT=%d", histVersion, (int)init.pressSpent,
                    (int)init.pressSpent2,
                    !(init.bandBranch & kBandFreeKnown) ? "unknown"
                    : (init.bandBranch & kBandFree) ? "1" : "0",
                    (int)init.spiderJumpT);
        if (histVersion >= 4)
            for (const auto& h : payloadHist)
                if (h.first.rfind("slope", 0) == 0)
                    std::printf(" %s=%d", h.first.c_str(), h.second);
        std::printf("\n");
    }
    // ...and at the level's own start the byte is known: resetLevel leaves
    // [layer+0x311] clear and only a mode portal writes it. Known only when the
    // objrects dump says which portals are Free Mode -- without that column no
    // portal could keep it up to date, so it stays unknown and the band keeps
    // the old grid test (bandIsWall).
    if (t0 <= 0 && g_freeModeCol) init.bandBranch |= kBandFreeKnown;
    // The recording-derived seeding still runs even when a payload owns
    // `touch`, because it also seeds the LOCK -- a subsystem no payload claims
    // today. What ownership changes is only whether the trig/fireB branches
    // below are allowed to write; skipping the whole block would leave the
    // lock unseeded, which is the death this code's first test produced.
    if (t0 > 0 && !g_touch.empty()) {
        if (!havePayload)
            std::printf("seed: touch=recording lock=recording\n");
        // Counted, because "nothing happened" is the failure mode this block has
        // (an anchor that silently keeps every door shut looks exactly like a
        // physics wall, and did for a whole session on lv22).
        int nCtl = 0, nNoRec = 0, nNoOff = 0, nNotEntered = 0, nEnteredRedated = 0;
        for (size_t b = 0; b < g_touch.size(); ++b) {
            // --touchentered: the boxes the attempt's own recorded positions
            // overlapped by t0 (the mod's touchEnteredArg). Given, a box
            // missing from it gets no bit from the recording below: an object
            // moving before t0 says SOMETHING moved it, and when its group is
            // also driven by an autonomous Move that something is not this box.
            // lv22's ceiling (group 265) is moved by the autonomous Move 18311
            // and by seven boxes' chains, so an anchor at x=2,954 used to open
            // all seven, the nearest 240 px ahead of anything the player had
            // reached. Not given, every box may open as before.
            // --seedchain (speed.hpp): ...and a Count or a Tap, which no row of the recording can "enter", is
            // judged by what it moves alone
            const bool chainBox = g_seedChain && (g_touch[b].count >= 0 || g_touch[b].tap);
            const bool mayOpen = !g_touchEnteredGiven
                                 || g_touchEntered.count(g_touch[b].uid) != 0 || chainBox;
            if (!mayOpen) ++nNotEntered;
            // --anchorentry: the box opened on a first motion measured after its entry, whose
            // tick is then kept rather than re-dated to the entry (the re-date below).
            bool keepRecFire = false;
            // HOW MANY BOXES CAN THIS SCAN NEVER SEED. Both branches below are
            // gated on `full > 0.5`, and `full` is the TRIGGER TABLE's offset,
            // so a box whose controllers all carry a zero offset -- move to
            // target, lock only, toggle -- gets no bit however plainly the
            // recording shows its objects moving. Counted rather than argued:
            // the fix's blast radius is exactly the boxes this reports.
            // WHY a box gets no bit, per box. Both branches below are gated on
            // `full > 0.5` and `full` is the TRIGGER TABLE's offset, so a
            // controller whose displacement does not live there -- Move-To
            // takes B-A at firing time, a lock or a toggle has no displacement
            // at all -- is invisible to them however plainly its object moved.
            // The three counts separate those cases so the fix is aimed rather
            // than loosened.
            if (g_seedDump >= 0 || g_seedEvery > 0) {
                int passes = 0, movedNoOff = 0, offNoMove = 0, noRec = 0;
                for (const TrigCtl& c : g_touch[b].ctl) {
                    const auto itc = gt.find(c.uid);
                    if (itc == gt.end() || itc->second.empty()) { ++noRec; continue; }
                    const DynSample& f = itc->second.front();
                    double mv = 0.0;
                    for (const DynSample& s : itc->second)
                        mv = std::max(mv, std::hypot((double)s.cx - (double)f.cx,
                                                     (double)s.cy - (double)f.cy));
                    const bool hasOff = std::hypot((double)c.dx, (double)c.dy) > 0.5;
                    if (hasOff && mv > 0.5) ++passes;
                    else if (!hasOff && mv > 0.5) ++movedNoOff;
                    else if (hasOff) ++offNoMove;
                }
                // ...and the key's window for the same box. It is built from
                // durTicks, and the chain walk only takes a hop's duration when
                // that hop MOVES something -- so a box whose mover is a Rotate
                // can lose the window for the same reason it loses the bit.
                const int win = (b < g_touchMoveTicks.size())
                                ? g_touchMoveTicks[b] : -1;
                std::printf("boxctl: box=%zu ctl=%zu seedable=%d "
                            "movedButNoTableOffset=%d offsetButNoMotion=%d "
                            "noRecording=%d window=%d%s\n",
                            b, g_touch[b].ctl.size(), passes, movedNoOff,
                            offNoMove, noRec, win,
                            (passes == 0 && movedNoOff > 0)
                                ? "  <- MISSED: recorded motion, no table offset"
                                : "");
            }
            for (const TrigCtl& c : g_touch[b].ctl) {
                ++nCtl;
                const auto it = gt.find(c.uid);
                const bool dbg = (g_dynDbg >= 0 && c.uid == g_dynDbg);
                if (it == gt.end() || it->second.empty()) {
                    ++nNoRec;
                    if (dbg)
                        std::printf("triggers: dyndbg uid=%d box=%zu -- no "
                                    "recording\n", c.uid, b);
                    continue;
                }
                if (std::hypot((double)c.dx, (double)c.dy) <= 0.5) ++nNoOff;
                if (dbg)
                    std::printf("triggers: dyndbg uid=%d box=%zu off=(%.1f,%.1f) "
                                "samples=%zu t[%d..%d]\n", c.uid, b,
                                (double)c.dx, (double)c.dy, it->second.size(),
                                it->second.front().t, it->second.back().t);
                // --anchorentry: the base is the recording at the attempt's own entry
                // into this box, else its first sample.
                long long entryT = -1;
                const DynSample* basePtr = &it->second.front();
                {
                    const auto et = g_touchEnteredT.find(g_touch[b].uid);
                    if (et != g_touchEnteredT.end() && et->second > 0
                        && (long long)et->second <= t0) {
                        entryT = et->second;
                        const DynSample* nextPtr = nullptr;
                        for (const DynSample& s : it->second) {
                            if ((long long)s.t <= entryT) basePtr = &s;
                            else if (!nextPtr) nextPtr = &s;
                        }
                        // ...unless the recording has the object MOVING through the entry (rows within
                        // 3 ticks on both sides): that motion began before this attempt got there --
                        // another attempt's entry, recorded -- and a base in the middle of it reads a
                        // finished slide as a slide in progress. A custom level (kitref): a box's 25
                        // objects move -12 over 720 ticks from t=4,722 in the recording, the attempt
                        // enters at 4,737, and the box was taken as still moving at an anchor 4,300
                        // ticks later. The first sample is the base then, as without the flag.
                        if (nextPtr && basePtr != &it->second.front()
                            && entryT - (long long)basePtr->t <= 3
                            && (long long)nextPtr->t - entryT <= 3) {
                            entryT = -1;
                            basePtr = &it->second.front();
                        }
                    }
                }
                const DynSample& s0 = *basePtr;
                const DynSample* at = &s0;
                for (const DynSample& s : it->second)
                    if ((long long)s.t <= t0) at = &s;
                const double moved = std::hypot((double)at->cx - (double)s0.cx,
                                                (double)at->cy - (double)s0.cy);
                const double full = std::hypot((double)c.dx, (double)c.dy);
                // WHEN the box was entered, from the recording rather than from
                // the anchor. The position at t0 cannot answer this once the
                // move has finished -- every fire tick at or before t0 - dur
                // puts the object in the same place -- but the recording holds
                // the whole history, and its first moving row is the move's
                // second tick (a box entered on F starts moving on F+1). The
                // recorder's epsilon is 0.05 px, so the first row past that is
                // the first row it wrote.
                //
                // This matters where the position does not: the formula path
                // opens its door FROM this tick, so an estimate 26 ticks late
                // (which `t0 - durTicks` is for lv19's uid14011 -- 20,530
                // against the recording's 20,504) opens the door 26 ticks late
                // and a hazard the real door had cleared kills the player.
                // A LOCK the anchor is already inside. `lockOff` is accumulated
                // tick by tick during a run, so an anchor dropped after the box
                // was punched has none of it -- the platform snaps back to its
                // base and the ride disappears, which is the same hole the fire
                // tick had. The recording answers it directly: this controller
                // moves the object nowhere (a lock-only Move carries offset
                // (0,0)), so whatever its recorded x has done since the first
                // sample IS the lock.
                if (c.lockTicks > 0.0
                    && std::hypot((double)c.dx, (double)c.dy) <= 0.5) {
                    const DynSample& f0 = it->second.front();
                    const DynSample* atL = &f0;
                    for (const DynSample& s : it->second)
                        if ((long long)s.t <= t0) atL = &s;
                    init.lockOff = (float)((double)atL->cx - (double)f0.cx);
                    if (dbg)
                        std::printf("triggers: dyndbg uid=%d box=%zu lock seed "
                                    "%.3f (recorded cx %.3f - base %.3f)\n",
                                    c.uid, b, (double)init.lockOff,
                                    (double)atL->cx, (double)f0.cx);
                }
                // ...minus how late that first row is. `- 1` was right only for
                // a move that clears 0.05 px in its first tick: the recorder
                // drops smaller changes, and an eased move spends its opening
                // ticks in the thousandths, so a SLOW controller's first row
                // can be several ticks after it started. recordLag computes
                // exactly that from the offset, the duration and the curve --
                // it is what the autonomous side already subtracts, and not
                // using it here left lv19's two slow boxes seeded 3 ticks late
                // (--seeddump: fireB[0] 19,586 against 19,589).
                const int lag = recordLag((double)c.dx, (double)c.dy,
                                          c.durTicks, c.ease, c.erate);
                // ...and the first row is looked for ALONG THIS CONTROLLER'S
                // OWN OFFSET. Taking the distance in both axes finds whichever
                // coordinate moved first, and for lv19's platforms that is the
                // LOCK -- their x tracks the player at 1.6 px/tick from the
                // moment the box is punched, so every row clears 0.05 px in x
                // while the eased y is still in the thousandths. The lag then
                // gets subtracted from the wrong row. Measured either way: the
                // door's fire tick is 20,503, which the y-only row (20,506)
                // minus its lag (3) gives and the both-axes row (20,504) does
                // not.
                const bool useX = std::fabs((double)c.dx) > 0.5;
                const bool useY = std::fabs((double)c.dy) > 0.5;
                long long recFire = -1;
                for (const DynSample& s : it->second) {
                    if (entryT >= 0 && (long long)s.t <= entryT) continue;   // --anchorentry
                    const double ddx = (double)s.cx - (double)s0.cx;
                    const double ddy = (double)s.cy - (double)s0.cy;
                    const bool moved =
                        (useX || useY)
                            ? ((useX && std::fabs(ddx) > 0.05)
                               || (useY && std::fabs(ddy) > 0.05))
                            : (std::hypot(ddx, ddy) > 0.05);
                    if (moved) {
                        // `- lag - 1`, not `- lag`: the move starts the tick
                        // AFTER the box is entered (the chain players read
                        // `(t - F - 1)/dur`), so the first row recordLag counts
                        // to is F + 1 + lag. Measured on all four of lv19's
                        // controllers at once -- 19,590-3-1, 19,950-3-1,
                        // 20,506-2-1 and the lock-only 20,504-0-1 give
                        // 19,586 / 19,946 / 20,503 / 20,503, which is what a
                        // whole run holds.
                        recFire = (long long)s.t - lag - 1;
                        // --spawnlag: ...and minus the chain's own queueing, which the
                        // formula path adds back on top of the box's fire tick (without
                        // this the anchored state counts it twice; spawnLag is 0 off).
                        recFire -= c.spawnLag;
                        // --seedchain: a Count's move starts on the tick it fires, not the one after, so the first
                        // row it writes is F + lag where a box's is F + 1 + lag
                        if (g_seedChain && g_touch[b].count >= 0) recFire += 1;
                        if (g_seedDump >= 0 || g_seedEvery > 0)
                            std::printf("seedscan: box=%zu uid=%d firstRow=%d "
                                        "lag=%d recFire=%lld dx=%.2f dy=%.2f "
                                        "dur=%.3f ease=%d\n",
                                        b, c.uid, s.t, lag, recFire,
                                        (double)c.dx, (double)c.dy, c.durTicks,
                                        c.ease);
                        break;
                    }
                }
                // A payload that owns `touch` has already set these; the rest
                // of this block still runs for the lock.
                if (havePayload) continue;
                if (!mayOpen) continue;
                if (full > 0.5 && moved > full * 0.5) {
                    init.trig |= touchBit(b);
                    keepRecFire = entryT >= 0 && recFire >= 0;   // --anchorentry
                    // far enough back that the move counts as finished
                    init.trigT = (int32_t)(t0 - (long long)std::llround(c.durTicks));
                    // ...and this box's own tick, or the anchored state would
                    // carry a set bit with no fire tick. fireB defaults to 0,
                    // which the key reads as "fired at tick 0" = long finished,
                    // so a box still sliding at the anchor would merge states
                    // that are genuinely at different points of it.
                    init.fireB[b] = (uint16_t)std::max(
                        0LL, recFire >= 0 ? recFire
                                          : t0 - (long long)std::llround(c.durTicks));
                    break;
                }
                // Started, but not half way: the anchor landed INSIDE the slide.
                // Neither "open" nor "shut" is true, so say open and let the
                // recording carry the phase (see g_recPhase).
                if (full > 0.5 && moved > 0.5) {
                    init.trig |= touchBit(b);
                    keepRecFire = entryT >= 0 && recFire >= 0;   // --anchorentry
                    g_recPhase |= touchBit(b);
                    // The anchor landed INSIDE the slide, so this box is still
                    // moving and the key has to keep it. How far in is what the
                    // recording already says: `moved` of `full` is the fraction
                    // travelled, so the move began that much before t0. (Under
                    // an ease this is approximate, and approximate LATE is the
                    // safe side -- it holds the box in the key a little longer
                    // than the motion lasts, which costs width, where early
                    // would merge states that still differ.)
                    init.fireB[b] = (uint16_t)std::max(
                        0LL, recFire >= 0
                                 ? recFire
                                 : t0 - (long long)std::llround(
                                            (moved / full) * c.durTicks));
                    break;
                }
                // NO DISPLACEMENT IN THE TABLE, BUT THE RECORDING MOVED.
                //
                // The two branches above answer two different questions with
                // one gate. "Did this state enter the box" is the `trig` bit
                // and CANNOT depend on how far anything travelled; "how far
                // along is the move" is the phase, and only exists where there
                // is a displacement to be a fraction of. Gating both on
                // `full > 0.5` silently answered the first with the second.
                //
                // What falls through is a controller whose motion is not a
                // translation. lv22's boxes 0, 2 and 5 are driven by Rotate
                // triggers (id 1346: uid199 -> group 39, uid255 -> 38,
                // uid321 -> 40 -- exactly three, exactly the three boxes that
                // were missed), and a rotation has no ox/oy to read: the zero
                // offset is CORRECT and the gate was wrong. Measured with
                // seedcheck: 768 unexpected field differences on lv22, every
                // one of them "recorded motion, no table offset", and zero of
                // the opposite kind.
                //
                // The bit is set from the recording having moved BEFORE the
                // anchor, which is the same evidence the branches above use
                // and does not care how the movement is expressed. No phase is
                // claimed: a rotation has no "fraction travelled", so this
                // does not set g_recPhase.
                // THE TICK THIS GIVES IS EARLY, AND KNOWINGLY SO. An object can
                // be under two controllers at once -- lv22's group 39 holds
                // both the touch Rotate (uid199) and an autonomous Move
                // (uid89, oy=+120) -- so the recording's first motion says
                // SOMETHING acted, not that this box was entered. uid195 climbs
                // from t=292 under the autonomous one and is only turned from
                // t=421, while the box is entered at 417.
                //
                // The rotation is not separable from the recording: grouptrace
                // writes the object's own `rot`, which stays 0.000 for the
                // whole run because the group is what turns, and the turn shows
                // only as position drift indistinguishable in kind from the
                // Move's. Splitting them needs the recorded ANGLE, which the
                // dump does not carry per object.
                //
                // So the bit is set from the earlier evidence and the tick is
                // early. That is the safe direction, the same asymmetry as the
                // key window: an early tick holds the box in the key longer
                // than it needs (cost), a late one merges states whose geometry
                // still differs (a wrong answer). Not setting the bit at all is
                // the worst of the three -- the anchor then plans against a
                // door the run had already opened.
                if (full <= 0.5 && recFire >= 0 && recFire <= t0) {
                    init.trig |= touchBit(b);
                    keepRecFire = entryT >= 0;   // --anchorentry
                    // fireB from the recording's own first motion, taken on
                    // EITHER axis because a rotation has no offset axis to
                    // prefer -- and nothing fast can contaminate it here: the
                    // only co-moving component that could is a lock, and the
                    // census found locks on lv19 alone. Early is the safe
                    // side, the same asymmetry as the key window: too early
                    // holds the box in the key longer (cost), too late merges
                    // states that still differ (a wrong answer).
                    init.fireB[b] = (uint16_t)std::max(0LL, recFire);
                    break;
                }
            }
            // --touchentered uid:tick: a box the scan opened is dated by the
            // attempt's own entry into it (the first recorded row that overlaps
            // it, markTouched's test), not by the first motion of an object it
            // moves -- which the branches above take, and which for an object an
            // autonomous Move also drives is that Move's (g_touchEnteredT).
            if (!havePayload && (init.trig & touchBit(b)) && !keepRecFire) {
                const auto et = g_touchEnteredT.find(g_touch[b].uid);
                if (et != g_touchEnteredT.end() && et->second > 0
                    && (long long)et->second <= t0) {
                    if (init.fireB[b] != (uint16_t)et->second) ++nEnteredRedated;
                    init.fireB[b] = (uint16_t)et->second;
                }
            }
        }
        // --tapclosebox: a box that ARMS a spawned Count or Tap (TouchTrig::armBy) is set when the
        // attempt had entered it by t0 (--touchentered uid:tick, markTouched's test on its rows).
        // The scan above opens a box only from the recorded motion of what its chain moves, and a
        // chain that ends at the roots it spawns may move nothing the recording shows: SubZero
        // 4003's box 6102 (entered at 13139) was missing from an anchor at 13212, so its Count
        // and Tap never listened there. Left to a payload that owns `touch`, as the rest.
        if (!havePayload && g_touchEnteredGiven) {
            TouchMask arms{};
            for (const TouchTrig& R : g_touch)
                if (R.armBy) arms |= R.armBy;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
                const TouchMask bit = touchBit((int)b);
                if (!(arms & bit) || (init.trig & bit)) continue;
                const auto et = g_touchEnteredT.find(g_touch[b].uid);
                if (et == g_touchEnteredT.end() || et->second <= 0
                    || (long long)et->second > t0)
                    continue;
                init.trig |= bit;
                init.fireB[b] = (uint16_t)et->second;
                init.trigT = std::max(init.trigT, (int32_t)et->second);
                std::printf("triggers: --tapclosebox: box %zu uid %d arms a spawned root and "
                            "was entered at t=%d <= t0, so the anchor starts with it set\n",
                            b, g_touch[b].uid, et->second);
            }
        }
        // --boxarm (constants.hpp): a toggle block that switches an offInit Move back on starts set when the
        // recording -- the anchor's own attempt, read up to its reach (--recprov) -- shows that Move firing, and
        // the block is behind a body at t0. The scan above opens a box from what its chain moves, and these move
        // nothing; --touchentered names only the first body's entries, button or not, while a custom level's
        // corridor has a block for each body. A Move is judged on an object it moves in y with no other Move of
        // that object in its window: the recorded change over the window is the Move's offset (fired) or nothing.
        // Dated where the x track first brings a body to the block, and never after the crossing.
        if (!havePayload && g_recProvGiven && g_recProvRec == g_recProvAnchor && !g_xTrack.empty()) {
            const double xP1 = xRefAt((int)t0);
            const double dx2 = init.dual ? (double)init.dx2 : 0.0;
            size_t nSet = 0;
            for (size_t k = 0; k < g_autoTrig.size(); ++k) {
                const AutoTrig& A = g_autoTrig[k];
                if (!A.offInit) continue;
                const int cross = xRefCross(A.cx);
                if (cross < 1) continue;
                int fired = -1;
                for (size_t i = 0; i < L.dyn.objs.size() && i < L.dyn.autoParts.size() && fired < 0; ++i) {
                    const Dynamics::AutoPart* mine = nullptr;
                    for (const Dynamics::AutoPart& p : L.dyn.autoParts[i])
                        if (p.trig == (int)k) mine = &p;
                    if (!mine || std::fabs(mine->dx) > 0.001f) continue;
                    const int w0 = cross - 1, w1 = cross + (int)std::ceil(mine->dur) + 3;
                    if ((long long)w1 > g_recProvReach) continue;
                    bool overlap = false;
                    for (const Dynamics::AutoPart& p : L.dyn.autoParts[i]) {
                        if (&p == mine || std::fabs(p.dy) < 0.001f) continue;
                        const int c2 = xRefCross(g_autoTrig[(size_t)p.trig].cx);
                        if (c2 >= 0 && c2 <= w1 && c2 + (int)std::ceil(p.dur) + 3 >= w0) overlap = true;
                    }
                    if (overlap) continue;
                    const std::vector<DynSample>& sm = L.dyn.samples[i];
                    if (sm.size() < 2 || sm.front().t > w0 || sm.back().t < w1) continue;
                    auto cyAt = [&](int t) {
                        size_t lo = 0, hi = sm.size();
                        while (lo + 1 < hi) {
                            const size_t md = (lo + hi) / 2;
                            if (sm[md].t <= t) lo = md; else hi = md;
                        }
                        return (double)sm[lo].cy;
                    };
                    const double d = cyAt(w1) - cyAt(w0);
                    if (std::fabs(d - (double)mine->dy) < 1.0) fired = 1;
                    else if (std::fabs(d) < 1.0) fired = 0;
                }
                if (fired != 1) continue;
                for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
                    const TouchTrig& B = g_touch[b];
                    bool arms = false;
                    for (const TouchTrig::TogEv& e : B.trigTog) arms = arms || (e.on && e.uid == A.uid);
                    if (!arms) continue;
                    if (init.trig & touchBit((int)b)) break;   // already set: this Move has its block
                    const double reach = B.cx + B.hw;
                    if (!(reach < xP1 || (dx2 > 0.0 && reach < xP1 + dx2))) continue;   // no body past it yet
                    const int at = std::max(0, std::min(xRefCross(B.cx - B.hw - dx2), cross - 1));
                    init.trig |= touchBit((int)b);
                    init.fireB[b] = (uint16_t)at;
                    init.trigT = std::max(init.trigT, (int32_t)at);
                    ++nSet;
                    std::printf("boxarm: box %zu uid %d starts set at t0=%lld - the recording fired the Move uid %d "
                                "on its crossing t=%d; dated t=%d\n", b, B.uid, t0, A.uid, cross, at);
                    break;
                }
            }
            std::printf("boxarm: anchor t0=%lld, %zu block(s) set from the recording\n", t0, nSet);
        }
        std::printf("triggers: anchor scan t0=%lld: %d controlled objects, "
                    "%d with no recording, %d with no offset\n",
                    t0, nCtl, nNoRec, nNoOff);
        if (g_touchEnteredGiven)
            std::printf("triggers: --touchentered names %zu boxes (%zu with an entry "
                        "tick, %d opened boxes re-dated by it); %d of %zu boxes "
                        "may not open from the recording\n",
                        g_touchEntered.size(), g_touchEnteredT.size(), nEnteredRedated,
                        nNotEntered, g_touch.size());
        if (init.trig)
            std::printf("triggers: anchor starts with mask 0x%llx (already open "
                        "in the recording)\n",
                        (unsigned long long)init.trig.word(0));
        if (g_recPhase)
            std::printf("triggers: mask 0x%llx was still MOVING at the anchor -- "
                        "replayed from the recording's own phase\n",
                        (unsigned long long)g_recPhase.word(0));
    }
    // Autonomous triggers behind the anchor fired before the sim begins; their
    // first-motion tick is already in the recording (recFire of the objects
    // they move), so resolving from it makes the re-timing a no-op for the
    // prefix. No recorded motion at all = treat the move as long finished
    // (fireT 0 saturates the analytic fallback).
    if (!g_autoTrig.empty() && x0 > 0.0) {
        // dx at the anchor, for turning "how far behind is this trigger" into
        // "how many ticks ago did we cross it". Same replay the tail does below.
        double dxA = kDxF;
        {
            size_t si = 0;
            auto gate = [&](const Obj& o) { return o.cx - o.hw - kCubeHalf; };
            while (si < L.speeds.size() && gate(L.speeds[si]) <= x0) {
                dxA = dxForSpeedId(L.speeds[si].id);
                ++si;
            }
        }
        // A level that can turn the player round (property 117 on a ring or a
        // pad, Obj::rev) breaks what est below assumes -- that x advanced
        // monotonically from the crossing to the anchor -- so est cannot vet
        // the recording there. What stays true is that a recorded tick at or
        // before the anchor lies in the prefix this anchor shares with the
        // recorded run, whichever side of x0 the trigger sits on: 4002 anchored
        // at t=20,861 with the player running back through x=31,874 had
        // uid17712 (cx=30,975) recorded at 19,999 and estimated at 20,304, and
        // the 305-tick shift held the drop of group 87 back until the model's
        // platform crushed the player.
        bool canReverse = false;
        for (const std::vector<Obj>* v : {&L.orbs, &L.pads, &L.dyn.objs})
            for (const Obj& o : *v) canReverse = canReverse || o.rev != 0;
        const auto sharedPast = [&](int rf) {
            return canReverse && rf >= 0 && (long long)rf <= (long long)t0;
        };
        int behind = 0, fromX = 0, fromRec = 0, xTracked = 0;
        if (g_autoTrigFrame)
            std::printf("autotrigframe: on (anchor frame %d, channel %d, %zu x-track rows with y/frame)\n", startFrame, g_startRotChan, g_xTrackW.size());
        for (size_t b = 0; b < g_autoTrig.size(); ++b) {
            AutoTrig& A = g_autoTrig[b];
            // --autotrigframe (dynamics.hpp): an anchor in a turned frame (x0 is its travel coordinate there) reads a trigger of the anchor's own
            // channel at ITS travel coordinate, and the x track's rows of that frame the same way. acx is what every comparison below calls the
            // trigger's x.
            double acx = A.cx;
            bool acxFrame = false;
            if (g_autoTrigFrame && startFrame != 0 && A.chan != 0 && A.chan == g_startRotChan) {
                double v;
                toFrame(startFrame, A.cx, A.cy, acx, v);
                acxFrame = true;
            }
            // --multilock: an object's extra lock starts from this trigger's own fireX unless the anchor is past it (below)
            for (std::vector<std::array<double, 4>>& xs : L.dyn.autoLockExtra)
                for (std::array<double, 4>& e : xs)
                    if ((int)e[0] == (int)b) e[3] = std::nan("");
            // recFire is the first ROW, which is 1-2 ticks after the move really
            // started (see recordLag). Everything here works in true ticks.
            int rf = -1;
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                if (L.dyn.autoAnchor[i] != (int)b || L.dyn.trigRecFire[i] < 0)
                    continue;
                // --anchornoop: its first recorded change is not this trigger's effect.
                if (i < L.dyn.anchorUnrec.size() && L.dyn.anchorUnrec[i])
                    continue;
                const int tr = L.dyn.trigRecFire[i] - L.dyn.autoLag[i];
                if (rf < 0 || tr < rf) rf = tr;
            }
            // [2026-08-22 r99] UNDER --trigraw, TRIGGERS AHEAD OF THE ANCHOR
            // ALSO TAKE THE RECORDING'S TICK FIRST. Across a rotated section
            // (2900) the world x does not advance while the camera x does not,
            // so "fire on the x crossing" is too early. Measured on lv22: the
            // model fires Toggle uid6439 (cx=15,765), which switches off group
            // 308, at t=12,627 (x crossing), but the live recording fires at
            // t=14,225 -- 1,598 ticks early. As a result the solid uid6332
            // that GD lands on at t=12,816 had vanished from the model
            // (census `m0/mini1/g0/gdg1/sp0.9/air` edvy -6.057).
            // --trigraw is only passed for section anchors of levels that have
            // a 2900 (quick_regress.rot_anchor_args), so lv1-21 are
            // bit-identical.
            if (acx > x0) {
                if ((g_trigRaw || sharedPast(rf)) && rf >= 0) {
                    A.fireT = rf;
                    A.fireX = A.lockLastX = acx;
                    if (g_dynDbg >= 0)
                        std::printf("anchortrig(ahead) uid=%d cx=%.1f rf=%d "
                                    "-> fireT=%d\n", A.uid, A.cx, rf, A.fireT);
                    continue;
                }
                // ...and `rf` only looks at the objects ANCHORED on this
                // trigger. An object driven by several controllers is anchored
                // on one of them, so a trigger that is only ever a PART of such
                // a superposition has no recorded tick at all and stays
                // unresolved -- and the superposition adds an unresolved part as
                // zero. SubZero 4002: the drop of group 87 (uid18255 at
                // x=31,899, (0,-600), duration 0) is a part of the saw uid16730,
                // whose anchor is another trigger; anchored past it in the
                // reversed section, the model kept that saw 600 px up and walked
                // through the kill GD makes at t=22,893.
                // The recording settles it: when an object's own rows show THIS
                // part's displacement already applied before the anchor, the row
                // that first shows it is the tick. Only for a single-part object
                // -- with two parts the rows are a sum and cannot be attributed.
                // ...and only for a move of DURATION 0. The row this looks for
                // is the one where the recording has reached the part's FULL
                // offset, which for a move that takes time is the tick it
                // ENDS: handing that tick to the closed form would replay the
                // whole ease from there and run the move late by its own
                // duration. An instant move begins and ends
                // on the same tick, so there the row is the answer.
                if (canReverse && A.fireT < 0) {
                    for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                        if (L.dyn.fromBootRows(i)) continue;   // --bootauto: rows on another clock
                        if (L.dyn.autoParts[i].size() != 1
                            || L.dyn.autoParts[i][0].trig != (int)b
                            || L.dyn.autoParts[i][0].dur > 0.0
                            || L.dyn.samples[i].size() < 2)
                            continue;
                        const Dynamics::AutoPart& p = L.dyn.autoParts[i][0];
                        const std::vector<DynSample>& sm = L.dyn.samples[i];
                        const double bx = sm[0].cx, by = sm[0].cy;
                        for (const DynSample& s : sm) {
                            if ((long long)s.t > t0) break;
                            if (std::fabs((s.cx - bx) - p.dx) > 1.0
                                || std::fabs((s.cy - by) - p.dy) > 1.0)
                                continue;
                            A.fireT = s.t;
                            A.fireX = A.lockLastX = acx;
                            ++fromRec;
                            // one line per resolution, always: the count alone
                            // cannot say which trigger took which object's word
                            std::printf("anchortrig: ahead trigger uid=%d cx=%.1f "
                                        "was unresolved -> fireT=%d from uid%d's "
                                        "recording (dur=%.2f d=(%.1f,%.1f))\n",
                                        A.uid, A.cx, A.fireT, L.dyn.objs[i].uid,
                                        p.dur, (double)p.dx, (double)p.dy);
                            break;
                        }
                        if (A.fireT >= 0) break;
                    }
                }
                continue;
            }
            // The crossing is a fact about THIS run's x, so derive it from x and
            // keep the recording's tick only when the two agree.
            //
            // The recording's tick is on the RECORDED run's timeline, which this
            // file already warns about for the forward case ("a bootstrap corpse
            // crossed x=30,375 at t=23,397 where the real run crosses at
            // t=21,690"). Behind the anchor it is worse, because a fireT in the
            // FUTURE reads as "not fired yet" and freezes the door at its first
            // sample. Measured 2026-08-09, lv20 uid15929 (id469, the block at
            // 28785,315 that moves -105): the live recording put its crossing at
            // t=20,928 while the run crosses x=28,545 at t=18,971 -- 1,957 ticks
            // of phase. The replay died at t=19,108 on that block sitting at its
            // STATIC cy=315, i.e. the model was flying into a door GD had opened
            // 137 ticks earlier. That is the whole x=28,867 wall.
            // [2026-08-21 r58] FLOOR, NOT ROUND. The crossing tick is "the
            // first tick at which x >= cx", so from x(t) = x0 - (t0-t)*dx:
            //   t >= t0 - (x0-cx)/dx  ->  crossing = t0 - floor((x0-cx)/dx)
            // llround steps one extra tick back for a fraction >= 0.5 and
            // fires 1 tick early. Measured on the lv20 t0=21,400 section:
            // uid18290 (cx=31,965, moves group 54 by +240) has rf=-1 (nothing
            // is anchored on this trigger) and falls back to estT.
            // (x0-cx)/dxA = 35.23/1.61426 = 21.82 -> llround 22 gives
            // estT=21,379, while the true effect tick is 21,380 (with a
            // t0=21,360 anchor it fires inside the sim and prints "crossed at
            // t=21379, effect t=21380"). floor 21 gives 21,380 and agrees.
            // The 1-tick phase is 0.74 px at EaseInOut's onset (f=0.237) --
            // the census family `m3/mini0/g0/gdg0/sp1.1/air` edy -0.743 was
            // entirely this, and r53, which pushes out onto the top face of
            // the moving solid uid18338, was pushing onto a face 0.744 px too
            // high.
            long long est = t0 - (long long)std::floor(
                (x0 - acx) / (dxA > 0.0 ? dxA : (double)kDxF)) + A.delay;
            // --xtrack: the crossing read off GD's own x, when the track reaches it.
            if (acxFrame) {
                // a turned frame: the rows of that frame, their travel coordinate from the dump's world x, y
                if (g_xTrackW.size() == g_xTrack.size())
                    for (size_t k = 0; k < g_xTrack.size(); ++k) {
                        if ((int)g_xTrackW[k][2] != startFrame) continue;
                        double u, v;
                        toFrame(startFrame, g_xTrackW[k][0], g_xTrackW[k][1], u, v);
                        if (u >= acx) {
                            est = (long long)g_xTrack[k].first + A.delay;
                            ++xTracked;
                            break;
                        }
                    }
            } else if (!g_xTrack.empty())
                for (const auto& r : g_xTrack)
                    if (r.second >= A.cx) {
                        est = (long long)r.first + A.delay;
                        ++xTracked;
                        break;
                    }
            const int estT = (int)std::max(0LL, est);
            // 60 ticks of slack: within that the recording IS this run's own
            // timeline (the live overlay comes from this plan's replays) and its
            // tick is exact, so keep the old no-op behaviour.
            // --trigraw removes this gate and always trusts the recording
            // (history at g_trigRaw's declaration: est assumes x advances
            // monotonically and lies behind a rotation maze).
            if (rf >= 0 && (g_trigRaw || sharedPast(rf)
                            || std::llabs((long long)rf - (long long)estT) <= 60)) {
                A.fireT = rf;
            } else {
                A.fireT = estT;
                if (rf >= 0) ++fromX;
            }
            // Behind the anchor the crossing x is not recorded anywhere, but it
            // is the trigger's own cx to within one tick of travel (the
            // crossing is the FIRST x at or past cx).
            A.fireX = A.lockLastX = acx;
            // --lockanchor [2026-09-19]: ...but for a lockToPlayerX trigger that
            // is not enough. lockLastX only moves while the replay is inside the
            // lock window, so an anchor past the window leaves fireX == lockLastX
            // and the object sits at its base: lv19 uid15908/15910 (trigger
            // uid15896, locked t=22,509..22,597) anchored at 22,600 came out at
            // cx 31,455 where GD holds 31,570.596, and the robot standing on them
            // fell. For an object moved in x by nothing but the lock, the
            // recording (this plan's own replays) holds the displacement
            // outright -- the same source the touch lock reads its lockOff from.
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                if (L.dyn.autoLockTrig[i] != (int)b
                    || L.dyn.samples[i].size() < 2 || L.dyn.fromBootRows(i))
                    continue;
                bool xFree = true;
                for (const auto& p : L.dyn.autoParts[i])
                    xFree = xFree && std::fabs(p.dx) <= 0.001f;
                if (!xFree) continue;
                const auto& sm = L.dyn.samples[i];
                const DynSample* at = &sm.front();
                for (const DynSample& s : sm)
                    if ((long long)s.t <= t0) at = &s;
                const double D = (double)at->cx - (double)sm.front().cx;
                // --lockmod (dynamics.hpp): the recorded displacement is the player's step times the Move's multiplier
                const double lm = lockMod(A);
                A.fireX = x0 - (lm != 0.0 ? D / lm : D);
                A.lockLastX = x0;
                break;
            }
            // --multilock: a lock that is only an EXTRA one of its objects (the --multilock note in dynamics.hpp) counts from the anchor
            // on: the recorded displacement above is the sum of all of them and sits in the first lock's term.
            {
                bool extra = false;
                for (std::vector<std::array<double, 4>>& xs : L.dyn.autoLockExtra)
                    for (std::array<double, 4>& e : xs)
                        if ((int)e[0] == (int)b) { e[3] = x0; extra = true; }
                if (extra) A.lockLastX = x0;
            }
            if (g_dynDbg >= 0)
                std::printf("anchortrig uid=%d cx=%.1f rf=%d estT=%d -> fireT=%d "
                            "(x0=%.2f dxA=%.5f t0=%lld)\n",
                            A.uid, A.cx, rf, estT, A.fireT, x0, dxA, (long long)t0);
            ++behind;
        }
        if (behind || fromRec)
            std::printf("autotrig: %d behind the anchor (%d re-dated from x, "
                        "the recording disagreed by >60 ticks); %d ahead of it "
                        "resolved from a recording\n", behind, fromX, fromRec);
        if (!g_xTrack.empty())
            std::printf("autotrig: %d of the %d behind the anchor dated by GD's x (--xtrack, "
                        "%zu rows)\n", xTracked, behind, g_xTrack.size());
        // --closedpast: a multi-Move object's closed form (Dynamics::autoClosed) is unchecked at load -- its
        // crossings are only dated here. On a level that turns the player round or rotates the gameplay, a
        // crossing can lie ahead of the anchor's x and still be in the past, and nothing dates it; held
        // against the object's own row at the anchor (the recording is this anchor's run), a closed form
        // that misses it by more than half a pixel gives way to the recording. A custom level's floor block
        // uid 30664: thirteen instant x Moves and a +3,900 y Move whose triggers the player ran back past in a
        // rotated stretch; the closed form left it 3,900 px down and the cube fell through the floor GD
        // lands on at t=13,875.
        // --closedpastall: ...on every level. A crossing the walk misses entirely is not only a reversal's: a
        // custom level's step block uid 5239 has three y Moves the walk found and +900 in x that it did not
        // (its rows sit at x 13,875, the closed form at 12,975); the mini UFO GD steps up 2 px onto it at
        // t=9,739 ran on under it in the model.
        if (g_closedPast && (canReverse || g_trigRaw || g_closedPastAll)) {
            int nOpen = 0;
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                if (!L.dyn.autoClosed[i] || L.dyn.autoParts[i].size() < 2 || L.dyn.autoLockTrig[i] >= 0
                    || L.dyn.fromBootRows(i))
                    continue;
                const std::vector<DynSample>& sm = L.dyn.samples[i];
                const DynSample* at = nullptr;
                for (const DynSample& s : sm) {
                    if ((long long)s.t > t0) break;
                    at = &s;
                }
                if (!at || at == &sm.front()) continue;
                double cx = sm.front().cx, cy = sm.front().cy;
                for (const Dynamics::AutoPart& p : L.dyn.autoParts[i]) {
                    const int f = g_autoTrig[(size_t)p.trig].fireT;
                    if (f < 0 || at->t < f) continue;
                    const int te = g_autoStop ? std::min(at->t, autoStopT(p.trig)) : at->t;
                    if (te < f) continue;
                    const double e = p.dur > 0.0 ? gdEase(p.ease, p.erate, (double)(te - f) / p.dur)
                                                 : instantE(te, f);
                    cx += p.dx * e;
                    cy += p.dy * e;
                }
                const bool missX = std::fabs(cx - (double)at->cx) > 0.5, missY = std::fabs(cy - (double)at->cy) > 0.5;
                if (!missX && !missY) continue;
                // --closedpastall, on a level that neither turns nor rotates: only a miss on an axis no part
                // of the closed form moves, where the walk has certainly lost a mover. A miss on a moving axis
                // can be the formula's own timing against another run's recording; lv20 t=22,200 went 400 ->
                // 89 tracked ticks when those were taken too.
                if (!canReverse && !g_trigRaw) {
                    bool movX = false, movY = false;
                    for (const Dynamics::AutoPart& p : L.dyn.autoParts[i]) {
                        movX = movX || std::fabs(p.dx) > 0.001f;
                        movY = movY || std::fabs(p.dy) > 0.001f;
                    }
                    if (!((missX && !movX) || (missY && !movY))) continue;
                }
                L.dyn.autoClosed[i] = 0;
                ++nOpen;
                if (g_dynDbg == L.dyn.objs[i].uid)
                    std::printf("closedpast: uid=%d closed form (%.2f,%.2f) vs its row t=%d (%.2f,%.2f) -> "
                                "recording\n", L.dyn.objs[i].uid, cx, cy, at->t, (double)at->cx,
                                (double)at->cy);
            }
            if (nOpen)
                std::printf("closedpast: %d closed-form objects miss their own row at the anchor and read "
                            "their recording\n", nOpen);
        }
    }
    // --areaadd: a closed-form object that Area Move actions also act on takes their displacement on top (dynamics.hpp g_areaAdd).
    // The rows up to the anchor (the recorded run's own x, --xtrack) are the check: the closed form plus the area must not miss
    // any of them by more than the closed form alone does, so an object the actions do not reach as the model reads them keeps
    // its old placement. With no row inside an action's reach both come out equal and the object takes it.
    if (g_areaAdd && !g_xTrack.empty() && L.dyn.areaOf.size() == L.dyn.objs.size()
        && L.dyn.areaArmed.size() == L.dyn.objs.size() && L.dyn.autoClosed.size() == L.dyn.objs.size()) {
        Dynamics& D = L.dyn;
        const size_t N = D.objs.size();
        D.areaAdd.assign(N, 0);
        size_t nCand = 0, nAdd = 0, nWorse = 0;
        for (size_t i = 0; i < N; ++i) {
            if (D.areaOf[i].empty() || D.areaArmed[i] || !D.autoClosed[i] || D.autoParts[i].empty()
                || D.autoLockTrig[i] >= 0 || D.trigMask[i].any() || D.samples[i].empty() || D.fromBootRows(i))
                continue;
            bool movesOnly = true, before = true;
            const std::vector<DynSample>& sm = D.samples[i];
            for (const int k : D.areaOf[i]) {
                movesOnly = movesOnly && g_areas[(size_t)k].id == 3006;
                before = before && (xRefAt(std::max(1, sm[0].t - 1)) < g_areas[(size_t)k].cx || sm[0].t <= 1);
            }
            if (!movesOnly || !before) continue;   // a scale / rotate needs the box; a base row that already carries an action has no base
            ++nCand;
            const double bx0 = sm[0].cx, by0 = sm[0].cy;
            double worstWith = 0.0, worstWithout = 0.0;
            for (size_t r = 1; r < sm.size() && (long long)sm[r].t <= t0; ++r) {
                double cx = bx0, cy = by0;
                for (const Dynamics::AutoPart& p : D.autoParts[i]) {
                    const int f = g_autoTrig[(size_t)p.trig].fireT;
                    if (f < 0 || sm[r].t < f) continue;
                    const int te = g_autoStop ? std::min(sm[r].t, autoStopT(p.trig)) : sm[r].t;
                    if (te < f) continue;
                    const double e = p.dur > 0.0 ? gdEase(p.ease, p.erate, (double)(te - f) / p.dur) : instantE(te, f);
                    cx += p.dx * e;
                    cy += p.dy * e;
                }
                const double prx = xRefAt(std::max(1, sm[r].t - 1));
                double adx = 0.0, ady = 0.0;
                for (const int k : D.areaOf[i]) {
                    const AreaAct& a = g_areas[(size_t)k];
                    if (prx < a.cx) continue;
                    double ax = 0.0, ay = 0.0;
                    if (areaDisp(a, cx, areaCentreX(a, prx, std::max(1, sm[r].t - 1)), ax, ay)) { adx += ax; ady += ay; }
                }
                worstWithout = std::max({worstWithout, std::fabs(cx - (double)sm[r].cx), std::fabs(cy - (double)sm[r].cy)});
                worstWith = std::max({worstWith, std::fabs(cx + adx - (double)sm[r].cx), std::fabs(cy + ady - (double)sm[r].cy)});
            }
            const bool ok = worstWith <= worstWithout + 0.01;
            if (g_dynDbg == D.objs[i].uid || g_dynDbg == -6)
                std::printf("areaadd: uid %d, %zu action(s), rows to t=%lld: worst %.3f px with the area, %.3f without -> %s\n",
                            D.objs[i].uid, D.areaOf[i].size(), (long long)t0, worstWith, worstWithout, ok ? "added" : "not added");
            if (ok) { D.areaAdd[i] = 1; D.anyAuto = true; ++nAdd; }
            else ++nWorse;
        }
        std::printf("areaadd: %zu closed-form object(s) under Area Moves; %zu take their displacement, %zu fit the rows worse "
                    "with it and keep the old placement\n", nCand, nAdd, nWorse);
        ++g_dynPlaceGen;   // areaAdd is purity-relevant (isPure)
    }
    std::printf("recanchorx: %d objects re-paired on the recording run's crossing of their anchor%s\n",
                g_recAnchorXN, g_xTrack.empty() ? " (no --xtrack: none decided)" : "");
    std::printf("rotlag: %d objects anchored on a Rotate given the rotation's threshold lag\n", g_rotLagN);
    std::printf("rottail: %d turned objects go on turning past the recording's end\n", g_rotTailN);
    std::printf("recendtail: %d of %d rig objects go on past their last row as the simulated rig%s; %d "
                "objects' switches go on past it\n", g_recEndTailN, g_recEndTailTried,
                g_xTrack.empty() ? " (no --xtrack: none simulated)" : "", g_recEndTailOnN);
    // --rectouchentered: the recording's own attempt's entries, by bit (dynamics.hpp g_recOwnEntry)
    g_recOwnEntry.fill(-1);
    if (g_recTouchEnteredGiven) {
        int nOwn = 0;
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            const auto et = g_recTouchEnteredT.find(g_touch[b].uid);
            if (et != g_recTouchEnteredT.end() && et->second > 0) { g_recOwnEntry[b] = et->second; ++nOwn; }
        }
        std::printf("rectouchentered: the recording's attempt entered %d of %zu boxes%s\n", nOwn,
                    std::min(g_touch.size(), (size_t)kTouchBits), g_touchOnRec ? " (--touchonrec reads them)" : "");
    }
    // --formularec: the attempt's own entry into each box, by bit (dynamics.hpp g_touchRecEntry),
    // from --touchentered uid:tick -- entries after t0 are not known, and read as "not entered".
    {
        g_touchRecEntry.fill(-1);
        int nRec = 0;
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            const auto et = g_touchEnteredT.find(g_touch[b].uid);
            if (et != g_touchEnteredT.end() && et->second > 0 && (long long)et->second <= t0) {
                g_touchRecEntry[b] = et->second;
                ++nRec;
            } else if (g_recProvGiven) {   // --formularectouch
                const auto rt = g_recTouch.find(g_touch[b].uid);
                if (rt != g_recTouch.end() && rt->second > 0 && (long long)rt->second <= g_recProvReach) {
                    g_touchRecEntry[b] = rt->second;
                    ++nRec;
                }
            }
        }
        size_t nForm = 0;
        for (size_t i = 0; i < L.dyn.objs.size(); ++i)
            if (i < L.dyn.formula.size() && L.dyn.formula[i]) ++nForm;
        std::printf("formularec: %zu formula-driven objects, %d boxes with the attempt's entry tick\n",
                    nForm, nRec);
        if (!g_recProvGiven)
            std::printf("formularec: no --recprov, so the recording's attempt is unknown and it is not read\n");
        else
            std::printf("formularec: recording of attempt %lld, anchor rows of attempt %lld, reached t=%lld -> %s\n",
                        g_recProvRec, g_recProvAnchor, g_recProvReach,
                        (g_recProvRec >= 0 && g_recProvRec == g_recProvAnchor) ? "read up to that tick"
                                                                                : "another attempt, not read");
    }
    // --slambox: the boxes the recorded attempt fired, by bit, each with its entry on the recorded rows
    // (dynamics.hpp g_recTouchBit, from --rectouch). Not given (the unanchored call), the slam loop
    // keeps --slamdedup's rule.
    {
        g_recTouchBit.fill(-1);
        int nRec = 0;
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            const auto rt = g_recTouch.find(g_touch[b].uid);
            if (rt != g_recTouch.end() && rt->second > 0) {
                g_recTouchBit[b] = rt->second;
                ++nRec;
            }
        }
        if (g_recTouchGiven)
            std::printf("slambox: --rectouch names %zu boxes, %d of them loaded with an entry tick\n",
                        g_recTouch.size(), nRec);
        else
            std::printf("slambox: no --rectouch on this call, the slam steps follow --slamdedup (%s)\n",
                        "on");
    }
    // --dyndbg <uid>: everything that decides where one moving object is placed.
    // Added 2026-08-09 chasing lv20's x=28,867 wall, where the model killed on
    // uid15929 at its STATIC cy=315 although the recording ends at cy=210.
    // Guessing between "not linked to its trigger", "fireT unresolved" and
    // "overlaid by a shorter timeline" cost more than printing all three.
    if (g_dynDbg >= 0) {
        bool found = false;
        for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
            if (L.dyn.objs[i].uid != g_dynDbg) continue;
            found = true;
            const auto& sm = L.dyn.samples[i];
            std::printf("dyndbg: uid=%d IN DYN  samples=%zu", g_dynDbg, sm.size());
            if (!sm.empty())
                std::printf("  first(t=%d cy=%.3f)  last(t=%d cy=%.3f)",
                            sm.front().t, sm.front().cy, sm.back().t, sm.back().cy);
            std::printf("\n  closed=%d  trigMask=0x%llx recFire=%d (lag %d -> "
                        "true %d; autoLag %d)  autoAnchor=%d autoD=(%.1f,%.1f) "
                        "autoDur=%.2f ease=%d/%.2f\n",
                        (int)L.dyn.autoClosed[i],
                        (unsigned long long)L.dyn.trigMask[i].word(0),
                        L.dyn.trigRecFire[i], L.dyn.recLag[i],
                        L.dyn.trigRecFire[i] >= 0
                            ? L.dyn.trigRecFire[i] - L.dyn.recLag[i] : -1,
                        L.dyn.autoLag[i],
                        L.dyn.autoAnchor[i], L.dyn.autoDx[i], L.dyn.autoDy[i],
                        L.dyn.autoDur[i], L.dyn.autoEase[i], L.dyn.autoErate[i]);
            const int aA = L.dyn.autoAnchor[i];
            if (aA >= 0)
                std::printf("  anchor trigger: uid=%d cx=%.1f fireT=%d delay=%d\n",
                            g_autoTrig[(size_t)aA].uid, g_autoTrig[(size_t)aA].cx,
                            g_autoTrig[(size_t)aA].fireT,
                            g_autoTrig[(size_t)aA].delay);
        }
        if (!found)
            std::printf("dyndbg: uid=%d is NOT in dyn (static grid)\n", g_dynDbg);
    }
    if (g_needUnseen) {
        // A box is "seen" when at least one of the objects it controls actually
        // moved in a recording. Read off Dynamics, which already joined the map
        // to the timeline.
        for (size_t b = 0; b < g_touch.size(); ++b) {
            bool seenIt = false;
            for (size_t i = 0; i < L.dyn.objs.size() && !seenIt; ++i)
                if ((L.dyn.trigMask[i] & touchBit(b)) && L.dyn.trigRecFire[i] >= 0)
                    seenIt = true;
            // ...and one the anchor already entered is settled, not unknown.
            if (!seenIt && !(init.trig & touchBit(b))) {
                // Unreachable from this anchor: the tail starts past the box.
                // Reported either way so the driver can tell a blocked anchor
                // from a physics wall; only --needtrig-skip drops it.
                const bool passed = g_touch[b].cx + g_touch[b].hw + 40.0 < x0;
                const bool skip = (g_needSkip & touchBit(b)).any();
                std::printf("needtrig: box=%zu cx=%.0f cy=%.0f hw=%.0f "
                            "passed=%d skipped=%d\n",
                            b, g_touch[b].cx, g_touch[b].cy, g_touch[b].hw,
                            passed ? 1 : 0, skip ? 1 : 0);
                if (skip) continue;
                g_needTrig |= touchBit(b);
                // Same facts, for a caller with no stdout to read (dp/progress.hpp)
                g_outcome.needTrigMask |= touchBit(b);
                if (passed) g_outcome.needTrigPassed |= touchBit(b);
            }
        }
    }
    // --needtrig-uid: the same requirement, the box named by its trigger's uid (this call's bit
    // numbering is its own window's). A box the anchor has already passed, or one outside the
    // window, is not required -- said so, never silently.
    for (const int u : g_needTrigUids) {
        int bit = -1;
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
            if (g_touch[b].uid == u) { bit = (int)b; break; }
        if (bit < 0) {
            std::printf("needtrig-uid: uid %d is not a box of this call's window - not required\n", u);
            continue;
        }
        const TouchTrig& T = g_touch[(size_t)bit];
        if (init.trig & touchBit(bit)) {
            std::printf("needtrig-uid: uid %d (box %d) is already entered at the anchor\n", u, bit);
            continue;
        }
        // --needtrigrev: behind is the side the anchor travels away from
        const bool behind = (g_needTrigRev && init.rev) ? T.cx - T.hw - 40.0 > x0
                                                        : T.cx + T.hw + 40.0 < x0;
        if (behind) {
            std::printf("needtrig-uid: uid %d (box %d, cx=%.0f) is behind the anchor x=%.0f - not "
                        "required\n", u, bit, T.cx, x0);
            continue;
        }
        g_needTrig |= touchBit(bit);
        std::printf("needtrig-uid: uid %d = box %d cx=%.0f cy=%.0f hw=%.0f - required\n", u, bit,
                    T.cx, T.cy, T.hw);
    }
    // After the level, because the queue joins to g_rotTrig by uid. Defaulted
    // from the objrects path the same way levelsettings is, so the harnesses do
    // not each have to learn a flag.
    if (rotQPath.empty()) rotQPath = rotQPathBeside(argv[1]);
    if (!rotQPath.empty() && !g_rotTrig.empty() && !loadRotQueue(rotQPath))
        std::printf("rotq: could not read %s\n", rotQPath.c_str());
    // The cursor is a 32-bit set: State::rotSpent marks a consumed entry by its
    // queue index, and both places that touch it stop at 32 -- step.hpp's
    // `if (idx < 32)` and buildRotQueue's channel mask. Past that an entry is
    // CONSUMED WITHOUT BEING RECORDED, so the popcount that picks the next one
    // points at an entry the walk already passed, silently and forever after.
    // Refuse the queue instead, and say why: this is the same rule --startrotq
    // follows a few lines down, where a uid that does not resolve is named
    // rather than dropped. lv22 -- the only level with a queue -- holds 30, so
    // this cannot fire today; it is here because 30 is two short of the limit
    // and nothing else would report crossing it.
    if (g_rotQ.size() > 32) {
        std::printf("rotq: %zu entries, but State::rotSpent is a 32-bit cursor "
                    "- entries 33+ would be consumed without being recorded. "
                    "Refusing the queue on this level.\n", g_rotQ.size());
        g_rotQueue = false;
        g_rotQ.clear();
    }
    // The queue in consumption order, handed back for a caller that derives its
    // own seed (progress.hpp rotQOrder). After the refusal above, so a refused
    // queue hands back nothing; printed nowhere, so no output changes.
    if (!g_rotQ.empty())
        for (int ch = 0; ch <= 15; ++ch)
            for (int k = g_rotQBeg[(size_t)ch]; k < g_rotQEnd[(size_t)ch]; ++k) {
                const RotQEntry& e = g_rotQ[(size_t)k];
                char b[192];
                std::snprintf(b, sizeof b, "%s%d,%d,%.4f,%.4f,%d,%d,%d,%d,%d",
                              g_outcome.rotQOrder.empty() ? "" : ";", ch, e.uid,
                              (double)e.px, (double)e.py, (int)e.swarm, (int)e.swch,
                              (int)e.chanOnly,
                              e.rotIdx >= 0 ? g_rotTrig[(size_t)e.rotIdx].gndDir : -1,
                              (int)e.id);
                g_outcome.rotQOrder += b;
            }
    // --rotqtoggle's masks (frames.hpp). For each queue entry and each pre-queue
    // trigger: the bits of the touch boxes whose Toggle names the object in its
    // chain (TouchTrig::ctl) and switches that group off (togOn 0) or on (1).
    // Built here because both tables exist by now: g_touch was loaded with the
    // level, the queue just above. Zeros with the flag off, so nothing reads
    // differently and nothing is printed.
    g_rotQOff.assign(g_rotQ.size(), 0);
    g_rotQOn.assign(g_rotQ.size(), 0);
    g_rotTrigOff.assign(g_rotTrig.size(), 0);
    g_rotTrigOn.assign(g_rotTrig.size(), 0);
    if (g_rotQToggle) {
        auto maskFor = [](int uid, int want) {
            uint32_t m = 0;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
                if (g_touch[b].togOn != want) continue;
                for (const TrigCtl& ct : g_touch[b].ctl)
                    if (ct.uid == uid) { m |= (uint32_t)touchBit((int)b).word(0); break; }
            }
            return m;
        };
        for (size_t k = 0; k < g_rotQ.size(); ++k) {
            g_rotQOff[k] = maskFor(g_rotQ[k].uid, 0);
            g_rotQOn[k] = maskFor(g_rotQ[k].uid, 1);
            if (g_rotQOff[k] | g_rotQOn[k])
                std::printf("rotqtoggle: queue uid %d off=0x%x on=0x%x\n",
                            g_rotQ[k].uid, g_rotQOff[k], g_rotQOn[k]);
        }
        for (size_t k = 0; k < g_rotTrig.size(); ++k) {
            g_rotTrigOff[k] = maskFor(g_rotTrig[k].uid, 0);
            g_rotTrigOn[k] = maskFor(g_rotTrig[k].uid, 1);
        }
    }
    // --startrotq: the queue's --spentrot. Applied HERE because the uids have
    // to be resolved against the built queue, and before init is used -- the
    // --replay path takes its copy at `State s = init` well below, and the
    // search takes its own at `cur.push_back(init)` below that.
    //
    // Every uid that does not resolve is NAMED. A seed that silently drops
    // entries is worse than no seed: it produces a state that looks anchored
    // and is not, which is the failure the flag exists to fix.
    if (g_startRotChan >= 0) {
        if (g_rotQ.empty()) {
            std::printf("startrotq: no rotation queue on this level - the seed "
                        "has nothing to bind to and is ignored\n");
            g_outcome.startRotHit = 0;
            g_outcome.startRotGiven = (int)g_startRotSpent.size();
            g_outcome.startRotMiss = "(no queue)";
        } else {
            init.rotChan = (uint8_t)g_startRotChan;
            init.rotRev = (uint16_t)g_startRotRev;
            init.rotSpent = 0;
            int hit = 0;
            std::string miss;
            for (int su : g_startRotSpent) {
                bool found = false;
                for (size_t q = 0; q < g_rotQ.size() && q < 32; ++q)
                    if (g_rotQ[q].uid == su) {
                        init.rotSpent |= (uint32_t)1 << q;
                        found = true;
                        ++hit;
                        break;
                    }
                if (!found) {
                    miss += miss.empty() ? "" : ",";
                    miss += std::to_string(su);
                }
            }
            std::printf("startrotq: chan=%d rev=0x%04x spent=%d/%zu of %zu "
                        "queued%s%s\n",
                        (int)init.rotChan, (unsigned)init.rotRev, hit,
                        g_startRotSpent.size(), g_rotQ.size(),
                        miss.empty() ? "" : "  NOT IN THE QUEUE: ",
                        miss.c_str());
            g_outcome.startRotHit = hit;
            g_outcome.startRotGiven = (int)g_startRotSpent.size();
            g_outcome.startRotMiss = miss;
        }
    }
    if (g_ctrlWinTrig && !g_ctrlTrigs.empty()) {
        int gated = 0;
        for (const CtrlTrig& t : g_ctrlTrigs) gated += t.gated;
        std::printf("ctrlwintrig: %zu Options triggers on player 1's controls (%d touch/spawn-triggered); %s\n",
                    g_ctrlTrigs.size(), gated,
                    !g_rotTrig.empty() ? "the level turns its gameplay - not used"
                    : g_ctrlTrigFrom == std::numeric_limits<long long>::max() ? "the recorded windows stand"
                                                                              : "answering past the reach");
    }
    std::printf("level: %zu colliders, %zu portals, %zu pads, %zu orbs, "
                "%zu moving, maxX=%.0f\n",
                L.objs.size(), L.portals.size(), L.pads.size(), L.orbs.size(),
                L.dyn.size(), L.maxX);
    if (g_dynSeamCut > 0)
        std::printf("dynseam: %d moving object(s) lost their recorded rows after a gap longer than %s (a seam between attempts, by the rule; --dynseamgap/--dynseamlen)\n",
                    g_dynSeamCut, g_dynSeamLen ? "the level's length in ticks" : std::to_string(g_dynSeamGap).c_str());
    // GAMEPLAY ROTATION. Printed always so a level that has them says so --
    // the model's whole frame ("x is the clock") only holds inside one of these.
    if (!g_rotTrig.empty()) {
        std::printf("rotation: %zu gameplay-rotation objects (id 2900)\n",
                    g_rotTrig.size());
        for (const RotTrig& r : g_rotTrig)
            std::printf("  rot: (%.0f,%.0f) frame=%d travel=%s\n",
                        r.cx, r.cy, r.frame,
                        r.frame == 0 ? "+X" : r.frame == 1 ? "-Y"
                        : r.frame == 2 ? "-X" : "+Y");
    }
    // ...and the queue those objects sit in, if the dump was passed. Printed in
    // consumption order per channel, because the order IS the mechanism: GD
    // walks one channel from a cursor and stops at the first element the player
    // has not passed, so an element in the wrong place blocks everything behind
    // it.
    //
    // [2026-09-10] The queue IS read now -- step.hpp:361 and :426, under
    // `--rotqueue`. The line this replaces said nothing did, which was true when
    // the queue was built and stopped being true without the comment moving.
    // What the line is FOR has changed with it: it is no longer a display, it is
    // the arm's proof of life. A run that turns the queue on and does not print
    // this loaded no queue, and its result is "the queue never ran" rather than
    // "the queue changed nothing" -- the two readings a silent empty arm cannot
    // be told apart by.
    if (!g_rotQ.empty()) {
        std::printf("rotq: %zu queued objects in %d channels\n",
                    g_rotQ.size(), g_rotQChans);
        for (int ch = 0; ch <= 15; ++ch) {
            if (g_rotQEnd[(size_t)ch] <= g_rotQBeg[(size_t)ch]) continue;
            int d = 4;
            for (const RotQEntry& e : g_rotQ)
                if (e.swarm && e.swch == ch && e.rotIdx >= 0) {
                    d = rotQDirection(g_rotTrig[(size_t)e.rotIdx].rawRot,
                                      g_rotTrig[(size_t)e.rotIdx].flipX != 0);
                    break;
                }
            std::printf("  rotq ch=%d dir=%s n=%d\n", ch,
                        d == 1 ? "y+" : d == 2 ? "y-" : d == 3 ? "x-" : "x+",
                        g_rotQEnd[(size_t)ch] - g_rotQBeg[(size_t)ch]);
            for (int k = g_rotQBeg[(size_t)ch]; k < g_rotQEnd[(size_t)ch]; ++k) {
                const RotQEntry& e = g_rotQ[(size_t)k];
                std::printf("    [%d] uid=%d ord=%d (%.0f,%.0f) swarm=%d "
                            "swch=%d chanOnly=%d gnddir=%d rev=%d\n",
                            k - g_rotQBeg[(size_t)ch], e.uid, e.ord, e.px, e.py,
                            e.swarm, e.swch, e.chanOnly,
                            e.rotIdx >= 0 ? g_rotTrig[(size_t)e.rotIdx].gndDir : -1,
                            e.rotIdx >= 0
                                ? ((unsigned)(g_rotTrig[(size_t)e.rotIdx].gndDir - 2) < 2u)
                                : 0);
            }
        }
    }
    // How much of the moving geometry the closed form can actually stand in for
    // -- printed always, so a level whose doors it cannot describe says so
    // instead of quietly falling back.
    if (L.dyn.anyAuto) {
        size_t autoN = 0, closedN = 0, closedRec = 0, lockN = 0;
        for (size_t i = 0; i < L.dyn.size(); ++i) {
            if (L.dyn.autoAnchor[i] < 0) continue;
            ++autoN;
            if (L.dyn.autoLock[i] > 0.0) ++lockN;
            if (!L.dyn.autoClosed[i]) continue;
            ++closedN;
            if (L.dyn.trigRecFire[i] >= 0) ++closedRec;
        }
        std::printf("autotrig: %zu/%zu moved objects match the closed form "
                    "(%zu of them checked against a recording, %zu follow the "
                    "player)%s\n",
                    closedN, autoN, closedRec, lockN,
                    ", --trigclosed ON");
    }
    // A re-anchor taken while the player was RIDING A RAMP used to restart with
    // onSlope = 0. The ride is sticky (see stickToSlope), so without it the
    // model free-falls off every downhill ramp it re-anchors on -- measured on
    // lv16: anchored at t=6300 on a downhill the frontier fell away and died 40
    // ticks later at x=9,372, while GD rode the ramp for hundreds more.
    // The dump has no such column, so infer it from the geometry exactly the
    // way the ride does: grounded, and sitting on a ramp's resting height.
    // ...and the SECOND BODY needs exactly the same inference. It rides its own
    // ramps, it has its own onSlope2 / slopeM2 / slopeT2, and `--start` carries
    // none of them -- so an anchor taken while the dual's second half was mid-ride
    // restarted it at slopeT2 = 0 while p1 got the saturated 24, and the two halves
    // left the ramp with different launches.
    // Measured on lv16 t=7,816 (both bodies on mirrored m=+-0.5 ramps, jumping
    // together): GD gives an exact mirror, p1 +12.663 / p2 -12.663. The model had
    // p1 +12.663 to the digit and p2 -12.197 -- the slope-exit bonus scaled by
    // slopeRampFactor(14 + 1) instead of (24 + 1), because p2 had only been able to
    // count the 15 ticks since the anchor. That 0.466 is the whole of the section.
    auto rideAtAnchor = [&](uint8_t grounded, uint8_t flip, float y,
                            uint8_t& onSlope, float& slopeM, uint8_t& slopeT,
                            uint8_t& rideLanded) {
        if (!grounded) return;
        const double pH = init.mini ? kMiniHalf : kCubeHalf;
        for (const Obj& sp : L.slopes) {
            const double sx0 = sp.cx - sp.hw, sx1 = sp.cx + sp.hw;
            const double m = (sp.sy1 - sp.sy0) / (sx1 - sx0);
            if (m == 0.0) continue;
            // Which corner of the box touches the line is decided in the
            // player's OWN gravity frame, so a hanging body takes the offset on
            // the other side -- the same mirror the hang-side rules make about
            // everything else. Written as "m as this body sees it".
            // With the upright sign both ways, p2's seat test missed by exactly
            // 2*slopeXOffset: lv16 t=7,800, ceiling ramp uid3357 (m=-0.5,
            // sy0=570), GD has p2 at 538.580 and the unmirrored formula asks for
            // 542.120. Mirrored it lands on 538.580 to the digit, the same way
            // p1 lands on its own 451.420.
            const double mEff = flip ? -m : m;
            const double xr0 =
                x0 + (mEff > 0 ? slopeXOffset(m, pH) : -slopeXOffset(m, pH));
            if (xr0 < sx0 || xr0 > sx1 + 1.5) continue;
            const double top =
                sp.sy0 + m * (std::min(xr0, sx1) - sx0) + (flip ? -pH : pH);
            if (std::fabs((double)y - top) > 0.6) continue;
            onSlope = 1;
            slopeM = (float)m;
            // KNOWN GAP (same class as the missing snapObj above): the dump
            // carries no slope-ride age, so an anchor taken mid-ride guesses
            // SATURATED (24 = full exit impulse). Right whenever the real ride
            // is >= 0.1 s -- the long chained climbs where anchors actually
            // land -- and over-launches on a shorter one; the loop's own GD
            // replay catches that case.
            slopeT = 24;
            // ...and the ride counts as LANDED, for the same reason slopeT is
            // saturated here: `--start` does not carry it, and 0 is not the
            // neutral value -- it means "never landed", which suppresses the
            // exit launch and release (State::rideLanded). An anchor taken
            // mid-ride would then leave the ramp with no launch at all, and
            // every anchored section that starts on a ramp would diverge from
            // the whole-run replay of the same plan. Caught exactly that way:
            // quick_regress lv16 lost 2 sections (t=5,400 went 400 -> 1 ticks)
            // while the whole-run A/B was byte-identical on all 21 other levels.
            // 1 is the pre-change behaviour, so an unseeded anchor behaves as it
            // always did and the loop's own GD replay judges the rest.
            // ([[gd-unseeded-field-safe-when-zero-is-old-behaviour]] -- here 0
            // IS a lie, so the default has to be 1.)
            rideLanded = 1;
            break;
        }
    };
    rideAtAnchor(init.grounded, init.flip, init.y,
                 init.onSlope, init.slopeM, init.slopeT, init.rideLanded);
    if (init.dual)
        rideAtAnchor(init.grounded2, init.flip2, init.y2,
                     init.onSlope2, init.slopeM2, init.slopeT2,
                     init.rideLanded2);
    // ...and the same default for a ride that reached the anchor by any other
    // route. rideAtAnchor only reconstructs one for a GROUNDED body, but the
    // seat sets grounded only when the landing is not a flipped rider on a
    // ramp's top -- so a hanging ride arrives here with onSlope set and
    // grounded clear, and rideAtAnchor never sees it. Seeding only inside that
    // lambda left lv16 t=13,000 short (264 -> 117 ticks) with quick_regress
    // still red after the first attempt. An anchor cannot know whether the ride
    // landed, and 0 means "it did not", so every anchored ride takes the
    // pre-change behaviour and the loop's GD replay judges it.
    if (init.onSlope) init.rideLanded = 1;
    if (init.dual && init.onSlope2) init.rideLanded2 = 1;
    // --anchorride: p1's ride as GD had it (the hist payload's version 4), in place of
    // the guess above. GD keeps ONE clock per ride (+0x598, stamped on the air -> ramp
    // transition and kept across a chain, and across a floor ride handed straight to an
    // underside contact) and one bit (+0x9b0) that does not tell a floor ride from an
    // underside press. The model splits that into three counters, and exactly one of
    // them takes the age, by the side and by whether the ride has landed:
    //   underside (slopeUnder = 1)     ceilT = min(200, A+1), ceilM4 = round(|m|*4),
    //                                  ceilMode = the anchor's mode, no floor ride
    //   floor side, landed             onSlope = 1, slopeT = min(24, A), rideLanded = 1,
    //                                  slopeM = the ramp's m (as the ride stores it),
    //                                  slopeUid0 = slopeUidNow = the ramp
    //   floor side, not landed (seat)  seatT = min(24, A+1), no floor ride
    // A counts ticks from the contact tick, 0 there: the model's slopeT is 0 on that
    // tick and seatT / ceilT are 1. The payload's slopeAge is GD's clock difference,
    // which the recording point reads as 1 on the contact tick (cfg slopetrace), hence
    // kRideAgeLag.
    // slopeUid0 is GD's CURRENT ramp: the one the ride began on is not in the payload,
    // and the two differ only for an anchor past a seam of a chain.
    // Nothing is written off a ramp, for a value not said (-1), a uid that is not a
    // ramp in this level, a rotated frame (the model's line is the turned one), or a
    // floor ride on a wave (the model's wave never rides): the guess above stands.
    if (g_anchorRide && t0 > 0) {
        constexpr int kRideAgeLag = 1;
        auto histVal = [&](const char* name) {
            for (const auto& h : payloadHist) if (h.first == name) return h.second;
            return -1;
        };
        const int on = histVal("slopeOn"), under = histVal("slopeUnder");
        const int uid = histVal("slopeUid"), age = histVal("slopeAge");
        const int landed = histVal("slopeLanded");
        const Obj* ramp = nullptr;
        if (uid >= 0) {
            for (const Obj& sp : L.slopes)
                if (sp.uid == uid) { ramp = &sp; break; }
            if (!ramp)
                for (const Obj& o : L.dyn.objs)
                    if (o.slope && o.uid == uid) { ramp = &o; break; }
        }
        const double m = (ramp && ramp->hw > 0.0)
            ? (ramp->sy1 - ramp->sy0) / (2.0 * ramp->hw) : 0.0;
        const char* skip = nullptr;
        if (!g_ownsHist || histVersion < 4) skip = "no version-4 hist payload";
        else if (on != 1) skip = on == 0 ? "off a ramp (slopeOn=0)" : "slopeOn not said";
        else if (under < 0 || age < 0) skip = "slopeUnder / slopeAge not said";
        else if (!ramp) skip = "the uid is not a ramp in this level";
        else if (m == 0.0) skip = "the ramp is flat";
        else if (init.frame != 0) skip = "a rotated frame";
        else if (under == 0 && landed < 0) skip = "slopeLanded not said";
        else if (under == 0 && init.mode == 4) skip = "a floor ride on a wave";
        if (skip) {
            std::printf("anchorride: nothing seeded, %s (slopeOn=%d slopeUnder=%d "
                        "slopeUid=%d slopeAge=%d slopeLanded=%d)\n",
                        skip, on, under, uid, age, landed);
        } else {
            const int A = std::max(0, age - kRideAgeLag);
            const int wasOn = init.onSlope, wasT = init.slopeT;
            const char* regime = nullptr;
            if (under == 1) {
                regime = "underside";
                init.onSlope = 0; init.slopeT = 0; init.rideLanded = 0; init.slopeM = 0.f;
                init.seatT = 0;
                init.ceilT = (uint8_t)std::min(200, A + 1);
                init.ceilM4 = (uint8_t)std::min<long>(255, std::lround(std::fabs(m) * 4.0));
                init.ceilMode = init.mode;
            } else if (landed == 1) {
                regime = "floor-landed";
                init.onSlope = 1;
                init.slopeT = (uint8_t)std::min(24, A);
                init.rideLanded = 1;
                init.slopeM = (float)m;
                init.slopeUid0 = uid;
                init.slopeUidNow = uid;
                init.seatT = 0;
                init.ceilT = 0; init.ceilM4 = 0;
            } else {
                regime = "floor-seat";
                init.onSlope = 0; init.slopeT = 0; init.rideLanded = 0; init.slopeM = 0.f;
                init.seatT = (uint8_t)std::min(24, A + 1);
                init.ceilT = 0; init.ceilM4 = 0;
            }
            std::printf("anchorride: %s uid=%d m=%.4f ceilRamp=%d flip=%d mode=%d "
                        "slopeAge=%d A=%d slopeLanded=%d -> onSlope=%d slopeT=%d "
                        "rideLanded=%d slopeM=%.4f seatT=%d ceilT=%d ceilM4=%d ceilMode=%d "
                        "(the geometric guess had onSlope=%d slopeT=%d)\n",
                        regime, uid, m, slopeIsCeiling(ramp->slopeDir) ? 1 : 0,
                        (int)init.flip, (int)init.mode, age, A, landed,
                        (int)init.onSlope, (int)init.slopeT, (int)init.rideLanded,
                        (double)init.slopeM, (int)init.seatT, (int)init.ceilT,
                        (int)init.ceilM4, (int)init.ceilMode, wasOn, wasT);
        }
    }
    // --ridebox: an anchor INSIDE a riding arm box's ride. --start carries no 2866 arm and
    // the 1859's field 29 is usually not said, and under the ride rule (modifiers.hpp,
    // g_rideBox) a state that is not armed inside the ride stays so to its end. The loop
    // gives an anchored call the recording of the anchor's own attempt, so the box's
    // recorded place on the first tick after the anchor says whether THIS state rides with
    // it: overlapping there = the arm was fresh at the anchor. Frame 0 only (lv22's rides
    // are); a turned anchor is left unarmed and says so.
    if (t0 > 0) {
        const int t1 = (int)t0 + 1;
        const double half = playerHalf(init.mode, init.mini != 0);
        auto over = [&](double bx, double by, double hw, double hh) {
            return init.frame == 0 && std::fabs(x0World - bx) <= hw + half
                   && std::fabs((double)init.y - by) <= hh + half;
        };
        for (const FlipHeadBox& b : g_flipHeadBoxes) {
            if (rideStage(b, t1) != 1) continue;
            double bx, by;
            flipHeadAt(b, t1, bx, by);
            const bool on = over(bx, by, b.hw, b.hh);
            if (on) init.fgArm = (uint8_t)kArmTicks;
            std::printf("ridebox: anchor t=%lld inside fliphead uid %d's ride - %s "
                        "(box %.2f,%.2f, frame %d)\n", t0, b.uid,
                        on ? "riding with it, armed" : "not overlapping it, unarmed", bx, by,
                        (int)init.frame);
        }
        for (const ArmBox& b : g_armBoxes) {
            if (rideStage(b, t1) != 1) continue;
            double bx, by;
            armBoxAt(b, t1, bx, by);
            const bool on = over(bx, by, b.hw, b.hh);
            if (on && init.armT >= (uint8_t)kArmTicks) init.armT = 0;
            std::printf("ridebox: anchor t=%lld inside ceilarm uid %d's ride - %s "
                        "(box %.2f,%.2f, frame %d, armT %d)\n", t0, b.uid,
                        on ? "riding with it, armed" : "not overlapping it, unarmed", bx, by,
                        (int)init.frame, (int)init.armT);
        }
    }
    double goalX = L.maxX + 60.0;
    // --goalgd (speed.hpp): GD's own end, the end portal's spawn x
    if (g_goalGd && g_endPortalX > 0.0 && !g_endPortalFlipX) {
        const double zoom = camScaleAt(g_endPortalX - 300.0, (double)kDx, -1);
        const double gdGoal = g_endPortalX - 300.0 / (zoom > 0.0 ? zoom : 1.0);
        std::printf("goalgd: end portal x %.2f, zoom %.3f -> goal %.2f (L.maxX + 60 = %.2f)\n",
                    g_endPortalX, zoom, gdGoal, goalX);
        goalX = gdGoal;
    }
    g_goalX = goalX;   // --goalspare (speed.hpp)
    // ---- --coins: route through the level's coins as well as to its end -----
    //
    // Two things and no more. A coin is COLLECTED when the player's own rect
    // overlaps it (hazardHalfFor, which is GD's getObjectRect half and the one
    // the coincal rig measured: 15/9 for most modes, 13.5/8.1 spider, 5/3 wave),
    // and a state that leaves an uncollected coin BEHIND IT is dead. The second
    // half is what keeps this cheap: without it the frontier fills with lineages
    // that can no longer finish, and the collected set would have to stay in the
    // key for the whole level instead of just inside a coin's window.
    //
    // Nothing here is a heuristic about where to go. The search is not steered
    // towards a coin; the goal simply is not reached without it.
    const uint32_t coinAll = g_coinRoute && !L.coins.empty()
        ? (uint32_t)((1u << L.coins.size()) - 1u) : 0u;
    if (g_coinRoute) {
        if (L.coins.size() > 8) {
            std::printf("coins: %zu is more than the 8 the mask holds -- "
                        "routing disabled\n", L.coins.size());
        } else if (L.coins.empty()) {
            std::printf("coins: none in this level\n");
        } else {
            std::printf("coins: %zu to collect, mask 0x%x\n",
                        L.coins.size(), coinAll);
            for (size_t i = 0; i < L.coins.size(); ++i)
                std::printf("  coin %zu: (%.0f,%.0f) box %.1fx%.1f\n", i,
                            L.coins[i].cx, L.coins[i].cy,
                            L.coins[i].hw * 2, L.coins[i].hh * 2);
        }
    }
    const bool coinOn = coinAll != 0 && L.coins.size() <= 8;
    // ...and whether the item gates are part of this call's world: with the coins, or alone under
    // --worldgates. Their tables (tapGive, itemBits, tapGateBits) and Model::fireGates follow this;
    // what the coins themselves need (coinEnable, the prunes, the goal) stays on coinOn.
    const bool gatesOn = coinOn || g_worldGates;
    // ---- WHAT A COIN NEEDS COUNTED BEFORE IT CAN BE HAD --------------------
    // Four of the corpus' coins sit behind a counter, and the search had no
    // reason to feed one: the only signal was the miss prune, which fires at
    // the coin. lv21's ten pickups end at x=21,455 and the coin is at 21,813,
    // so a branch that skipped the FIRST pickup (x=19,079) still flew 2,759 px
    // -- filling the frontier under a cap of 2,000 with branches that were
    // already doomed -- before anything said so.
    //
    // Only ONE shape gives a sound prune, and it is the shape the corpus has.
    // A counter never decreases, so "can this branch still reach N" is exactly
    // `have + still ahead >= N`. That is a proof, not a heuristic. The other
    // two comparisons (n == C, n > C) cannot be made unreachable by running out
    // of feeders, so they get no prune here and are left to the miss test.
    //
    // WHICH WAY the trigger decides is read off the offset its chain gives the
    // coin, not assumed: if firing moves the coin further than the collect
    // bound, firing is what loses it, and the requirement is the negation of
    // the trigger's own test. lv21: uid 21783 is `item 1 < 10` and its chain
    // drops the coin 120 px against a bound of 25, so the coin needs ten.
    // (struct CoinCountReq: search_dp.hpp)
    std::vector<CoinCountReq> coinReq;
    if (coinOn) {
        for (const TouchTrig& T : g_touch) {
            if (T.count < 0 || T.cmode != 2) continue;   // only `n < C` bites
            for (const TrigCtl& c : T.ctl) {
                int ci = -1;
                for (size_t i = 0; i < L.coins.size(); ++i)
                    if (L.coins[i].uid == c.uid) { ci = (int)i; break; }
                if (ci < 0) continue;
                const double reach = L.coins[(size_t)ci].hh + 15.0;
                if (std::fabs((double)c.dy) <= reach) continue;   // not decisive
                coinReq.push_back({T.item, T.count, ci});
                std::printf("coins: coin %d needs item %d to reach %d before it"
                            " (uid %d moves it %.0f px, reach %.0f)\n",
                            ci, T.item, T.count, c.uid, (double)c.dy, reach);
            }
        }
    }
    // ---- ...AND THE COIN ITS GROUP CAN BE SWITCHED OFF ---------------------
    // lv20's first coin is the other shape, and it has no counter at all --
    // the level holds no pickups. A Toggle at x=6,465 switches its group off on
    // the crossing, and a TOUCH-TRIGGERED one at (7,003, 405.5) switches it
    // back on; the coin is at x=15,631. So the only thing that could have saved
    // the branch is 8,628 px behind by the time the miss test speaks.
    //
    // Sound for the same reason as the counter, and gated on the same fact: x
    // only grows in frame 0, so once every switch-on box is behind the player
    // and none of them has fired, that coin can never come back. The test also
    // asks whether the coin is off RIGHT NOW (it is absent from the live rows
    // when its group is) -- without that, a coin that is on by default and
    // merely has an enabling Toggle somewhere would be pruned for never having
    // needed it.
    //
    // Only a Toggle that is the ROOT of its chain is seen here (togOn is read
    // off the root row). A switch-on nested inside a spawn chain would not be,
    // and the branch would keep the old behaviour -- late, not wrong.
    // ---- WHAT A FIRED CHAIN ADDS TO AN ITEM COUNTER ------------------------
    // The counter used to be "collectibles the player has physically touched"
    // (g_collect, which loadCollectibles fills from the rows with pickup=1).
    // On lv22's first coin that counter can never move: the gate is
    // `item 2 == 5`, and all three rows that give item 2 have pickup=0 --
    // they are Pickup triggers (1817) and a collectible (1816) that a TOUCH BOX
    // spawns, so nothing is ever touched. The gate could not fire, the coin was
    // never raised, and the model was not choosing the low lane over the high
    // one: IT HAD NO HIGH LANE. Closing the low band with --deadband made it go
    // lower still (under the coin, between the two spikes) rather than up.
    //
    // So count what the FIRED CHAINS give as well. The per-bit table is static:
    // g_itemGiver holds every row that names an item, touchable or not, and a
    // box's ctl list is what its chain reaches. No new state -- s.trig already
    // says which boxes have fired.
    //
    // MULTIPLICITY IS TAKEN FROM THE CTL LIST, and the game agrees on both
    // boxes it was asked about. A uid reached by two paths appears twice there,
    // and whether GD's counter moves once or twice for that is the game's
    // answer to give (cfg coins=1 prints `itemcnt:`). Dropped onto each box in
    // turn, lv22 says:
    //
    //   box uid 18090   weight 1   itemcnt v=1, still 1 fourteen ticks later
    //   box uid 2195    weight 2   itemcnt v=1 at the touch, v=2 at +25 ticks
    //
    // THE SECOND INCREMENT IS LATE, which a first reading missed: that box was
    // measured on a run that died nine ticks in and reported 1, and a walk of
    // the same chains written independently said 3 for the first box. Both were
    // wrong and the ctl list was right -- so the weights are measured, and the
    // number to distrust is a counter read from a run that ended too early.
    //
    // The line below prints what the model derives, so any level can be put
    // beside its `itemcnt:` the same way. On lv22 nine boxes feed item 2, 13 in
    // total, against a gate of 5. Under --itemsnoblock five boxes, 5 in total: the
    // other eight were the Collision Block uid 18154 on the ceiling every one of
    // those chains moves (g_itemsNoBlock) -- and GD's counter, taken on a pass
    // rather than a drop, agrees with the five.
    std::unordered_map<int, TouchMask> itemBits;   // item -> bits that give it
    std::vector<std::vector<std::pair<int, int>>> bitItemGive(g_touch.size());
    // ---- A TAP THAT COUNTS PRESSES -----------------------------------------
    // A fired box gives its items once, and s.trig says so. A counting TAP
    // (TouchTrig::tapCloseX) gives them on every press, which a bit cannot
    // hold: lv22's third coin opens at item 1 >= 6, six presses of the box at
    // x=15,525 before the Stop at x=15,765 (six confirmed in the game). So its
    // gift is taken out of the per-bit table and counted in State::taps
    // instead. One such tap per call -- State::taps is a single number; a
    // second one keeps the fire-once reading and says so.
    struct TapGive {
        TouchMask bit{};
        int item = 0, per = 0;
        double armX = 0.0, closeX = 1e18;
        // GD's terms for the two ends (TouchTrig::tapChan): the channel each
        // sits on and its world point.
        int armChan = 0, closeChan = 0;
        double armY = 0.0, closeY = 0.0;
        // --spawnroots: a Tap a box spawns opens when that box fires (not at armX), and one
        // with no Stop past it stays open (closeX stays at 1e18).
        TouchMask armBy{};
        // --tapclosebox: the Stop is a touch box and shuts the window on rect overlap
        // (TouchTrig::tapCloseTouch; closeHw/closeHh its half sizes, closeUid its uid).
        bool closeBox = false;
        double closeHw = 0.0, closeHh = 0.0;
        int closeUid = -1;
    };
    TapGive tapGive;   // bit 0 = none
    // --tap2: the counting Taps after the first (State::tapsX[i] counts tapExtra[i]); empty without the flag
    std::vector<TapGive> tapExtra;
    // --tapfeed: the items a Count reads at a count above 1 -- a Tap that feeds one of them counts presses even with no
    // Stop past it (it stays open: a Tap is open until a Stop shuts it)
    std::unordered_set<int> multiCountItems;
    for (const auto& tb : g_touch)
        if (tb.count >= 2 && tb.item != 0) multiCountItems.insert(tb.item);
    if (gatesOn && !g_itemGiver.empty()) {
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            std::vector<std::pair<int, int>>& v = bitItemGive[b];
            for (const TrigCtl& c : g_touch[b].ctl) {
                const auto gi = g_itemGiver.find(c.uid);
                if (gi == g_itemGiver.end()) continue;
                // ...but NOT the ones the player has to run into. Those are in
                // g_collect and are counted from s.items, so counting them here
                // as well would credit a branch for a collectible it never
                // touched -- and credit it twice if it did. No corpus level
                // does this today (lv20 has no items at all and no chain on
                // lv21 reaches one of its eleven pickups, so this loop is empty
                // there), which is exactly why it would have gone unnoticed.
                bool isPickup = false;
                for (const auto& col : g_collect)
                    if (col.uid == c.uid) { isPickup = true; break; }
                if (isPickup) continue;
                bool seen = false;
                for (auto& p : v)
                    if (p.first == gi->second) { ++p.second; seen = true; break; }
                if (!seen) v.push_back({gi->second, 1});
            }
            if (g_touch[b].tap && v.size() == 1
                && (g_touch[b].tapCloseX < 1e17 || (g_touch[b].armBy) || multiCountItems.count(v[0].first))) {
                if (!tapGive.bit) {
                    tapGive.bit = touchBit((int)b);
                    tapGive.armBy = g_touch[b].armBy;
                    tapGive.item = v[0].first;
                    tapGive.per = v[0].second;
                    tapGive.armX = g_touch[b].cx;
                    tapGive.armY = g_touch[b].cy;
                    tapGive.armChan = g_touch[b].tapChan;
                    tapGive.closeX = g_touch[b].tapCloseX;
                    tapGive.closeY = g_touch[b].tapCloseY;
                    tapGive.closeChan = g_touch[b].tapCloseChan;
                    tapGive.closeBox = g_touch[b].tapCloseTouch
                                       && g_touch[b].tapCloseX < 1e17;
                    tapGive.closeHw = g_touch[b].tapCloseHw;
                    tapGive.closeHh = g_touch[b].tapCloseHh;
                    tapGive.closeUid = g_touch[b].tapCloseUid;
                    std::printf("items: tap %zu uid %d gives item %d x%d PER PRESS,"
                                " open from (%.0f,%.0f) on channel %d to the Stop at"
                                " (%.0f,%.0f) on channel %d\n", b, g_touch[b].uid,
                                tapGive.item, tapGive.per, tapGive.armX, tapGive.armY,
                                tapGive.armChan, tapGive.closeX, tapGive.closeY,
                                tapGive.closeChan);
                    if (tapGive.closeBox)
                        std::printf("items: --tapclosebox: the Stop uid %d is a touch box "
                                    "%.1fx%.1f, and shuts the window on the first tick the "
                                    "player's rect overlaps it\n", tapGive.closeUid,
                                    2.0 * tapGive.closeHw, 2.0 * tapGive.closeHh);
                    v.clear();
                    continue;
                }
                if (tapExtra.size() < 3 && tapExtra.size() + 1 < (size_t)g_tap2Max) {
                    TapGive g;
                    g.bit = touchBit((int)b);
                    g.armBy = g_touch[b].armBy;
                    g.item = v[0].first;
                    g.per = v[0].second;
                    g.armX = g_touch[b].cx;
                    g.armY = g_touch[b].cy;
                    g.armChan = g_touch[b].tapChan;
                    g.closeX = g_touch[b].tapCloseX;
                    g.closeY = g_touch[b].tapCloseY;
                    g.closeChan = g_touch[b].tapCloseChan;
                    g.closeBox = g_touch[b].tapCloseTouch && g_touch[b].tapCloseX < 1e17;
                    g.closeHw = g_touch[b].tapCloseHw;
                    g.closeHh = g_touch[b].tapCloseHh;
                    g.closeUid = g_touch[b].tapCloseUid;
                    tapExtra.push_back(g);
                    std::printf("items: --tap2: tap %zu uid %d is counting tap #%zu, gives item %d x%d PER PRESS,"
                                " open from (%.0f,%.0f) on channel %d to the Stop at (%.0f,%.0f) on channel %d\n", b,
                                g_touch[b].uid, tapExtra.size() + 1, g.item, g.per, g.armX, g.armY, g.armChan,
                                g.closeX, g.closeY, g.closeChan);
                    v.clear();
                    continue;
                }
                std::printf("items: tap %zu uid %d also counts presses -- only one"
                            " is held per call, so it gives once\n", b,
                            g_touch[b].uid);
            }
            for (const auto& p : v) itemBits[p.first] |= touchBit((int)b);
            if (!v.empty()) {
                std::printf("items: box %zu uid %d (x=%.0f) gives", b,
                            g_touch[b].uid, g_touch[b].cx);
                for (const auto& p : v)
                    std::printf(" item %d x%d", p.first, p.second);
                std::printf("\n");
            }
        }
    }
    // --tapclosebox: an anchor after the attempt entered the window's touch-box Stop starts with
    // the window shut (--touchentered uid:tick). Its box is behind the player by then, so no
    // overlap in the step can shut it, and a press past it would count where GD counts none.
    if (tapGive.closeBox && t0 > 0 && g_touchEnteredGiven) {
        const auto et = g_touchEnteredT.find(tapGive.closeUid);
        if (et != g_touchEnteredT.end() && et->second > 0 && (long long)et->second <= t0) {
            init.taps = 0x40;
            std::printf("items: --tapclosebox: the Stop uid %d was entered at t=%d <= t0, so "
                        "the window starts shut\n", tapGive.closeUid, et->second);
        }
    }
    // s.trig -> how much this state has fed `item`. The mask makes the common
    // case (no box for this item has fired) a single test.
    auto chainItems = [&itemBits, &bitItemGive](TouchMask trig, int item) -> int {
        const auto mi = itemBits.find(item);
        if (mi == itemBits.end() || !(trig & mi->second)) return 0;
        int add = 0;
        for (size_t b = 0; b < bitItemGive.size(); ++b) {
            if (!(trig & touchBit((int)b))) continue;
            for (const auto& p : bitItemGive[b])
                if (p.first == item) add += p.second;
        }
        return add;
    };
    // The Item Compare gates (cmode 3) that read the counting tap's item --
    // once one has fired, further presses change nothing -- and the coins each
    // gate switches on. A gated coin is live only in a state whose gate has
    // fired (coinLive below): the recording's own on/off belongs to the run
    // that recorded it, and lv22's recordings are of runs that never pressed
    // six times, so they hold the third coin off for every state. The chain
    // is read as switching the coin ON; lv22's reaches it through Toggle
    // uid17968 (activate 1), and no corpus gate reaches a coin any other way.
    TouchMask tapGateBits{};
    // --tap2: the boxes of a Count / Item Compare on an extra tap's item. One of them fired makes that tap's presses moot.
    std::vector<TouchMask> tapExtraGate(tapExtra.size(), TouchMask{});
    for (size_t e = 0; e < tapExtra.size(); ++e)
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
            if (g_touch[b].count >= 0 && !g_touch[b].tap && g_touch[b].item == tapExtra[e].item)
                tapExtraGate[e] |= touchBit((int)b);
    // ...and the largest count any Count / Item Compare asks of that item: a tap's presses past it change no answer (cmode 1 "larger than C"
    // needs C + 1, the others C), so the count stops there and the states that pressed more are one world, not up to 63.
    std::vector<int> tapExtraCap(tapExtra.size(), 0x3f);
    for (size_t e = 0; e < tapExtra.size(); ++e) {
        int mx = 0;
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
            if (g_touch[b].count >= 0 && !g_touch[b].tap && g_touch[b].item == tapExtra[e].item) mx = std::max(mx, g_touch[b].count);
        if (mx > 0) tapExtraCap[e] = std::min(0x3f, mx + 1);
    }
    std::vector<TouchMask> coinEnable(L.coins.size(), TouchMask{});
    if (gatesOn)
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            const TouchTrig& T = g_touch[b];
            if (T.count < 0 || T.cmode != 3) continue;
            if (tapGive.bit && T.item == tapGive.item) tapGateBits |= touchBit((int)b);
            if (!coinOn) continue;   // (the coins it switches on are the route's)
            for (const TrigCtl& c : T.ctl)
                for (size_t ci = 0; ci < L.coins.size(); ++ci)
                    if (L.coins[ci].uid == c.uid && !(coinEnable[ci] & touchBit((int)b))) {
                        coinEnable[ci] |= touchBit((int)b);
                        std::printf("coins: coin %zu is switched on by Item Compare uid %d"
                                    " (item %d at least %d)\n",
                                    ci, T.uid, T.item, T.count);
                    }
        }
    // ...and a coin a Toggle inside a touch box's chain switches on
    // (TouchTrig::togOnUids). The same shape and the same reason as the gate
    // above: without it the coin's on/off is whatever the RECORDING holds, and a
    // recording of a run that never entered the box holds it off for every
    // state -- so the model could not collect it from any branch. SubZero 4002's
    // first coin: the Spawn uid 500 at (1661,263) is touched, spawns group 679,
    // and its Toggle uid 505 switches the coin's group 678 on. Measured in the
    // game by teleporting onto each coin: the first (group off) awards nothing,
    // the second (group on) awards on contact.
    //
    // NOT for a Tap or a Count root: there the COUNTER decides, and the box's
    // own bit is set by the first press. lv22's third coin is reached by a
    // Toggle in the tap box uid 17958's chain and needs six presses, so reading
    // the Toggle there would switch it on five presses early -- the same trap
    // the walk already avoids by treating an Item Compare as a root of its own.
    // ...EXCEPT a Count a box spawns (--spawnroots, TouchTrig::armBy): its bit is set only
    // when its counter condition holds (itemGates), so the Toggle in its chain is exactly
    // when the coin comes on. SubZero 4003's third coin: box 6102 spawns the Count
    // `item 1 == 4`, whose Toggle switches the coin on.
    if (coinOn)
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            if (g_touch[b].tap) continue;
            if (g_touch[b].count >= 0 && !(g_touch[b].armBy)) continue;
            for (const int u : g_touch[b].togOnUids)
                for (size_t ci = 0; ci < L.coins.size(); ++ci)
                    if (L.coins[ci].uid == u
                        && !(coinEnable[ci] & touchBit((int)b))) {
                        coinEnable[ci] |= touchBit((int)b);
                        std::printf("coins: coin %zu is switched on by the Toggle in"
                                    " touch box uid %d's chain\n", ci, g_touch[b].uid);
                    }
        }
    // ...and which of those coins can be collected in a TURNED frame too. The
    // collect test runs in frame 0 because a moving coin's position there is
    // read off the dyn rows; a coin nothing moves has one world position, and
    // toFrame turns it into any frame exactly as it turns the geometry.
    // lv22's third coin needs this: GD's own runs pass it at y=506 in frame 0,
    // 175 px under it, and come back up through x=16,090..16,131 in frame 3.
    // "Nothing moves it" = no touch chain gives it an offset, a duration, a
    // lock or a target-mode move; its group (537) is otherwise only toggled.
    std::vector<uint8_t> coinAnyFrame(L.coins.size(), 0);
    for (size_t ci = 0; ci < L.coins.size(); ++ci) {
        if (!coinEnable[ci]) continue;
        bool moved = false;
        for (size_t b = 0; b < g_touch.size() && !moved; ++b)
            for (const TrigCtl& c : g_touch[b].ctl)
                if (c.uid == L.coins[ci].uid
                    && (c.dx != 0.f || c.dy != 0.f || c.durTicks != 0.0
                        || c.lockTicks != 0.0 || c.lockYTicks != 0.0 || c.tmode != 0)) {
                    moved = true;
                    break;
                }
        if (moved) continue;
        coinAnyFrame[ci] = 1;
        std::printf("coins: coin %zu never moves, so it is collected in any frame\n", ci);
    }
    // Whether the counting tap is the only thing that feeds its item. Then a
    // window that shut with the gate unfired is final (the prune in the step).
    bool haveCmpGate = false;
    for (const TouchTrig& T : g_touch)
        if (T.count >= 0 && T.cmode == 3) haveCmpGate = true;
    // --tapdisarm: a Tap the flag made a box without the gates (triggers.hpp isTapDisarm) is fired by the
    // press below as well, so the gates are plugged in for it alone.
    bool tapDisarmBox = false;
    if (g_tapDisarm && !gatesOn)
        for (const TouchTrig& T : g_touch)
            if (T.tap) tapDisarmBox = true;
    bool tapSole = tapGive.bit.any() && !itemBits.count(tapGive.item);
    for (const auto& col : g_collect)
        if (col.item == tapGive.item) tapSole = false;
    // ---- WHEN THE COUNTING TAP'S WINDOW IS OPEN, IN GD'S TERMS -------------
    // A channel's direction is the one the queue print above names
    // (rotQDirection of the 2900 that switches TO it: 1 y+, 2 y-, 3 x-, else
    // x+), and a point is PASSED once the player's WORLD coordinate on that
    // axis has reached it in that direction -- the comparison step.hpp's queue
    // walk makes for the 2900s themselves. Passed now (the channel is active and
    // the player is there) or earlier, which the queue's own record keeps: an
    // entry of that channel lying at or beyond the point has been consumed.
    // That second reading is what lets an anchored call inside the window know
    // it is open without having seen it open.
    int chanDir[16];
    for (int ch = 0; ch < 16; ++ch) {
        chanDir[ch] = 4;
        for (const RotQEntry& e : g_rotQ)
            if (e.swarm && e.swch == ch && e.rotIdx >= 0) {
                chanDir[ch] = rotQDirection(g_rotTrig[(size_t)e.rotIdx].rawRot,
                                            g_rotTrig[(size_t)e.rotIdx].flipX != 0);
                break;
            }
    }
    const bool tapQueue = g_rotQueue && !g_rotQ.empty();
    auto reachedOn = [&chanDir](int ch, double wx, double wy, double px, double py) {
        switch (chanDir[ch & 15]) {
            case 1: return wy >= py;
            case 2: return wy <= py;
            case 3: return wx <= px;
            default: return wx >= px;
        }
    };
    // ...and WHERE SUCH A COIN IS PASSED FOR GOOD. The ordinary miss test is
    // withheld on a level that turns (coinPruneOk), and on lv22 rightly so for
    // the third coin in general: the maze brings the player back past it. But
    // the coin can only be had after its window, and past the shut point the
    // way on is the shut channel's run -- lv22's channel 10, x+, on which the
    // ship glides under the coin at y=615 and on (GD's own rows, t=14,250..
    // 14,580). So a coin lying AHEAD of the shut point in that channel's
    // direction is missed once the player is past its far edge on that same
    // channel. `gatedMask` is those coins; `tapMissX/Y` the far-edge point.
    uint8_t gatedMask = 0;
    std::vector<double> tapMissX(L.coins.size(), 0.0), tapMissY(L.coins.size(), 0.0);
    if (tapGive.bit)
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            if (!(coinEnable[ci] & tapGateBits) || ((g_coinSkip >> ci) & 1)) continue;
            const Obj& c = L.coins[ci];
            const int d = chanDir[tapGive.closeChan & 15];
            // the coin has to be ahead of the shut point on that run
            if (!reachedOn(tapGive.closeChan, c.cx, c.cy, tapGive.closeX, tapGive.closeY))
                continue;
            gatedMask |= (uint8_t)(1u << ci);
            const double m = 15.0;   // the largest half any mode has, as the mod's miss
            tapMissX[ci] = d == 3 ? c.cx - c.hw - m : (d == 1 || d == 2) ? c.cx : c.cx + c.hw + m;
            tapMissY[ci] = d == 1 ? c.cy + c.hh + m : d == 2 ? c.cy - c.hh - m : c.cy;
        }
    // ...and hand both points to the mod (Outcome::coinGates), so a flown
    // attempt that passes either -- the shut point with the count short, or the
    // coin's far edge without it -- ends there the way one that passes a missed
    // coin does. Only for a coin this run still wants.
    if (tapGive.bit && tapSole)
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            const TouchMask gates = coinEnable[ci] & tapGateBits;
            if (!gates || ((g_coinSkip >> ci) & 1)) continue;
            int need = 1 << 30;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
                if (gates & touchBit((int)b)) need = std::min(need, g_touch[b].count);
            const bool miss = ((gatedMask >> ci) & 1) != 0;
            char e[224];
            std::snprintf(e, sizeof e, "%s%d,%d,%.3f,%.3f,%d,%d,%d,%d,%.3f,%.3f",
                          g_outcome.coinGates.empty() ? "" : ";", L.coins[ci].uid,
                          tapGive.closeChan, tapGive.closeX, tapGive.closeY,
                          chanDir[tapGive.closeChan & 15], tapGive.item, need,
                          miss ? 1 : 0, tapMissX[ci], tapMissY[ci]);
            g_outcome.coinGates += e;
        }
    auto passedOn = [&](const State& st, int frame, int ch, double wx, double wy,
                        double px, double py) {
        // No queue in this call: only channel 0 exists, and it is the plain
        // frame-0 crossing the fire-once tap already uses.
        if (!tapQueue) return ch == 0 && frame == 0 && wx > px;
        if ((st.rotChan & 15) == (ch & 15) && reachedOn(ch, wx, wy, px, py)) return true;
        const int beg = g_rotQBeg[(size_t)(ch & 15)], end = g_rotQEnd[(size_t)(ch & 15)];
        for (int k = beg; k < end && k < 32; ++k)
            if (((st.rotSpent >> k) & 1u)
                && reachedOn(ch, g_rotQ[(size_t)k].px, g_rotQ[(size_t)k].py, px, py))
                return true;
        return false;
    };
    // ---- THE ITEM GATES: one function for the search AND the walks ---------
    // The pickups, the Count triggers they and the fired chains feed, the Tap
    // triggers and the counting tap: every bit that the item counter or a press
    // sets, rather than the player's box entering one. markTouched (step.hpp)
    // skips all of them, and until this was a function only the search's step ran
    // this code, so a walk of the same plan (--replay, the witness resim) never
    // fired a Count gate. On lv22 the search planned under a ceiling `item 2 == 5`
    // had lifted while its own resim walked under the unlifted one (ledger
    // 2026-09-24 02:51). The search calls it where the code used to be; the walks
    // call it under --walkgates (g_walkGates).
    // `ks`: the state after the step and any rotation. `prevAction`: the input
    // the tick started with (a Tap fires on the press EDGE). `gframe`: the frame
    // the step started in; px/py/ph: the travel coordinates and hazard half in
    // it; `t`: the step's tick.
    auto itemGates = [&](State& ks, uint8_t prevAction, int input, const StepCtx& K,
                         int gframe, double px, double py, double ph, long long t) {
        // Frame 0 only, as everything here was before the coin collect learnt
        // the turned frames: the pickups are world rows, and the counters are
        // asked where they always were. The exception is an Item Compare gate
        // (cmode 3): it reads a count and nothing else, and lv22's third coin is
        // collected in frame 3, where an anchored call has to open it from GD's
        // own counter (--itembase) or never see the coin at all.
        if ((gframe == 0
             && (!g_collect.empty() || !itemBits.empty() || tapGive.bit || tapDisarmBox))
            || (gframe != 0 && haveCmpGate)) {
            for (size_t ii = 0; gframe == 0 && ii < g_collect.size() && ii < 16; ++ii) {
                const uint16_t ibit = (uint16_t)(1u << ii);
                if (ks.items & ibit) continue;
                const Collectible& C = g_collect[ii];
                if (std::fabs(px - C.cx) <= C.hw + ph - kCoinMargin
                    && std::fabs(py - C.cy) <= C.hh + ph - kCoinMargin) {
                    // --coindbg: and the same line for the pickups. GD prints
                    // `itemcnt:` per increment, so a replay of one plan through
                    // both sides says whether the two take the SAME ones --
                    // which is what an --itembase of 1 against a model that
                    // needs ten turns on.
                    if (g_coinDbgT >= 0 && !g_itemSaid[ii]) {
                        g_itemSaid[ii] = 1;
                        std::printf("itemcollect: t=%lld item=%d uid=%d"
                                    " player=(%.3f,%.3f) obj=(%.3f,%.3f)"
                                    " |dx|=%.3f |dy|=%.3f bound=(%.3f,%.3f)"
                                    " mode=%d mini=%d\n",
                                    (long long)t, C.item, C.uid, px, py,
                                    C.cx, C.cy, std::fabs(px - C.cx),
                                    std::fabs(py - C.cy),
                                    C.hw + ph - kCoinMargin,
                                    C.hh + ph - kCoinMargin,
                                    (int)ks.mode, (int)ks.mini);
                    }
                    ks.items |= ibit;
                }
            }
            if (K.trigs)
                for (const auto& tb : *K.trigs) {
                    const TouchTrig* T = tb.first;
                    if (T->count < 0 || (ks.trig & tb.second)) continue;
                    if (T->tap) continue;   // fired below, by the press
                    // --spawnroots: a Count a box spawns listens only once that box fired.
                    if (T->armBy && !(ks.trig & T->armBy)) continue;
                    if (gframe != 0 && T->cmode != 3) continue;
                    int n = 0;
                    for (const auto& ib : g_itemBase)
                        if (ib.first == T->item) n += ib.second;
                    const int nBase = n;   // before anything this run gave
                    for (size_t ii = 0; ii < g_collect.size() && ii < 16; ++ii)
                        if ((ks.items & (uint16_t)(1u << ii))
                            && g_collect[ii].item == T->item)
                            ++n;
                    // ...and what the boxes this state has already fired gave.
                    // Without this lv22's `item 2 == 5` is unreachable: none of
                    // its givers is a pickup.
                    n += chainItems(ks.trig, T->item);
                    // ...and the counting tap's presses (tapGive).
                    if (tapGive.bit && T->item == tapGive.item)
                        n += (int)(ks.taps & 0x3f) * tapGive.per;
                    // --tap2: ...and the other counting taps' (State::tapsX)
                    for (size_t e = 0; e < tapExtra.size(); ++e)
                        if (T->item == tapExtra[e].item)
                            n += (int)(ks.tapsX[e] & 0x3f) * tapExtra[e].per;
                    // GD's own order: 0 equals, 1 larger, 2 smaller. Measured on
                    // lv21, where the third coin's gate is a pair -- uid21783
                    // "item 1 SMALLER than 10" drops the coin out of reach,
                    // uid21781 "EQUALS 10" keeps it -- which is what "collect
                    // ten and it appears" is made of.
                    // EQUALS IS "THE COUNTER PASSED THROUGH C", not "it is C on
                    // this tick". Every increment the model counts is a single +1
                    // in the game -- a pickup, or one Pickup trigger firing -- so
                    // the game's counter visits every integer on the way up, and
                    // an equals gate fires on the visit. The model sums a box's
                    // whole weight on the tick the box fires, so it can skip:
                    // lv22's four yellow blocks weighed 2 each and the count went
                    // 2,4,6,8,9 where the game's goes 1,2,3,4,5. (The second of
                    // those two was a Collision Block's block ID -- --itemsnoblock,
                    // g_itemsNoBlock -- and the "second increment 25 ticks later"
                    // it was taken for is the next box (measured 2026-09-24).)
                    // With the old `n == C` the gate `item 2 == 5` could never
                    // fire even with every feeder forced.
                    //
                    // So with unit steps from nBase, `n == C` happens at some tick
                    // iff nBase <= C <= n. Where increments really do land one per
                    // tick (lv21's eleven pickups, collected one at a time) this
                    // fires on the same tick as before; it only differs where the
                    // old test stepped over C.
                    // cmode 3 is an Item Compare's "at least", asked on every
                    // press, so it simply reads the count.
                    const bool hit = T->cmode == 3 ? (n >= T->count)
                                   : T->cmode == 1 ? (n > T->count)
                                   : T->cmode == 2 ? (n < T->count)
                                   : (nBase <= T->count && T->count <= n);
                    // --coindbg: the gate itself, once per distinct n, so "the
                    // coin never moved" can be split into "the gate was never
                    // asked" and "it was asked and said no". Without this the
                    // only witness is the coin's live row, which cannot tell the
                    // two apart.
                    if (g_coinDbgT >= 0 && T->count >= 0 && !T->tap) {
                        const int bi = touchBitIndex(tb.second);
                        const int key = std::min(n, 60) + 1;
                        if (bi >= 0 && bi < kTouchBits
                            && g_countSaid[bi].load() < key) {
                            g_countSaid[bi].store(key);
                            std::printf("countgate: t=%lld bit=%d uid=%d id=%d "
                                        "item=%d cmode=%d need=%d n=%d base=%d "
                                        "px=%.1f hit=%d\n",
                                        (long long)t, bi, T->uid, T->id,
                                        T->item, T->cmode, T->count, n, nBase,
                                        px, hit ? 1 : 0);
                        }
                    }
                    if (!hit) continue;
                    // An INSTANT Count (1811) asks the question once, as the
                    // player crosses it; a Count (1611) is a standing listener.
                    // Without the crossing test the smaller-than ones fire at t=0
                    // for every state (0 < 10 is true from the start) and lv21's
                    // coin drops before the level has begun.
                    if (T->id == 1811 && px < T->cx) continue;
                    ks.trig |= tb.second;
                    ks.trigT = (int32_t)t;
                    const int b = touchBitIndex(tb.second);
                    if (g_touchFireT[b] < 0) g_touchFireT[b] = (int)t;
                    ks.fireB[b] = (uint16_t)t;
                }
        }
        // ---- the TAP triggers (1595) ----------------------------------------
        // Armed by crossing, fired by a press -- measured on the rig
        // calib_coingate5: a tap before the trigger's x does nothing, a tap past
        // it switches its target group. The press EDGE is what GD sees, so a
        // state already holding the button does not fire one by continuing to
        // hold.
        // --p2taps (frames.hpp): in a two-player level a Tap is fired by the SECOND player's press (measured on a custom level, a
        // Touch trigger with no dual-mode flag: a mask-1 press at t=200 leaves its door shut, mask 2 opens it from t=203), and
        // armed by player 1's x as the layer's trigger queue is. So the press edge is the other body's -- the ghost's (or a
        // replay's) second-player levels in the first body's pass, the searched body's own in the swapped pass -- and the
        // arming x is the first player's (the second slot's, in the swapped pass).
        int tapIn = input;
        uint8_t tapPrev = prevAction;
        double tapPx = px;
        if (g_p2Taps && g_twoPlayer) {
            if (g_swapBodies) {
                tapPx = px + (double)ks.dx2;
            } else {
                tapIn = secondLevelAt(t);
                tapPrev = (uint8_t)secondLevelAt(t - 1);
            }
        }
        if (K.trigs && tapIn && !tapPrev && gframe == 0)
            for (const auto& tb : *K.trigs) {
                const TouchTrig* T = tb.first;
                if (!T->tap || (ks.trig & tb.second)) continue;
                // --spawnroots: a Tap a box spawns is armed by that box, not by its x.
                if (T->armBy ? !(ks.trig & T->armBy) : (tapPx <= T->cx)) continue;
                ks.trig |= tb.second;
                ks.trigT = (int32_t)t;
                const int b = touchBitIndex(tb.second);
                if (g_touchFireT[b] < 0) g_touchFireT[b] = (int)t;
                ks.fireB[b] = (uint16_t)t;
            }
        // ...and the COUNTING tap (tapGive): every press edge while its window is
        // open adds one. State::taps holds the count in its low six bits, 0x80
        // once the window has opened and 0x40 once it has shut (see passedOn for
        // both ends). Read from tapGive rather than K.trigs, which drops a box
        // 40 px behind the player. The count is dropped once it cannot matter
        // (window shut, or a gate on its item already fired), so it splits the
        // key only inside the window. Any frame: lv22's window opens travelling
        // -x.
        if (tapGive.bit) {
            if (ks.trig & tapGateBits) {
                ks.taps = 0;
            } else {
                double wx = 0.0, wy = 0.0;
                fromFrame(gframe, (double)ks.xAbs, (double)ks.y, wx, wy);
                uint8_t tp = ks.taps;
                const bool armed =
                    (tp & 0x80) != 0
                    || (tapGive.armBy ? (bool)(ks.trig & tapGive.armBy)
                                      : passedOn(ks, gframe, tapGive.armChan, wx, wy,
                                                 tapGive.armX, tapGive.armY));
                // A tap a box arms (--spawnroots) closes where the player passes the Stop
                // in the frame it is travelling -- not through passedOn's rotation queue,
                // which reads a queue entry spent EARLIER in the run past the Stop's x as
                // "passed" and shut SubZero 4003's window the tick it opened.
                // --tapclosebox (g_tapCloseBox): a touch-box Stop shuts it where GD fires it,
                // on the first tick the player's rect overlaps the Stop's (edges inclusive, as
                // collisionCheckObjects tests). The player's rect and the pre-press y are
                // markTouched's: a spider that warps on this tick was tested by the collision
                // pass at the y it left (g_preBtnY, p1's only -- not read for a dual state,
                // whose second step may have written it). Frame 0 only, as the x test beside it.
                bool boxShut = false;
                if (tapGive.closeBox && armed && gframe == 0) {
                    const double half = playerHalf(ks.mode, ks.mini != 0);
                    const double preY = (!ks.dual && g_preBtnSet) ? g_preBtnY : wy;
                    boxShut = std::fabs(wx - tapGive.closeX) <= tapGive.closeHw + half
                              && (std::fabs(wy - tapGive.closeY) <= tapGive.closeHh + half
                                  || std::fabs(preY - tapGive.closeY) <= tapGive.closeHh + half);
                }
                const bool shut =
                    (tp & 0x40) != 0 || boxShut
                    || (!tapGive.closeBox && armed && tapGive.closeX < 1e17
                        && (tapGive.armBy ? (gframe == 0 && wx > tapGive.closeX)
                                          : passedOn(ks, gframe, tapGive.closeChan, wx, wy,
                                                     tapGive.closeX, tapGive.closeY)));
                if (shut) {
                    tp = 0x40;
                } else if (armed) {
                    tp |= 0x80;
                    if (input && !prevAction && (tp & 0x3f) < 0x3f) ++tp;
                }
                ks.taps = tp;
            }
        }
        // --tap2: the other counting taps, one window each, counted the same way. A Count on the tap's item that has fired
        // (tapExtraGate) makes the count moot, and it is dropped so it splits the key only while it can matter.
        for (size_t e = 0; e < tapExtra.size(); ++e) {
            const TapGive& g = tapExtra[e];
            if (ks.trig & tapExtraGate[e]) {
                ks.tapsX[e] = 0;
                continue;
            }
            double wx = 0.0, wy = 0.0;
            fromFrame(gframe, (double)ks.xAbs, (double)ks.y, wx, wy);
            uint8_t tp = ks.tapsX[e];
            const bool armed =
                (tp & 0x80) != 0
                || (g.armBy ? (bool)(ks.trig & g.armBy)
                            : passedOn(ks, gframe, g.armChan, wx, wy, g.armX, g.armY));
            bool boxShut = false;
            if (g.closeBox && armed && gframe == 0) {
                const double half = playerHalf(ks.mode, ks.mini != 0);
                const double preY = (!ks.dual && g_preBtnSet) ? g_preBtnY : wy;
                boxShut = std::fabs(wx - g.closeX) <= g.closeHw + half
                          && (std::fabs(wy - g.closeY) <= g.closeHh + half
                              || std::fabs(preY - g.closeY) <= g.closeHh + half);
            }
            const bool shut =
                (tp & 0x40) != 0 || boxShut
                || (!g.closeBox && armed && g.closeX < 1e17
                    && (g.armBy ? (gframe == 0 && wx > g.closeX)
                                : passedOn(ks, gframe, g.closeChan, wx, wy, g.closeX, g.closeY)));
            if (shut) {
                tp = 0x40;
            } else if (armed) {
                tp |= 0x80;
                if (input && !prevAction && (tp & 0x3f) < tapExtraCap[e]) ++tp;
            }
            ks.tapsX[e] = tp;
        }
    };
    // ---- CAN "THE PLAYER IS PAST IT" BE FINAL FOR THIS COIN? ---------------
    // Both prunes below -- the switch-on gate and the miss test -- say "behind
    // the player, so gone". That rests on the player never coming back, which
    // used to be asked of the LEVEL: no gameplay rotation anywhere, or neither
    // prune runs. lv22 turns the frame, so on the one level whose coins are
    // still missing, nothing pruned and a branch that skipped a coin flew to the
    // end for free. The level's own log says what that costs: fourteen runs
    // completed it with 0/3.
    //
    // It is a per-COIN question, and a static one. Collecting needs frame 0 (the
    // live coin rows are built only there -- in a turned frame xAbs is a
    // different axis), and inside frame 0 x only grows. So a coin is out of
    // reach for good once every LATER return to frame 0 happens further along
    // than the coin. The returns are known before the search starts: a 2900
    // carries the frame it SETS and its world position, and it is one-shot, so
    // the chain is a fixed sequence. Take the ones at or after the point the
    // coin is passed and ask for the smallest.
    //
    // The index is taken crudely -- the first rotation in chain order sitting
    // past the coin -- which can only pull in rotations that fire EARLIER than
    // the coin is passed. That makes the minimum smaller and the verdict more
    // conservative, never less.
    //
    // On lv22 this separates the three coins, and its "no" is independently
    // right: a recorded run comes back past c3 for 2,840 ticks and never returns
    // past c1 or c2, which is exactly what the rule says.
    //
    //   c1 (3,919)   smallest later return 6,875    prunable
    //   c2 (10,513)  smallest later return 15,015   prunable
    //   c3 (16,097)  smallest later return 15,015   NOT prunable
    // ...and a ROTATION is not the only way back. A ring or a pad carrying
    // property 117 (Obj::rev) turns the player round, so a coin with one of
    // those anywhere past it can still be taken on the way back, and "passing
    // it is final" is simply false. SubZero 4002: the first coin sits at
    // x=1,787 with reversing rings later in the level, and with the prune on
    // every branch that ran past it died -- the coin run gave up at x=1,911
    // (5%) while the same level clears without coins.
    const auto revPast = [&](double cx) {
        for (const std::vector<Obj>* v : {&L.orbs, &L.pads, &L.dyn.objs})
            for (const Obj& o : *v)
                if (o.rev != 0 && o.cx > cx) return true;
        return false;
    };
    std::vector<uint8_t> coinPruneOk(L.coins.size(), 0);
    if (coinOn) {
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            const double past = L.coins[ci].cx + L.coins[ci].hw + 40.0;
            if (revPast(L.coins[ci].cx)) {
                std::printf("coins: coin %zu at x=%.0f -- something past it turns "
                            "the player round, so passing it says nothing (no "
                            "miss prune)\n", ci, L.coins[ci].cx);
                // ...and the mod is told, so its own attempt cut agrees (Outcome).
                g_outcome.coinNoPrune += (g_outcome.coinNoPrune.empty() ? "" : ";")
                                         + std::to_string(L.coins[ci].uid);
                continue;
            }
            if (g_rotTrig.empty()) { coinPruneOk[ci] = 1; continue; }
            size_t idx = g_rotTrig.size();
            for (size_t j = 0; j < g_rotTrig.size(); ++j)
                if (g_rotTrig[j].cx > L.coins[ci].cx) { idx = j; break; }
            double minRe = 1e18;
            for (size_t j = idx; j < g_rotTrig.size(); ++j)
                if (g_rotTrig[j].frame == 0)
                    minRe = std::min(minRe, g_rotTrig[j].cx);
            coinPruneOk[ci] = (minRe > past) ? (uint8_t)1 : (uint8_t)0;
            if (!coinPruneOk[ci])
                g_outcome.coinNoPrune += (g_outcome.coinNoPrune.empty() ? "" : ";")
                                         + std::to_string(L.coins[ci].uid);
            if (!g_rotTrig.empty())
                std::printf("coins: coin %zu at x=%.0f -- the frame returns to 0"
                            " next at x=%.0f, %s\n", ci, L.coins[ci].cx,
                            minRe > 1e17 ? -1.0 : minRe,
                            coinPruneOk[ci] ? "so passing it is final"
                                            : "which is BEHIND it, so passing it "
                                              "says nothing (no miss prune)");
        }
    }
    // (struct CoinOnGate: search_dp.hpp)
    std::vector<CoinOnGate> coinGate;
    // Same gate as the miss test below, and for the same reason -- both rest on
    // x growing for ever, now asked per coin.
    if (coinOn) {
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            if (!coinPruneOk[ci]) continue;
            CoinOnGate g{(int)ci, {}};
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
                if (g_touch[b].togOn != 1) continue;
                bool hits = false;
                for (const TrigCtl& c : g_touch[b].ctl)
                    if (c.uid == L.coins[ci].uid) { hits = true; break; }
                if (!hits) continue;
                g.ons.push_back({touchBit((int)b),
                                 g_touch[b].cx + g_touch[b].hw + 15.0});
            }
            if (g.ons.empty()) continue;
            coinGate.push_back(std::move(g));
            std::printf("coins: coin %zu is switched on by %zu box(es), the last"
                        " at x=%.0f\n", ci, coinGate.back().ons.size(),
                        coinGate.back().ons.back().second);
        }
    }
    // ...and whether a coin the player has passed can be called missed. The
    // answer is coinPruneOk above, per coin. Say so once, with the count, so a
    // level where nothing prunes reads differently from one where the flag is
    // off: "0 of 3" is a verdict, a missing line is not.
    if (coinOn && !g_rotTrig.empty()) {
        size_t n = 0;
        for (uint8_t ok : coinPruneOk) n += ok ? 1 : 0;
        std::printf("coins: the level turns the frame -- %zu of %zu coins can "
                    "still be called missed once passed\n", n, coinPruneOk.size());
    }
    // 120 px = 4 blocks of slack above the highest surface, so a legitimate
    // arc over the top of the level is still allowed
    g_yBound = std::max(700.0, L.maxY + 120.0);
    // A turned frame's `y` is a world X (or a negated one), so the bound has to
    // cover the level's whole x extent in both signs.
    g_yBoundTurned = std::max(L.maxX, L.maxY) + 400.0;
    std::printf("  yBound=%.0f (level maxY=%.0f)\n", g_yBound, L.maxY);
    {   // suffix max of surface tops per 30 px bucket (see g_topAhead)
        double lo = 1e18, hi = -1e18;
        auto scan = [&](const std::vector<Obj>& v) {
            for (const Obj& o : v) {
                lo = std::min(lo, o.cx - o.hw);
                hi = std::max(hi, o.cx + o.hw);
            }
        };
        scan(L.objs); scan(L.pads); scan(L.orbs); scan(L.portals);
        if (lo < hi) {
            g_bucketX0 = lo;
            const size_t n = (size_t)((hi - lo) / 30.0) + 2;
            g_topAhead.assign(n, -1e18);
            auto put = [&](const std::vector<Obj>& v) {
                for (const Obj& o : v) {
                    long long i = (long long)((o.cx - lo) / 30.0);
                    if (i < 0) i = 0;
                    if (i >= (long long)n) i = (long long)n - 1;
                    g_topAhead[(size_t)i] =
                        std::max(g_topAhead[(size_t)i], o.cy + o.hh);
                }
            };
            put(L.objs); put(L.pads); put(L.orbs); put(L.portals);
            for (size_t i = n - 1; i-- > 0;)
                g_topAhead[i] = std::max(g_topAhead[i], g_topAhead[i + 1]);
            // --escdyn: the moving objects on the same grid (search_key.hpp g_topAheadDyn). L.dyn is
            // already at t0 here (seek above), so the placed top is taken from baseCy as well.
            if (!L.dyn.objs.empty()) {
                g_topAheadDyn.assign(n, -1e18);
                auto upOf = [](const std::vector<Dynamics::AutoPart>& v) {
                    double s = 0.0;
                    for (const Dynamics::AutoPart& p : v) s += std::max(0.0, (double)p.dy);
                    return s;
                };
                for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                    const Obj& o = L.dyn.objs[i];
                    double up = 0.0, s1 = 0.0, s2 = 0.0;
                    if (i < L.dyn.autoParts.size()) s1 += upOf(L.dyn.autoParts[i]);
                    if (i < L.dyn.touchParts.size()) s1 += upOf(L.dyn.touchParts[i]);
                    if (i < L.dyn.partsAny.size()) s2 = upOf(L.dyn.partsAny[i]);
                    up = std::max(s1, s2);
                    double s3 = 0.0;
                    if (i < L.dyn.autoDy.size()) s3 += std::max(0.0, (double)L.dyn.autoDy[i]);
                    if (i < L.dyn.trigDy.size()) s3 += std::max(0.0, (double)L.dyn.trigDy[i]);
                    up = std::max(up, s3);
                    double top = o.cy + o.hh + up, cxMin = o.cx;
                    if (i < L.dyn.baseCy.size())
                        top = std::max(top, (double)L.dyn.baseCy[i] + o.hh + up);
                    if (i < L.dyn.samples.size())
                        for (const DynSample& sm : L.dyn.samples[i]) {
                            top = std::max(top, (double)sm.cy + (double)sm.hh);
                            cxMin = std::min(cxMin, (double)sm.cx);
                        }
                    long long b = (long long)((cxMin - lo) / 30.0);
                    if (b < 0) b = 0;
                    if (b >= (long long)n) b = (long long)n - 1;
                    g_topAheadDyn[(size_t)b] = std::max(g_topAheadDyn[(size_t)b], top);
                }
                for (size_t i = n - 1; i-- > 0;)
                    g_topAheadDyn[i] = std::max(g_topAheadDyn[i], g_topAheadDyn[i + 1]);
                std::printf("escdyn: %zu moving objects on the escapee table\n", L.dyn.objs.size());
            }
        }
    }
    // The tick budget is the count "from x=0 to goalX at the base speed". A
    // LEVEL WITH SLOW SECTIONS EXCEEDS IT: lv22 has sections where x barely
    // advances (rotation / reverse), so for a t=18,600 anchor tEnd came out
    // as 18,602 and both the replay and the DP ENDED AFTER 1 TICK (SURVIVED
    // to t=18601 / maxAlive=1). The late-game wall that looked like a
    // coordinate-system problem was this. Starting from an anchor, allow at
    // least 8,000 ticks beyond it (max horizon 6,000 + slack). A run from the
    // start (t0=0) is as before.
    // --goalgd: ...and counted to the old goal still (L.maxX + 60): GD's end is nearer, and a budget cut to it
    // ran out short of it on a custom level (3,702 ticks against its 3,732, the level's slow start).
    long long tEnd = (long long)std::ceil(std::max(goalX, L.maxX + 60.0) / kDx) + 2;
    if (t0 > 0) tEnd = std::max(tEnd, t0 + 8000);

    const gdapprox::ShipParams SP = gdapprox::ShipParams::normal();
    const gdapprox::ShipParams SPmini = gdapprox::ShipParams::mini();
    const gdapprox::UfoParams UP = gdapprox::UfoParams::normal();
    const gdapprox::UfoParams UPmini = gdapprox::UfoParams::mini();

    // Whether the checkpoint channel has a subscriber (dp/progress.hpp): a search method publishes
    // checkpoints only then (and keeps the arena's modes for them: search_dp.hpp SearchArena).
    const bool checkWanted = g_check.enabled.load(std::memory_order_relaxed);
    // This call's checkpoints, not the previous call's -- and cleared again on the way OUT, so a
    // finished call leaves nothing behind that still looks live, and a judgement that arrives for
    // it afterwards is refused (dp/progress.hpp SearchCheckpoints::pass).
    struct CheckGuard {
        bool on;
        ~CheckGuard() { if (on) g_check.reset(); }
    } checkGuard{checkWanted};
    if (checkWanted) g_check.reset();
    // NOTE: init keeps being amended below (band seed, anchor dx). The root
    // state is taken AFTER the last amendment (a layer search's LayerWork is
    // made from init when the search starts) -- it used to be pushed here,
    // which silently dropped the seeded band and the anchor dx from the
    // whole SEARCH while the witness resim (State s = init, after the
    // amendments) kept them. Every re-anchored tail searched with NO
    // invisible ceiling: found via the lineage-vs-resim fork at lv16
    // t=14,770, where the resim clamped to the band (390/690) the search
    // never had (0/1e9), and GD then died on the plan at x=23,000.

    // (the layer searches' frontier and dedupe scratch: search_dp.hpp LayerWork)
    // The world the groups are stepped in (model.hpp): the level, the vehicle parameters, the coin
    // tables the step reads, and the per-frame slices and ramp-veto storage the groups reuse.
    ModelWorld world{L, &SP, &SPmini, &UP, &UPmini, coinOn, &coinEnable, &coinAnyFrame, {}, {}};
    // ...its item gates (itemGates above; built, like their tables, only under gatesOn) and the
    // counting tap's bits (Model::fireGates / pressMatters).
    if (gatesOn || tapDisarmBox) {
        world.gatesCtx = (void*)&itemGates;
        world.gates = [](void* ctx, State& ks, uint8_t prevAction, int input, const StepCtx& K,
                         int gframe, double px, double py, double ph, long long t) {
            (*static_cast<decltype(itemGates)*>(ctx))(ks, prevAction, input, K, gframe, px, py, ph, t);
        };
    }
    world.tapBit = tapGive.bit;
    // --tap2: the extra taps' gates, for Model::pressMatters (a press is never moot while one of their windows can still count)
    world.nTapX = (int)tapExtra.size();
    for (size_t e = 0; e < tapExtra.size() && e < 3; ++e) world.tapXGate[e] = tapExtraGate[e];
    world.tapGateBits = tapGateBits;
    // ...and the model as a search sees it (model.hpp Model): the world and its plugs.
    const Model model{world};
    // Below this many children the pool's wakeup costs more than the work.
    // A thin frontier is cheap anyway; the layers that matter run at the cap.
    const size_t kParallelMin = 64;   // (256 until the pool cut a short job into chunks of 8: a group of 32 parents is 64 children)
    std::unique_ptr<ThreadPool> pool;
    if (g_threads > 1) pool.reset(new ThreadPool(g_threads - 1));  // + this one
    // ...lent to the moving geometry's per-object loops (g_dynParTasks) for as
    // long as it exists. The guard is declared after the pool, so it takes the
    // loan back before the pool goes away -- including on every early return.
    struct DynParLoan {
        ~DynParLoan() { g_dynParTasks = nullptr; }
    } dynParLoan;
    if (pool)
        g_dynParTasks = [&pool](size_t n, const std::function<void(size_t)>& task) {
            pool->parallelTasks(n, task);
        };
    long long bestT = 0;
    double bestX = 0;
    State goalState{};
    bool solved = false;
    // Cap accounting for the driver's tier ladder (see `capstat:` below).
    // A PARTIAL that never TOUCHED the cap died of physics, not of capacity,
    // and re-running it at a higher tier is pure waste -- the driver reads
    // these to skip that.
    size_t maxAlive = 0;
    long long capHits = 0, capDropped = 0;
    // --heldcellcap: layers over the cap in states whose cells fit (kept whole), layers cut by
    // cells, and the button twins those cuts kept beyond the cap (printed only under the flag).
    long long cellSpared = 0, cellCuts = 0, cellTwinsKept = 0;
    SearchCensus census;   // --searchcensus only
    TwinStats twin;        // --twinskip / --twinaudit
    // Seed the anchor's band by replaying every mode portal already behind x0.
    // This is the one place the x-only approximation survives -- a re-anchored
    // run does not know which lane the player took, so it takes the last portal
    // by x. The search itself carries the band per state from here on.
    {
        FlyBand seed;
        // ...and replay the dual portals the same way the search does, or an
        // anchor taken inside a dual seeds the band from the mode portal's own
        // cy instead of the dual's (lv16's first dual: 390 instead of 330).
        double seedRefY = 0.0;
        bool seedDual = false;
        int seedMode = (int)init.mode;
        for (const Obj& p : L.portals) {
            if (p.cx > x0) break;
            if (p.type == 23 || p.type == 24) {
                seedDual = (p.type == 23);
                if (seedDual) {
                    seedRefY = p.cy;
                    seed = bandFor(seedRefY, bandHeightDual(seedMode));
                }
                continue;
            }
            const double modeH = bandHeightFor(p.type);
            const bool isModeP = modeH > 0.0 || p.type == 6 || p.type == 27;
            if (isModeP) {
                seedMode = (p.type == 5)    ? 1
                           : (p.type == 16) ? 2
                           : (p.type == 19) ? 3
                           : (p.type == 26) ? 4
                                            : 0;
            }
            if (seedDual && isModeP) {
                seed = bandFor(seedRefY, bandHeightDual(seedMode));
            } else if (modeH > 0.0) {
                seedRefY = p.cy;
                seed = bandFor(p.cy, modeH);
            }
        }
        init.bandRefY = (float)seedRefY;
        // ...unless GD was asked. `--startband f,c` carries the anchor tick's
        // own getMinPortalY / getMaxPortalY straight out of the dump, and it
        // REPLACES the x-only guess above rather than refining it.
        //
        // The guess is wrong whenever the player flew past a mode portal
        // instead of through it. Measured on lv20 (2026-08-09): the wave goes
        // UNDER the portal at (4445,303) -- its box is y[253,353] and the wave
        // is at y=114..160 -- so GD never fires it and its band stays
        // [90,390] (dump pmin/pmax read 11/309 + the layer offsets). The seed
        // took it anyway, put the band at [150,450], and clamped the wave at
        // 156 where GD was at 122. Everything after ran 34 px high, so GD died
        // where the model lived and the driver bought the difference back four
        // ticks at a time -- 5 px per iteration, which is not progress.
        if (startBandUsable()) {
            // ...unless the anchor is OUTSIDE it, in which case GD's pmin/pmax
            // are stale defaults and not a band the player is in. Measured on
            // lv22's ball corridor (2026-08-14): every anchor the ladder takes
            // there comes back as `--startband 90,386.999969` (the default
            // 90 + 270/camScale) while GD's own y is ~1,000 and climbing to
            // 1,074 -- GD plainly does not clamp there. Taking the band at face
            // value clamped the seed to 387 on the very first tick, so the
            // fixup pass reported `regenDied=t0+1` with dy=-593.95 and the
            // driver could not re-anchor anywhere in the corridor.
            const bool inside = (double)init.y >= g_startBandFloor - 1.0
                                && (double)init.y <= g_startBandCeil + 1.0;
            if (inside) {
                seed.floorY = g_startBandFloor;
                seed.ceilY = g_startBandCeil;
            } else {
                std::printf("startband: anchor y=%.2f is OUTSIDE [%.1f, %.1f]"
                            " - ignoring it (stale pmin/pmax)\n",
                            (double)init.y, g_startBandFloor, g_startBandCeil);
                seed.floorY = kGroundY;
                seed.ceilY = 1e9;
            }
        }
        if (g_shipCeilSet) seed.ceilY = g_shipCeil + kCubeHalf;
        if (g_ufoCeil > 0.0) seed.ceilY = g_ufoCeil;
        if (g_flyFloor > 0.0) seed.floorY = g_flyFloor;
        init.bandFloor = (float)seed.floorY;
        init.bandCeil = (float)seed.ceilY;
        // --bandowntrack: an anchor whose own band (GD's, --startband) is not the band the
        // recording holds at t0 on a portal-set track came through a portal the recording did not
        // fire, so its band is its own from the first tick (bands.hpp kBandOwn).
        if (g_bandOwnTrack && startBandUsable() && !g_bandTrack.empty() && !bandTrackIsCamera()
            && seed.floorY == g_startBandFloor && seed.ceilY == g_startBandCeil) {
            double fl = 0.0, ce = 0.0;
            if (bandTrackRowAt(t0, fl, ce)
                && (std::fabs(fl - g_startBandFloor) > 1.0 || std::fabs(ce - g_startBandCeil) > 1.0)) {
                init.bandBranch |= kBandOwn;
                std::printf("bandowntrack: the anchor's band [%.1f, %.1f] is not the recording's "
                            "[%.1f, %.1f] at t=%lld - its own from the start\n",
                            g_startBandFloor, g_startBandCeil, fl, ce, t0);
            }
        }
        if (startBandUsable())
            std::printf("startband: the seed will start in [%.1f, %.1f]\n",
                        seed.floorY, seed.ceilY);
        // --dualband: an anchor inside a dual takes the reference y from GD's own bands instead.
        // The replay above sees only the portals in L.portals, and a dual portal in a group is not
        // there -- it also re-reads every dual portal by x, where GD keeps
        // the one that turned the dual on. Every band GD placed since then is bandFor(ref, H), so
        // each on-grid row [fl, fl + H] with H a dual height puts ref in [fl + H/2, fl + H/2 + 30)
        // (a floor held at 90 bounds it from above only). The rows are intersected from the anchor
        // backwards -- the startband first, then the track -- and the walk stops at the first row
        // that would empty it, which is a band from before this reference. Off-grid rows are the
        // camera's and say nothing about it.
        if (init.dual) {
            double lo = -1e18, hi = 1e18;
            int used = 0;
            auto take = [&](double fl, double ce) -> bool {
                const double H = ce - fl;
                if (std::fabs(H - kBandFly) > 0.01 && std::fabs(H - kBandOther) > 0.01)
                    return true;   // not a dual height: no information, keep walking
                if (fl <= 90.0 + 0.01) {
                    const double b = 90.0 + H * 0.5 + 30.0;
                    if (b <= lo) return false;
                    hi = std::min(hi, b);
                    ++used;
                    return true;
                }
                if (std::fabs(fl - std::round(fl / 30.0) * 30.0) > 0.01) return true;
                const double a = fl + H * 0.5, b = a + 30.0;
                if (b <= lo || a >= hi) return false;
                lo = std::max(lo, a);
                hi = std::min(hi, b);
                ++used;
                return true;
            };
            bool open = true;
            if (startBandUsable()) open = take(g_startBandFloor, g_startBandCeil);
            for (size_t k = g_bandTrackIntervals.size(); open && k-- > 0;) {
                const BandTrackRow& r = g_bandTrack[g_bandTrackIntervals[k]];
                if ((long long)r.t > t0) continue;
                open = take((double)r.fl, (double)r.ce);
            }
            if (used > 0 && lo > -1e17 && hi < 1e17) {
                // The rows only narrow the reference to a 30 px window when one height has been
                // seen, and a window's midpoint can sit on the wrong side of the other height's
                // 30 px step (lv16's anchors before t=8,459: H 270 alone gives [495,525), the
                // dual portal is at 509, the midpoint 510 puts the H 300 floor at 360 where GD
                // has 330). So an actual portal y wins: the replay's own when it fits, else the
                // last dual portal behind the anchor whose cy fits -- grouped ones included, which
                // the replay cannot see (custom level C's uid 36984, cy 465, in [465,480)).
                double pick = 0.0;
                const char* how = nullptr;
                if (seedRefY >= lo && seedRefY < hi) {
                    pick = seedRefY;
                    how = "the portal replay";
                } else {
                    double bestX = -1e18;
                    auto consider = [&](const Obj& p) {
                        if (p.type != 23 || p.cx > x0 || p.cx <= bestX) return;
                        if (p.cy >= lo && p.cy < hi) {
                            bestX = p.cx;
                            pick = p.cy;
                            how = "a dual portal";
                        }
                    };
                    for (const Obj& p : L.portals) consider(p);
                    for (size_t k = 0; k < L.dyn.objs.size() && k < L.dyn.bucket.size(); ++k)
                        if (L.dyn.bucket[k] == Dynamics::PORT) consider(L.dyn.objs[k]);
                    if (!how) {
                        pick = (lo + hi) * 0.5;
                        how = "the rows' midpoint";
                    }
                }
                init.bandRefY = (float)pick;
                std::printf("dualband: the anchor is inside a dual - reference y %.1f (%s) from %d"
                            " band row(s), in [%.1f, %.1f) (the portal replay gave %.1f)\n",
                            (double)init.bandRefY, how, used, lo, hi, seedRefY);
            } else {
                std::printf("dualband: the anchor is inside a dual but no band row bounds the"
                            " reference y - left at %.1f\n", seedRefY);
            }
        }
    }
    size_t ceilIdx = 0;

    // (there is no shared x accumulator any more -- every state carries its own
    // xAbs and its own dx, and the layer is processed one speed group at a
    // time. See the speed-group note in the loop below.)
    // Current speed. A re-anchored solve starts mid-level, so every speed
    // portal already behind x0 has to be replayed here or the tail would run
    // the whole rest of the level at the wrong dx.
    float curDxF = kDxF;
    size_t spdIdx = 0;
    auto speedGate = [&](const Obj& o) { return o.cx - o.hw - kCubeHalf; };
    while (spdIdx < L.speeds.size() && speedGate(L.speeds[spdIdx]) <= x0) {
        curDxF = dxForSpeedId(L.speeds[spdIdx].id);
        ++spdIdx;
    }
    // The loop above positions spdIdx for the portals AHEAD, which is what it is
    // really for. What it must not do is decide the CURRENT speed by x alone: a
    // portal the player flew past without touching never fired in GD. When the
    // caller passed GD's own multiplier, that is a measurement of the answer, so
    // it wins over the replay. (lv16: the replay says 0.9 from x=17,403 on, GD
    // says 1.1 -- 0.3145 px/tick, and every tail past there was mistimed.)
    if (g_startSpeedMul > 0.0) {
        const float measured = dxForSpeedMul(g_startSpeedMul);
        if (measured != curDxF)
            std::printf("start speed: replay-by-x says %.5f, GD says %.5f "
                        "(mul %.3f) - using GD\n",
                        (double)curDxF, (double)measured, g_startSpeedMul);
        curDxF = measured;
    }
    // ...and the seed state carries it, so a re-anchored tail starts at the
    // section's real speed rather than 0.9 (see State::dx).
    init.dx = curDxF;

    // --start-state (statefile.hpp): the state the file holds replaces the anchor's seeding, with the
    // triggers' fire state and the moving geometry it stood in. --start must name the same tick.
    bool startStateLoaded = false;
    StateFile startSF;   // kept for the witness walk, which starts from the file's world again
    if (!g_startStatePath.empty()) {
        StateFile& sf = startSF;
        std::string why;
        if (!readStateFile(g_startStatePath, sf, why)) {
            std::printf("startstate: %s: %s - NOT applied\n", g_startStatePath.c_str(), why.c_str());
        } else if (sf.t != t0) {
            std::printf("startstate: %s holds t=%lld, the call's --start is t=%lld - NOT applied\n",
                        g_startStatePath.c_str(), (long long)sf.t, t0);
        } else if (!applyStateFile(sf, L, init, curDxF, why)) {
            std::printf("startstate: %s: %s - NOT applied\n", g_startStatePath.c_str(), why.c_str());
        } else {
            startStateLoaded = true;
            g_outcome.startStateT = t0;
            g_planPrevHeld = g_startHoldField >= 0 ? g_startHoldField : (int)init.held;
            std::printf("startstate: t=%lld x=%.4f y=%.4f vy=%.4f mode=%d frame=%d from %s\n", t0,
                        (double)init.xAbs, (double)init.y, (double)init.vy, (int)init.mode,
                        (int)init.frame, g_startStatePath.c_str());
        }
    }

    // --swapbodies (frames.hpp): the level's second player into the first slot, for a search that serves it. The two slots' last buttons go with them.
    // (Not for --replay: its plan's bits name the players, and a replay steps both.)
    // A search of a two-player level that has both bodies searches the FIRST body, the second a ghost that holds nothing (unless --ghostlevels says what it
    // holds); cliMain then runs the second body's pass against this one's plan (g_twoBodyCall, frames.hpp).
    if (g_twoPlayer && init.dual && replayPath.empty() && !g_swapBodies) {
        g_twoBodyCall = true;
        g_ghostOn = true;
    }
    // --startact2: the second player's button as the anchor left it (the first's is the --start hold field); without it a mid-level anchor
    // reads the second player as not holding.
    if (g_twoPlayer && init.dual && g_startAct2 >= 0) init.action2 = (uint8_t)g_startAct2;
    if (g_swapBodies && replayPath.empty() && init.dual) {
        const uint8_t a1 = init.action, a2 = (init.action2 == 2) ? (uint8_t)0 : init.action2;
        swapHalves(init);
        init.action = a2;
        init.action2 = a1;
        init.held = (g_startAct2 >= 0) ? (uint8_t)g_startAct2 : (uint8_t)0;   // the searched body's own hold; the ghost's is the levels file's
        std::printf("swapbodies: the second player is the searched body (y=%.3f vy=%.3f flip=%d mode=%d), the first the ghost (y2=%.3f)\n",
                    (double)init.y, (double)init.vy, (int)init.flip, (int)init.mode, (double)init.y2);
    }

    // ---- --replay: fixed-input diagnostic resim (no search) ----------------
    // Mirrors the witness resim at the end of main (same stepBoth, same window
    // construction) but takes its input from a plan file instead of the arena
    // walk, and STOPS on death instead of ignoring it (the witness resim's
    // documented flaw: it writes trace rows past rdead, and the band clamp is
    // an absorbing fixed point, so a dead lineage can look alive).
    prepMark(4);
    if (!replayPath.empty()) {
        struct Edge { long long press; int v; };
        std::vector<Edge> edges;
        std::vector<Edge> edges2;   // a two-player level's second button (input bit 1), when the level is one (g_twoPlayer)
        {
            std::ifstream pf(replayPath);
            if (!pf) {
                std::printf("REPLAY: cannot open %s\n", replayPath.c_str());
                return 2;
            }
            std::string ln;
            while (std::getline(pf, ln)) {
                // the driver splices plans through PowerShell, which stamps a
                // UTF-8 BOM -- without this the FIRST edge of every spliced
                // plan is silently dropped (found the hard way: the replayed
                // cube never took its first jump and died 40 px later)
                if (ln.size() >= 3 && (unsigned char)ln[0] == 0xEF &&
                    (unsigned char)ln[1] == 0xBB && (unsigned char)ln[2] == 0xBF)
                    ln.erase(0, 3);
                long long p; int v;
                if (std::sscanf(ln.c_str(), "input=%lld,%d", &p, &v) == 2) {
                    // The level is the buttons held from that tick: bit 0 player 1's jump, bit 1 player 2's (a two-player
                    // level's second button). A mask this call cannot drive is refused, not read as a press of player 1 as
                    // the level used to be (any nonzero value was a press).
                    if ((v & ~3) || ((v & 2) && !g_twoPlayer)) {
                        std::printf("REPLAY: input=%lld,%d holds a button this call does not drive%s\n", p, v,
                                    (v & 2) && !(v & ~3) ? " (the level is not two-player: no levelsettings twoPlayer=1)" : "");
                        return 2;
                    }
                    edges.push_back(Edge{p, v & 1});
                    if (g_twoPlayer) edges2.push_back(Edge{p, (v >> 1) & 1});
                }
            }
        }
        // [2026-08-19] WINDOW RE-PUSH. On w1, the tick a 2899 window lifts, GD
        // re-issues pushButton for a button that is physically still held --
        // worker98 measurement: at t=20,487 of the reference run (the end of
        // window 20110:20487) a PO_pushButton appears with NO INJECT and the
        // robot jumps at 20,488 with vy=4.568. Five variants all divide
        // cleanly by "held in the press ledger before w1 <=> fires":
        //   press20484/rel20487 -> jumps     press20486/rel20487 -> jumps
        //   press20484/rel20485 -> no jump   press20470/rel20473 -> no jump
        //   no pair -> no jump
        // (Even when the release lands on the same tick as w1, the re-push
        // runs before that tick's injected release.)
        // The model mirrors it with synthetic edges: if held(<w1), add a press
        // at press=w1 (its effect lands at w1+1 through the existing latOf
        // path, and the window's suppression ends at w1, so it passes
        // through). If already released by w1, add a release at w1+1 too,
        // closing it within 1 tick (holding across the window is unchanged =
        // the measured ship shape).
        // The search side (which has no edges) is untouched -- the
        // completeness hole that the post-window jump cannot be used as a
        // move remains, but the correctness side (census) is closed by this.
        std::stable_sort(edges.begin(), edges.end(),
                         [](const Edge& a, const Edge& b) { return a.press < b.press; });
        std::stable_sort(edges2.begin(), edges2.end(),
                         [](const Edge& a, const Edge& b) { return a.press < b.press; });
        if (!g_ctrlWin.empty()) {
            const size_t nOrig = edges.size();
            for (const auto& w : g_ctrlWin) {
                int heldBefore = 0, heldAfter = 0;
                for (size_t k = 0; k < nOrig; ++k) {
                    if (edges[k].press < w.second) heldBefore = edges[k].v;
                    if (edges[k].press <= w.second) heldAfter = edges[k].v;
                    else break;
                }
                if (heldBefore) {
                    edges.push_back(Edge{w.second, 1});
                    if (!heldAfter) {
                        edges.push_back(Edge{w.second + 1, 0});
                        // a buffered jump fires without holding ->
                        // this jump does not hover (rePushNoHoverAt)
                        g_winRePushJump.push_back(w.second + 1);
                    }
                    std::printf("REPLAY: ctrl re-push at t=%lld (held into "
                                "window end)\n", w.second);
                }
            }
            // synthetic edges are appended and then stable-sorted again --
            // they must come AFTER the real release {w1,0} at the same press
            // (on an equal effect tick the later press wins, see the note
            // below)
            std::stable_sort(edges.begin(), edges.end(),
                             [](const Edge& a, const Edge& b) { return a.press < b.press; });
        }
        std::printf("REPLAY: %zu edges from %s\n", edges.size(), replayPath.c_str());
        g_hist = HistStat{};   // --histstat: this replay's counts only
        g_histFired.clear();
        g_histPads.clear();
        // The plan stores PRESS ticks (effect - latency, see the emitter at the
        // bottom of main). Latency depends on the mode at the END of the press
        // tick, which a forward sim knows by the time it needs it: after
        // stepping tick t, schedule every edge pressed at t for t + lat(mode).
        // The two lats are 1 and 2, so effects never cross in press order (a
        // swap would need a lat gap >= 2); equal-tick effects apply in press
        // order, later press winning, which is what GD does. Mode-portal
        // boundary ticks can still land +/-1 tick -- the known latency class
        // the driver absorbs -- so a divergence needs ~2 ticks of slack before
        // it means anything.
        auto latOf = [](uint8_t m) { return (m == 1 || m == 3) ? 2 : 1; };
        size_t eIdx = 0;
        std::vector<std::pair<long long, int>> fx;  // (effect tick, level)
        // --sametick (speed.hpp): the edges of one press tick, scheduled as GD's button pass takes them.
        // `pressLevel` is the button as the edges before this tick left it. A press then a release on the
        // tick is a tap: it holds for the effect tick only, and the loop keeps it only if pushButton's own
        // work takes it there (tapEff). A release then a press while held is a fresh press on the effect
        // tick (refreshEff).
        std::set<long long> refreshEff, tapEff;
        int pressLevel = 0;
        auto scheduleTick = [&](size_t from, size_t to, long long eff, uint8_t mode,
                                std::vector<std::pair<long long, int>>& out) {
            const int before = pressLevel;
            bool fresh = false;
            int lvl = before;
            for (size_t k = from; k < to; ++k) {
                if (edges[k].v != 0 && lvl == 0) fresh = true;
                lvl = edges[k].v;
            }
            pressLevel = lvl;
            if (!g_sameTick || to - from < 2 || !fresh) {
                for (size_t k = from; k < to; ++k) out.push_back({eff, edges[k].v});
                return;
            }
            if (lvl == 0) {
                out.push_back({eff, 1});
                out.push_back({eff + 1, 0});
                tapEff.insert(eff);
                if (before != 0) refreshEff.insert(eff);
            } else {
                out.push_back({eff, lvl});
                if (lvl != 0 && before != 0) refreshEff.insert(eff);
            }
        };
        // An edge whose EFFECT tick is at or before t0 was already consumed by
        // the time the anchor's state was recorded, so it must NOT be replayed
        // as a change: it defines the level the anchor comes in HOLDING.
        // Pushing it into fx made the very first tick see `input=1` against an
        // `action` of 0, i.e. a fresh press -- and the two modes whose action
        // is the rising EDGE (`act = input && !s.action`, swing and UFO) then
        // fired one extra time. Measured on lv22 t=4,200 (swing, plan edge
        // pressed at 4,199 -> effect 4,200 = the anchor tick itself): GD flips
        // at 4,201, the model flipped at 4,202 and stayed a tick behind for
        // the rest of the segment.
        // `init.action` is normally seeded from the --start hold field only
        // when the anchor is mid-hover or mid-dash (see that gate's note: the
        // unconditional version cost lv12/18/20 a segment each). This is why
        // it did -- the hold field is `held_before(plan, t0)`, which counts by
        // PRESS tick, so for a ship/UFO (latency 2) an edge pressed at t0-1
        // reads as already held when its effect is still one tick away. The
        // level derived here is by EFFECT tick and per-mode, which is the
        // quantity `action` actually means.
        int preLevel = 0;
        bool preRise = false;   // the rising edge landed exactly ON t0
        while (eIdx < edges.size() && edges[eIdx].press <= t0) {
            const long long eff = edges[eIdx].press + latOf(init.mode);
            size_t to = eIdx + 1;
            while (to < edges.size() && edges[to].press == edges[eIdx].press) ++to;
            std::vector<std::pair<long long, int>> tick;
            scheduleTick(eIdx, to, eff, init.mode, tick);
            for (const auto& e : tick) {
                if (e.first <= t0) {
                    preRise = (e.first == t0) && e.second != 0 && preLevel == 0;
                    preLevel = e.second;
                } else {
                    fx.push_back(e);
                }
            }
            eIdx = to;
        }
        // The second body's own schedule, the same walk with ITS mode's latency (a two-player level only; edges2 is empty otherwise).
        size_t eIdx2 = 0;
        std::vector<std::pair<long long, int>> fx2;   // (effect tick, level)
        int preLevel2 = 0;
        while (eIdx2 < edges2.size() && edges2[eIdx2].press <= t0) {
            const long long eff = edges2[eIdx2].press + latOf(init.mode2);
            if (eff <= t0) preLevel2 = edges2[eIdx2].v;
            else fx2.push_back({eff, edges2[eIdx2].v});
            ++eIdx2;
        }
        if (g_twoPlayer && init.dual) init.action2 = (uint8_t)preLevel2;
        if (startStateLoaded) {
            // --start-state: the state's own input level and pending bits are the walk's, exactly
            if ((int)init.action != preLevel)
                std::printf("startstate: the plan's input level at t0 reads %d, the state's %d - the "
                            "state's is kept\n", preLevel, (int)init.action);
            preLevel = (int)init.action;
        } else {
        init.action = (uint8_t)preLevel;
        // --spentaction (the hist seeding's note): ...but an edge whose press GD has already SPENT
        // before t0 -- a ring takes it on the press's own tick, a tick sooner than the flap's
        // latency 2 counts -- is not an edge any more when its counted effect tick arrives.
        if (init.pressSpent && (init.mode == 3 || init.mode == 7))
            init.action = 1;
        // ...and the SWING's pending flip is state the dump has no column for.
        // The tap sets a pending bit and the flip lands on the NEXT tick, so an
        // anchor taken on the tick the press took effect owes the flip. Same
        // lv22 t=4,200 measurement: with the level fixed above and no pending
        // bit the model never flips at all. The bit rides rHover (robot-only,
        // free in mode 7) exactly as the airborne path uses it.
        if (init.mode == 7 && preRise) init.rHover = 1;
        }
        std::ofstream tr(outPath + ".trace.csv");
        tr.precision(10);
        // `act` = the input level the tick was stepped with. The driver's
        // fixup recorder needs it (a divergence override is gated on the
        // input), and reconstructing it from the plan would duplicate the
        // latency rules in PowerShell.
        // ...and the CONTACT STATE, so the driver can group divergences by
        // CAUSE instead of patching them point by point.
        //
        // Why: a fixup record used to carry only (x, y, vy, dy, dvy), so every
        // occurrence of ONE mishandled interaction became its own local override
        // (matched within 1.2 px of x and 4 px of y). Measured on lv20
        // 2026-08-08: 104 records to reach x=11,973 -- 115 px per record -- and
        // 62 of them fell into SIX signatures. One rule fix (the ceiling ramp)
        // had moved the same level 9,266 -> 18,214 by itself. Without saying what
        // the model was touching, the driver cannot tell "a constant is off"
        // (tight dy spread -> fit it) from "the formula is wrong" (wide spread ->
        // fix the code).
        // `dx` = this state's speed (px/tick). THE SIGNATURE NEEDS THE SPEED:
        // the acceleration switch threshold is 2 x GD's per-speed gravity
        // (models/speed.hpp), so it takes a different value in every speed band.
        // Without knowing which band a divergence shows up in, "the threshold is
        // off" cannot be told apart from some other cause. (This used to say
        // 1.3 and 1.6 were unmeasured and inherited 0.9's value -- they were,
        // and that was the row the formula corrected.)
        // `flip` and `frame` are here because the rotated sections are exactly
        // where `flip` stops meaning what GD's `upsideDown` means, and without
        // them a sign disagreement reads as a physics bug (lv22 t=5,110: the
        // model's vy is the negative of GD's for the whole frame-3 section, so
        // a type-4 portal looks like a no-op and never fires).
        tr << "tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act"
              ",onslope,slopem,slopet,bandf,bandc,mini,held,dx,nearorb,clamp"
              ",clampuid,clampcx,clampcy,flip,frame,rot,rotneg"
              // The SECOND BODY's own ride state. The dual runs the whole of
              // stepOne twice and the second half keeps its own copy of every
              // one of these, but only p1's was ever written -- so a divergence
              // that lives in p2's ride ("only p2 is off, by exactly the slope
              // exit bonus") had nothing to read. Appended at the end; existing
              // readers index the columns before this by position.
              ",grounded2,onslope2,slopem2,slopet2,mode2,ceilt,ceilt2,mini2"
              // `fxblk`: the moving object that kept a MATCHING fixup from
              // firing on the step into this row, -1 if none (fixup.hpp,
              // g_fxBlockedUid). The recorder's third answer.
              ",fxblk"
           // --tracestate: the state's own vy and flip, which `vy` and `flip` are not on a
           // display row (refwatch.hpp, g_traceState).
           << (g_traceState ? ",svy,sflip" : "")
           // --p2xoff: the second body's x offset (State::dx2), under the flag only
           << ",dx2"
           // a two-player level: the second body's own button as the tick was stepped with it (the ghost levels of a search that serves the other body)
           << (g_twoPlayer ? ",act2" : "") << "\n";
        // Make --snaplog usable in replay too (it used to exist only on the
        // SOLVE side, so a known plan's stair snaps could never be checked
        // against GD's snaptrace).
        std::ofstream sn;
        if (!snapLogPath.empty()) {
            sn.open(snapLogPath);
            g_snapOut = &sn;
        }
        State s = init;
        // Did --seeddump's tick actually occur in this walk? A tick the walk
        // never reached must SAY SO rather than print nothing: silence reads
        // the same as "nothing was consumed there", and a producer would take
        // the absence for a clean seed. Same discipline as NO VERSION and
        // resimdie=? -- a value that is missing has to be distinguishable from
        // a value that is zero.
        bool sawSeedTick = false;
        // Slices live in the CURRENT frame and restart when it turns (their
        // cursor is monotone in that frame's u).
        // ...and when the ANCHOR itself starts inside a turned frame
        // (--start's 21st field), bind the world in that orientation from the
        // start. This was fixed at &L, so the state was in frame 3 while the
        // geometry was in frame 0, and the replay lost its footing within 2
        // ticks (the "follow 2 ticks" at lv22 t=11,340).
        // (model.hpp FrameSlices: the replay keeps its own, not ModelWorld's cache.)
        FrameSlices rfs;
        bindFrameSlices(rfs, frameLevel(L, (int)init.frame));
        // Turn the frame ONE TICK AFTER the crossing (measured: the boxes
        // overlap for ~15 ticks and nothing happens; the change lands the tick
        // after the player's forward coordinate passes the object's).
        int pendingFrame = -1;
        const float rDxF = curDxF;
        int curIn = preLevel;   // (see the pre-anchor edge split above)
        int curIn2 = preLevel2;   // the second body's button (a two-player level)
        // --p2taps: the second body's button by tick, for itemGates' Tap (frames.hpp secondLevelAt); entry 0 is the anchor tick's
        if (g_twoPlayer && g_p2Taps) {
            g_p2ReplayBase = t0;
            g_p2ReplayLvl.assign(1, (uint8_t)(curIn2 > 0 ? 1 : 0));
        }
        size_t fxIdx = 0, fxIdx2 = 0;
        long long diedT = -1;
        double diedX = 0;
        long long lastT = t0;
        for (long long t = t0 + 1; t <= tEnd; ++t) {
            lastT = t;
            while (fxIdx < fx.size() && fx[fxIdx].first <= t)
                curIn = fx[fxIdx++].second;
            while (fxIdx2 < fx2.size() && fx2[fxIdx2].first <= t)
                curIn2 = fx2[fxIdx2++].second;
            if (g_twoPlayer && g_p2Taps) {
                g_p2ReplayLvl.resize((size_t)(t - g_p2ReplayBase + 1), 0);
                g_p2ReplayLvl[(size_t)(t - g_p2ReplayBase)] = (uint8_t)(curIn2 > 0 ? 1 : 0);
            }
            // Under reverse (State::rev) x decreases. Apply the same sign
            // here as the DP side's group windows (if the replay / resim do
            // not match GD, every fixup comparison becomes a lie).
            const float rDxUsed = ((s.dx > 0.f) ? s.dx : rDxF)
                                  * (s.rev ? -1.f : 1.f);
            const double xPrevR = (double)s.xAbs;
            // --tpjumpnext: the rotation's crossing starts where last tick's forward jump left (step.hpp crossBaseX)
            const double xCrossR = crossBaseX(s);
            const double x = (double)advanceX(s.xAbs, rDxUsed);
            if (g_dynDbg == -2 && t == t0 + 1)
                std::printf("revdbg init.rev=%d (at replay start)\n", (int)init.rev);
            if (g_dynDbg == -2 && t <= t0 + 3)
                std::printf("revdbg t=%lld s.rev=%d dx=%.4f xPrev=%.3f x=%.3f\n",
                            t, (int)s.rev, rDxUsed, xPrevR, x);
            // autonomous fires, from this single trajectory's own crossing
            // (x is the position the player reaches AT tick t, so a first
            // x >= cx means the crossing is t and the move starts at t + delay)
            for (AutoTrig& A : g_autoTrig) {
                if (A.fireT >= 0) continue;
                if (A.chan != 0 && g_rotQueue && !g_rotQ.empty()) {
                    // --chanmove: a channelled trigger is judged in the travel direction of the channel it belongs to; a reversed
                    // rider passes it when x falls to cx, not when x reaches it, and is not in the sorted-by-cx scan
                    if ((int)s.rotChan != A.chan) continue;
                    // --autotrigframe (dynamics.hpp): the trigger along the travel axis of the state's frame
                    double ucx = A.cx;
                    if (g_autoTrigFrame && (int)s.frame != 0) { double v; toFrame((int)s.frame, A.cx, A.cy, ucx, v); }
                    if (s.rev ? (x > ucx) : (x < ucx)) continue;
                } else {
                    // no rotation queue on this level (a queue of more than 32 entries is refused): the active channel is
                    // not tracked, so the crossing alone fires it, and a Toggle --chantog drops stays dropped
                    if (A.chan != 0 && A.chanStatic) continue;
                    if (x < A.cx) break;   // sorted by cx
                }
                A.fireT = (int)t + A.delay;
                A.fireX = A.lockLastX = x;
                std::printf("autotrig: uid %d x=%.0f crossed at t=%lld, "
                            "effect t=%d\n", A.uid, A.cx, t, A.fireT);
            }
            // GAMEPLAY ROTATION: did this tick's advance pass a rotation
            // object, measured along the CURRENT frame's travel axis?
            // ...those pointing at the same frame are reverse toggles, so
            // they are NOT excluded (applyRotation handles them with a
            // proximity test). The crossing direction follows the direction
            // of travel.
            // [2026-08-24] THE SAME GATE AS applyRotation, or the two paths live
            // in different worlds: this copy still held the falsified 30px window
            // after the gate itself moved to kRotPerpWin + nearest-|dv|-wins, so
            // the DP turned at t=16,428 (|dv|=115, inside the measured firing at
            // 116.7) while the replay of the very same plan sailed past in frame 0
            // and rode the underside of the death block out of the level. Every
            // constant here follows the measurement at applyRotation's gate.
            {
                RotTrig* rBest = nullptr;
                bool bestSame = false;
                double bestDvR = 1e18;
                for (RotTrig& r : g_rotTrig) {
                    // `same` MUST HAVE THE SAME DEFINITION as applyRotation
                    // (4941). Read from frame equality alone, a reverse setting
                    // that has a gnddir (setRev=1, frame stays 0 and only the
                    // direction changes) turns into a "pure toggle" and falls
                    // into the else branch below. [2026-08-18] lv22 uid16659
                    // (22575,1755) was that: only the one-shot (revT) got burnt,
                    // applyRotation was never called, the model kept moving
                    // forward and GD's floor vanished from the near slice (the
                    // missed landing at t=18,513).
                    const bool same = (r.setRev < 0)
                        ? (r.frame == (int)s.frame)
                        : (r.frame == (int)s.frame
                           && r.setRev == (int)s.rev);
                    if ((same ? r.revT : r.firedT) >= 0) continue;  // one shot
                    const double ru = frameU((int)s.frame, r.cx, r.cy);
                    const bool cross = s.rev ? (xCrossR > ru && x <= ru)
                                             : (xCrossR < ru && x >= ru);
                    // ...and it must overlap on the perpendicular axis too. This
                    // CONSUMES the one-shot, so it is narrowed by the same
                    // condition as applyRotation's gate -- merely passing far
                    // away must not burn the trigger. [2026-08-16] Measured
                    // (lv22): uid6308 (15,399,615) was consumed at t~11,2xx,
                    // when the world y crossed 615 in frame 3 -- the player was
                    // at world x 16,125 then, 726px AWAY on the perpendicular
                    // axis. By t=12,134, when GD actually turns, it was already
                    // spent and only the model failed to turn.
                    const double dvR =
                        std::fabs((double)s.y
                                  - frameV((int)s.frame, r.cx, r.cy));
                    if (!cross || dvR > kRotPerpWin) continue;
                    if (dvR < bestDvR) {
                        bestDvR = dvR; rBest = &r; bestSame = same;
                    }
                }
                if (rBest) {
                    // a same-frame firing (reverse toggle / absolute rev
                    // setting) is ALWAYS handed to applyRotation too. Burning
                    // only the one-shot without raising pendingFrame leaves
                    // rev unchanged forever
                    pendingFrame = rBest->frame;
                    if (!bestSame) rBest->firedT = (int)t;
                    else rBest->revT = (int)t;
                }
            }
            // --fbforce <box>:<tick> (diagnostic): place this tick's geometry as if
            // that box had fired at <tick>, when this walk has entered it. Asks
            // whether a box's fire tick still moves an object after the box's key
            // window (g_touchMoveTicks) has run out -- the window assumes it does
            // not.
            uint16_t fbForced[kTouchBits];
            const uint16_t* fbUse = s.fireB;
            if (g_fbForceBox >= 0 && g_fbForceBox < kTouchBits && s.trig.test(g_fbForceBox)) {
                std::copy(s.fireB, s.fireB + kTouchBits, fbForced);
                fbForced[g_fbForceBox] = (uint16_t)g_fbForceTick;
                fbUse = fbForced;
            }
            // The tick's geometry, built by the model as the search's groups are (model.hpp
            // prepareGroup) -- a group of one, its window the state's own x. The lock offset is
            // + this tick's advance (xPrevR is tick t-1's x): see the group call's note on why a
            // lock needs the CURRENT tick. No coin list, no veto index, no --trigdbg line: the
            // replay never had them.
            GroupEnv env;
            double pLo = x, pHi = x;
            p2Widen(pLo, pHi, p2Dx(s), p2Dx(s));   // --p2xoff
            prepareGroup(world, rfs, env, (int)s.frame, s.trig, (int)s.trigT, fbUse,
                         s.lockOff + (float)(x - xPrevR), x, xPrevR, rDxUsed, pLo, pHi, t,
                         GroupOpts{false, false, false});
            const std::vector<std::pair<const TouchTrig*, TouchMask>>& rt = env.trigs;
            if (g_slopeDbg && s.frame != 0) {
                std::printf("trigcand t=%lld frame=%d x=%.3f y=%.3f "
                            "boxes=%zu cand=%zu:",
                            (long long)t, (int)s.frame, x, (double)s.y,
                            touchFor((int)s.frame).size(), rt.size());
                for (const auto& tb : rt)
                    std::printf(" (%.1f,%.1f %.0fx%.0f du=%.2f dv=%.2f)",
                                tb.first->cx, tb.first->cy,
                                2 * tb.first->hw, 2 * tb.first->hh,
                                std::fabs(x - tb.first->cx),
                                std::fabs((double)s.y - tb.first->cy));
                std::printf("\n");
            }
            const StepCtx K = env.ctx();
            bool rdead = false;
            g_nearOrb = 0;
            g_dashVySet = 0;
            g_clampWhy = "";
            g_clampUid = -1;
            // ...and the COORDINATES beside them, which this block cleared the
            // "why" and the uid of but not the numbers. seqcall found it: two
            // identical whole-run replays in one process differ in clampcx /
            // clampcy on 539 of lv20's rows and 448 of lv21's, starting at
            // t=1 with the LAST clamp of the previous call. Nothing else
            // differs, so the trajectory was never at risk -- but the comment
            // at CLAMP0O says these exist to catch stale geometry, and they
            // were themselves stale across calls.
            g_clampCx = g_clampCy = 0.f;
            g_deadWhy = "";
            g_deadObj = nullptr;
            // g_deadCx/Cy are NOT diagnostics: the resim block publishes them
            // as g_resimObjX / g_resimObjY. An object-free death (deadband,
            // maxplayy, offboard -- and the two-tick latch GD kills with) sets
            // g_deadObj to null and leaves the coordinates alone, so the
            // outcome carried the PREVIOUS killer's position with uid = -1.
            g_deadCx = g_deadCy = 0.f;
            const bool sPrevGrounded = (s.grounded != 0);   // applyRotation's re-tap
            const double sPrevY = (double)s.y;              // ...and its pre-tap y
            const int wgFrame = (int)s.frame;               // --walkgates (below)
            // --sametick: a release then a press on one tick while held -- the step sees a fresh press
            if (g_sameTick && curIn != 0 && refreshEff.count(t)) s.action = 0;
            // ...and a tap acts only through pushButton's own work: a ring it fires, or the jump of a
            // grounded ball, cube, robot or spider. A flap, a thrust or a wave's climb needs the hold
            // that the release has already taken back (updateJump reads +0x985 and +0x986).
            if (g_sameTick && curIn != 0 && tapEff.count(t)) {
                bool deadTry = false;
                const State tryC = stepBoth(s, 1, K, deadTry);
                const bool ring = (tryC.ringHold && !s.ringHold)
                                  || (tryC.usedOrb != nullptr && tryC.usedOrb != s.usedOrb);
                const bool groundJump = s.grounded && sameTickTapMode(s.mode)
                                        && ((tryC.pressSpent && !s.pressSpent)
                                            || (s.mode == 2 && tryC.flip != s.flip));
                if (!ring && !groundJump) curIn = 0;
                // the trial's diagnostics are not this tick's
                g_nearOrb = 0;
                g_dashVySet = 0;
                g_clampWhy = "";
                g_clampUid = -1;
                g_clampCx = g_clampCy = 0.f;
                g_deadWhy = "";
                g_deadObj = nullptr;
                g_deadCx = g_deadCy = 0.f;
            }
            const uint8_t wgPrev = s.action;
            State c = stepBoth(s, (uint8_t)curIn, K, rdead, g_twoPlayer ? curIn2 : -1);
            c.action = (uint8_t)curIn;
            s = c;
            // --seeddump <t>: the state's ACCUMULATED fields at one tick.
            //
            // These are the fields an anchor has to seed, and the check they
            // exist for is one comparison: run from t=0 with --seeddump T, run
            // with --start at T-1 and --seeddump T, and the two lines must
            // match. Three separate defects today were all "the anchor scan
            // does not seed this" (State::fireB, State::lockOff, and the
            // queue's rotSpent / rotChan / rotRev), each found only after it
            // changed an answer. WHOEVER ADDS A PER-STATE ACCUMULATED FIELD
            // ADDS IT HERE -- a field missing from this line is a field the
            // check cannot see.
            if ((g_seedDump >= 0 && t == (long long)g_seedDump)
                || (g_seedEvery > 0 && t % (long long)g_seedEvery == 0)) {
                // pressSpent is printed but NOT seeded: gdref has no column for
                // GD's +0x986, so an anchor starts it at 0 ("this press has not
                // been spent"). It joins ringHold in the hole the list at the top
                // of this file names. The bias is one-sided and small -- a state
                // anchored mid-hold can fire one ring GD would have refused --
                // and it needs the button held ACROSS the anchor tick to bite.
                // groundUid (--stickseam) is the same kind of hole: printed, not
                // seeded; an anchor mid-ride re-ties to the lowest-uid support.
                // usedOrbOld too (the ring fired before usedOrb): an orb is spent by
                // a press, and the recording cannot say which were taken -- the
                // hole usedOrb itself has (the list at the top of this file).
                std::printf("seed: t=%lld sizeof=%zu trig=0x%llx trigT=%d "
                            "lockOff=%.4f rotSpent=0x%x rotChan=%d "
                            "rotRev=0x%x rotStep=%.6f rotNeg=%d rotBoost=%d "
                            "ringHold=%d pressSpent=%d portalLatch=%s/%s "
                            "groundUid=%d usedOrbOld=%d usedOrbHist=%d|%d|%d portSeen=%d|%d|%d "
                            "touchRing=%d|%d|%d|%d/%x taps=%d fireB=",
                            t, sizeof(State), (unsigned long long)s.trig.word(0),
                            (int)s.trigT,
                            (double)s.lockOff, s.rotSpent, (int)s.rotChan,
                            (unsigned)s.rotRev, (double)s.rotStep,
                            (int)s.rotNeg, (int)s.rotBoost, (int)s.ringHold,
                            (int)s.pressSpent, gravLatchHex(s.portalLatch).c_str(),
                            gravLatchHex(s.portalLatch2).c_str(),
                            (int)s.groundUid, (int)s.usedOrbOld, (int)s.usedOrbHist[0],
                            (int)s.usedOrbHist[1], (int)s.usedOrbHist[2], (int)s.portSeen[0],
                            (int)s.portSeen[1], (int)s.portSeen[2], (int)s.touchRing[0],
                            (int)s.touchRing[1], (int)s.touchRing[2], (int)s.touchRing[3],
                            (unsigned)s.touchRingT, (int)s.taps);
                for (int b = 0; b < kTouchBits; ++b)
                    if (s.fireB[b]) std::printf("%d:%u,", b, s.fireB[b]);
                std::printf("\n");
                if (s.revOff != 0.f || s.revCarry != 0.f)   // --revcarry
                    std::printf("seedrev: t=%lld revOff=%.6f revCarry=%.6f\n", t, (double)s.revOff, (double)s.revCarry);
                // The speed portals' memory (State::spdFired), on its own line so the one above
                // keeps its shape for the parsers that read it.
                std::printf("seed: t=%lld spdFired=%d|%d spdFired2=%d|%d\n", t,
                                (int)s.spdFired[0], (int)s.spdFired[1],
                                (int)s.spdFired2[0], (int)s.spdFired2[1]);
                // --spdsameonce: the memory's two more places (State::spdMore)
                std::printf("seed: t=%lld spdMore=%d|%d spdMore2=%d|%d\n", t, (int)s.spdMore[0],
                            (int)s.spdMore[1], (int)s.spdMore2[0], (int)s.spdMore2[1]);
                // +0xa1c (State::a1cLatch, bit 1 = the second body): printed, not seeded -- the
                // dump has no column for it, so an anchor starts it at 0
                std::printf("seed: t=%lld a1cLatch=%d\n", t, (int)s.a1cLatch);
                // +0x728 (State::ball728, --ballscaleair): printed, not seeded, for the same reason
                std::printf("seed: t=%lld ball728=%d\n", t, (int)s.ball728);
                // --p2xoff (State::dx2): seeded at an anchor from --startp2dx
                std::printf("seed: t=%lld dx2=%.4f\n", t, (double)s.dx2);
                // --followy (State::followF): seeded from the recording at an anchor
                if (!g_followYs.empty())
                    std::printf("seed: t=%lld followOk=%d followF=%.3f|%.3f followOwn=%d|%d followN=%d|%d\n", t,
                                (int)s.followOk, (double)s.followF[0], (double)s.followF[1],
                                (int)s.followOwn[0], (int)s.followOwn[1], (int)s.followN[0], (int)s.followN[1]);
                // ...and the same state as a READY-MADE --startrotq argument.
                //
                // rotSpent is a mask over g_rotQ, and the bit order is
                // buildRotQueue's, not rotgameplay.txt's -- seeding uid 1215
                // and uid 1343, the file's first two rows, gives bits 0 and 8.
                // So the inversion back to uids belongs HERE, beside the queue
                // it indexes. A caller doing it would have to hold a copy of
                // that ordering, which is the proxy-for-identity shape this
                // campaign spent 2026-09-10 learning to distrust.
                //
                // SCOPE: these values come from a walk of the MODEL, not from
                // the game. frames.hpp:165-168 is what licenses that -- the
                // queue is right from t=0, measured at 191 of 192 transitions
                // agreeing on lv22 -- so a seed read off a t=0 walk carries at
                // most that 1-in-192 of the model's own error, and is not the
                // circular "assume the equality to prove it".
                //
                // Built ONCE, into a string, and then both printed and
                // published. The in-process caller (the repair loop) cannot
                // read stdout, so it needs the value rather than the line; two
                // formatters for one argument would be two things to keep in
                // step, and the bit->uid inversion above is precisely the part
                // that must not be duplicated.
                if (!g_rotQ.empty()) {
                    char head[64];
                    std::snprintf(head, sizeof head, "%d,%04x",
                                  (int)s.rotChan, (unsigned)s.rotRev);
                    std::string arg(head);
                    for (size_t q = 0; q < g_rotQ.size() && q < 32; ++q)
                        if ((s.rotSpent >> q) & 1u)
                            arg += "," + std::to_string(g_rotQ[q].uid);
                    std::printf("seedrotq: t=%lld --startrotq %s\n",
                                t, arg.c_str());
                    g_outcome.seedRotQ = arg;
                }
                sawSeedTick = true;
            }
            // ...and now turn, if the tick that just ran crossed one. The world
            // point is what carries over; (u,v) is re-read in the new frame.
            // The world VELOCITY carries too: the forward speed becomes the new
            // perpendicular one (measured on lv22 uid 6286 -- the cube keeps
            // moving +X at 1.298 px/tick while its travel axis is now +Y, which
            // reads as vy = 1.298 / kYScale = 5.193 on the tick it turns), and
            // the old perpendicular velocity is dropped because the forward
            // speed is the section's, not a free variable.
            // A `pendingFrame != s.frame` gate STRUCTURALLY DROPS SAME-FRAME
            // REVERSE (a firing that goes frame 0 -> 0 and changes only the
            // direction cannot pass). The DP side (stepKid) calls
            // applyRotation unconditionally and can solve reverse, yet this
            // replay path alone could not keep up -- the real cause of "the
            // DP says SOLVED, the replay keeps going forward". [2026-08-18]
            // --rottouch (frames.hpp): a touch trigger the stepped body has reached is due as well
            // (the collision pass activates it where the tick left the body, a teleport's landing too)
            if (g_rotTouch && pendingFrame < 0) {
                double twx, twy;
                fromFrame((int)s.frame, (double)s.xAbs, (double)s.y, twx, twy);
                for (const RotTrig& r : g_rotTrig)
                    if (rotTouchHit(r, twx, twy)) {
                        pendingFrame = r.frame;
                        break;
                    }
            }
            // --rotqreplay (frames.hpp): the queue decides on every tick, not only on a scan hit
            if (pendingFrame < 0 && g_rotQReplay && g_rotQueue && !g_rotQ.empty())
                pendingFrame = (int)s.frame;
            if (pendingFrame >= 0) {
                double wx0, wy0;
                fromFrame((int)s.frame, (double)s.xAbs, (double)s.y, wx0, wy0);
                const int rev0dbg = (int)s.rev;
                const int f0dbg = (int)s.frame;
                const int nf = applyRotation(s, xCrossR, (double)rDxUsed, t,
                                             curIn, sPrevGrounded,
                                             sPrevY, true);
                if ((nf >= 0 && nf != f0dbg) || (int)s.rev != rev0dbg) {
                    if (nf >= 0 && nf != f0dbg)
                        bindFrameSlices(rfs, frameLevel(L, nf));
                    std::printf("rotation: frame -> %d at t=%lld "
                                "world=(%.1f,%.1f) u=%.3f v=%.3f vy=%.3f "
                                "f0=%d rev0=%d -> rev=%d flip=%d\n",
                                (nf >= 0 ? nf : f0dbg), t, wx0, wy0,
                                (double)s.xAbs, (double)s.y,
                                (double)s.vy, f0dbg, rev0dbg,
                                (int)s.rev, (int)s.flip);
                }
                pendingFrame = -1;
            }
            // --walkgates: the item gates the search's own step fires (itemGates),
            // after the turn and only on a tick that did not turn -- the search's
            // gate. Without it this walk never fires a Count or a Tap trigger.
            model.fireGates(s, wgPrev, curIn, K, wgFrame, t, rdead);
            // The trace is always WORLD coordinates -- that is what GD's dump
            // is, and a turned frame would otherwise read as a huge divergence.
            double wX, wY;
            fromFrame((int)s.frame, (double)s.xAbs, (double)s.y, wX, wY);
            // ...and so is vy. GD's dump reports the player's own vertical
            // velocity in the CURRENT gameplay frame, and in frame 3 the
            // model's vertical axis points the other way -- the same statement
            // `gdUpOf` makes about `flip`. Leaving the raw value here made
            // every tick of a frame-3 section look like a divergence with
            // dy=0.000 and dvy = -2*gd_vy, which is what the driver's fixup
            // recorder then wrote down: measured on lv22 (2026-08-14) the pass
            // anchored at t=4,643 reported "first divergence t=4,666 dy=-0.0000
            // dvy=-0.4240" while x and y agreed to three decimals for the whole
            // rotated section, and the loop spent 18 iterations at x=6,930-7,101
            // chasing 41 phantom fixups.
            // Only on ticks where a dash ring engaged, emit the same vy as
            // GD's dump (history and measurements at g_dashVy's declaration).
            // The state stays 0, so the trajectory is unchanged.
            // Fly-rings (speed.hpp): GD's row already carries the ring a ship's or UFO's press fires from this
            // state (the flag's note in speed.hpp); the step applies it on the next tick, so only
            // the display moves. The same gate as stepOne's, with the next tick's input.
            double rowVy = (double)s.vy;
            int rowFlip = (int)s.flip;
            if ((s.mode == 1 || s.mode == 3) && !s.dual && !s.jumpBuf
                && !s.pressSpent && !s.ctrlT && (!s.holdDead || !s.action)
                && !(g_stopJump && s.pcRel)) {
                int nextIn = curIn;
                for (size_t j = fxIdx; j < fx.size() && fx[j].first <= t + 1; ++j)
                    nextIn = fx[j].second;
                if (!g_ctrlWin.empty() && ctrlInputOffAt(t + 1, curIn != 0)) nextIn = 0;
                const Obj* ring = nextIn ? flyEarlyRing(s, K) : nullptr;
                if (ring) {
                    State d = s;
                    const bool hs = g_histStatOn;   // a look ahead for the row, not a fire
                    g_histStatOn = false;
                    const bool took = tryApplyFlyRing(d, ring, K);
                    g_histStatOn = hs;
                    if (took) {
                        rowVy = (double)d.vy;
                        rowFlip = (int)d.flip;
                    }
                }
            }
            const double vyGd = (g_dashVySet ? g_dashVy : rowVy)
                                * (s.frame == 3 ? -1.0 : 1.0);
            // ...and which line of step.hpp wrote this tick's vy, when asked.
            // Reported HERE rather than inside stepOne: this is the replay's own
            // per-tick loop, so it fires once per tick and cannot be confused
            // with the search running the same tick for many states.
            //
            // `writes` matters as much as `writer`. One write and the site is
            // the answer; several and the last one is only the last -- reading
            // it as "the site that decided the value" is the mistake the count
            // is here to prevent. Zero means vy was not written this step at all
            // and the value came from somewhere else entirely.
            // The vpNew trail is printed oldest first; `vpwrites` above eight
            // means the head of the sequence was overwritten.
            if ((g_vyWatchT >= 0 && t == g_vyWatchT)
                || (g_vyWatchT2 >= 0 && t == g_vyWatchT2)) {
                char vpt[96] = "-";
                const int nvp = g_vpWrites < 8 ? g_vpWrites : 8;
                for (int k = 0, o = 0; k < nvp; ++k)
                    o += std::snprintf(vpt + o, sizeof vpt - o, k ? ",%d" : "%d",
                                       g_vpTrail[(g_vpWrites - nvp + k) & 7]);
                char impt[96] = "-";
                const int nimp = g_impCount < 8 ? g_impCount : 8;
                for (int k = 0, o = 0; k < nimp; ++k)
                    o += std::snprintf(impt + o, sizeof impt - o, k ? ",%d" : "%d",
                                       g_impTrail[(g_impCount - nimp + k) & 7]);
                std::printf("vywriter: t=%lld writes=%d lastline=%d ywrites=%d ylastline=%d vyIn=%.6f vy=%.6f yIn=%.4f y=%.4f seatgate=%d impulsedOff=%d tappedOff=%d sOnSlope=%d took=%d impsite=%d "
                            "impcount=%d imptrail=%s vpwrites=%d vptrail=%s traceVy=%.6f frame=%d mode=%d\n",
                            (long long)t, g_vyWrites, g_vyWriter, g_yWrites, g_yWriter, (double)g_vyIn, (double)s.vy, (double)g_yIn, (double)s.y, g_seatGateSeen, g_seatImpulsedOff, g_seatTappedOff, g_seatOnSlope, g_seatTook, g_impulseSite,
                            g_impCount, impt, g_vpWrites, vpt, vyGd, (int)s.frame, (int)s.mode);
            }
            // --fxwatch: the fixup gate's own account of this step (fixup.hpp
            // fxDescribe). Kt is the step's tick as stepBoth saw it; it has to
            // equal t or the line describes some other step.
            if (g_fxWatchT >= 0 && t == g_fxWatchT)
                std::printf("fxwhy: t=%lld %s\n", (long long)t, g_fxWhy);
            tr << t << ',' << wX << ',' << wY << ',' << vyGd << ','
               << (int)s.mode << ',' << (int)s.grounded << ',' << (int)s.dual
               << ',' << s.y2 << ',' << s.vy2 << ',' << (int)s.flip2 << ','
               << curIn
               << ',' << (int)s.onSlope << ',' << s.slopeM << ','
               << (int)s.slopeT << ',' << s.bandFloor << ',' << s.bandCeil
               << ',' << (int)s.mini << ',' << (int)s.held << ',' << s.dx
               << ',' << (int)g_nearOrb
               << ',' << (*g_clampWhy ? g_clampWhy : "-")
               << ',' << g_clampUid << ',' << g_clampCx << ',' << g_clampCy
               << ',' << rowFlip << ',' << (int)s.frame
               // The player's sprite rotation. THE HITBOX TEST OF A TURNED
               // OBJECT IS DECIDED BY THIS, yet until now it was not in the
               // trace, so there was no way to measure "how far is the
               // model's rot from GD's" (measured on lv20: 1,158 degrees off
               // over a 281-tick anchor). Columns are appended at the end
               // (existing readers index by position).
               << ',' << s.rot << ',' << (int)s.rotNeg
               << ',' << (int)s.grounded2 << ',' << (int)s.onSlope2
               << ',' << s.slopeM2 << ',' << (int)s.slopeT2
               << ',' << (int)s.mode2 << ',' << (int)s.ceilT
               << ',' << (int)s.ceilT2 << ',' << (int)s.mini2
               << ',' << g_fxBlockedUid;
            if (g_traceState)
                tr << ',' << (double)s.vy * (s.frame == 3 ? -1.0 : 1.0) << ',' << (int)s.flip;
            tr << ',' << s.dx2;
            if (g_twoPlayer) tr << ',' << curIn2;
            tr << "\n";
            // --dumpstate (statefile.hpp): the state after this tick, whole, for a later call's --start-state
            if (g_dumpStateT >= 0 && t == g_dumpStateT && !rdead) {
                bool okRefs = true;
                const StateFile sf = captureState(s, t, rDxF, L, okRefs);
                const bool wrote = okRefs && writeStateFile(g_dumpStatePath, sf);
                std::printf("dumpstate: t=%lld x=%.4f y=%.4f mode=%d frame=%d -> %s%s\n", t, (double)s.xAbs,
                            (double)s.y, (int)s.mode, (int)s.frame, g_dumpStatePath.c_str(),
                            !okRefs ? " NOT WRITTEN (an object pointer outside the levels)"
                            : !wrote ? " NOT WRITTEN (write failed)" : "");
                // ...and the --start a call taking it up passes beside --start-state (its setup reads
                // --start before the file replaces the state): world x/y, frame and the per-body fields
                double dwx, dwy;
                fromFrame((int)s.frame, (double)s.xAbs, (double)s.y, dwx, dwy);
                // The hold field is the plan's button by PRESS tick before t (the loop's heldBefore: the
                // prefix it splices onto is cut at press < t), not s.held, which a cube never writes.
                int heldPress = 0;
                for (const Edge& e : edges) {
                    if (e.press >= t) break;
                    heldPress = e.v ? 1 : 0;
                }
                char ds[512];
                std::snprintf(ds, sizeof ds,
                              "%lld,%.6f,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%d,%d,0,%d,%d,%.6f,"
                              "-1,0,%d,%d,%.6f,%d,%d,%d,%d,%d,%d,%.6f",
                              t, dwx, dwy, (double)s.vy * (s.frame == 3 ? -1.0 : 1.0), (int)s.mode,
                              (int)s.grounded, heldPress, (int)s.flip, (int)s.mini, (int)s.dual, (double)s.y2,
                              (double)s.vy2, (int)s.flip2, (int)s.grounded2, (int)s.rHover, (int)s.dashing,
                              (double)s.dashSlope, (int)s.frame, (int)s.rev, (double)s.rot, (int)s.rotNeg,
                              (int)s.boost, (int)s.mode2, (int)s.mini2, (int)s.flipT, (int)s.armT,
                              (double)s.rotStep);
                std::printf("dumpstate-start: %s\n", ds);
                if (okRefs && wrote) {
                    g_outcome.dumpT = t;
                    g_outcome.dumpStart = ds;
                }
            }
            if (rdead) {
                // --replayon (diagnostic): note the first death and walk on, the
                // way the witness resim does, so geometry after it can be read.
                if (g_replayOn) {
                    if (diedT < 0) { diedT = t; diedX = (double)s.xAbs; }
                } else {
                    diedT = t;
                    diedX = (double)s.xAbs;
                    break;
                }
            }
            if ((double)s.xAbs >= goalX) break;
            if (eIdx < edges.size() && edges[eIdx].press == t) {
                size_t to = eIdx + 1;
                while (to < edges.size() && edges[to].press == t) ++to;
                scheduleTick(eIdx, to, t + latOf(s.mode), s.mode, fx);
                eIdx = to;
                // a tap's release lands a tick after its press: keep the effects in tick order
                if (g_sameTick)
                    std::stable_sort(fx.begin() + (long long)fxIdx, fx.end(),
                                     [](const auto& a, const auto& b) { return a.first < b.first; });
            }
            // the second body's edges of this tick, taken a tick (or two) later by ITS mode's latency
            while (eIdx2 < edges2.size() && edges2[eIdx2].press == t) {
                fx2.push_back({t + latOf(s.dual ? s.mode2 : s.mode), edges2[eIdx2].v});
                ++eIdx2;
            }
        }
        g_outcome.replayDiedT = diedT;
        if (g_histStatOn) {
            if (FILE* hf = std::fopen(g_histStatPath.c_str(), "a")) {
                std::fprintf(hf,
                             "histstat out=%s x=%.1f died=%lld touchMax=%d touchDrop=%d "
                             "portMax=%d portFull=%d padMax=%d padFull=%d orbFired=%d "
                             "orbDrop=%d orbRefire=%d padRefire=%d\n",
                             outPath.c_str(), (double)s.xAbs, diedT, g_hist.touchMax,
                             g_hist.touchDrop, g_hist.portMax, g_hist.portFull, g_hist.padMax,
                             g_hist.padFull, g_hist.orbFired, g_hist.orbDrop, g_hist.orbRefire,
                             g_hist.padRefire);
                std::fclose(hf);
            }
        }
        if (diedT >= 0) {
            std::printf("portseen6: slot overflow %lld\n", (long long)g_portSeenOverflow.load());
            std::printf("spdseenall: spill nodes %zu\n", spdSpillCount());   // --spdseenall: the pool is never freed, so its size is printed
            std::printf("REPLAY: model DIED at t=%lld x=%.1f (y=%.2f vy=%.3f "
                        "mode=%d grounded=%d dual=%d y2=%.2f vy2=%.3f held=%d)\n",
                        diedT, diedX, (double)s.y, (double)s.vy, (int)s.mode,
                        (int)s.grounded, (int)s.dual, (double)s.y2,
                        (double)s.vy2, (int)curIn);
            // The reason was already being recorded -- only --dbg (inside the
            // DP) ever read it, so a replay that died reported the state and
            // left "what killed it" to be guessed. A false death is exactly
            // the case where the object matters more than the state: measured
            // on lv20 t=786, the model and GD agree to 0.3 px for 786 ticks
            // and then only the model dies.
            if (*g_deadWhy) {
                std::printf("REPLAY: cause=%s", g_deadWhy);
                if (g_deadObj)
                    std::printf(" obj=uid%d id%d %gx%g @%.2f,%.2f (now %.2f,%.2f)",
                                g_deadObj->uid, g_deadObj->id,
                                2 * g_deadObj->hw, 2 * g_deadObj->hh,
                                (double)g_deadCx, (double)g_deadCy,
                                g_deadObj->cx, g_deadObj->cy);
                // WHICH copy of the level the killer came from. A moving object
                // exists once per gameplay frame that has been built, and only
                // the CURRENT frame's copy is seeked -- so a stale one reads as
                // a hazard frozen at its t=1 position.
                if (g_deadObj) {
                    const char* home = "?";
                    auto in = [&](const std::vector<Obj>& v) {
                        return !v.empty() && g_deadObj >= v.data()
                               && g_deadObj < v.data() + v.size();
                    };
                    if (in(L.objs)) home = "L.objs";
                    else if (in(L.dyn.objs)) home = "L.dyn";
                    else if (in(L.slopes)) home = "L.slopes";
                    else for (int fi = 1; fi < 4; ++fi)
                        if (g_frameLv[fi]) {
                            if (in(g_frameLv[fi]->objs)) { home = "frame.objs"; break; }
                            if (in(g_frameLv[fi]->dyn.objs)) { home = "frame.dyn"; break; }
                            if (in(g_frameLv[fi]->slopes)) { home = "frame.slopes"; break; }
                        }
                    std::printf(" from=%s", home);
                }
                std::printf("\n");
            }
        }
        else
            std::printf("REPLAY: SURVIVED to t=%lld x=%.1f (goal %.1f)\n",
                        lastT, (double)s.xAbs, goalX);
        // A --seeddump tick this walk never reached is reported as such. The
        // caller asked for the state at a tick; "the walk stopped first" is an
        // answer and silence is not.
        if (g_seedDump >= 0 && !sawSeedTick)
            std::printf("seedrotq: t=%d NOT REACHED - this walk ended at "
                        "t=%lld, so there is no state to seed from\n",
                        g_seedDump, lastT);
        std::printf("trace -> %s.trace.csv\n", outPath.c_str());
        std::printf("formularec: %lld formula-driven placements read the recording, %lld took "
                    "the formula (a box the state and the attempt disagree on)\n",
                    g_formulaRecHit.load(), g_formulaRecMiss.load());
        g_snapOut = nullptr;
        return 0;
    }
    // Where the search is going (search_dp.hpp CurrentRules): the level's end, with every coin
    // under --coins and the coin rules a child is held to, and the state a cut or a PARTIAL emits.
    struct CoinTables {
        decltype(L)& L;
        decltype(passedOn)& passedOn;
        decltype(tapGive)& tapGive;
        decltype(tapGateBits)& tapGateBits;
        decltype(gatedMask)& gatedMask;
        decltype(tapMissX)& tapMissX;
        decltype(tapMissY)& tapMissY;
        decltype(coinGate)& coinGate;
        decltype(tapSole)& tapSole;
        decltype(coinEnable)& coinEnable;
        decltype(coinReq)& coinReq;
        decltype(coinPruneOk)& coinPruneOk;
    };
    const CoinTables coinTables{L, passedOn, tapGive, tapGateBits, gatedMask, tapMissX,
                                tapMissY, coinGate, tapSole, coinEnable, coinReq, coinPruneOk};
    const CurrentRules<CoinTables> rules{goalX, coinOn, coinAll, coinTables};
    rules.init(init);   // the route's part of the root state (--coinmask, --coinskip)
    // (the root state is init from here on: a layer search's LayerWork starts its frontier there)
    // Announce the search to anything watching from another thread (dp/progress.hpp). The end
    // is announced from the guard below, so that every way out of the loop clears it.
    //
    // THE LAST LAYER THIS LOOP CAN PROCESS, which is not `t0 + horizon`. The horizon bounds the
    // PLAN, not the search: the loop runs on to t0 + 2*horizon and only then takes a survivor
    // (the survivor guarantee below), and it can never run past tEnd either way. Announcing
    // t0 + horizon gave the mod's search bar a denominator it is wrong on both sides of --
    // under the short horizon (3,000) the tick runs PAST it, and under the full one (level
    // length + 2,000) an anchored tail at t0=18,000 announced 44,000-odd while the loop stops
    // at tEnd around 21,000, so the bar could never fill. Nothing the CLI prints reads this
    // field (it is written here and read only through dpbridge::progress), so the printed
    // output is unchanged.
    g_progress.begin(t0, horizon > 0 ? std::min(tEnd, t0 + 2 * horizon) : tEnd);
    struct ProgressGuard { ~ProgressGuard() { g_progress.end(); } } progressGuard;
    // --phaseprof buckets: 0 group setup, 1 step, 2 dedupe/merge, 3 cap, 4 gc, 5 mem+checkpoints,
    // 6 the rest of the layer (progress, prints, next layer's top).
    double pp[7] = {0, 0, 0, 0, 0, 0, 0};
    auto ppLast = std::chrono::steady_clock::now();
    auto ppMark = [&](int b) {
        if (!g_phaseProf) return;
        const auto n = std::chrono::steady_clock::now();
        pp[b] += std::chrono::duration<double>(n - ppLast).count();
        ppLast = n;
    };
    const auto ppStart = std::chrono::steady_clock::now();
    // What a search method (search_dp.hpp searchLayers / searchBeam / searchMontecarlo) is handed,
    // in two parts:
    //   in       the call's inputs: the anchor, the horizon, the goal, the model, the thread pool,
    //            whether checkpoints are wanted;
    //   outcome  what the method leaves for the call: the deepest point, the goal state, the cap
    //            accounting and the counters printed after the search.
    // The plan comes back in SearchResult::inputs. How a method keeps its states is its own: the
    // three here keep them in a SearchArena, and the layer searches their frontier and dedupe
    // scratch in a LayerWork (search_dp.hpp).
    struct SearchIn {
        decltype(t0)& t0;
        decltype(dbgLayers)& dbgLayers;
        decltype(init)& init;
        decltype(horizon)& horizon;
        decltype(planCut)& planCut;
        decltype(goalX)& goalX;
        decltype(tEnd)& tEnd;
        decltype(curDxF)& curDxF;
        decltype(model)& model;
        decltype(pool)& pool;
        decltype(kParallelMin)& kParallelMin;
        decltype(ppMark)& ppMark;
        decltype(checkWanted)& checkWanted;
    };
    struct SearchOutcome {
        decltype(bestT)& bestT;
        decltype(bestX)& bestX;
        decltype(goalState)& goalState;
        decltype(solved)& solved;
        decltype(maxAlive)& maxAlive;
        decltype(capDropped)& capDropped;
        decltype(capHits)& capHits;
        decltype(cellCuts)& cellCuts;
        decltype(cellSpared)& cellSpared;
        decltype(cellTwinsKept)& cellTwinsKept;
        decltype(census)& census;
        decltype(twin)& twin;
    };
    struct SearchEnv {
        SearchIn in;
        SearchOutcome outcome;
    };
    // The autonomous triggers' fire state before the search, for --witnessauto's witness walk.
    struct AutoFire { int fireT; double fireX, lockLastX; };
    std::vector<AutoFire> autoBeforeSearch;
    for (const AutoTrig& A : g_autoTrig) autoBeforeSearch.push_back({A.fireT, A.fireX, A.lockLastX});
    // --resimladder (constants.hpp): every field the search writes into the autonomous and the
    // rotation triggers, before it runs -- a search from a later tick starts from these for every
    // trigger the plan had not crossed by then.
    struct RlAuto { int fireT; double fireX; int fireSpreadT; double lockLastX; };
    struct RlRot { int firedT, revT; };
    auto rlSaveTrig = [](std::vector<RlAuto>& a, std::vector<RlRot>& r) {
        a.clear();
        r.clear();
        for (const AutoTrig& A : g_autoTrig) a.push_back({A.fireT, A.fireX, A.fireSpreadT, A.lockLastX});
        for (const RotTrig& R : g_rotTrig) r.push_back({R.firedT, R.revT});
    };
    std::vector<RlAuto> rlAutoBefore;
    std::vector<RlRot> rlRotBefore;
    rlSaveTrig(rlAutoBefore, rlRotBefore);
    SearchResult found;
    {
        SearchEnv se{
            {t0, dbgLayers, init, horizon, planCut, goalX, tEnd, curDxF, model, pool, kParallelMin,
             ppMark, checkWanted},
            {bestT, bestX, goalState, solved, maxAlive, capDropped, capHits, cellCuts, cellSpared,
             cellTwinsKept, census, twin}};
        // The run's call index, counted for every search call in a fixed order: a search that
        // draws random numbers mixes it into its seed (search_dp.hpp SearchRun).
        ++g_run.calls;
        int rc = g_searchMethod == 1   ? searchBeam(se, rules, found)
                 : g_searchMethod == 2 ? searchMontecarlo(se, rules, found)
                                       : searchLayers(se, rules, found);
        // A search that declines this call (SearchResult::Declined) has touched nothing: the
        // default search answers instead, so the caller never reads a refusal as being stuck.
        // Under --capladder (cliMain's g_declineToLadder) the call ends here instead, and the
        // ladder answers it with the layer DP -- the answer the call gets without --search.
        if (rc < 0 && found.kind == SearchResult::Declined && g_declineToLadder) {
            std::printf("search: declined -- the layer DP answers this call under the cap ladder\n");
            g_declinedToLadder = true;
            return 0;
        }
        if (rc < 0 && found.kind == SearchResult::Declined) {
            std::printf("search: declined -- the layer DP answers this call\n");
            found = SearchResult{};
            rc = searchLayers(se, rules, found);
        }
        if (!found.deterministic) ++g_run.nondet;
        if (rc >= 0) return rc;
    }
    // Cap accounting, one line, parsed by the driver's tier ladder: a PARTIAL
    // with capHits=0 died of physics at full enumeration, so a bigger cap
    // cannot change the answer and the next tier is skipped.
    if (g_fireBCheck)
        std::printf("firebcheck: bit-without-tick=%llu tick-without-bit=%llu too-early=%llu"
                    " (both must be 0)\n", g_fireBNoTick, g_fireBNoBit, g_fireBTooEarly);
    std::printf("portseen6: slot overflow %lld\n", (long long)g_portSeenOverflow.load());
            std::printf("spdseenall: spill nodes %zu\n", spdSpillCount());   // --spdseenall: the pool is never freed, so its size is printed
    std::printf("capstat: maxAlive=%zu capHits=%lld dropped=%lld cap=%zu\n",
                maxAlive, capHits, capDropped, g_aliveCap);
    // --groupxsplit: this search's own counts (zeroed here, so the next search in the call
    // starts from 0).
    if (g_groupXSplit) {
        std::printf("groupxsplit: gap>2*%.0f+|dx| layers-split=%lld max-parts=%d\n",
                    kGroupWinMargin, g_groupXSplitLayers, g_groupXSplitMax);
        g_groupXSplitLayers = 0;
        g_groupXSplitMax = 0;
    }
    if (g_heldCellCap && g_heldAll)
        std::printf("cellcap: spared=%lld cut=%lld twinsKept=%lld (layers over the cap in states "
                    "whose cells fit / layers cut by cells / twins kept past the cap)\n",
                    cellSpared, cellCuts, cellTwinsKept);
    if (g_searchCensus > 0) census.print();
    twin.print();
    // --keycensus: the per-box tally, with the uid so it can be joined against
    // objrects. Boxes that never divided the key are printed as 0 rather than
    // omitted -- "absent" and "zero" are the distinction this project keeps
    // getting wrong, and a box that cost nothing is the interesting case here.
    // --coindbg: the closest approach per coin, whether or not it was taken.
    if (g_coinDbgT >= 0 && coinOn)
        for (size_t i = 0; i < L.coins.size() && i < 8; ++i) {
            // The denominator is on the line, always: "never entered" reads the
            // same whether no state entered the bound or the search never got
            // that far, and this project has mis-read that 0 six times in one
            // night. `seen` is how many states were examined inside the bound,
            // `reachedX` is the deepest x the coin test itself ever saw.
            if (g_coinNearDy[i] < 0.0) {
                std::printf("coinnear: coin=%zu at (%.0f,%.0f) NEVER ENTERED "
                            "(seen=%lld, the coin test reached x=%.0f, this "
                            "coin's x bound starts at %.0f)%s\n",
                            i, L.coins[i].cx, L.coins[i].cy, g_coinSeen[i],
                            g_coinProbeMaxX, L.coins[i].cx - L.coins[i].hw - 15.0,
                            (g_coinProbeMaxX < L.coins[i].cx - L.coins[i].hw - 15.0)
                                ? "  <-- NOT YET: the search never got this far"
                                : "  <-- a real zero: the search passed it and no "
                                  "state was inside the x bound");
                continue;
            }
            std::printf("coinnear: coin=%zu at (%.0f,%.0f) closest |dy|=%.3f at "
                        "t=%lld player=(%.3f,%.3f)  yBound=%.1f  seen=%lld%s\n",
                        i, L.coins[i].cx, L.coins[i].cy, g_coinNearDy[i],
                        g_coinNearT[i], g_coinNearX[i], g_coinNearY[i],
                        L.coins[i].hh + 15.0, g_coinSeen[i],
                        (g_coinNearDy[i] <= L.coins[i].hh + 5.0) ? "  <-- inside" : "");
        }
    if (g_keyCensus) {
        long long tot = 0;
        for (int b = 0; b < kTouchBits; ++b) tot += g_keyCount[b].load();
        std::printf("keycensus: total=%lld over %zu boxes\n", tot, g_touch.size());
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
            std::printf("keycensus: box=%zu uid=%d cx=%.0f moveTicks=%lld splits=%lld\n",
                        b, g_touch[b].uid, g_touch[b].cx,
                        (long long)(b < g_touchMoveTicks.size() ? g_touchMoveTicks[b] : -1),
                        (long long)g_keyCount[b].load());
    }
    clearReport();
    g_outcome.capHits = capHits;
    if (!g_fixups.empty()) {
        std::printf("fixups: %lld transitions overridden this call\n",
                    g_fixupHits);
        // ...and in which frame the overridden state was. A record's dy is
        // GD's world dy; a rotated state's y is not on that axis. Printed
        // whether or not any rotated hit occurred, because "the path exists
        // and never fired" is the answer this was built to be able to give.
        // CALLS first, then HITS, on one line each and never merged: a hit count
        // without its call count cannot be read (0 hits means "never matched"
        // or "never asked", and those are different findings).
        // (frame, rev) for every STEP and for every fixup LOOKUP, side by side.
        // The mod rewrites GD's frame 2 as (frame 0, rev 1), so frame 2 can
        // arrive under either name and a frame-only count cannot say which.
        const FixupCensusRow fc = fixupCensus();
        for (int f = 0; f < 4; ++f)
            for (int r = 0; r < 2; ++r)
                if (frameRevReach(f, r) || fc.call[f][r] || fc.hit[f][r])
                    std::printf("framerev: f%d rev%d steps=%lld lookups=%lld hits=%lld\n",
                                f, r, frameRevReach(f, r), fc.call[f][r], fc.hit[f][r]);
        const auto perFrame = [](const long long (&n)[4][2], int f) { return n[f][0] + n[f][1]; };
        std::printf("fixupcall:  f0=%lld f1=%lld f2=%lld f3=%lld rotated=%lld",
                    perFrame(fc.call, 0), perFrame(fc.call, 1),
                    perFrame(fc.call, 2), perFrame(fc.call, 3), fc.callRot);
        for (int i = 0, n = g_fixupCallRotSeen.load(); i < n; ++i)
            std::printf(" x=%.1f", g_fixupCallRotX[i]);
        std::printf("\n");
        std::printf("fixupframe: f0=%lld f1=%lld f2=%lld f3=%lld rotated=%lld",
                    perFrame(fc.hit, 0), perFrame(fc.hit, 1),
                    perFrame(fc.hit, 2), perFrame(fc.hit, 3), fc.hitRot);
        for (int i = 0, n = g_fixupRotSeen.load(); i < n; ++i)
            std::printf(" x=%.1f", g_fixupRotX[i]);
        std::printf("\n");
    }
    // The reference watch's verdict. Printed whether or not it was lost: a run
    // that carried the reference all the way is the negative control, and it has
    // to be as visible as a loss or the instrument only ever speaks when it has
    // something to blame.
    if (g_refWatch)
        std::printf("refwatch: %s%s%s%s\n",
                    g_refLostAt < 0 ? "CARRIED to the end"
                                    : "lost (see the line above)",
                    // Past an ADOPT the carrier is a substitute, not the reference.
                    g_refAdoptT >= 0 ? " -- a SUBSTITUTE lineage since t=" : "",
                    g_refAdoptT >= 0 ? std::to_string(g_refAdoptT).c_str() : "",
                    g_refAdoptT >= 0 ? (", overlooked:" + g_refAdoptFields).c_str() : "");
    if (g_phaseProf) {
        ppMark(6);
        const double tot = std::chrono::duration<double>(std::chrono::steady_clock::now() - ppStart).count();
        std::printf("phaseprof: layers=%lld total=%.2fs setup=%.2f step=%.2f dedupe=%.2f cap=%.2f "
                    "gc=%.2f mem=%.2f rest=%.2f prep=%.2f\n", bestT - t0, tot, pp[0], pp[1], pp[2],
                    pp[3], pp[4], pp[5], pp[6],
                    std::chrono::duration<double>(ppStart - cliT0).count());
        // what the setup bucket is made of (search_dp.hpp ppSub): the autonomous triggers' crossings, the group keys, the rotation triggers and the model's prepare of the moving geometry
        std::printf("phaseprof: setup= autotrig %.2f groupkeys %.2f rottrig+autoown %.2f prepare-rest %.2f (s)\n", g_ppSub[0], g_ppSub[1], g_ppSub[2], g_ppSub[3]);
        std::printf("phaseprof: prepare= seek %.2f applyTriggers %.2f near %.2f other-windows %.2f touch+coins+follow %.2f veto+solidorder %.2f (s)\n", g_ppSub[4], g_ppSub[5],
                    g_ppSub[6], g_ppSub[7], g_ppSub[8], g_ppSub[9]);
        std::printf("phaseprof: groups=%lld (%.2f a layer) parents stepped=%lld (%.1f a layer, %.1f a group)\n", g_ppGroups,
                    bestT > t0 ? (double)g_ppGroups / (double)(bestT - t0) : 0.0, g_ppParents,
                    bestT > t0 ? (double)g_ppParents / (double)(bestT - t0) : 0.0,
                    g_ppGroups ? (double)g_ppParents / (double)g_ppGroups : 0.0);
        std::printf("phaseprof: step blocks wall=%.2f s, children's work=%.2f thread-s, on %d threads => %.0f%% busy; %lld of the groups stepped on one thread\n",
                    (double)g_ppStepNs * 1e-9, (double)g_ppKidNs.load() * 1e-9, (int)g_threads,
                    g_ppStepNs > 0 ? 100.0 * (double)g_ppKidNs.load() / ((double)g_ppStepNs * (double)std::max(1, (int)g_threads)) : 0.0,
                    g_ppSerialGroups);
        if (g_stateCross) {
            std::printf("statecross: %lld late crossings noted, %lld records lost (xcOver)\n", (long long)g_crossNoted.load(),
                        (long long)g_crossOver.load());
            size_t nMove = 0, nHead = 0;
            for (size_t i = 0; i < g_crossHead.size(); ++i) {
                nMove += g_crossHead[i] >= 0;
                nHead += g_crossHead[i] == (int)i;
            }
            std::printf("statecross: %zu moving triggers at %zu x (--crosscx %s)\n", nMove, nHead, g_crossCxHeads ? "on" : "off");
        }
        const CenStats& cs = g_cen;
        std::printf("census: ticks=%lld always-objects/tick=%.0f | near unchanged=%lld moved=%lld | ahead unchanged=%lld moved=%lld | "
                    "behind unchanged=%lld moved=%lld (of which never-forward unchanged=%lld moved=%lld, can-catch-up unchanged=%lld moved=%lld) | "
                    "unchanged: anchor fired=%lld unfired=%lld\n", cs.ticks, cs.ticks ? (double)cs.n / (double)cs.ticks : 0.0,
                    cs.zone[0][0], cs.zone[0][1], cs.zone[1][0], cs.zone[1][1], cs.zone[2][0], cs.zone[2][1],
                    cs.never[0], cs.never[1], cs.behindCatch[0], cs.behindCatch[1], cs.idleFired, cs.idleUnfired);
    }
    if (g_rjOn)
        std::printf("rejoin: t0=%lld after=%lld reached=%lld layers=%lld exact=%lld(m%d,n%lld) "
                    "near2=%lld(m%d,n%lld) near8=%lld(m%d,n%lld)\n",
                    t0, g_rjAfter, bestT, g_rjLayers,
                    g_rjFirst[0], g_rjMode[0], g_rjLayersNear[0],
                    g_rjFirst[1], g_rjMode[1], g_rjLayersNear[1],
                    g_rjFirst[2], g_rjMode[2], g_rjLayersNear[2]);
    // ---- no answer before every checkpoint is judged (dp/progress.hpp) ----
    // The search is over, but the caller may still be flying checkpoints it published. Wait for
    // every one of them: if the game refutes one, the caller throws this whole call away, and an
    // answer handed back first would make the outcome depend on which of the two finished first.
    // The flights are the game replaying a few thousand ticks; the search is not blocked while
    // they run, only its answer is. `enabled` going off (the session ended) releases the wait.
    if (checkWanted) {
        while (g_check.enabled.load(std::memory_order_acquire)
               && !g_check.cancel.load(std::memory_order_acquire)
               && g_check.judged.load(std::memory_order_acquire) < g_check.published())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (g_check.cancel.load(std::memory_order_acquire)) {
            std::printf("CANCELLED: after the search, waiting on a checkpoint (search abandoned "
                        "by the caller)\n");
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = bestT;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;
        }
    }
    // The frontier died before the cut. That is still useful to the driver: the
    // deepest surviving branch is the best guess at how to get near the wall,
    // and replaying it in GD is how the wall gets localized at all. Emit it
    // (truncated to the cut) instead of nothing -- but only in horizon mode,
    // because a tail solve that returns a doomed plan would look like success
    // to the driver's `Test-Path $tail` check and the loop would never move.
    if (found.kind == SearchResult::Partial) {
        solved = true;
        goalState = found.goal;
        planCut = 0;   // no truncation: the driver wants to reach the wall
        std::printf("PARTIAL: frontier died at t=%lld x=%.1f, emitting the "
                    "deepest branch\n", found.t, found.x);
        // PARTIAL is announced BEFORE `solved` is forced true, so the SOLVED line below is
        // printed for it as well. The driver resolves that by letting PARTIAL win; the same
        // precedence is kept here (the SOLVED site does not overwrite a PARTIAL verdict)
        g_outcome.verdict = VerdictPartial;
        g_outcome.deepT = bestT;
        g_outcome.deepX = bestX;
    }
    // ...and whether it died at a coin's miss prune: the deepest x at the far edge of a coin whose
    // passing is final, where every state still without it is cut (the prune above: past cx + hw
    // by the player's half, at most 15, plus one tick's travel).
    // (a lambda: --resimladder asks it again of a PARTIAL it keeps)
    auto coinWallAt = [&](double x) -> int {
        if (!coinOn) return -1;
        for (size_t ci = 0; ci < L.coins.size() && ci < coinPruneOk.size(); ++ci) {
            if (!coinPruneOk[ci]) continue;
            const double edge = (double)L.coins[ci].cx + (double)L.coins[ci].hw;
            if (x >= edge - 5.0 && x <= edge + 40.0) {
                std::printf("coinwall: the frontier died at coin %zu's miss prune (x %.1f, far "
                            "edge %.1f)\n", ci, x, edge);
                return (int)ci;
            }
        }
        return -1;
    };
    if (g_outcome.deepX >= 0.0 && coinOn) {
        const int cw = coinWallAt(bestX);
        if (cw >= 0) g_outcome.coinWall = cw;
    }
    if (found.kind == SearchResult::NoPlan) {
        std::printf("FAILED: frontier died at t=%lld x=%.1f\n", found.t, found.x);
        g_outcome.verdict = VerdictFailed;
        g_outcome.deepT = bestT;
        g_outcome.deepX = bestX;
        // the last layer the frontier had, as the method handed it back (SearchResult::lastLayer)
        const std::vector<State>& prev = found.lastLayer;
        for (size_t i = 0; i < prev.size() && i < 12; ++i)
            std::printf("  prev[%zu]: y=%.2f vy=%.2f mode=%d g=%d held=%d\n", i,
                        (double)prev[i].y, (double)prev[i].vy, (int)prev[i].mode,
                        (int)prev[i].grounded, (int)prev[i].held);
        return 1;
    }
    // A PARTIAL comes through here too (above). The line's head is kept for the scripts that count it;
    // the verdict goes on its end, so a reader of a PARTIAL does not take this line for a solve
    // (AUD-290, 10-12: "the call SOLVED alone" was read off this line for a PARTIAL at t=5,432).
    if (g_outcome.verdict == VerdictPartial)
        std::printf("SOLVED at x=%.0f, reconstructing plan (verdict PARTIAL: the deepest branch, t=%lld "
                    "x=%.1f)\n", (double)goalState.y * 0 + goalX, (long long)g_outcome.deepT,
                    (double)g_outcome.deepX);
    else
        std::printf("SOLVED at x=%.0f, reconstructing plan\n", (double)goalState.y * 0 + goalX);
    // Everything past this line is the reconstruction, so DIE can tell the two
    // apart (see g_inRecon in constants.hpp). One aggregate line at the end.
    g_inRecon = true;
    if (g_outcome.verdict != VerdictPartial) g_outcome.verdict = VerdictSolved;

    // the per-tick input level, as the search method handed it back (SearchResult::inputs)
    std::vector<uint8_t> lvl = std::move(found.inputs);
    // truncate to the horizon cut (see planCut). lvl[i] is the input at tick
    // t0+i+1, so keeping (planCut - t0) entries ends the plan exactly at planCut.
    if (planCut > 0 && (long long)lvl.size() > planCut - t0)
        lvl.resize((size_t)std::max<long long>(0, planCut - t0));
    // --rejoinuse: the search stopped on the old plan's trajectory at g_rjJoinT; the rest is the
    // old plan's own input levels, read off its trace (act at tick k steps into tick k, which is
    // lvl[k - t0 - 1]). The witness walk below covers the joined plan whole.
    if (g_rjJoinT >= 0 && (long long)lvl.size() == g_rjJoinT - t0) {
        size_t added = 0;
        for (long long k = g_rjJoinT + 1;; ++k) {
            const auto it = g_rjRows.find(k);
            if (it == g_rjRows.end() || it->second.act < 0) break;
            lvl.push_back((uint8_t)it->second.act);
            ++added;
        }
        std::printf("rejoin: the plan carries on with %zu ticks of the old plan's inputs\n", added);
    } else if (g_rjJoinT >= 0) {
        std::printf("rejoin: the prefix is %zu ticks, not %lld - not joined\n", lvl.size(),
                    g_rjJoinT - t0);
    }
    std::vector<uint8_t> modeAt(lvl.size(), 0);  // filled by the resim below

    // --resimladder (constants.hpp): the walk's state after every tick and the kind of its first death,
    // and the triggers' fire state as the search left it -- what a search from a tick of this plan needs.
    std::vector<State> rlStates;
    std::vector<double> rlLockX;   // each autonomous trigger's lockLastX after each tick (the walk writes it)
    uint8_t rlKind0 = kDeathPhysics;
    uint8_t pubKind = kDeathPhysics;   // the same, carried through a resumed walk, for g_outcome only
    std::vector<RlAuto> rlAutoPlan;   // the triggers as the search behind the current plan left them
    std::vector<RlRot> rlRotPlan;
    rlSaveTrig(rlAutoPlan, rlRotPlan);

    // re-simulate the witness and write its per-tick trace (for diffing
    // against a GD dump to localize model divergences)
    auto witnessWalk = [&]() {
        std::ofstream tr(outPath + ".trace.csv");
        // 6 significant digits hid the thing we most often need to see: whether
        // x is a hair over or under a contact boundary. Print enough to compare
        // against the MOD's snaptrace, which gives 4 decimals.
        tr.precision(10);
        // dual/y2/vy2/flip2: the second body's track. lv16's late wall sat
        // undiagnosed for a whole session because the trace carried p1 only --
        // gd_diff said "full match" while GD was killing PLAYER 2 (the death
        // looked position-independent: same tick for every injected p1 y/x).
        // act: the input level of the tick (the fixup recorder gates on it).
        // ...and `flip` and `frame`, appended. --refwatch looks up its reference
        // columns by NAME (refwatch.hpp:108 asks for "flip" and "frame") and this
        // trace carried neither, so feeding a resim trace back to the search
        // reported `cannot-reproduce` with dy=0.000 dvy=0.000 -- the trajectory
        // matched to seven figures and the match was refused on two fields that
        // were not in the file. refwatch.hpp:42-46 already records that the frame
        // mismatch "accounted for most of the first sweep's cannot-reproduce
        // count"; this is the other half of that, on the writing side.
        //
        // Appended rather than inserted: every reader found is name-based
        // (mcp/gdmcp/data.py builds a csv.DictReader and validates the columns it
        // wants against r.fieldnames), and the positional reads in the tree index
        // into diff rows, regex groups and triggers.txt -- not this file.
        // ...and the ROTATION QUEUE's three per-state values. The anchor scan
        // does not seed them (frames.hpp:158-174), which is why --rotqueue is
        // opt-in and why --startrotq exists. The producer of a --startrotq seed
        // needs their value at the tick the next anchor will use, and this walk
        // passes every such tick: recording them here means the seed is READ
        // OFF a walk rather than DERIVED from something adjacent. Deriving is
        // what --spentrot has to do from the dump's frame transitions, and it
        // cannot see the entries that change no frame (2899 and chanOnly).
        // `dead`, appended last for the same reason as the columns above: the walk's own kill on
        // the tick, so a later reader (--rejoinuse) can tell the walk's deaths from its corpse
        // rows -- the walk runs on through a kill, and y/vy alone do not say where it fired.
        tr << "tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act,flip,frame"
              ",rotspent,rotchan,rotrev,dead,key\n";
        std::ofstream sn;
        if (!snapLogPath.empty()) {
            sn.open(snapLogPath);
            g_snapOut = &sn;
        }
        State s = init;
        // --start-state: the search has moved the world on since the file put it in place, and the walk
        // starts where the file's world stood (the moving geometry's last placement, the button's y)
        if (startStateLoaded) restoreStateWorld(startSF, L);
        // GAMEPLAY ROTATION: the witness resim did not turn. `applyRotation` had
        // exactly two callers -- the search's kid (:3108) and --replay (:2654) --
        // and this walk, which is the one that writes the trace everything is
        // diffed against, was not one of them. On lv22 the search is in the
        // rotated frame from t=6,000 (its layer line reads `x=0 y=[8272,8272]`,
        // which is the turned coordinate system) while this loop kept walking
        // +X. So the trace was not the plan's trajectory, and every death
        // counted here after a turn was the walk's, not the plan's.
        //
        // The slices have to be re-bound with the frame for the same reason
        // --replay re-binds (:2440): after a turn the geometry that matters is
        // the NEW frame's, and reading the old frame's is worse than not
        // turning at all. XSlice holds a reference, so it cannot be assigned --
        // hence the pointers, which is also how --replay carries them.
        // --witnessframe (default off): bind the frame the call STARTS in, as
        // --replay does at its own Lf. Fixed at &L, a call anchored in a turned
        // frame walks frame 0's geometry: lv22 plan 466 (t0=6127, frame 1) fell
        // through uid 5845 at t=6162 where --replay lands, missed the turn at
        // 6299 and reported escapee-prune at 6505. The walk also fills modeAt,
        // which sets the emitted edges' latency, so this is not print-only.
        // (model.hpp FrameSlices: the witness keeps its own, as the replay does.)
        FlyBand rBand;
        size_t rIdx = 0;
        // Fallback speed only: replayed by x for an anchor that carries no
        // measured multiplier, and used solely to seed `init.dx` (the state's
        // own speed is what the resim actually runs on).
        float rDxF = kDxF;
        size_t rSpd = 0;
        auto rGate = [&](const Obj& o) { return o.cx - o.hw - kCubeHalf; };
        while (rSpd < L.speeds.size() && rGate(L.speeds[rSpd]) <= x0) {
            rDxF = dxForSpeedId(L.speeds[rSpd].id);
            ++rSpd;
        }
        // GD's measured speed at the anchor beats replay-by-x here too, same
        // as the search's curDxF above. Without it, any re-anchor whose
        // replay-by-x speed differs from the measured one would run the
        // resim's LAYER x (collision windows) at the wrong rate while every
        // state's own xAbs ran at the right one -- an invisible desync (the
        // trace's x column prints s.xAbs). The lv16 anchors checked so far
        // happened to agree by luck; this closes the class.
        if (g_startSpeedMul > 0.0) rDxF = dxForSpeedMul(g_startSpeedMul);
        // The model walks the plan (model.hpp rollout): each tick's geometry is built as the
        // search's groups are, from THIS lineage's own x, speed, doors and frame, and the tick is
        // stepped and turned before anything below sees it. What the witness does with each tick
        // is here: before it is committed, the item gates and the death record; after, the row.
        // --witnessauto: from the triggers' state before the search, fired by this walk itself.
        if (g_witnessAuto || g_stateCross) {   // --statecross dates the states by their own crossings, so the walk does too
            for (size_t ai = 0; ai < g_autoTrig.size() && ai < autoBeforeSearch.size(); ++ai) {
                g_autoTrig[ai].fireT = autoBeforeSearch[ai].fireT;
                g_autoTrig[ai].fireX = autoBeforeSearch[ai].fireX;
                g_autoTrig[ai].lockLastX = autoBeforeSearch[ai].lockLastX;
            }
            g_rolloutFiresAuto = true;
        }
        struct RolloutAutoOff {
            ~RolloutAutoOff() { g_rolloutFiresAuto = false; }
        } rolloutAutoOff;
        // --walkresume (g_wrc, at the top of this file): take this walk up where an earlier attempt
        // of the same ladder left an identical one. Only where the cache can carry everything the
        // walk would have done: not with --witnessauto (the walk fires and prints), --snaplog or
        // --dyndbg (per-tick output), a joined plan (its leave line prints from the walk), the
        // reference watch, or gameplay rotation (other frames' geometry). Otherwise, and on any
        // miss, the walk runs from t0 as it does without the flag.
        // Never taken up now: the resim ladder (always on, formerly --resimladder) keeps every tick's state from t0,
        // so the walks of one ladder never share a checkpoint. The condition that used to gate it was
        // g_wrc.active && !g_witnessAuto && snapLogPath.empty() && g_dynDbg == -1 && g_rjJoinT < 0 && !g_refWatch
        // && g_rotTrig.empty(); g_wrc and the code under wrOn are the next thing to go.
        const bool wrOn = false;
        std::ostringstream trBuf;   // the rows, when they are kept or fingerprinted
        trBuf.precision(10);
        std::ostream& trOut = (wrOn || g_walkResumeCheck) ? static_cast<std::ostream&>(trBuf)
                                                          : static_cast<std::ostream&>(tr);
        bool wrSigOk = true;
        const uint64_t wrSig = wrOn ? walkWorldSig(model, t0, init, rDxF, wrSigOk) : 0;
        size_t wrFrom = 0;
        const long long wrFixupHits0 = g_fixupHits;   // what the search left; the walk adds to it
        bool wrPreBtnSeen = false;    // has the walk written g_preBtnY, and the last it wrote
        double wrPreBtnY = 0.0;
        std::vector<WalkCheckpoint> wrKeep;   // this walk's checkpoints, for the next attempt
        // why this walk was or was not taken up (--walkresumecheck prints it)
        const char* wrWhy = !wrOn ? "not taken up here"
                            : !wrSigOk ? "a start-state pointer encodeObj cannot name"
                            : g_wrc.t0 != t0 ? "no earlier walk in this ladder"
                            : g_wrc.worldSig != wrSig ? "the world it starts from differs"
                            : "no checkpoint in the shared prefix";
        size_t wrCommon = 0;
        if (wrOn && wrSigOk && g_wrc.t0 == t0 && g_wrc.worldSig == wrSig) {
            size_t common = 0;
            while (common < lvl.size() && common < g_wrc.lvl.size() && lvl[common] == g_wrc.lvl[common])
                ++common;
            wrCommon = common;
            // ...and frame 0's turnedBox latch now as it was at the kept tick: a latch the walk
            // up there would have set that this world has not (it never has: the search seeks
            // every tick a plan can reach) would otherwise be lost.
            uint64_t stickyNow = 1469598103934665603ull;
            model.levelOf(0).dyn.foldSticky(stickyNow, false);
            // ...and short of the plan's end: the walk's last step leaves its per-step records
            // (stepOne's g_yIn / g_vyIn and the writers' counts) behind, so a walk taken up at its
            // very end would leave the search's instead.
            const WalkCheckpoint* cp = nullptr;
            for (const WalkCheckpoint& k : g_wrc.cps)
                if (k.i <= common && k.i < lvl.size() && k.sticky == stickyNow && (!cp || k.i > cp->i))
                    cp = &k;
            if (cp) {
                wrWhy = "taken up";
                s = cp->s;
                s.snapObj = model.decodeObj(cp->objRefs[0]);
                s.usedOrb = model.decodeObj(cp->objRefs[1]);
                s.snapObj2 = model.decodeObj(cp->objRefs[2]);
                s.usedOrb2 = model.decodeObj(cp->objRefs[3]);
                for (int k = 0; k < 4; ++k) s.usedPad[k] = model.decodeObj(cp->objRefs[4 + k]);
                for (int k = 0; k < 4; ++k) s.usedPad2[k] = model.decodeObj(cp->objRefs[8 + k]);
                for (size_t a = 0; a < g_autoTrig.size() && a < cp->autoLockLastX.size(); ++a)
                    g_autoTrig[a].lockLastX = cp->autoLockLastX[a];
                model.loadWorld(0, cp->carry, t0 + (long long)cp->i);
                model.levelOf(0).dyn.resetCursors();
                g_resimDead = cp->resimDead;
                g_resimTicks = cp->resimTicks;
                g_dieRecon = cp->dieRecon;
                g_resimFirst = cp->resimFirst;
                g_resimLast = cp->resimLast;
                g_resimUid = cp->resimUid;
                g_resimFrame = cp->resimFrame;
                g_resimWhy = cp->resimWhy;
                pubKind = cp->resimKind;
                g_resimPX = cp->resimPX;
                g_resimObjX = cp->resimObjX;
                g_resimObjY = cp->resimObjY;
                g_resimTrig = cp->resimTrig;
                g_dieReconWhy = cp->dieReconWhy;
                // The last death's record is the walk's only once the walk has had one; before
                // that it is whatever this thread's share of the search left, as it is unbroken.
                if (cp->dieRecon > 0) {
                    g_deadWhy = cp->deadWhy;
                    g_deadObj = model.decodeObj(cp->deadObjRef);
                    g_deadCx = cp->deadCx;
                    g_deadCy = cp->deadCy;
                }
                g_fixupHits = wrFixupHits0 + cp->fixupHitsWalk;
                if (cp->preBtnSeen) {
                    g_preBtnY = cp->preBtnY;
                    wrPreBtnSeen = true;
                    wrPreBtnY = cp->preBtnY;
                }
                // first used inside the part taken up: use it now, which prints its line
                if (cp->bandTrackCam >= 0 && g_bandTrackCam < 0) (void)bandTrackIsCamera();
                for (size_t k = 0; k < cp->i && k < modeAt.size(); ++k) modeAt[k] = g_wrc.modeAt[k];
                trBuf.write(g_wrc.trace.data(), (std::streamsize)cp->traceLen);
                wrFrom = cp->i;
                for (const WalkCheckpoint& k : g_wrc.cps)
                    if (k.i <= cp->i) wrKeep.push_back(k);
            }
        }
        auto wrPre =
            [&](size_t i, long long t, const State& prev, State& c, bool rdead, const StepCtx& K) {
                // this tick's step wrote it (stepBoth clears the flag); tracked for the check as well
                if ((wrOn || g_walkResumeCheck) && g_preBtnSet) {
                    wrPreBtnSeen = true;
                    wrPreBtnY = g_preBtnY;
                }
                // --walkgates: the item gates the search's own step fires (itemGates),
                // so the witness walks the world the search planned in. Same gate as
                // the search's: alive, and not on a tick that turned the player.
                model.fireGates(c, prev.action, (int)lvl[i], K, (int)prev.frame, t, rdead);
                // ...and rdead is not read. It was declared, passed, and dropped:
                // the witness resim walks the whole plan whether or not the player
                // survived it, so a plan that dies at tick 40 of 1,200 still writes
                // 1,160 rows of a corpse and the run still prints SOLVED (which was
                // printed before this loop, by the search). Count it here rather
                // than at DIE: this is the ONE walk of the FINAL plan, so a death
                // counted here is the plan's, with none of the search's prunes in
                // it. First and last are recorded too, because "one corpse dying
                // every tick" and "many separate deaths" produce the same total and
                // are told apart only by whether the ticks are contiguous.
                if (rdead) {
                    if (g_resimDead == 0) {
                        g_resimFirst = (int)t;
                        g_resimWhy = g_deadWhy;
                        rlKind0 = g_deadKind;   // --resimladder: physics, rule or prune
                        pubKind = g_deadKind;
                        // ...and where the player was, in world x, for --vetophys's deepX
                        // (the loop's PARTIAL gate compares deepX with the verified x).
                        {
                            double wxR, wyR;
                            fromFrame((int)c.frame, (double)c.xAbs, (double)c.y, wxR, wyR);
                            g_resimPX = (float)wxR;
                        }
                        // ...and the object, from the same DIE that set the cause.
                        // g_deadObj is the killer; its uid is the level's, not this
                        // build's ordinal, so it can be looked up in the dump.
                        g_resimUid = g_deadObj ? g_deadObj->uid : -1;
                        g_resimObjX = g_deadCx;
                        g_resimObjY = g_deadCy;
                        g_resimTrig = prev.trig;
                        // ...and the frame those coordinates are expressed in.
                        g_resimFrame = (int)c.frame;
                    }
                    g_resimLast = (int)t;
                    ++g_resimDead;
                }
                ++g_resimTicks;
            };
        auto wrPost = [&](size_t i, long long t, const State& s, bool rdead) {
                modeAt[i] = s.mode;
                {   // the state after tick t0 + i + 1
                    if (rlStates.size() <= i) rlStates.resize(i + 1);
                    rlStates[i] = s;
                    const size_t nA = g_autoTrig.size();
                    if (rlLockX.size() < (i + 1) * nA) rlLockX.resize((i + 1) * nA);
                    for (size_t a = 0; a < nA; ++a) rlLockX[i * nA + a] = g_autoTrig[a].lockLastX;
                }
                trOut << t << ',' << (double)s.xAbs << ',' << s.y << ',' << s.vy << ','
                   << (int)s.mode << ',' << (int)s.grounded << ',' << (int)s.dual
                   << ',' << s.y2 << ',' << s.vy2 << ',' << (int)s.flip2 << ','
                   << (int)lvl[i] << ',' << (int)s.flip << ',' << (int)s.frame
                   << ',' << s.rotSpent << ',' << (int)s.rotChan
                   << ',' << (unsigned)s.rotRev << ',' << (rdead ? 1 : 0)
                   << ',' << (unsigned long long)keyOf(s, (long long)t) << "\n";
                // --rejoinuse: past the join the walk must retrace the old plan's walk -- the same
                // fields on every tick the old trace has, and no kill it did not have. The first tick
                // it does not is the join failing its own premise (repair.hpp refuses the join).
                if (g_rjJoinT >= 0 && (long long)t > g_rjJoinT && g_outcome.rejoinBadT < 0) {
                    const auto ot = g_rjRows.find((long long)t);
                    if (ot != g_rjRows.end()) {
                        const RefRow& o = ot->second;
                        const char* why = nullptr;
                        if ((o.mode >= 0 && (int)s.mode != o.mode) || (o.flip >= 0 && (int)s.flip != o.flip)
                            || (o.frame >= 0 && (int)s.frame != o.frame))
                            why = "mode/flip/frame";
                        else if (std::fabs((double)s.y - o.y) > g_refEps
                                 || std::fabs((double)s.vy - o.vy) > g_refEps)
                            why = "y/vy";
                        else if (rdead && o.dead == 0)
                            why = "a kill the old walk did not have";
                        if (why) {
                            g_outcome.rejoinBadT = (long long)t;
                            g_outcome.rejoinBadWhy = why;
                            std::printf("rejoin: the joined walk leaves the old one at t=%lld (%s)\n",
                                        (long long)t, why);
                        }
                    }
                }
                // --walkresume: what the next tick reads and the lines after the walk report, as
                // of the end of this tick.
                if (wrOn && wrSigOk && (i + 1) % (size_t)g_walkResumeEvery == 0 && i + 1 > wrFrom) {
                    WalkCheckpoint k;
                    k.i = i + 1;
                    k.s = s;
                    bool ok = true;
                    k.objRefs[0] = model.encodeObj(s.snapObj, ok);
                    k.objRefs[1] = model.encodeObj(s.usedOrb, ok);
                    k.objRefs[2] = model.encodeObj(s.snapObj2, ok);
                    k.objRefs[3] = model.encodeObj(s.usedOrb2, ok);
                    for (int q = 0; q < 4; ++q) k.objRefs[4 + q] = model.encodeObj(s.usedPad[q], ok);
                    for (int q = 0; q < 4; ++q) k.objRefs[8 + q] = model.encodeObj(s.usedPad2[q], ok);
                    model.saveWorld(0, k.carry);
                    k.autoLockLastX.reserve(g_autoTrig.size());
                    for (const AutoTrig& A : g_autoTrig) k.autoLockLastX.push_back(A.lockLastX);
                    k.resimDead = g_resimDead;
                    k.resimTicks = g_resimTicks;
                    k.dieRecon = g_dieRecon;
                    k.resimFirst = g_resimFirst;
                    k.resimLast = g_resimLast;
                    k.resimUid = g_resimUid;
                    k.resimFrame = g_resimFrame;
                    k.resimWhy = g_resimWhy;
                    k.resimKind = pubKind;
                    k.resimPX = g_resimPX;
                    k.resimObjX = g_resimObjX;
                    k.resimObjY = g_resimObjY;
                    k.resimTrig = g_resimTrig;
                    k.dieReconWhy = g_dieReconWhy;
                    k.deadWhy = g_deadWhy;
                    k.deadObjRef = model.encodeObj(g_deadObj, ok);
                    k.deadCx = g_deadCx;
                    k.deadCy = g_deadCy;
                    k.fixupHitsWalk = g_fixupHits - wrFixupHits0;
                    k.preBtnSeen = wrPreBtnSeen;
                    k.preBtnY = wrPreBtnY;
                    k.bandTrackCam = g_bandTrackCam;
                    k.traceLen = (size_t)trBuf.tellp();
                    k.sticky = 1469598103934665603ull;
                    model.levelOf(0).dyn.foldSticky(k.sticky, false);
                    if (ok) wrKeep.push_back(std::move(k));
                }
            };
        // g_deadObj is thread_local and this thread has just run the search, so what it holds here is the last kill of
        // some search step: an object of a world that is gone by now. A death the walk meets that no DIE names (the
        // recording's end, say) leaves it alone, and wrPre read `g_deadObj->uid` -- freed memory, an access violation in
        // about one run in four of a custom level's last call (the game went down with it). Start the walk without one:
        // a death names its killer through DIE, and one that does not has none. (A walk resumed from a checkpoint
        // brings its own.)
        if (wrFrom == 0) g_deadObj = nullptr;
        if (wrFrom > 0) {
            const std::vector<uint8_t> tail(lvl.begin() + (std::ptrdiff_t)wrFrom, lvl.end());
            s = model.rollout(s, t0 + (long long)wrFrom, tail, rDxF,
                [&](size_t i, long long t, const State& prev, State& c, bool rdead, const StepCtx& K) {
                    wrPre(i + wrFrom, t, prev, c, rdead, K);
                },
                [&](size_t i, long long t, const State& st, bool rdead) { wrPost(i + wrFrom, t, st, rdead); });
        } else {
            s = model.rollout(s, t0, lvl, rDxF, wrPre, wrPost);
        }
        if (wrOn) {   // this walk is what the ladder's next attempt can take up
            if (wrSigOk) {
                g_wrc.t0 = t0;
                g_wrc.worldSig = wrSig;
                g_wrc.lvl = lvl;
                g_wrc.trace = trBuf.str();
                g_wrc.modeAt = modeAt;
                g_wrc.cps = std::move(wrKeep);
            } else {
                g_wrc.t0 = -1;
                g_wrc.cps.clear();
            }
        }
        if (wrOn || g_walkResumeCheck) {
            const std::string rows = trBuf.str();
            tr << rows;
            if (g_walkResumeCheck) {
                // every global resetInvocationState covers (g_wrc and these two flags are not
                // among them, so the runs with and without --walkresume print the same)
                // Less two counters that differ between two runs of the same call without the
                // flag at all (measured): g_fixupHits is incremented from the pool's threads
                // without synchronisation, and g_seatGateSeen is per thread, so both count the
                // scheduling as much as the walk. And g_preBtnY only when the walk wrote it: it is
                // per thread, so one the walk never wrote is the search's leftover on this thread
                // (measured: three runs of one call without the flag left 736.5, 745.459 and
                // 745.678 after a walk that did not write it).
                std::string prof;
                {
                    const std::string all = defaultsProfile();
                    size_t p = 0;
                    while (p < all.size()) {
                        size_t q = all.find(' ', p);
                        if (q == std::string::npos) q = all.size();
                        const std::string tok = all.substr(p, q - p);
                        if (tok.rfind("g_preBtnY=", 0) == 0 && !wrPreBtnSeen)
                            prof += "g_preBtnY=(not written by the walk) ";
                        else if (tok.rfind("g_fixupHits=", 0) != 0 && tok.rfind("g_seatGateSeen=", 0) != 0)
                            prof += tok + ' ';
                        p = q + 1;
                    }
                }
                uint64_t hp = walkFnv(1469598103934665603ull, prof.data(), prof.size());
                {   // the globals themselves, one file per walk of the process, for a diff when
                    // the two runs' fingerprints part
                    static int s_walkN = 0;
                    std::ofstream pf(outPath + ".walkprof" + std::to_string(++s_walkN) + ".txt");
                    pf << prof;
                }
                uint64_t ht = walkFnv(1469598103934665603ull, rows.data(), rows.size());
                uint64_t hm = walkFnv(1469598103934665603ull, modeAt.data(), modeAt.size());
                std::printf("walkstate: t0=%lld ticks=%zu trace=%016llx modeAt=%016llx globals=%016llx\n",
                            t0, lvl.size(), (unsigned long long)ht, (unsigned long long)hm,
                            (unsigned long long)hp);
                std::printf("walkresume: %s (%s; plan prefix shared with the kept walk %zu), took %zu of "
                            "%zu ticks from an earlier attempt, kept %zu checkpoints; in a ladder %d, "
                            "cache %s\n",
                            wrOn ? "on" : "off", wrWhy, wrCommon, wrFrom, lvl.size(),
                            wrOn ? g_wrc.cps.size() : (size_t)0,
                            g_ladder ? 1 : 0,
                            (!g_ladder && (!g_wrc.cps.empty() || g_wrc.active)) ? "LEFT OVER" : "ok");
            }
        }
        g_snapOut = nullptr;
    };
    // --resimladder (constants.hpp): the plan's own walk died of physics. Search again from the walk's
    // state a rung before that death (the world sought afresh there, the triggers as the plan had
    // fired them by then), splice the answer onto the plan's prefix, and walk the whole plan again
    // from t0. A later death moves the ladder there and starts its rungs again; the call keeps the
    // plan whose walk dies latest, or is clean, and everything that walk left (the trace, modeAt, the
    // resim counters), so what follows the walk reads that plan's walk exactly.
    auto resimLadder = [&]() {
        // (a joined plan too: the plan the ladder keeps is spliced from the walk, a plain plan --
        // the join is cleared with it, and put back with the joined plan if nothing is kept)
        // ...and a PARTIAL whose walk dies short of its frontier's death by more than a rung: the plan
        // is there to take GD to the wall, and GD would die where the walk does instead. A search
        // from the walk may answer with a PARTIAL too; the plan kept is the one whose walk gets
        // furthest alive, and the call publishes the verdict of the search that made it. (Both only
        // under --resimladderpartial.)
        if (g_resimDead == 0 || g_resimFirst <= t0 || rlKind0 != kDeathPhysics) return;
        if (g_searchMethod != 0) return;
        if (!g_resimLadderPartial && g_outcome.verdict != VerdictSolved) return;
        if (g_resimLadderHold && g_outcome.verdict == VerdictPartial) {
            std::printf("resimladder: the walk dies at t=%d; a PARTIAL in a cap-ladder attempt is left "
                        "to the ladder\n", g_resimFirst);
            return;
        }
        if (!(g_outcome.verdict == VerdictSolved
              || (g_outcome.verdict == VerdictPartial
                  && (long long)g_resimFirst + kResimLadderRungs[0] < g_outcome.deepT)))
            return;
        struct Walked {
            std::vector<uint8_t> lvl, modeAt;
            std::string trace;
            long long dead = 0, ticks = 0, dieRecon = 0, planCut = 0;
            int first = -1, last = -1, uid = -1, frame = -1;
            const char* why = nullptr;
            float px = 0.f, ox = 0.f, oy = 0.f;
            TouchMask trig{};
            std::vector<std::pair<const char*, long long>> dieReconWhy;
            long long rjJoinT = -1, rejoinT = -1, rejoinBadT = -1;
            const char* rejoinBadWhy = nullptr;
        };
        auto keep = [&](Walked& w) {
            w.lvl = lvl;
            w.modeAt = modeAt;
            std::ifstream in(outPath + ".trace.csv", std::ios::binary);
            w.trace.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            w.dead = g_resimDead; w.ticks = g_resimTicks; w.dieRecon = g_dieRecon; w.planCut = planCut;
            w.first = g_resimFirst; w.last = g_resimLast; w.uid = g_resimUid; w.frame = g_resimFrame;
            w.why = g_resimWhy; w.px = g_resimPX; w.ox = g_resimObjX; w.oy = g_resimObjY;
            w.trig = g_resimTrig; w.dieReconWhy = g_dieReconWhy;
            w.rjJoinT = g_rjJoinT; w.rejoinT = g_outcome.rejoinT;
            w.rejoinBadT = g_outcome.rejoinBadT; w.rejoinBadWhy = g_outcome.rejoinBadWhy;
        };
        auto putBack = [&](const Walked& w) {
            lvl = w.lvl;
            modeAt = w.modeAt;
            { std::ofstream o(outPath + ".trace.csv", std::ios::binary); o << w.trace; }
            g_resimDead = w.dead; g_resimTicks = w.ticks; g_dieRecon = w.dieRecon; planCut = w.planCut;
            g_resimFirst = w.first; g_resimLast = w.last; g_resimUid = w.uid; g_resimFrame = w.frame;
            g_resimWhy = w.why; g_resimPX = w.px; g_resimObjX = w.ox; g_resimObjY = w.oy;
            g_resimTrig = w.trig; g_dieReconWhy = w.dieReconWhy;
            g_rjJoinT = w.rjJoinT; g_outcome.rejoinT = w.rejoinT;
            g_outcome.rejoinBadT = w.rejoinBadT; g_outcome.rejoinBadWhy = w.rejoinBadWhy;
        };
        // a search from tick tr of the current plan; `out` is the plan's lvl[0..tr - t0) and its answer
        // ...and what that search said of it: whether it reached the goal or the horizon (a PARTIAL's
        // answer is the deepest branch, for a PARTIAL call only), which of the two, and how deep its
        // frontier got. A search that was cancelled stops the ladder.
        long long rlCut = 0, rlDeepT = -1;
        double rlDeepX = -1.0;
        bool rlSolved = false, rlPartial = false, rlHorizon = false, rlCancelled = false;
        // (hzProbe > 0: a probe, searched to that horizon instead of the call's -- --rlbound)
        auto researchFrom = [&](long long tr, std::vector<uint8_t>& out, long long hzProbe = 0) -> bool {
            rlSolved = rlPartial = rlHorizon = rlCancelled = false;
            rlDeepT = -1;
            const size_t ip = (size_t)(tr - t0);
            const size_t nA = g_autoTrig.size();
            if (ip == 0 || ip > rlStates.size() || ip > lvl.size() || rlLockX.size() < ip * nA) return false;
            const State sTr = rlStates[ip - 1];
            // the triggers: as the search behind this plan fired them, less every firing past tr
            for (size_t a = 0; a < nA && a < rlAutoPlan.size() && a < rlAutoBefore.size(); ++a) {
                AutoTrig& A = g_autoTrig[a];
                const RlAuto& p = rlAutoPlan[a];
                const bool kept = rlAutoBefore[a].fireT >= 0
                                  || (p.fireT >= 0 && (long long)p.fireT - A.delay <= tr);
                const RlAuto& u = kept ? p : rlAutoBefore[a];
                A.fireT = u.fireT;
                A.fireX = u.fireX;
                A.fireSpreadT = u.fireSpreadT;
                // --rlspread0: a crossing at or before tr belongs to the one state the search starts from, so every
                // group of it shares that tick; the spread the first search measured (the frontier's laggards) is not theirs
                if (g_resimLadderSpread0 && A.fireT >= 0 && (long long)A.fireT - A.delay <= tr) A.fireSpreadT = 0;
                A.lockLastX = rlLockX[(ip - 1) * nA + a];
            }
            for (size_t r = 0; r < g_rotTrig.size() && r < rlRotPlan.size() && r < rlRotBefore.size(); ++r) {
                RotTrig& R = g_rotTrig[r];
                R.firedT = (rlRotPlan[r].firedT >= 0 && (long long)rlRotPlan[r].firedT <= tr)
                               ? rlRotPlan[r].firedT : rlRotBefore[r].firedT;
                R.revT = (rlRotPlan[r].revT >= 0 && (long long)rlRotPlan[r].revT <= tr)
                             ? rlRotPlan[r].revT : rlRotBefore[r].revT;
            }
            for (int f = 0; f < 4; ++f)
                if (model.frameBound(f)) model.forgetWorld(f);
            const long long t0s = t0, cutS = planCut;
            const State initS = init;
            const float dxS = curDxF;
            const bool rjOnS = g_rjOn, rjUseS = g_rjUse;
            // ...and the first search's outcome, which the lines after the walk still read
            const long long bestTS = bestT, capHitsS = capHits, capDroppedS = capDropped;
            const double bestXS = bestX;
            const bool solvedS = solved;
            const State goalS = goalState;
            const size_t maxAliveS = maxAlive;
            const long long cellCutsS = cellCuts, cellSparedS = cellSpared, cellTwinsS = cellTwinsKept;
            // ...and the published outcome whole: the search writes horizonCut, capHits and the work
            // count into it, and none of that is the plan's until the plan is kept
            const auto outS = g_outcome;   // (auto: cliMainOnce has a SearchOutcome of its own)
            g_outcome.horizonCut = false;
            t0 = tr;
            init = sTr;
            planCut = 0;
            if (sTr.dx > 0.f) curDxF = sTr.dx;
            bestT = tr;
            bestX = (double)sTr.xAbs;
            solved = false;
            goalState = State{};
            maxAlive = 0;
            capHits = capDropped = 0;
            cellCuts = cellSpared = cellTwinsKept = 0;
            g_rjOn = g_rjUse = false;   // a join would need the splice the first search's has
            g_inRecon = false;
            const bool noCheck = false;   // the caller's checkpoints belong to the first search's t0
            long long hzSearch = hzProbe > 0 ? hzProbe : horizon;
            SearchEnv se2{{t0, dbgLayers, init, hzSearch, planCut, goalX, tEnd, curDxF, model, pool,
                           kParallelMin, ppMark, noCheck},
                          {bestT, bestX, goalState, solved, maxAlive, capDropped, capHits, cellCuts,
                           cellSpared, cellTwinsKept, census, twin}};
            ++g_run.calls;
            SearchResult f2;
            const int rc = searchLayers(se2, rules, f2);
            g_inRecon = true;
            rlSolved = f2.kind == SearchResult::Solved;
            rlPartial = f2.kind == SearchResult::Partial;
            rlHorizon = g_outcome.horizonCut;
            rlCancelled = g_outcome.verdict == VerdictCancelled;
            rlDeepT = bestT;
            rlDeepX = bestX;
            rlCut = rlSolved ? planCut : 0;   // a PARTIAL is not cut: it is there to reach the wall
            t0 = t0s;
            init = initS;
            planCut = cutS;
            curDxF = dxS;
            g_rjOn = rjOnS;
            g_rjUse = rjUseS;
            bestT = bestTS;
            bestX = bestXS;
            solved = solvedS;
            goalState = goalS;
            maxAlive = maxAliveS;
            capHits = capHitsS;
            capDropped = capDroppedS;
            cellCuts = cellCutsS;
            cellSpared = cellSparedS;
            cellTwinsKept = cellTwinsS;
            g_outcome = outS;
            if (rc >= 0 || rlCancelled || f2.inputs.empty()) return false;
            if (!rlSolved
                && (f2.kind != SearchResult::Partial || g_resimLadderHold || !g_resimLadderPartial))
                return false;
            out.assign(lvl.begin(), lvl.begin() + (std::ptrdiff_t)ip);
            std::vector<uint8_t> tail = std::move(f2.inputs);
            if (rlCut > 0 && (long long)tail.size() > rlCut - tr) tail.resize((size_t)(rlCut - tr));
            out.insert(out.end(), tail.begin(), tail.end());
            return true;
        };
        Walked best;
        keep(best);
        const int first0 = best.first;
        long long deathAt = best.first;
        if (g_phaseProf)
            std::printf("resimladder: the walk of the plan dies at t=%d: %s uid=%d obj=(%.1f,%.1f) px=%.1f frame=%d trig=0x%llx mode=%d\n",
                        best.first, best.why ? best.why : "?", best.uid, (double)best.ox, (double)best.oy, (double)best.px,
                        best.frame, (unsigned long long)best.trig.word(0),
                        (best.first >= t0 && (size_t)(best.first - t0) < best.modeAt.size()) ? (int)best.modeAt[(size_t)(best.first - t0)] : -1);        int tries = 0;
        size_t rung = 0;
        bool bestOnDisk = true;   // the trace and the globals are best's
        std::vector<RlAuto> candAuto;
        std::vector<RlRot> candRot;
        const size_t nRungs = sizeof(kResimLadderRungs) / sizeof(kResimLadderRungs[0]);
        // the walk of a plan that replaces lvl starts from nothing: the globals it fills, cleared
        auto takePlan = [&](std::vector<uint8_t>&& plan) {
            lvl = std::move(plan);
            modeAt.assign(lvl.size(), 0);
            rlStates.clear();
            rlLockX.clear();
            rlKind0 = kDeathPhysics;
            pubKind = kDeathPhysics;
            g_resimDead = 0; g_resimTicks = 0; g_dieRecon = 0; g_dieReconWhy.clear();
            g_resimFirst = -1; g_resimLast = -1; g_resimUid = -1; g_resimFrame = -1;
            g_resimWhy = nullptr; g_resimPX = 0.f; g_resimObjX = 0.f; g_resimObjY = 0.f;
            g_resimTrig = TouchMask{};
            g_rjJoinT = -1;   // a plain plan: no old walk to retrace
            g_outcome.rejoinT = -1;
            g_outcome.rejoinBadT = -1;
            g_outcome.rejoinBadWhy = nullptr;
        };
        while (tries < kResimLadderMax && rung < nRungs) {
            const long long tr = deathAt - kResimLadderRungs[rung];
            if (tr <= t0) break;
            if (g_resimLadderBound > 0) {
                // --rlbound: the rung's probe. A search to the walk's death plus the bound, its
                // answer walked; a probe whose walk dies no later than the plan's is the rung's
                // answer, and only one that lives on is followed by the full search below.
                std::vector<uint8_t> pc;
                const long long hz = (deathAt - tr) + g_resimLadderBound;
                const bool pok = researchFrom(tr, pc, hz);
                if (rlCancelled) {
                    std::printf("resimladder: probe from t=%lld: cancelled\n", tr);
                    break;
                }
                if (!pok) {
                    std::printf("resimladder: probe from t=%lld (%lld before the walk's death at %lld, "
                                "horizon %lld): no plan - next rung\n",
                                tr, kResimLadderRungs[rung], deathAt, hz);
                    ++rung;
                    continue;
                }
                const long long pcut = planCut;
                takePlan(std::move(pc));
                planCut = rlCut;
                witnessWalk();
                bestOnDisk = false;
                const bool pclean = g_resimDead == 0;
                const long long preach = pclean ? t0 + (long long)lvl.size() : (long long)g_resimFirst;
                const bool plater = (pclean || rlKind0 == kDeathPhysics) && preach > best.first;
                planCut = pcut;
                std::printf("resimladder: probe from t=%lld (%lld before the walk's death at %lld, "
                            "horizon %lld): %zu ticks, walk %s%s\n", tr, kResimLadderRungs[rung],
                            deathAt, hz, lvl.size(),
                            pclean ? "clean" : ("dies at t=" + std::to_string(g_resimFirst)).c_str(),
                            plater ? " - the full search follows" : " - not later, next rung");
                if (!plater) {
                    ++rung;
                    continue;
                }
            }
            ++tries;
            std::vector<uint8_t> cand;
            if (!researchFrom(tr, cand)) {
                // A search that got past the walk's death and then died short of the goal says the
                // death can be avoided from here and the goal still not reached: what stops this plan
                // lies further on, and a search from further back meets it again. With only a SOLVED
                // answer to keep, the ladder stops there. On a custom level three walks spent all
                // four rungs that way, about 20 s a search, 147 s of a 439 s run, and kept nothing.
                // ("past" is further than the ladder reaches back: on 4001 a frontier dying 8 ticks
                // past a death at 20,522 was the same stretch, and the search from 96 before passed)
                const bool past = rlPartial && !g_resimLadderPartial
                                  && rlDeepT > deathAt + kResimLadderRungs[nRungs - 1];
                char why[96];
                if (rlCancelled)
                    std::snprintf(why, sizeof(why), "cancelled");
                else if (rlPartial)
                    std::snprintf(why, sizeof(why), "PARTIAL to t=%lld%s", rlDeepT,
                                  past ? ", past the death - no rung further back" : "");
                else
                    std::snprintf(why, sizeof(why), "no plan");
                std::printf("resimladder: from t=%lld (%lld before the walk's death at %lld): %s\n",
                            tr, kResimLadderRungs[rung], deathAt, why);
                if (rlCancelled || past) break;
                ++rung;
                continue;
            }
            rlSaveTrig(candAuto, candRot);
            takePlan(std::move(cand));
            const long long cutS = planCut;
            planCut = rlCut;
            witnessWalk();
            bestOnDisk = false;
            const bool clean = g_resimDead == 0;
            // how far the walk got alive: its first death, or the end of the plan. A PARTIAL's plan
            // ends at its own frontier's death, and one that ends within a rung of where the walk
            // died is that same wall (4001: walk dead at 18,485, the search's frontier at 18,488)
            const long long reach = clean ? t0 + (long long)lvl.size() : (long long)g_resimFirst;
            const long long same = (clean && !rlSolved) ? kResimLadderRungs[0] : 0;
            const bool later = (clean || rlKind0 == kDeathPhysics) && reach > best.first + same;
            char what[64] = "";
            if (!rlSolved) std::snprintf(what, sizeof(what), "PARTIAL to t=%lld, ", rlDeepT);
            std::printf("resimladder: from t=%lld (%lld before the walk's death at %lld): %s%zu ticks, "
                        "walk %s%s\n", tr, kResimLadderRungs[rung], deathAt, what, lvl.size(),
                        clean ? "clean" : ("dies at t=" + std::to_string(g_resimFirst)).c_str(),
                        !later ? " - not later" : clean ? " - kept" : " - later, kept");
            if (later) {
                keep(best);
                bestOnDisk = true;
                rlAutoPlan = candAuto;
                rlRotPlan = candRot;
                // the call's verdict is the kept plan's search's
                if (rlSolved) {
                    g_outcome.verdict = VerdictSolved;
                    g_outcome.horizonCut = rlHorizon;
                    g_outcome.deepT = -1;
                    g_outcome.deepX = -1.0;
                    g_outcome.coinWall = -1;
                } else {
                    g_outcome.verdict = VerdictPartial;
                    g_outcome.horizonCut = false;
                    g_outcome.deepT = rlDeepT;
                    g_outcome.deepX = rlDeepX;
                    g_outcome.coinWall = coinWallAt(rlDeepX);
                }
                if (clean) break;
                deathAt = g_resimFirst;
                rung = 0;
            } else {
                planCut = cutS;
                ++rung;
            }
        }
        if (!bestOnDisk) putBack(best);
        std::printf("resimladder: %d search(es); the walk died at t=%d, now %s\n", tries, first0,
                    best.dead == 0 ? "clean" : ("at t=" + std::to_string(best.first)).c_str());
    };
    witnessWalk();
    resimLadder();

    if (!g_bandPath.empty()) {
        std::ofstream bo(g_bandPath);
        bo << "tick,alive,ylo,yhi,x,capdrop,merged,classes\n";
        for (const BandRow& b : g_bands)
            bo << b.t << ',' << b.alive << ',' << b.ylo << ',' << b.yhi << ','
               << b.x << ',' << b.capdrop << ',' << b.merged << ',' << b.cls
               << "\n";
        std::printf("bands: %zu layers -> %s\n", g_bands.size(),
                    g_bandPath.c_str());
    }
    // --rejoinprobe (refwatch.hpp): a join whose own walk dies within the probe's ticks of the join is
    // not flown -- this call searches again without --rejoinuse, and that answer is the call's.
    if (g_rejoinProbe > 0 && g_rjJoinT >= 0 && g_resimDead > 0 && g_resimFirst >= 0
        && (long long)g_resimFirst >= g_rjJoinT
        && (long long)g_resimFirst - g_rjJoinT <= g_rejoinProbe && !t_rejoinRetry) {
        std::printf("rejoin: the joined walk dies at t=%d, %lld ticks past the join -- searching "
                    "again without the join\n", g_resimFirst, (long long)g_resimFirst - g_rjJoinT);
        std::fflush(stdout);
        std::vector<char*> p;
        for (int i = 0; i < argc; ++i)
            if (std::strcmp(argv[i], "--rejoinuse")) p.push_back(argv[i]);
        struct Retry {
            Retry() { t_rejoinRetry = true; }
            ~Retry() { t_rejoinRetry = false; }
        } retry;
        return cliMainOnce((int)p.size(), p.data());
    }
    // emit input edges. Latency is PER MODE (measured on lv1): cube presses
    // act on the next tick (+1), ship/UFO two ticks later (+2).
    std::ofstream out(outPath);
    g_outcome.planWritten = true;   // ...and the trace above with it (cliMain's step 5 keeps both)
    // prev is the button state the plan STARTS from, and on a re-anchor that is
    // `--start`'s held field, not 0. Hard-coding 0 dropped the very first edge
    // whenever a tail was solved from a HELD anchor: the tail's own model
    // released on its first tick, and the edge that release needs was never
    // written, so the emitted plan could not reproduce the trace the same run
    // had just printed.
    //
    // Measured 2026-08-09, lv20 cold, anchor t0=717 (mini wave, held from
    // t=698). The tail's trace turns the dart down at t=718 and needs
    // `input=717,0` (wave latency 1). Nothing was emitted; the driver's splice
    // guessed `input={t0-1},0` = 716 instead, one tick early, which is 2 ticks
    // of the dart's dy = 5.193 px of permanent offset. Replaying the SAME tail
    // in GD with only that seam moved:
    //
    //   input=716,0  ->  death t=723 x=938.64   (16 ticks short)
    //   input=717,0  ->  death t=739 x=959.42   = the tail's own horizon, and
    //                    GD matches the tail's y to 4 decimals at every tick
    //
    // That one tick was the whole "grind": the tail was right, the plan that
    // carried it was not, GD died early, and the driver logged the miss as a
    // `kill` fixup -- 11 of them inside a 44 px band in 13 iterations, none of
    // which was a physics gap. Emitting the edge here (instead of letting the
    // caller guess) also puts the latency where the latency logic already
    // lives: `latOf` below is per-mode, and the driver's fixed t0-1 is only
    // ever right for ship/UFO (lat 2).
    // The scan itself now lives in planEdges (top of this file), because the common-prefix
    // publisher inside the layer loop has to produce the same edges for the same ticks. The
    // note below is about the RULE, and is kept here where the plan is written.
    int edges = 0;
    {
        // ...and the UFO is mode 3, which this test was missing: it got the
        // cube's +1 and every flap in the plan came out one tick late in GD.
        // Measured on lv12: pressed at t=170, GD flapped at t=172.
        // The mode that decides the latency is the one the input is applied
        // UNDER, i.e. the mode at the START of that tick -- modeAt[i] is
        // the mode at its END, which on a mode-portal tick is already the
        // NEW one. lv19 t=19,525 is exactly that tick: the input is a
        // WAVE's (lat 1) but the state ends as a UFO (lat 2), the edge was
        // emitted one tick early, and GD turned the wave down one tick
        // before the model did. The model then floated 1.3 px higher,
        // fired the UFO portal at (27,315,333) that GD does not reach, and
        // every cold run oscillated between x=27,297 and x=27,328.
        // The mode that decides the latency is the one GD is in when the
        // BUTTON is pressed, which is `lat` ticks before the effect -- not
        // modeAt[i], the mode the tick ENDS in. On a mode-portal tick those
        // differ, and lv19 t=19,525 is exactly that tick: the input is a
        // WAVE's (lat 1) but the state ends as a UFO (lat 2), so the edge
        // went out one tick early, GD turned the wave down a tick before
        // the model did, the model floated 1.3 px higher and fired the UFO
        // portal at (27,315,333) that GD never reaches. Every cold run
        // oscillated between x=27,297 and x=27,328.
        // Resolved in two passes because `lat` picks its own lookback:
        // start from the mode at the START of the effect tick, then take
        // the mode at the tick that guess points at.
        const std::vector<PlanEdge> pe =
            planEdges(lvl, modeAt, t0, planPrevHeld(init), init.mode);
        for (const PlanEdge& e : pe) {
            // --p2shared: the search world pressed both bodies' buttons together (no ghost, no swap), and the plan says so
            const int bits = (g_p2Shared && g_twoPlayer && !g_ghostOn && !g_swapBodies && g_bodyBit == 1) ? (e.level ? 3 : 0) : (e.level << (g_bodyBit - 1));
            out << "input=" << e.press << ',' << bits << "\n";   // --bodybit: the second player's levels are bit 1
            ++edges;
        }
        // ...and remembered for the run (search_dp.hpp SearchRun): a verification compares the
        // plan it flew with this one.
        g_run.lastT0 = t0;
        g_run.lastEdges.clear();
        for (const PlanEdge& e : pe) g_run.lastEdges.emplace_back(e.press, e.level);
    }
    // AFTER the reconstruction, not with `capstat:`. A non-zero count means the
    // plan this run just reported does not survive its own replay: the search
    // reached the goal, and rebuilding that path then died.
    // [2026-09-08] This print started life beside `capstat:` at :3898, which is
    // FIFTY LINES BEFORE the reconstruction begins, so it read the counter
    // before anything could increment it and reported n=0 for every case --
    // including the one where the reconstruction death had already been
    // observed directly. The `inRecon` field is what caught it: it printed 0
    // where it had to be 1, which is why a probe should report the state it
    // depends on and not only its answer (the same reason `pogate` prints its
    // seven conjuncts).
    std::printf("recondie: n=%lld inRecon=%d last=%s\n",
                g_dieRecon, g_inRecon ? 1 : 0,
                (g_dieRecon && g_deadWhy) ? g_deadWhy : "-");
    // ...and the split, because the total is not a number about physics: the
    // `out-of-play` bound is a search prune. Every cause is listed, so the
    // reader is not left inferring the population from the last entry.
    for (const auto& e : g_dieReconWhy)
        std::printf("recondiewhy: %s=%lld\n", e.first ? e.first : "?", e.second);
    // ...and the walk itself. `contig` is the test, not a decoration: if the
    // dead ticks are the tail of the walk, one plan died once and the rest is
    // its corpse; if they are not, the total was never one number about one
    // event. `of` is here so a zero can be told from a walk that never ran.
    // READ THE COUNTERS ONCE. The printf and the publish below used to read the
    // globals separately, twenty lines apart, and that is enough to make the two
    // channels disagree about the same call: nothing serialises cliMain (there
    // is no lock in dpcore::solveInProcess, three call sites in repair.hpp, and
    // this project has already met a solver thread that outlived its session --
    // the orphaned-worker case), so a second walk mutating g_resim* between the
    // two reads publishes a number no printf ever emitted.
    //
    // That is not hypothetical arithmetic. In the 2026-09-08 cold run
    // result.txt carries `resimdie=10@19554` and BOTH logs -- the suite's stdout
    // capture and Geode's own -- hold 423 resimdie lines each, agree on 23 with
    // dead>0, and contain no `dead=10` at all. The cause was never pinned down;
    // this removes the one mechanism that could produce it from here.
    const long long rDead = g_resimDead, rOf = g_resimTicks;
    const int rFirst = g_resimFirst, rLast = g_resimLast;
    const char* rWhy = g_resimWhy;
    // --vetophys (default off). A SOLVED whose witness walk dies of a physical
    // cause is the model agreeing that the route dies, which is what the repair
    // loop means by PARTIAL (checkPhantom: "a PARTIAL tail is the model agreeing
    // that the route dies, which is not a phantom"). So publish it as the PARTIAL
    // at that death, and let the loop's own PARTIAL gate decide whether to play
    // it. The witness is bound to its own frame's geometry (--witnessframe,
    // always on since the flag clean-up), and only physical causes count -- a search prune
    // (escapee-prune, deadband, maxplayy, out-of-play) is not the model's
    // physics. The emitted tail is not cut, as for any PARTIAL.
    if (g_vetoPhys && rDead > 0 && g_outcome.verdict == VerdictSolved && rWhy) {
        static const char* const kPhys[] = {"cube/hazard", "wave/hazard", "fly/hazard",
                                            "spider/tp-hazard", "cube/solid-side",
                                            "fly/solid-side", "crush"};
        bool phys = false;
        for (const char* p : kPhys)
            if (!std::strcmp(rWhy, p)) phys = true;
        if (phys) {
            g_outcome.verdict = VerdictPartial;
            g_outcome.deepT = rFirst;
            g_outcome.deepX = g_resimPX;
            std::printf("vetophys: SOLVED -> PARTIAL at t=%d x=%.1f (witness %s, start frame %d)\n",
                        rFirst, (double)g_resimPX, rWhy, (int)init.frame);
        }
    }
    std::printf("gfirestat: groups=%lld spread=%lld sum=%lld max=%d\n",
                g_gfireGroups, g_gfireSpread, g_gfireSum, g_gfireMax);
    std::printf("resimdie: dead=%lld of=%lld first=%d last=%d contig=%d why=%s\n",
                rDead, rOf, rFirst, rLast,
                (rDead > 0 && (long long)(rLast - rFirst + 1) == rDead) ? 1 : 0,
                rWhy ? rWhy : "-");
    if (g_resimDead > 0)
        std::printf("resimwho: uid=%d obj=(%.1f,%.1f) frame=%d trig=0x%08llx\n",
                    g_resimUid, g_resimObjX, g_resimObjY, g_resimFrame,
                    (unsigned long long)g_resimTrig.word(0));
    // --verdictinfo (default off): one line per SOLVED call, carrying that call's
    // verdict beside its OWN witness walk's death. Print only -- the verdict is not
    // touched here and the plan is not withheld. The direction is "send the plan,
    // attach the known death as information" (audit 05:02), which is why this sits
    // after the verdict is settled and changes nothing about it.
    //
    // Emitted for EVERY SOLVED call, dead=0 included, so the line carries its own
    // denominator. "How many SOLVED plans had a dying witness" cannot be read off
    // lines that exist only when one died -- the same discipline as the resimdie=?
    // note above: a value that is missing must be distinguishable from a zero.
    //
    // Reads the SNAPSHOT (rDead/rOf/rFirst/rLast/rWhy) rather than g_resim*. Nothing
    // serialises cliMain, and re-reading those globals here is exactly the mechanism
    // the READ THE COUNTERS ONCE note removed after the 2026-09-08 cold run published
    // a resimdie=10 that no printf ever emitted. uid/frame are read once, right here,
    // and only when there was a death to describe.
    if (g_verdictInfo && g_outcome.verdict == VerdictSolved) {
        const int vUid   = (rDead > 0) ? g_resimUid   : -1;
        const int vFrame = (rDead > 0) ? g_resimFrame : -1;
        std::printf("vinfo: verdict=SOLVED dead=%lld of=%lld first=%d last=%d "
                    "why=%s uid=%d objframe=%d startframe=%d\n",
                    rDead, rOf, rFirst, rLast,
                    (rDead > 0 && rWhy) ? rWhy : "-",
                    vUid, vFrame, (int)init.frame);
    }
    // Push this call's tail out NOW. stdout is a pipe here (Geode captures it), so it is
    // block-buffered: everything printed after the search loop -- capstat: at the top of
    // this block through resimdie:/resimwho:/vinfo: -- sits in the buffer until the NEXT
    // call's search output happens to fill it. Every call but one is rescued by its
    // successor; the last call of a run has none, so its whole tail dies with the process
    // and a verdict that result.txt records is simply absent from the log. Measured on
    // 2026-09-12: one SOLVED call's block missing, which made a healthy instrument look
    // like a broken one and cost that run its (1).
    //
    // The two existing flushes (the gc line and memstat) are both INSIDE the search loop,
    // so neither covers this block, and nothing in the tree calls setvbuf.
    //
    // Outside the --verdictinfo guard on purpose: the loss is a property of the block, not
    // of that flag. Inside it, a run with the flag off would keep losing its tail while the
    // flagged case looked fixed -- and the run that found this had the flag on.
    //
    // Not behind a flag of its own: this changes no output byte, only when the bytes leave
    // the buffer. A default-off flag would leave the instrument broken by default, which is
    // the opposite of what defaulting off protects.
    std::fflush(stdout);
    // ...and out through the struct, because the repair loop -- the one consumer
    // that ACTS on these plans -- reads dp::g_outcome, not stdout. A walk that
    // never ran stays -1 and must not be counted as a clean plan.
    //
    // The printf is not lost in the mod, though 98c36ed's message says it is:
    // Geode captures stdout into its own log, and the whole first cold run of
    // this counter was read out of there. What dp_bridge.hpp:57 says is that
    // there is no pipe to the PYTHON DRIVER, which is a narrower claim than the
    // one that got written down. The value of this field is correlation -- it
    // sits on the iteration's own line beside capHits and the verdict -- not
    // that the number would otherwise be unobtainable.
    //
    // Note the gate below is one-way, and the asymmetry is load-bearing when the
    // two channels disagree: the printf above is unconditional, so "printed but
    // not published" happens (that is exactly what `resimdie=?` means) and
    // "published but not printed" cannot come from this path.
    if (rOf > 0) {
        g_outcome.resimDead = rDead;
        g_outcome.resimFirst = rFirst;
        g_outcome.resimWhy = rWhy;   // a string literal: static, and dp is
                                     // linked into the mod, so it outlives
                                     // the call the way the others do not
        if (rDead > 0) g_outcome.resimKind = pubKind;
        // The killer travels with the cause. Read from the same locals-once
        // discipline as the counters above, and left at their reset values when
        // the walk found nothing -- `uid=-1` is "no death here", not "unknown".
        g_outcome.resimUid = g_resimUid;
        g_outcome.resimObjX = g_resimObjX;
        g_outcome.resimObjY = g_resimObjY;
        g_outcome.resimTrig = g_resimTrig;
        g_outcome.resimFrame = g_resimFrame;
    }
    std::printf("plan: %d edges, %zu ticks -> %s\n", edges, lvl.size(),
                outPath.c_str());
    return 0;
}

// --capladder <c0>: solve with a small alive cap first, and raise it only where that search runs
// out of states.
//
// Measured 2026-09-25 on the official lv1-15 from t=0 (one search each, the loop's cap 2000 as
// the reference): cap 125 reaches the end on 13 of 15 levels with 0.068-0.099 of the stepped
// children, and where it does not (lv11, lv12) the frontier dies at one or two narrow passages.
// Raising the cap over a window starting some distance BEFORE the passage gets through it
// (lv12: 1000 from >= 300 px before, not 100; lv11: 250 from >= 600 px), because the passage
// needs states that have to be kept on the way to it, not at it. Which cap a passage needs also
// depends on what the search kept earlier (lv11's second passage fails with 250 from x=12,111 to
// the end, and passes with 250 from 3,000 px before it), so each attempt is a whole search again
// rather than a resumed one.
//
// The attempts:
//   1. --cap c0 everywhere.
//   2. The frontier died at x_w: add a window [x_w - 600, x_w + 300] at 4 * c0, search again.
//   3. Died inside a window again: that window's cap x4 (at most --cap) and its lead x2. Died
//      outside every window: a new window at the new wall. Windows are kept, since each attempt
//      starts from t0.
//   4. Died inside a window that already has the full --cap, or more than kLadderMaxAttempts
//      attempts: the plain search with the caller's own arguments, i.e. exactly what runs
//      without the flag. Also straight after an attempt whose alive cap never bound (capHits
//      0): the attempts after it only raise caps, and a cap no layer reaches cuts nothing (the
//      layer loop truncates only a layer over it), so each would be that search again. In the
//      official levels with coins, 311 of the ladders' 764 attempts came after one.
//   5. The plain search's frontier died too, on an EARLIER tick than the deepest attempt's did:
//      that attempt is searched again (the first attempt to reach that tick, with its own map)
//      and its result is the ladder's. The alive cap is not monotone -- measured 2026-09-26 with
//      --heldall, the plain cap-2000 search died at lv21 t=1,961 and lv19 t=2,317 where the
//      ladder's attempts had reached t=13,930 and t=11,350 -- and without this step the ladder
//      handed back the shallower plan. "Deeper" is the tick the frontier died on (deepT), not x,
//      which runs backwards in a reversed section; on a tie the plain search's result stands.
//      A death is still a death either way (PARTIAL/FAILED): this picks which one, not whether.
// Anything that is not a frontier death (SOLVED, a horizon cut, CANCELLED, an early return)
// ends the ladder with that attempt's result. Every attempt writes the same output files, so
// the last search's are the ones left behind -- which is why step 5 searches again rather than
// keeping copies: the plan, its trace and g_outcome all come from that one search.
//
// A search restriction only: every attempt steps the model exactly as a plain search does.
inline constexpr int kLadderMaxAttempts = 8;
inline constexpr double kLadderLead = 600.0;
inline constexpr double kLadderPast = 300.0;

// The plain search that ends a ladder, run somewhere else and started with the ladder rather
// than after it. Its arguments are the caller's own plus --gridmap -1e9:1 -- nothing the attempts
// find out goes into them -- so it can start before the first attempt, and the ladder takes its
// result exactly where it would have searched it itself (step 4); a ladder that ends without it
// throws it away. What the ladder hands back is the same either way; only the time differs. The
// mod supplies the second search from a second copy of this core (src/mod/dp_bridge2.cpp): the
// solver's state is all in globals, so two searches cannot share one copy. Its printed lines are
// held and come out where this copy's plain search would have printed them.
struct PlainElsewhere {
    // Start the plain search with these arguments (argv[0] included). False = not available for
    // this call, and the ladder searches it itself.
    bool (*start)(const std::vector<std::string>& argv) = nullptr;
    // The ladder has come to it: wait for it, put its output files where this call's own go and
    // its outcome into g_outcome, and return its exit code.
    int (*take)() = nullptr;
    // The ladder ended without it: stop it and throw it away.
    void (*drop)() = nullptr;
    // Set: the instrument that checks the claim above. The ladder searches the plain search
    // itself as it does without any of this, then hands its exit code here to be compared with
    // the one run elsewhere, which is thrown away; take() is not called.
    void (*check)(int rc) = nullptr;
    // Set: THE GAMBLE, which gives up the claim above for time. Asked after every attempt with
    // the deepest tick an attempt has died on so far (bestT; -1 while none has): true when the
    // plain search has finished and either solved or died no earlier than that, and the ladder
    // then takes it at once instead of searching on -- stopping an attempt in progress through
    // g_ladderStop, which the other side raises when the same holds as it finishes. An attempt
    // still to come could have got further or solved; which of the two finishes first depends on
    // the machine, so the answer does too.
    bool (*ready)(long long bestT) = nullptr;
};
inline PlainElsewhere g_plainElsewhere;

inline int cliMainBody(int argc, char** argv) {
    long long c0 = 0;
    std::vector<std::string> base;
    for (int i = 0; i < argc; ++i) {
        if (i + 1 < argc && !std::strcmp(argv[i], "--capladder")) {
            c0 = std::atoll(argv[++i]);
            continue;
        }
        // the ladder writes its own maps; a caller's would be overwritten by them anyway
        if (i + 1 < argc && !std::strcmp(argv[i], "--capmap")) { ++i; continue; }
        if (i + 1 < argc && !std::strcmp(argv[i], "--gridmap")) { ++i; continue; }
        base.push_back(argv[i]);
    }
    if (c0 <= 0) return cliMainOnce(argc, argv);
    // --ladderprefix (search_dp.hpp LadderPrefixCache): read here, where the ladder is; each attempt's
    // own parse would reset it with the rest of the call's state. On by default; --no-ladderprefix
    // is the off arm, and --ladderprefixcheck (debug) turns it on whatever else is given.
    bool lpLadder = true, lpCheck = false;
    for (const std::string& s : base) {
        if (s == "--ladderprefix") lpLadder = true;
        if (s == "--no-ladderprefix") lpLadder = false;
        if (s == "--ladderprefixcheck") lpCheck = true;
    }
    if (lpCheck) lpLadder = true;
    g_lpc = LadderPrefixCache{};
    // --ladderprefixmaxmib <n>: the cap on all the saves together (LadderPrefixCache::maxMiB); a test sets it low.
    for (size_t i = 0; i + 1 < base.size(); ++i)
        if (base[i] == "--ladderprefixmaxmib") g_lpc.maxMiB = (size_t)std::atoll(base[i + 1].c_str());
    // --walkresume (g_wrc): for this ladder only. Whatever is in it now came from another call,
    // which is the leak the cache's lifetime rules out; --walkresumecheck says so if it happens.
    bool wrLadder = false, wrCheck = false;
    for (const std::string& s : base) {
        if (s == "--walkresume") wrLadder = true;
        if (s == "--walkresumecheck") wrCheck = true;
    }
    if (wrCheck)
        std::printf("walkresume: at the ladder's entry the cache holds %zu checkpoints (%s)\n",
                    g_wrc.cps.size(), g_wrc.cps.empty() && !g_wrc.active ? "empty" : "LEFT OVER");
    g_wrc = WalkResumeCache{};
    g_wrc.active = wrLadder;
    // The attempts share one level (LadderLevelCache), for this ladder only.
    static unsigned long long s_ladders = 0;
    g_ladder = ++s_ladders;
    g_ladderBestT.store(-1);
    g_ladderStop.store(false);
    struct EndLadder {
        ~EndLadder() {
            g_ladder = 0;
            g_ladderLevel = LadderLevelCache{};
            g_lpc = LadderPrefixCache{};   // --ladderprefix: the saves and the kept arena
            g_wrc = WalkResumeCache{};     // --walkresume: the kept walk, for this ladder only
            g_ladderBestT.store(-1);
            g_ladderStop.store(false);
        }
    } endLadder;
    // Another search (--search, the last one given): it answers the call by itself, and the ladder
    // runs only for a call it declines (g_declineToLadder) -- with the layer DP, as without
    // --search. The attempt shares the level the declined call read (same arguments).
    {
        int method = 0;
        for (size_t i = 0; i + 1 < base.size(); ++i)
            if (base[i] == "--search") method = base[i + 1] == "dp" ? 0 : 1;
        if (method) {
            std::vector<char*> p;
            for (std::string& s : base) p.push_back(s.data());
            struct Clear {
                ~Clear() { g_declineToLadder = false; }
            } clear;
            g_declineToLadder = true;
            g_declinedToLadder = false;
            const int rc = cliMainOnce((int)p.size(), p.data());
            g_declineToLadder = false;
            if (!g_declinedToLadder) return rc;
            base.push_back("--search");
            base.push_back("dp");
        }
    }
    // --inputgrid under the ladder: a wall the grid makes (a press it cannot place) looks to the
    // ladder exactly like a wall the cap makes, so the plain search that ends the ladder runs
    // without the grid (--gridmap -1e9:1). The windows keep it.
    // TRIED AND REVERTED (2026-09-25): lifting the grid inside every window as well (a --gridmap
    // over the stretches whose cap was raised). On lv3/11/12/16/22 and two custom levels at
    // --capladder 125 --inputgrid 2 it cost 199M stepped children against 147M with the grid kept
    // (lv11 3 -> 5 attempts, the custom level 2 -> 4): the lifted window searched finer and took other
    // routes, and none of the seven had needed it -- all were SOLVED with the grid kept.
    // The full cap is whatever the caller's --cap (or the default) leaves g_aliveCap at, which
    // is known once an attempt has parsed the arguments; the first attempt always runs.
    long long fullCap = -1;
    const std::string noGrid = "-1e9:1";
    // The plain search, started elsewhere now if the caller can (PlainElsewhere); dropped on every
    // way out of the ladder that does not take it.
    struct Elsewhere {
        bool on = false, taken = false;
        ~Elsewhere() {
            if (on && !taken && g_plainElsewhere.drop) g_plainElsewhere.drop();
        }
    } elsewhere;
    if (g_plainElsewhere.start && g_plainElsewhere.take && g_plainElsewhere.drop) {
        std::vector<std::string> a = base;
        a.push_back("--gridmap");
        a.push_back(noGrid);
        elsewhere.on = g_plainElsewhere.start(a);
    }

    // --ladderback's envelope step: the attempts run with --capenvelope while this is set.
    bool envOn = false;
    // ...and its coin-wall trigger (--ladderenv): the coins it has fired for, whether the attempt
    // in flight is its trial, and the death tick that trial has to beat.
    uint32_t envCoinTried = 0;
    bool envCoinPending = false;
    long long envCoinT = -1;
    auto run = [&](const std::string* map, const std::string* grid) {
        std::vector<std::string> a = base;
        if (envOn) a.push_back("--capenvelope");
        if (map) {
            a.push_back("--capmap");
            a.push_back(*map);
        }
        if (grid) {
            a.push_back("--gridmap");
            a.push_back(*grid);
        }
        std::vector<char*> p;
        p.reserve(a.size());
        for (std::string& s : a) p.push_back(s.data());
        return cliMainOnce((int)p.size(), p.data());
    };

    // stepped / lastBack / lastWx / backFailed / capFailed: --ladderback's account of the window's
    // last change (a lead step or a cap step), where the frontier died after it, and which kinds
    // of step have left the death where it was.
    struct Window {
        double wall, lead;
        long long cap;
        bool stepped = false, lastBack = false;
        double lastWx = 0.0;
        long long lastT = -1;   // the tick the frontier died on after the window's last change
        bool backFailed = false, capFailed = false;
        int over = 0;   // --ladderovercap steps taken (cap above --cap)
    };
    std::vector<Window> wins;
    // The map is a step function (thread_pool.hpp capAtX), so overlapping windows are resolved
    // here: each stretch between two window edges takes the largest cap that covers it.
    auto mapOf = [&]() {
        std::vector<double> edges;
        for (const Window& w : wins) {
            edges.push_back(w.wall - w.lead);
            edges.push_back(w.wall + kLadderPast);
        }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        char buf[64];
        std::snprintf(buf, sizeof(buf), "-1e9:%lld", c0);
        std::string m = buf;
        for (double e : edges) {
            long long c = c0;
            for (const Window& w : wins)
                if (e >= w.wall - w.lead && e < w.wall + kLadderPast) c = std::max(c, w.cap);
            std::snprintf(buf, sizeof(buf), ",%.1f:%lld", e, c);
            m += buf;
        }
        return m;
    };

    auto frontierDied = [] {
        const int v = g_outcome.verdict;
        return (v == VerdictPartial || v == VerdictFailed) && g_outcome.deepX >= 0.0
               && g_outcome.capHits >= 0;
    };
    // The ladder's account (SearchOutcome::ladder), written on every way out -- after the answer
    // is in place, so the chosen hash is the plan the caller will read.
    std::string outPath;
    for (size_t i = 0; i + 1 < base.size(); ++i)
        if (base[i] == "--out") outPath = base[i + 1];
    auto planHash = [&]() -> std::string {
        std::string bytes;
        if (outPath.empty() || !readFileBytes(outPath, bytes)) return "none";
        uint64_t h = 1469598103934665603ULL;
        for (char c : bytes) {
            h ^= (uint8_t)c;
            h *= 1099511628211ULL;
        }
        char b[24];
        std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
        return b;
    };
    int nAttempts = 0, nIdle = 0;
    std::string plainHash = "-";
    const char* via = "attempt";
    // What each search cost, for a change to the ladder to be read against: `ms/cap/capHits/deepT/work` per
    // ladder attempt (cap = the largest alive cap its map carried; deepT = the tick the frontier died on,
    // absolute -- the distance past the anchor is deepT - the call's anchor tick, which the mod's
    // `[anchor] ... --start <tick>` line gives; work = the states its layers carried forward, the search's own
    // work and the nearest thing to a peak alive that the outcome keeps), then `P ms` for the plain search (under
    // PlainElsewhere that is the time the ladder WAITED for it, not its own) and `A ms` for step 5's repeat.
    // Print only; the account line is the only place it goes.
    std::string rungs;
    auto rungNote = [&](const char* tag, double ms, long long cap, long long hits, long long deepT,
                        long long work) {
        char r[112];
        if (tag[0]) std::snprintf(r, sizeof r, "%s%s%.0f", rungs.empty() ? "" : ",", tag, ms);
        else std::snprintf(r, sizeof r, "%s%.0f/%lld/%lld/%lld/%lld", rungs.empty() ? "" : ",", ms, cap, hits, deepT, work);
        rungs += r;
    };
    struct Account {
        std::function<void()> f;
        ~Account() { f(); }
    } account{[&] {
        char b[960];
        const std::string chosen = planHash();
        std::snprintf(b, sizeof b, "attempts=%d idle=%d plain=%s chosen=%s via=%s rungs=%s", nAttempts,
                      nIdle, plainHash.c_str(), chosen.c_str(), via, rungs.empty() ? "-" : rungs.c_str());
        g_outcome.ladder = b;
        std::printf("capladder: result %s\n", b);
        std::fflush(stdout);
    }};
    // Step 5: the first attempt whose frontier died deepest, and the map it ran with.
    long long bestT = -1;
    int bestAttempt = 0;
    std::string bestMap;
    bool bestEnv = false;   // ...and whether it ran with --ladderback's envelope step
    // ...and what it left, kept as it finished: its plan, its trace and its outcome, which is what
    // searching it again would write. Step 5 puts them back instead of searching it again -- the
    // second search of the same map is the same search. Not under the checkpoint channel, where
    // searching it again also publishes checkpoints for the game to fly, and not for an attempt
    // that wrote no plan (then the files would still hold whatever the plain search left).
    const bool keep = !outPath.empty() && !g_check.enabled.load(std::memory_order_acquire);
    const std::string keptPlan = outPath + ".best";
    const std::string keptTrace = outPath + ".best.trace.csv";
    bool kept = false;
    int keptRc = 0;
    SearchOutcome keptOutcome;
    auto copyFile = [](const std::string& from, const std::string& to) {
        std::string bytes;
        if (!readFileBytes(from, bytes)) return false;
        std::ofstream o(to, std::ios::binary | std::ios::trunc);
        o.write(bytes.data(), (std::streamsize)bytes.size());
        return (bool)o;
    };
    struct DropKept {
        std::string a, b;
        ~DropKept() {
            std::remove(a.c_str());
            std::remove(b.c_str());
        }
    } dropKept{keptPlan, keptTrace};
    // The gamble (PlainElsewhere::ready): the plain search beside the ladder, taken now.
    auto gambleTakes = [&](int attempt) {
        if (!elsewhere.on || !g_plainElsewhere.ready || !g_plainElsewhere.ready(bestT))
            return false;
        std::printf("capladder: attempt=%d took the plain search beside it (the gamble; the "
                    "ladder had died at t=%lld at best)\n", attempt, bestT);
        std::fflush(stdout);
        elsewhere.taken = true;
        return true;
    };
    // --ladderprefix: the map the last attempt ran with and whether it ran with the envelope.
    std::string lpPrevMap;
    bool lpPrevEnv = false;
    auto capOfMap = [](const std::string& map, double x) {
        long long c = (long long)g_aliveCap;
        size_t q = 0;
        while (q < map.size()) {
            size_t e = map.find(',', q);
            if (e == std::string::npos) e = map.size();
            const std::string one = map.substr(q, e - q);
            const size_t k = one.find(':');
            if (k != std::string::npos && x >= std::atof(one.substr(0, k).c_str()))
                c = std::atoll(one.substr(k + 1).c_str());
            q = e + 1;
        }
        return c;
    };
    // The attempt itself: from the last attempt's saves where its layers are this one's (t* below),
    // and under --ladderprefixcheck from t0 as well, comparing the two.
    auto runAttempt = [&](const std::string& m) {
        g_lpc.resume = -1;
        if (lpLadder && !lpPrevMap.empty() && lpPrevEnv == envOn && g_lpc.usable) {
            long long tStar = g_lpc.t0 + 1 + (long long)g_lpc.layerX.size();
            for (size_t i = 0; i < g_lpc.layerX.size(); ++i)
                if (capOfMap(lpPrevMap, g_lpc.layerX[i]) != capOfMap(m, g_lpc.layerX[i])) {
                    tStar = g_lpc.t0 + 1 + (long long)i;
                    break;
                }
            for (int i = (int)g_lpc.snaps.size() - 1; i >= 0; --i)
                if (g_lpc.snaps[(size_t)i].T <= tStar - 1) { g_lpc.resume = i; break; }
        }
        g_lpc.active = lpLadder;
        lpPrevMap = m;
        lpPrevEnv = envOn;
        if (!(lpCheck && g_lpc.resume >= 0)) {
            const int rc = run(&m, nullptr);
            g_lpc.active = false;
            return rc;
        }
        // --ladderprefixcheck: the resumed attempt, its plan, trace and outcome kept aside...
        const int rcA = run(&m, nullptr);
        const long long fromT = g_lpc.resumedFrom;
        std::string planA, traceA;
        readFileBytes(outPath, planA);
        readFileBytes(outPath + ".trace.csv", traceA);
        const SearchOutcome oA = g_outcome;
        // ...then the same attempt from t0 (its saves replace the resumed one's), which is kept
        g_lpc.resume = -1;
        const int rcB = run(&m, nullptr);
        g_lpc.active = false;
        std::string planB, traceB;
        readFileBytes(outPath, planB);
        readFileBytes(outPath + ".trace.csv", traceB);
        const SearchOutcome& oB = g_outcome;
        std::string diff;
        if (rcA != rcB) diff += " rc";
        if (planA != planB) diff += " plan";
        if (traceA != traceB) diff += " trace";
        if (oA.verdict != oB.verdict) diff += " verdict";
        if (oA.deepT != oB.deepT) diff += " deepT";
        if (oA.deepX != oB.deepX) diff += " deepX";
        if (oA.capHits != oB.capHits) diff += " capHits";
        if (oA.workStates != oB.workStates) diff += " workStates";
        if (oA.planWritten != oB.planWritten) diff += " planWritten";
        std::printf("ladderprefixcheck: %s (resumed after t=%lld)%s\n", diff.empty() ? "same" : "MISMATCH",
                    fromT, diff.c_str());
        std::fflush(stdout);
        return rcB;
    };
    for (int attempt = 1;; ++attempt) {
        const std::string m = mapOf();
        long long maxCap = c0;
        for (const Window& w : wins) maxCap = std::max(maxCap, w.cap);
        const auto rungT0 = std::chrono::steady_clock::now();
        // (--resimladder holds: this attempt's verdict is the ladder's to read)
        const int rc = [&] {
            struct Hold {
                Hold() { g_resimLadderHold = true; }
                ~Hold() { g_resimLadderHold = false; }
            } hold;
            return runAttempt(m);
        }();
        const double rungMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rungT0).count();
        const int v = g_outcome.verdict;
        const double wx = g_outcome.deepX;
        rungNote("", rungMs, maxCap, g_outcome.capHits, g_outcome.deepT, g_outcome.workStates);
        std::printf("capladder: attempt=%d map=%s verdict=%d deepX=%.1f rc=%d ms=%.0f cap=%lld capHits=%lld\n",
                    attempt, m.c_str(), v, wx, rc, rungMs, maxCap, (long long)g_outcome.capHits);
        std::fflush(stdout);
        ++nAttempts;
        // The gamble's taking of the plain search (below): its plan is the answer.
        auto takeGamble = [&] {
            const int grc = g_plainElsewhere.take();
            plainHash = planHash();
            via = "gamble";
            return grc;
        };
        // Stopped in progress: the other side raised g_ladderStop because the gamble held when
        // its search finished. This attempt's answer is gone either way, so it is taken.
        if (g_ladderStop.load()) {
            std::printf("capladder: attempt=%d stopped; took the plain search beside it (the "
                        "gamble; the ladder had died at t=%lld at best)\n", attempt, bestT);
            std::fflush(stdout);
            elsewhere.taken = true;
            return takeGamble();
        }
        if (g_outcome.capHits == 0) ++nIdle;
        if (fullCap < 0) fullCap = (long long)g_aliveCap;
        const bool died = frontierDied();
        if (!died || c0 >= fullCap) return rc;
        if (g_outcome.deepT > bestT) {
            bestT = g_outcome.deepT;
            bestAttempt = attempt;
            bestMap = m;
            bestEnv = envOn;
            kept = keep && g_outcome.planWritten && copyFile(outPath, keptPlan)
                   && copyFile(outPath + ".trace.csv", keptTrace);
            if (kept) {
                keptRc = rc;
                keptOutcome = g_outcome;
            }
            g_ladderBestT.store(bestT);
        }
        if (gambleTakes(attempt)) return takeGamble();
        Window* hit = nullptr;
        for (Window& w : wins)
            if (wx >= w.wall - w.lead && wx < w.wall + kLadderPast) hit = &w;
        // Step 4: the plain search -- at once if this attempt's cap never bound, since every later
        // attempt would only raise caps it never reached
        bool plain = attempt >= kLadderMaxAttempts || g_outcome.capHits == 0;
        // --ladderenv at a coin's miss prune: the frontier died where the states without that coin
        // are cut, so the route that takes it is a minority the stride lost on the way -- the
        // envelope's case. The next attempt runs with it at once, the windows left as they are,
        // once per coin; a death no later than this one drops it again and the windows step as
        // usual. Official lv1 with coins: its last coin's prune killed every attempt at x 26,359
        // that lead and cap steps made, and the envelope at cap 125 takes it on the first try.
        if (!plain) {
            if (envCoinPending) {
                envCoinPending = false;
                if (g_outcome.deepT <= envCoinT) envOn = false;
            } else if (!envOn && g_outcome.coinWall >= 0 && g_outcome.coinWall < 32
                       && !((envCoinTried >> g_outcome.coinWall) & 1u)) {
                envCoinTried |= 1u << g_outcome.coinWall;
                envOn = true;
                envCoinPending = true;
                envCoinT = g_outcome.deepT;
                continue;
            }
        }
        if (hit && g_ladderOverCap && hit->over > 0) {
            // --ladderovercap: past --cap the window only widens (x4) or gives way to the plain
            // search; lead steps at a cap above --cap would be the dearest attempts of all.
            // (A death that moved inside the window under the same map would be the same search
            // again, so the only steps are x4 once and then the plain search.)
            if (g_ladderOverCapStop && hit->over == 1 && g_outcome.deepT <= hit->lastT) {
                // --ladderovercapstop: the 2x step did not move the death, so the 4x one is not tried.
                std::printf("capladder: attempt=%d the 2x window left the death at t=%lld (not past t=%lld);"
                            " no 4x step (--ladderovercapstop)\n", attempt, (long long)g_outcome.deepT, hit->lastT);
                plain = true;
            } else if (hit->over < 2) {
                hit->cap = fullCap * 4;
                ++hit->over;
                std::printf("capladder: attempt=%d window at x %.1f still inside; its cap to %lld"
                            " (--ladderovercap)\n", attempt + 1, hit->wall, hit->cap);
            } else {
                plain = true;
            }
            hit->lastT = std::max(hit->lastT, g_outcome.deepT);
        } else if (hit) {
            // --ladderback: one kind of step at a time. The first repeat reaches further back
            // (lead x2, cap kept); after that, a step that moved the death is repeated in kind and
            // one that did not is swapped for the other kind (cap x4, lead kept). A kind that is
            // spent (the window runs from x 0, or has the full cap) gives way to the other.
            const bool canBack = hit->wall - hit->lead > 0.0;
            const bool canCap = hit->cap < fullCap;
            // A wall that neither kind of step has moved, or with both kinds spent, goes to the
            // plain search, as the default ladder's wall does: more steps of the same kinds would
            // be the same death again.
            bool back = true;
            if (hit->stepped) {
                // "Moved" is a later death, not a different x: on RC9 lv20 a lead step left the
                // death 2 px BEHIND where it was (x 6,577.4 -> 6,575.2), and read as a move it
                // sent the ladder on through four more lead steps before the plain search.
                const bool moved = g_outcome.deepT > hit->lastT;
                if (moved) {
                    hit->backFailed = hit->capFailed = false;
                } else if (hit->lastBack) {
                    hit->backFailed = true;
                } else {
                    hit->capFailed = true;
                }
                back = moved ? hit->lastBack : !hit->lastBack;
            }
            if (back && !canBack) back = false;
            if (!back && !canCap) back = canBack;
            if ((!canBack && !canCap) || (hit->backFailed && hit->capFailed)) {
                // TRIED AND REMOVED (2026-10-01, --ladderenv): one attempt with --capenvelope
                // here, at every wall neither kind moved. At a wall no search crosses it was one
                // more search for nothing: RC9 lv20 without coins 26 -> 41 s offline. The envelope
                // now runs only at a coin's miss prune (above), which is where it was ever needed.
                // TRIED AND REMOVED (2026-10-01): one window from x 0 at the full cap, input grid
                // kept, before the plain search. It crossed lv16's wall at x 11,371, which moved
                // for nothing until a window from x 1,771 at 2000 -- but at a wall no search
                // crosses it was a second plain-sized search on top of the plain one, and those
                // walls are the common case: SubZero 4002 with coins went 36 -> 49 rounds and
                // 251 -> 853 s of dp, seven calls over 20 s against one, every one of them 5-8
                // attempts. Without it the ladder never costs more at a wall than the default
                // one does, bar one attempt at the window's cap.
                if (g_ladderOverCap && hit->over < 2) {
                    // --ladderovercap: the window alone above --cap (x2, then x4)
                    hit->cap = fullCap * (hit->over == 0 ? 2 : 4);
                    ++hit->over;
                    hit->backFailed = hit->capFailed = false;
                    std::printf("capladder: attempt=%d window at x %.1f stuck at --cap; its cap to %lld"
                                " (--ladderovercap)\n", attempt + 1, hit->wall, hit->cap);
                } else {
                    plain = true;
                }
            } else if (back) {
                hit->lead *= 2;
            } else {
                hit->cap = std::min(hit->cap * 4, fullCap);
            }
            hit->stepped = true;
            hit->lastBack = back;
            hit->lastWx = wx;
            hit->lastT = g_outcome.deepT;
        } else if (hit && hit->cap >= fullCap) {
            plain = true;
        } else if (hit) {
            hit->cap = std::min(hit->cap * 4, fullCap);
            hit->lead *= 2;
        } else {
            wins.push_back({wx, kLadderLead, std::min(c0 * 4, fullCap), false, false, 0.0, -1, false,
                            false});
        }
        if (plain) {
            envOn = false;   // the plain search is the caller's own arguments
            std::printf("capladder: attempt=%d plain search at --cap %lld%s\n", attempt + 1,
                        fullCap, elsewhere.on ? " (started with the ladder)" : "");
            std::fflush(stdout);
            int prc;
            const auto plainT0 = std::chrono::steady_clock::now();
            if (elsewhere.on && !g_plainElsewhere.check) {
                elsewhere.taken = true;
                prc = g_plainElsewhere.take();
            } else {
                prc = run(nullptr, &noGrid);
                if (elsewhere.on) {
                    elsewhere.taken = true;
                    g_plainElsewhere.check(prc);
                }
            }
            rungNote("P", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - plainT0).count(), 0, 0, 0, 0);
            plainHash = planHash();
            via = "plain";
            // The other side may have raised it as it finished; step 5's search must not see it.
            g_ladderStop.store(false);
            const long long plainT = g_outcome.deepT;
            if (!frontierDied() || plainT >= bestT) return prc;
            via = "again";
            // (a kept PARTIAL was held from --resimladder; searched again, it is not)
            if (kept && !(g_resimLadderPartial && keptOutcome.verdict == VerdictPartial)) {
                std::remove(outPath.c_str());
                std::remove((outPath + ".trace.csv").c_str());
                if (copyFile(keptPlan, outPath) && copyFile(keptTrace, outPath + ".trace.csv")) {
                    g_outcome = keptOutcome;
                    std::printf("capladder: the plain search died at t=%lld, before attempt %d's"
                                " t=%lld - attempt %d's own result is the ladder's (kept)\n",
                                plainT, bestAttempt, bestT, bestAttempt);
                    std::fflush(stdout);
                    return keptRc;
                }
            }
            std::printf("capladder: the plain search died at t=%lld, before attempt %d's t=%lld"
                        " - searching attempt %d again for the result\n",
                        plainT, bestAttempt, bestT, bestAttempt);
            std::fflush(stdout);
            envOn = bestEnv;
            const auto againT0 = std::chrono::steady_clock::now();
            const int brc = run(&bestMap, nullptr);
            rungNote("A", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - againT0).count(), 0, 0, 0, 0);
            std::printf("capladder: attempt=%d again map=%s verdict=%d deepT=%lld (was %lld)"
                        " deepX=%.1f rc=%d\n", bestAttempt, bestMap.c_str(), g_outcome.verdict,
                        g_outcome.deepT, bestT, g_outcome.deepX, brc);
            std::fflush(stdout);
            return brc;
        }
    }
}

// ---- a two-player level: the second body's pass -----------------------------------------------------------------------------
// A search call of a two-player level that has both bodies (g_twoBodyCall) has searched the FIRST body against a ghost that holds nothing. The second body
// is searched here against that plan: the first body's tick-by-tick buttons (the `act` column of the plan's trace) are the ghost's levels, the second
// player is swapped into the first slot (--swapbodies, --startp2dx and --startact2 as the caller gave them), and its plan is written as bit 1. The two are
// merged into the call's `--out`, one `input=<press>,<mask>` line per tick that changes either body's button, and the outcome the caller reads is the
// worse of the two searches'. Two bodies that share nothing but the clock (a level with no triggers; the speed portals count) are solved this way
// where a joint search would have the product of their reachable sets for a frontier.
inline int cliMain(int argc, char** argv) {
    // --p2switch + --switchtab <file> (switch_model.hpp): the world the search sees has the walls a body's press moves where the schedule of presses puts
    // them -- the table's geometry written as the last --groups layer -- and the schedule is found here, from where the first body's search stops.
    std::string outPath, switchTabPath;
    bool p2switch = false;
    for (int i = 0; i < argc; ++i) {
        if (i + 1 < argc && !std::strcmp(argv[i], "--out")) outPath = argv[i + 1];
        if (i + 1 < argc && !std::strcmp(argv[i], "--switchtab")) switchTabPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--p2switch")) p2switch = true;
    }
    SwitchTable swTab;
    const bool swOn = p2switch && !switchTabPath.empty() && !outPath.empty() && loadSwitchTable(switchTabPath, swTab);
    if (p2switch && !swOn) std::printf("twobody: --p2switch without a readable switch table (%s)\n", switchTabPath.c_str());
    std::vector<long long> swS;   // the presses of the switch body (edge ticks), ascending
    std::string replayPlan;
    long long swT0 = 0;           // the call's start tick (--start's first field): presses before it are the plan's own, and stay
    std::vector<long long> swPre;   // --switchpre: every press of the plan the caller holds
    for (int i = 0; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--replay")) replayPlan = argv[i + 1];
        if (!std::strcmp(argv[i], "--start")) swT0 = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--switchpre"))
            for (const char* q = argv[i + 1]; *q; ) {
                swPre.push_back(std::atoll(q));
                const char* c = std::strchr(q, ',');
                if (!c) break;
                q = c + 1;
            }
    }
    if (swOn) {
        if (!replayPlan.empty()) {
            // a walk of a plan: the geometry its own presses give (the plan's bit 1, each rising edge)
            std::ifstream rf(replayPlan);
            std::string ln;
            int prev = 0;
            while (rf && std::getline(rf, ln)) {
                if (ln.size() >= 3 && (unsigned char)ln[0] == 0xEF && (unsigned char)ln[1] == 0xBB && (unsigned char)ln[2] == 0xBF) ln.erase(0, 3);
                long long pt;
                int pv;
                if (std::sscanf(ln.c_str(), "input=%lld,%d", &pt, &pv) != 2) continue;
                const int bit = (pv >> (swTab.body - 1)) & 1;
                if (bit && !prev) swS.push_back(pt);
                prev = bit;
            }
        } else {
            for (const long long tk : swPre)
                if (tk < swT0) swS.push_back(tk);
            std::sort(swS.begin(), swS.end());
        }
    }
    const std::string swGroups = outPath + ".switch.groups";
    auto runPass1 = [&]() -> int {
        if (!swOn) return cliMainBody(argc, argv);
        writeSwitchGroups(swTab, swS, swGroups);
        std::vector<std::string> a;
        for (int i = 0; i < argc; ++i) a.push_back(argv[i]);
        a.push_back("--groups");
        a.push_back(swGroups);
        std::vector<char*> p;
        for (std::string& s : a) p.push_back(s.data());
        return cliMainBody((int)p.size(), p.data());
    };
    const int rc = runPass1();
    if (!g_twoBodyCall || rc != 0) return rc;
    if (outPath.empty()) return rc;
    SearchOutcome out1 = g_outcome;
    // the first body's plan, as edges, and the ghost's levels from its trace
    auto readEdges = [](const std::string& path, std::vector<std::pair<long long, int>>& e) {
        std::ifstream f(path);
        if (!f) return false;
        std::string ln;
        while (std::getline(f, ln)) {
            if (ln.size() >= 3 && (unsigned char)ln[0] == 0xEF && (unsigned char)ln[1] == 0xBB && (unsigned char)ln[2] == 0xBF) ln.erase(0, 3);
            long long p;
            int v;
            if (std::sscanf(ln.c_str(), "input=%lld,%d", &p, &v) == 2) e.emplace_back(p, v);
        }
        return true;
    };
    std::vector<std::pair<long long, int>> e1;
    const std::string ghostPath = outPath + ".ghost1";
    struct TraceRow { long long t; double x, y; };
    std::vector<TraceRow> traceXY;   // the first body's x, y by tick, for --p2tappress and the switch schedule
    std::vector<std::pair<long long, double>> traceX;
    // A first body that never presses writes no plan file (and its trace is then the second body's ghost, holding nothing); the second body is
    // still searched -- it may have presses to make.
    auto loadPass1 = [&]() {
        e1.clear();
        traceXY.clear();
        traceX.clear();
        readEdges(outPath, e1);
        std::ifstream tf(outPath + ".trace.csv");
        std::ofstream gf(ghostPath, std::ios::trunc);
        std::string hd;
        int actCol = -1, xCol = -1, yCol = -1;
        if (tf && std::getline(tf, hd)) {
            int c = 0;
            std::stringstream hs(hd);
            std::string name;
            while (std::getline(hs, name, ',')) {
                if (name == "act") actCol = c;
                if (name == "x") xCol = c;
                if (name == "y") yCol = c;
                ++c;
            }
        }
        std::string ln;
        while (tf && actCol >= 0 && std::getline(tf, ln)) {
            std::vector<std::string> f;
            std::stringstream ls(ln);
            std::string cell;
            while (std::getline(ls, cell, ',')) f.push_back(cell);
            if ((int)f.size() > actCol) gf << f[0] << ',' << f[(size_t)actCol] << "\n";
            if (xCol >= 0 && (int)f.size() > xCol) {
                const long long tk = std::atoll(f[0].c_str());
                const double xv = std::atof(f[(size_t)xCol].c_str());
                traceX.emplace_back(tk, xv);
                traceXY.push_back({tk, xv, (yCol >= 0 && (int)f.size() > yCol) ? std::atof(f[(size_t)yCol].c_str()) : 0.0});
            }
        }
    };
    loadPass1();
    long long inputGridSw = 1;
    for (int i = 0; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--inputgrid")) inputGridSw = std::max(1LL, (long long)std::atoll(argv[i + 1]));
    std::string swMustPress, swMustRelease;
    if (swOn) {
        // --p2switch: the schedule of the switch body's presses. Where the first body's search stops, find the wall of the table that stands in its way
        // in the geometry the schedule so far gives; if a press makes that wall give way by the time the body arrives (the press's curve over the ticks
        // it needs), add it -- the last one that still does -- and search again. A press flips every object the table moves, so each one is checked
        // against the geometry that results, by the search itself, not by a count of presses.
        long long armMin = 1;
        {
            std::string trigPath;
            for (int i = 0; i + 1 < argc; ++i)
                if (!std::strcmp(argv[i], "--triggers")) trigPath = argv[i + 1];
            const std::unordered_map<int, TrigRow>* tp = trigPath.empty() ? nullptr : trigRowsCached(trigPath);
            if (tp && !traceX.empty()) {
                double firstTapX = 1e18;
                for (const auto& kv : *tp)
                    if (kv.second.id == 1595 && !kv.second.touch && !kv.second.spawn && kv.second.cx > traceX.front().second)
                        firstTapX = std::min(firstTapX, kv.second.cx);
                if (firstTapX < 1e17)
                    for (const auto& r : traceX)
                        if (r.second > firstTapX) {
                            armMin = r.first + 3;
                            break;
                        }
            }
        }
        for (int iter = 0; iter < 6 && !traceXY.empty(); ++iter) {
            if (out1.verdict == VerdictSolved) break;
            const long long tA = out1.deepT >= 0 ? out1.deepT : traceXY.back().t;
            const TraceRow& pl = traceXY.back();
            // the wall in the way: a table object, in the geometry the schedule gives at the arrival, that the body's box meets within a few ticks' run
            const SwObj* blocker = nullptr;
            double bestGap = 1e18;
            for (const SwObj& o : swTab.objs) {
                const SwRow s = switchState(o, swS, tA);
                if (!s.on) continue;
                const double gap = (s.cx - s.w / 2) - (pl.x + 15.0);
                if (std::fabs(s.cy - pl.y) < s.h / 2 + 15.0 + 2.0 && gap > -s.w - 30.0 && gap < 12.0 && std::fabs(gap) < bestGap) {
                    bestGap = std::fabs(gap);
                    blocker = &o;
                }
            }
            if (!blocker) {
                std::printf("twobody: switch: no wall of the table stands at the first body's stop (t=%lld x=%.1f)\n", tA, pl.x);
                break;
            }
            const std::vector<SwRow>& curve = blocker->c[swS.size() & 1];
            if (curve.empty()) break;
            const long long open = curve.back().dt + 4;   // a press made this long before the arrival has finished its curve
            long long tp0 = tA - open;
            tp0 -= tp0 % inputGridSw;
            const long long lo = std::max(std::max(armMin, swT0 + 1), swS.empty() ? 1LL : swS.back() + 2 * inputGridSw);
            if (tp0 < lo) tp0 = lo + ((inputGridSw - lo % inputGridSw) % inputGridSw);
            std::vector<long long> next = swS;
            next.push_back(tp0);
            const SwRow before = switchState(*blocker, swS, tA), after = switchState(*blocker, next, tA);
            if (std::fabs(before.cy - after.cy) < 0.5 && std::fabs(before.cx - after.cx) < 0.5 && before.on == after.on) {
                std::printf("twobody: switch: a press at t=%lld would not move uid %d by the first body's arrival (t=%lld)\n", tp0, blocker->uid, tA);
                break;
            }
            const SearchOutcome keepOut = out1;
            const std::vector<long long> keepS = swS;
            swS = next;
            std::printf("twobody: switch: wall uid %d stops the first body at t=%lld x=%.1f - press %zu of the second body's button at t=%lld\n", blocker->uid,
                        tA, pl.x, swS.size(), tp0);
            std::fflush(stdout);
            const int rcN = runPass1();
            if (rcN != 0 || !g_twoBodyCall) { swS = keepS; break; }
            out1 = g_outcome;
            loadPass1();
            const bool better = out1.verdict == VerdictSolved || out1.deepT > keepOut.deepT;
            std::printf("twobody: switch: with it the first body reaches t=%lld (was %lld)\n", out1.deepT, keepOut.deepT);
            if (!better) {
                swS = keepS;
                out1 = keepOut;
                runPass1();   // the plan files of the schedule that stays
                out1 = g_outcome;
                loadPass1();
                break;
            }
        }
        // Edits of the schedule. Adding a press where a wall of the table stands is one move; two things it cannot do are the ones a person finds by
        // trying: (1) the PARITY at the body's arrival -- an earlier press of this call that leaves a wall the wrong way round is taken out, and
        // (2) the TIMING of a press while the first body is carried by an object the press moves (a platform it rides is lowered or raised under it)
        // -- a press is moved earlier or later. Each edit is judged as the add above is: by how far the first body then gets. Only presses at or
        // after this call's start can be edited (the ones before it are the plan's history), and the budget is small and fixed.
        if (out1.verdict != VerdictSolved && !traceXY.empty()) {
            std::vector<size_t> editable;
            for (size_t j = 0; j < swS.size(); ++j)
                if (swS[j] >= swT0) editable.push_back(j);
            int budget = 10;
            bool improved = true, filesAreSchedule = true;   // the plan files are those of swS until a rejected edit is searched
            while (improved && budget > 0 && !editable.empty() && out1.verdict != VerdictSolved) {
                improved = false;
                // latest press first: the ones nearest the stop decide the arrival
                for (size_t ei = editable.size(); ei-- > 0 && budget > 0 && !improved;) {
                    const size_t j = editable[ei];
                    if (j >= swS.size()) continue;
                    std::vector<std::vector<long long>> cands;
                    {
                        std::vector<long long> c = swS;
                        c.erase(c.begin() + (long)j);
                        cands.push_back(c);   // the parity: this press taken out
                    }
                    for (const long long d : {-64LL, -24LL, 24LL, 64LL}) {
                        long long tk = swS[j] + d * inputGridSw;
                        tk -= tk % inputGridSw;
                        const long long lo = std::max(std::max(armMin, swT0 + 1), j == 0 ? 1LL : swS[j - 1] + 2 * inputGridSw);
                        const long long hi = j + 1 < swS.size() ? swS[j + 1] - 2 * inputGridSw : (traceXY.back().t > swS[j] ? traceXY.back().t : swS[j] + 64);
                        if (tk < lo || tk > hi || tk == swS[j]) continue;
                        std::vector<long long> c = swS;
                        c[j] = tk;
                        cands.push_back(c);   // the timing: this press earlier or later
                    }
                    for (const std::vector<long long>& c : cands) {
                        if (budget-- <= 0) break;
                        const SearchOutcome keepOut = out1;
                        const std::vector<long long> keepS = swS;
                        swS = c;
                        const int rcE = runPass1();
                        if (rcE != 0 || !g_twoBodyCall) { swS = keepS; out1 = keepOut; filesAreSchedule = false; budget = 0; break; }
                        out1 = g_outcome;
                        loadPass1();
                        const bool better = out1.verdict == VerdictSolved || out1.deepT > keepOut.deepT;
                        std::printf("twobody: switch: edit of press %zu (%zu press(es) now, first at t=%lld): the first body reaches t=%lld (was %lld)\n", j + 1, swS.size(),
                                    swS.empty() ? 0LL : swS.front(), out1.deepT, keepOut.deepT);
                        std::fflush(stdout);
                        filesAreSchedule = better;
                        if (better) {
                            improved = true;
                            editable.clear();
                            for (size_t q = 0; q < swS.size(); ++q)
                                if (swS[q] >= swT0) editable.push_back(q);
                            break;
                        }
                        swS = keepS;
                        out1 = keepOut;
                    }
                }
            }
            // the plan files of the schedule that stays (the last search may have been a rejected edit)
            if (!filesAreSchedule) {
                runPass1();
                out1 = g_outcome;
                loadPass1();
            }
        }
        // the second body's button: pressed at the schedule's ticks, and nowhere else (a press of its own changes the walls, which the geometry
        // written for the schedule does not know)
        long long endT = std::max(swTab.tend, traceXY.empty() ? 0LL : traceXY.back().t) + 1;
        long long from = std::max(1LL, swT0);
        for (const long long tk : swS) {
            if (tk < swT0) continue;   // a press the plan made before this call's start
            if (!swMustPress.empty()) swMustPress += ",";
            swMustPress += std::to_string(tk) + ":" + std::to_string(tk + inputGridSw - 1);
            if (tk - 1 >= from) {
                if (!swMustRelease.empty()) swMustRelease += ",";
                swMustRelease += std::to_string(from) + ":" + std::to_string(tk - 1);
            }
            from = tk + inputGridSw + 1;
        }
        if (endT >= from) {
            if (!swMustRelease.empty()) swMustRelease += ",";
            swMustRelease += std::to_string(from) + ":" + std::to_string(endT);
        }
        std::printf("twobody: switch: %zu press(es) of the second body's button\n", swS.size());
    }    // --p2tappress (frames.hpp): each Tap (1595; neither spawned nor touched) whose x the first body's plan passes gets a press of the second
    // player's, three ticks after the pass and on the input grid, forced into the second body's pass (--mustpress). A Tap already behind the
    // call's start (the anchor's x) was pressed by the plan before it, and is left alone.
    std::string mustPress, mustRelease;
    if (g_p2TapPress && !swOn && !traceX.empty()) {
        std::string trigPath;
        long long grid = 1;
        double holdX = 0.0;
        for (int i = 0; i + 1 < argc; ++i) {
            if (!std::strcmp(argv[i], "--triggers")) trigPath = argv[i + 1];
            if (!std::strcmp(argv[i], "--inputgrid")) grid = std::max(1LL, (long long)std::atoll(argv[i + 1]));
            if (!std::strcmp(argv[i], "--p2holdx")) holdX = std::atof(argv[i + 1]);
        }
        const std::unordered_map<int, TrigRow>* tp = trigPath.empty() ? nullptr : trigRowsCached(trigPath);
        if (tp) {
            std::vector<std::pair<double, int>> taps;   // (x, uid)
            for (const auto& kv : *tp)
                if (kv.second.id == 1595 && !kv.second.touch && !kv.second.spawn) taps.emplace_back(kv.second.cx, kv.first);
            std::sort(taps.begin(), taps.end());
            const double x0 = traceX.front().second;
            for (const auto& tap : taps) {
                if (tap.first <= x0) continue;
                long long tArm = -1;
                for (const auto& r : traceX)
                    if (r.second > tap.first) { tArm = r.first; break; }
                if (tArm < 0) continue;
                long long T = tArm + 3;
                if (T % grid) T += grid - (T % grid);
                if (!mustPress.empty()) mustPress += ",";
                mustPress += std::to_string(T) + ":" + std::to_string(T + grid - 1);
                std::printf("twobody: Tap uid %d (x %.1f) is armed by the first body at t=%lld - the second body presses at t=%lld\n", tap.second, tap.first, tArm, T);
                // --p2holdx X: a Tap in toggle mode takes every press of the second player, so the door it opened shuts again on the next one
                // (measured: a second press at t=300 starts the door down at 309). The second body presses no more until the first body's
                // plan is past X -- the x of the farthest wall the first body has died at, plus a margin -- or the plan ends.
                if (holdX > 0.0) {
                    long long tHold = traceX.back().first;
                    for (const auto& r : traceX)
                        if (r.second >= holdX) { tHold = r.first; break; }
                    const long long h0 = T + grid + 1;
                    if (tHold >= h0) {
                        if (!mustRelease.empty()) mustRelease += ",";
                        mustRelease += std::to_string(h0) + ":" + std::to_string(tHold);
                        std::printf("twobody: ...and no more until the first body is past x %.1f (t=%lld)\n", holdX, tHold);
                    }
                }
            }
        }
    }
    // the second pass's arguments: the same call, its own output, the ghost, the swap
    std::vector<std::string> a2;
    for (int i = 0; i < argc; ++i) {
        const bool pair = i + 1 < argc;
        if (pair && (!std::strcmp(argv[i], "--out") || !std::strcmp(argv[i], "--bands") || !std::strcmp(argv[i], "--ghostlevels")
                     || !std::strcmp(argv[i], "--bodybit"))) {
            if (!std::strcmp(argv[i], "--out")) { a2.push_back("--out"); a2.push_back(outPath + ".b2"); }
            ++i;
            continue;
        }
        if (!std::strcmp(argv[i], "--swapbodies")) continue;
        a2.push_back(argv[i]);
    }
    a2.push_back("--swapbodies");
    a2.push_back("--bodybit");
    a2.push_back("2");
    a2.push_back("--ghostlevels");
    a2.push_back(ghostPath);
    if (swOn) {   // the same geometry as the first body saw, and the schedule of presses as the only presses of this body
        a2.push_back("--groups");
        a2.push_back(swGroups);
        if (!swMustPress.empty()) { a2.push_back("--mustpress"); a2.push_back(swMustPress); }
        if (!swMustRelease.empty()) { a2.push_back("--mustrelease"); a2.push_back(swMustRelease); }
    }
    if (!mustPress.empty()) {
        a2.push_back("--mustpress");
        a2.push_back(mustPress);
    }
    if (!mustRelease.empty()) {
        a2.push_back("--mustrelease");
        a2.push_back(mustRelease);
    }
    std::vector<char*> p2;
    for (std::string& s : a2) p2.push_back(s.data());
    std::printf("twobody: the second player's pass (the first body's plan has %zu edges)\n", e1.size());
    std::fflush(stdout);
    const int rc2 = cliMainBody((int)p2.size(), p2.data());
    std::vector<std::pair<long long, int>> e2;   // the second body's levels (the plan file holds them as bit 1)
    {
        std::vector<std::pair<long long, int>> raw;
        if (readEdges(outPath + ".b2", raw))
            for (const auto& e : raw) e2.emplace_back(e.first, (e.second >> 1) & 1);
    }
    // merge
    struct Ev { long long t; int bit; int lvl; };
    std::vector<Ev> ev;
    for (const auto& e : e1) ev.push_back({e.first, 1, e.second & 1});
    for (const auto& e : e2) ev.push_back({e.first, 2, e.second});
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    if (ev.empty()) {
        std::remove(outPath.c_str());   // neither body presses: no plan file, as a single body's call writes none
    } else {
        std::ofstream mf(outPath, std::ios::trunc);
        int mask = 0;
        for (size_t i = 0; i < ev.size(); ++i) {
            mask = ev[i].lvl ? (mask | ev[i].bit) : (mask & ~ev[i].bit);
            if (i + 1 < ev.size() && ev[i + 1].t == ev[i].t) continue;
            mf << "input=" << ev[i].t << ',' << mask << "\n";
        }
    }
    std::remove((outPath + ".b2").c_str());
    std::remove((outPath + ".b2.trace.csv").c_str());
    // the worse of the two searches' outcomes
    const SearchOutcome out2 = g_outcome;
    const bool firstWorse = out1.verdict < out2.verdict
                            || (out1.verdict == out2.verdict && out1.deepT >= 0 && (out2.deepT < 0 || out1.deepT < out2.deepT));
    if (rc2 == 0 && firstWorse) g_outcome = out1;
    std::printf("twobody: merged plan %zu lines -> %s (first body verdict=%d deepT=%lld, second verdict=%d deepT=%lld)\n", ev.size(), outPath.c_str(),
                (int)out1.verdict, out1.deepT, (int)out2.verdict, out2.deepT);
    std::fflush(stdout);
    return rc2 != 0 ? rc2 : rc;
}

}  // namespace dp
