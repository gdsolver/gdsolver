#pragma once
// Solving a heavy level on a slice of itself (solver/slice.hpp), and verifying the plan on the
// level itself before anything is filed.
//
// THE FLOW. The session's first solve (dpsolve::start) asks maybeSlice first. When the cut
// removes at least `slicemin` objects, the level is swapped for a copy without them and the solve
// starts there, as on any level. The plan that clears the copy is not filed: the level itself is
// swapped back in and the plan is flown on it (onSliceCleared, then the next start). A clear
// there is the ordinary clear of a solve session -- filed and shown under the level's own id. A
// death there means the cut was wrong somewhere (onVerifyDeath): the objects dropped around the
// tick where the two runs parted are put back and the solve goes on on the new copy; after
// `sliceaddbacks` of those, or when the parting did not move on, it goes on on the level itself.
// A copy that is wrong only at a wall would never clear, so a wall the copy holds is checked on
// the level itself too (maybeCheckWall), and giving up on the copy moves the solve to the level
// itself (onGiveUp) rather than ending it.
//
// WHAT CARRIES ACROSS A SWAP. A swap is one run, not a new one -- choosing a level, solving it and
// playing the solution back are one frame, so a plan and what the run has learnt are not thrown
// away because some objects came back. The plan, the fixups (keyed by the player's state, not by
// any object), the phantom vetoes and the round count carry over. What is keyed by an object's
// uid does not -- the recordings of the moving geometry, the anchor rows, the pads and rings an
// attempt spent -- because the copy numbers its objects differently; those are recorded again on
// the new level (dpsolve::start with a carried plan).
//
// The copy is an editor-type level with the original's id, song and version. The game turns a
// modified main level away as soon as it starts, and an editor-type copy of one plays the same
// (the player's state on every tick of known solutions of the four heaviest official levels).
#include "mod/level_entry.hpp"
#include "solver/slice.hpp"

namespace p1 {
namespace levelslice {

// THE THRESHOLD, IN OBJECTS REMOVED, is where the copy pays for itself.
//
// What an object costs, per round of the loop: measured on a heavy custom level solved twice from
// the same build, once on the level (109,663 objects) and once on its cut (4,827), whose 44 rounds
// made identical decisions ([fp] equal line for line) -- so the two runs did the same work and
// only its price differs. The 39 solver calls took 175 s against 75 s (the level the model reads
// is built from every object), and the 221,738 attempt ticks 17.1 s against 10.2 s. Per object
// removed that is about 24 us a round. (The section search's restores, which also follow the
// object count, come on top and are not counted here.)
//
// What the copy costs, once: a second load of the level and one flight of the plan on it, plus the
// cut itself (tens of ms, 162 ms on the level above). On level 22 (18,329 objects) everything
// after the last solve returned -- the copy's clearing flight, the swap back, the flight on the
// level and the session's end -- took under 10 s.
//
// So a cut of 10,000 objects saves about 0.24 s a round and has paid for itself within some twenty
// rounds. A solve shorter than that is a short solve, and the most it loses is those few seconds.
inline constexpr int kSliceMinDefault = 10000;
// Half the width of the stretch of level put back around where the plan and the level parted.
inline constexpr double kAddBackHalfWidth = 1500.0;
// Ticks after the plan's first press within which a parting from the copy says the two runs did
// not start alike rather than anything about the cut (onVerifyDeath). Measured on a level that
// starts as a ship: a press at t=0 first shows in the attempt's record at t=2.
inline constexpr long long kSyncTicks = 2;

inline GJGameLevel* g_orig = nullptr;   // the level the session was started on (retained)
inline std::string g_raw;                // its level string, decompressed
inline slice::Cut g_cut;                 // what the rule keeps (views into g_raw)
inline std::vector<std::pair<double, double>> g_keepX;   // stretches put back
inline int g_addBacks = 0;
inline double g_lastPartX = -1e18;       // where the last verification parted from the copy
inline std::vector<InputCmd> g_carry;    // the plan the next level flies first
inline bool g_carryPending = false;
// ...and flown WITHOUT touching the loop: a wall check's flight on the level, and the return to
// the same copy after one. The copy is rebuilt from the same string, so its objects get the same
// uids and every recording, anchor and plan the loop holds is still the copy's.
inline bool g_lightPending = false;
inline std::vector<AnchorRow> g_clearRows;   // the copy's attempt the level is compared with
inline long long g_refEnd = -1;          // ...the tick it ended on
inline bool g_refCleared = false;        // ...and whether that end was a clear or a death
inline long long g_checkedWall = -1;     // the copy's wall last checked on the level itself
inline long long g_ownWall = -1;         // ...when that check found it the level's own
inline long long g_giveUpCopyAt = -1;    // the wall a give-up last went on on a fresh copy from
inline int g_wallChecks = 0;
inline bool g_swapInit = false;          // the PlayLayer being built is one of our swaps
inline size_t g_keptNow = 0;             // objects in the level being played
inline int g_verifies = 0;               // plans flown on the level itself
inline int g_heldType = -1;              // a copy's own type while it builds as its level (below)
inline long long g_origCoins = 0;        // coins (142, 1329) in the level itself
inline std::chrono::steady_clock::time_point g_t0;
// The uids (slice.hpp "HOLDING THE UIDS"): each string object's uid in the level itself, and for
// the copy being played, which of the level's string objects each of its objects is. The copy is
// checked against the level once it has loaded (checkCopyUids); g_uidsHeld is that verdict.
inline std::vector<int> g_levelUid;
inline std::vector<std::vector<int>> g_levelExtra;
inline std::vector<int> g_copyOrd, g_copyIds;
inline size_t g_placeholders = 0;
inline bool g_uidCheckPending = false;
inline bool g_uidsHeld = false;
// cfg slicefirstref (onVerifyDeath / onRefFlightEnd): the wall check's plan and the level's flight
// of it, kept while the same plan is flown on a fresh copy as that copy's first attempt.
inline std::vector<InputCmd> g_verifyPlan;
inline std::vector<AnchorRow> g_levelRows;
inline long long g_levelDeath = -1;
inline double g_levelDeathX = 0.0;
inline long long g_firstParting = -1;
inline int g_refFlights = 0;
// cfg sliceverifyrec (config.hpp): the check flights on the level itself fly without the recording of moving
// geometry. g_heldFlight: the flight at hand did not record (set at its reset, hooks_playlayer.cpp).
// g_verifyRecord: the next check flight does -- it is the second flight of a clear check that died unrecorded.
// g_clearPlan: the plan onSliceCleared handed to the level, for that second flight.
inline bool g_heldFlight = false;
inline bool g_verifyRecord = false;
inline std::vector<InputCmd> g_clearPlan;
inline int g_reflies = 0;
// cfg stagecuts (STAGES, below): the cuts, ascending, each short of the level's last object; the piece
// being played (size() = the level itself, -1 = off); the pieces handed on.
inline std::vector<double> g_stageCuts;
inline int g_stage = -1;
inline int g_stageHandOffs = 0;
inline std::string g_stageBase;   // what the pieces are cut from: the level's string, or the slice's copy of it
inline bool g_stageSliced = false;
// The check of a hand-off (stageCompare): the clearing flight of the piece just left, and whether the first
// flight on the next piece is still to be compared with it.
inline std::vector<AnchorRow> g_stageRows;
inline long long g_stageClearT = -1;
inline double g_stageCutX = 0.0;
inline bool g_stageWatch = false;
inline int g_stageSame = 0, g_stageEdge = 0, g_stageApart = 0;   // hand-offs: consistent / parted at the piece's end / parted before it

// Whether a flight that starts now flies without the recording: a check flight on the level itself.
inline bool flightUnrecorded() {
    return !g_cfg.sliceVerifyRec && g_phase == Verifying && !g_verifyRecord;
}

inline int sliceMin() { return g_cfg.sliceMin >= 0 ? g_cfg.sliceMin : kSliceMinDefault; }

inline void holdOrig(GJGameLevel* l) {
    if (l) l->retain();
    if (g_orig) g_orig->release();
    g_orig = l;
}

inline void reset() {
    g_phase = Off;
    holdOrig(nullptr);
    g_cut = slice::Cut{};   // before the string its views point into
    g_raw.clear();
    g_keepX.clear();
    g_addBacks = 0;
    g_lastPartX = -1e18;
    g_carry.clear();
    g_carryPending = false;
    g_lightPending = false;
    g_clearRows.clear();
    g_refEnd = -1;
    g_refCleared = false;
    g_checkedWall = -1;
    g_ownWall = -1;
    g_giveUpCopyAt = -1;
    g_wallChecks = 0;
    g_swapInit = false;
    g_keptNow = 0;
    g_verifies = 0;
    g_levelUid.clear();
    g_levelExtra.clear();
    g_copyOrd.clear();
    g_copyIds.clear();
    g_placeholders = 0;
    g_uidCheckPending = false;
    g_uidsHeld = false;
    g_verifyPlan.clear();
    g_levelRows.clear();
    g_levelDeath = -1;
    g_levelDeathX = 0.0;
    g_heldFlight = false;
    g_verifyRecord = false;
    g_clearPlan.clear();
    g_reflies = 0;
    g_firstParting = -1;
    g_refFlights = 0;
    g_heldType = -1;
    g_origCoins = 0;
    g_stageCuts.clear();
    g_stageRows.clear();
    g_stageClearT = -1;
    g_stageCutX = 0.0;
    g_stageWatch = false;
    g_stageSame = g_stageEdge = g_stageApart = 0;
    g_stage = -1;
    g_stageHandOffs = 0;
    g_stageBase.clear();
    g_stageSliced = false;
}

// The level string the layer was built from. The main levels' strings begin with whitespace
// before the data (hooks_playmenu.cpp readLevel), and a stored level is compressed.
inline std::string levelString(GJGameLevel* level) {
    if (!level) return {};
    std::string s(level->m_levelString);
    const size_t b = s.find_first_not_of(" \t\r\n");
    const size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    return gdcompat::rawLevelString(std::move(s));
}

inline std::vector<slice::LiveObj> liveObjects(GJBaseGameLayer* l) {
    std::vector<slice::LiveObj> out;
    if (!l || !l->m_objects) return out;
    out.reserve(l->m_objects->count());
    for (auto* o : CCArrayExt<GameObject*>(l->m_objects)) {
        if (!o) continue;
        slice::LiveObj r;
        r.id = o->m_objectID;
        r.type = (int)o->m_objectType;
        if (o->m_groupCount > 0 && o->m_groups)
            for (int i = 0; i < std::min((int)o->m_groupCount, 10); ++i)
                if ((*o->m_groups)[i] > 0) r.groups.push_back((int)(*o->m_groups)[i]);
        if (auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(o)) {
            r.effect = true;
            r.target = e->m_targetGroupID;
            r.center = e->m_centerGroupID;
            r.tmodCenter = e->m_targetModCenterID;
            r.moveTarget = e->m_useMoveTarget;
            r.dynamic = e->m_isDynamicMode;
        }
        out.push_back(std::move(r));
    }
    return out;
}

// The built objects as (uid, id), in uid order.
inline std::vector<std::pair<int, int>> byUid(GJBaseGameLayer* l) {
    std::vector<std::pair<int, int>> out;
    if (!l || !l->m_objects) return out;
    out.reserve(l->m_objects->count());
    for (auto* o : CCArrayExt<GameObject*>(l->m_objects))
        if (o) out.emplace_back(o->m_uniqueID, o->m_objectID);
    std::sort(out.begin(), out.end());
    return out;
}

// The level's own manifest, taken before the first copy is built: each string object's uid and
// the uids of what the game built right behind it (slice_uids_level_lv<id>.tsv).
inline void noteLevelUids(GJBaseGameLayer* l) {
    const std::vector<int> ids = slice::stringIds(g_raw);
    slice::OrdinalUids o = slice::ordinalUids(ids, byUid(l));
    g_levelUid = std::move(o.uid);
    g_levelExtra = std::move(o.extra);
    std::ofstream f(std::string(DATA_DIR) + "/slice_uids_level_lv" + std::to_string(g_cfg.levelId)
                    + ".tsv", std::ios::trunc);
    f << "ordinal\tid\tuid\tbehind\n";
    for (size_t k = 0; k < ids.size(); ++k) {
        f << k << '\t' << ids[k] << '\t' << g_levelUid[k] << '\t';
        for (size_t e = 0; e < g_levelExtra[k].size(); ++e) f << (e ? " " : "") << g_levelExtra[k][e];
        f << '\n';
    }
    if (o.matched != ids.size())
        writeResult("slice: uids - only " + std::to_string(o.matched) + " of the level's "
                    + std::to_string(ids.size()) + " string objects were found among its objects "
                      "in uid order");
}

// The copy's string from the cut and what has been put back, noting which of the level's string
// objects each of its objects is, for the check once it has loaded.
inline std::string copyString(size_t* kept) {
    size_t ph = 0;
    slice::g_placeholderGroups = g_cfg.sliceKeepGroups;   // cfg slicekeepgroups
    std::string s = slice::build(g_cut, g_keepX, kept, g_cfg.sliceUids, &ph, &g_copyOrd);
    g_copyIds = slice::stringIds(s);
    g_placeholders = ph;
    g_uidCheckPending = true;
    return s;
}

// Once a copy has loaded: does each of its objects have the uid the same string object has in the
// level itself, and does what the game built behind it too (slice_uids_copy_lv<id>.tsv)? The last
// object's tail is left out of the verdict: it also holds what the game makes after the level
// (the anti-cheat spike, the collision block) and what a run makes (checkpoints).
inline void checkCopyUids(GJBaseGameLayer* l) {
    g_uidCheckPending = false;
    const slice::OrdinalUids o = slice::ordinalUids(g_copyIds, byUid(l));
    size_t same = 0, differ = 0, behindDiffer = 0;
    long long firstBad = -1;
    std::ofstream f(std::string(DATA_DIR) + "/slice_uids_copy_lv" + std::to_string(g_cfg.levelId)
                    + ".tsv", std::ios::trunc);
    f << "index\tordinal\tid\tuid\tlevel_uid\tbehind\tlevel_behind\tok\n";
    for (size_t k = 0; k < g_copyIds.size() && k < g_copyOrd.size(); ++k) {
        const int ord = g_copyOrd[k];
        const bool known = ord >= 0 && (size_t)ord < g_levelUid.size();
        const int want = known ? g_levelUid[(size_t)ord] : -1;
        static const std::vector<int> kNone;
        const std::vector<int>& wantBehind = known ? g_levelExtra[(size_t)ord] : kNone;
        const bool last = k + 1 == g_copyIds.size();
        const bool uidOk = known && o.uid[k] == want;
        const bool behindOk = last || o.extra[k] == wantBehind;
        if (uidOk) ++same; else ++differ;
        if (!behindOk) ++behindDiffer;
        if ((!uidOk || !behindOk) && firstBad < 0) firstBad = (long long)k;
        f << k << '\t' << ord << '\t' << g_copyIds[k] << '\t' << o.uid[k] << '\t' << want << '\t';
        for (size_t e = 0; e < o.extra[k].size(); ++e) f << (e ? " " : "") << o.extra[k][e];
        f << '\t';
        for (size_t e = 0; e < wantBehind.size(); ++e) f << (e ? " " : "") << wantBehind[e];
        f << '\t' << ((uidOk && behindOk) ? 1 : 0) << '\n';
    }
    g_uidsHeld = differ == 0 && behindDiffer == 0 && o.matched == g_copyIds.size()
                 && g_copyIds.size() == g_copyOrd.size();
    char b[360];
    snprintf(b, sizeof(b), "slice: uids - the copy's %zu objects (%zu placeholders): %zu have the "
             "level's uid, %zu do not, %zu differ in what the game built behind them; %s",
             g_copyIds.size(), g_placeholders, same, differ, behindDiffer,
             g_uidsHeld ? "the copy numbers its objects as the level does"
                        : ("NOT the level's numbering (first at copy object "
                           + std::to_string(firstBad) + ") - not for fidelity").c_str());
    writeResult(b);
}

inline GJGameLevel* makeCopy(const std::string& raw) {
    auto* c = GJGameLevel::create();
    c->m_levelName = g_orig->m_levelName;
    c->m_levelID = g_orig->m_levelID.value();
    c->m_levelType = GJLevelType::Editor;
    c->m_audioTrack = g_orig->m_audioTrack;
    c->m_songID = g_orig->m_songID;
    c->m_songIDs = g_orig->m_songIDs;
    c->m_sfxIDs = g_orig->m_sfxIDs;
    c->m_gameVersion = g_orig->m_gameVersion;
    c->m_levelVersion = g_orig->m_levelVersion;
    c->m_twoPlayerMode = g_orig->m_twoPlayerMode;
    // GD's own compression: doing base64+gzip by hand fails to load (level_entry.hpp).
    c->m_levelString = gdcompat::compressLevelString(raw);
    return c;
}

// THE COPY'S SECRET COINS. The game builds a secret coin (id 142, object type 22) only in a main
// level: PlayLayer::processCreateObjectsFromSetup (0x3ad0f0 in 2.2081) makes each object of the
// string and adds it to the layer unless its type is 22 and the level's type is not Main, the type
// being read once at the top of each batch. So an editor-type copy has none of them, and a run that
// takes the level's coins could not take them there (level 22 with coins on cleared its copy 103
// times, each with 0 of 3 coins by the game's own count).
//
// While a copy builds its objects for a run that takes coins, its type therefore reads as the type
// of the level it was cut from, and only then (hooks_playlayer.cpp). It is put back before
// createObjectsFromSetupFinished, which the last batch calls, so the finish, setupHasCompleted and
// the play itself see the editor-type level they saw before; the game's check of a main level's
// string (verifyLevelIntegrity) is in PlayLayer::init, before any batch. Inside the window the type
// is read once more, for the coin itself: whether the player already holds it (hasUniqueCoin, from
// objectFromVector for id 142), which gets the answer the level itself gets. A user coin (id 1329,
// type 31) is built in either type. A run that does not take coins builds the copy as before.
//
// Nothing a coin earns is recorded from the copy. PlayLayer::levelComplete stores the coins taken
// (processItems: storeSecretCoin, storeUserCoin or the pending user coins), the completion and the
// coin achievement only for a level whose type is not Editor, and the copy plays as one; and each
// of those stores is blocked while the bot drives anyway (hooks_system.cpp).
inline bool buildsAsLevel(GJGameLevel* level) {
    return g_cfg.coinRoute && g_phase != Off && g_orig && level && level != g_orig;
}

// What the outgoing layer leaves behind that points into it. The session goes on, so this is
// the level-specific part of resetSessionState and nothing else.
inline void forgetLayer() {
    speedgate::reset();
    orbtrace::reset();
    padtrace::reset();
    touchseed::reset();
    touchfire::reset();
    portalseed::reset();
    padseed::reset();
    ringseed::reset();
    grouptrace::reset();     // g_on stays: the new level records as this one did
    areaenv::clear();        // keyed by GameObject*, which the new layer reallocates
    cpflight::reset();       // checkpoints of a layer that is going (cfg cpflight)
    secsolve::reset();
    hitbox::clearHidden();
    g_ckpt = nullptr; g_ckptTick = -1; g_headHeld = 0;
    solver::g_levelMaxX = 0;
    solver::g_prevTickX = -1e9f;
    solver::g_pois.clear(); solver::g_poisBuilt = false;
    solver::g_coins.clear();
    solver::g_coinPickupTick.clear();
    solver::g_coinGdTick.clear();
    solver::g_coinGdUnmatched = 0;
    solver::g_coinMissFired = false;
    solver::g_coinMissIdx = -1;
    solver::g_endTriggers = 0;
    solver::g_endTriggerFiredTick = -1;
    solver::g_hasRotGameplay = false;
    solver::g_itemCounts.clear();
    solver::g_coinGates.clear();
    solver::g_coinNoMiss.clear();
}

// Replace the level being played, once nothing else is replacing the scene. Never where the
// callers stand -- inside the game's update or its completion path, where replacing the scene
// deletes the layer they are in (level_entry.hpp swapLevelNow) -- and never inside a scene
// transition: a level entered with a fade (enterConfiguredLevel) is built and starts its first
// solve while the fade still runs, and the fade's end puts ITS scene back on top of one replaced
// in the meantime. Measured on the first cut of this (a heavy custom level, entered by the cold
// harness): the copy was built and its solve started, then the original went on running under a
// PlayLayer::get() that named the copy, and every hook took it for a layer on its way out. The
// suite waits for the same thing (SuiteKeeper). The level is held still until the new one takes
// over. The wait is capped in time, not frames: cfg fps sets the frame rate, and at fps=1000 a
// cap of 1,200 frames would give up after 1.2 s, well inside an ordinary fade.
inline constexpr std::chrono::seconds kSwapWait{20};

// Why the scene cannot be replaced yet, or nullptr when it can. A running transition is not the
// only way to be early: the play menu's level page builds the PlayLayer in playStep3 and starts the
// fade to it only in playStep4, frames later (LevelInfoLayer 0x2fd190 / 0x2fd310 in 2.2081, the
// page of an online level). The session's first solve runs in between,
// while the running scene is still the level page and no transition is in sight, and a swap there
// put the copy on screen only for the fade to replace it with the level itself: the original ran
// undriven, PlayLayer::get() named a copy that had no scene, the loop stood still, and Escape
// crashed in pauseGame, which adds the pause menu to that copy's missing parent (reported on the
// play menu, 2026-09-27, on two heavy online levels). So the layer the session is on has to be the
// one on screen, with no scene change queued behind it.
inline const char* sceneBusy() {
    auto* dir = cocos2d::CCDirector::sharedDirector();
    auto* running = dir->getRunningScene();
    if (!running) return "no running scene";
    if (geode::cast::typeinfo_cast<cocos2d::CCTransitionScene*>(running)) return "a transition runs";
    if (dir->getNextScene()) return "a scene change is queued";
    auto* pl = PlayLayer::get();
    if (!pl || pl->getParent() != running) return "the level is not on screen yet";
    return nullptr;
}

inline void swapWhenClear(geode::Ref<GJGameLevel> keep, int frames, std::string waitedFor = {},
                          std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now()) {
    geode::Loader::get()->queueInMainThread([keep, frames, waitedFor, since] {
        if (!g_started || g_sessionOver) return;   // the session ended in the meantime
        const char* busy = sceneBusy();
        if (frames == 0) phase::begin("waiting for the scene");   // ended where the scene is swapped
        std::string seen = waitedFor;
        if (busy && seen.find(busy) == std::string::npos) seen += std::string(seen.empty() ? "" : ", ") + busy;
        if (busy && std::chrono::steady_clock::now() - since < kSwapWait) {
            swapWhenClear(keep, frames + 1, seen, since);
            return;
        }
        writeResult(busy ? "slice: the scene was still changing after " + std::to_string(frames)
                               + " frames (" + busy + ") - swapping anyway"
                         : "slice: swapping the level in (waited " + std::to_string(frames)
                               + " frame(s) for the scene to settle"
                               + (seen.empty() ? "" : ": " + seen) + ")");
        g_forceCleanStart = true;
        g_swapInit = true;
        phase::begin("cutting / swapping the level in (slice)");
        cocos2d::CCDirector::sharedDirector()->replaceScene(PlayLayer::scene(keep.data(), false, false));
        phase::end();
    });
}

// The census (cfg slicecount) ends the session the same way, once the scene has settled: leaving
// a level inside its entry fade leaves the layer up, and a suite waiting for it gives up.
inline void endWhenClear(std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now()) {
    geode::Loader::get()->queueInMainThread([since] {
        if (!g_started || g_sessionOver) return;
        if (sceneBusy() && std::chrono::steady_clock::now() - since < kSwapWait) {
            endWhenClear(since);
            return;
        }
        endSession("slice_count");
    });
}

inline void swapTo(GJGameLevel* level, size_t objects) {
    forgetLayer();
    g_paused = true;
    g_keptNow = objects;
    swapWhenClear(geode::Ref<GJGameLevel>(level), 0);
}

inline std::string whyLine(const slice::Cut& c) {
    std::string s;
    for (const auto& [k, v] : c.why) s += " " + k + "=" + std::to_string(v);
    return s;
}

// ---- STAGES (cfg `stagecuts=<x1>,<x2>,...`) -------------------------------------------------
//
// A long level is solved in pieces, each on the level cut at an x (everything past it is gone, as
// py/cutlevel.py cuts it; the cut is where the copy ends and clears), and the plan that clears one
// piece is flown first on the next, longer one -- the last being the level itself. It is the
// level slice's swap and carry (above): the plan, the fixups, the vetoes and the round count go
// over with the swap, what is keyed by an object's uid is recorded again on the new layer. What
// differs is the copy -- a prefix, whose uids may not be the level's, so nothing here relies on
// them -- and the clear: a piece's clear is not verified or filed, it is handed on; only the
// level's own clear is filed (g_phase is Whole there).
//
// Why: the cost of a round that follows the level's length (the model's table, the recordings,
// the recorder's pass) is paid at the length of the piece, and a hand-off in one game saves the
// restart a hand-off between two games costs. A DEVELOPMENT AID, off by default: the plan one
// piece hands the next is a seed from the same run, so the result is not reported as a cold solve
// (CLAUDE.md rule 1) -- every piece is solved cold, the level's last one from the plan in hand.
// A cut inside a cluster of portals can hand on a lane that dead-ends past it (measured on
// a long custom level at x=45,400); the loop then walks back on the longer level as after any wall.
// The x of an object string ("1,id,2,x,3,y,..."): false when it has none.
inline bool objectX(std::string_view o, double* x) {
    size_t pos = 0;
    while (pos < o.size()) {
        const size_t c1 = o.find(',', pos);
        if (c1 == std::string_view::npos) return false;
        const size_t c2 = o.find(',', c1 + 1);
        if (o.substr(pos, c1 - pos) == "2") {
            const std::string v(o.substr(c1 + 1, (c2 == std::string_view::npos ? o.size() : c2) - c1 - 1));
            char* end = nullptr;
            *x = std::strtod(v.c_str(), &end);
            return end != v.c_str();
        }
        if (c2 == std::string_view::npos) return false;
        pos = c2 + 1;
    }
    return false;
}

// The largest object x of a level string.
inline double stringMaxX(const std::string& raw) {
    double best = 0.0;
    size_t pos = raw.find(';');
    if (pos == std::string::npos) return 0.0;
    for (++pos; pos < raw.size();) {
        size_t e = raw.find(';', pos);
        if (e == std::string::npos) e = raw.size();
        double x;
        if (e > pos && objectX(std::string_view(raw.data() + pos, e - pos), &x)) best = std::max(best, x);
        pos = e + 1;
    }
    return best;
}

// py/cutlevel.py cut_level: the header and every object whose x is not past the cut (an object with
// no x stays); when something was dropped and the survivors stop short of the cut, a decoration
// (id 503) at the cut puts the level's end there.
inline std::string cutPrefix(const std::string& raw, double cutX, size_t* kept, size_t* dropped) {
    *kept = *dropped = 0;
    const size_t h = raw.find(';');
    if (h == std::string::npos) return raw;
    std::string out = raw.substr(0, h + 1);
    double maxKept = 0.0;
    for (size_t pos = h + 1; pos < raw.size();) {
        size_t e = raw.find(';', pos);
        if (e == std::string::npos) e = raw.size();
        const std::string_view o(raw.data() + pos, e - pos);
        pos = e + 1;
        if (o.empty()) continue;
        double x = 0.0;
        const bool has = objectX(o, &x);
        if (has && x > cutX) { ++*dropped; continue; }
        out.append(o);
        out += ';';
        ++*kept;
        if (has) maxKept = std::max(maxKept, x);
    }
    if (*dropped > 0 && maxKept < cutX) {
        char m[96];
        snprintf(m, sizeof(m), "1,503,2,%g,3,15;", cutX);
        out += m;
    }
    return out;
}

inline std::vector<double> parseStageCuts(const std::string& s) {
    std::vector<double> v;
    for (size_t pos = 0; pos < s.size();) {
        size_t e = s.find_first_of(",; ", pos);
        if (e == std::string::npos) e = s.size();
        if (e > pos) {
            char* end = nullptr;
            const std::string t = s.substr(pos, e - pos);
            const double x = std::strtod(t.c_str(), &end);
            if (end != t.c_str() && x > 0.0) v.push_back(x);
        }
        pos = e + 1;
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// ---- where a cut may go (cfg stagecutcheck) ----
// A piece is the level with everything past the cut gone. A trigger that reads a GROUP AS A POSITION --
// a Rotate or Scale about a centre group, a Move to a target group, a Follow, an Area effect timed off its
// target as a whole (the reads of src/solver/slice.hpp) -- behaves differently when the group it reads is
// partly or wholly gone and the trigger is still there. For one such reader (trigger at x_T acting on Gt,
// reading Gp), a cut c is unsafe when c >= x_T, c < the largest x of Gp's members, and c >= the smallest x
// of Gt's and Gp's members. A rotate-gameplay or options trigger (2900, 2899) turns the frame or is read
// as if it did: 400 px either side of it are unsafe too. py/cutsafety.py is this rule on a level file.
// A PREDICTOR from the string: the truth is stageCompare's line at the hand-off.
struct StageReader {
    double lo, hi;       // the unsafe cuts
    std::string why;
};

inline std::vector<StageReader> stageReaders(const std::string& base) {
    std::vector<StageReader> rows;
    struct Reader { double x; int id, target; std::vector<int> reads; };
    std::unordered_map<int, std::pair<double, double>> span;   // group -> smallest and largest member x
    std::vector<Reader> readers;
    const size_t h = base.find(';');
    if (h == std::string::npos) return rows;
    for (size_t pos = h + 1; pos < base.size();) {
        size_t e = base.find(';', pos);
        if (e == std::string::npos) e = base.size();
        const std::string_view o(base.data() + pos, e - pos);
        pos = e + 1;
        if (o.empty()) continue;
        const int id = (int)slice::numOr(slice::kvGet(o, "1"), 0);
        const double x = slice::numOr(slice::kvGet(o, "2"), 0);
        for (const char* key : {"57", "274"})
            for (const int g : slice::intList(slice::kvGet(o, key))) {
                auto it = span.find(g);
                if (it == span.end()) span[g] = {x, x};
                else { it->second.first = std::min(it->second.first, x); it->second.second = std::max(it->second.second, x); }
            }
        if (id == 2899 || id == 2900) {
            rows.push_back({x - 400.0, x + 400.0, "rotate-gameplay/options trigger " + std::to_string(id)
                                                    + " at x=" + std::to_string((long long)x)});
            continue;
        }
        const bool area = id >= 3006 && id <= 3010;
        if (!(id == 901 || id == 1346 || id == 1347 || id == 2067 || id == 1814 || area)) continue;
        const int t = (int)slice::numOr(slice::kvGet(o, "51"), 0);
        if (t <= 0) continue;
        Reader r{x, id, t, {}};
        const int c = (int)slice::numOr(slice::kvGet(o, "71"), 0);
        const int tm = (int)slice::numOr(slice::kvGet(o, "395"), 0);
        if (c > 0) r.reads.push_back(c);
        if (tm > 0) r.reads.push_back(tm);
        if (area || slice::numOr(slice::kvGet(o, "100"), 0) == 1 || slice::numOr(slice::kvGet(o, "397"), 0) == 1)
            r.reads.push_back(t);
        if (!r.reads.empty()) readers.push_back(std::move(r));
    }
    for (const Reader& r : readers) {
        double gpLo = 1e18, gpHi = -1e18;
        for (const int g : r.reads) {
            const auto it = span.find(g);
            if (it == span.end()) continue;
            gpLo = std::min(gpLo, it->second.first);
            gpHi = std::max(gpHi, it->second.second);
        }
        if (gpHi < gpLo) continue;
        double lo = gpLo;
        const auto tg = span.find(r.target);
        if (tg != span.end()) lo = std::min(lo, tg->second.first);
        lo = std::max(lo, r.x);
        if (gpHi > lo)
            rows.push_back({lo, gpHi, "trigger " + std::to_string(r.id) + " at x=" + std::to_string((long long)r.x)
                                          + " on group " + std::to_string(r.target) + " reads group "
                                          + std::to_string(r.reads.front())});
    }
    return rows;
}

// The cuts of cfg stagecuts, checked against stageReaders(base): logged (stagecutcheck=1), or moved to the
// nearest x that splits no reader within stagecutwindow px, or dropped when there is none (=2). False when
// no cut is left.
inline bool checkStageCuts(const std::string& base) {
    if (g_cfg.stageCutCheck <= 0) return true;
    const std::vector<StageReader> rows = stageReaders(base);
    constexpr double kMargin = 120.0;
    std::vector<std::pair<double, double>> bad;   // merged, margin included
    {
        std::vector<std::pair<double, double>> v;
        for (const auto& r : rows) v.push_back({r.lo - kMargin, r.hi + kMargin});
        std::sort(v.begin(), v.end());
        for (const auto& p : v) {
            if (!bad.empty() && p.first <= bad.back().second) bad.back().second = std::max(bad.back().second, p.second);
            else bad.push_back(p);
        }
    }
    const double maxX = stringMaxX(base);
    std::vector<double> out;
    char b[420];
    for (const double c : g_stageCuts) {
        const auto in = std::find_if(bad.begin(), bad.end(), [&](const auto& p) { return c >= p.first && c < p.second; });
        if (in == bad.end()) { out.push_back(c); continue; }
        std::string list;
        int n = 0;
        for (const auto& r : rows)
            if (c >= r.lo - kMargin && c < r.hi + kMargin && n++ < 3) list += (list.empty() ? "" : "; ") + r.why;
        double best = -1.0;
        for (const double cand : {in->first - 1.0, in->second})
            if (cand > 0.0 && cand < maxX && (out.empty() || cand > out.back()) && std::fabs(cand - c) <= g_cfg.stageCutWindow
                && (best < 0.0 || std::fabs(cand - c) < std::fabs(best - c)))
                best = cand;
        if (g_cfg.stageCutCheck == 1) {
            snprintf(b, sizeof(b), "stage: cut x=%.0f splits what a kept trigger reads (unsafe x %.0f..%.0f): %s - kept "
                     "as asked (stagecutcheck=1)", c, in->first, in->second, list.c_str());
            out.push_back(c);
        } else if (best > 0.0) {
            snprintf(b, sizeof(b), "stage: cut x=%.0f splits what a kept trigger reads (unsafe x %.0f..%.0f): %s - "
                     "moved to x=%.0f", c, in->first, in->second, list.c_str(), best);
            out.push_back(best);
        } else {
            snprintf(b, sizeof(b), "stage: cut x=%.0f splits what a kept trigger reads (unsafe x %.0f..%.0f): %s - "
                     "no safe x within %d px, the cut is dropped", c, in->first, in->second, list.c_str(), g_cfg.stageCutWindow);
        }
        writeResult(b);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    g_stageCuts = out;
    if (out.empty()) writeResult("stage: no cut is left - solving as the slice would");
    return !out.empty();
}

inline double sessionSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
}

// The copy of piece `idx`: the level cut at its x, kept as stage_lv<id>_<idx>.lvl for a look.
inline GJGameLevel* makeStageCopy(int idx, size_t* keptOut) {
    size_t kept = 0, dropped = 0;
    const std::string s = cutPrefix(g_stageBase, g_stageCuts[(size_t)idx], &kept, &dropped);
    {
        std::ofstream f(std::string(DATA_DIR) + "/stage_lv" + std::to_string(g_cfg.levelId) + "_"
                            + std::to_string(idx) + ".lvl", std::ios::binary | std::ios::trunc);
        f << s;
    }
    char b[240];
    snprintf(b, sizeof(b), "stage: piece %d of %zu - the level cut at x=%.0f (%zu objects kept, %zu "
             "dropped); %.1f s into the session", idx + 1, g_stageCuts.size() + 1,
             g_stageCuts[(size_t)idx], kept, dropped, sessionSeconds());
    writeResult(b);
    *keptOut = kept;
    return makeCopy(s);
}

// The pieces' cuts for a level whose objects reach `maxX`: false (and a line) when none is short of it.
inline bool setStageCuts(double maxX) {
    std::vector<double> cuts;
    for (const double c : parseStageCuts(g_cfg.stageCuts))
        if (c < maxX) cuts.push_back(c);
    if (cuts.empty()) {
        writeResult("stage: not used - cfg stagecuts=" + g_cfg.stageCuts + " has no cut short of the "
                    "level's last object (x=" + std::to_string((long long)maxX) + ")");
        return false;
    }
    g_stageCuts = cuts;
    return true;
}

// The pieces are cut from `base`: the level's own string, or -- when the slice applies -- the slice's
// copy of it (placeholders and all), so a piece is the slice of the level cut at an x and the last
// piece is the slice's copy itself. g_raw and g_orig are set. True = the first piece is being swapped in.
inline bool startStages(std::string base, bool sliced) {
    if (!checkStageCuts(base)) return false;   // cfg stagecutcheck
    g_stageBase = std::move(base);
    g_stageSliced = sliced;
    g_stage = 0;
    g_stageHandOffs = 0;
    if (sliced) g_uidCheckPending = false;   // the pieces are prefixes: the uid check is for the last one
    std::string list;
    for (const double c : g_stageCuts) list += (list.empty() ? "" : ",") + std::to_string((long long)c);
    writeResult("stage: " + std::to_string(g_stageCuts.size() + 1) + " pieces, cut at x=" + list
                + (sliced ? " and the slice's copy" : " and the level itself")
                + " (last object x=" + std::to_string((long long)stringMaxX(g_raw)) + ")");
    size_t kept = 0;
    GJGameLevel* copy = makeStageCopy(0, &kept);
    g_phase = Sliced;
    g_hudPhase = "solving the first piece";
    swapTo(copy, kept);
    return true;
}

// With no slice to cut from (disabled, below the threshold, refused): the pieces are cut from the level.
inline bool beginStagesRaw(GJBaseGameLayer* l) {
    auto* pl = PlayLayer::get();
    if (!pl || pl != l || !pl->m_level) return false;
    g_t0 = std::chrono::steady_clock::now();
    std::string raw = levelString(pl->m_level);
    if (raw.empty()) {
        writeResult("stage: not used - the level string could not be read");
        return false;
    }
    g_raw = std::move(raw);
    if (!setStageCuts(stringMaxX(g_raw))) {
        g_raw.clear();
        return false;
    }
    holdOrig(pl->m_level);
    return startStages(g_raw, false);
}

inline long long partingTick(const std::vector<AnchorRow>& a, const std::vector<AnchorRow>& b,
                             long long until, const char** field);

// Ticks before a piece's clear within which a parting from the next piece is read as the edge of the piece
// (the end of the level, the camera and the bands settle there), not as the cut: the log gives the tick either way.
constexpr long long kStageEdgeTicks = 150;

// The first flight on a piece, compared with the clearing flight of the piece before it: the same plan on the
// same level up to the cut should give the same player tick for tick. Where it does not, something past the
// cut reached back -- a group with members on both sides (a rotate or scale about the group's centre, a
// member picked by the group's count), a trigger the dropped part fired, an object the cut took out of the way.
// `end` is the tick that flight ended on; a pending check is spent by the first flight that ends.
inline void stageCompare(long long end, bool cleared) {
    if (!g_stageWatch) return;
    g_stageWatch = false;
    const long long until = std::min(end, g_stageClearT);
    const char* field = "";
    const long long pt = partingTick(g_stageRows, anchors::g_live, until, &field);
    const int prev = g_stage;   // the piece being left is prev - 1 (g_stage counts from 0)
    char b[400];
    if (pt < 0) {
        if (!cleared && end < g_stageClearT) {
            // the same on every tick, then dead before the piece's clear: what only the longer piece has kills there
            const bool edge = g_stageClearT - end <= kStageEdgeTicks;
            if (edge) ++g_stageEdge; else ++g_stageApart;
            snprintf(b, sizeof(b), "stage: hand-off %d at x=%.0f - %s: the next piece's first flight matches the clearing "
                     "flight to t=%lld, then died at t=%lld, %lld ticks before the clear (t=%lld): what the longer piece has that the "
                     "shorter did not kills there (or the plan is not the one that cleared)",
                     prev, g_stageCutX, edge ? "parted at the edge of the piece" : "DISAGREE", until, end,
                     g_stageClearT - end, g_stageClearT);
        } else {
            ++g_stageSame;
            snprintf(b, sizeof(b), "stage: hand-off %d at x=%.0f - consistent: the next piece's first flight matches the "
                     "clearing flight on every tick to t=%lld", prev, g_stageCutX, until);
        }
    } else {
        const double px = (size_t)pt < g_stageRows.size() ? g_stageRows[(size_t)pt].x : 0.0;
        const long long before = g_stageClearT - pt;
        if (before <= kStageEdgeTicks) ++g_stageEdge; else ++g_stageApart;
        snprintf(b, sizeof(b), "stage: hand-off %d at x=%.0f - %s: the next piece's first flight parts from the clearing "
                 "flight at t=%lld x=%.1f (%s), %lld ticks before the clear (t=%lld)", prev, g_stageCutX,
                 before <= kStageEdgeTicks ? "parted at the edge of the piece" : "DISAGREE", pt, px, field, before,
                 g_stageClearT);
    }
    writeResult(b);
}

// A piece cleared: its plan goes over to the next, with what the run has learnt.
inline bool onStageCleared(const std::vector<InputCmd>& plan, long long tick) {
    const int done = g_stage;
    stageCompare(tick, true);   // the flight that cleared this piece may itself be the first on it
    g_stageRows = anchors::g_live;
    g_stageClearT = tick;
    g_stageCutX = g_stageCuts[(size_t)done];
    g_stageWatch = true;
    ++g_stage;
    ++g_stageHandOffs;
    g_carry = plan;
    if (g_cfg.stageBreak > 0 && g_carry.size() > (size_t)g_cfg.stageBreak) {   // test only: a hand-off that parts on purpose
        char kb[160];
        snprintf(kb, sizeof(kb), "stage: cfg stagebreak - the plan handed on keeps its first %d of %zu inputs",
                 g_cfg.stageBreak, g_carry.size());
        writeResult(kb);
        g_carry.resize((size_t)g_cfg.stageBreak);
    }
    g_carryPending = true;
    const bool last = g_stage >= (int)g_stageCuts.size();
    char b[300];
    snprintf(b, sizeof(b), "stage: piece %d (cut x=%.0f) cleared at t=%lld after %d rounds, %.1f s "
             "into the session - handing its plan (%zu inputs) to %s", done + 1,
             g_stageCuts[(size_t)done], tick, dpsolve::g_iter, sessionSeconds(), plan.size(),
             last ? "the level itself" : "the next piece");
    writeResult(b);
    if (last && g_stageSliced) {
        // the slice's own copy, as the slice would have started on it; its clear is verified on the level
        g_phase = Sliced;
        g_hudPhase = "solving on the slice's copy";
        size_t kept = 0;
        const std::string s = copyString(&kept);
        swapTo(makeCopy(s), kept + g_placeholders);
    } else if (last) {
        g_phase = Whole;
        g_hudPhase = "solving on the level itself";
        swapTo(g_orig, g_cut.objects);
    } else {
        g_phase = Sliced;
        g_hudPhase = "solving the next piece";
        size_t kept = 0;
        GJGameLevel* copy = makeStageCopy(g_stage, &kept);
        swapTo(copy, kept);
    }
    return true;
}

// Asked by the session's first solve. True = the level is being swapped for its slice, and the
// solve starts there instead (dpsolve::start runs again on the new layer).
inline bool maybeSliceOnly(GJBaseGameLayer* l) {
    // On a copy this is also where it is first seen loaded: its uids are checked against the level.
    if (g_phase != Off) {
        if (g_uidCheckPending && (g_phase == Sliced || g_phase == RefFlight) && l == PlayLayer::get())
            checkCopyUids(l);
        return false;
    }
    if (!g_cfg.slice || !g_cfg.dpSolve) return false;
    auto* pl = PlayLayer::get();
    if (!pl || pl != l || !pl->m_level) return false;
    g_t0 = std::chrono::steady_clock::now();
    std::string raw = levelString(pl->m_level);
    if (raw.empty()) {
        writeResult("slice: not used - the level string could not be read");
        return false;
    }
    g_raw = std::move(raw);
    const std::vector<slice::LiveObj> live = liveObjects(l);
    // A run that takes coins keeps every trigger on the copy. The solver's choice of touch triggers
    // counts a box whose Move shifts only decoration as one that moves something, so a copy without
    // those Moves gives the search a different set of boxes and a different route -- the same
    // physics, another search. On level 22 with coins that route missed its first coin and timed
    // out after 60 iterations; with the triggers kept the copy's calls read the level's own
    // trigger table and it cleared with all three coins.
    const bool dropTriggers = g_cfg.sliceTriggers && !g_cfg.coinRoute;
    g_cut = slice::cut(g_raw, live, slice::Options{!g_cfg.sliceNoPosition, g_cfg.sliceNoTouch,
                                                   dropTriggers});
    if (g_cfg.sliceNoPosition)
        writeResult("slice: cfg slicenoposition - decorations read as positions are dropped too "
                    "(a cut that is wrong on purpose)");
    if (!g_cut.ok) {
        writeResult("slice: not used - " + g_cut.refused);
        g_cut = slice::Cut{};
        g_raw.clear();
        return false;
    }
    char b[320];
    snprintf(b, sizeof(b), "slice: %zu objects, %zu the run cannot depend on (threshold %d); "
             "%zu positional / %zu relevant groups;", g_cut.objects, g_cut.dropped, sliceMin(),
             g_cut.positionalGroups, g_cut.relevantGroups);
    writeResult(b + whyLine(g_cut));
    // cfg `slicecount=1`: the census of what the rule would remove, one line per level, and no
    // solve (a suite steps straight on to its next level).
    if (g_cfg.sliceCount) {
        writeResult("slice: count only (cfg slicecount) - nothing is solved");
        g_cut = slice::Cut{};
        g_raw.clear();
        g_paused = true;
        endWhenClear();
        return true;
    }
    if ((long long)g_cut.dropped < (long long)sliceMin()) {
        writeResult("slice: not used - below the threshold; solving on the level itself");
        g_cut = slice::Cut{};
        g_raw.clear();
        return false;
    }
    // A run that takes the level's coins takes them on the copy too: its secret coins are built as
    // the level builds them (buildsAsLevel), and a user coin is built and credited in an editor-type
    // level as in a main one (config.hpp, levelFileMain). The copy's own count is reported when it
    // has been built (hooks_playlayer.cpp createObjectsFromSetupFinished).
    g_origCoins = std::count_if(live.begin(), live.end(), [](const slice::LiveObj& o) {
        return o.id == 142 || o.id == 1329;
    });
    holdOrig(pl->m_level);
    noteLevelUids(l);
    size_t kept = 0;
    const std::string cutStr = copyString(&kept);
    {
        std::ofstream f(std::string(DATA_DIR) + "/slice_lv" + std::to_string(g_cfg.levelId)
                            + ".lvl",
                        std::ios::binary | std::ios::trunc);
        f << cutStr;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_t0).count();
    snprintf(b, sizeof(b), "slice: solving on a copy with %zu of %zu objects (%.1f%%), cut in "
             "%lld ms - its plan is verified on the level itself before it is filed", kept,
             g_cut.objects, 100.0 * (double)kept / (double)g_cut.objects, (long long)ms);
    writeResult(b);
    writeResult(g_cfg.sliceUids
                    ? "slice: the " + std::to_string(g_placeholders) + " objects left out are each "
                      "replaced by a placeholder that takes their uids (slice.hpp)"
                    : std::string("slice: cfg sliceuids=0 - the objects left out are removed, which "
                                  "renumbers every object after them (not for fidelity)"));
    // cfg stagecuts: the pieces are cut from this copy (STAGES, above); the last is the copy itself.
    if (!g_cfg.stageCuts.empty() && setStageCuts(stringMaxX(g_raw)) && startStages(cutStr, true)) return true;
    g_phase = Sliced;
    swapTo(makeCopy(cutStr), kept + g_placeholders);
    return true;
}

// Asked by the session's first solve. True = the level is being swapped for its slice (or, with cfg
// stagecuts, for its first piece), and the solve starts there instead.
inline bool maybeSlice(GJBaseGameLayer* l) {
    const bool first = g_phase == Off;
    if (maybeSliceOnly(l)) return true;
    // cfg stagecuts without a slice to cut from: the pieces are cut from the level itself.
    if (first && g_phase == Off && !g_cfg.stageCuts.empty() && g_cfg.dpSolve) return beginStagesRaw(l);
    return false;
}

// Asked by dpsolve::start on each new layer: the plan to fly first, if one was handed over.
inline bool takeCarry(std::vector<InputCmd>& out) {
    if (!g_carryPending) return false;
    g_carryPending = false;
    out = std::move(g_carry);
    g_carry.clear();
    return true;
}

// ...and whether it is to be flown without touching the loop (g_lightPending).
inline bool takeLight(std::vector<InputCmd>& out) {
    if (!g_lightPending) return false;
    g_lightPending = false;
    out = std::move(g_carry);
    g_carry.clear();
    return true;
}

// The copy was cleared (levelComplete, past its false-clear and coin gates). True = taken over:
// the plan goes to the level itself instead of being filed.
inline bool onRefFlightEnd(long long end, double endX, bool cleared);
inline bool onSliceCleared(const std::vector<InputCmd>& plan, long long tick) {
    // cfg slicefirstref: a first-attempt flight on a copy that clears is never a clear to file.
    if (g_phase == RefFlight) {
        double x = 0.0;
        for (auto it = anchors::g_live.rbegin(); it != anchors::g_live.rend(); ++it)
            if (it->valid) { x = it->x; break; }
        onRefFlightEnd(tick, x, true);
        return true;
    }
    if (g_phase != Sliced || !g_orig) return false;
    if (g_stage >= 0 && g_stage < (int)g_stageCuts.size()) return onStageCleared(plan, tick);   // cfg stagecuts
    if (g_stage >= 0) stageCompare(tick, true);   // the last piece cleared on the first flight of the hand-off
    g_clearRows = anchors::g_live;
    g_refEnd = tick;
    g_refCleared = true;
    std::vector<InputCmd> flown = plan;
    // cfg sliceverifybreak (test only): a plan that does not hold on the level, wrong on purpose
    if (g_cfg.sliceVerifyBreak > 0 && flown.size() > (size_t)g_cfg.sliceVerifyBreak) {
        char kb[160];
        snprintf(kb, sizeof(kb), "slice: cfg sliceverifybreak - the plan handed to the level keeps its first %d "
                 "of %zu inputs (a verification that fails on purpose)", g_cfg.sliceVerifyBreak, flown.size());
        writeResult(kb);
        flown.resize((size_t)g_cfg.sliceVerifyBreak);
    }
    g_carry = flown;
    g_carryPending = true;
    g_phase = Verifying;
    g_verifyRecord = false;   // cfg sliceverifyrec: this check flies without the recording ...
    g_clearPlan = flown;      // ... and with it again if it dies (onVerifyDeath)
    ++g_verifies;
    char b[256];
    snprintf(b, sizeof(b), "slice: the copy cleared at t=%lld after %d rounds - flying the plan "
             "on the level itself (%zu inputs)", tick, dpsolve::g_iter, flown.size());
    writeResult(b);
    g_hudPhase = "verifying the plan on the level itself";
    swapTo(g_orig, g_cut.objects);
    return true;
}

// The level itself was cleared.
inline void onLevelCleared(long long tick) {
    if (g_phase == Off) return;
    char b[200];
    snprintf(b, sizeof(b), "slice: cleared on the level itself at t=%lld (%s, %d add-back%s)",
             tick, g_phase == Verifying ? "the copy's plan held" : "after solving on it",
             g_addBacks, g_addBacks == 1 ? "" : "s");
    writeResult(b);
}

// Where the attempt on the level itself first parted from the copy's clearing attempt: the first
// tick whose player state differs. -1 when none does before `until`.
inline long long partingTick(const std::vector<AnchorRow>& a, const std::vector<AnchorRow>& b,
                             long long until, const char** field) {
    const long long n = std::min<long long>({(long long)a.size(), (long long)b.size(), until});
    for (long long t = 0; t < n; ++t) {
        const AnchorRow& p = a[(size_t)t];
        const AnchorRow& q = b[(size_t)t];
        if (!p.valid || !q.valid) continue;
        // Not the flight band: the game carries it from one attempt to the next, so an attempt on
        // a layer just swapped in starts with another band than the copy's hundredth attempt did,
        // whatever the cut. Compared, it parted every check at tick 1 (a heavy custom level whose
        // copy's plan cleared the level itself). Where the band matters it moves the player.
        const char* f = nullptr;
        if (p.x != q.x) f = "x";
        else if (p.y != q.y) f = "y";
        else if (p.vy != q.vy) f = "vy";
        else if (p.mode != q.mode) f = "mode";
        if (f) { *field = f; return t; }
    }
    return -1;
}

inline size_t buildCopy(GJGameLevel** out) {
    size_t kept = 0;
    *out = makeCopy(copyString(&kept));
    return kept + g_placeholders;
}

// After a verification has read a death as the cut's: the objects dropped around partX go back and
// the solve goes on on that copy -- or, with the add-backs spent or the parting not moving on, on
// the level itself.
inline bool addBackOrWhole(double partX) {
    char b[360];
    if (g_addBacks < g_cfg.sliceAddBacks && partX > g_lastPartX) {
        g_lastPartX = partX;
        ++g_addBacks;
        g_keepX.push_back({partX - kAddBackHalfWidth, partX + kAddBackHalfWidth});
        size_t kept = 0;
        const std::string cutStr = copyString(&kept);
        snprintf(b, sizeof(b), "slice: add-back %d - the objects dropped within %.0f px of x=%.0f "
                 "go back; solving on a copy with %zu of %zu objects, from the same plan",
                 g_addBacks, kAddBackHalfWidth, partX, kept, g_cut.objects);
        writeResult(b);
        g_carry = dpsolve::g_plan;
        g_carryPending = true;
        g_phase = Sliced;
        g_hudPhase = "solving on the copy with objects put back";
        swapTo(makeCopy(cutStr), kept + g_placeholders);
        return true;
    }
    writeResult(std::string("slice: solving on the level itself from here, with the plan and "
                            "what the run has learnt (")
                + (g_addBacks >= g_cfg.sliceAddBacks ? "add-backs spent" : "the parting did not "
                                                                             "move on")
                + ")");
    g_phase = Whole;
    g_keptNow = g_cut.objects;
    if (g_refCleared) return false;   // the loop is already the level's (a full carry)
    // A wall check's flight left the loop as the copy had it, keyed by the copy's uids: the level
    // is loaded again for a full carry.
    g_carry = dpsolve::g_plan;
    g_carryPending = true;
    swapTo(g_orig, g_cut.objects);
    return true;
}

// cfg slicefirstref: the end of the plan's flight on a fresh copy (onVerifyDeath started it), the
// copy's first attempt of the plan, compared with the level's flight of it -- also a first attempt.
// A wall check compares the level with the copy's DEEPEST attempt, which can be its hundredth, and
// what the game carries from one attempt to the next (the flight band) can move the player there:
// on a heavy custom level a check parted from the copy at t=10,460 and put objects back, while the
// same plan flown as a first attempt on the level and on the copy matched on every column for
// 25,588 ticks. So when the two first attempts agree and die on the same tick, the wall is the
// level's own and nothing goes back; when they part, the parting is the cut's. True = taken over;
// false = this death is the loop's round on the copy, as after any return to it.
inline bool onRefFlightEnd(long long end, double endX, bool cleared) {
    const char* field = "";
    const long long upTo = std::min(g_levelDeath, end);
    const long long pt = partingTick(anchors::g_live, g_levelRows, upTo, &field);
    char b[360];
    g_phase = Sliced;
    g_hudPhase = "solving on the copy";
    // ...but two first attempts that agree on a death BEFORE the copy's wall have said nothing about
    // that wall: they never reached it. A custom level that turns a spike on at its start for an
    // attempt count of 0 kills every first attempt at t=1 -- the check flight's and the fresh
    // copy's alike, while the loop's own attempts (counts of 1 and up) live -- and taking that
    // death for the wall put t=1 in g_checkedWall: the wall at t=7,243 was checked again on every
    // round, 207 times, until the level's budget ran out. The wall being checked (g_refEnd, which
    // maybeCheckWall also put in g_checkedWall) stays checked, nothing is put back, and the solve
    // goes back to the copy.
    if (pt < 0 && !cleared && end == g_levelDeath && end < g_refEnd) {
        snprintf(b, sizeof(b), "slice: first attempts agree at t=%lld, before the copy's wall at "
                 "t=%lld - the copy's first attempt of the plan dies there as the level's does, so "
                 "the check says nothing about the wall; it is not checked again and nothing is "
                 "put back", end, g_refEnd);
        writeResult(b);
        g_levelRows.clear();
        return false;
    }
    if (pt < 0 && !cleared && end == g_levelDeath) {
        snprintf(b, sizeof(b), "slice: first attempts agree - the copy's first attempt of the plan "
                 "dies at t=%lld as the level's does, the same way; the check's %s the copy's "
                 "deepest attempt, not the cut. The wall is the level's own; nothing is put back",
                 end, g_firstParting >= 0 ? "parting was" : "difference was");
        writeResult(b);
        g_levelRows.clear();
        // The wall this check was about is the COPY's (g_refEnd, the loop's g_bestDeath when
        // maybeCheckWall fired), not the tick the two first attempts died on: the copy's deepest
        // attempt can die earlier than a first attempt of the same plan. Marking `end` let the very
        // next round find its wall unchecked and fly the same check again. A custom level: the copy's
        // wall at t=15,834, both first attempts dying at 16,019 -- the check ran 125 times, the four
        // lines identical each time, until the level's budget ran out. The wall stays checked until
        // the loop's deepest death moves, which is maybeCheckWall's own "once per wall".
        g_checkedWall = g_refEnd;
        g_ownWall = g_refEnd;
        return false;
    }
    double partX;
    if (pt >= 0) {
        partX = (size_t)pt < g_levelRows.size() ? g_levelRows[(size_t)pt].x : endX;
        snprintf(b, sizeof(b), "slice: first attempts part at t=%lld x=%.1f (%s) - the level's "
                 "(died t=%lld) and the copy's (%s t=%lld): the parting is the cut's", pt, partX,
                 field, g_levelDeath, cleared ? "cleared" : "died", end);
    } else if (cleared || end > g_levelDeath) {
        partX = g_levelDeathX;
        snprintf(b, sizeof(b), "slice: the copy's first attempt %s t=%lld, past the level's death "
                 "at t=%lld x=%.0f, the same way until then: that death is the cut's",
                 cleared ? "cleared at" : "died at", end, g_levelDeath, partX);
    } else {
        partX = endX;
        snprintf(b, sizeof(b), "slice: the copy's first attempt died at t=%lld x=%.0f, before the "
                 "level's (t=%lld), the same way until then: the level went past the copy's wall",
                 end, partX, g_levelDeath);
    }
    writeResult(b);
    g_levelRows.clear();
    return addBackOrWhole(partX);
}

// A death while a plan from the copy is flown on the level itself -- the plan that cleared the
// copy, or the copy's deepest plan when its wall is being checked (maybeCheckWall). True = taken
// over (the solve goes back to a copy: with the objects around the parting put back, or as it was
// when the wall is the level's own); false = the loop takes this death on the level itself, as
// any other.
inline bool onVerifyDeath(long long dt, float deathX) {
    if (g_stageWatch) stageCompare(dt, false);   // cfg stagecuts: the first flight after a hand-off
    if (g_phase == RefFlight) return onRefFlightEnd(dt, deathX, false);
    if (g_phase != Verifying) return false;
    // cfg sliceverifyrec: the check of a plan that cleared the copy flew without the recording of moving
    // geometry, and it died. Where the loop takes this death on the level itself (the add-backs spent, or the two
    // runs not in step), the solve goes on from that recording -- so the plan is flown again, recorded, on a
    // fresh swap exactly as the first flight was, and the second flight's death is read as every death of a
    // recorded check always was. (A wall check's death never reaches the loop: every way out of it below swaps
    // to a copy.) The first flight's death is not scored, as with every take-over here.
    if (g_heldFlight && g_refCleared && !g_clearPlan.empty()) {
        ++g_reflies;
        g_heldFlight = false;
        g_verifyRecord = true;
        char rb[300];
        snprintf(rb, sizeof(rb), "slice: the check flight on the level died at t=%lld x=%.0f without the "
                 "recording of moving geometry (cfg sliceverifyrec=0) - flying the plan on the level again "
                 "with it, then the death is read", dt, (double)deathX);
        writeResult(rb);
        g_carry = g_clearPlan;
        g_carryPending = true;
        swapTo(g_orig, g_cut.objects);
        return true;
    }
    const char* field = "";
    const long long upTo = std::min(dt, g_refEnd >= 0 ? g_refEnd : dt);
    long long pt = partingTick(g_clearRows, anchors::g_live, upTo, &field);
    double partX = deathX;
    if (pt >= 0 && (size_t)pt < anchors::g_live.size()) partX = anchors::g_live[(size_t)pt].x;
    char b[360];
    // FAIL-SAFE: a parting is read as the cut's only when the two runs were in step. They are not
    // when the flight's inputs were taken before its first tick (the layer a swap replaced once
    // fed on them: 28c44ad), and they are not proven to be before the plan's first press has had
    // its effect -- a parting there says the runs did not start alike, which no object put back
    // around it can mend. Either way nothing is put back: a wall check goes back to the copy with
    // the wall marked checked, and a clear's check goes on on the level itself. Both sides around
    // the first press are logged, so a mismatch of this kind can be looked at.
    long long firstPress = -1;
    for (const auto& in : g_cfg.inputs)
        if (in.down && !in.p2) { firstPress = in.step; break; }
    const long long syncTick = std::max<long long>(firstPress, 0) + kSyncTicks;
    const bool tookEarly = g_nextInputAtStart != 0;
    const bool beforeSync = pt >= 0 && pt <= syncTick;
    {
        std::string s = "slice: sync check - the flight's first tick found "
                        + std::to_string(g_nextInputAtStart) + " input(s) already taken; the "
                        "plan's first press at t=" + std::to_string(firstPress) + "; copy|level";
        for (long long t = std::max<long long>(firstPress, 1); t <= syncTick + 1; ++t) {
            const bool ha = (size_t)t < g_clearRows.size() && g_clearRows[(size_t)t].valid;
            const bool hb = (size_t)t < anchors::g_live.size() && anchors::g_live[(size_t)t].valid;
            const AnchorRow& ra = ha ? g_clearRows[(size_t)t] : AnchorRow{};
            const AnchorRow& rb = hb ? anchors::g_live[(size_t)t] : AnchorRow{};
            char r[160];
            snprintf(r, sizeof(r), " t%lld %.3f/%.3f/%.3f/m%d|%.3f/%.3f/%.3f/m%d", t, ra.x, ra.y,
                     ra.vy, ra.mode, rb.x, rb.y, rb.vy, rb.mode);
            s += r;
        }
        writeResult(s);
    }
    if (tookEarly || beforeSync) {
        snprintf(b, sizeof(b), "slice: the flight on the level itself died at t=%lld x=%.0f, %s - "
                 "read as the two runs not being in step, not as the cut: nothing is put back",
                 dt, (double)deathX,
                 tookEarly ? "with inputs taken before its first tick"
                           : (std::string("parting from the copy at t=") + std::to_string(pt)
                              + " (" + field + "), before the plan's first press had had its "
                                "effect").c_str());
        writeResult(b);
        g_clearRows.clear();
        if (!g_refCleared) {
            // Back to the same copy, as when the wall is the level's own; g_checkedWall keeps
            // this wall from being checked again.
            g_carry = dpsolve::g_plan;
            g_lightPending = true;
            g_phase = Sliced;
            GJGameLevel* copy = nullptr;
            const size_t kept = buildCopy(&copy);
            swapTo(copy, kept);
            return true;
        }
        writeResult("slice: solving on the level itself from here, with the plan and what the run "
                    "has learnt (the check flight was not in step with the copy)");
        g_phase = Whole;
        g_keptNow = g_cut.objects;
        return false;   // the loop is already the level's (a full carry)
    }
    // cfg slicefirstref: before a wall check's death is read as the cut's, the plan is flown on a
    // fresh copy as its first attempt, and that is what the level's flight is compared with
    // (onRefFlightEnd). A check that finds the wall the level's own needs no second flight.
    const bool ownWall = pt < 0 && !g_refCleared && dt == g_refEnd;
    if (g_cfg.sliceFirstRef && !g_refCleared && !ownWall) {
        g_levelRows = anchors::g_live;
        g_levelDeath = dt;
        g_levelDeathX = deathX;
        g_firstParting = pt;
        ++g_refFlights;
        snprintf(b, sizeof(b), "slice: the check flight died at t=%lld x=%.0f, %s the copy's deepest "
                 "attempt (which is not a first attempt) - flying the plan on a fresh copy to "
                 "compare the two first attempts (cfg slicefirstref)", dt, (double)deathX,
                 pt >= 0 ? ("parting at t=" + std::to_string(pt) + " (" + field + ") from").c_str()
                         : "not ending like");
        writeResult(b);
        g_clearRows.clear();
        g_carry = g_verifyPlan;
        g_lightPending = true;
        g_phase = RefFlight;
        g_hudPhase = "flying the plan on a fresh copy";
        GJGameLevel* copy = nullptr;
        const size_t kept = buildCopy(&copy);
        swapTo(copy, kept);
        return true;
    }
    if (pt >= 0) {
        snprintf(b, sizeof(b), "slice: the plan died on the level itself at t=%lld x=%.0f; it "
                 "parted from the copy at t=%lld x=%.1f (%s)", dt, (double)deathX, pt, partX,
                 field);
    } else if (!g_refCleared && dt == g_refEnd) {
        // The copy's wall is the level's own: the same plan dies on the same tick, and the
        // player's state agreed with the copy's all the way there. Back to the same copy.
        snprintf(b, sizeof(b), "slice: the copy's wall at t=%lld is the level's own - the plan "
                 "dies there on the level too, the same way; back to the copy", dt);
        writeResult(b);
        g_clearRows.clear();
        g_checkedWall = dt;
        g_ownWall = dt;
        // The same copy, from the same string: the loop carries on as it was (g_lightPending).
        g_carry = dpsolve::g_plan;
        g_lightPending = true;
        g_phase = Sliced;
        GJGameLevel* copy = nullptr;
        const size_t kept = buildCopy(&copy);
        swapTo(copy, kept);
        return true;
    } else if (!g_refCleared && dt > g_refEnd) {
        // The level went past the copy's wall: that death was the cut's.
        pt = g_refEnd;
        if ((size_t)pt < g_clearRows.size() && g_clearRows[(size_t)pt].valid)
            partX = g_clearRows[(size_t)pt].x;
        snprintf(b, sizeof(b), "slice: the plan went on past the copy's wall on the level itself "
                 "(the copy died at t=%lld x=%.1f, the level at t=%lld x=%.0f)", g_refEnd, partX,
                 dt, (double)deathX);
    } else {
        snprintf(b, sizeof(b), "slice: the plan died on the level itself at t=%lld x=%.0f; the "
                 "player's state matched the copy's until then", dt, (double)deathX);
    }
    writeResult(b);
    g_clearRows.clear();
    return addBackOrWhole(partX);
}

// A wall the copy cannot get past may be the cut's rather than the level's, and a copy that is
// wrong only there would never clear to be verified. So after kWallCheckRounds rounds without
// getting deeper, the copy's deepest plan is flown on the level itself, once per wall, and
// compared with the copy's attempt of the same plan (onVerifyDeath): where the two part, the
// objects go back; if the level dies on the same tick the same way, the wall is real and the solve
// goes back to the copy. Called from the loop's death handler before the death is scored; the
// death that fires it is left unscored (the plan is flown again on the level).
inline constexpr int kWallCheckRounds = 8;
inline bool maybeCheckWall() {
    if (g_phase != Sliced || !g_orig) return false;
    // a piece is a prefix, not a cut of the level: no wall of it to compare on the level
    if (g_stage >= 0 && g_stage < (int)g_stageCuts.size()) return false;
    if (dpsolve::g_stallRuns < kWallCheckRounds) return false;
    if (dpsolve::g_best.empty() || dpsolve::g_bestDeath < 0) return false;
    if (dpsolve::g_bestDeath == g_checkedWall) return false;
    if (anchors::g_deepest.empty()) return false;
    g_checkedWall = dpsolve::g_bestDeath;
    ++g_wallChecks;
    g_clearRows = anchors::g_deepest;
    g_refEnd = dpsolve::g_bestDeath;
    g_refCleared = false;
    g_carry = dpsolve::g_best;
    g_verifyPlan = dpsolve::g_best;   // cfg slicefirstref flies it again on a fresh copy
    g_lightPending = true;   // a flight, not a new run: the loop stays the copy's
    g_phase = Verifying;
    char b[256];
    snprintf(b, sizeof(b), "slice: %d rounds at the copy's wall t=%lld - flying its deepest plan "
             "on the level itself, to see whether the wall is the level's or the cut's",
             dpsolve::g_stallRuns, dpsolve::g_bestDeath);
    writeResult(b);
    g_hudPhase = "checking the copy's wall on the level itself";
    swapTo(g_orig, g_cut.objects);
    return true;
}

// The loop gave up on the copy. That is the copy's verdict, not the level's: the solve goes on on
// the level itself, from the copy's deepest plan, with what the run has learnt.
//
// cfg `slicegiveupcopy` (on by default since 2026-10): ...unless the wall it gave up at is one the level was flown
// to and shares -- the same plan dying on the same tick the same way (onVerifyDeath). The copy is
// right there, so the level would only be the same wall at the level's price, and what the level
// brought was the full carry: the recordings, anchors and seeds taken again. A fresh copy gets
// the same carry. On a heavy custom level (147,423 objects, 14,482 in the copy) the section
// search's step cost 858-2,059 us on the level against 91-183 us on the copy, and the searches
// after the move to the level took 91% of that run's search time. Once per wall: a second give-up
// at the same wall goes to the level as before.
inline bool onGiveUp(const char* reason) {
    if (g_phase != Sliced || !g_orig) return false;
    if (g_cfg.sliceGiveUpCopy && g_ownWall >= 0 && dpsolve::g_bestDeath == g_ownWall
        && g_giveUpCopyAt != g_ownWall) {
        g_giveUpCopyAt = g_ownWall;
        char b[256];
        snprintf(b, sizeof(b), "slice: the solve gave up on the copy (%s) at t=%lld, a wall the "
                 "level shares - going on on a fresh copy from the copy's deepest plan", reason,
                 g_ownWall);
        writeResult(b);
        g_carry = dpsolve::g_best.empty() ? dpsolve::g_plan : dpsolve::g_best;
        g_carryPending = true;
        g_phase = Sliced;
        g_hudPhase = "solving on the copy again";
        GJGameLevel* copy = nullptr;
        const size_t kept = buildCopy(&copy);
        swapTo(copy, kept);
        return true;
    }
    if (g_stage >= 0) g_stage = (int)g_stageCuts.size();   // cfg stagecuts: the pieces end here
    writeResult(std::string("slice: the solve gave up on the copy (") + reason
                + ") - going on on the level itself from the copy's deepest plan");
    g_carry = dpsolve::g_best.empty() ? dpsolve::g_plan : dpsolve::g_best;
    g_carryPending = true;
    g_phase = Whole;
    g_hudPhase = "solving on the level itself";
    swapTo(g_orig, g_cut.objects);
    return true;
}

// What the slice holds allocated (capacities; the retained original level is counted as its string's
// size only), for the memory census (cfg memcensus).
inline size_t heldBytes() {
    size_t n = g_raw.capacity() + g_cut.head.capacity() + g_cut.body.capacity() * sizeof(std::string_view)
               + g_cut.objs.capacity() * sizeof(slice::StrObj) + g_cut.drop.capacity()
               + g_keepX.capacity() * sizeof(std::pair<double, double>) + g_carry.capacity() * sizeof(InputCmd)
               + (g_clearRows.capacity() + g_levelRows.capacity()) * sizeof(AnchorRow)
               + (g_levelUid.capacity() + g_copyOrd.capacity() + g_copyIds.capacity()) * sizeof(int)
               + g_verifyPlan.capacity() * sizeof(InputCmd);
    for (const auto& v : g_levelExtra) n += v.capacity() * sizeof(int);
    if (g_orig) n += g_orig->m_levelString.size();
    return n;
}

inline std::string summary() {
    if (g_phase == Off) return {};
    static const char* names[] = {"off", "sliced", "verifying", "whole", "ref-flight"};
    char b[280];
    snprintf(b, sizeof(b), "slice: ended %s - %zu of %zu objects in play, %d verification%s and "
             "%d wall check%s on the level itself, %d add-back%s", names[g_phase], g_keptNow,
             g_cut.objects, g_verifies, g_verifies == 1 ? "" : "s", g_wallChecks,
             g_wallChecks == 1 ? "" : "s", g_addBacks, g_addBacks == 1 ? "" : "s");
    std::string s = b;
    if (!g_stageCuts.empty())   // cfg stagecuts
        s += "; stages: " + std::to_string(g_stageHandOffs) + " of " + std::to_string(g_stageCuts.size())
             + " pieces handed on, " + std::to_string(g_stage) + " reached; hand-offs consistent "
             + std::to_string(g_stageSame) + ", parted at the piece's edge " + std::to_string(g_stageEdge)
             + ", parted before it " + std::to_string(g_stageApart);
    if (g_refFlights > 0)   // cfg slicefirstref
        s += "; " + std::to_string(g_refFlights) + " first-attempt flight"
             + (g_refFlights == 1 ? "" : "s") + " on a fresh copy";
    return s;
}

}  // namespace levelslice
}  // namespace p1
