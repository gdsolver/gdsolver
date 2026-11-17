// GJBaseGameLayer hook: tick control, fast loop, replay/serve, section solver, tracing.
#include "mod/playlayer_helpers.hpp"
#include "mod/profsample.hpp"
#include "mod/replaystep.hpp"
#include <Geode/modify/GameObject.hpp>   // cfg objwatch: ActWatchObject
// For PROCESS_MEMORY_COUNTERS's named fields -- the struct is only used through
// GetProcAddress("K32GetProcessMemoryInfo"), so nothing links psapi.
#ifdef GEODE_IS_WINDOWS
#include <psapi.h>
#endif

using namespace p1;

// cfg cpcarryparts: the carry gate asks whether the screen is off (cp_flight.hpp gateMask)
static const bool s_cpScreenOffSet = (cpflight::g_screenOff = &visibilityPassOff, true);

// The rest of the player at each entry snapshot, same index as g_snaps: what
// psnap's byte copy cannot carry -- the containers (the slope map, the touched rings and pads) and
// the pointers to the objects the player stands on, rides or dashes from -- carried by C++
// assignment, as the section search carries them.
struct SnapExtra {
    secsolve::DashState dash, dash2;
    psnap::Touch touch;
    // GD's anti-cheat check (PlayLayer::m_damageVerified): not in the checkpoint, and outside
    // practice mode resetLevel clears it, which puts GD's spike back on player 1 until a
    // destroyPlayer with it clears the check again -- and every substep the spike is hit,
    // checkCollisions returns before postCollision. Measured on lv16: 11 such calls after each
    // restore, so a ramp left within those substeps (t=6,404; 10,809) never launched the player.
    bool damageVerified = true;
};
static std::vector<SnapExtra> g_snapExtras;

// Time spent INSIDE the restore, summed across the search. `lms` on the seclayer
// line is the whole layer's elapsed time, so `lms/lrest` is "layer time per
// restore" and NOT the restore's cost -- reading it as the latter is what made
// the 3x ramp look like a restore problem. The bench says the restore itself is
// flat under every quantity it can vary (live checkpoints to 40,000 and 3.3 GB,
// cold objects cycled, array length, cumulative count: 4.3-5.1 ms throughout),
// so this counter is what settles whether the growth is inside the restore at all.
static double g_secRestMs = 0.0;
// ...and the same total split in two, because "the restore got slower" is not yet
// a place to look. A section restore is two GD calls: storeCheckpoint, which
// copies the state out of the node's CheckpointObject, and resetLevel, which
// rebuilds the level around it. The ramp has to sit in one of them, and which one
// halves the candidate list: the copy is a read of a cold ~70 KB object (locality
// of the node bank), the reset is GD's own O(level) work (effects, colours,
// sections, spawn objects). Both accumulate over a layer and are reported beside
// lrestms, whose remainder is this function's own tail.
static double g_secStoreMs = 0.0;
static double g_secResetMs = 0.0;
// ...and on the player-snapshot path, which the three above never see: the snapshot restore
// (player, effects, game state, touches, world) and the step after it. Printed only. These are
// what put lv4003's slow windows (t 9,860 against 8,820: 1.1 ms a restore + step against 97 us)
// in the STEP -- 724 us against 26, the restore 31 against 22 -- with a cost that climbed every
// layer and fell back at each checkpoint restore: the wave trail (see secsolve g_trailClear).
static double g_secSnapMs = 0.0;
static double g_secStepMs = 0.0;

// Empty the practice checkpoint list without leaving the checkpoints' physical objects in GD's
// section table. markCheckpoint registers each checkpoint's physical object in a section, and
// only GD's own removals (removeCheckpoint, storeCheckpoint's eviction past 50) take it out
// again; clearing the list directly skipped that, and once the mod let go of such a checkpoint
// the object was freed with its pointer still in the section. Every pass over the sections then
// called into freed memory: resetLevelVariables (the decompilation: a loop over +0x3598/+0x3640
// calling each object's vtable+0x478) and updateVisibility faulted in cocos2d's addChild with a
// null child, the visibility pass lost every frame, and a replay after a section solve drew
// nothing but the player (user report on the panel, 2026-09-25; reproduced in a worker with
// skiprender=0: the faults began with the reset after the first rung).
static void clearCheckpointsUnsectioned(PlayLayer* pl) {
    static_assert(offsetof(CheckpointObject, m_physicalCheckpointObject) == gdoff::kCheckpointObject,
                  "the object removeCheckpoint takes out of its section");
    if (!pl || !pl->m_checkpointArray) return;
    for (unsigned i = 0; i < pl->m_checkpointArray->count(); ++i) {
        auto* cp = static_cast<CheckpointObject*>(pl->m_checkpointArray->objectAtIndex(i));
        GameObject* o = cp ? cp->m_physicalCheckpointObject : nullptr;
        if (o && o->m_outerSectionIndex >= 0) {
            pl->removeObjectFromSection(o);
            o->m_outerSectionIndex = -1;   // out: a later removal of it is GD's own no-op
        }
        // ...and out of the layer, as GD's own removals do (removeCheckpoint 0x3b7f00 and
        // storeCheckpoint's eviction: the node at object+0x2f0 released and removed, then the
        // object itself). Left there, every checkpoint a section search made stayed a child of
        // one of the nodes resetLevel sorts on every restore (the CCArray at layer+0x1028):
        // 5,710 -> 41,726 children over one run of a custom level's slice, the restore 24 -> 72 us.
        if (o) {
            auto*& attached = *reinterpret_cast<cocos2d::CCNode**>(
                reinterpret_cast<char*>(o) + gdoff::kObjGlowSprite);
            if (attached) {
                attached->removeFromParentAndCleanup(true);
                attached->release();
                attached = nullptr;
            }
            o->removeFromParentAndCleanup(true);
        }
    }
    pl->m_checkpointArray->removeAllObjects();
}

// The process's working set, for putting the cost curve and the resident curve on
// the same axis. Resolved dynamically so nothing has to link psapi: measured from
// outside, 60,000 restores leave ~250 MB behind (~4 KB each), and the per-restore
// cost tracks that growth -- but "tracks" has to be shown on one series, not
// inferred from two runs.
// {current working set, peak working set} in MB, through psapi's NAMED fields.
// Two hand-rolled offset guesses got this wrong in both directions on 2026-09-02:
// the first printed PeakWorkingSetSize as "current", the second printed
// QuotaPeakPagedPoolUsage (a kernel pool quota, not physical memory) as "ws". The
// numbers mattered -- read as a peak, the 10 GB looked like a monotone artefact
// and was withdrawn; read correctly it is the LIVE working set during the loop,
// which is a completely different hypothesis (something held until the frame ends,
// not something transient). No third guess: the struct has names.
// The process's private bytes in MB (cfg seekcp's memory line); -1 where not measured.
static long long privateMB() {
#ifndef GEODE_IS_WINDOWS
    return -1;
#else
    using Fn = int(__stdcall*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!fn || !fn(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1;
    return (long long)(pmc.PagefileUsage / (1024 * 1024));
#endif
}

static void procMemMB(size_t& cur, size_t& peak) {
#ifndef GEODE_IS_WINDOWS
    cur = peak = 0;   // Windows only; 0 = not measured
#else
    using Fn = int(__stdcall*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                      "K32GetProcessMemoryInfo");
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    cur = peak = 0;
    if (fn && fn(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        cur = pmc.WorkingSetSize / (1024 * 1024);
        peak = pmc.PeakWorkingSetSize / (1024 * 1024);
    }
#endif
}

// NOT counting the autorelease pool directly: CCPoolManager::getCurReleasePool is
// private in this cocos and CCAutoreleasePool has no count(). The working set is
// the proxy -- the pool is drained at the END OF A FRAME, so a loop that never
// returns a frame keeps every autoreleased object it makes (candidate 1:
// ColorAction::create, 168 B,
// autoreleased, remade for every channel on every resetLevel), and that shows up
// as live memory. The decisive test is the drain control arm, which needs no count.

static size_t layerWsMB() {
    size_t cur = 0, peak = 0;
    procMemMB(cur, peak);
    return cur;
}

static int runningActionsOn(cocos2d::CCNode* t) {
    if (!t) return -1;
    auto* d = cocos2d::CCDirector::sharedDirector();
    if (!d || !d->getActionManager()) return -1;
    return d->getActionManager()->numberOfRunningActionsInTarget(t);
}

// GD's section vectors: how many objects the restore's SORT walks, and how big
// the pool behind them has grown. Read once per layer / per bench bucket.
//
// Why these two numbers and not a timer: PlayLayer::resetLevel is the ONLY caller
// of GJBaseGameLayer::sortSectionVector (win 2.2081, 0x226fa0, one call site at
// resetLevel+0xbd1), and that sort is the only part of a restore whose cost is
// O(objects). Everything else the reset does to the sections is two
// removeObjectFromSection calls -- on the two players, not in a loop.
//
// The layout, read off addToSection (0x226500) / removeObjectFromSection
// (0x226d60) / sortSectionVector itself:
//     +0x35b0  vector<vector<vector<GameObject*>*>*>   sections[i][j] -> leaf
//     +0x3658  vector<vector<int>*>                    counts  [i][j] = LIVE size
// and the same pair again at +0x35c8 / +0x3670 for the second family. A leaf is
// only ever push_back'd; removal swaps the last live element into the hole and
// decrements counts[i][j]. So the leaf's std::vector size is a HIGH-WATER MARK
// and counts[i][j] is the live number -- the sort's own bound
// (`movsxd rsi, [counts + j*4]`, then std::sort over rsi elements).
//
// That asymmetry is the whole point of measuring it. Add and remove are
// symmetric on every arm (both dispatch on obj+0x40c == 0x718, obj+0x3a0 == 7 and
// obj+0x280 the same way), so the maintenance CANNOT leak by itself: `live` can
// only grow if something adds objects to sections without removing them. If
// `live` is flat across a run, the restore ramp is not the section vectors and
// this closes hypothesis (A) as a negative; if it climbs, the ramp has its
// mechanism and the next question is who does the adding.
// Takes the base pointer, not PlayLayer*, so the upcast is the compiler's job:
// these offsets are GJBaseGameLayer's.
static void sectionCensus(GJBaseGameLayer* layer, size_t& live, size_t& phys) {
    live = phys = 0;
    if (!layer) return;
    const uint8_t* b = (const uint8_t*)layer;
    // std::vector<T> element count, for an 8-byte or 4-byte T.
    auto vecN = [](const uint8_t* v, size_t elem) -> size_t {
        if (!v) return 0;
        const uint8_t* beg = *(const uint8_t* const*)v;
        const uint8_t* end = *(const uint8_t* const*)(v + 8);
        if (!beg || !end || end < beg) return 0;
        return (size_t)(end - beg) / elem;
    };
    const size_t fam[2][2] = {
        {gdoff::kLayerNonEffectObjects, gdoff::kLayerNonEffectObjectsSizes},
        {gdoff::kLayerCollisionBlockSections, gdoff::kLayerCollisionBlockSectionSizes}};
    for (const auto& f : fam) {
        const uint8_t* secV = b + f[0];
        const uint8_t* cntV = b + f[1];
        const size_t nSec = std::min(vecN(secV, 8), vecN(cntV, 8));
        const uint8_t* const* secB = *(const uint8_t* const* const*)secV;
        const uint8_t* const* cntB = *(const uint8_t* const* const*)cntV;
        if (!secB || !cntB) continue;
        for (size_t i = 0; i < nSec; ++i) {
            const uint8_t* inner = secB[i];
            const uint8_t* ints = cntB[i];
            if (!inner || !ints) continue;
            const size_t nSub = std::min(vecN(inner, 8), vecN(ints, 4));
            const uint8_t* const* leafB = *(const uint8_t* const* const*)inner;
            const int* cv = *(const int* const*)ints;
            if (!leafB || !cv) continue;
            for (size_t j = 0; j < nSub; ++j) {
                if (!leafB[j]) continue;
                if (cv[j] > 0) live += (size_t)cv[j];
                phys += vecN(leafB[j], 8);
            }
        }
    }
}

struct SecRestTimer {
    std::chrono::steady_clock::time_point t0{std::chrono::steady_clock::now()};
    ~SecRestTimer() {
        g_secRestMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
};

// The srect:/drect: tick window. These queries run constantly, so a run that
// names no upper bound (hbto) keeps the lv22 t=1,813 window they were added for;
// with hbfrom/hbto set they follow it like the other hitboxtrace lines.
static bool spiderRectWindow() {
    if (g_cfg.hbTo != 0) return g_tick >= g_cfg.hbFrom && g_tick <= g_cfg.hbTo;
    return g_tick >= 1750 && g_tick <= 1900;
}

// ...and what the query returned (`shit:` / `dhit:`, uids only). Reading the
// objects' rects here would call getObjectRect, which recomputes a dirty rect
// and so changes what GD itself tests afterwards; the uids do not.
static void printQueryHits(const char* tag, cocos2d::CCArray* arr) {
    std::string s = std::string(tag) + ": t=" + std::to_string(g_tick)
                    + " n=" + std::to_string(arr ? arr->count() : 0);
    int k = 0;
    if (arr) {
        for (auto* o : CCArrayExt<GameObject*>(arr)) {
            if (!o) continue;
            if (k++ >= 24) { s += " ..."; break; }
            s += " " + std::to_string(o->m_uniqueID);
        }
    }
    writeResult(s);
}

// ---- The section hooks, on only where they can act ----
//
// updateObjectSection and removeObjectFromSection are hooked for the section search alone, yet GD
// calls updateObjectSection for every object a move trigger moves, every tick. On a level that
// moves tens of thousands of objects the hook's dispatch took about 80% of a screen-off replay
// (cfg profsample, 2026-10-05: a heavy custom level's run through 9.4 s with it, 1.8 s without). So, under
// cfg `sectionhooklazy` (on by default; 0 = always on, as before):
//   * updateObjectSection's body acts only while a search runs (secsolve::g_active), so the hook is
//     enabled exactly then -- every write of g_active goes through secsolve::setSearching, which
//     calls back here before the search's first update;
//   * removeObjectFromSection's body acts while secsolve::g_on, which only a session that can
//     start a section search raises (a solve, a served worker, a cfg-driven section run), so it
//     is enabled in those sessions. It is not hot.
namespace {
geode::Hook* g_updSecHook = nullptr;
geode::Hook* g_remSecHook = nullptr;
bool g_updSecOn = true, g_remSecOn = true;
long long g_secHookFlips = 0;
}
inline void setSectionHook(geode::Hook* h, bool& on, bool want, const char* name) {
    if (!h || on == want) return;
    if (auto r = want ? h->enable() : h->disable(); r.isErr()) {
        writeResult(std::string("sectionhooks: ") + name + (want ? " enable" : " disable")
                    + " FAILED: " + r.unwrapErr());
        return;
    }
    on = want;
    if (++g_secHookFlips <= 4)
        writeResult(std::string("sectionhooks: ") + name + (want ? " enabled" : " disabled")
                    + " (t=" + std::to_string((long long)g_tick) + ")");
}
inline void syncSectionHooks() {
    const bool lazy = itermap::g_sectionHookLazy;
    setSectionHook(g_updSecHook, g_updSecOn, !lazy || secsolve::g_active, "updateObjectSection");
    setSectionHook(g_remSecHook, g_remSecOn,
                   !lazy || (g_started && (g_cfg.dpSolve || g_serveMode || secsolve::g_on)),
                   "removeObjectFromSection");
}

// ---- cfg objwatch: who activates a watched object ----
//
// GameObject::activateObject (0x4a3b10) with a backtrace whenever a watched object (cfg objwatch) is
// activated -- the path by which a checkpoint restore activates objects a head run never had activated
// (SubZero 4002, uid 17999-18002 at t=20,404). Off unless objwatch is set: the hook is installed
// disabled and enabled at the first frame of a session that watches.
namespace {
geode::Hook* g_actHook = nullptr;
bool g_actHookOn = false;
}
class $modify(ActWatchObject, GameObject) {
    static void onModify(auto& self) {
        ::gdsolver::exeguard::onModify(self);
        if (auto h = self.getHook("GameObject::activateObject")) {
            g_actHook = h.unwrap();
            g_actHook->setAutoEnable(false);
        }
    }
    void activateObject() {
        if (!itermap::g_objWatch.empty()
            && std::find(itermap::g_objWatch.begin(), itermap::g_objWatch.end(), m_uniqueID)
                   != itermap::g_objWatch.end()) {
            auto* pl = PlayLayer::get();
            char b[220];
            snprintf(b, sizeof(b), "objwatch: activateObject uid=%d t=%lld player.x=%.2f restoring=%d run=%s%lld",
                     m_uniqueID, (long long)g_tick, pl && pl->m_player1 ? pl->m_player1->getPositionX() : -1.f,
                     (int)seekcp::g_restoring, seekcp::g_from < 0 ? "head" : "cp@",
                     seekcp::g_from < 0 ? 0LL : seekcp::g_from);
            writeResult(b);
#ifdef GEODE_IS_WINDOWS
            postmortem::writeBacktrace("objwatch-bt:");
            // (A raw stack scan was tried here; it read past the stack's end inside updateVisibility's
            // SEH guard and cut that pass short. The activation is updateVisibility's, as that showed.)
#endif
        }
        GameObject::activateObject();
    }
};
inline void syncActWatchHook() {
    const bool want = !itermap::g_objWatch.empty();
    if (!g_actHook || want == g_actHookOn) return;
    if (auto r = want ? g_actHook->enable() : g_actHook->disable(); r.isOk()) g_actHookOn = want;
}

// ---- Tracing: frame / step structure ----
class $modify(GJBaseGameLayer) {
    static void onModify(auto& self) {
        ::gdsolver::exeguard::onModify(self);
        if (auto h = self.getHook("GJBaseGameLayer::updateObjectSection")) g_updSecHook = h.unwrap();
        if (auto h = self.getHook("GJBaseGameLayer::removeObjectFromSection")) g_remSecHook = h.unwrap();
        secsolve::g_onActive = [] { syncSectionHooks(); };
        itermap::g_cpAhead = [](long long now, long long target) { return seekcp::aheadOf(now, target); };
        itermap::g_cpLine = [] { return seekcp::summary(); };
    }
    // The EXACT rect the spider's target search queries (cfg `hitboxtrace=1`,
    // `srect:`/`drect:` lines). spiderTestJumpInternal (win 0x3943f0) builds two
    // rects -- solids and damaging -- whose transverse anchoring in a rotated
    // frame defeated every closed-form fit (lv22 t=1,813: GD's candidate list
    // holds uid1204 at a 6px GAP from the player's box; the injection sweep
    // brackets the cutoff between centre distances 37.5 and 41.5). Printing the
    // rect the binary actually built is the instrument that replaces the
    // guessing; these hooks fire from every caller, so the tick stamp and the
    // hitboxtrace gate keep it to the probe runs that ask for it.
    cocos2d::CCArray* staticObjectsInRect(cocos2d::CCRect rect, bool enabledGroups) {
        // The tick window, not a line cap: this query runs constantly (4,000
        // lines burnt in the first second of the natural attempt), so a cap
        // never survives to the tick under study.
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && spiderRectWindow()) {
            {
                char b[192];
                snprintf(b, sizeof(b),
                         "srect: t=%lld o=(%.3f,%.3f) s=(%.3f,%.3f) eg=%d",
                         (long long)g_tick, rect.origin.x, rect.origin.y,
                         rect.size.width, rect.size.height, (int)enabledGroups);
                writeResult(b);
            }
            auto* r = GJBaseGameLayer::staticObjectsInRect(rect, enabledGroups);
            printQueryHits("shit", r);
            return r;
        }
        return GJBaseGameLayer::staticObjectsInRect(rect, enabledGroups);
    }
    cocos2d::CCArray* damagingObjectsInRect(cocos2d::CCRect rect, bool enabledGroups) {
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && spiderRectWindow()) {
            {
                char b[192];
                snprintf(b, sizeof(b),
                         "drect: t=%lld o=(%.3f,%.3f) s=(%.3f,%.3f) eg=%d",
                         (long long)g_tick, rect.origin.x, rect.origin.y,
                         rect.size.width, rect.size.height, (int)enabledGroups);
                writeResult(b);
            }
            auto* r = GJBaseGameLayer::damagingObjectsInRect(rect, enabledGroups);
            printQueryHits("dhit", r);
            return r;
        }
        return GJBaseGameLayer::damagingObjectsInRect(rect, enabledGroups);
    }
    // ---- WHO SAYS A COIN WAS COLLECTED (cfg `coins=1`) ----------------------
    //
    // Until now the only witness was the mod's own test: "the player's centre came
    // within 20 px of where the coin was when the level loaded" (the block near
    // g_coinPickupTick below, and COIN_RADIUS in solver.hpp). Its comment claims the
    // radius is conservative enough that a detection implies GD collected it too --
    // an assertion nothing had ever checked. A route planned to collect coins rests
    // entirely on that claim, so it has to become a measurement.
    //
    // GD's own statement is this call, and it was read out of the binary rather than
    // picked by name. collisionCheckObjects (win 0x214960) reaches a coin on two
    // branches and both end the same way:
    //     call hasUniqueCoin ; test al,al ; jne skip ; call pickupItem
    // at 0x215c39/0x215c48 and 0x215cef/0x215cfe. The hasUniqueCoin guard is what
    // makes this exactly-once per coin: the second contact skips the call. The
    // function whose NAME looks right, collectedObject, is `win inline` in the
    // 2.2081 bindings and cannot be hooked at all.
    //
    // OBSERVE ONLY -- the original always runs. It writes GD's in-level "already
    // collected" dictionary, so suppressing it would change the game instead of
    // measuring it, and it is not where a coin is earned: the awards still die at
    // storeSecretCoin / storeUserCoin / checkCoinAchievement (hooks_system.cpp), and
    // the session's `level record changed:` line is the proof that they did.
    void pickupItem(EffectGameObject* object) {
        if (object && g_started && !g_sessionOver && g_cfg.coinMode) {
            // Matched by uid, not by position: a coin under group control is not
            // where the level dump put it, which is the whole reason this hook
            // exists rather than a second coordinate test.
            const int uid = object->m_uniqueID;
            size_t i = 0;
            for (; i < solver::g_coins.size(); ++i)
                if (solver::g_coins[i].uid == uid) break;
            const bool known = i < solver::g_coins.size();
            if (known) {
                if (i < solver::g_coinGdTick.size() && solver::g_coinGdTick[i] < 0)
                    solver::g_coinGdTick[i] = g_tick;
            } else {
                ++solver::g_coinGdUnmatched;
            }
            if (++solver::g_coinLogLines <= 400) {
                auto p = object->getPosition();
                float px = 0.f, py = 0.f;
                if (m_player1) { px = m_player1->getPositionX(); py = m_player1->getPositionY(); }
                char b[224];
                snprintf(b, sizeof(b),
                         "coingd: t=%lld uid=%d id=%d obj=(%.2f,%.2f) player=(%.2f,%.2f) "
                         "coin=%d secretid=%d item=%d",
                         (long long)g_tick, uid, object->m_objectID, p.x, p.y, px, py,
                         known ? (int)i : -1, object->m_secretCoinID, object->m_itemID);
                writeResult(b);
            }
        }
        GJBaseGameLayer::pickupItem(object);
    }
    // GD's own item counters (cfg `coins=1`, `itemcnt:` line).
    //
    // Not needed to compare the two coin verdicts, but it is the same question one
    // level down and the same hook group: what makes lv21's third coin appear is
    // "ten of the id-1840 collectibles", and lv22's third is a collision count --
    // both are item counters, and this is the single function that writes them
    // (win 0x233ec0, five call sites). One line per change is the difference
    // between "the route missed the gate" and "the model cannot express it".
    // ...and the gate in FRONT of pickupItem. collisionCheckObjects asks this
    // first (`call hasUniqueCoin; test al,al; jne skip`), so a coin GD counts as
    // ALREADY TAKEN is never credited however exactly the player passes through
    // it -- which from outside looks exactly like a coin that cannot be
    // collected at all. Measured on lv21 (2026-09-20): the player was put
    // through the third coin's centre (|dx| 1, |dy| 5) with the coin's own group
    // enabled, and nothing was credited.
    //
    // ...WHICH MAKES A COIN SESSION DEPEND ON THE PLAYER'S SAVE. On an install whose save already
    // holds a level's coins, every one of them answers "taken", nothing is credited, and a coin
    // solve can only ever end at the missed-coin request: measured on lv1 (2026-09-22), all three
    // coins -> 1, 79 attempts ended at the first one with the player passing 16 px from its
    // centre, where the same plan on an empty save is credited. So while the mod is driving a
    // coin session, the level's own coins (solver::g_coins) are asked as if the save held none --
    // GD then collects them as on a new save. Nothing reaches the save for it: coin storage and
    // the level's record are blocked for the whole of a driven session (hooks_system.cpp).
    bool hasUniqueCoin(EffectGameObject* object) {
        const bool r = GJBaseGameLayer::hasUniqueCoin(object);
        bool ours = false;
        if (object && r && g_started && !g_sessionOver && g_cfg.coinMode && botDriving())
            for (const auto& c : solver::g_coins)
                if (c.uid == object->m_uniqueID) { ours = true; break; }
        if (object && g_started && !g_sessionOver && g_cfg.coinMode
            && ++solver::g_coinLogLines <= 400) {
            char ub[192];
            snprintf(ub, sizeof(ub), "coinunique: t=%lld uid=%d id=%d -> %d%s",
                     (long long)g_tick, object->m_uniqueID, object->m_objectID,
                     r ? 1 : 0, ours ? " (in the save; asked as 0 for this session)" : "");
            writeResult(ub);
        }
        return ours ? false : r;
    }
    void updateCounters(int itemId, int value) {
        // The value itself, for the anchor to carry (solver::g_itemCounts). The
        // line below is capped at 400 for the log's sake; the table is not --
        // a counter that stopped being recorded at the cap would tell a
        // re-anchored search the wrong number.
        if (g_started && !g_sessionOver && (g_cfg.coinRoute || g_cfg.worldGates))
            solver::g_itemCounts[itemId] = value;
        // (...and under cfg worldgates: the counters are what the gates read, and this line is the
        // game's side of a gate-by-gate comparison with the model's countgate lines.)
        if (g_started && !g_sessionOver && (g_cfg.coinMode || g_cfg.worldGates)
            && ++solver::g_coinLogLines <= 400) {
            char b[128];
            snprintf(b, sizeof(b), "itemcnt: t=%lld item=%d v=%d",
                     (long long)g_tick, itemId, value);
            writeResult(b);
        }
        GJBaseGameLayer::updateCounters(itemId, value);
    }
    // When and by which object the rotate trigger fired (cfg `hitboxtrace=1`, `rot:` line).
    //
    // The model's crossing test runs in the progress coordinate of the rotation frame,
    // and in frame 3 that becomes "the trigger's world Y", so uid5957 (8297,423),
    // 1,500px away in world space, misfires at u=423 (lv22 t=4,688).
    // Replacing it with "cross in world X" broke the first section, so
    // do not guess the test axis -- ask GD. Once this line appears, the tick and the
    // partner object are settled.
    void rotateGameplay(RotateGameplayGameObject* object) {
        // Absolute angle. Same convention as the model's nf (lround(rot/90) & 3).
        if (object)
            g_gameFrame = ((int)std::lround(object->getRotation() / 90.0)) & 3;
        ++g_rotGameplayCalls;
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver) {
            static int lines = 0;
            if (++lines <= 200) {
                auto p = object ? object->getPosition() : cocos2d::CCPoint(0, 0);
                // Also emit properties that objrects does not export. Some objects with
                // the same orot=0 reverse and some do not, yet the exported rows were
                // completely identical and could not be told apart (lv22 uid 16659 and
                // 1215). The distinguishing bit must be in one of these four.
                char b[256];
                snprintf(b, sizeof(b),
                         "rot: t=%lld uid=%d id=%d obj=(%.1f,%.1f) orot=%.1f "
                         "px=%.3f py=%.3f isRev=%d endRev=%d mvDir=%d gndDir=%d",
                         (long long)g_tick,
                         object ? object->m_uniqueID : -1,
                         object ? object->m_objectID : -1,
                         p.x, p.y, object ? object->getRotation() : 0.f,
                         m_player1 ? m_player1->getPositionX() : 0.f,
                         m_player1 ? m_player1->getPositionY() : 0.f,
                         object ? (int)object->m_isReverse : -1,
                         object ? (int)object->m_endReversed : -1,
                         object ? (int)object->m_moveDirection : -1,
                         object ? (int)object->m_groundDirection : -1);
                writeResult(b);
            }
        }
        GJBaseGameLayer::rotateGameplay(object);
    }

    // When and by which object the Options trigger (id 2899) fired
    // (cfg `hitboxtrace=1`, `opt:` line).
    //
    // Why it is needed: in lv22, m_controlsDisabled is set in 4 sections, and buttons
    // are ignored entirely while it is. Trying to reverse-engineer the firing rule from
    // the dump's transitions failed because the same x is crossed repeatedly (reverse
    // travel), so it could not be decided whether it is one-shot or per-crossing, or
    // whether height matters. Having GD name the partner uid settles it in one shot.
    void processOptionsTrigger(GameOptionsTrigger* object) {
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver) {
            static int lines = 0;
            if (++lines <= 200) {
                auto p = object ? object->getPosition() : cocos2d::CCPoint(0, 0);
                char b[256];
                snprintf(b, sizeof(b),
                         "opt: t=%lld uid=%d id=%d obj=(%.1f,%.1f) "
                         "px=%.3f py=%.3f p1=%d p2=%d touch=%d spawn=%d "
                         "multi=%d was=%d",
                         (long long)g_tick,
                         object ? object->m_uniqueID : -1,
                         object ? object->m_objectID : -1,
                         p.x, p.y,
                         m_player1 ? m_player1->getPositionX() : 0.f,
                         m_player1 ? m_player1->getPositionY() : 0.f,
                         object ? (int)object->m_disableP1Controls : -9,
                         object ? (int)object->m_disableP2Controls : -9,
                         object ? (int)object->m_isTouchTriggered : -1,
                         object ? (int)object->m_isSpawnTriggered : -1,
                         object ? (int)object->m_isMultiTriggered : -1,
                         m_player1 ? (int)m_player1->m_controlsDisabled : -1);
                writeResult(b);
            }
        }
        GJBaseGameLayer::processOptionsTrigger(object);
    }

    // When, by whom, and in which direction a group toggle happened
    // (cfg `hitboxtrace=1`, `togl:` line).
    //
    // Why it is needed: lv22's teleport exit cluster (group 309: ship portal uid 6440
    // etc.) is controlled by two Toggle (id 1049) triggers; GD did not fire it even
    // though the player sat inside the portal rect for 13 ticks, and fired it on the
    // tick after the x=15,765 crossing (t=14,226). There are 3 crossings
    // (forward -> reverse -> forward), and "which crossing counted" is the same unsolved
    // gate as 2899 -- do not guess; have GD name it together with triggerID (the uid
    // of the firing object).
    void toggleGroupTriggered(int group, bool activate,
                              gd::vector<int> const& remapKeys,
                              int triggerID, int controlID) {
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver) {
            static int lines = 0;
            if (++lines <= 2000) {
                char b[192];
                snprintf(b, sizeof(b),
                         "togl: t=%lld g=%d on=%d trig=%d ctl=%d px=%.3f",
                         (long long)g_tick, group, activate ? 1 : 0,
                         triggerID, controlID,
                         m_player1 ? m_player1->getPositionX() : 0.f);
                writeResult(b);
            }
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys,
                                              triggerID, controlID);
    }

    // Likewise, the direct call of the group toggle (`toglD:` line).
    // Callers that do not go through toggleGroupTriggered (collision blocks etc.) show up
    // only here -- lv22's group 309 never appeared on the Triggered side, so both are
    // hooked to pin down the real activation path.
    void toggleGroup(int id, bool activate) {
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver) {
            static int lines = 0;
            if (++lines <= 2000) {
                char b[128];
                snprintf(b, sizeof(b), "toglD: t=%lld g=%d on=%d px=%.3f",
                         (long long)g_tick, id, activate ? 1 : 0,
                         m_player1 ? m_player1->getPositionX() : 0.f);
                writeResult(b);
            }
        }
        GJBaseGameLayer::toggleGroup(id, activate);
    }

    // Watch the collision candidate list (cfg `watchuid=N` + hbfrom/hbto, `ccl:` line).
    // Answers in one bit the last remaining suspect for "the test formula is right but it
    // does not fire" = "it is not in the list in the first place". has = whether watchuid
    // is in the list.
    void collisionCheckObjects(PlayerObject* player,
                               gd::vector<GameObject*>* objects,
                               int objectCount, float dt) {
        if (g_cfg.watchUid > 0 && g_started && !g_sessionOver && objects
            && player == m_player1
            && g_tick >= g_cfg.hbFrom
            && (g_cfg.hbTo == 0 || g_tick <= g_cfg.hbTo)) {
            static int lines = 0;
            if (++lines <= 2000) {
                int has = 0;
                GameObject* found = nullptr;
                const int n = (int)objects->size();
                const int lim = objectCount < n ? objectCount : n;
                for (int i = 0; i < lim; ++i) {
                    GameObject* o = (*objects)[i];
                    if (o && o->m_uniqueID == g_cfg.watchUid) {
                        has = 1; found = o; break;
                    }
                }
                const auto prct = player->getObjectRect();
                // ...and, when it IS in the list, the object's own rect and the
                // state a refusal could come from. `has=1` alone cannot tell
                // "the box is not what I think" from "the box matches and a
                // latch is up": measured on lv22 uid 13833, GD carries it as a
                // candidate for 34 ticks at up to the full 25px overlap with
                // the polarity opposite to the portal's, and never fires. The
                // corpus already refutes a narrower box (GD fires elsewhere at
                // 0.045px) and a coordinate-convention shift (~1.6px cannot
                // explain 25px), so what is left is state -- and this line is
                // where to read it.
                // m_hasBeenActivated / ...ByPlayer are the per-object latches
                // the orbs' "once per run" also uses; the byte at +0x740 is the
                // claim bit already read on the ring path (see orbcall).
                char ob[192] = "";
                if (found) {
                    const auto orct = found->getObjectRect();
                    snprintf(ob, sizeof(ob),
                             " orect=(%.3f,%.3f,%.3f,%.3f) oid=%d otype=%d"
                             " act=%d actP=%d grpOff=%d grpOffT=%d claim=%d",
                             orct.origin.x, orct.origin.y,
                             orct.size.width, orct.size.height,
                             found->m_objectID, (int)found->getType(),
                             found->hasBeenActivated() ? 1 : 0,
                             found->hasBeenActivatedByPlayer(player) ? 1 : 0,
                             (int)found->m_isGroupDisabled,
                             (int)found->m_isGroupDisabledTemp,
                             (int)*((unsigned char*)found + gdoff::kRingClaimTouch));
                }
                char b[448];
                snprintf(b, sizeof(b),
                         "ccl: t=%lld n=%d has=%d "
                         "prect=(%.3f,%.3f,%.3f,%.3f)%s",
                         (long long)g_tick, lim, has,
                         prct.origin.x, prct.origin.y,
                         prct.size.width, prct.size.height, ob);
                writeResult(b);
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
    }

    // When and by whom gravity was flipped (cfg `hitboxtrace=1`, `fg:` line).
    //
    // Why it is needed: at lv22 t=2,749 GD set upsideDown 0->1, and the model misread
    // that as "hit the ceiling and stopped". But no causing object could be found -- the
    // level has no id 2066 (the 2.2 gravity trigger), and `m_gravityValue` is the
    // default 1.0 on every EffectGameObject, so triggers.txt cannot tell. Watching the
    // moment of the call is the reliable way (the sign of the player's rotation also
    // could not be closed by sweep + static reading, and the hook settled it at once).
    void flipGravity(PlayerObject* p, bool flip, bool noEffects) {
        const bool watch = g_cfg.hitboxTrace && g_started && !g_sessionOver
                           && p && p == m_player1;
        const int before = watch ? (int)p->m_isUpsideDown : 0;
        GJBaseGameLayer::flipGravity(p, flip, noEffects);
        if (!watch) return;
        static int lines = 0;
        if (++lines > 2000) return;
        char b[192];
        snprintf(b, sizeof(b),
                 "fg: t=%lld flip=%d noFx=%d up %d->%d x=%.3f y=%.3f vy=%.3f",
                 (long long)g_tick, flip ? 1 : 0, noEffects ? 1 : 0,
                 before, (int)p->m_isUpsideDown,
                 p->getPositionX(), p->getPositionY(), p->m_yVelocity);
        writeResult(b);
    }

    void update(float dt) {
        // Re-entrancy guard. GD's resetLevel calls update internally, so every hook
        // re-entry stacks another fast loop and the stack runs out (0xC00000FD). Nested
        // calls pass through; session bookkeeping, the fast loop and hotkeys belong to
        // the outermost call only
        if (hookdepth::g_slots[hookdepth::BGL_UPDATE].cur > 0) {
            ++g_reentrantUpdates;
            GJBaseGameLayer::update(dt);
            return;
        }
        hookdepth::Guard hg(hookdepth::BGL_UPDATE);
        stallwatch::Mark sm(stallwatch::UPDATE);
        // A prelude attempt's `until` (plan_io.hpp) restarts the level here at a frame boundary (the fast loop does
        // the same between its updates).
        if (g_preludeResetPending && g_preludePendingAttempt != g_attempt) g_preludeResetPending = false;
        if (g_preludeResetPending && g_started && !g_sessionOver) {
            g_preludeResetPending = false;
            ++g_preludeResets;
            ++g_finishedAttempts;   // the prelude attempt ended by its `until`
            if (auto* pl = PlayLayer::get()) pl->resetLevel();
        }
        if (secsolve::inFlight() && secsolve::g_sliceEnded)
            secsolve::g_outsideMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - secsolve::g_sliceEnd).count();
        ++g_frame;
        // The discriminating experiment for hole 2's two-tick gap.
        // resetLevel sets m_resumeTimer unconditionally, and the question is
        // whether the updates it covers still run substeps. Printing the timer
        // beside the number of processCommands calls each update actually made
        // separates the candidates in ONE run:
        //   2 / 1(+0) / 0(+0) / 0(+1)   the updates are frozen -- ticks are not
        //                               spent, and the compensation belongs on
        //                               the side that counts steps itself
        //   +1 from the first line      they are not frozen, and the two ticks
        //                               go somewhere else (the input queue
        //                               release, or a split step)
        // An earlier attempt to settle this by reading `dt` inside
        // processCommands proved nothing: that dt is the caller's value and
        // says nothing about whether the update was frozen.
        if (g_restoreProbe > 0) {
            --g_restoreProbe;
            const long long before = g_pcCalls;
            const int timerBefore = m_resumeTimer;
            GJBaseGameLayer::update(dt);
            char pb[192];
            snprintf(pb, sizeof(pb),
                     "restoreprobe: resumeTimer %d->%d  pcCalls=+%lld  "
                     "tick=%lld dt=%.6f",
                     timerBefore, m_resumeTimer, g_pcCalls - before,
                     (long long)g_tick, dt);
            writeResult(pb);
            return;
        }
        // Verification C: fake the frame dt. This dt is the only path by which the physics
        // learns the frame rate.
        if (g_started && g_cfg.framedt > 0) dt = g_cfg.framedt;
        // Slow motion (F11 / cfg slowmo). The substep size stays 1/240; only "how many
        // substeps per frame" shrinks. Trajectory and tick numbers are unchanged
        if (g_slowmo > 1.f) dt /= g_slowmo;
        // Only the slow side of spectating speed (0.25x/0.5x) is made by dividing dt here.
        // The fast side is made by "update several times with the same dt" (loops below)
        // -- doubling dt would shift the position of processing tied to frame boundaries.
        // Either way the 1/240 step is unchanged
        if (g_watchSpeed < 1.f && !(g_cfg.fastdt > 0 && !g_realtimeOverride))
            dt *= g_watchSpeed;
        // The command file belongs to the driver and is only read while the session is live.
        // The KEYS outlive the session on purpose: clearing the level ends it, and F7 (replay
        // again) / F8 (rendering) / F9 (leave) are exactly the keys a person reaches for at
        // that moment
        if (g_started) {
            if (!g_sessionOver) pollCommandFile();
            pollHotkeys();
        }
        // `swaplevel <path>`: off the update stack, or the scene replace would delete the
        // layer this call is inside. One request at a time -- the global is cleared here,
        // and the swap itself reports what it cost (level_entry swapLevelNow).
        if (!g_swapReq.empty()) {
            const std::string path = g_swapReq;
            g_swapReq.clear();
            geode::Loader::get()->queueInMainThread([path] { swapLevelNow(path); });
        }
        // Show the role in the window title (prevents mixing up a human's GD with a
        // worker). SetWindowText is only called when the content changes, so calling it
        // every frame is fine
        {
            static int s_titleTick = 0;
            if ((++s_titleTick & 63) == 0) updateWindowTitle();
        }
        // Simulated frame drops (cfg `lagms`). For reproducing a heavy environment.
        // With `lagat` it becomes "stall once, for a long time, at that tick"
        if (g_lagMs > 0 && g_started && !g_sessionOver) {
            if (g_lagAtTick < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(g_lagMs));
            } else if (!g_lagFired && g_tick >= g_lagAtTick) {
                g_lagFired = true;
                writeResult("lag: stalling " + std::to_string(g_lagMs)
                    + "ms at tick=" + std::to_string((long long)g_tick));
                std::this_thread::sleep_for(std::chrono::milliseconds(g_lagMs));
            }
        }
        // Always record a frame whose real time was too long (under syncprobe).
        // Sampling every 0.5 s alone misses the frame that caused it
        if (g_syncProbe && g_started && !g_sessionOver) {
            static std::chrono::steady_clock::time_point s_prevFrame{};
            auto nowFrame = std::chrono::steady_clock::now();
            if (s_prevFrame.time_since_epoch().count() != 0) {
                double frameMs = std::chrono::duration<double, std::milli>(
                    nowFrame - s_prevFrame).count();
                if (frameMs > 100.0) {
                    double musicSec = 0.0;
                    if (auto* fe = FMODAudioEngine::sharedEngine())
                        musicSec = fe->getMusicTimeMS(0) / 1000.0;
                    char hb[192];
                    snprintf(hb, sizeof(hb),
                        "hitch: frame=%.0fms tick=%lld game=%.2f music=%.2f d=%.2f",
                        frameMs, (long long)g_tick, (double)g_tick / 240.0, musicSec,
                        musicSec - (double)g_tick / 240.0);
                    writeResult(hb);
                }
            }
            s_prevFrame = nowFrame;
        }
        // Measure the drift between music and game time (cfg `syncprobe=1`). To tell who
        // drifted, line up real time / game time (tick/240) / GD's own attemptTime / the
        // FMOD music position
        if (g_syncProbe && g_started && !g_sessionOver) {
            static std::chrono::steady_clock::time_point s_lastSync{};
            auto nowSync = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(nowSync - s_lastSync).count() >= 0.5) {
                s_lastSync = nowSync;
                auto* pl = PlayLayer::get();
                double musicSec = 0.0;
                if (auto* fe = FMODAudioEngine::sharedEngine())
                    musicSec = fe->getMusicTimeMS(0) / 1000.0;
                double gameSec = (double)g_tick / 240.0;
                // timeForPos returns, from the level's speed layout, "the time at which
                // this position should be reached" (GD uses it for music/trigger sync).
                // Difference from the real game time = fast/slow relative to the chart
                double wantSec = -1.0;
                if (m_player1) wantSec = (double)this->timeForPos(
                    m_player1->getPosition(), 0, 0, true, 0);
                char sb[352];
                snprintf(sb, sizeof(sb),
                    "sync: real=%.2f game=%.2f attempt=%.2f music=%.2f "
                    "d(music-game)=%.2f want=%.2f d(game-want)=%.2f "
                    "spd=%.2f tick=%lld x=%.0f locked=%d endAnim=%d",
                    std::chrono::duration<double>(nowSync - solver::g_solveStart).count(),
                    gameSec, pl ? pl->m_attemptTime : -1.0, musicSec, musicSec - gameSec,
                    wantSec, gameSec - wantSec,
                    m_player1 ? (float)m_player1->m_playerSpeed : -1.f,
                    (long long)g_tick,
                    m_player1 ? m_player1->getPositionX() : -1.f,
                    m_player1 ? (int)m_player1->m_isLocked : -1,
                    pl ? (int)pl->m_levelEndAnimationStarted : -1);
                writeResult(sb);
            }
        }
        // Rollback verification (method B): run practice/checkpoint/restore at a frame
        // boundary
        if (g_started && !g_sessionOver) {
            auto* pl = PlayLayer::get();
            if (pl) {
                if (g_cfg.practiceAt >= 0 && !g_practiceOn && g_tick >= g_cfg.practiceAt) {
                    g_practiceOn = true;
                    pl->togglePracticeMode(true);
                    ev("PRACTICE_on");
                    writeResult("practice_on: tick=" + std::to_string(g_tick));
                }
                // A checkpoint at EVERY tick in `snapat`,
                // taken during one replay of a verified solution so that a
                // section run can start at any window's entry without
                // replaying the level again.
                //
                // markCheckpoint places on its own schedule and can land LATE
                // (measured: six attempts aimed at three ticks landed 1 to 17
                // ticks after), so the tick it actually landed on is recorded
                // beside the tick that was asked for. A section whose entry
                // moved is still usable -- it just starts where the checkpoint
                // is, and the consumer needs to know that rather than assume.
                if (g_nextSnap < g_cfg.snapAt.size()
                    && g_tick >= g_cfg.snapAt[g_nextSnap]) {
                    const int asked = g_cfg.snapAt[g_nextSnap];
                    while (g_nextSnap < g_cfg.snapAt.size()
                           && g_cfg.snapAt[g_nextSnap] <= g_tick) ++g_nextSnap;
                    // Taken before the checkpoint, as the section search takes its head's.
                    std::vector<uint8_t> snapPlayer;
                    SnapExtra extra;
                    if (m_player1) {
                        psnap::captureRaw(m_player1, this, snapPlayer);
                        secCaptureDash(extra.dash);
                        if (m_player2) secCaptureDash(extra.dash2, m_player2);
                        psnap::captureTouch(m_player1, extra.touch, this);
                        extra.damageVerified = pl->m_damageVerified;
                    }
                    if (auto* cp = pl->markCheckpoint()) {
                        cp->retain();
                        int snapHeld = 0;
                        if (m_player1) {
                            auto hb = m_player1->m_holdingButtons.find(1);
                            snapHeld = (hb != m_player1->m_holdingButtons.end()
                                        && hb->second) ? 1 : 0;
                        }
                        g_snaps.push_back({(long long)g_tick, cp,
                                           g_nextInput, g_nextToggle,
                                           snapHeld, {}, std::move(snapPlayer)});
                        g_snapExtras.push_back(std::move(extra));
                        char sb[192];
                        snprintf(sb, sizeof(sb),
                                 "snap: asked=%d at=%lld x=%.3f y=%.3f "
                                 "in=%zu tog=%zu",
                                 asked, (long long)g_tick,
                                 m_player1 ? m_player1->getPositionX() : -1.0,
                                 m_player1 ? m_player1->getPositionY() : -1.0,
                                 g_nextInput, g_nextToggle);
                        writeResult(sb);
                    } else {
                        writeResult("snap: asked=" + std::to_string(asked)
                                    + " FAILED (markCheckpoint returned 0)");
                    }
                }
                if (g_cfg.checkpointAt >= 0 && !g_ckpt && g_tick >= g_cfg.checkpointAt) {
                    // vytest writes BEFORE markCheckpoint, or the checkpoint has
                    // already copied the real velocity and the test measures
                    // nothing (first attempt: whatever was injected, the restore
                    // came back with the run's own -4.026 every time).
                    if (g_cfg.vyTestOn && m_player1)
                        m_player1->m_yVelocity = g_cfg.vyTest;
                    // The player as the plain flight brought it here, for the section search's
                    // head (secsolve::g_cpPlayer): the checkpoint does not carry all of it.
                    if (m_player1) psnap::captureRaw(m_player1, this, secsolve::g_headPlain);
                    // ...and each body's m_jumpBuffered, which a held restore raises (secjbkeep)
                    secsolve::g_headJb[0] = m_player1 ? (int8_t)(m_player1->m_jumpBuffered ? 1 : 0)
                                                      : (int8_t)-1;
                    secsolve::g_headJb[1] = (m_player2 && m_player2 != m_player1)
                                                ? (int8_t)(m_player2->m_jumpBuffered ? 1 : 0)
                                                : (int8_t)-1;
                    g_ckpt = markCheckpointAtPhys(pl);
                    if (g_ckpt) {
                        g_ckpt->retain();
                        g_ckptTick = g_tick;
                        g_headHeld = 0;
                        if (m_player1) {
                            auto it = m_player1->m_holdingButtons.find(1);
                            g_headHeld = (it != m_player1->m_holdingButtons.end()
                                          && it->second) ? 1 : 0;
                        }
                        ev("CHECKPOINT_made", (double)g_ckptTick);
                        // Also emit x/y: without matching against the post-restore position
                        // we would miss that "the point where the checkpoint was made" and
                        // "the point the restore returns to" differ
                        // The out-of-bounds latch as it stood when the
                        // checkpoint was taken. This is the value a replay from
                        // the head would hold here, so it is what a restore has
                        // to put back -- CheckpointObject does not carry it.
                        //
                        // `oobtest=1` forces it to 1 first. Without that the
                        // save/restore path cannot be exercised at all: the
                        // latch is only set by a substep that is genuinely out
                        // of bounds, and on a plain replay the band CLAMPS
                        // rather than letting the player leave it (measured --
                        // a ship held into the ceiling parks at the band edge
                        // with the latch still 0, as does a cube with the floor
                        // removed, because the world floor holds it). The state
                        // is reachable in the section solver, where a leaf's
                        // last substep can be out of bounds, and not easily
                        // anywhere else.
                        if (g_cfg.oobTest && m_player1)
                            reinterpret_cast<char*>(m_player1)[kOobLatchOff] = 1;
                        secCaptureDash(secsolve::g_ckptDash);
                        // ...and player 2's, for a section search's head (cfg secheaddash)
                        if (m_player2 && m_player2 != m_player1)
                            secCaptureDash(secsolve::g_ckptDash2, m_player2);
                        else
                            secsolve::g_ckptDash2 = secsolve::DashState{};
                        // Where the plan's input stream had got to. A restore
                        // rewinds the game to this tick but g_nextInput is a
                        // forward-only cursor, so without putting it back the
                        // restored run is fed the inputs for the tick it was
                        // AT when the restore happened -- a hundred ticks of
                        // plan applied to a state that is a hundred ticks
                        // earlier. That is not a restore, and any fidelity
                        // measured through it is measuring the mismatch.
                        g_ckptNextInput = g_nextInput;
                        g_ckptNextToggle = g_nextToggle;
                        // vy at full precision (hole 3). The
                        // restore is read as re-rounding the y velocity to
                        // "integer part + round(fraction*1000)/1000", which
                        // would destroy the HALF-grid values that flipGravity's
                        // x0.5 and a ball tap's x0.6 produce. dump.csv prints
                        // three decimals and cannot show a 0.0005 difference,
                        // so the two values are printed here instead and the
                        // test is one run: what the checkpoint held against
                        // what the restore left.
                        g_ckptVy = m_player1 ? m_player1->m_yVelocity : 0.0;
                        g_ckptVyRel = m_player1 ? m_player1->m_yVelocityRelated : 0.0;
                        g_ckptOobLatch = m_player1
                            ? (unsigned char)reinterpret_cast<const char*>(m_player1)[kOobLatchOff]
                            : 0;
                        char cb[192];
                        snprintf(cb, sizeof(cb),
                                 "checkpoint: tick=%lld x=%.3f y=%.3f oobLatch=%d",
                                 (long long)g_ckptTick,
                                 m_player1 ? m_player1->getPositionX() : -1.0,
                                 m_player1 ? m_player1->getPositionY() : -1.0,
                                 (int)g_ckptOobLatch);
                        writeResult(cb);
                    } else {
                        // markCheckpoint (0x3b7570) returns null only for a dead player
                        // (m_isDead, +0x9c0), so say whether that is the case here.
                        const bool dead = m_player1 && m_player1->m_isDead;
                        writeResult("checkpoint FAILED at tick=" + std::to_string(g_tick)
                                    + " (player dead=" + std::to_string(dead ? 1 : 0) + ")");
                        dpsolve::secRungPrefixFailed(
                            g_tick, m_player1 ? m_player1->getPositionX() : 0.f,
                            "could not take the checkpoint");
                    }
                }
                // Feasibility of the section-limited GD solver: run only restores N times
                // and measure the real cost per restore (cfg `restoreloop=N`). This is not
                // the search itself, so a broken state does not matter -- the session is
                // closed right after.
                if (g_cfg.restoreLoop > 0 && g_ckpt && !g_restoreLoopDone) {
                    g_restoreLoopDone = true;
                    const auto rt0 = std::chrono::steady_clock::now();
                    // **The array is the third quantity, and the average hides it.**
                    // This loop used to clear m_checkpointArray every iteration, which
                    // pins its length at 1 -- so the bench could not see a cost that
                    // grows with the array however long it ran, and 2026-09-02's sweep
                    // (restores flat over 120x, depth +9%) was measuring with that
                    // quantity held constant by construction. `restoreloopkeep=1` lets
                    // it grow the way the section search grows it (one checkpoint per
                    // kept node), and the series says what the mean cannot.
                    // **Hold N live CheckpointObjects first, then time.** The
                    // search keeps one per node it retains, so hundreds are alive
                    // at once, while this bench used to have exactly ONE -- and
                    // the bench is flat where the search ramps (2026-09-02: 120x
                    // in restores, array 1->50, depth +9%/4000 ticks, all flat;
                    // the same window's search ramps 1.59x by layer 352). Live
                    // count is the quantity that was never varied.
                    // Made BEFORE the timing loop and never restored from, so
                    // this varies how many are alive and nothing else -- mixing
                    // creation into the loop would put the cost of making them
                    // back into the number being measured.
                    static std::vector<CheckpointObject*> s_held;
                    if (g_cfg.restoreLoopHold > 0) {
                        const auto ht0 = std::chrono::steady_clock::now();
                        for (int i = 0; i < g_cfg.restoreLoopHold; ++i)
                            if (auto* hc = pl->markCheckpoint()) {
                                hc->retain();
                                s_held.push_back(hc);
                            }
                        char hb2[160];
                        snprintf(hb2, sizeof(hb2),
                                 "restoreloop_hold: live=%zu madeMs=%.0f",
                                 s_held.size(),
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - ht0).count());
                        writeResult(hb2);
                    }
                    // CONTROL ARM 3: run the bench with the heap the SEARCH ends
                    // up with, and no cumulative restores at all. 4 MB blocks
                    // rather than one slab, and every page written, because an
                    // untouched reservation costs nothing and would test nothing
                    // -- what is being reproduced is a process whose live pages
                    // are spread over hundreds of MB, not its address space.
                    // Written with a per-block varying byte so a compiler cannot
                    // decide the stores are dead.
                    static std::vector<char*> s_heap;
                    if (g_cfg.restoreLoopHeapMB > 0 && s_heap.empty()) {
                        const size_t blk = 4u << 20;
                        const auto gt0 = std::chrono::steady_clock::now();
                        for (int mb = 0; mb < g_cfg.restoreLoopHeapMB; mb += 4) {
                            char* p = (char*)malloc(blk);
                            if (!p) break;
                            for (size_t o = 0; o < blk; o += 4096)
                                p[o] = (char)(mb + 1);
                            s_heap.push_back(p);
                        }
                        size_t hc = 0, hp = 0;
                        procMemMB(hc, hp);
                        char gb[160];
                        snprintf(gb, sizeof(gb),
                                 "restoreloop_heap: blocks=%zu MB=%zu wsMB=%zu "
                                 "madeMs=%.0f",
                                 s_heap.size(), s_heap.size() * 4, hc,
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - gt0).count());
                        writeResult(gb);
                    }
                    const int bucket = 50;
                    // Counted, not `i % bucket`: the one-shot drain restarts the
                    // clock, and a bucket that spans the restart would divide a
                    // short interval by 50 and report a cost nobody paid.
                    int bn = 0;
                    auto bt0 = std::chrono::steady_clock::now();
                    for (int i = 0; i < g_cfg.restoreLoop; ++i) {
                        // CONTROL ARM 4: drain ONCE, here, rather than on every
                        // restore. The buckets on either side say whether the
                        // cost follows the live heap back down.
                        if (g_cfg.restoreLoopDrainAt > 0
                            && i == g_cfg.restoreLoopDrainAt) {
                            size_t bc = 0, bp = 0;
                            procMemMB(bc, bp);
                            cocos2d::CCPoolManager::sharedPoolManager()->pop();
                            size_t ac = 0, ap = 0;
                            procMemMB(ac, ap);
                            char db[160];
                            snprintf(db, sizeof(db),
                                     "restoreloop_drain: at=%d wsMB %zu -> %zu",
                                     i, bc, ac);
                            writeResult(db);
                            bn = 0;
                            bt0 = std::chrono::steady_clock::now();
                        }
                        if (!g_cfg.restoreLoopKeep) clearCheckpointsUnsectioned(pl);
                        // **Restore from a COLD object, the way the search does.**
                        // secRestoreFrom is this same three-line sequence, so the
                        // path is right; what differs is that the search hands it
                        // a different node's checkpoint every time while this loop
                        // kept re-reading one hot object. With `restoreloopcycle=1`
                        // it walks the held ones instead, so each restore reads
                        // ~70 KB that has not been touched in N iterations.
                        CheckpointObject* src = g_ckpt;
                        if (g_cfg.restoreLoopCycle && !s_held.empty())
                            src = s_held[(size_t)i % s_held.size()];
                        pl->storeCheckpoint(src);
                        // CONTROL ARM 1: make the bench look like the search, which
                        // throws this queue away on every restore (secRestoreFrom).
                        // resetLevel pushes one synthetic button command per restore
                        // and removeReleasedButtons walks the whole queue, so the
                        // bench alone carries an O(N) the search does not have.
                        if (g_cfg.restoreLoopClearQ) pl->m_queuedButtons.clear();
                        g_restorePending = true;
                        pl->resetLevel();
                        g_restorePending = false;
                        // CONTROL ARM 2: drain the autorelease pool the way the end
                        // of a frame would. The loop never returns a frame, so every
                        // autoreleased object a restore makes (candidate 1: the
                        // ColorAction generation that GJEffectManager::reset throws
                        // away and loadDefaultColors/loadFromState rebuild) stays
                        // live. If the cost slope and the memory slope BOTH vanish
                        // here, that is the mechanism.
                        if (g_cfg.restoreLoopDrain)
                            cocos2d::CCPoolManager::sharedPoolManager()->pop();
                        if (++bn == bucket) {
                            bn = 0;
                            const auto bt1 = std::chrono::steady_clock::now();
                            size_t wsCur = 0, wsPeak = 0;
                            procMemMB(wsCur, wsPeak);
                            size_t secLive = 0, secPhys = 0;
                            sectionCensus(pl, secLive, secPhys);
                            char sb[224];
                            snprintf(sb, sizeof(sb),
                                     "restoreloop_series: i=%d perMs=%.4f arr=%d "
                                     "wsMB=%zu peakMB=%zu actN=%d "
                                     "secLive=%zu secPhys=%zu",
                                     i + 1,
                                     std::chrono::duration<double, std::milli>(
                                         bt1 - bt0).count() / bucket,
                                     pl->m_checkpointArray
                                         ? (int)pl->m_checkpointArray->count() : -1,
                                     wsCur, wsPeak, runningActionsOn(m_player1),
                                     secLive, secPhys);
                            writeResult(sb);
                            bt0 = bt1;
                        }
                    }
                    const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - rt0).count();
                    char rb[192];
                    snprintf(rb, sizeof(rb),
                             "restoreloop: n=%d totalMs=%.1f perMs=%.4f "
                             "perSec=%.0f ckptTick=%lld",
                             g_cfg.restoreLoop, ms, ms / g_cfg.restoreLoop,
                             1000.0 * g_cfg.restoreLoop / (ms > 0 ? ms : 1e-9),
                             (long long)g_ckptTick);
                    writeResult(rb);
                }
                // Section-limited GD solver. If the checkpoint exists and it has not run
                // yet, run it to completion inside this frame (no return to rendering
                // during the search).
                // `secsolve=1` without a checkpoint used to do NOTHING and say
                // nothing -- the level simply completed and the result carried
                // no sec line at all, which reads as "the search found nothing"
                // rather than "the search never ran". Same family as a
                // value-less flag that silently does not apply.
                if (secsolve::g_on && !g_ckpt && !secsolve::g_warnedNoCkpt
                    && g_cfg.checkpointAt < 0) {
                    secsolve::g_warnedNoCkpt = true;
                    writeResult("secsolve: NOT RUNNING -- secsolve=1 needs a "
                                "checkpoint (set practiceat= and checkpointat=)"
                                "; secstart alone does nothing");
                }
                if (secsolve::g_on && g_ckpt && !secsolve::g_done
                    && g_tick >= secsolve::g_startTick) {
                    secsolve::g_done = true;
                    if (secsolve::g_worldDiff > 0) runWorldDiff(pl);
                    else if (secsolve::g_snapCmp > 0) runSnapCompare(pl);
                    else if (secsolve::g_verify) runSectionVerify(pl);
                    // The search is created suspended and takes its first slice below, on this
                    // same frame -- so the one-frame path and the sliced path start identically.
                    else {
                        secsolve::g_task = runSectionSolve(pl);
                        secsolve::g_taskLayer = pl;
                    }
                }
                // The acceptance for this pass: every snapshot is checked by
                // restoring it and running the same ticks again. It runs once,
                // after the last window has gone by -- the pass's own remaining
                // progress is not meaningful afterwards, since the game has
                // been rewound to a snapshot and left there.
                //
                // The restored run is fed the plan through the normal path, so
                // the cursors are rewound with the game exactly as a section
                // run will do it. Anything that does not match here is either
                // one of the known holes reopening or a new one.
                // ...once EVERY requested snapshot has been taken. Keying this
                // on g_snaps.back() alone fired after the first one, because
                // "the last snapshot so far" is not "the last snapshot there
                // will be" -- measured: it verified 1/1 and then went on to
                // take three more.
                if (g_cfg.snapVerify > 0 && !g_snapVerified && !g_snaps.empty()
                    && g_nextSnap >= g_cfg.snapAt.size()
                    && g_tick > g_snaps.back().tick + g_cfg.snapVerify) {
                    g_snapVerified = true;
                    int okCount = 0;
                    for (auto& s : g_snaps) {
                        std::vector<SnapState> got;
                        clearCheckpointsUnsectioned(pl);
                        pl->storeCheckpoint(s.cp);
                        g_restorePending = true;
                        pl->resetLevel();
                        g_restorePending = false;
                        // The same two things the section restore does, for the
                        // same measured reason. resetLevel pushes a button
                        // command of its own on every restore -- {button=1,
                        // push=<is the UI holding?>} -- and a bot's UI holds
                        // nothing, so what it pushes is a RELEASE, consumed on
                        // the first substep after the restore. And nothing puts
                        // the hold back, so a snapshot taken mid-hold replays
                        // with the button up.
                        //
                        // Measured before this (gd-7d): snapshots whose entry
                        // was HELD reproduce 29 of 33 times as MISMATCH, while
                        // held=0 entries mismatch 37 of 154. The prediction this
                        // change is accepted against is that the held=1 column
                        // goes to about zero and the held=0 column does not move.
                        pl->m_queuedButtons.clear();
                        // ...and the player as the pass had it. The checkpoint
                        // does not carry all of it: loadFromCheckpoint copies its fields back and
                        // then calls the mode setters, which overwrite some of them again
                        // (m_accelerationOrSpeed goes to 1.5 for a robot, which reads as "the
                        // hold's boost is over"), and m_touchedPad is left as the run before the
                        // restore had it. Measured on lv19, snapshots at t=9,200..11,200 verified
                        // in one pass: 2/6 reproduced the head run without this, 6/6 with it,
                        // and writing back those two members alone also gave 6/6 where either
                        // one alone gave 2/6. The held=0 snapshots that failed failed because a
                        // restored run that had come apart ran before them; one restored on its
                        // own reproduced.
                        //
                        // With it, a held entry is armed the way the section search arms one
                        // (secArmHold), not pressed again. A press is a new press: it fires a
                        // ring the player is touching there and then. Measured on lv18 at
                        // t=2,400, the re-press took a ring the pass held through (vy 5.348 ->
                        // 8.05); writing the player back over that undid the jump but not the
                        // ring's use, the restored run came apart 104 px, and every snapshot
                        // after it in the pass came apart too, where 51/52 had reproduced.
                        const bool putPlayer = m_player1 && !s.player.empty();
                        if (s.held && putPlayer) {
                            m_player1->m_holdingButtons[1] = true;
                            if (m_player2) m_player2->m_holdingButtons[1] = true;
                        } else if (s.held) {
                            g_injecting = true;
                            this->handleButton(true, 1, true);
                            g_injecting = false;
                        }
                        if (putPlayer) psnap::restore(m_player1, this, s.player.data());
                        // ...and what the byte copy cannot carry: the containers and object
                        // pointers (the restore leaves the slope map's count and m_currentSlope
                        // different from the pass's), carried as the section search carries them,
                        // and GD's anti-cheat check (see SnapExtra). The check is the one the gate
                        // measured: on lv16 at t=6,400 and 10,800 the pass left a ramp at its top
                        // with the ramp's speed (vy 9.208, 3.683) and the restored run stayed on it
                        // (vy -0.215) -- postCollision, which launches it, was skipped while the
                        // spike was being hit. The containers and pointers alone did not change
                        // that, and no snapshot of the gate needed them.
                        const size_t si = (size_t)(&s - g_snaps.data());
                        if (putPlayer && si < g_snapExtras.size()) {
                            const SnapExtra& e = g_snapExtras[si];
                            secRestoreDash(e.dash);
                            if (m_player2) secRestoreDash(e.dash2, m_player2);
                            psnap::restoreTouch(m_player1, e.touch, this);
                            pl->m_damageVerified = e.damageVerified;
                        }
                        g_nextInput = s.nextInput;
                        g_nextToggle = s.nextToggle;
                        g_tick = s.tick;
                        g_snapProbe = &got;
                        const long long target = s.tick + g_cfg.snapVerify;
                        for (int guard = 0; g_tick < target && guard < 4096; ++guard)
                            GJBaseGameLayer::update((float)(1.0 / 60.0));
                        g_snapProbe = nullptr;
                        // compare by tick, so a frozen update or a skipped row
                        // cannot silently align two different moments
                        std::unordered_map<long long, const SnapState*> byT;
                        for (const auto& g : got) byT[g.t] = &g;
                        double worst = 0.0;
                        long long firstBad = -1;
                        size_t n = 0;
                        for (const auto& h : s.head) {
                            auto it = byT.find(h.t);
                            if (it == byT.end()) continue;
                            ++n;
                            const double d = std::max(
                                std::fabs(h.y - it->second->y),
                                std::fabs(h.vy - it->second->vy));
                            if (d > worst) worst = d;
                            if (d > 1e-9 && firstBad < 0) firstBad = h.t;
                        }
                        const bool ok = (n == s.head.size() && worst <= 1e-9);
                        okCount += ok;
                        char vb[224];
                        snprintf(vb, sizeof(vb),
                                 "snapverify: at=%lld held=%d compared=%zu/%zu "
                                 "worst=%.9f firstBad=%lld %s",
                                 s.tick, s.held, n, s.head.size(), worst,
                                 firstBad, ok ? "OK" : "MISMATCH");
                        writeResult(vb);
                    }
                    writeResult("snapverify: " + std::to_string(okCount) + "/"
                                + std::to_string(g_snaps.size()) + " snapshots "
                                "reproduce the head run");
                }
                if (g_cfg.restoreAt >= 0 && g_ckpt && !g_restoreDone && g_tick >= g_cfg.restoreAt) {
                    g_restoreDone = true;
                    long long fromTick = g_tick;
                    // The official restore path: reduce the list to our own checkpoint only,
                    // then resetLevel (in practice mode resetLevel restores to the latest
                    // checkpoint)
                    clearCheckpointsUnsectioned(pl);
                    pl->storeCheckpoint(g_ckpt);
                    g_restorePending = true;
                    pl->resetLevel();
                    g_restorePending = false;
                    ev("RESTORE_done", (double)fromTick, (double)g_ckptTick);
                    // player+0x187 is the out-of-bounds kill's 2-substep latch:
                    // checkCollisions reads it, clears it, and re-latches this
                    // substep's condition, and it kills only when BOTH substeps
                    // said out-of-bounds. It is written in exactly three places
                    // (checkCollisions twice, the constructor once) -- NOT in
                    // resetObject, NOT in loadFromCheckpoint, NOT in
                    // PlayerCheckpoint. So it survives a restore, and printing
                    // it either side of one is how that gets seen rather than
                    // argued.
                    // PATCH (hole 1): put the latch back. GD's
                    // restore does not, so without this the value left behind
                    // by whatever ran before the restore survives into the
                    // restored run -- and "restore and continue" then differs
                    // from "replay from the head", which is exactly what §5.3
                    // forbids.
                    //
                    // `wasLatch` is what GD's own restore left, printed beside
                    // the value being written so one line carries the evidence
                    // that the patch is doing something rather than agreeing
                    // with the game by accident.
                    // ...and the dash (hole 2), for the same reason: GD saved it
                    // and its own restore refused to read it back.
                    const int wasDash = m_player1 ? (int)m_player1->m_isDashing : -1;
                    secRestoreDash(secsolve::g_ckptDash);
                    // ...and the exact y velocity (hole 3). GD's restore puts
                    // it back on the 0.001 grid -- MEASURED by injecting a value
                    // before the checkpoint and reading what came back:
                    //   1.9815    -> 1.982      half away from zero
                    //   -3.5935   -> -3.594     same, on the negative side
                    //   2.3164999 -> 2.316      below half, down
                    //   7.0001    -> 7.000      down
                    // so the half-grid velocities that flipGravity's x0.5 and a
                    // ball tap's x0.6 produce do not survive a restore. The
                    // captured value is written back verbatim.
                    const double wasVy = m_player1 ? m_player1->m_yVelocity : 0.0;
                    if (m_player1) m_player1->m_yVelocity = g_ckptVy;
                    g_restoreProbe = 3;    // report the next three updates
                    // ...and rewind the plan's cursors with it, and the tick
                    // they are keyed on. Without this the restored run gets a
                    // different input stream and every comparison against a
                    // replay from the head measures that instead of the
                    // restore.
                    g_nextInput = g_ckptNextInput;
                    g_nextToggle = g_ckptNextToggle;
                    g_tick = g_ckptTick;
                    const int wasLatch = m_player1
                        ? (int)(unsigned char)reinterpret_cast<const char*>(m_player1)[kOobLatchOff]
                        : -1;
                    if (m_player1)
                        reinterpret_cast<char*>(m_player1)[kOobLatchOff] =
                            (char)g_ckptOobLatch;
                    const char* pb1 = m_player1
                        ? reinterpret_cast<const char*>(m_player1) : nullptr;
                    writeResult("restore: from=" + std::to_string(fromTick)
                        + " to=" + std::to_string(g_ckptTick) + " tickNow=" + std::to_string(g_tick)
                        + " oobLatch=" + std::to_string(pb1 ? (int)(unsigned char)pb1[kOobLatchOff] : -1)
                        + " oobAtCkpt=" + std::to_string((int)g_ckptOobLatch)
                        + " oobGDLeft=" + std::to_string(wasLatch)
                        + " dashGDLeft=" + std::to_string(wasDash)
                        + " dashAtCkpt=" + std::to_string((int)secsolve::g_ckptDash.on));
                    {
                        char vb[224];
                        snprintf(vb, sizeof(vb),
                                 "restorevy: ckpt=%.12f gdLeft=%.12f now=%.12f",
                                 g_ckptVy, wasVy,
                                 m_player1 ? m_player1->m_yVelocity : 0.0);
                        writeResult(vb);
                    }
                }
            }
        }
        // Pause/step for manual experiments (F2/F3/F4). Keys and the overlay are handled
        // here first so they work outside a session too (if a later return stopped the
        // keys from being picked up, the pause could never be released)
        {
            const bool searching = secsolve::inFlight();
            const auto ot0 = std::chrono::steady_clock::now();
            updateOverlays(this);
            if (searching)
                secsolve::g_overlayMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - ot0).count();
        }
        audio::sync();
        notify::watchdog();   // un-freeze and hide a notification a purge left pinned
        notify::selfTest();   // cfg `notifytest=1` only
        hitbox::perFrame(this);
        // The stall guard's deferred reset (see g_stallResetPending): performed here, at
        // the same boundary where plans are installed, so the whole next batch runs on the
        // fresh attempt from its first tick.
        // Both this and the poll below can reset the level, and a reset in the middle of a
        // section search replaces the world that search is expanding into -- so both wait for
        // it. Nothing is lost by waiting: a search ends its own session.
        if (g_stallResetPending && !secsolve::inFlight()) {
            g_stallResetPending = false;
            if (auto* pl = PlayLayer::get()) {
                writeResult("stall: resetLevel at the frame boundary");
                pl->resetLevel();
            }
        }
        if (!secsolve::inFlight())
            dpsolve::poll();   // installs the in-process solver's plan at a frame boundary
        // cfg cpflight: the plan poll just installed -- or the probe it queued -- starts from a
        // checkpoint here, at the same boundary; and a clear from a checkpoint is flown again from
        // the head (mod/cp_flight.hpp).
        if (static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this && !secsolve::inFlight()) {
            if (cpflight::g_pending.rec) cpStart(PlayLayer::get());
            // The human takeover: a reset GD asked for while the person played, made a restore
            // of its checkpoint here, at the same boundary (mod/takeover.hpp).
            if (takeover::g_restorePending) takeoverRestore(PlayLayer::get());
            if (cpflight::g_headPending) {
                cpflight::g_headPending = false;
                if (auto* pl = PlayLayer::get()) pl->resetLevel();
            }
            if (cpflight::g_controlPending) {   // the probe's control: the same plan from the head
                cpflight::g_controlPending = false;
                if (auto* pl = PlayLayer::get()) {
                    const cpflight::CarrySig now = cpflight::carrySig(pl);
                    cpflight::g_probeCap.controlCarry = cpflight::sigDiff(cpflight::g_probeCap.sourceSig, now);
                    if (g_cfg.cpFlightProbeCarry)
                        cpflight::g_probeCap.controlVsProbe = cpflight::sigDiff(cpflight::g_probeCap.probeSig, now);
                    cpflight::g_controlStarting = true;
                    pl->resetLevel();
                    cpflight::g_controlStarting = false;
                    g_paused = false;   // held since the probe's end, as the probe's own start is
                }
            }
        }
        pollProbeHotkeys();
        handleProbeRequest(this);
        // ---- one slice of the section search, then the frame goes back to the game ----
        // Placed after the overlays and the audio so each slice is bracketed by a HUD that
        // redraws and a song that stays stopped, and BEFORE every path that advances game time:
        // while a search is in flight the world belongs to it, exactly as it belongs to the DP
        // solve during its own freeze a few lines down.
        if (secsolve::inFlight()) {
            if (PlayLayer::get() != secsolve::g_taskLayer) {
                // The level was rebuilt underneath a suspended search. Its frame still points at
                // the old one, so it can only be dropped -- resuming would walk freed objects.
                writeResult("secsolve: the level was rebuilt during the search - dropping it");
                secsolve::g_task.destroy();
                secsolve::g_taskLayer = nullptr;
                secsolve::setSearching(false);
                secsolve::g_noKill = false;
            } else if (g_sessionOver) {
                // The session ended while a search was suspended between slices -- a give-up, the
                // `quit` command, the budget -- and nothing asked it to stop: it went on being resumed
                // here to its own end, restoring and stepping the level for minutes after the solve was
                // over (a custom level, 2026-10-09: 170 s past `session_end`, ~90,000 restores, each of
                // which leaves memory behind). Leaving the level drops it (resetSessionState); this is
                // every other way out. Dropped here, at the frame boundary, never from inside it.
                const int before = postmortem::privateMB();
                secsolve::reset();   // the task destroyed, its tables and checkpoints let go
                writeResult("secsolve: the session ended during the search - dropped it (private "
                            + std::to_string(before) + " -> " + std::to_string(postmortem::privateMB())
                            + " MB)");
                dpsolve::memCensus("search dropped");   // cfg memcensus (print only)
            } else {
                secsolve::g_sliceStart = std::chrono::steady_clock::now();
                if (secsolve::g_sliceEnded) {
                    secsolve::g_frameGapMs += std::chrono::duration<double, std::milli>(
                        secsolve::g_sliceStart - secsolve::g_sliceEnd).count();
                    ++secsolve::g_frames;
                }
                secsolve::g_task.resume();
                secsolve::g_sliceEnd = std::chrono::steady_clock::now();
                secsolve::g_sliceEnded = true;
                if (secsolve::g_task.done()) {
                    secsolve::g_task.destroy();
                    secsolve::g_taskLayer = nullptr;
                    secsolve::g_sliceEnded = false;
                }
                return;
            }
        }
        // Consume a render-resume request at the frame boundary: reset the level and
        // rebuild the render state with it. The order -- reset first, then resume
        // rendering -- is the point; in the reverse order the first updateVisibility after
        // resuming steps on state that piled up while stopped
        //
        // ABOVE THE STOPS, and that is the whole reason it is here rather than below them.
        // A search holds the level with g_paused (repair.hpp's spawn), so for the entire time the
        // solver thread is working, every frame returned before reaching this -- the request sat
        // raised, the screen stayed black, and pressing the key again did nothing because the
        // request it wanted to raise was already up. Reported 2026-08-28 as "F5 sometimes does not
        // switch": it always switched, just not until the search let a frame through. The reset is
        // safe here for the same reason the freeze is: the solver thread reads the recorder's
        // finished buffers, never the level.
        if (g_visResetPending && g_started && !g_sessionOver) {
            g_visResetPending = false;
            // The spectating speed is deliberately left alone: F8 is a render toggle now, and
            // forcing 1x here would silently undo the arrow keys. The cfg-driven paths
            // (watchat / watchback) set the speed themselves before asking for the reset.
            if (auto* pl = PlayLayer::get()) {
                log::info("watch: reset level, then resume rendering");
                // The reset's own visibility pass is what draws a level that is held still (a search holds
                // it, and no frame reaches PlayLayer::update to run one): with skipRender it would be
                // skipped here, before real time is back, and the screen came up black. Only a session the panel
                // started has a screen to come back to; a worker's reset is left as it always was.
                const bool skipWas = g_cfg.skipRender;
                if (g_uiSession) g_cfg.skipRender = false;
                pl->resetLevel();
                g_cfg.skipRender = skipWas;
                g_realtimeOverride = true;
                return;   // this frame ends here
            }
            g_realtimeOverride = true;
        }
        // Update the HUD before the pause branch (keep HUD and liveness display alive
        // while paused)
        // A seek in flight lifts the stop for as long as it takes to land, and the arrival puts
        // it back (g_seekRepause). It has to: a seek gets where it is going by running the level,
        // and with time frozen it never arrives -- a forward seek asked for while F2 was down
        // used to hang the level behind its own blackout for good. The one that needs this is a
        // backward frame-step, which physics cannot do in reverse and so must replay.
        if (probe::g_pause && !itermap::seeking()) {
            if (probe::g_step > 0) {
                GJBaseGameLayer::update(probe::g_step / 240.0f);
                probe::g_step = 0;
            }
            return; // freeze game time completely
        }
        // The human takeover's stop before the person's run (cfg takeoverwait), over on the wall
        // clock: the game runs on from where the checkpoint holds it.
        if (takeover::g_waiting && std::chrono::steady_clock::now() >= takeover::g_waitUntil) {
            takeover::g_waiting = false;
            g_paused = false;
        }
        // Auto-stop just before a wall (cfg `pauseatx`). Meant for freezing the screen to
        // take a picture; it only stops, so physics is unaffected (update is not called
        // afterwards = time does not advance)
        if (g_pauseAtX > 0.f && !g_pauseAtXFired && !g_paused && g_started
            && !g_sessionOver
            && m_player1 && m_player1->getPositionX() >= g_pauseAtX) {
            g_pauseAtXFired = true;   // once only; otherwise F7 could not resume
            g_paused = true;
            writeResult("pauseatx: paused at x=" + std::to_string(m_player1->getPositionX())
                + " y=" + std::to_string(m_player1->getPositionY())
                + " tick=" + std::to_string((long long)g_tick));
        }
        // ...and by tick (cfg `pauseattick`), once per session, for two runs to be compared side by
        // side at the same moment. Not during a seek: the stop is for the run that plays after it.
        {
            static bool s_tickFired = false;
            if (!g_started) s_tickFired = false;
            if (itermap::g_pauseAtTick > 0 && !s_tickFired && !g_paused && g_started && !g_sessionOver
                && !itermap::seeking() && m_player1 && g_tick >= itermap::g_pauseAtTick) {
                s_tickFired = true;
                g_paused = true;
                writeResult("pauseattick: paused at tick=" + std::to_string((long long)g_tick)
                    + " x=" + std::to_string(m_player1->getPositionX())
                    + " y=" + std::to_string(m_player1->getPositionY())
                    + " run=" + (seekcp::g_from < 0 ? std::string("head") : "cp@" + std::to_string(seekcp::g_from)));
            }
        }
        // While paused: do not call update at all, freezing game time. If a step was
        // requested, advance by exactly that much
        if (g_started && g_paused && !itermap::seeking()) {   // ...and the same for this stop
            if (g_stepTicks > 0) {
                ev("STEP_exec", g_stepTicks);
                GJBaseGameLayer::update(g_stepTicks / 240.0f);
                g_stepTicks = 0;
                flushAll();
            }
            return;
        }
        // A seek asked for by clicking the iteration map (itermap.hpp), when the target is
        // BEHIND the player and the level has to run again. A frame-boundary job, like every
        // other level change in this file -- resetLevel() from inside touch handling would be
        // the mistake the stall guard's deferred reset exists to avoid.
        if (itermap::g_seekRestart && g_started) {
            itermap::g_seekRestart = false;
            // The bar keeps working on the results screen, which is exactly when you want to go
            // back and look at something. Reviving is the same three lines F7 uses, and for the
            // same reason: the session ended with the level, not with the watching of it. Replays
            // only -- in serve mode the session belongs to the driver.
            if (itermap::g_seekRevive) {
                itermap::g_seekRevive = false;
                if (g_sessionOver && !g_serveMode) {
                    g_sessionOver = false;
                    g_finishedAttempts = 0;
                    ++g_cfg.maxAttempts;
                    writeResult("session_resume: the seek bar restarted the replay");
                }
            }
            g_paused = false;
            clearManualPause();
            g_pauseAtXFired = false;
            if (!g_sessionOver) {
                if (auto* pl = PlayLayer::get()) {
                    itermap::dismissEndScreen();   // or every clear stacks another one
                    // Before the reset, not after: resetLevel starts the song from the top, and
                    // audio::sync only gets to pause it on the NEXT frame -- which is the blip of
                    // the track's first moment you hear on every rewind.
                    audio::muteForSeek();
                    // cfg seekcp: from the latest checkpoint at or before the target, if any
                    if (!(itermap::g_seekTick >= 0 && seekCpRestore(pl, itermap::g_seekTick)))
                        pl->resetLevel();
                    // ...and again on the OTHER side of it. The reset starts the song on a
                    // channel it creates itself, so the group silenced a line ago is not the
                    // group that is now playing (see hushNow).
                    audio::hushNow();
                    return;   // this frame ends here; the seek runs from the next one
                }
            }
            itermap::endSeek("nothing to restart");
        }
        // ...and the arrival. The seek ends where it was asked to end, or on a death that got
        // there first -- a plan that dies on the way is the answer to "what is at that point", so
        // the seek hands the speed back and lets it be seen rather than racing past it.
        if (itermap::seeking()) {
            const float sx = m_player1 ? m_player1->getPositionX() : 0.f;
            const bool sdead = !m_player1 || m_player1->m_isDead;
            const bool over = g_sessionOver;
            if (over) itermap::endSeek("the session ended");
            else if (itermap::seekArrived(sdead, dt))
                itermap::endSeek(sdead ? "died on the way" : "arrived");
            // Landed: put back the stop the seek had to lift to get here (a frame-step
            // backwards while paused is the case -- see g_seekRepause).
            if (!itermap::seeking() && itermap::g_seekRepause) {
                itermap::g_seekRepause = false;
                probe::g_pause = true;
                // A backward step aimed short on purpose; walk the last few ticks with the
                // step mechanism, which is exact where a seek batch is not. The pause branch
                // above consumes this on the next frame.
                long long fin = 0;
                if (itermap::g_seekStepTo >= 0)
                    fin = itermap::g_seekStepTo - itermap::g_nowTick;
                probe::g_step = (fin > 0 && fin < 240) ? (int)fin : 0;
                // The whole arithmetic of a backward step, in one line, because from outside the
                // process a step that lands in the wrong place and one that never ran look the
                // same. Reported 2026-08-28: a step back of 10 moved the tick FORWARD by 40.
                if (itermap::g_seekStepTo >= 0) {
                    char sb[192];
                    snprintf(sb, sizeof(sb),
                             "itermap: [stepback] landed t=%lld, wanted t=%lld, finishing +%d",
                             (long long)itermap::g_nowTick,
                             (long long)itermap::g_seekStepTo, probe::g_step);
                    writeResult(sb);
                }
                itermap::g_seekStepTo = -1;
            }
            // A rewind restarts the level, and GD's spawn effect is born behind the cover. Its
            // animation is driven by cocos ACTIONS, which only advance on rendered frames, and a
            // seek renders very few -- so the effect is still near its beginning when the cover
            // lifts and plays out over the arrival.
            //
            // NOT purgeDanglingActions() here, whatever the render toggle does. Tried
            // 2026-08-28: it takes the PLAYER's actions with everything else, and the player
            // came back from a rewind invisible -- its spawn animation is an action like any
            // other, and purging one mid-flight leaves the sprite wherever that action had got
            // to. The toggle can afford it because it resets the level immediately afterwards;
            // an arrival cannot.
            //
            // The circle waves are safe (they are effects that remove themselves on the
            // scheduler, which is why the sweep exists) but do not appear to be what is seen.
            if (!itermap::seeking() && g_fxSweep) sweepCircleWaves();
        }
        ev("BGL_update_pre", dt);
        // cfg `watchat`: at the given tick, cause the same state transition as F8 (return to
        // realtime). Must be the same transition as the F8 handler (a different path would
        // measure something different)
        bool wantWatch = (g_watchAtTick > 0 && g_tick >= g_watchAtTick)
            || (g_watchAtX > 0.f && m_player1 && m_player1->getPositionX() >= g_watchAtX);
        if (!wantWatch && g_watchAfterSec > 0.0) {
            double el = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - solver::g_solveStart).count();
            if (el >= g_watchAfterSec) wantWatch = true;
        }
        // (not under a section solve's hold, which runs the fast loop on purpose: secRenderHold)
        if (wantWatch && !g_realtimeOverride && g_cfg.fastdt > 0
            && g_watchStartTick < 0 && g_secRenderWas < 0) {
            g_realtimeOverride = true; watchSpeedSet(WATCH_SPEED_1X);
            g_visRefresh = true;
            g_visResetPending = true;
            if (g_watchPurge) purgeDanglingActions();
            g_watchStartTick = g_tick;
            float wx = m_player1 ? m_player1->getPositionX() : -1.f;
            log::info("watchat: tick={} x={} -> realtime (F8 equivalent)",
                (long long)g_tick, wx);
            writeResult("watchat: tick=" + std::to_string((long long)g_tick)
                + " x=" + std::to_string(wx) + " -> realtime (F8 equivalent)");
            writeResult(fxCensus("watchat"));
        }
        // Reproduce the usage of pressing F8 repeatedly (cfg `watchcycle=<seconds>`).
        // Repeat the same state transition as the F8 handler on the wall clock
        if (g_watchCycleSec > 0.0 && g_cfg.fastdt > 0 && g_started && !g_sessionOver) {
            double el = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - solver::g_solveStart).count();
            if (g_watchCycleLast < 0.0) g_watchCycleLast = el;
            if (el - g_watchCycleLast >= g_watchCycleSec) {
                g_watchCycleLast = el;
                ++g_watchFlips;
                // Go through the same function as a human key press (poking the state
                // directly would measure something different)
                renderTogglePress();
                writeResult("watchcycle: press=" + std::to_string(g_watchFlips)
                    + " tick=" + std::to_string((long long)g_tick)
                    + " -> " + (g_realtimeOverride ? "realtime" : "FAST/pending"));
            }
        }
        // Return leg: from realtime back to frame skipping. Keep it the same 2 lines as the
        // last branch of the F8 cycle
        if (g_watchBackTicks > 0 && g_realtimeOverride && g_watchStartTick >= 0
            && g_tick - g_watchStartTick >= g_watchBackTicks) {
            g_realtimeOverride = false; watchSpeedSet(WATCH_SPEED_1X);
            g_watchBackTicks = 0;
            log::info("watchback: tick={} -> FAST (frame skip again)", (long long)g_tick);
            writeResult("watchback: tick=" + std::to_string((long long)g_tick)
                + " -> FAST (frame skip again)");
        }
        // End-zone lock tracking, for BOTH branches below. GD sets m_isLocked when the end
        // sequence takes the player (and during the level-start animation, hence x > 500).
        // The sequence is driven by the scheduler = by REAL frames, so everything that
        // reasons about it (the burn below, the fast loop's pacing, the stall guards) has
        // to read a wall clock, not ticks.
        profsample::frame(g_started && !g_sessionOver);   // cfg profsample (off)
        syncSectionHooks();                               // cfg sectionhooklazy (off)
        syncActWatchHook();                               // cfg objwatch (print only)
        const bool lockedNow = g_started && !g_sessionOver && m_player1
                   && m_player1->m_isLocked && m_player1->getPositionX() > 500.f;
        const auto nowLock = std::chrono::steady_clock::now();
        if (!lockedNow) g_endzoneLockStart = {};
        else if (g_endzoneLockStart.time_since_epoch().count() == 0)
            g_endzoneLockStart = nowLock;
        if (g_started && !g_sessionOver && g_cfg.fastdt > 0 && !g_realtimeOverride) {
            // Visual effects while not rendering are not deleted but "advanced to
            // completion". GD's timed effects clean up by "the action removing itself",
            // but in the fast loop actions advance only once per rendered frame and pile
            // up, so advance the actions by the game time advanced in this frame.
            // Do not advance the whole scheduler (the game layer's update hangs off it too
            // and the physics would break). Advance only the action manager (actions are
            // for visual effects only)
            if (g_fxSweep && g_cfg.skipRender && vtimeNow() <= 0.f) {
                // ...by the game time the last batch actually ran, once batches are cut by wall
                // time (cfg framebudgetms); without that the sweep ran a full batch's worth per
                // frame however few ticks the frame had. Off, it is the full batch, as before.
                const int ran = g_cfg.frameBudgetMs > 0.0 ? std::max(1, g_lastBatchCalls)
                                                          : std::max(1, g_cfg.fastloops);
                float gameSecs = g_cfg.fastdt * ran;
                if (auto* d = cocos2d::CCDirector::sharedDirector())
                    if (auto* am = d->getActionManager())
                        static_cast<ActionManagerTick*>(am)->update(gameSecs);
                // CCCircleWave removes itself via the scheduler, not an action, so it gets
                // a separate sweep
                sweepCircleWaves();
            }
            // Fast mode: run a large number of physics ticks per rendered frame.
            // ...except while the END SEQUENCE holds the player (lockedNow): its
            // completion is scheduler-driven, i.e. it needs REAL frames, and pumping
            // 1,800 physics ticks a frame only inflates g_tick while the animation
            // stands still -- 3,000 "stillness" ticks pass in under two frames and the
            // tick-based stall guard was killing legitimate clears at the end wall
            // (lv8/11/17; see ENDZONE_KILL_AFTER_SEC). One tick per frame lets wall
            // time flow, the sequence finish and levelComplete fire.
            stallwatch::Mark fm(stallwatch::FASTLOOP);
            int fastLoops = lockedNow ? 1 : std::max(1, g_cfg.fastloops);
            // A section solve handed over by the loop (secsolve / secrung) replays the verified
            // prefix to its head with fastloops=1, because the practice switch and the checkpoint
            // are placed at frame boundaries and have to land on the asked tick. Only the last
            // ticks before the practice switch need that; the rest is a plain replay of a plan
            // the loop flies at its own rate every round. At one update a frame the replay was
            // the dearest part of a rung: 35-40 s from the loop's last death to its next one
            // around a search of 1-6 s (lv4003, heads at t=7,700-8,400). g_secSaveFastloops is
            // the loop's rate, written by the handoff only, so a cfg-driven section run keeps 1.
            const long long headAt = (secsolve::g_on && !secsolve::g_done && !g_ckpt
                                      && g_cfg.practiceAt > 0)
                                         ? (long long)g_cfg.practiceAt - 8 : -1;
            if (!lockedNow && headAt > 0 && g_tick < headAt && g_secSaveFastloops > fastLoops)
                fastLoops = g_secSaveFastloops;
            if (lockedNow) {
                static long long s_lockLogged = -1;
                if (s_lockLogged != g_attempt) {
                    s_lockLogged = g_attempt;
                    writeResult("endzone: player locked at x="
                        + std::to_string((int)m_player1->getPositionX())
                        + " tick=" + std::to_string((long long)g_tick)
                        + " - dropping to 1 tick/frame for the end sequence");
                }
            }
            // cfg framebudgetms: the batch also ends when the frame has used its wall time, so a
            // heavy level still hands the window back several times a second. The calls are the
            // same calls at the same dt -- only where the frame boundaries fall moves -- and the
            // first call always runs, so a frame never stalls the game outright.
            const auto batchT0 = std::chrono::steady_clock::now();
            g_lastBatchCalls = 0;
            for (int i = 0; i < fastLoops; ++i) {
                if (g_sessionOver) break;
                if (i > 0 && g_cfg.frameBudgetMs > 0.0
                    && std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - batchT0).count()
                           >= g_cfg.frameBudgetMs)
                    break;
                // The END SEQUENCE took the player mid-batch: stop pumping ticks
                // right here, so the next frames run at 1 tick/frame (lockedNow
                // above) and wall time reaches the scheduler. Without this the
                // whole pin-to-stillness window fits inside ONE batch.
                if (m_player1 && m_player1->m_isLocked
                    && m_player1->getPositionX() > 500.f) break;
                // Serve mode: once the one supplied attempt is over, stop inside this loop.
                // The frame boundary is too late (thousands of ticks per frame, so hundreds
                // of attempts would run on their own)
                if (g_serveMode && g_serveWait) break;
                // ...and the section head's last ticks go back to one update a frame (above).
                if (i > 0 && headAt > 0 && g_tick >= headAt) break;
                if (g_serveReset) {
                    g_serveReset = false;
                    if (auto* pl = PlayLayer::get()) pl->resetLevel();
                }
                if (g_preludeResetPending && g_preludePendingAttempt != g_attempt) g_preludeResetPending = false;
                if (g_preludeResetPending) {
                    g_preludeResetPending = false;
                    ++g_preludeResets;
                    ++g_finishedAttempts;   // the prelude attempt ended by its `until`
                    if (auto* pl = PlayLayer::get()) pl->resetLevel();
                }
                if (i > 0 && takeover::stopsBatch()) break;   // the takeover's checkpoint is in
                // cfg vtime (hooks_vtime.cpp): one frame's worth of actions per update call, as one update a frame
                // gives them -- the scheduler has run the action manager for the frame's first call already
                if (i > 0 && vtimeNow() > 0.f)
                    if (auto* d = cocos2d::CCDirector::sharedDirector())
                        if (auto* am = d->getActionManager())
                            static_cast<ActionManagerTick*>(am)->update(
                                g_vtScale ? vtimeNow() * d->getScheduler()->getTimeScale() : vtimeNow());
                { stallwatch::Mark gm(stallwatch::GD_UPDATE);
                  GJBaseGameLayer::update(g_cfg.fastdt); }
                ++g_lastBatchCalls;
                cpAfterUpdate();   // cfg cpflight: between two update calls, where a restore lands
                takeoverAfterUpdate();
            }
        } else {
            // F8 spectating speed (1x/2x/4x). Rather than increasing dt, call several times
            // with the same dt (doubling dt shifts the position of processing tied to frame
            // boundaries). Below 1x is made on the dt side (the g_watchSpeed < 1 branch
            // above), so here it truncates to 1 call. Only 1x and above become the loop
            // count
            int loops = (g_started && !g_sessionOver)
                            ? std::max(1, (int)g_watchSpeed) : 1;
            // A seek (the seek bar, the arrows) rides this same loop rather than the notch,
            // which stops at 16x. It only ever RAISES the count, so the arrows keep meaning what
            // they meant and the level is back at the user's own speed the moment it ends. The
            // count falls as the target approaches, so it decelerates into it.
            //
            // AND IT PINS dt. The note above is the whole reason: the fast side of the speed has
            // to be more CALLS at the same dt, never a bigger dt, or processing tied to frame
            // boundaries lands somewhere else. During a seek the frames are heavy by
            // construction -- hundreds of batches each -- so the real dt inflates, and passing
            // that through moved exactly the thing the note forbids moving. Measured on lv22: a
            // rewind replayed the plan into a death it does not have at 1x. 1/60 is the value
            // the solve's own verification replays use (cfg fastdt), where the plan reproduces
            // exactly, and it also makes the seek's speed independent of slow motion for free.
            // A takeover session's approach to its point (mod/takeover.hpp) rides the same loop on
            // the same terms: more calls, dt pinned. 0 outside one.
            const int approach =
                takeover::approachLoops(g_tick, m_player1 ? m_player1->getPositionX() : 0.f);
            const bool seekNow = g_started && !g_sessionOver && (itermap::seeking() || approach > 0);
            float stepDt = seekNow ? (1.f / 60.f) : dt;
            // cfg replaystep (replaystep.hpp): the scheduler pass decided how many fixed steps this frame runs.
            const bool fixedStep = replaystep::active() && replaystep::g_calls >= 0 && !seekNow;
            const int fixedCalls = fixedStep ? replaystep::g_calls : 0;
            if (fixedStep) {
                stepDt = replaystep::stepSeconds();
                replaystep::g_calls = -1;
            }
            if (g_started && !g_sessionOver)
                loops = std::max(loops, std::max(itermap::seekLoops(stepDt), approach));
            // The game is STOPPED: run nothing. Normally the pause branch far above has already
            // returned and this is unreachable -- but a seek that lands puts the stop back AFTER
            // that branch has been passed, so the arrival frame would run its ordinary batch on
            // top of a game that is supposed to be frozen. Measured: a backward frame-step landed
            // exactly where it was asked to and the tick had crept 8 further on by the next
            // keypress, which made a sequence of steps drift the wrong way.
            if (g_paused || probe::g_pause) loops = 0;
            // Burning the end animation (a safety net). The end animation is a GD action =
            // driven by rendered frames, so running extra physics does not shorten real
            // time, only inflates ticks -- in realtime replay, correct behaviour is to not
            // enter the burn. Enter only when the lock persists 3 s of real time without
            // completing (the clock must be real time; measuring frame-driven effects in
            // game time is meaningless). GD also sets m_isLocked during the level-start
            // animation, so add the condition that the player has progressed (x>500), and
            // always revert once the lock releases (leave no latch)
            // lockedNow / nowLock / g_endzoneLockStart are maintained once above the
            // branch, shared with the fast loop's end-sequence pacing.
            bool locked = lockedNow && g_endzoneBurnOn
                && std::chrono::duration<double>(nowLock - g_endzoneLockStart).count()
                   >= ENDZONE_BURN_AFTER_SEC;
            if (!locked && g_endzoneBurn && (!m_player1 || !m_player1->m_isLocked)) {
                g_endzoneBurn = false;   // burn over: restore rendering
            }
            if (locked) {
                loops = std::max(loops, 1800);
                // Skip rendering during the burn too (1800 updates per frame with rendering
                // on is too heavy and looks like a freeze). Log only the moment the burn
                // starts
                if (!g_endzoneBurn) {
                    g_endzoneStart = nowLock;
                    writeResult("endzone: still locked "
                        + std::to_string((int)ENDZONE_BURN_AFTER_SEC)
                        + "s after the goal (tick=" + std::to_string((long long)g_tick)
                        + ") -> burning the end animation (render off, 1800 ticks/frame)");
                }
                g_endzoneBurn = true;
            }
            if (fixedStep) loops = fixedCalls;   // 0 when this frame has gathered less than one step
            const bool seekCounts = itermap::seeking();
            if (seekCounts) ++itermap::g_seekFrames;
            // cfg seekblind: screen off up to the last checkpoint slot at or before the target.
            long long blindCut = -1;
            if (seekCounts && itermap::g_seekBlind && !itermap::g_seekBlindDone && itermap::g_seekTick >= 0)
                blindCut = seekBlindCut(itermap::g_seekTick);
            itermap::g_seekBlindNow = blindCut > (long long)g_tick;
            for (int i = 0; i < loops; ++i) {
                // `i > 0` is required: with an unconditional break, update is never called
                // after the session ends and the game freezes. The only thing to stop is
                // "keep running multiple times after the end"
                if (i > 0 && g_sessionOver) break;
                if (i > 0 && takeover::stopsBatch()) break;   // the takeover's checkpoint is in
                // cfg seekvisevery: during a seek, the visibility pass runs on every Nth batch
                // and on the frame's last, so what the draw walks is this frame's.
                const int ve = itermap::g_seekVisEvery;
                itermap::g_seekVisSkip = itermap::g_seekBlindNow
                    || (seekCounts && ve > 1 && i + 1 < loops && (i % ve) != ve - 1);
                // The scheduler gave the first step of the frame its actions; each further one gets its own, as the fast loop does.
                if (fixedStep && i > 0)
                    if (auto* d = cocos2d::CCDirector::sharedDirector())
                        if (auto* am = d->getActionManager())
                            static_cast<ActionManagerTick*>(am)->update(stepDt * d->getScheduler()->getTimeScale());
                stallwatch::Mark gm(stallwatch::GD_UPDATE);
                GJBaseGameLayer::update(stepDt);   // pinned while seeking; see above
                itermap::g_seekVisSkip = false;
                if (seekCounts) ++itermap::g_seekBatches;
                cpAfterUpdate();
                takeoverAfterUpdate();
                // The screen-off stretch ends at the cut (whose checkpoint cpAfterUpdate has just
                // taken) or on a death before it: the rest runs from that checkpoint, loaded on
                // the next frame by the restart branch, with the visibility pass back on.
                if (itermap::g_seekBlindNow
                    && ((long long)g_tick >= blindCut || !m_player1 || m_player1->m_isDead)) {
                    endSeekBlind(blindCut);
                    break;
                }
            }
        }
        // Post-session heartbeat. "Freezes after replay" cannot be told from a hang, so
        // leave numbers on whether updates are running / whether rendering is being
        // skipped. Do not gate on g_started (if it drops after a retry, the heartbeat
        // would stop too)
        if (g_sessionOver || g_retryDone) {
            static int s_beats = 0;
            static std::chrono::steady_clock::time_point s_last{};
            static std::chrono::steady_clock::time_point s_end = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            // Fire the equivalent of a retry (cfg `retryafter`)
            if (g_retryAfterSec > 0.0 && !g_retryDone
                && std::chrono::duration<double>(now - s_end).count() >= g_retryAfterSec) {
                g_retryDone = true;
                if (auto* pl = PlayLayer::get()) {
                    writeResult("retry: calling resetLevel (what the Retry button does)");
                    flushAll();
                    pl->resetLevel();
                }
            }
            if (s_beats < 24
                && std::chrono::duration<double>(now - s_last).count() > 2.0) {
                s_last = now;
                ++s_beats;
                char b[224];
                // So that whoever is stopping things can be named, line up GD's own stop
                // flags and the call count of the physics entry point (processCommands)
                auto* pl = PlayLayer::get();
                snprintf(b, sizeof(b),
                    "post-session heartbeat %d: tick=%lld x=%.1f started=%d over=%d "
                    "paused=%d gdStarted=%d endAnim=%d ourPause=%d probePause=%d "
                    "pcCalls=%lld visitDraws=%lld",
                    s_beats, (long long)g_tick,
                    m_player1 ? m_player1->getPositionX() : -1.f,
                    g_started ? 1 : 0, g_sessionOver ? 1 : 0,
                    pl ? (pl->m_isPaused ? 1 : 0) : -1,
                    pl ? (pl->m_started ? 1 : 0) : -1,
                    pl ? (pl->m_levelEndAnimationStarted ? 1 : 0) : -1,
                    g_paused ? 1 : 0, probe::g_pause ? 1 : 0,
                    g_pcCalls, g_visitDraws);
                writeResult(b);
                flushAll();
            }
        }
        ev("BGL_update_post", dt);
    }

    void visit() {
        // Self-recovery: if the game entered pause by some path, release it (a safety net
        // against callers other than the pauseGame hook). While paused no update arrives,
        // so this can only be noticed on the render side. Must come before the render
        // skip (after it, it would never run with skiprender=1)
        // Not while a person plays a takeover: their pause is a pause (the pause menu, focus lost).
        if (g_started && !g_sessionOver && !takeover::personDriving()) {
            if (auto* pl = PlayLayer::get())
                if (pl->m_isPaused) {
                    ++g_unfocusPauseBlocked;
                    if (g_unfocusPauseBlocked <= 5)
                        writeResult("pause: found the game paused mid-session -> resuming"
                            " (tick=" + std::to_string(g_tick) + ")");
                    if (auto* pause = pl->getChildByType<PauseLayer>(0)) pause->onResume(nullptr);
                }
        }
        // In fast mode, skip rendering the game layer entirely.
        // But render in realtime mode (F8) -- so the user can spectate.
        // The HUD is a child of the scene, so it is drawn separately even when skipped here.
        if (renderSuppressed()) {   // fast mode's own skip, or F8
            ++g_visitSkips; return;
        }
        // A section search in flight owns the world: it restores a checkpoint tens of thousands
        // of times and update() returns before updateVisibility, so the visibility state the
        // draw would walk is both stale and being rewritten underneath it -- the hazard the SEH
        // guard below exists for, once per frame for the length of the search. The HUD hangs off
        // the SCENE and is drawn either way, which is what the operator needs to see here.
        if (secsolve::inFlight()) { ++g_visitSkips; return; }
        if (g_endzoneBurn) { ++g_visitSkips; return; }   // burning the end animation (see below)
        // A seek's screen-off stretch (cfg seekblind), and the frame it ended on: the visibility
        // state stays stale until the checkpoint load on the next frame resets the level.
        if (itermap::g_seekBlindNow || (itermap::g_seekBlindDone && itermap::g_seekRestart)) {
            ++g_visitSkips; return;
        }
        ++g_visitDraws;
        hookdepth::Guard hg(hookdepth::VISIT);
        // The first visit after a render skip walks over a stale visibility state, so it
        // goes through an SEH guard (render preparation only; unrelated to physics or
        // determinism)
        const bool seekCounts = itermap::seeking();
        const auto t0 = seekCounts ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        safeVisit();
        if (seekCounts)
            itermap::g_seekDrawMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
    }

    void safeVisit() {
#ifdef GEODE_IS_WINDOWS
        __try {
            GJBaseGameLayer::visit();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            logVisitCrashSwallowed();
        }
#else
        // No SEH on Android: the draw runs unguarded, and a fault in it ends the game.
        GJBaseGameLayer::visit();
#endif
    }

    // ---- Section-limited GD solver (read the design notes in src/solver/secsolve.hpp) ----
    // Breadth-first per layer. Each node is represented as "the input sequence from the
    // section head", and on every expansion the state is rebuilt by: restore to the
    // section checkpoint -> replay the prefix -> branch. Not keeping a checkpoint per layer
    // avoids the "hold a huge number of full object states" that caused the old
    // implementation's OOM. Replaying the prefix costs about 0.007ms per tick, cheap
    // compared with a restore (2.02ms).
    void secRestore(PlayLayer* pl) { secRestoreFrom(pl, g_ckpt); }

    // Rebuild a held button without an edge.
    //
    // Why pushButton (= handleButton(true)) is not called: the "action" of the press is
    // already in the restored state. Calling it would be a second firing.
    // For ship / wave the press is a sustained quantity so the second firing is invisible,
    // but for spider the press is the teleport itself, so that one call makes the
    // trajectory a different thing.
    // Measured (lv22, depth 128 from t=1664, checkpoint path): the search exit was
    // x=3106.5 while the plain replay gave x=2143.5. y and vy matched exactly at every
    // depth, and the only split was the last 1 tick of the only run of consecutive 1s in
    // the input sequence (`secsolve_split`).
    // Dash save and restore (see the note on DashState). psnap does not carry it,
    // and the CHECKPOINT DOES -- but its restore never gets there:
    // PlayerObject::saveToCheckpoint writes the dash fields whenever m_isDashing
    // is set, and loadFromCheckpoint gates reading them back on
    // `m_jumpBuffered != 0`, which PlayLayer::loadFromCheckpoint computes from
    // "is a touch or key held in the UI layer RIGHT NOW". A bot has no held UI
    // input at the moment of a restore, so the gate is shut and the saved dash
    // is dropped. Setting the gate
    // ahead of time does not help either -- loadFromCheckpoint overwrites the
    // field from the UI before it reads it. So it is written back here.
    void secCaptureDash(secsolve::DashState& d, PlayerObject* who = nullptr) {
        auto* p = who ? who : m_player1;
        if (!p) { d = secsolve::DashState{}; return; }
        d.on = p->m_isDashing;
        d.ring = reinterpret_cast<GameObject*>(p->m_dashRing);
        d.x = p->m_dashX;
        d.y = p->m_dashY;
        d.angle = p->m_dashAngle;
        d.startTime = p->m_dashStartTime;
        d.lastGround = p->m_lastGroundObject;
        d.maybeLastGround = p->m_maybeLastGroundObject;
        d.preLastGround = p->m_preLastGroundObject;
        d.collided = p->m_collidedObject;
        d.collideLeft = p->m_collidingWithLeft;
        d.collideRight = p->m_collidingWithRight;
        d.slope = p->m_currentSlope;
        d.slope2 = p->m_currentSlope2;
        d.potentialSlope = p->m_currentPotentialSlope;
        d.snappedTo = p->m_objectSnappedTo;
        d.lastPortal = p->m_lastActivatedPortal;
        d.jumpBuf = (int8_t)(p->m_jumpBuffered ? 1 : 0);
    }
    void secRestoreDash(const secsolve::DashState& d, PlayerObject* who = nullptr) {
        auto* p = who ? who : m_player1;
        if (!p) return;
        p->m_isDashing = d.on;
        p->m_dashRing = reinterpret_cast<DashRingObject*>(d.ring);
        p->m_dashX = d.x;
        p->m_dashY = d.y;
        p->m_dashAngle = d.angle;
        p->m_dashStartTime = d.startTime;
        p->m_lastGroundObject = d.lastGround;
        p->m_maybeLastGroundObject = d.maybeLastGround;
        p->m_preLastGroundObject = d.preLastGround;
        p->m_collidedObject = d.collided;
        p->m_collidingWithLeft = d.collideLeft;
        p->m_collidingWithRight = d.collideRight;
        p->m_currentSlope = d.slope;
        p->m_currentSlope2 = d.slope2;
        p->m_currentPotentialSlope = d.potentialSlope;
        p->m_objectSnappedTo = d.snappedTo;
        p->m_lastActivatedPortal = d.lastPortal;
    }
    // cfg secjbkeep: the point's own m_jumpBuffered, written after secArmHold has raised it (see
    // secsolve::g_jbKeep). Only the section search's own restores call this.
    void secRestoreJb(const secsolve::DashState& d, PlayerObject* who = nullptr) {
        auto* p = who ? who : m_player1;
        if (!secsolve::g_jbKeep || !p || d.jumpBuf < 0) return;
        p->m_jumpBuffered = d.jumpBuf != 0;
    }

    // Player 2's share of the per-node extras (cfg secp2extras, secsolve::g_dash2 and the rest):
    // taken wherever player 1's are and written back wherever theirs are. Outside a dual section
    // GD does not move p2, so what is put back is what is already there.
    PlayerObject* secP2() {
        return (secsolve::g_p2Extras && m_player2 && m_player2 != m_player1) ? m_player2 : nullptr;
    }
    // Player 2 while a dual section has it (secsolve::g_p2Key), else null.
    PlayerObject* secDualP2() {
        return (m_gameState.m_isDualMode && m_player2 && m_player2 != m_player1) ? m_player2
                                                                                 : nullptr;
    }
    float secDualY2() {
        auto* q = secDualP2();
        return q ? (float)psnap::physPosition(q, this).y : 0.f;
    }
    void secPushP2() {
        auto* q = secP2();
        secsolve::g_dash2.emplace_back();
        if (q) secCaptureDash(secsolve::g_dash2.back(), q);
        secsolve::g_vy2.push_back(q ? q->m_yVelocity : 0.0);
        secsolve::g_accel2.push_back(q ? q->m_accelerationOrSpeed : 0.0);
        secsolve::g_pad2.push_back(q && q->m_touchedPad ? 1 : 0);
    }
    // `vyToo`: the checkpoint path, whose restore puts the velocity on the 0.001 grid (psnap
    // carries it).
    void secRestoreP2(size_t ni, bool vyToo) {
        auto* q = secP2();
        if (!q || ni >= secsolve::g_vy2.size()) return;
        secRestoreDash(secsolve::g_dash2[ni], q);
        secRestoreJb(secsolve::g_dash2[ni], q);
        if (vyToo) q->m_yVelocity = secsolve::g_vy2[ni];
        q->m_accelerationOrSpeed = secsolve::g_accel2[ni];
        q->m_touchedPad = secsolve::g_pad2[ni] != 0;
    }

    // m_holdingButtons alone is not enough to restore a held button.
    // m_jumpBuffered must be set too (cfg `secjumpbuf=0` turns this off; default ON).
    //
    // History (2026-08-13, the lv22 rotated-section wall at t=12,029 / x=16,165):
    // the input sequence the section solver bought died in plain replay. The checkpoint
    // restore is bit-exact (psnapDrift=0.00), yet `secsolve_verify: FAIL deadAt=401`.
    // The split was
    // `secsolve_split: depth=262/600 x 16131.000->16128.987 vy 0.000->-8.944`:
    // only the replay side performed a mini-cube jump (8.944 = 11.18x0.8).
    // The input sequence at that depth has index 250..266 all 1 = held.
    // Measured the same day: a cube that lands while still held re-jumps
    // (lv1: press once and never release -> vy=11.18 at t=61 and t=165). The restored
    // state only had m_holdingButtons set, so that re-jump did not happen.
    //
    // Same-condition A/B (same plan, same entry, same cap, checkpoint):
    //   secjumpbuf=0 -> UNVERIFIED  verify FAIL deadAt=401  split depth=262
    //   secjumpbuf=1 -> SOLVED      verify OK deadAt=-1  dy=dvy=dx=0.0000
    //                               split depth=-1 (the split disappeared)
    // The worry that a false solution slips through is covered by verify: that check only
    // passes when "search transition = plain replay". It is switchable only for A/B.
    void secArmHold() {
        auto arm = [](PlayerObject* p) {
            if (!p) return;
            p->m_holdingButtons[1] = true;
            if (secsolve::g_jumpBuf) p->m_jumpBuffered = true;
        };
        arm(m_player1);
        arm(m_player2);
        secsolve::g_held = 1;
    }

    // heldAfter: whether the button was held at the moment of the restore. The checkpoint
    // was taken in that state, so a branch that advances to the next tick while still
    // holding must pass 1. Leaving it 0 means "release and press again", i.e. searching a
    // different input sequence from the plain replay.
    void secRestoreFrom(PlayLayer* pl, CheckpointObject* cp, int heldAfter = 0) {
    const SecRestTimer restTimer;
        clearCheckpointsUnsectioned(pl);
        {
            const auto s0 = std::chrono::steady_clock::now();
            pl->storeCheckpoint(cp);
            g_secStoreMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - s0).count();
        }
        g_restorePending = true;
        {
            const auto r0 = std::chrono::steady_clock::now();
            pl->resetLevel();
            g_secResetMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - r0).count();
        }
        g_restorePending = false;
        // The button state after a restore is not necessarily "released". Assuming so and
        // setting g_held=0 means that, while actually still held, no difference shows up,
        // handleButton is not called, and the wave flies the wrong way. Release explicitly
        // first, then put it into a known state.
        // resetLevel pushes a button command of its own on EVERY restore --
        // {button=1, push=<is the UI holding?>, ts=0} -- and a bot's UI holds
        // nothing, so what it pushes is a RELEASE. It is consumed on the first
        // real substep after the restore, AFTER anything injected here, so the
        // hold this function is about to arm is stripped before the physics
        // reads it, and g_held stays 1 so nothing re-presses.
        //
        // In a plain replay that happens once per attempt and is invisible. In
        // a search that restores ONCE PER TICK it happens every tick, which is
        // why a section with a sustained hold cannot be crossed: measured, the
        // spine tracks the head run to 0.05 px for 158 layers and comes apart
        // three ticks into lv22's 16-tick hold at t=5,544.
        //
        // Drained here and nowhere else: the plain replay path keeps GD's own
        // behaviour, because changing that would change what every recorded
        // run means.
        //
        // IT DOES NOT EXPLAIN THE DRIFT IT WAS AIMED AT. With the drain in, the
        // spine still tracks to exactly depth 158 and comes apart with the same
        // 0.126 px -- so either the event is not the one that strips the hold,
        // or the hold is lost some other way. Kept because the mechanism is
        // real (the decompilation has resetLevel pushing it unconditionally and
        // a bot's UI holding nothing), and because draining it cannot make a
        // section restore less faithful. Not kept as an explanation.
        pl->m_queuedButtons.clear();
        g_injecting = true;
        this->handleButton(false, 1, true);
        g_injecting = false;
        secsolve::g_held = 0;
        if (heldAfter) secArmHold();
        // cfg secheaddash: back at the head, each body's dash as the plain flight had it (the
        // load never carries one); the head's DashState is then taken from this
        if (secsolve::g_headDash && cp == g_ckpt) {
            secRestoreDash(secsolve::g_ckptDash);
            if (auto* q = secP2()) secRestoreDash(secsolve::g_ckptDash2, q);
        }
        // cfg secjbkeep: back at the head, each body's m_jumpBuffered as the plain flight had it
        if (secsolve::g_jbKeep && cp == g_ckpt) {
            if (m_player1 && secsolve::g_headJb[0] >= 0)
                m_player1->m_jumpBuffered = secsolve::g_headJb[0] != 0;
            if (m_player2 && m_player2 != m_player1 && secsolve::g_headJb[1] >= 0)
                m_player2->m_jumpBuffered = secsolve::g_headJb[1] != 0;
        }
        // cfg secforcesize: back at the head, at the size asked for. (Where the search then puts
        // the plain flight's player over the load, it forces the size again after it.)
        if (secsolve::g_forceP1Size >= 0 && cp == g_ckpt && m_player1
            && (m_player1->m_vehicleSize < 0.9f) != (secsolve::g_forceP1Size == 1))
            m_player1->togglePlayerScale(secsolve::g_forceP1Size == 1, true);
    }

    // Advance 1 tick. The input is held by secsolve::g_feed (applied on the
    // processCommands side).
    //
    // dt must be 1/240. The physics runs at 240Hz; passing the default fastdt=1/60
    // advances 4 substeps per call. Not noticing that at first, the x right after a
    // restore advancing 5.193 (= 4 x 1.298) per tick was misread as "the restore is
    // broken". The input granularity also becomes 4 ticks, changing the meaning of the
    // search.
    void secStep(int down, float) {
        secsolve::g_feed = down;
        GJBaseGameLayer::update((float)secsolve::g_dt);
    }

    // An end the frame raised between a search's slices. checkForEnd (0x3b4cf0) latches the
    // attempt's end at layer+0x3878 and starts the end animation (0x3aba00, its only caller), which
    // raises m_levelEndAnimationStarted (+0x3570) and schedules levelComplete as its last action,
    // frames later. GD asks checkForEnd once a frame, not once a physics step, so a search step
    // never does -- the frame between two slices did, on whatever the search had left in the
    // player, and its completion was taken for the session's clear. Measured on a custom level:
    // the plan filed was the one the search had started from, which died short of the end, and
    // the slice's flight on the level itself was skipped. (Judging a branch that reaches the end
    // inside the search was tried, by asking checkForEnd after every step, and never fired where
    // the frame did; what the frame's end test reads that the step does not is not known yet.)
    //
    // So before every search step both bytes go back down -- GD skips processCommands while
    // +0x3570 is up, which would freeze every branch after it. The completion the animation
    // raises later then arrives with the byte down, or during the search, and is refused (the
    // levelComplete hook).
    void secStepBegin(PlayLayer* pl) {
        static_assert(offsetof(PlayLayer, m_levelEndAnimationStarted) == gdoff::kLayerLevelEndAnimStarted,
                      "the byte 0x3aba00 raises and pauseGame tests first");
        if (!pl || !pl->m_levelEndAnimationStarted) return;
        pl->m_levelEndAnimationStarted = false;
        *(reinterpret_cast<uint8_t*>(pl) + gdoff::kPlayLayerEndLatch) = 0;   // checkForEnd's latch
    }

    // A checkpoint taken where the physics stands. After a secStep during a mirror transition the
    // players' nodes sit at their screen-flipped drawing places (psnap::physPosition), and a
    // checkpoint stores the node: restored, the drawing offset became physics -- on a custom level the
    // checkpoint path jumped ~150 px on the tick after the mirror portal. Put the nodes on the
    // physics position for the call and back afterwards; outside a transition nothing moves.
    // `createOnly`: the checkpoint is created and not stored (createCheckpoint instead of
    // markCheckpoint, for the human takeover -- see takeoverMark).
    CheckpointObject* markCheckpointAtPhys(PlayLayer* pl, bool createOnly = false) {
        PlayerObject* ps[2] = {m_player1, m_player2};
        cocos2d::CCPoint drawn[2];
        bool moved[2] = {false, false};
        for (int i = 0; i < 2; ++i) {
            if (!ps[i]) continue;
            const cocos2d::CCPoint at = psnap::physPosition(ps[i], this);
            drawn[i] = ps[i]->getPosition();
            if (at.x != drawn[i].x || at.y != drawn[i].y) {
                ps[i]->setPosition(at);
                moved[i] = true;
            }
        }
        CheckpointObject* cp = createOnly ? pl->createCheckpoint() : pl->markCheckpoint();
        for (int i = 0; i < 2; ++i)
            if (moved[i]) ps[i]->setPosition(drawn[i]);
        return cp;
    }

    // ---- cfg cpflight: loop flights from a checkpoint (mod/cp_flight.hpp) ----

    // The game events our own checkpoints raise, dropped (see cpflight::g_inTake).
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if ((cpflight::g_inTake && event == GJGameEvent::Checkpoint)
            || (cpflight::g_starting && event == GJGameEvent::CheckpointRespawn)
            || (takeover::g_restoring && event == GJGameEvent::CheckpointRespawn)
            || (seekcp::g_restoring && event == GJGameEvent::CheckpointRespawn)) {
            ++cpflight::g_eventsDropped;
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    // After every update call: a checkpoint when one is due, and the end of a probe that has outlived
    // the flight it repeats.
    void cpAfterUpdate() {
        // cfg objwatch=<uid>[,<uid>...] (print only): each watched object's toggle and section
        // membership, printed when either changes, with the tick and the run.
        if (!itermap::g_objWatch.empty() && m_objects && g_started && !g_sessionOver) {
            static const void* s_layer = nullptr;
            static std::vector<std::pair<GameObject*, int>> s_objs;   // object, last (disabled | in-section<<1)
            if (s_layer != this) {
                s_layer = this;
                s_objs.clear();
                for (auto* o : CCArrayExt<GameObject*>(m_objects))
                    if (o && std::find(itermap::g_objWatch.begin(), itermap::g_objWatch.end(), o->m_uniqueID)
                                 != itermap::g_objWatch.end())
                        s_objs.emplace_back(o, -1);
            }
            // ...with the two bytes staticObjectsInRect (0x211260) tests before it takes an object --
            // +0x305 (taken whatever its groups) and +0x28e (taken by an enabledGroups query only when 0)
            // -- and the object's slot in its section against the section's live count (layer+0x3658),
            // which bounds the query's walk.
            for (auto& [o, last] : s_objs) {
                const auto* ob = reinterpret_cast<const uint8_t*>(o);
                const int b305 = ob[0x305], b28e = ob[0x28e];
                // the slot and the live count of its section (removeObjectFromSection: +0x278 / +0x27c /
                // +0x270; staticObjectsInRect: counts at layer+0x3658, sections at layer+0x35b0)
                const int outer = *reinterpret_cast<const int*>(ob + 0x278);
                const int inner = *reinterpret_cast<const int*>(ob + 0x27c);
                const int slot = *reinterpret_cast<const int*>(ob + 0x270);
                int liveCount = -1;
                {
                    const auto* lb = reinterpret_cast<const uint8_t*>(this);
                    const long long xs = (*reinterpret_cast<const long long*>(lb + gdoff::kLayerNonEffectObjects + 8)
                                          - *reinterpret_cast<const long long*>(lb + gdoff::kLayerNonEffectObjects)) >> 3;
                    const long long cv = *reinterpret_cast<const long long*>(lb + gdoff::kLayerNonEffectObjectsSizes);
                    if (outer >= 0 && inner >= 0 && outer < xs && cv) {
                        const long long vp = *reinterpret_cast<const long long*>(cv + (long long)outer * 8);
                        if (vp) {
                            const long long d0 = *reinterpret_cast<const long long*>(vp);
                            const long long d1 = *reinterpret_cast<const long long*>(vp + 8);
                            if (d0 && inner < (d1 - d0) / 4) liveCount = *reinterpret_cast<const int*>(d0 + (long long)inner * 4);
                        }
                    }
                }
                const int now = (o->m_isGroupDisabled ? 1 : 0) | (o->m_outerSectionIndex >= 0 ? 2 : 0)
                                | (b305 ? 4 : 0) | (b28e ? 8 : 0) | ((slot < liveCount ? 1 : 0) << 4);
                if (now == last) continue;
                last = now;
                writeResult("objwatch: t=" + std::to_string((long long)g_tick) + " uid=" + std::to_string(o->m_uniqueID)
                            + " disabled=" + std::to_string(now & 1) + " sec=" + std::to_string(o->m_outerSectionIndex)
                            + " b305=" + std::to_string(b305) + " b28e=" + std::to_string(b28e)
                            + " sec=(" + std::to_string(outer) + "," + std::to_string(inner) + ") slot=" + std::to_string(slot)
                            + " live=" + std::to_string(liveCount)
                            + " run=" + (seekcp::g_from < 0 ? std::string("head") : "cp@" + std::to_string(seekcp::g_from)));
            }
        }        // cfg batchlog (print only): the ticks each update call ran, (end of the last call, now]
        {
            static long long s_prevEnd = -1;
            if (itermap::g_batchFrom >= 0 && g_started && !g_sessionOver && g_tick >= itermap::g_batchFrom
                && s_prevEnd <= itermap::g_batchTo) {
                char b[200];
                snprintf(b, sizeof(b), "batch: update ran t=%lld..%lld extraDelta=%.17g run=%s%lld", s_prevEnd + 1,
                         (long long)g_tick, m_extraDelta, seekcp::g_from < 0 ? "head" : "cp@",
                         seekcp::g_from < 0 ? 0LL : seekcp::g_from);
                writeResult(b);
            }
            s_prevEnd = g_tick;
        }
        // cfg cpflightlayerdiff: the layer's and player 1's bytes after the calls that follow a
        // checkpoint, recorded by the flight that took it and compared by a flight restored from it
        if (g_cfg.cpFlightLayerDiff && m_player1 && cpflight::g_diffStep < cpflight::kDiffSteps) {
            auto* pl = PlayLayer::get();
            const auto* lb = reinterpret_cast<const uint8_t*>(pl);
            const auto* pb = reinterpret_cast<const uint8_t*>(m_player1);
            if (pl && cpflight::g_diffCollect) {
                auto& r = *cpflight::g_diffCollect;
                r.afterLayer.emplace_back(lb, lb + sizeof(PlayLayer));
                r.afterPlayer.emplace_back(pb, pb + sizeof(PlayerObject));
                r.afterTick.push_back(g_tick);
                r.afterTweens.push_back(cpflight::tweensLine(this));
                if (++cpflight::g_diffStep >= cpflight::kDiffSteps) cpflight::g_diffCollect.reset();
            } else if (pl && cpflight::g_diffCompare) {
                auto& r = *cpflight::g_diffCompare;
                const size_t k = (size_t)cpflight::g_diffStep;
                if (k < r.afterLayer.size() && r.afterTick[k] == g_tick) {
                    cpflight::layerDiff(r.afterLayer[k], pl, g_tick);
                    std::vector<uint8_t> tmp = r.afterPlayer[k];
                    writeResult("cpflight: playerdiff follows (offsets from player 1)");
                    cpflight::layerDiff(tmp, m_player1, g_tick);
                    const std::string tw = cpflight::tweensLine(this);
                    if (k < r.afterTweens.size() && tw != r.afterTweens[k])
                        writeResult("cpflight: tweens t=" + std::to_string(g_tick) + " flight:" + r.afterTweens[k]
                                    + " | restored:" + tw);
                    else
                        writeResult("cpflight: tweens t=" + std::to_string(g_tick) + " same:" + tw);
                }
                if (++cpflight::g_diffStep >= cpflight::kDiffSteps) cpflight::g_diffCompare.reset();
            }
        }
        if (cpflight::g_objAfter > 0) {
            --cpflight::g_objAfter;
            cpflight::objLine(this, "after-update", g_tick);
        } else if (g_cfg.cpFlightObjFrom >= 0 && g_tick >= g_cfg.cpFlightObjFrom && g_tick <= g_cfg.cpFlightObjTo) {
            // cfg cpflightobjfrom / cpflightobjto: every update call in that tick range, in any flight
            cpflight::objLine(this, ("tick att=" + std::to_string(g_attempt)).c_str(), g_tick);
        }
        if (cpflight::g_next >= 0 && g_tick >= cpflight::g_next)
            if (auto* pl = PlayLayer::get(); pl && static_cast<GJBaseGameLayer*>(pl) == this) cpTake(pl);
        seekCpAfterUpdate();
        cpflight::nodeWatch(this, g_tick, nullptr);
        // the probe's control at the boundary where the probe's checkpoint was taken: its carry state,
        // for the comparison with the probe's right after the restore (cpflight::compareCarryFull)
        if (cpflight::g_controlFlying && !cpflight::g_controlAtT.have && cpflight::g_probeCap.from >= 0
            && g_tick >= cpflight::g_probeCap.from)
            if (auto* pl = PlayLayer::get(); pl && static_cast<GJBaseGameLayer*>(pl) == this)
                cpflight::g_controlAtT = cpflight::captureCarryFull(pl, g_tick);
        if (cpflight::g_probeFlying && cpflight::g_probeSet
            && g_tick > cpflight::g_probeSet->end + cpflight::kProbeOverrun) {
            writeResult(cpflight::probeCapture(g_tick, m_player1 ? m_player1->getPositionX() : -1.f,
                                               "outlived its source"));
            g_paused = true;
        } else if (cpflight::g_controlFlying && g_tick > cpflight::g_probeCap.end + cpflight::kProbeOverrun) {
            writeResult(cpflight::controlEnd(g_tick, m_player1 ? m_player1->getPositionX() : -1.f,
                                             "outlived the probe"));
            g_paused = true;
        }
    }

    // ---- the strict checkpoint: shared by the loop's flights and the human takeover ----
    //
    // What a restore needs beyond GD's own checkpoint (cp_flight.hpp names each gap these close).
    // One definition for both users, so a gap closed for the loop's flights is closed for the
    // takeover too.

    // cfg cpfollowsection: the layer's Follow Player Y objects -- everything in a group some 1814
    // targets -- worked out once per layer (its object array does not change while it lives).
    std::vector<GameObject*>& followSectObjects() {
        static const void* layerOf = nullptr;
        static const void* arrayOf = nullptr;
        static size_t countOf = 0;
        static std::vector<GameObject*> objs;
        const size_t n = m_objects ? (size_t)m_objects->count() : 0;
        if (layerOf == this && arrayOf == m_objects && countOf == n) return objs;
        layerOf = this;
        arrayOf = m_objects;
        countOf = n;
        objs.clear();
        if (!m_objects) return objs;
        std::unordered_set<int> groups;
        for (auto* o : CCArrayExt<GameObject*>(m_objects))
            if (o && o->m_objectID == 1814)
                if (auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(o); e && e->m_targetGroupID > 0)
                    groups.insert(e->m_targetGroupID);
        if (groups.empty()) return objs;
        for (auto* o : CCArrayExt<GameObject*>(m_objects)) {
            if (!o || o->m_groupCount <= 0 || !o->m_groups) continue;
            for (int i = 0; i < std::min((int)o->m_groupCount, 10); ++i)
                if (groups.count((*o->m_groups)[i])) {
                    objs.push_back(o);
                    break;
                }
        }
        return objs;
    }
    // The object's collision section cell, x and y (GJBaseGameLayer::updateObjectSection 0x227f50
    // compares (int)(scale * m_positionX / Y) with these two and re-buckets on a difference).
    static int& sectionCellX(GameObject* o) { return *reinterpret_cast<int*>(reinterpret_cast<char*>(o) + 0x278); }
    static int& sectionCellY(GameObject* o) { return *reinterpret_cast<int*>(reinterpret_cast<char*>(o) + 0x27c); }

    // The state GD's checkpoint does not carry, taken where the physics stands.
    void cpCaptureState(PlayLayer* pl, cpflight::Rec& r) {
        psnap::captureRaw(m_player1, this, r.player);
        secCaptureDash(r.dash);
        if (m_player2) secCaptureDash(r.dash2, m_player2);
        psnap::captureTouch(m_player1, r.touch, this);
        r.damageVerified = pl->m_damageVerified;
        {
            auto hb = m_player1->m_holdingButtons.find(1);
            r.held = (hb != m_player1->m_holdingButtons.end() && hb->second) ? 1 : 0;
        }
        r.oobLatch = (int)(unsigned char)reinterpret_cast<const char*>(m_player1)[kOobLatchOff];
        r.nextInput = g_nextInput;
        r.nextToggle = g_nextToggle;
        {
            auto* base = reinterpret_cast<unsigned char*>(geode::base::get());
            r.seedTrig = *reinterpret_cast<long long*>(base + gdoff::kSeedTriggerRva);
            r.seedVarIndex = *reinterpret_cast<long long*>(base + gdoff::kSeedVarIndexRva);
            r.seedVarTable = *reinterpret_cast<long long*>(base + gdoff::kSeedVarTableRva);
            r.seedKept = *reinterpret_cast<long long*>(reinterpret_cast<char*>(this)
                                                       + gdoff::kLayerRandomSeed);
        }
        cpflight::captureCamera(this, r.camera);
        cpflight::captureArea(this, r.area);
        // cfg cpfollowsection: where the flight has each Follow Player Y object bucketed -- the
        // flight's own history of re-buckets (Move triggers do, the follow does not), which is a run
        // from the head's as long as the flights before it kept to that too
        r.followCells.clear();
        if (g_cfg.cpFollowSection)
            for (GameObject* o : followSectObjects())
                r.followCells.push_back({o, sectionCellX(o), sectionCellY(o)});
    }

    // GD's own checkpoint for the record, taken out of GD's list at once so that no reset of GD's
    // own can load it. False: GD gave none (markCheckpoint returned 0).
    bool cpMark(PlayLayer* pl, cpflight::Rec& r) {
        cpflight::g_inTake = true;
        CheckpointObject* cp = markCheckpointAtPhys(pl);
        cpflight::g_inTake = false;
        if (!cp) return false;
        cp->retain();
        r.cp = cp;
        r.layer = pl;
        r.objects = pl->m_objects ? (size_t)pl->m_objects->count() : 0;
        clearCheckpointsUnsectioned(pl);
        return true;
    }

    // After the reset that loaded the record's checkpoint: what that load does not put back. The
    // caller stored the checkpoint and ran the reset; the cursors and the tick are the caller's.
    // `diag`: the loop's cfg cpflightobj line between the camera and the objects.
    void cpRestoreState(PlayLayer* pl, const cpflight::Rec& r, bool diag) {
        clearCheckpointsUnsectioned(pl);
        pl->m_queuedButtons.clear();
        if (r.held) {
            m_player1->m_holdingButtons[1] = true;
            if (m_player2) m_player2->m_holdingButtons[1] = true;
        }
        psnap::restore(m_player1, this, r.player.data());
        secRestoreDash(r.dash);
        if (m_player2) secRestoreDash(r.dash2, m_player2);
        psnap::restoreTouch(m_player1, r.touch, this);
        // ...and the two object pointers restoreTouch leaves to cfg secsnapobj: the object the
        // player is snapped to and the one it collided with. A restore of the loop is on the same
        // layer, whose objects are never re-created, so they are carried. Measured on lv22 at t=600
        // without them: the rows after the restore took a different m_snapDistance from t=602.
        m_player1->m_objectSnappedTo = r.touch.objectSnappedTo;
        m_player1->m_collidedObject = r.touch.collidedObject;
        if (m_player2 && r.touch.second) {
            m_player2->m_objectSnappedTo = r.touch.second->objectSnappedTo;
            m_player2->m_collidedObject = r.touch.second->collidedObject;
        }
        pl->m_damageVerified = r.damageVerified;
        reinterpret_cast<char*>(m_player1)[kOobLatchOff] = (char)r.oobLatch;
        {
            auto* base = reinterpret_cast<unsigned char*>(geode::base::get());
            *reinterpret_cast<long long*>(base + gdoff::kSeedTriggerRva) = r.seedTrig;
            *reinterpret_cast<long long*>(base + gdoff::kSeedVarIndexRva) = r.seedVarIndex;
            *reinterpret_cast<long long*>(base + gdoff::kSeedVarTableRva) = r.seedVarTable;
            *reinterpret_cast<long long*>(reinterpret_cast<char*>(this) + gdoff::kLayerRandomSeed) =
                r.seedKept;
        }
        cpflight::restoreCamera(this, r.camera);
        if (diag) cpflight::objLine(this, "reset", r.tick);
        cpflight::restoreArea(this, r.area);
        // cfg groupcheck (print only): each object's toggle and section membership against what the
        // flight had at the checkpoint (seekcp records them at the take, g_toggles). The first try at a
        // fix took switched-off objects out of their sections here and crashed the worker:
        // removeObjectFromSection does not clear the object's section index, so GD's own next
        // updateObjectSection removed it a second time (2026-10-06, SubZero 4002). And a head run keeps
        // switched-off objects in sections too (85 of them at 4002's t=20,400), so "off means out" is
        // not GD's rule.
        if (itermap::g_groupCheck) groupToggleDiff(r);
        // cfg cpvisafter (off by default): the visibility pass the restore's reset would have run is
        // run here instead, now that the camera is the flight's (see hooks_playlayer updateVisibility).
        if (g_cfg.cpVisAfter) pl->updateVisibility(0.f);
        // cfg cpfollowsection: the checkpoint load re-buckets every object where it stands; put each
        // Follow Player Y object back in the cell the flight had it in. GD's own re-bucket is the
        // only tool that keeps the section lists and the indices in step (a bare removal crashed a
        // worker on 2026-10-06, the groupcheck note above), so the position is lent for the call:
        // the cell's centre, updateObjectSection, the real position back.
        if (g_cfg.cpFollowSection && !r.followCells.empty()) {
            const float kx = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(this) + gdoff::kLayerSectionSizeX);
            const float ky = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(this) + gdoff::kLayerSectionSizeY);
            int moved = 0, already = 0, outside = 0, notStart = 0;
            if (kx > 0.f && ky > 0.f) {
                for (const auto& fc : r.followCells) {
                    GameObject* o = fc.obj;
                    if (!o || fc.sx < 0 || sectionCellX(o) < 0) { ++outside; continue; }
                    // print only: whether the checkpoint's cell is the load-time one (a Move trigger
                    // re-buckets on its way, so the two part where one moved the object)
                    {
                        auto cellOf = [](double p, float k) {
                            return p <= 0.0 ? 0 : (int)((double)k * std::min(p, 1.0e7));
                        };
                        if (cellOf(o->m_startPosition.x, kx) != fc.sx || cellOf(o->m_startPosition.y, ky) != fc.sy)
                            ++notStart;
                    }
                    if (sectionCellX(o) == fc.sx && sectionCellY(o) == fc.sy) { ++already; continue; }
                    const double px = o->m_positionX, py = o->m_positionY;
                    o->m_positionX = ((double)fc.sx + 0.5) / (double)kx;
                    o->m_positionY = ((double)fc.sy + 0.5) / (double)ky;
                    this->updateObjectSection(o);
                    o->m_positionX = px;
                    o->m_positionY = py;
                    ++moved;
                }
            }
            char b[256];
            snprintf(b, sizeof(b), "cpfollowsection: the restore at t=%lld put %d of %zu Follow Player Y "
                     "object(s) back in their checkpoint cells (%d already there, %d outside any section; "
                     "%d checkpoint cell(s) are not the load-time one)",
                     r.tick, moved, r.followCells.size(), already, outside, notStart);
            writeResult(b);
        }
    }

    // The objects whose toggle (m_isGroupDisabled) or section membership (m_outerSectionIndex >= 0)
    // differs from the record taken with the checkpoint: counts and the first uids, each with
    // (disabled, in-section) at the checkpoint and now.
    void groupToggleDiff(const cpflight::Rec& r) {
        auto it = seekcp::g_toggles.find(&r);
        if (it == seekcp::g_toggles.end() || !m_objects) {
            writeResult("groupcheck: no toggle record for the checkpoint at t=" + std::to_string(r.tick));
            return;
        }
        const auto& rec = it->second;
        size_t i = 0;
        int flagDiff = 0, secDiff = 0;
        std::string s;
        for (auto* o : CCArrayExt<GameObject*>(m_objects)) {
            if (i >= rec.size()) break;
            const uint8_t was = rec[i++];
            if (!o) continue;
            const uint8_t now = (uint8_t)((o->m_isGroupDisabled ? 1 : 0) | (o->m_outerSectionIndex >= 0 ? 2 : 0));
            if (now == was) continue;
            if ((now ^ was) & 1) ++flagDiff;
            if ((now ^ was) & 2) ++secDiff;
            if (flagDiff + secDiff <= 16)
                s += " " + std::to_string(o->m_uniqueID) + "(" + std::to_string(was & 1) + std::to_string((was >> 1) & 1)
                     + "->" + std::to_string(now & 1) + std::to_string((now >> 1) & 1) + ")";
        }
        writeResult("groupcheck: after the restore at t=" + std::to_string(r.tick) + " toggle differs on "
                    + std::to_string(flagDiff) + ", section on " + std::to_string(secDiff)
                    + " object(s) [uid(disabled,in-section) at the checkpoint -> now]:" + s);
    }

    // cfg groupcheck (print only): the objects whose section membership disagrees with their toggle --
    // switched off but in a section, and switched on but in none -- with the first few uids of each.
    void groupSectionCheck(const char* when, long long t) {
        if (!m_objects) return;
        int offIn = 0, onOut = 0;
        std::string offUids, onUids;
        for (auto* o : CCArrayExt<GameObject*>(m_objects)) {
            if (!o) continue;
            const bool in = o->m_outerSectionIndex >= 0;
            if (o->m_isGroupDisabled && in) {
                if (++offIn <= 8) offUids += " " + std::to_string(o->m_uniqueID);
            } else if (!o->m_isGroupDisabled && !in) {
                if (++onOut <= 8) onUids += " " + std::to_string(o->m_uniqueID);
            }
        }
        writeResult("groupcheck: " + std::string(when) + " t=" + std::to_string(t) + " run="
                    + (seekcp::g_from < 0 ? std::string("head") : "cp@" + std::to_string(seekcp::g_from))
                    + " off-but-in-a-section=" + std::to_string(offIn) + offUids
                    + " | on-but-in-none=" + std::to_string(onOut) + onUids);
    }

    // ---- the human takeover (mod/takeover.hpp) ----

    // The game's checkpoint for a takeover record: created where the physics stands and NOT stored.
    // cpMark's markCheckpoint also clears the queued Z press (PlayLayer+0x3a64) and dims the previous
    // checkpoint's marker -- the game's own practice state, which the person is using -- so the
    // takeover asks only for the checkpoint itself (createCheckpoint, which tryPlaceCheckpoint also
    // calls on its own). It is stored by the restore that loads it, as cpStart stores the loop's.
    bool takeoverMark(PlayLayer* pl, cpflight::Rec& r) {
        cpflight::g_inTake = true;
        CheckpointObject* cp = markCheckpointAtPhys(pl, true);
        cpflight::g_inTake = false;
        if (!cp) return false;
        cp->retain();
        r.cp = cp;
        r.layer = pl;
        r.objects = pl->m_objects ? (size_t)pl->m_objects->count() : 0;
        return true;
    }

#ifdef GEODE_IS_WINDOWS
    // The members the takeover reads by name, at the offsets the disassembly reads them at
    // (tryPlaceCheckpoint 0x3a32d0 keeps the pending checkpoint at player+0x890; handleButton
    // 0x2338e0 reads two-player mode at levelSettings+0x154).
    static_assert(offsetof(PlayerObject, m_pendingCheckpoint) == gdoff::kPlayerPendingCheckpoint, "PlayerObject::m_pendingCheckpoint");
    static_assert(offsetof(LevelSettingsObject, m_twoPlayerMode) == 0x154, "LevelSettingsObject::m_twoPlayerMode");
#endif

    // After every update call of a takeover session: the start point's checkpoint once the point is
    // reached; later, a strict checkpoint for each one the game places. Taken between two update
    // calls, where a restore also lands, as the loop's are (cpTake).
    void takeoverAfterUpdate() {
        if (secsolve::g_active) return;
        const bool start = takeover::approaching();
        if (!start && !(takeover::personDriving() && !takeover::g_waiting)) return;
        auto* pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this || !m_player1) return;
        if (pl->m_playerDied || m_player1->m_isDead) {
            if (!start) {   // whatever the attempt that died had asked for goes with it
                takeover::g_cpWanted = false;
                takeover::dropPending();
            } else {
                takeover::g_takeNow = false;   // a key press does not wait for the next attempt
            }
            return;
        }
        if (start) {
            const bool atTick = g_cfg.takeoverAt >= 0 && g_tick >= g_cfg.takeoverAt;
            const bool atX = g_cfg.takeoverX > 0.f && m_player1->getPositionX() >= g_cfg.takeoverX;
            // cfg takeoverkey: the key was pressed (takeover::takeNow), here is the point.
            const bool byKey = takeover::g_takeNow;
            takeover::g_takeNow = false;
            if (atTick || atX || byKey) takeoverTake(pl, takeover::TakeKind::Start);
            return;
        }
        // The game stored its pending checkpoint: the strict one taken when it appeared goes in.
        if (takeover::g_commitPending) takeover::commitPending();
        // The game stored one at once (Z, a checkpoint in the level): a strict one here.
        if (takeover::g_cpWanted) {
            takeover::g_cpWanted = false;
            takeoverTake(pl, takeover::TakeKind::Now);
        }
        // The game began a pending checkpoint (its auto setting), or let one go: the strict one is
        // taken now, at the moment the game judged safe, and kept aside until the game decides.
        const void* pend = m_player1->m_pendingCheckpoint;
        if (pend != takeover::g_pendingSrc) {
            takeover::dropPending();
            if (pend) {
                takeoverTake(pl, takeover::TakeKind::Pending);
                takeover::g_pendingSrc = pend;
            }
        }
    }

    // One strict checkpoint where the physics stands: the start point's, which hands the input
    // over; one for a checkpoint the game stored at once; or one kept aside for its pending one.
    void takeoverTake(PlayLayer* pl, takeover::TakeKind kind) {
        const bool start = kind == takeover::TakeKind::Start;
        const long long T = g_tick;
        takeover::Cp c;
        c.rec = std::make_shared<cpflight::Rec>();
        c.rec->tick = T;
        cpCaptureState(pl, *c.rec);
        if (!takeoverMark(pl, *c.rec)) {
            if (start) {
                takeover::g_done = true;
                takeover::line("refused: GD gave no checkpoint at t=" + std::to_string(T)
                               + " - the mode is off for this session");
            } else {
                takeover::line("no checkpoint: GD gave none at t=" + std::to_string(T));
            }
            return;
        }
        // The inputs an attempt from here flies before the person: those of step < T, which are
        // the ones fed so far (processCommands feeds step <= g_tick and then advances the tick) --
        // the plan's at the start point, then the person's own -- and a release at T if the button
        // was held: the person is not holding the key after a restore, and a hold nobody releases
        // would carry into their run.
        if (start) {
            c.before.assign(g_cfg.inputs.begin(), g_cfg.inputs.begin()
                            + (std::ptrdiff_t)std::min(g_nextInput, g_cfg.inputs.size()));
        } else {
            for (const InputCmd& in : takeover::attemptPlan())
                if (in.step < T) c.before.push_back(in);
        }
        if (c.rec->held) c.before.push_back({(int)T, false});
        c.tick = T;
        const auto pos = m_player1->getPosition();
        c.x = pos.x;
        c.y = pos.y;
        c.vy = (float)m_player1->m_yVelocity;
        c.dumpAttempt = g_attempt;
        c.parent = takeover::g_from;
        c.source = start ? "start" : kind == takeover::TakeKind::Now ? "game" : "auto";
        c.gdSrc = kind == takeover::TakeKind::Now ? takeover::g_wantedSrc
                : kind == takeover::TakeKind::Pending ? (const void*)m_player1->m_pendingCheckpoint
                                                      : nullptr;
        if (kind == takeover::TakeKind::Now) {
            takeover::addCp(std::move(c));
            return;
        }
        if (kind == takeover::TakeKind::Pending) {
            takeover::g_pending = std::move(c);
            takeover::g_hasPending = true;
            return;
        }
        takeover::g_fullInputs = g_cfg.inputs;     // for the replay key's way back (takeoverkey)
        takeover::g_fullToggles = g_cfg.toggles;
        g_cfg.inputs = c.before;
        g_cfg.toggles.resize(std::min(g_nextToggle, g_cfg.toggles.size()));
        // The person plays at the speed the game runs: back to realtime, which is where the
        // approach already is unless the session runs the fast loop (it should not; see the py
        // driver). The screen of a fast-loop session would stay dark.
        if (g_cfg.fastdt > 0 && !g_realtimeOverride) {
            g_realtimeOverride = true;
            takeover::line("warning: the session ran the fast loop - realtime from here, and the "
                           "screen stays as it was");
        }
        takeover::begin(std::move(c));
        // ...and the game's checkpoints from here on are the person's (its auto setting, Z and X),
        // each replaced by a strict one. Practice mode is what turns them on; entering it keeps the
        // run where it is (togglePracticeMode(true) sets the flag, the checkpoint buttons and the
        // practice song, and resets nothing -- only leaving it restarts the level). After the take,
        // so the start point's checkpoint is the plan's world as it was.
        if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);
    }

    // A death while the person plays (or anything else that asked GD for a reset) comes back here
    // at the frame boundary: the newest checkpoint is loaded the way cpStart loads one, and the
    // person's next attempt starts from it.
    void takeoverRestore(PlayLayer* pl) {
        takeover::g_restorePending = false;
        takeover::g_cpWanted = false;
        takeover::dropPending();
        if (!takeover::personDriving() || takeover::g_cps.empty()) return;
        const takeover::Cp& c = takeover::g_cps.back();
        if (!c.rec || !c.rec->cp) return;
        const cpflight::Rec& r = *c.rec;
        // Raised before the store as well as the reset: the store hook takes any store while the
        // person plays for one the game made, and answered this one with a new checkpoint four
        // ticks on -- every restore crept the newest checkpoint forward (measured on lv1: t=2443,
        // 2447, 2451 ... one per death).
        takeover::g_restoring = true;
        clearCheckpointsUnsectioned(pl);
        pl->storeCheckpoint(r.cp);
        g_restorePending = true;
        pl->resetLevel();
        g_restorePending = false;
        takeover::g_restoring = false;
        cpRestoreState(pl, r, false);
        g_cfg.inputs = c.before;
        g_nextToggle = r.nextToggle;
        // The cursor over what this checkpoint's attempts fly before the person: past everything
        // before T, so only a release at T is left to feed. (Not the record's own cursor: a
        // checkpoint the person's run took counted over a list without the person's presses, which
        // GD received directly.)
        g_nextInput = 0;
        while (g_nextInput < g_cfg.inputs.size() && g_cfg.inputs[g_nextInput].step < r.tick)
            ++g_nextInput;
        g_tick = r.tick;
        const auto pos = m_player1->getPosition();
        takeover::restored(c, pos.x, pos.y, (float)m_player1->m_yVelocity);
    }

    // ---- cfg seekcp: the seek bar's checkpoints (mod/seek_cp.hpp) ----

    // cfg `latchat=<tick>` (print only): once per run, at the first update boundary at or past the tick,
    // how many objects carry the rect-path latch (GameObject+0x2e8, m_shouldUseOuterOb) and, within
    // 300 px of player 1, each rotated or latched object with its uid, id, latch and rotation. Reads members
    // only: getObjectRect / getOrientedBox can raise the latch it is reading.
    void latchLine() {
        static long long s_doneFor = -2;   // the run (attempt, seekcp::g_from) it was printed for
        const long long at = itermap::g_latchAt;
        if (at <= 0 || !m_player1 || !m_objects || !g_started || g_sessionOver) return;
        const long long run = (long long)g_attempt * 1000000 + seekcp::g_from;
        if (g_tick < at || s_doneFor == run) return;
        s_doneFor = run;
        const float px = m_player1->getPositionX(), py = m_player1->getPositionY();
        int set = 0;
        std::string nearby;
        for (auto* o : CCArrayExt<GameObject*>(m_objects)) {
            if (!o) continue;
            if (o->m_shouldUseOuterOb) ++set;
            if (std::fabs((float)o->m_positionX - px) > 300.f || std::fabs((float)o->m_positionY - py) > 300.f) continue;
            if (o->getRotation() == 0.f && !o->m_shouldUseOuterOb) continue;
            char b[96];
            snprintf(b, sizeof(b), " %d/%d:L%d r%.1f", o->m_uniqueID, o->m_objectID,
                     o->m_shouldUseOuterOb ? 1 : 0, (double)o->getRotation());
            nearby += b;
        }
        char h[200];
        snprintf(h, sizeof(h), "latch: t=%lld run=%s%lld player=(%.3f,%.3f) latched=%d near(rotated or latched):",
                 (long long)g_tick, seekcp::g_from < 0 ? "head" : "cp@", seekcp::g_from < 0 ? 0LL : seekcp::g_from,
                 (double)px, (double)py, set);
        writeResult(std::string(h) + nearby);
    }

    // cfg `regionat=<tick>` + `regionbox=x0:y0:x1:y1` (print only): once per run, at the first update
    // boundary at or past the tick, every object whose position is in the box, with its on/off flags and
    // its cached rect (m_objectRect as it stands; nothing is recomputed).
    void regionLine() {
        static long long s_doneFor = -2;
        const long long at = itermap::g_regionAt;
        if (at <= 0 || !m_player1 || !m_objects || !g_started || g_sessionOver) return;
        const long long run = (long long)g_attempt * 1000000 + seekcp::g_from;
        if (g_tick < at || s_doneFor == run) return;
        s_doneFor = run;
        const float* bx = itermap::g_regionBox;
        char h[200];
        snprintf(h, sizeof(h), "region: t=%lld run=%s%lld player=(%.3f,%.3f) vy=%.3f box=(%.0f,%.0f)-(%.0f,%.0f)",
                 (long long)g_tick, seekcp::g_from < 0 ? "head" : "cp@", seekcp::g_from < 0 ? 0LL : seekcp::g_from,
                 (double)m_player1->getPositionX(), (double)m_player1->getPositionY(),
                 (double)m_player1->m_yVelocity, (double)bx[0], (double)bx[1], (double)bx[2], (double)bx[3]);
        writeResult(h);
        groupSectionCheck("region", (long long)g_tick);
        for (auto* o : CCArrayExt<GameObject*>(m_objects)) {
            if (!o) continue;
            const double x = o->m_positionX, y = o->m_positionY;
            if (x < bx[0] || x > bx[2] || y < bx[1] || y > bx[3]) continue;
            const auto& r = o->m_objectRect;
            char b[260];
            snprintf(b, sizeof(b), "region:   uid=%d id=%d type=%d pos=(%.3f,%.3f) rect=(%.2f,%.2f %.2fx%.2f) "
                     "gdis=%d gdisT=%d dis=%d dis2=%d inv=%d hide=%d vis=%d rot=%.1f sec=%d",
                     o->m_uniqueID, o->m_objectID, (int)o->m_objectType, x, y, (double)r.origin.x, (double)r.origin.y,
                     (double)r.size.width, (double)r.size.height, (int)o->m_isGroupDisabled,
                     (int)o->m_isGroupDisabledTemp, (int)o->m_isDisabled, (int)o->m_isDisabled2,
                     (int)o->m_isInvisible, (int)o->m_isHide, (int)o->isVisible(), (double)o->getRotation(),
                     (int)o->m_outerSectionIndex);
            writeResult(b);
        }
    }

    // cfg actcensus=1 (print only): after every update call, GD's own collision queries around each
    // player (its rect grown by 60 px, enabledGroups=true, as the hitbox probe asks), and every object
    // they return although its group is switched off -- returned only because it is activated
    // (m_isActivated, +0x305), which is the visibility pass's to set and clear. Counted per run: the
    // update boundaries with at least one such object, and each object's first and last tick
    // (`actcensus:` lines, the summary at the session's end).
    void actCensus() {
        if (!itermap::g_actCensus || !g_started || g_sessionOver || !m_player1) return;
        auto& c = itermap::g_actCensusState;
        bool any = false;
        for (PlayerObject* p : {m_player1, m_player2}) {
            if (!p || (p == m_player2 && !m_gameState.m_isDualMode)) continue;
            cocos2d::CCRect q = p->getObjectRect();
            q.origin.x -= 60.f; q.origin.y -= 60.f; q.size.width += 120.f; q.size.height += 120.f;
            for (int kind = 0; kind < 2; ++kind) {
                cocos2d::CCArray* a = kind == 0 ? staticObjectsInRect(q, true) : damagingObjectsInRect(q, true);
                if (!a) continue;
                for (auto* o : CCArrayExt<GameObject*>(a)) {
                    if (!o || !reinterpret_cast<const uint8_t*>(o)[0x28e]) continue;
                    any = true;
                    auto& e = c.objs[o->m_uniqueID];
                    if (e.first < 0) {
                        e.first = g_tick;
                        e.id = o->m_objectID;
                        e.kind = kind;
                        if (c.objs.size() <= 40)
                            writeResult("actcensus: t=" + std::to_string((long long)g_tick) + " uid=" + std::to_string(o->m_uniqueID)
                                        + " id=" + std::to_string(o->m_objectID) + (kind == 0 ? " solid" : " hazard")
                                        + " is switched off and activated near the player");
                    }
                    e.last = g_tick;
                    ++e.boundaries;
                }
            }
        }
        if (any) ++c.boundaries;
        ++c.checked;
    }

    // After every update call of a plain replay: a checkpoint when one is due.
    void seekCpAfterUpdate() {
        actCensus();
        latchLine();
        regionLine();
        if (seekcp::g_every <= 0 || seekcp::g_off) return;
        if (!g_started || g_sessionOver || g_cfg.dpSolve || g_serveMode || secsolve::g_on
            || secsolve::g_active || takeover::armed())
            return;
        auto* pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this || !m_player1) return;
        if (pl->m_isPracticeMode || pl->m_playerDied || m_player1->m_isDead || m_player1->m_isLocked) return;
        seekcp::validate(pl, pl->m_objects ? (size_t)pl->m_objects->count() : 0);
        if (seekcp::g_step <= 0) seekcp::g_step = seekcp::g_every;
        if (!seekcp::due(g_tick)) return;
        if (seekcp::g_memBefore < 0) seekcp::g_memBefore = privateMB();
        const auto t0 = std::chrono::steady_clock::now();
        auto r = std::make_shared<cpflight::Rec>();
        r->tick = g_tick;
        cpCaptureState(pl, *r);
        if (!cpMark(pl, *r)) {
            writeResult("seekcp: markCheckpoint returned 0 at t=" + std::to_string((long long)g_tick)
                        + " - off for this session");
            seekcp::g_off = true;
            return;
        }
        if (itermap::g_groupCheck) {
            std::vector<uint8_t> tg;
            tg.reserve(m_objects ? m_objects->count() : 0);
            for (auto* o : CCArrayExt<GameObject*>(m_objects))
                tg.push_back(o ? (uint8_t)((o->m_isGroupDisabled ? 1 : 0) | (o->m_outerSectionIndex >= 0 ? 2 : 0)) : 0);
            seekcp::g_toggles[r.get()] = std::move(tg);
            groupSectionCheck("checkpoint taken", (long long)g_tick);
        }
        seekcp::add(std::move(r));
        ++seekcp::g_taken;
        seekcp::g_takeMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        seekcp::g_memAfter = privateMB();
    }

    // cfg seekblind: the tick a seek to `target` runs screen off up to -- the last checkpoint slot at
    // or before it, so the checkpoint the restart then loads is at most one spacing short of the
    // target. -1: no checkpoints in this session.
    long long seekBlindCut(long long target) {
        if (seekcp::g_every <= 0 || seekcp::g_off) return -1;
        const long long step = seekcp::g_step > 0 ? seekcp::g_step : seekcp::g_every;
        return (target / step) * step;
    }

    void endSeekBlind(long long cut) {
        itermap::g_seekBlindNow = false;
        itermap::g_seekBlindDone = true;
        itermap::g_seekBlindMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - itermap::g_seekT0).count();
        itermap::g_seekRestart = true;
        char b[200];
        snprintf(b, sizeof(b), "seekblind: screen off up to t=%lld (cut %lld%s) in %.0f ms; the rest from "
                 "the checkpoint at or before t=%lld", (long long)g_tick, cut,
                 (m_player1 && m_player1->m_isDead) ? ", died first" : "", itermap::g_seekBlindMs,
                 (long long)itermap::g_seekTick);
        writeResult(b);
    }

    // A seek's restart from the latest checkpoint at or before `target`, loaded the way the loop's
    // flights load one (cpStart). False: none to load, and the caller restarts the level.
    bool seekCpRestore(PlayLayer* pl, long long target) {
        if (seekcp::g_every <= 0 || seekcp::g_off || !pl || !m_player1) return false;
        seekcp::validate(pl, pl->m_objects ? (size_t)pl->m_objects->count() : 0);
        long long want = target;
        if (itermap::g_seekFromCp >= 0) { want = std::min(want, itermap::g_seekFromCp); itermap::g_seekFromCp = -1; }
        auto r = seekcp::atOrBefore(want);
        if (!r || !r->cp) return false;
        const auto t0 = std::chrono::steady_clock::now();
        seekcp::g_restoring = true;
        clearCheckpointsUnsectioned(pl);
        pl->storeCheckpoint(r->cp);
        g_restorePending = true;
        pl->resetLevel();
        g_restorePending = false;
        seekcp::g_restoring = false;
        cpRestoreState(pl, *r, false);
        g_nextInput = r->nextInput;
        g_nextToggle = r->nextToggle;
        g_tick = r->tick;
        itermap::g_nowTick = r->tick;
        seekcp::g_from = r->tick;
        seekcp::g_partSaid = false;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        ++seekcp::g_restores;
        seekcp::g_restoreMs += ms;
        char b[200];
        snprintf(b, sizeof(b), "seekcp: restored the checkpoint at t=%lld for a seek to t=%lld in %.1f ms",
                 r->tick, target, ms);
        writeResult(b);
        return true;
    }

    // A checkpoint of the loop's flight at the tick just run, with what it does not carry, as the
    // entry-snapshot gate takes one (snapat), and taken out of GD's list at once so that no reset of
    // GD's own can load it. Between two update calls, which is where a restore also lands: the frame
    // boundaries of the flight do not move.
    void cpTake(PlayLayer* pl) {
        const long long S = g_cfg.cpFlight;
        const long long T = g_tick;
        if (!m_player1 || !cpflight::g_live || cpflight::g_live->attempt != g_attempt || S <= 0) {
            cpflight::g_next = -1;
            return;
        }
        if (pl->m_playerDied || m_player1->m_isDead) return;
        // A tick without an anchor row cannot be checked after a restore: the next slot instead.
        if (T < 1 || (size_t)T >= anchors::g_live.size() || !anchors::g_live[(size_t)T].valid) {
            cpflight::g_next = (T / S + 1) * S;
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto r = std::make_shared<cpflight::Rec>();
        r->tick = T;
        cpCaptureState(pl, *r);
        if (g_cfg.cpFlightGaps || g_cfg.cpDisabled) r->gaps = cpflight::gapSig(pl);
        if (g_cfg.cpFollowFloats) cpflight::captureFollow(pl, *r);
        if (g_cfg.cpRotNode) cpflight::captureRotNodes(pl, *r);
        if (g_cfg.cpFlightLayerDiff)
            r->layerBytes.assign(reinterpret_cast<const uint8_t*>(pl),
                                 reinterpret_cast<const uint8_t*>(pl) + sizeof(PlayLayer));
        cpflight::objLine(this, "take", T);
        if (g_cfg.cpFlightObj >= 0) cpflight::g_objAfter = 3;
        cpflight::captureBooks(*r);
        if (!cpMark(pl, *r)) {
            writeResult("cpflight: markCheckpoint returned 0 at t=" + std::to_string(T)
                        + " - no more checkpoints this attempt");
            cpflight::g_next = -1;
            return;
        }
        if (g_cfg.cpFlightLayerDiff) {
            cpflight::g_diffCollect = r;
            cpflight::g_diffCompare.reset();
            cpflight::g_diffStep = 0;
        }
        cpflight::add(std::move(r));
        cpflight::g_takeMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    // Start the installed plan -- or the probe -- from the checkpoint poll chose. The restore is the
    // entry-snapshot gate's, which reproduced the head run on 313/313 snapshots: the checkpoint, the
    // player's bytes and containers, a hold armed rather than pressed again (a press fires the ring
    // the player touches there), GD's anti-cheat check. On top of it what a restore of the loop
    // needs: the out-of-bounds latch, GD's random seeds as the flight had them (rngfix has just set
    // them to the start-of-level values), and the cursors and tick. The books are the reset hook's
    // (cpflight::splice). A restore that does not land on the row the attempt recorded there is not
    // flown: the plan goes from the head.
    void cpStart(PlayLayer* pl) {
        cpflight::Choice c = std::move(cpflight::g_pending);
        cpflight::g_pending = cpflight::Choice{};
        if (!pl) return;
        if (!c.rec || !c.set || !m_player1) {
            if (!c.probe) pl->resetLevel();
            return;
        }
        const cpflight::Rec& r = *c.rec;
        if (c.probe && cpflight::firstDifference(c.set->plan, g_cfg.inputs) != LLONG_MAX) {
            writeResult("cpflight: probe dropped - the installed plan is not the one that died");
            return;
        }
        // The carry once more, at the reset itself (it was judged at the install or the queueing).
        const cpflight::CarrySig now = cpflight::carrySig(this);
        if (const int d = cpflight::sigDiff(c.set->startSig, now); d != 0) {
            if (c.probe && !g_cfg.cpFlightProbeCarry) {
                ++cpflight::g_probeSkips;
                return;
            }
            if (!c.probe && (d & cpflight::gateMask())) {
                cpflight::countCarryMiss(d & cpflight::gateMask());
                pl->resetLevel();
                return;
            }
        }
        // cfg cpflightprobecarry: the probe is flown whatever it inherits, and what it inherits is
        // taken here, at its own reset
        if (c.probe && g_cfg.cpFlightProbeCarry) {
            cpflight::g_probeInherit = now;
            cpflight::g_probeInheritDiff = cpflight::sigDiff(c.set->startSig, now);
        }
        const auto t0 = std::chrono::steady_clock::now();
        clearCheckpointsUnsectioned(pl);
        pl->storeCheckpoint(r.cp);
        cpflight::g_starting = &c;
        g_restorePending = true;
        pl->resetLevel();
        g_restorePending = false;
        cpflight::g_starting = nullptr;
        cpRestoreState(pl, r, true);
        // cfg cpdisabled / cpfollowfloats: what the checkpoint itself does not bring back
        int disabledPut = -1;
        if (g_cfg.cpDisabled) disabledPut = cpflight::restoreDisabled(pl, r.gaps);
        if (g_cfg.cpFollowFloats) cpflight::restoreFollow(pl, r);
        int rotPut = -1;
        if (g_cfg.cpRotNode) rotPut = cpflight::restoreRotNodes(r);
        cpflight::objLine(this, "restored", r.tick);
        cpflight::nodeWatch(this, r.tick, "restored");
        if (g_cfg.cpFlightObj >= 0) cpflight::g_objAfter = 3;
        g_nextInput = r.nextInput;
        g_nextToggle = r.nextToggle;
        g_tick = r.tick;
        if (g_cfg.cpFlightLayerDiff) cpflight::layerDiff(r.layerBytes, pl, r.tick);
        // The state of row T+1 (see cp_flight.hpp): the position the attempt that took the checkpoint
        // recorded at the head of the next substep, which nothing between the two moves.
        const AnchorRow& row = c.set->rows[(size_t)r.tick + 1];
        const auto pos = m_player1->getPosition();
        const float vy = (float)m_player1->m_yVelocity;
        char b[320];
        if (pos.x != row.x || pos.y != row.y) {
            snprintf(b, sizeof(b), "cpflight: the restore at t=%lld came back at x=%.4f y=%.4f vy=%.4f, "
                     "not row t+1's x=%.4f y=%.4f vy=%.4f - %s", r.tick, (double)pos.x, (double)pos.y,
                     (double)vy, (double)row.x, (double)row.y, (double)row.vy,
                     c.probe ? "no probe" : "flying the plan from the head");
            writeResult(b);
            ++cpflight::g_refused;
            if (!c.probe) {
                cpflight::g_forceHead = true;
                pl->resetLevel();
            } else {
                g_paused = true;
            }
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        cpflight::g_startMs += ms;
        if (g_cfg.cpFlightLayerDiff) {
            cpflight::g_diffCompare = c.rec;
            cpflight::g_diffCollect.reset();
            cpflight::g_diffStep = 0;
        }
        if (c.probe) {
            cpflight::g_probeFlying = true;
            cpflight::g_ended = false;   // counted as a probe flight
            cpflight::g_probeSet = c.set;
            cpflight::g_probeRec = c.rec;
            cpflight::g_probeAtT = cpflight::captureCarryFull(pl, r.tick);   // for the control's comparison
        } else {
            ++cpflight::g_starts;
            cpflight::g_ticksSkipped += r.tick;
        }
        snprintf(b, sizeof(b), "cpflight: %s attempt %d from the checkpoint at t=%lld x=%.1f (taken by "
                 "attempt %d%s), restored in %.1f ms", c.probe ? "probe" : "flight", g_attempt, r.tick,
                 (double)row.x, c.set->attempt,
                 c.probe ? (", which died at t=" + std::to_string(c.set->end)).c_str() : "", ms);
        writeResult(b);
        // cfg cpflightgaps (print only): what the restore left different from the take
        if (g_cfg.cpFlightGaps && r.gaps.have) {
            std::string line;
            const int d = cpflight::gapDiff(r.gaps, cpflight::gapSig(pl), &line);
            ++cpflight::g_gapChecks;
            for (int k = 0; k < cpflight::kGapParts; ++k)
                if (d & (1 << k)) ++cpflight::g_gapPart[k];
            writeResult("cpflight: gaps after the restore at t=" + std::to_string(r.tick) + " (take / now): " + line
                        + (disabledPut >= 0 ? " | cpdisabled put " + std::to_string(disabledPut) + " object(s) back"
                                            : std::string())
                        + (rotPut >= 0 ? " | cprotnode put " + std::to_string(rotPut) + " of "
                                             + std::to_string(r.rotNodes.size()) + " rotation(s) back"
                                       : std::string()));
        }
    }

    // Measure whether a player-only restore can replace a full checkpoint restore
    // (cfg `secpsnap=N`).
    //
    // The measurement is exactly "is it equivalent as a branching primitive":
    //   A: full checkpoint restore -> run N ticks                       (current branch)
    //   B: level left as is, restore only the player -> run N ticks      (proposed branch)
    // B is repeated R times because that means restoring from a level state that has
    // drifted further and further ahead; if nothing in the section moves, it should match
    // A regardless of the count. In a section where it matches, one branch goes from
    // 2.02ms/0.95MB to a single memcpy.
    //
    // Never judge by survival or the terminal x (got fooled by that once today).
    // Compare y/vy every tick and report the first tick that splits.
    void runSnapCompare(PlayLayer* pl) {
        snapSweep(pl, secsolve::g_snapCmp, true);
        endSession("psnap");
    }

    static GJEffectManager* EM(GJBaseGameLayer* l) {
        return l ? l->m_effectManager : nullptr;
    }

    // Measure whether geometry moves in this section. psnap restores only the player and
    // game-layer state and does not restore GameObject positions, so in a section with
    // moving objects it disagrees with the checkpoint.
    //
    // Measured (lv20): in a non-moving section (inside wave2, target 5000) all counters
    // match the checkpoint and the solution replays plainly with dy=0.000. In a moving
    // section (ship, 231 grouped objects in the window) it splits at depth 106 -- psnap
    // misses branches the checkpoint counts as dead (dead 4 vs 2). The sweep does not
    // show it (the window is short and touches no moving object).
    // If `out` is passed, the set to restore is also built here (for `wsnap`). It is
    //   objects that actually moved (whole level) U grouped objects inside the section's
    //   x window
    // where the latter picks up "objects that move once touched". Running once plainly
    // does not move them, so the moved objects alone are not enough
    // ([[gd-touch-triggers]]).
    // Objects whose position shift makes the world a different one (mode/gravity/size/
    // dual/mirror/teleport portals). Using psnap in a section where these move makes the
    // search see "a world that does not exist" -- with a portal in a different position,
    // the mode does not change where it should (or changes where it should not).
    //
    // Measured (lv22, 2026-08-12): the ball portal (2413,59) has groups=1 and moves,
    // rising up to the player's height. The psnap search does not know that, so it
    // wiped out at the same depth with cap 400 and with cap 2000. "Detect and repair"
    // (`secverifyevery`) works against drift but not against a systematically false world.
    static bool isWorldDefiningPortal(int id) {
        switch (id) {
            case 12: case 13: case 47: case 111: case 660:
            case 745: case 1331: case 1933:            // mode
            case 10: case 11:                          // gravity
            case 99: case 101:                         // size
            case 286: case 287:                        // dual
            case 45: case 46:                          // mirror
            case 747: case 2902:                       // teleport
                return true;
            default:
                return false;
        }
    }

    // `movedOnly`: `out` takes only the objects that moved under the three input patterns, not
    // also every grouped object in reach (cfg secwsnapset=1).
    // cfg secwsnapset=2 / secwsnapgroups: the objects of the groups a section's own position
    // triggers aim at -- move 901, rotate 1346, follow 1347, follow-player-Y 1814, scale 2067 and
    // toggle 1049 placed in [x0, x1], plus those triggers when they sit in a group a spawn 1268 in
    // that range calls -- or of the groups named (secsolve::g_wsnapGroups, diagnosis), appended
    // to `out` without duplicates.
    // `anyTrigger` (secwsnapset=3): every trigger kind counts, not those six -- the Area effects,
    // alpha, animate and the rest move or reshape objects too -- and a trigger's center group goes
    // in with its target group.
    void wsnapTriggerSet(std::vector<GameObject*>& out, double x0, double x1, bool anyTrigger = false) {
        if (!this->m_objects) return;
        auto mover = [anyTrigger](int id) {
            if (anyTrigger) return id != 1268;
            return id == 901 || id == 1346 || id == 1347 || id == 1814 || id == 2067 || id == 1049;
        };
        auto take = [&](EffectGameObject* e, std::set<int>& into) {
            into.insert(e->m_targetGroupID);
            if (anyTrigger && e->m_centerGroupID > 0) into.insert(e->m_centerGroupID);
        };
        std::set<int> groups(secsolve::g_wsnapGroups.begin(), secsolve::g_wsnapGroups.end());
        if (groups.empty()) {
            std::set<int> spawned;
            for (auto* o : CCArrayExt<GameObject*>(this->m_objects)) {
                if (!o) continue;
                const double ox = o->getPositionX();
                if (ox < x0 || ox > x1) continue;
                auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(o);
                if (!e || e->m_targetGroupID <= 0) continue;
                if (mover(o->m_objectID)) take(e, groups);
                else if (o->m_objectID == 1268) spawned.insert(e->m_targetGroupID);
            }
            for (int g : spawned)
                if (auto* arr = this->getGroup(g))
                    for (auto* o : CCArrayExt<GameObject*>(arr))
                        if (auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(o))
                            if (e->m_targetGroupID > 0 && mover(o->m_objectID))
                                take(e, groups);
        }
        std::unordered_set<GameObject*> seen(out.begin(), out.end());
        for (int g : groups)
            if (auto* arr = this->getGroup(g))
                for (auto* o : CCArrayExt<GameObject*>(arr))
                    if (o && seen.insert(o).second) out.push_back(o);
    }

    int movingObjectsInSection(PlayLayer* pl, int n,
                               std::vector<GameObject*>* out = nullptr,
                               int* portalsOut = nullptr, bool movedOnly = false,
                               double* winLo = nullptr, double* winHi = nullptr,
                               bool physOnly = false, bool toHorizon = false) {
        if (out) out->clear();
        auto* arr = this->m_objects;
        const int nobj = arr ? arr->count() : 0;
        if (!nobj) return 0;
        const float dt = g_cfg.fastdt;
        int held = 0;
        for (const auto& in : g_cfg.inputs)
            if (in.step <= g_ckptTick && !in.p2) held = in.down ? 1 : 0;
        secRestore(pl);
        for (int i = 0; i < 2; ++i) secStep(held, dt);   // the frozen ticks
        std::vector<float> px((size_t)nobj), py((size_t)nobj);
        for (int i = 0; i < nobj; ++i) {
            auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
            px[(size_t)i] = o->getPositionX();
            py[(size_t)i] = o->getPositionY();
        }
        // The window is measured, not derived: the x range the player actually occupied,
        // plus a margin.
        //
        // Never decide from a single run. The first version measured with just one run,
        // "n ticks holding the section's input", and in the ship section it died within a
        // few dozen ticks and left the loop. Both the window and the moved objects only
        // covered the head of the section; the set of 162 was too small and it dropped the
        // d=106 deaths (4 of them) wholesale. Swallow deaths, run to the end, and take the
        // union over different input patterns.
        const bool savedNoKill = secsolve::g_noKill;
        secsolve::g_noKill = true;
        double wx0 = m_player1 ? m_player1->getPositionX() : 0.0, wx1 = wx0;
        double vMax = 0.0;   // the fastest x step the probe saw (secwsnapset=5)
        std::vector<uint8_t> moving((size_t)nobj, 0);
        // cfg secwsnapany (secsolve::g_wsnapAny, with secwsnapauto): `changed` takes an object
        // whose pose -- position, rotation, scale, visibility and the area offsets -- differs from
        // its pose at the pattern's start at any sample (every kPoseEvery ticks and at the end).
        // `moving` stays the old end-to-start position test: the portal count and the return value
        // read it, so a run without secwsnapauto is the old qualify to the byte.
        const bool anyChange = secsolve::g_wsnapAuto && secsolve::g_wsnapAny && out;
        constexpr int kPoseEvery = 10;
        static const std::vector<psnap::Mem> poseMems = [] {
            static const char* const kNames[] = {
                "m_positionXOffset", "m_positionYOffset", "m_rotationXOffset", "m_rotationYOffset",
                "m_scaleXOffset", "m_scaleYOffset", "m_customScaleX", "m_customScaleY",
                "m_isRotationAligned"};
            std::vector<psnap::Mem> r;
            for (const auto& m : psnap::goScalars())
                for (const char* n : kNames)
                    if (std::strcmp(m.name, n) == 0) { r.push_back(m); break; }
            return r;
        }();
        size_t poseBytes = 7 * sizeof(float);
        for (const auto& m : poseMems) poseBytes += m.size;
        std::vector<uint8_t> changed(anyChange ? (size_t)nobj : 0, 0);
        std::vector<uint8_t> pose0(anyChange ? (size_t)nobj * poseBytes : 0);
        auto poseOf = [&](GameObject* o, uint8_t* d) {
            const float f[7] = {o->getPositionX(), o->getPositionY(), o->getRotationX(),
                                o->getRotationY(), o->getScaleX(), o->getScaleY(),
                                o->isVisible() ? 1.f : 0.f};
            std::memcpy(d, f, sizeof f);
            d += sizeof f;
            for (const auto& m : poseMems) {
                std::memcpy(d, (const uint8_t*)o + m.off, m.size);
                d += m.size;
            }
        };
        auto poseCheck = [&]() {
            std::vector<uint8_t> now(poseBytes);
            for (int i = 0; i < nobj; ++i) {
                if (changed[(size_t)i]) continue;
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                poseOf(o, now.data());
                const uint8_t* b = pose0.data() + (size_t)i * poseBytes;
                const float* fa = (const float*)b;
                const float* fb = (const float*)now.data();
                bool diff = false;
                for (int k = 0; k < 7 && !diff; ++k) diff = std::fabs(fa[k] - fb[k]) > 1e-4f;
                if (!diff)
                    diff = std::memcmp(b + 7 * sizeof(float), now.data() + 7 * sizeof(float),
                                       poseBytes - 7 * sizeof(float)) != 0;
                if (diff) changed[(size_t)i] = 1;
            }
        };
        // 0=always released / 1=always held / 2=toggle every tick. What gets touched
        // changes, so put "objects moved by any one of them" into the set (the same reason
        // only the every-tick toggle broke in the sweep -- what is touched depends on the
        // trajectory).
        for (int pat = 0; pat < 3; ++pat) {
            secRestore(pl);
            for (int i = 0; i < 2; ++i) secStep(held, dt);   // the frozen ticks
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                px[(size_t)i] = o->getPositionX();
                py[(size_t)i] = o->getPositionY();
                if (anyChange) poseOf(o, pose0.data() + (size_t)i * poseBytes);
            }
            double xPrev = m_player1 ? m_player1->getPositionX() : 0.0;
            for (int i = 0; i < n; ++i) {
                const int in = pat == 0 ? 0 : (pat == 1 ? 1 : (i & 1));
                secStep(in, dt);
                if (!m_player1) break;
                const double x = m_player1->getPositionX();
                wx0 = std::min(wx0, x); wx1 = std::max(wx1, x);
                vMax = std::max(vMax, x - xPrev);
                xPrev = x;
                if (anyChange && i % kPoseEvery == kPoseEvery - 1) poseCheck();
            }
            if (anyChange) poseCheck();
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                if (std::fabs(o->getPositionX() - px[(size_t)i]) > 1e-4
                    || std::fabs(o->getPositionY() - py[(size_t)i]) > 1e-4)
                    moving[(size_t)i] = 1;
            }
        }
        secsolve::g_noKill = savedNoKill;
        // secwsnapset=5 (`toHorizon`): the probe runs the first n ticks only, and a long window's
        // later part lies past it -- the reach window's far end goes on by the ticks the probe did
        // not run at the fastest x step it saw. Measured (2026-10-08) on four custom windows of 904 to
        // 3,151 ticks: set 4 moved the snapshot's loss of the plan's own branch hundreds of ticks
        // later, and in each the new loss point lay past the window or on its edge, the moving
        // objects there left out.
        if (toHorizon && secsolve::g_horizon > n && vMax > 0.0) {
            const double ext = (double)(secsolve::g_horizon - n) * vMax;
            wx1 += ext;
            char eb[160];
            snprintf(eb, sizeof(eb), "secsolve: wsnap reach extended %.0f px past the probe (%d of %d ticks, "
                     "%.2f px a tick at most) (secwsnapset=5)", ext, n, secsolve::g_horizon, vMax);
            writeResult(eb);
        }
        wx0 -= 900.0; wx1 += 900.0;
        if (winLo) *winLo = wx0;
        if (winHi) *winHi = wx1;
        int moved = 0;
        if (portalsOut) *portalsOut = 0;
        for (int i = 0; i < nobj; ++i) {
            if (moving[(size_t)i]) ++moved;
            auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
            const bool grouped = o->m_groupCount > 0 || o->m_hasGroupParent
                                 || o->m_hasAreaParent;
            const double ox = o->getPositionX();
            const bool inWindow = ox >= wx0 && ox <= wx1;
            // Count only portals that actually moved. Including "grouped objects in the
            // window" drops back to the checkpoint because of far-away portals that are
            // never touched -- measured (lv22's early cube section): a search of a few
            // seconds with psnap took over 10 minutes with the checkpoint. Objects that
            // move once touched may be missed, but the leaf cross-check and DOOMED catch
            // that (a false solution is rejected by the verdict).
            if (portalsOut && isWorldDefiningPortal(o->m_objectID)
                && moving[(size_t)i])
                ++*portalsOut;
            if (!out) continue;
            const bool posed = anyChange && changed[(size_t)i];
            // secwsnapset=4 (`physOnly`): the grouped objects in reach, but not decoration (object
            // type 7) -- what a trigger can move into the player's way, without the decoration that
            // carries groups too and is most of the in-reach set on a heavy level.
            const bool reach = grouped && inWindow && !(physOnly && (int)o->m_objectType == 7);
            if (moving[(size_t)i] || posed || (!movedOnly && reach))
                out->push_back(o);
        }
        if (anyChange) {
            size_t nChanged = 0;
            for (uint8_t c : changed) nChanged += c;
            char cb[160];
            snprintf(cb, sizeof(cb), "secsolve: wsnap probe %zu objects - %d moved end to start, %zu changed "
                     "pose at a sample (cfg secwsnapany)", out->size(), moved, nChanged);
            writeResult(cb);
        }
        return moved;
    }

    // Look at the world side directly (cfg `secworld=N`).
    // Compare the world after a checkpoint restore with the world after a psnap restore,
    // object by object.
    void runWorldDiff(PlayLayer* pl) {
        using namespace secsolve;
        const float dt = g_cfg.fastdt;
        const int N = g_worldDiff;
        char b[256];
        auto* arr = this->m_objects;
        const int nobj = arr ? arr->count() : 0;
        // A per-member fingerprint (folded over all objects). Emitting per object could
        // only say "19,671 differ" without telling what differs.
        // The first 3 are position and rotation.
        const auto& gos = psnap::goScalars();
        const auto& gbs = psnap::gbScalars();
        // First half = object side (folded over all objects), second half = game-layer
        // side. There is only one layer, so raw bytes rather than a fold.
        const size_t NOBJM = gos.size() + 3;
        const size_t NCNT_ = 21;                 // must match cnts below
        const size_t NM = NOBJM + gbs.size() + NCNT_;
        // The element counts of the OPAQUE members (containers) go into the fingerprint
        // too. Of the 261, 157 are visual-only (sprite batches etc.), but the remaining 64
        // include things that affect physics: m_sections (the spatial index for
        // collisions), m_groupDict, m_effectManager, the spawn queue. They cannot be
        // tracked by bytes, so first only check "do the counts disagree".
        struct Cnt { const char* name; std::function<long long()> get; };
        const std::vector<Cnt> cnts = {
            {"m_objects", [&] { return (long long)(this->m_objects ? this->m_objects->count() : -1); }},
            {"m_collisionBlocks", [&] { return (long long)(this->m_collisionBlocks ? this->m_collisionBlocks->count() : -1); }},
            {"m_spawnObjectsArray", [&] { return (long long)(this->m_spawnObjectsArray ? this->m_spawnObjectsArray->count() : -1); }},
            {"m_disabledObjects", [&] { return (long long)this->m_disabledObjects.size(); }},
            {"m_areaObjects", [&] { return (long long)this->m_areaObjects.size(); }},
            {"m_processedAreaObjects", [&] { return (long long)this->m_processedAreaObjects.size(); }},
            {"m_visibleObjects", [&] { return (long long)this->m_visibleObjects.size(); }},
            {"m_visibleObjects2", [&] { return (long long)this->m_visibleObjects2.size(); }},
            {"m_groups", [&] { return (long long)this->m_groups.size(); }},
            {"m_staticGroups", [&] { return (long long)this->m_staticGroups.size(); }},
            {"m_sections", [&] { return (long long)this->m_sections.size(); }},
            // ---- Contents of GJEffectManager ----
            // What CheckpointObject saves wholesale as `EffectManagerState`. This is the
            // set GD itself decided "breaks unless restored", so look here first rather
            // than blindly widening the 261 (user suggestion). It holds the trigger queue
            // and the in-progress group commands = the state while moving geometry is
            // in motion.
            {"em.pulseVec", [&] { return EM(this) ? (long long)EM(this)->m_pulseEffectVector.size() : -1; }},
            {"em.pulseMap", [&] { return EM(this) ? (long long)EM(this)->m_pulseEffectMap.size() : -1; }},
            {"em.opacityMap", [&] { return EM(this) ? (long long)EM(this)->m_opacityEffectMap.size() : -1; }},
            {"em.touchToggle", [&] { return EM(this) ? (long long)EM(this)->m_unkVector1e0.size() : -1; }},
            {"em.countTrig", [&] { return EM(this) ? (long long)EM(this)->m_countTriggerActions.size() : -1; }},
            {"em.collisionTrig", [&] { return EM(this) ? (long long)EM(this)->m_unkVector230.size() : -1; }},
            {"em.toggleTrig", [&] { return EM(this) ? (long long)EM(this)->m_unkVector248.size() : -1; }},
            {"em.itemCount", [&] { return EM(this) ? (long long)EM(this)->m_itemCountMap.size() : -1; }},
            {"em.timerItem", [&] { return EM(this) ? (long long)EM(this)->m_timerItemMap.size() : -1; }},
            {"em.timerTrig", [&] { return EM(this) ? (long long)EM(this)->m_unkMap3f8.size() : -1; }},
        };
        const size_t NCNT = cnts.size();
        auto sig = [&](std::vector<uint64_t>& out) {
            out.assign(NM, 1469598103934665603ull);
            for (size_t k = 0; k < gbs.size(); ++k) {
                uint64_t h = 1469598103934665603ull;
                const uint8_t* p = (const uint8_t*)this + gbs[k].off;
                for (size_t j = 0; j < gbs[k].size; ++j) {
                    h ^= p[j]; h *= 1099511628211ull;
                }
                out[NOBJM + k] = h;
            }
            for (size_t k = 0; k < NCNT; ++k)
                out[NOBJM + gbs.size() + k] = (uint64_t)cnts[k].get();
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(
                    arr->objectAtIndex((unsigned)i));
                auto mix = [](uint64_t& h, const void* p, size_t n) {
                    const uint8_t* b = (const uint8_t*)p;
                    for (size_t j = 0; j < n; ++j) {
                        h ^= b[j]; h *= 1099511628211ull;
                    }
                };
                const float px = o->getPositionX(), py = o->getPositionY(),
                            pr = o->getRotation();
                mix(out[0], &px, 4); mix(out[1], &py, 4); mix(out[2], &pr, 4);
                for (size_t k = 0; k < gos.size(); ++k)
                    mix(out[k + 3], (const uint8_t*)o + gos[k].off, gos[k].size);
            }
        };
        auto nameOf = [&](size_t k) -> const char* {
            if (k == 0) return "<x>";
            if (k == 1) return "<y>";
            if (k == 2) return "<rot>";
            if (k < NOBJM) return gos[k - 3].name;
            if (k < NOBJM + gbs.size()) return gbs[k - NOBJM].name;
            return cnts[k - NOBJM - gbs.size()].name;
        };
        auto sideOf = [&](size_t k) {
            return k < NOBJM ? "obj"
                 : k < NOBJM + gbs.size() ? "LAYER" : "COUNT";
        };
        snprintf(b, sizeof(b), "world: objects=%d scalarsPerObj=%zu N=%d",
                 nobj, psnap::goScalars().size(), N);
        writeResult(b);
        if (!nobj) { endSession("world"); return; }

        int held = 0;
        for (const auto& in : g_cfg.inputs)
            if (in.step <= g_ckptTick && !in.p2) held = in.down ? 1 : 0;
        secsolve::setSearching(true);
        int freeze = 0;
        std::vector<uint64_t> base, again, after;
        std::vector<uint8_t> snapBytes;

        // (1) The world right after a checkpoint restore
        secRestore(pl);
        {
            const double xf = m_player1 ? m_player1->getPositionX() : 0.0;
            for (int i = 0; i < 8; ++i) {
                secStep(held, dt);
                if (m_player1 && m_player1->getPositionX() != xf) break;
                ++freeze;
            }
        }
        secRestore(pl);
        for (int i = 0; i < freeze; ++i) secStep(held, dt);
        sig(base);
        psnap::capture(m_player1, this, snapBytes);

        // (2) The same thing once more -> check whether the checkpoint restore itself is
        // deterministic (the denominator)
        for (int i = 0; i < N; ++i) secStep(held, dt);
        secRestore(pl);
        for (int i = 0; i < freeze; ++i) secStep(held, dt);
        sig(again);
        // Members that differ between two checkpoint restores cannot be used for the
        // comparison at all (frame-derived caches etc.). The denominator is decided
        // automatically here.
        std::vector<uint8_t> stable(NM, 1);
        size_t nStable = 0;
        for (size_t k = 0; k < NM; ++k) {
            stable[k] = (again[k] == base[k]) ? 1 : 0;
            nStable += stable[k];
        }
        snprintf(b, sizeof(b),
                 "world: CP->run%d->CP stable=%zu / %zu members "
                 "(the rest move on the restore itself, so they are not compared)",
                 N, nStable, NM);
        writeResult(b);
        for (size_t k = 0; k < NM; ++k)
            if (!stable[k]) {
                snprintf(b, sizeof(b), "world:   unstable across CP [%s] %s",
                         sideOf(k), nameOf(k));
                writeResult(b);
            }
        // If only positions fluctuate, "how many objects fail to return fully" is directly
        // the answer to "does the checkpoint restore moving geometry". Update 32's "the
        // checkpoint is bit-exact including moving geometry" only looked at the player's
        // x/y, so this is the first direct check.
        if (!stable[0] || !stable[1] || !stable[2]) {
            std::vector<float> px((size_t)nobj), py((size_t)nobj);
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                px[(size_t)i] = o->getPositionX();
                py[(size_t)i] = o->getPositionY();
            }
            secRestore(pl);
            for (int i = 0; i < freeze; ++i) secStep(held, dt);
            int moved = 0; double worst = 0.0; int worstIdx = -1;
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                const double d = std::fabs(o->getPositionX() - px[(size_t)i])
                               + std::fabs(o->getPositionY() - py[(size_t)i]);
                if (d > 1e-4) {
                    ++moved;
                    if (d > worst) { worst = d; worstIdx = i; }
                }
            }
            snprintf(b, sizeof(b),
                     "world: objects at a different position after two CP restores"
                     " = %d / %d (worst %.2fpx, obj[%d])",
                     moved, nobj, worst, worstIdx);
            writeResult(b);
        }

        // (3) Run N ticks, then restore only the player with psnap = what the search does
        for (int i = 0; i < N; ++i) secStep(held, dt);
        psnap::restore(m_player1, this, snapBytes.data());
        g_injecting = true;
        this->handleButton(false, 1, true);
        g_injecting = false;
        g_held = 0;
        sig(after);
        size_t nDiff = 0, shown = 0;
        for (size_t k = 0; k < NM; ++k) {
            if (!stable[k] || after[k] == base[k]) continue;
            ++nDiff;
            if (shown < 40) {
                snprintf(b, sizeof(b), "world:   psnap differs [%s] %s",
                         sideOf(k), nameOf(k));
                writeResult(b);
                ++shown;
            }
        }
        snprintf(b, sizeof(b),
                 "world: CP->run%d->psnap differs=%zu / %zu stable members",
                 N, nDiff, nStable);
        writeResult(b);
        secsolve::setSearching(false);
        endSession("world");
    }

    // The sweep proper. Returns whether psnap may be used as the branching primitive in
    // this section. If verbose, emit every line (the cfg `secpsnap=N` diagnostic);
    // otherwise just the verdict.
    bool snapSweep(PlayLayer* pl, int N, bool verbose) {
        using namespace secsolve;
        const float dt = g_cfg.fastdt;
        char b[256];
        if (verbose) {
            snprintf(b, sizeof(b),
                     "psnap: sizeof(PlayerObject)=%zu scalars=%zu copyBytes=%zu",
                     sizeof(PlayerObject), psnap::scalarCount(),
                     psnap::maskedBytes());
            writeResult(b);
        }
        // The held state at the section head (the value of pattern "keep")
        int held0 = 0;
        for (const auto& in : g_cfg.inputs)
            if (in.step <= g_ckptTick && !in.p2) held0 = in.down ? 1 : 0;

        // Input patterns. The every-tick toggle must be included. The layered BFS branches
        // both ways every tick, so psnap is unusable unless hold boundaries pass -- that is
        // exactly where the old warp historically broke (bit-identical only on non-hold
        // ticks).
        struct Pat { const char* name; int period; int base; };
        // period 0 = constant (keep emitting base) / period k = toggle every k ticks
        const Pat PATS[] = {
            {"keep",    0, held0}, {"hold", 0, 1}, {"release", 0, 0},
            {"alt1",    1, 0},     {"alt2", 2, 0}, {"alt4",    4, 0},
            {"alt8",    8, 0},     {"alt16", 16, 0},
        };
        auto inputAt = [](const Pat& p, int i) -> int {
            if (p.period <= 0) return p.base;
            return ((i / p.period) & 1) ^ p.base;
        };
        // How many ticks to run the world ahead before restoring. The real search expands
        // 100,000 times per section, so branching with psnap alone lets the world get that
        // far ahead. "How far it may drift" decides the period K at which a checkpoint is
        // re-inserted.
        const int DRIFTS_FULL[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 512, 2000, 10000};
        const int DRIFTS_QUICK[] = {0, 1, 8, 64, 512, 5000};
        const int* DRIFTS = verbose ? DRIFTS_FULL : DRIFTS_QUICK;
        const size_t NDRIFT = verbose ? sizeof(DRIFTS_FULL) / sizeof(int)
                                      : sizeof(DRIFTS_QUICK) / sizeof(int);
        bool allOk = true;

        const bool wasActive = g_active;
        secsolve::setSearching(true);
        std::vector<double> refY, refV;
        std::vector<uint8_t> refFlags;   // mode|grounded<<4|mini<<5|flip<<6
        std::vector<uint8_t> snapBytes;
        double sx = 0.0, sy = 0.0;
        int freeze = 0;
        auto flagsOf = [&](PlayerObject* p) -> uint8_t {
            return (uint8_t)((int)modeIdx(p)
                             | ((p->m_isOnGround ? 1 : 0) << 4)
                             | ((p->m_vehicleSize < 0.9f ? 1 : 0) << 5)
                             | ((p->m_isUpsideDown ? 1 : 0) << 6));
        };
        const Pat* pat = &PATS[0];

        // ---- A: reference run from a full checkpoint restore. Take the snapshot right
        // after the checkpoint ----
        //
        // Take the reference over a length that survives. Running it until death puts the
        // level into the "attempt over" state, after which restoring the player's bytes
        // advances no physics at all (the first version stepped on this, and dy became the
        // reference's fall distance itself = 152px. It looks like the player side is
        // broken, but it had simply stopped). If it dies, cut it shorter.
        auto refPass = [&](int n) -> int {
            secRestore(pl);
            if (freeze == 0) {
                const double xf = m_player1 ? m_player1->getPositionX() : 0.0;
                for (int i = 0; i < 8; ++i) {
                    secStep(inputAt(*pat, 0), dt);
                    if (m_player1 && m_player1->getPositionX() != xf) break;
                    ++freeze;
                }
                secRestore(pl);
            }
            for (int i = 0; i < freeze; ++i) secStep(inputAt(*pat, 0), dt);
            if (!m_player1) return -2;
            psnap::capture(m_player1, this, snapBytes);
            sx = m_player1->getPositionX();
            sy = m_player1->getPositionY();
            refY.assign((size_t)n, 0.0);
            refV.assign((size_t)n, 0.0);
            refFlags.assign((size_t)n, 0);
            for (int i = 0; i < n; ++i) {
                secStep(inputAt(*pat, i), dt);
                if (!m_player1) return -2;
                refY[(size_t)i] = m_player1->getPositionY();
                refV[(size_t)i] = (double)m_player1->m_yVelocity;
                refFlags[(size_t)i] = flagsOf(m_player1);
                if (m_player1->m_isDead) return i;
            }
            return -1;
        };

        // After a restore, rebuild the button state with the same steps as the checkpoint
        // path. `m_holdingButtons` is on the PRESERVE side (not injected), so skipping this
        // means comparing "a ship that is pressing" with "a ship that is not" (the first
        // version split by -109px because of that).
        auto rearmButton = [&]() {
            g_injecting = true;
            this->handleButton(false, 1, true);
            g_injecting = false;
            g_held = 0;
        };

        for (const Pat& P : PATS) {
            pat = &P;
            int n = N;
            int refDied = refPass(n);
            if (refDied >= 0) {
                n = refDied - 2;
                if (n < 4) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%s SKIP - the reference dies after "
                             "%d tick", P.name, refDied);
                    writeResult(b);
                    continue;
                }
                refDied = refPass(n);
            }
            if (refDied != -1) {
                snprintf(b, sizeof(b), "psnap: pat=%s SKIP - unstable reference",
                         P.name);
                writeResult(b);
                continue;
            }

            for (size_t di = 0; di < NDRIFT; ++di) {
                const int D = DRIFTS[di];
                // Every time, return the world to the checkpoint first, then advance exactly
                // D ticks. Otherwise "drift=0" is actually a state shifted by the reference
                // run (n ticks), and tolerance to small drifts cannot be measured (the first
                // version was like that).
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                // Advance only the world by D ticks. The player is restored and pinned
                // every tick so it does not die; only the world's phase drifts (= what
                // happens when branching with psnap continuously without inserting a
                // checkpoint).
                for (int d = 0; d < D; ++d) {
                    psnap::restore(m_player1, this, snapBytes.data());
                    rearmButton();
                    secStep(inputAt(P, 0), dt);
                    if (!m_player1 || m_player1->m_isDead) break;
                }
                const auto t0 = std::chrono::steady_clock::now();
                psnap::restore(m_player1, this, snapBytes.data());
                const double us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - t0).count();
                rearmButton();
                int firstBad = -1, died = -1, flagBad = -1;
                double maxDy = 0.0, maxDv = 0.0;
                for (int i = 0; i < n; ++i) {
                    secStep(inputAt(P, i), dt);
                    if (!m_player1) break;
                    if (m_player1->m_isDead) { died = i; break; }
                    const double dy = m_player1->getPositionY() - refY[(size_t)i];
                    const double dv = (double)m_player1->m_yVelocity - refV[(size_t)i];
                    if (std::fabs(dy) > std::fabs(maxDy)) maxDy = dy;
                    if (std::fabs(dv) > std::fabs(maxDv)) maxDv = dv;
                    if (firstBad < 0 && (std::fabs(dy) > 1e-4 || std::fabs(dv) > 1e-4))
                        firstBad = i;
                    if (flagBad < 0 && flagsOf(m_player1) != refFlags[(size_t)i])
                        flagBad = i;
                }
                const bool bad = (firstBad >= 0 || flagBad >= 0 || died >= 0);
                if (verbose || bad) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%-7s drift=%-5d n=%-4d restoreUs=%5.1f "
                             "firstDiff=%-4d flagDiff=%-4d maxDy=%9.4f "
                             "maxDvy=%8.4f died=%d",
                             P.name, D, n, us, firstBad, flagBad, maxDy, maxDv,
                             died);
                    writeResult(b);
                }
                if (bad) {
                    allOk = false;
                    break;   // this pattern split; larger drifts are not examined
                }
            }
            // ---- C: chain test. Measures the same usage as the search ----
            //
            // The sweep above only looks at "take one at the section head and restore it
            // repeatedly". The search captures at every node and restores at every node,
            // so if capture/restore drops even a drop of state, it accumulates with depth.
            // Indeed, even in a section where the whole sweep passed, the exit state the
            // search produced was 21px off from the plain replay. Here a capture->restore
            // is inserted every tick and compared with the plain run.
            for (int withRearm = 0; withRearm < 2; ++withRearm) {
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                std::vector<uint8_t> s;
                int firstBad = -1, died = -1;
                double maxDy = 0.0, maxDv = 0.0;
                for (int i = 0; i < n; ++i) {
                    if (!m_player1) break;
                    psnap::capture(m_player1, this, s);
                    psnap::restore(m_player1, this, s.data());
                    if (withRearm) rearmButton();
                    secStep(inputAt(P, i), dt);
                    if (!m_player1) break;
                    if (m_player1->m_isDead) { died = i; break; }
                    const double dy = m_player1->getPositionY() - refY[(size_t)i];
                    const double dv = (double)m_player1->m_yVelocity - refV[(size_t)i];
                    if (std::fabs(dy) > std::fabs(maxDy)) maxDy = dy;
                    if (std::fabs(dv) > std::fabs(maxDv)) maxDv = dv;
                    if (firstBad < 0 && (std::fabs(dy) > 1e-4 || std::fabs(dv) > 1e-4))
                        firstBad = i;
                }
                const bool bad = (firstBad >= 0 || died >= 0);
                if (verbose || bad) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%-7s CHAIN rearm=%d n=%-4d "
                             "firstDiff=%-4d maxDy=%9.4f maxDvy=%8.4f died=%d",
                             P.name, withRearm, n, firstBad, maxDy, maxDv, died);
                    writeResult(b);
                }
                if (bad) allOk = false;
            }
            // ---- D: rewind test. An operation only the search performs ----
            //
            // In both the sweep (restore the same one) and the chain (capture and restore
            // every tick), the player only moves forward. The search returns to a parent =
            // moves x backward. GD switches the active object sections by the player's x,
            // so rewinding x without resetting the level can leave the active state
            // inconsistent. If it splits here, then that is what CheckpointObject spends
            // 0.95MB holding.
            for (int k : {1, 2, 4, 8, 16, 32, 64, 128}) {
                if (k >= n) break;
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                std::vector<uint8_t> back;
                bool ok = true;
                for (int i = 0; i < n; ++i) {
                    if (i == n - k && m_player1) psnap::capture(m_player1, this, back);
                    secStep(inputAt(P, i), dt);
                    if (!m_player1 || m_player1->m_isDead) { ok = false; break; }
                }
                if (!ok || back.empty()) continue;
                // Here the world is at tick n and so is the player. Rewind x by k ticks.
                psnap::restore(m_player1, this, back.data());
                rearmButton();
                for (int j = n - k; j < n; ++j) secStep(inputAt(P, j), dt);
                if (!m_player1) break;
                const double dy = m_player1->getPositionY() - refY[(size_t)n - 1];
                const double dv = (double)m_player1->m_yVelocity - refV[(size_t)n - 1];
                const bool bad = (std::fabs(dy) > 1e-4 || std::fabs(dv) > 1e-4
                                  || m_player1->m_isDead);
                if (verbose || bad) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%-7s REWIND k=%-4d n=%-4d dy=%9.4f "
                             "dvy=%8.4f dead=%d",
                             P.name, k, n, dy, dv, m_player1->m_isDead ? 1 : 0);
                    writeResult(b);
                }
                if (bad) { allOk = false; break; }
            }
            // ---- E: wake test. The only case where the world ends up "behind" the
            // player ----
            //
            // When a branch dies the level stops, so it is woken again with a checkpoint
            // (needWake). That checkpoint restore returns the world to the section head,
            // yet the player placed next is the state at depth d. So only the world lags by
            // d ticks. The drift sweep only looks at the world being ahead, so this
            // direction had never been measured.
            for (int k : {1, 4, 16, 64}) {
                if (k >= n) break;
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                std::vector<uint8_t> deep;
                bool ok = true;
                for (int i = 0; i < n - k; ++i) {
                    secStep(inputAt(P, i), dt);
                    if (!m_player1 || m_player1->m_isDead) { ok = false; break; }
                }
                if (!ok) continue;
                psnap::capture(m_player1, this, deep);
                // This reproduces needWake: return only the world to the section head
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                psnap::restore(m_player1, this, deep.data());
                rearmButton();
                for (int j = n - k; j < n; ++j) secStep(inputAt(P, j), dt);
                if (!m_player1) break;
                const double dy = m_player1->getPositionY() - refY[(size_t)n - 1];
                const double dv = (double)m_player1->m_yVelocity - refV[(size_t)n - 1];
                const bool bad = (std::fabs(dy) > 1e-4 || std::fabs(dv) > 1e-4
                                  || m_player1->m_isDead);
                if (verbose || bad) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%-7s WAKE   k=%-4d n=%-4d dy=%9.4f "
                             "dvy=%8.4f dead=%d",
                             P.name, k, n, dy, dv, m_player1->m_isDead ? 1 : 0);
                    writeResult(b);
                }
                if (bad) { allOk = false; break; }
            }
            if (!allOk && !verbose) break;   // verdict only: settled at the first split
        }
        secsolve::setSearching(wasActive);
        return allOk;
    }

    // Restore fidelity check (cfg `secverify=1`). Restore to the section checkpoint, then
    // re-feed the plan's inputs from secstart on as they are, and see whether it reaches
    // the same point as the original run. The whole correctness of the section solver
    // rests on this, so pass it before searching.
    void runSectionVerify(PlayLayer* pl) {
        using namespace secsolve;
        const float dt = g_cfg.fastdt;
        // Expand the inputs from secstart on into "tick -> pressed" form.
        // The origin is neither secstart nor ckptTick: the checkpoint is made at a frame
        // boundary so it is not necessarily the requested tick (measured: requested 4700,
        // got 4701), and the restore lands on the state of ckptTick+1, so the first step
        // is fed the input of tick ckptTick+2. Off by 1 tick, re-feeding the original
        // inputs still dies.
        const long long base = g_ckptTick - 3;   // margin on both sides so secoff can act
        // seq[j] = pressed state of the plain run at tick base+j
        std::vector<uint8_t> seq((size_t)g_horizon + 12, 0);
        int held = 0;
        size_t idx = 0;
        for (long long t = 0; t <= base + (long long)seq.size(); ++t) {
            while (idx < g_cfg.inputs.size() && g_cfg.inputs[idx].step <= t) {
                if (!g_cfg.inputs[idx].p2) held = g_cfg.inputs[idx].down ? 1 : 0;
                ++idx;
            }
            if (t >= base && (size_t)(t - base) < seq.size())
                seq[(size_t)(t - base)] = (uint8_t)held;
        }
        secsolve::setSearching(true);
        // Measure the frozen steps. Count them the same way the search does: the frozen
        // ticks idle with "the input at the time of the restore", and the k-th advance
        // after that produces tick base+k. Doing this with a flat array index lets the
        // frozen ticks eat two inputs and shifts the whole sequence forward (which made
        // it look like "the restore is broken").
        // seq index: tick t of the plain run is seq[t - base]. The restore lands on tick
        // ckptTick+1, so the k-th advance produces tick ckptTick+1+k. The input fed to it
        // is seq[(ckptTick+1+k) - base + g_off].
        const size_t at0 = (size_t)(g_ckptTick + 1 - base);   // = 4
        secRestore(pl);
        int freeze = 0;
        {
            const double xf = m_player1 ? m_player1->getPositionX() : 0.0;
            for (int i = 0; i < 8; ++i) {
                secStep(seq[at0], dt);
                if (m_player1 && m_player1->getPositionX() != xf) break;
                ++freeze;
            }
        }
        secRestore(pl);
        for (int i = 0; i < freeze; ++i) secStep(seq[at0], dt);
        const double x0 = m_player1 ? m_player1->getPositionX() : -1;
        const double y0 = m_player1 ? m_player1->getPositionY() : -1;
        int diedAt = -1;
        for (int i = 0; i < g_horizon; ++i) {
            const size_t si = (size_t)std::max(
                0, std::min((int)seq.size() - 1, (int)at0 + 1 + i + g_off));
            secStep(seq[si], dt);
            // With seclog=1, every step. A restore-fidelity break can only be located by
            // "from which tick it started to diverge" (the terminal difference alone cannot
            // separate a restore cause from a mid-way event).
            if ((g_log || i < 12) && m_player1) {
                char sb[192];
                snprintf(sb, sizeof(sb),
                         "secverify_step: i=%d in=%d x=%.4f y=%.4f vy=%.4f "
                         "mode=%s dead=%d",
                         i, (int)seq[si], m_player1->getPositionX(),
                         m_player1->getPositionY(),
                         (double)m_player1->m_yVelocity, modeStr(m_player1),
                         m_player1->m_isDead ? 1 : 0);
                writeResult(sb);
            }
            if (m_player1 && m_player1->m_isDead) { diedAt = i; break; }
        }
        const double x1 = m_player1 ? m_player1->getPositionX() : -1;
        const double y1 = m_player1 ? m_player1->getPositionY() : -1;
        secsolve::setSearching(false);
        char b[256];
        snprintf(b, sizeof(b),
                 "secverify: restoreX=%.3f restoreY=%.3f -> x=%.3f y=%.3f "
                 "diedAt=%d horizon=%d freeze=%d endTick=%lld",
                 x0, y0, x1, y1, diedAt, g_horizon, freeze,
                 (long long)(g_ckptTick + 1 + (diedAt < 0 ? g_horizon : diedAt)));
        writeResult(b);
        endSession("secverify");
    }

    // A COROUTINE (secsolve::SecTask): it suspends at a layer boundary once its slice budget is
    // spent and is resumed on the next frame, so the window, the HUD and the hotkeys stay alive
    // through a search that runs for twenty minutes. The body below is the search as it was --
    // suspending keeps its locals and lambdas in place, and no game time passes while it is
    // suspended (the driver in update() returns where the DP solve's freeze returns).
    secsolve::SecTask runSectionSolve(PlayLayer* pl) {
        using namespace secsolve;
        if (g_memAt >= 0) {
            char mb[220];
            for (auto& m : psnap::scalars()) {
                if ((long long)(m.off + m.size) <= g_memAt - 48) continue;
                if ((long long)m.off >= g_memAt + 48) continue;
                snprintf(mb, sizeof(mb), "secmem: off=%zu size=%zu %s",
                         m.off, m.size, m.name);
                writeResult(mb);
            }
        }
        const float dt = g_cfg.fastdt;
        const auto t0 = std::chrono::steady_clock::now();
        // The search runs outside tick space. Tens of thousands of ticks advance inside,
        // so the session bookkeeping (g_tick / input index) is saved and restored
        const long long savedTick = g_tick;
        const size_t savedInput = g_nextInput;

        // Choice of branching primitive (cfg `secsnap`). psnap restores only the player's
        // 3,144 bytes, so 1.4us / 3KB; the checkpoint is 2,020us / 0.95MB. But psnap does
        // not reset the level, so the world's phase keeps advancing, and in a section
        // with phase-dependent trajectories a 1 tick shift splits it (measured in lv20's
        // ship section). 2 = sweep at the section head and decide automatically. The
        // sweep checks "is it truly equivalent in this section" over 8 input patterns x
        // drift (snapSweep).
        g_snapOn = false;
        g_worldOn = false;
        g_movSet.clear();
        // cfg secsnapem / secsnapemfrom: whether this search's snapshots carry the effect manager
        psnap::g_snapEMActive = psnap::g_snapEM && (long long)g_ckptTick >= psnap::g_snapEMFrom;
        if (g_snapMode == 1) {
            g_snapOn = (psnap::scalarCount() > 0);
        } else if (g_snapMode == 3) {
            // Forced. Skip the sweep and run with wsnap (for diagnosis)
            g_snapOn = (psnap::scalarCount() > 0);
            if (g_snapOn) {
                movingObjectsInSection(pl, std::min(400, g_horizon), &g_movSet);
                g_worldOn = !g_movSet.empty();
            }
        } else if (g_snapMode == 4) {
            // Diagnosis only: put every object into the set. If this matches the
            // checkpoint, the cause of the split is "how the set is chosen"; if not, "what
            // is restored per object". One node becomes several MB, so use a small seccap.
            g_snapOn = (psnap::scalarCount() > 0);
            if (g_snapOn && this->m_objects) {
                for (unsigned i = 0; i < this->m_objects->count(); ++i)
                    g_movSet.push_back(static_cast<GameObject*>(
                        this->m_objects->objectAtIndex(i)));
                g_worldOn = true;
            }
        } else if (g_snapMode == 2) {
            const auto q0 = std::chrono::steady_clock::now();
            // The sweep window must cover the section length. When it was fixed at 120, in
            // a section with horizon=80 it dropped the known break at tick=105 outside the
            // window and reported PASS (trusting that, the section was solved with psnap
            // and the exit was 21px off).
            // If there is cross-check repair, no prior gate is needed (user suggestion).
            // Both the sweep and "count the moving objects" were prior "may it be used"
            // checks, kept because removing them produced false solutions. Now that the
            // frontier is matched against the ground truth every V layers, a lie is
            // rejected where it appears. The sweep takes 1.5 s and fails ship sections, so
            // skip it when V is set.
            const bool gated = g_verifyEvery <= 0;
            g_snapOn = gated ? snapSweep(pl, std::min(400, g_horizon), false)
                             : (psnap::scalarCount() > 0);
            // Even if the sweep passes, psnap must not be used plainly in a section where
            // geometry moves. The sweep's window is short, touches no moving object, and
            // passes ship sections. When there are moving objects, restore just those
            // objects as well (`wsnap`). If the set is too large one node gets heavier
            // than a checkpoint, so only then fall back to the checkpoint.
            //
            // wsnap is not used here. "Restoring the moving objects too fixes it" was
            // refuted by measurement (2026-08-07): at d=106 of lv20 ship, widening the
            // set 162->217, even restoring all 19,684 objects, the 4 branches the
            // checkpoint kills still pass through and the layer fingerprint did not move a
            // single bit. The killer is the stationary spike uid=4451, not a moving object
            // at all. Until the cause is known, moving sections stay on the checkpoint
            // (opt-in via `secsnap=3`).
            //
            // However, if `secverifyevery` is set, psnap is used even in moving sections
            // (user suggestion 2026-08-07). Even without nailing the fidelity, matching the
            // frontier every V layers by "replaying the input sequence from the section
            // checkpoint" rejects false branches there. psnap's errors only go one way
            // (missing deaths), so matching against the ground truth suffices. Replaying
            // the one solution at the end to confirm is done always.
            int moved = -1;
            int movPortals = 0;
            g_wsnapHeld = 0;
            if (g_snapOn && gated) {
                moved = movingObjectsInSection(pl, std::min(400, g_horizon),
                                               &g_movSet, &movPortals);
                g_movSet.clear();
                if (moved > 0) g_snapOn = false;
            } else if (g_snapOn) {
                // Even with cross-check repair, a section with moving portals falls back to
                // the checkpoint. "Detect and repair" works against drift, but a world with
                // portals in different positions is not drift but a different world, and
                // the very path the search finds becomes a lie (see the note on
                // isWorldDefiningPortal).
                double wLo = 0.0, wHi = 0.0;
                const bool byTrig = g_wsnapSet == 2 || g_wsnapSet == 3 || !g_wsnapGroups.empty();
                const int mv = movingObjectsInSection(pl, std::min(400, g_horizon),
                                                      g_wsnapAuto ? &g_movSet : nullptr,
                                                      &movPortals, g_wsnapSet == 1 || byTrig,
                                                      &wLo, &wHi, g_wsnapSet == 4 || g_wsnapSet == 5,
                                                      g_wsnapSet == 5);
                if (g_wsnapAuto && byTrig && movPortals == 0)
                    wsnapTriggerSet(g_movSet, wLo, wHi, g_wsnapSet == 3);
                if (movPortals > 0) {
                    g_movSet.clear();
                    g_snapOn = false;
                    // When falling back to the checkpoint, lower the cap too. Cost grows
                    // super-linearly with cap and memory grows as well (measured 15GB at
                    // cap 200). The caller passes a cap meant for psnap, so clamp it here.
                    if (g_cap > 120) {
                        char cb[120];
                        snprintf(cb, sizeof(cb),
                                 "secsolve: fell back to CP, so cap %zu -> 120", g_cap);
                        writeResult(cb);
                        g_cap = 120;
                    }
                } else if (g_wsnapAuto) {
                    // cfg secwsnapauto: ...and the objects that move are put back with the
                    // player (wsnap), where the cross-checks alone left them as the node stepped
                    // before had them (see secsolve::g_wsnapAuto).
                    // cfg secwsnapgate: a rung's first ask holds a set it could put back and runs
                    // as secwsnapauto=0 does; only if it finds nothing is the window asked again
                    // with the set (g_wsnapForce, the rung's end below).
                    // cfg seclieskippsnap: ...except in a window whose plain search was caught
                    // parting from the game before (secsolve::g_liedWindows): the set goes back now.
                    const bool lieSkip = g_lieSkipPsnap && g_wsnapGate && !g_wsnapForce && g_secRung
                                         && liedWindow(secsolve::g_startTick);
                    const bool hold = g_wsnapGate && !g_wsnapForce && !lieSkip && g_secRung
                                      && !g_movSet.empty() && g_movSet.size() <= g_maxMoving;
                    if (!hold) moved = mv;
                    // ...and say what was done with the set, every time: a set above secmaxmov
                    // is dropped, and the search then runs on psnap alone -- the same lie the set
                    // was for (a custom level's window at 10,116 objects), which must not read as covered.
                    char wb[240];
                    if (hold) {
                        g_wsnapHeld = g_movSet.size();
                        snprintf(wb, sizeof(wb), "secsolve: wsnap set %zu objects (secwsnapset=%d) - held "
                                 "back (cfg secwsnapgate): psnap alone first, the set only if that finds "
                                 "nothing", g_movSet.size(), g_wsnapSet);
                        g_movSet.clear();
                    } else if (!g_movSet.empty() && g_movSet.size() <= g_maxMoving) {
                        g_worldOn = true;
                        if (lieSkip)
                            snprintf(wb, sizeof(wb), "secsolve: wsnap set %zu objects (secwsnapset=%d) - "
                                     "put back from the start (cfg seclieskippsnap): this window's plain "
                                     "snapshot search parted from the game before", g_movSet.size(), g_wsnapSet);
                        else
                            snprintf(wb, sizeof(wb), "secsolve: wsnap set %zu objects (secwsnapset=%d) - "
                                     "put back with the player", g_movSet.size(), g_wsnapSet);
                    } else if (g_movSet.empty()) {
                        snprintf(wb, sizeof(wb), "secsolve: wsnap set empty (secwsnapset=%d) - "
                                 "the search runs on psnap alone", g_wsnapSet);
                    } else {
                        snprintf(wb, sizeof(wb), "secsolve: wsnap set %zu objects is over secmaxmov %zu "
                                 "(secwsnapset=%d) - NOT put back, the search runs on psnap alone",
                                 g_movSet.size(), g_maxMoving, g_wsnapSet);
                        g_movSet.clear();
                    }
                    writeResult(wb);
                }
            }
            const double qms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - q0).count();
            char qb[256];
            snprintf(qb, sizeof(qb),
                     "secsolve: snap qualify %s (%.0f ms, movingObjects=%d, "
                     "movingPortals=%d, verifyEvery=%d) - branching with %s",
                     g_snapOn ? "PASS" : "FAIL", qms, moved, movPortals,
                     g_verifyEvery,
                     !g_snapOn ? "CheckpointObject"
                               : (moved > 0 ? "psnap + a checked correction"
                                            : "psnap (3KB/1.4us)"));
            writeResult(qb);
        }
        // cfg seccpfallback=0: a window of the repair loop the snapshot cannot branch in is not
        // searched on checkpoints either. A checkpoint search costs ~20x a snapshot search per
        // restore; in the logs of 09-22..09-30 it found a crossing in 5 of 58 (level 22, 406-926 s
        // each; the others took 0-835 s for nothing), and level 19's in the bundle cold of 09-30
        // took 1,568 s. The window goes back to the loop as searched and exhausted.
        const bool cpRefused = !g_snapOn && g_secRung && !g_cfg.secCpFallback;
        if (cpRefused)
            writeResult("secsolve: not searched - the snapshot cannot branch in this window and "
                        "cfg seccpfallback=0 keeps the loop's searches off checkpoints");
        // Out-of-bounds ceiling (see the note on g_maxY). Look only at collidable objects,
        // and only within the x range this section reaches.
        //
        // Taking the whole level and every type is meaningless: lv22 has triggers stacked
        // at y=3,285 (decorations at 3,255), and with that as the ceiling not one arc
        // (y=3,200-3,900) can be dropped. With collidable objects only, the top around
        // here is about 900.
        g_maxYAuto = 0.0;
        if (g_maxY <= 0.0 && this->m_objects && m_player1) {
            const double x0 = m_player1->getPositionX();
            const double x1 = x0 + (double)g_horizon * 5.5 + 300.0;
            double top = 0.0;
            for (unsigned i = 0; i < this->m_objects->count(); ++i) {
                auto* o = static_cast<GameObject*>(
                    this->m_objects->objectAtIndex(i));
                const int ty = (int)o->m_objectType;
                if (ty == 7 || ty == 20) continue;      // decoration / trigger
                const double ox = o->getPositionX();
                if (ox < x0 - 300.0 || ox > x1) continue;
                top = std::max(top, (double)o->getPositionY());
            }
            if (top > 0.0) g_maxYAuto = top + 600.0;
        }
        const double killAboveY = g_maxY > 0.0 ? g_maxY : g_maxYAuto;

        secsolve::setSearching(true);
        // Only the search proper swallows deaths. The sweep (snapSweep) must detect death
        // to shorten its window, so do not set this before here.
        g_noKill = g_killOverride < 0 ? g_snapOn : (g_killOverride == 1);
        g_died = false;
        g_nodes.clear();
        releaseAnchors();
        secsolve::g_keepDropped = 0;   // cfg seckeepsize's count, this search
        // The head's `in` is the button as the checkpoint caught it (g_headHeld), not 0. The
        // expansion restores every node with its own `in` -- the head included -- and a head
        // stored as released turned the first held step of every branch into a fresh press.
        // A fresh press in flight is pushButton, which fires every ring the player is
        // touching: on a custom level t=23,929, a flipped ship holding through a yellow orb it
        // never fires in the game, took vy -11.23 on the first layer of every search from
        // that head and drifted 11.5 px off the plan it was replaying before the 20-layer
        // check put it back. The leaf and check replays already restored the head with
        // g_headHeld; only the expansion disagreed with them.
        g_nodes.push_back({-1, (uint8_t)(g_headHeld ? 1 : 0), 0, 0.f, 0.f, 0.f, 0,
                           (uint8_t)(m_player1 && m_player1->m_vehicleSize < 0.9f ? 1 : 0),
                           (uint8_t)(secDualP2() ? 1 : 0), secDualY2()});
        g_dash.assign(1, secsolve::DashState{});  // section head (input unused)
        g_vy.assign(1, m_player1 ? m_player1->m_yVelocity : 0.0);
        g_accel.assign(1, m_player1 ? m_player1->m_accelerationOrSpeed : 0.0);
        g_pad.assign(1, m_player1 && m_player1->m_touchedPad ? 1 : 0);
        secsolve::g_dash2.clear();
        secsolve::g_vy2.clear();
        secsolve::g_accel2.clear();
        secsolve::g_pad2.clear();
        secPushP2();                      // its dash is retaken below, as player 1's is
        // cfg secheadaccel: the head's accumulator and pad flag as the plain flight had them, not as
        // the qualify probes' last checkpoint load left the player (see secsolve::g_headAccel).
        if (secsolve::g_headAccel && !secsolve::g_headPlain.empty()) {
            const size_t one = sizeof(PlayerObject) + psnap::kExtra;
            const uint8_t* hp = secsolve::g_headPlain.data();
            std::memcpy(&g_accel[0], hp + offsetof(PlayerObject, m_accelerationOrSpeed), sizeof(double));
            g_pad[0] = hp[offsetof(PlayerObject, m_touchedPad)] ? 1 : 0;
            const size_t p2at = one + psnap::layerBytes();
            if (secP2() && !secsolve::g_accel2.empty() && secsolve::g_headPlain.size() >= p2at + one) {
                std::memcpy(&secsolve::g_accel2[0],
                            hp + p2at + offsetof(PlayerObject, m_accelerationOrSpeed), sizeof(double));
                secsolve::g_pad2[0] = hp[p2at + offsetof(PlayerObject, m_touchedPad)] ? 1 : 0;
            }
        }
        // The section head IS the spine's first node: the verified solution
        // passed through here, so following its own inputs from this node is
        // the one rollout that must never be pruned (see g_spineOn).
        g_spine = g_spineOn ? 0 : -1;
        g_spineNext = -1;

        // Frontier nodes hold their own checkpoint. In the version that replayed the
        // prefix every time, step count grew with the square of depth: 767,972 steps /
        // 154 s in one lv20 section, far above restores (20 s). With checkpoints, 1
        // expansion = 1 restore + 1 step. The key is holding only the frontier's worth;
        // holding hundreds to thousands goes back to the "hold a huge number of full
        // object states" that caused the old implementation's OOM.
        // Right after a restore there are substeps in which physics does not advance. The
        // count can vary with the reset path, so it is measured every time. Without the correction, the 1 step of "1 expansion
        // = restore + 1 step" is entirely a frozen step, and the frontier spins while x
        // never advances (which actually happened).
        int freeze = 0;
        {
            secRestoreFrom(pl, g_ckpt, g_headHeld);
            const double x0 = m_player1 ? m_player1->getPositionX() : 0.0;
            const double y0 = m_player1 ? m_player1->getPositionY() : 0.0;
            for (int i = 0; i < 8; ++i) {
                secStep(g_headHeld, dt);
                // Do not look only at x. In a rotated gameplay section x is frozen and y
                // advances, so "x does not move = physics did not advance" is always true
                // and freeze=8 is falsely detected (measured at lv22's entry t=1777). Then
                // every expansion idles 8 extra ticks and the search looks at a different
                // level. If either moved, physics advanced.
                if (m_player1 && (m_player1->getPositionX() != x0
                                  || m_player1->getPositionY() != y0)) break;
                ++freeze;
            }
        }
        // Dash state at the section head (the checkpoint does not carry it, so take it
        // ourselves)
        secRestoreFrom(pl, g_ckpt, g_headHeld);
        for (int f = 0; f < freeze; ++f) secStep(g_headHeld, dt);
        secCaptureDash(g_dash[0]);
        if (auto* q = secP2()) secCaptureDash(secsolve::g_dash2[0], q);
        // Baseline for the counter delta (Node::cnt). Taken once in the section-head world.
        secsolve::g_cntBase = secsolve::countSum(this);

        g_cps.assign(1, g_ckpt);          // nodes[0]'s checkpoint is the section checkpoint itself
        // Node state on the psnap path. Unlike checkpoints it is 3KB, so holding the whole
        // frontier is light.
        std::vector<std::vector<uint8_t>> snaps;
        // A cross-check anchor's snapshot, kept past the node's release for the next cross-check,
        // whose replay starts at the anchor's checkpoint (secsolve::g_cpPlayer). Same keys as
        // g_anchors.
        std::unordered_map<int, std::vector<uint8_t>> anchorSnaps;
        // cfg seccpcheck (print only): at each of the search's checkpoint loads, the members a load
        // was measured to leave behind, as the point had them / right after the load / after the
        // write-back (psnap::cpCheckLine). The first 8 of each site per search.
        int cpChecks[5] = {0, 0, 0, 0, 0};
        std::vector<uint8_t> cpLoad, cpAfter;
        auto cpTakeLoad = [&]() {
            if (psnap::g_cpCheck) psnap::captureRaw(m_player1, this, cpLoad);
        };
        auto cpReport = [&](int site, const char* name, long long at,
                            const std::vector<uint8_t>& origin) {
            if (!psnap::g_cpCheck || origin.empty() || cpLoad.empty() || cpChecks[site] >= 8) return;
            ++cpChecks[site];
            psnap::captureRaw(m_player1, this, cpAfter);
            writeResult(psnap::cpCheckLine(name, at, origin, cpLoad, cpAfter, secsolve::g_cpPlayer));
        };
        // The effect manager's queue (what CheckpointObject saves). Carried by C++
        // assignment, not bytes. Same index as snaps.
        std::vector<psnap::EMSnap> pulses;   // the effect manager per node (psnap::EMSnap)
        // GJGameState (in-progress moves/rotations, portals, object physics). The main
        // suspect.
        //
        // Held through a pointer, as the touch bookkeeping below is. A GJGameState is 1,744
        // bytes by itself (a Touch 240), and a released node gives back only what its element
        // owns, so a vector of the structs kept one per node ever made: 621k nodes, 1.2 GB of
        // elements (1.8 GB of capacity) at the end of one section search of a custom level
        // (09-26), each doubling copying all of them. A released node now keeps 8 bytes.
        std::vector<std::unique_ptr<GJGameState>> states;
        // ...and a released node's state goes here rather than being freed, for the next node
        // to take: a state assigned into one that has been used keeps its vectors' capacity and
        // its ordered maps' nodes (psnap::assignState), where a new one allocates every element
        // again and a freed one frees them. The pool never holds more than the most states that
        // were live at once, which is what the search needed anyway.
        std::vector<std::unique_ptr<GJGameState>> statePool;
        auto newState = [&statePool]() {
            if (statePool.empty()) return std::make_unique<GJGameState>();
            std::unique_ptr<GJGameState> s = std::move(statePool.back());
            statePool.pop_back();
            return s;
        };
        // The player-side "already touched" bookkeeping (orbs/pads/slopes)
        std::vector<std::unique_ptr<psnap::Touch>> touches;
        // ...and the object side of it: which portals, pads and rings in reach have been
        // activated (psnap::captureAct -- what GD's own checkpoint carries). Same index.
        std::vector<std::vector<uint8_t>> acts;
        // ...and its signature, one per node on EVERY path (the cap's buckets read it by node
        // index), 0 where nothing was captured. Mixed into the dedupe key and the buckets under
        // cfg secactkey (psnap::captureActSig).
        std::vector<uint64_t> actSigs(1, 0);
        std::unordered_set<uint64_t> actSigSeen;
        // `secorigin:` (print only): at each cross-check replay's origin, right after the
        // checkpoint is loaded, the objects the origin node had used and how many of them the
        // game has as unused again.
        long long originChecks = 0, originUsed = 0, originLost = 0;
        int actSelfCheck = -1;
        // The pose of moving objects (`wsnap`). In a section with moving objects this is
        // the core -- do for the moving objects what psnap does for the player.
        std::vector<std::vector<uint8_t>> worlds;
        // psnap does not go through resetLevel, so it has no frozen steps (1 restore + 1
        // step advances exactly 1 tick). The sweep confirms bit-identity with exactly this
        // procedure.
        const int snapFreeze = g_snapOn ? 0 : freeze;
        // The stacking check (`secoverlay`) takes the same snapshots on the checkpoint
        // path too.
        const bool keepSnaps = g_snapOn || g_overlay != 0;
        if (keepSnaps) {
            secRestoreFrom(pl, g_ckpt, g_headHeld);
            for (int i = 0; i < freeze; ++i) secStep(g_headHeld, dt);
            cpTakeLoad();
            // The head's player is the plain flight's, not what the load left (g_cpPlayer).
            if (secsolve::g_cpPlayer && !secsolve::g_headPlain.empty())
                psnap::restore(m_player1, this, secsolve::g_headPlain.data());
            // cfg secforcesize: after the plain flight's player, which would put the size back --
            // and before the head's snapshot, which every later return to the head restores.
            if (secsolve::g_forceP1Size >= 0 && m_player1
                && (m_player1->m_vehicleSize < 0.9f) != (secsolve::g_forceP1Size == 1)) {
                m_player1->togglePlayerScale(secsolve::g_forceP1Size == 1, true);
                writeResult(std::string("secsolve: player 1 forced to ")
                            + (secsolve::g_forceP1Size == 1 ? "mini" : "normal")
                            + " size at the head (cfg secforcesize) - a probe, not a route");
            }
            cpReport(0, "head", g_ckptTick, secsolve::g_headPlain);
            snaps.resize(1);
            pulses.resize(1);
            states.resize(1);
            touches.resize(1);
            acts.resize(1);
            states[0] = std::make_unique<GJGameState>();
            touches[0] = std::make_unique<psnap::Touch>();
            psnap::capture(m_player1, this, snaps[0]);
            psnap::captureEM(this, pulses[0]);
            psnap::captureState(this, *states[0]);
            psnap::captureTouch(m_player1, *touches[0], this);
            // The objects in reach: from a little behind the head to as far as the horizon
            // can carry the fastest speed (4.0 px/tick is above every speed GD has).
            {
                const double hx = m_player1 ? m_player1->getPositionX() : 0.0;
                psnap::buildActWindow(this, hx - 300.0, hx + 4.0 * (double)g_horizon + 300.0);
                actSigs[0] = psnap::captureActSig(acts[0]);
                actSigSeen.insert(actSigs[0]);
                if (m_player1)
                    actSelfCheck = psnap::actKeySelfCheck(
                        acts[0], keyOf(m_player1->getPositionY(), m_player1->m_yVelocity,
                                       (int)modeIdx(m_player1),
                                       m_player1->m_vehicleSize < 0.9f ? 1 : 0,
                                       m_player1->m_isUpsideDown ? 1 : 0, g_headHeld,
                                       hx, m_player1->m_isDashing ? 1 : 0, 0));
            }
            if (g_worldOn) {
                worlds.resize(1);
                psnap::captureWorld(g_movSet, worlds[0]);
            }
        }
        // cfg secbound: the level's box, from where every object stands at the section head.
        g_boundSkips = 0;
        g_shaderSkips = 0;
        if (g_shaderSig) writeResult("shadersig: search start " + shaderSig(this));
        g_frames = 0;
        g_frameGapMs = 0.0;
        g_overlayMs = 0.0;
        g_outsideMs = 0.0;
        g_sliceEnded = false;
        if (g_boundOn && this->m_objects) {
            double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
            for (unsigned i = 0; i < this->m_objects->count(); ++i) {
                auto* o = static_cast<GameObject*>(this->m_objects->objectAtIndex(i));
                if (!o) continue;
                x0 = std::min(x0, o->m_positionX); x1 = std::max(x1, o->m_positionX);
                y0 = std::min(y0, o->m_positionY); y1 = std::max(y1, o->m_positionY);
            }
            g_boxX0 = x0 - kBoundMargin; g_boxX1 = x1 + kBoundMargin;
            g_boxY0 = y0 - kBoundMargin; g_boxY1 = y1 + kBoundMargin;
        }
        // cfg seccoins (see secsolve::g_secCoins): the coins this search has to take itself. Bit c
        // of a node's word = coin c is taken, or does not count (taken or passed before the head,
        // not in the level). The loop's own coin bookkeeping is the attempt's, and a search moves
        // the player through it thousands of times: it is put back as it was when the search ends.
        struct CoinBook {
            std::vector<long long> pickup = solver::g_coinPickupTick, gd = solver::g_coinGdTick;
            std::vector<uint8_t> said = solver::g_coinLiveSaid;
            std::map<int, int> items = solver::g_itemCounts;
            bool missFired = solver::g_coinMissFired;
            float prevX = solver::g_prevTickX;
            bool armed = true;   // cfg secbookfirst: put back before the reset, not at the end
            void restore() {
                solver::g_coinPickupTick = pickup; solver::g_coinGdTick = gd;
                solver::g_coinLiveSaid = said; solver::g_itemCounts = items;
                solver::g_coinMissFired = missFired; solver::g_prevTickX = prevX;
            }
            ~CoinBook() {
                if (armed) restore();
            }
        } coinBook;
        // Unlike the loop's coinmiss, not off for a whole level with rotated gameplay (all three
        // SubZero levels have it): the touch test is two boxes in world space, and "left behind" is
        // judged per step, only while the player runs forward (below).
        const bool coinsOn = g_secCoins && g_cfg.coinRoute && !solver::g_coins.empty()
                             && solver::g_coins.size() <= 32 && m_player1 && this->m_objects;
        std::vector<GameObject*> coinObj;
        std::vector<uint8_t> coinFinal;   // passing coin c forward ends its chance (set below)
        std::vector<uint32_t> coinBits(1, 0u);
        long long coinMissBranches = 0;
        long long coinLessLeaves = 0;     // cfg dpseccoinrung: leaves without the owed coin
        // The gameplay frame at the head, and the rotations run by then (see `forward` below).
        int coinFrame = -1;
        long long coinRotAt = 0;
        if (coinsOn) {
            coinObj.assign(solver::g_coins.size(), nullptr);
            for (unsigned i = 0; i < this->m_objects->count(); ++i) {
                auto* o = static_cast<GameObject*>(this->m_objects->objectAtIndex(i));
                if (!o) continue;
                for (size_t c = 0; c < solver::g_coins.size(); ++c)
                    if (solver::g_coins[c].uid == o->m_uniqueID) coinObj[c] = o;
            }
            const float hx = m_player1->getPositionX();
            // What the plan had taken by the head, as GD credited it when the loop flew the plan
            // (the anchor row's coin bits): the replay that brought the player here ran in practice
            // mode, where GD credits nothing, so the attempt's own pickup ticks are empty.
            const AnchorRow* headRow = anchors::row(secsolve::g_startTick);
            // Taken BY THE HEAD: a pickup tick after it is the flight that went on past the head
            // before the search restored the checkpoint there, and a checkpoint restore keeps the
            // attempt's coin record (hooks_playlayer). lv22 t=8,194: the plan being repaired took
            // coin 1 at x=10,513, 93 px ahead of the head; the search counted it taken, spliced a
            // route below it and pinned it, and every rung past the pin then had no state.
            const long long headT = secsolve::g_startTick;
            const auto byHead = [headT](const std::vector<long long>& v, size_t c) {
                return c < v.size() && v[c] >= 0 && v[c] <= headT;
            };
            int lateTicks = 0;
            coinFinal.assign(coinObj.size(), 1);
            uint32_t head = 0;
            for (size_t c = 0; c < coinObj.size(); ++c) {
                // Passing a coin forward is final only where the loop's own cut calls it so: not
                // for a coin the search says something beyond turns the player back to
                // (g_coinNoMiss), nor while a Move that carries it is still ahead (per step,
                // below). SubZero 4002's third coin drops 600 px after it is passed and is taken
                // on the way back.
                if (std::find(solver::g_coinNoMiss.begin(), solver::g_coinNoMiss.end(),
                              solver::g_coins[c].uid) != solver::g_coinNoMiss.end())
                    coinFinal[c] = 0;
                // ...except the coin a coin-wall rung was fired for (cfg dpseccoinrung): this
                // window is where it is owed.
                if ((int)c == secsolve::g_rungCoin) coinFinal[c] = 1;
                const bool taken = (headRow && c < 8 && ((headRow->coins >> c) & 1))
                                   || byHead(solver::g_coinGdTick, c)
                                   || byHead(solver::g_coinPickupTick, c);
                if (!taken && ((c < solver::g_coinGdTick.size() && solver::g_coinGdTick[c] > headT)
                               || (c < solver::g_coinPickupTick.size()
                                   && solver::g_coinPickupTick[c] > headT)))
                    ++lateTicks;
                const bool moveAhead = c < solver::g_coinMoveX.size()
                                       && solver::g_coinMoveX[c] > -1e8f
                                       && hx <= solver::g_coinMoveX[c];
                bool passed = coinObj[c] && coinFinal[c] && !moveAhead
                              && !m_player1->m_isGoingLeft && !m_player1->m_isSideways
                              && hx > coinObj[c]->getObjectRect().getMaxX() + 15.f;
                // cfg secrungcoinoff: the coin this rung was fired for, passed at the head while
                // its group is off, is not taken -- GD never credited it, and counting it as done
                // lets every leaf answer the rung (SubZero 4003, head t=10,596: bits 7 -> 7).
                if (passed && secsolve::g_rungCoinOff && (int)c == secsolve::g_rungCoin
                    && coinObj[c]->m_isGroupDisabled)
                    passed = false;
                if (!coinObj[c] || taken || passed) head |= 1u << c;
            }
            coinBits[0] = head;
            // The anchor row's frame is the replay's, written as it passed the head; g_gameFrame is
            // whatever the flight past the head left it at.
            coinFrame = headRow ? headRow->gframe : g_gameFrame;
            coinRotAt = g_rotGameplayCalls;
            int left = 0;
            for (size_t c = 0; c < coinObj.size(); ++c)
                if (!(head & (1u << c))) ++left;
            char cb[256];
            snprintf(cb, sizeof(cb), "secsolve: coins - %zu in the level, %d still to take past the "
                     "head (x=%.0f); the search takes them itself (GD collects none in practice "
                     "mode); frame %d camera %.1f; %d coin(s) touched only after the head",
                     coinObj.size(), left, (double)hx, coinFrame,
                     (double)this->m_gameState.m_cameraAngle, lateTicks);
            writeResult(cb);
        }
        long long rephases = 0;
        bool needWake = false;   // previous branch died -> wake the level before next expansion
        // Continued psnap branching advances the world's phase by 1 tick per expansion. A
        // safety valve that periodically pulls it back with a checkpoint, to stay within
        // the range confirmed by the sweep (5,000 ticks).
        auto rephase = [&]() {
            if (!g_snapOn) return;
            secRestoreFrom(pl, g_ckpt, g_headHeld);
            for (int i = 0; i < freeze; ++i) secStep(g_headHeld, dt);
            ++rephases;
        };
        std::vector<int> cur{0}, nxt;
        std::unordered_set<long long> seen;
        // Number of live checkpoints at once. Before a layer is pruned, cur + nxt = up to
        // 3*cap are held, so this is where to look for a return of the OOM (the old
        // implementation's failure was a mix-up of exactly this).
        long long restores = 0, steps = 0, cpMade = 0, cpPeak = 0;
        int foundLeaf = -1;
        double foundX = 0.0;
        int foundDepth = 0;
        int deepest = 0;
        // Why the search stopped, for the report line: "none" is the ordinary
        // end (exhausted, or a leaf crossed). See g_deadlineSec.
        const char* stopWhy = "none";
        secsolve::g_nonFinite = 0;   // objects handed to re-bucketing at a non-finite position
        // Results of the leaf cross-check (filled by evalLeaf below). Used in the report.
        g_leafVerified = false;
        g_leafDeadAt = -1;
        double verifyX = 0.0, verifyY = 0.0, verifyVy = 0.0;
        int splitAt = -1;
        double splitNodeX = 0, splitRepX = 0, splitNodeY = 0, splitRepY = 0;
        double splitNodeVy = 0, splitRepVy = 0;
        int graceOk = -1, graceDeadAt = -1;
        long long doomedLeaves = 0;      // number of exits discarded as dead ends
        long long unverLeaves = 0;       // ...and, in a rung, as not reproduced by the replay
        double maxVerifyDrift = 0.0;     // size of psnap's lie (max diff in the cross-check pass)

        auto releaseCp = [&](int ni) {
            if (ni <= 0) return;                       // the section checkpoint is never released
            if (keepSnaps) {
                // Assignment or clear() does not give memory back. Containers only set size
                // to 0 and keep the allocated buffer, so swap with an empty temporary and
                // let the temporary carry it away.
                //
                // Getting this wrong made GD grow 27MB per layer, reaching 14GB around depth
                // 400 and putting the whole machine into swap (lv20's ship section fell to
                // 9 s/layer; psnap is normally 7ms/layer). Only snaps used swap correctly;
                // the other three were assignments.
                postmortem::g_secPhase = 80;
                if ((size_t)ni < snaps.size())
                    std::vector<uint8_t>().swap(snaps[(size_t)ni]);
                postmortem::g_secPhase = 81;
                if ((size_t)ni < pulses.size()) {
                    psnap::EMSnap tmp;
                    std::swap(pulses[(size_t)ni], tmp);
                }
                postmortem::g_secPhase = 82;
                if ((size_t)ni < states.size() && states[(size_t)ni])
                    statePool.push_back(std::move(states[(size_t)ni]));
                postmortem::g_secPhase = 83;
                if ((size_t)ni < touches.size()) touches[(size_t)ni].reset();
                postmortem::g_secPhase = 84;
                if ((size_t)ni < worlds.size())
                    std::vector<uint8_t>().swap(worlds[(size_t)ni]);
                // The used-object bytes too (up to 533 a node, 316 MB over one search of
                // a custom level) -- except a cross-check anchor's, which the next cross-check puts
                // back at its replay's origin. The anchor swap below frees those.
                postmortem::g_secPhase = 85;
                if ((size_t)ni < acts.size() && !g_anchors.count(ni))
                    std::vector<uint8_t>().swap(acts[(size_t)ni]);
                postmortem::g_secPhase = 86;
                if (g_snapOn) return;                  // the psnap path holds no checkpoint
            }
            if ((size_t)ni >= g_cps.size()) return;
            if (g_cps[(size_t)ni]) { g_cps[(size_t)ni]->release(); g_cps[(size_t)ni] = nullptr; }
        };

        // Is the state controllable (used for exit survivability)
        auto controllable = [](PlayerObject* p) {
            if (!p) return false;
            const int md = modeIdx(p);
            // ship / UFO / wave / swing accept input even in the air
            if (md == 1 || md == 3 || md == 4 || md == 7) return true;
            return p->m_isOnGround != 0;
        };
        auto flyMode = [](PlayerObject* p) {
            const int md = p ? modeIdx(p) : -1;
            return md == 1 || md == 3 || md == 4 || md == 7;
        };
        // cfg secgracedash: a body riding a dash ring's dash is controllable -- the release ends
        // the dash (see secsolve::g_graceDash). Either body in a dual.
        auto dashing = [this]() {
            if (m_player1 && m_player1->m_isDashing) return true;
            return m_gameState.m_isDualMode && m_player2 && m_player2 != m_player1
                   && m_player2->m_isDashing;
        };

        // ---- Leaf cross-check (plain replay + exit survivability) -------------------
        // Verify "on the spot" when the goal is reached. Formerly the cross-check ran once
        // after stopping the search, so if the first leaf that arrived was a dead end, the
        // search ended there. The right thing is to discard the dead-end exit and keep
        // searching (there may be other paths).
        // Return 0=SOLVED / 1=UNVERIFIED (not reproduced by replay) / 2=DOOMED (exit is a
        // dead end)
        // The last leaf check's input and node lists (size, capacity), for the containment's line:
        // they live inside evalLeaf and are gone by the time an exception reaches the catch.
        size_t leafSeqN = 0, leafSeqCap = 0, leafPathN = 0, leafPathCap = 0;
        auto evalLeaf = [&](int leaf) -> int {
            g_leafVerified = false;
            g_leafDeadAt = -1;
            splitAt = -1;
            graceOk = -1;
            graceDeadAt = -1;
            verifyX = verifyY = verifyVy = 0.0;
            std::vector<uint8_t> seq;
            std::vector<int> path;
            for (int i = leaf; i > 0; i = g_nodes[(size_t)i].parent) {
                seq.push_back(g_nodes[(size_t)i].in);
                path.push_back(i);
            }
            std::reverse(seq.begin(), seq.end());
            std::reverse(path.begin(), path.end());
            leafSeqN = seq.size(); leafSeqCap = seq.capacity();
            leafPathN = path.size(); leafPathCap = path.capacity();
            secRestoreFrom(pl, g_ckpt, g_headHeld);
            for (int f = 0; f < freeze; ++f) secStep(g_headHeld, dt);
            cpTakeLoad();
            if (secsolve::g_cpPlayer && keepSnaps && !snaps.empty() && !snaps[0].empty())
                psnap::restore(m_player1, this, snaps[0].data());
            cpReport(1, "leaf", (long long)seq.size(), secsolve::g_headPlain);
            bool dead = false;
            for (size_t i = 0; i < seq.size(); ++i) {
                g_died = false;
                secStepBegin(pl);
                secStep((int)seq[i], dt); ++steps;
                if (!m_player1 || m_player1->m_isDead || g_died) {
                    dead = true; g_leafDeadAt = (int)i + 1; break;
                }
                if (splitAt < 0) {
                    const auto& nd = g_nodes[(size_t)path[i]];
                    // The physics position, as the node recorded it (psnap::physPosition).
                    const cocos2d::CCPoint at = psnap::physPosition(m_player1, this);
                    const double rx = at.x;
                    const double ry = at.y;
                    const double rv = (double)m_player1->m_yVelocity;
                    if (std::fabs(rx - (double)nd.x) > 0.02
                        || std::fabs(ry - (double)nd.y) > 0.02
                        || std::fabs(rv - (double)nd.vy) > 0.02) {
                        splitAt = (int)i + 1;
                        splitNodeX = nd.x; splitRepX = rx;
                        splitNodeY = nd.y; splitRepY = ry;
                        splitNodeVy = nd.vy; splitRepVy = rv;
                    }
                }
            }
            if (dead || !m_player1) return 1;
            {
                // Judged where the physics stands: during a mirror transition the node is at
                // its drawing place (psnap::physPosition).
                const cocos2d::CCPoint at = psnap::physPosition(m_player1, this);
                verifyX = at.x;
                verifyY = at.y;
            }
            verifyVy = (double)m_player1->m_yVelocity;
            if (!reachedGoal(verifyX, verifyY, (int)seq.size())) return 1;
            g_leafVerified = true;
            if (g_grace <= 0) return 0;
            // Exit survivability (see the note on g_grace). Two runs are not enough -- the
            // spider can teleport even in the air if a surface is in range, so "never
            // press" and "always hold" both dying is no proof that "input can do nothing".
            static const int kGracePeriods[] = {0, -1, 2, 3, 4, 6, 8, 12, 20, 40};
            CheckpointObject* lcp = markCheckpointAtPhys(pl);
            if (!lcp) return 0;
            lcp->retain();
            // ...and the player at the exit, for the loads of lcp below (g_cpPlayer).
            std::vector<uint8_t> lcpSnap, lcpOrigin;
            if (secsolve::g_cpPlayer && keepSnaps) psnap::captureRaw(m_player1, this, lcpSnap);
            if (psnap::g_cpCheck) psnap::captureRaw(m_player1, this, lcpOrigin);
            graceOk = 0;
            for (size_t pi = 0;
                 pi < sizeof(kGracePeriods) / sizeof(int) && graceOk == 0; ++pi) {
                const int per = kGracePeriods[pi];
                if (pi) {
                    secRestoreFrom(pl, lcp, 0); ++restores;
                    for (int f = 0; f < freeze; ++f) secStep(0, dt);
                    cpTakeLoad();
                    if (!lcpSnap.empty()) psnap::restore(m_player1, this, lcpSnap.data());
                    cpReport(2, "exit", (long long)pi, lcpOrigin);
                }
                bool gdead = false;
                for (int i = 0; i < g_grace; ++i) {
                    const int in = per == 0 ? 0 : per < 0 ? 1
                                 : ((i % per) == 0 ? 1 : 0);
                    g_died = false;
                    secStep(in, dt); ++steps;
                    if (!m_player1 || m_player1->m_isDead || g_died) {
                        gdead = true;
                        if (pi == 0) graceDeadAt = i + 1;
                        break;
                    }
                    // A mode that steers in the air is controllable on every tick, including
                    // the last ones before a wall it can no longer miss; with secgracefly it
                    // has to live that many ticks first (see g_graceFly).
                    if ((controllable(m_player1) || (secsolve::g_graceDash && dashing()))
                        && (g_graceFly <= 0 || !flyMode(m_player1) || i + 1 >= g_graceFly)) {
                        graceOk = 1;
                        break;
                    }
                }
                if (!gdead && graceOk == 0) graceOk = 1;
            }
            // cfg secleaflife=N (print only): how long the accepted exit lives under the best of
            // the same lines, up to N ticks -- whether a leaf is a dead end a few ticks on.
            if (graceOk == 1 && g_leafLife > 0) {
                int life = 0, lifeAt = -1;
                for (size_t pi = 0; pi < sizeof(kGracePeriods) / sizeof(int); ++pi) {
                    const int per = kGracePeriods[pi];
                    secRestoreFrom(pl, lcp, 0); ++restores;
                    for (int f = 0; f < freeze; ++f) secStep(0, dt);
                    if (!lcpSnap.empty()) psnap::restore(m_player1, this, lcpSnap.data());
                    int i = 0;
                    for (; i < g_leafLife; ++i) {
                        const int in = per == 0 ? 0 : per < 0 ? 1 : ((i % per) == 0 ? 1 : 0);
                        g_died = false;
                        secStep(in, dt); ++steps;
                        if (!m_player1 || m_player1->m_isDead || g_died) break;
                    }
                    if (i > life) { life = i; lifeAt = per; }
                }
                char lb2[96];
                snprintf(lb2, sizeof(lb2), "secleaf: depth=%zu life=%d/%d line=%d",
                         seq.size(), life, g_leafLife, lifeAt);
                writeResult(lb2);
            }
            lcp->release();
            return graceOk == 0 ? 2 : 0;
        };

        // Layer breakdown (cfg `seclog=1`). Report separately whether it thinned by dead /
        // dedupe / cap. If the frontier thins in a layer the cap is not binding, the wall
        // is real; if the cap is binding, it is a cap or granularity matter. Reading a wall
        // as real from "depth stops growing" alone, without this split, was the weak point
        // of update 32.
        int layDead = 0, layDup = 0, layCap = 0;
        long long capTotal = 0;   // nodes the cap dropped over the whole search (all layers)
        double layMaxX = 0.0, layMinY = 0.0, layMaxY = 0.0, layMinX = 0.0;
        double deadMaxX = 0.0;
        // What the verified plan holds at this layer -- the spine's own input.
        //
        // WHICH tick's plan input a layer should apply is not derived here, it is
        // swept (`secspineoff`). Reasoning it out from "the dump writes after
        // ++g_tick" and "the child at depth d is the state after tick ckpt+d+1"
        // produced two different answers and the first one tried made the
        // tracking WORSE (depth 10 and 1.30 px became depth 9 and 2.50). The knob
        // exists so the answer comes from the trajectory instead -- the same
        // instruction the note at secoff already gives for the verify path.
        //
        // Hoisted out of the dedupe so the step itself can be labelled as the
        // spine's BEFORE it is taken, which is what the per-substep print needs:
        // deciding afterwards tells you which child was the spine but not which
        // substep to watch.
        auto spinePlanHeld = [this](int depth) {
            const long long tHere = g_ckptTick + depth + g_spineOff;
            int held = 0;
            for (const auto& in : g_cfg.inputs) {
                if (in.step > tHere) break;
                if (!in.p2) held = in.down ? 1 : 0;
            }
            return held;
        };
        // The spine as a check on the snapshot (secsolve::g_spineUntil): noted when a layer ends
        // without the spine's child while the plan it flies was still alive in the game there.
        // The search goes on -- whatever it finds is replayed from the head before it counts, so a
        // snapshot that is wrong about the spine cannot make a wrong answer, only miss a right one;
        // a search that then finds nothing is asked again on checkpoints (the rung's end).
        secsolve::g_psnapLied = false;
        secsolve::g_spineLostT = -1;
        auto spineLied = [&](int depth) {
            if (!g_snapOn || secsolve::g_spineUntil <= 0 || g_spine < 0 || secsolve::g_psnapLied)
                return;
            if (g_spineNext >= 0 && std::find(nxt.begin(), nxt.end(), g_spineNext) != nxt.end())
                return;
            const long long t = (long long)g_ckptTick + 1 + depth;
            if (t >= secsolve::g_spineUntil - secsolve::kSpineSlack) return;
            secsolve::g_psnapLied = true;
            secsolve::g_spineLostT = t;
        };
        // An exception out of the search gives the search up, not the game. The search's coroutine
        // would hand it to std::terminate (SecTask's unhandled_exception), which ended the whole
        // game: a custom level's window at t=9,628 (depth 1,654) threw "vector too long" at depth 66 in every
        // run, with and without cfg cpflight. Caught around the layers, it ends only the search: the
        // layer being built is let go, the verdict is EXHAUSTED with stop=exception, and everything
        // after the layers -- the checkpoints and anchors released, the rung's hand-back, the level
        // reset, the render hold let go -- runs as after any search that found nothing. Where it was
        // is printed (the step is postmortem::g_secPhase), and how far the search's lists had grown,
        // length_error (a list asked past its limit) and bad_alloc (the process out of memory) apart.
        auto giveUp = [&](const char* kind, const char* what) {
            char eb[320];
            snprintf(eb, sizeof(eb), "secsolve: gave up the search - an exception at depth %d (step %d, node %d): %s",
                     (int)postmortem::g_secPhaseDepth, (int)postmortem::g_secPhase,
                     (int)postmortem::g_secPhaseNode, what);
            writeResult(eb);
            char sb[480];
            snprintf(sb, sizeof(sb), "secsolve: exception kind=%s stage=%s depth=%d node=%d size/capacity "
                     "cur=%zu/%zu nxt=%zu/%zu nodes=%zu/%zu snaps=%zu/%zu acts=%zu/%zu path=%zu/%zu "
                     "seq=%zu/%zu", kind, postmortem::secPhaseName((int)postmortem::g_secPhase),
                     (int)postmortem::g_secPhaseDepth, (int)postmortem::g_secPhaseNode, cur.size(),
                     cur.capacity(), nxt.size(), nxt.capacity(), g_nodes.size(), g_nodes.capacity(),
                     snaps.size(), snaps.capacity(), acts.size(), acts.capacity(), leafPathN,
                     leafPathCap, leafSeqN, leafSeqCap);
            writeResult(sb);
            for (int ni : nxt) releaseCp(ni);
            nxt.clear();
            foundLeaf = -1;
            stopWhy = "exception";
        };
        try {
        if (cpRefused) stopWhy = "no-cp";
        for (int depth = 1; depth <= g_horizon && foundLeaf < 0 && !cpRefused; ++depth) {
            nxt.clear();
            seen.clear();
            layDead = layDup = layCap = 0;
            layMaxX = deadMaxX = 0.0;
            layMinY = 1e9; layMaxY = -1e9; layMinX = 1e9;
            g_depth = depth;
            postmortem::g_secPhaseDepth = depth;
            postmortem::g_secPhase = 1;
            // The search's own clock (cfg `secdeadline`). Checked HERE, at the
            // top of a layer, because this is the one place where `cur` is the
            // live frontier and `nxt` is empty -- breaking anywhere later would
            // leave the next layer's checkpoints unreleased.
            const double elapsedS = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            if (g_deadlineSec > 0.0 && depth > 1) {
                if (elapsedS >= g_deadlineSec) { stopWhy = "deadline"; break; }
                // ...and the projection: at this rate, can the horizon be
                // reached at all before the deadline? Checked every 20 layers
                // rather than every layer so that early noise cannot end a
                // search that is merely warming up.
                if (depth >= 40 && depth % 20 == 0) {
                    const double perLayer = elapsedS / (double)(depth - 1);
                    if (elapsedS + perLayer * (double)(g_horizon - depth + 1)
                        > g_deadlineSec) {
                        stopWhy = "projected";
                        break;
                    }
                }
            }
            const double layerT0 = elapsedS;
            const long long layerR0 = restores;
            const double layerRestMs0 = g_secRestMs;
            const double layerStoreMs0 = g_secStoreMs;
            const double layerResetMs0 = g_secResetMs;
            const double layerSnapMs0 = g_secSnapMs;
            const double layerStepMs0 = g_secStepMs;
            // cfg secleafmid: a layer whose children can be the answer is expanded from the
            // middle of the band -- then its lowest edge, then outwards from the middle -- so the
            // first leaf taken is not the edge a flying mode hugs (see g_leafMid), while a ground
            // mode, whose middle is in the air, still reaches its floor second rather than after
            // every mid-jump exit has been tried and found a dead end (lv4003 w2: 104 of them,
            // 50,805 steps -> 109,735, and the leaf it ended on sat at y 311 over the floor's 309).
            // Every other layer keeps its order.
            if (g_leafMid && g_targetDepth > 0 && depth >= g_targetDepth && cur.size() > 2) {
                std::vector<int> byY(cur);
                std::stable_sort(byY.begin(), byY.end(), [&](int a, int b) {
                    return g_nodes[(size_t)a].y < g_nodes[(size_t)b].y;
                });
                const size_t mid = byY.size() / 2;
                cur.clear();
                cur.push_back(byY[mid]);
                cur.push_back(byY[0]);
                for (size_t k = 1; cur.size() < byY.size(); ++k) {
                    if (mid + k < byY.size()) cur.push_back(byY[mid + k]);
                    if (k <= mid && mid - k > 0) cur.push_back(byY[mid - k]);
                }
            }
            // cfg seccoins: ...and at a layer that can hold the answer, the parents that have taken
            // more coins go first (stable, so the order above holds among equals). Without it the
            // first leaf alive is taken whatever it holds, and a coin the window's route passes
            // on the way back is left to a pin the ladder cannot plan past.
            if (coinsOn && g_targetDepth > 0 && depth >= g_targetDepth && cur.size() > 1) {
                auto pc = [&](int n) {
                    uint32_t b = (size_t)n < coinBits.size() ? coinBits[(size_t)n] : 0u;
                    int k = 0;
                    for (; b; b &= b - 1) ++k;
                    return k;
                };
                std::stable_sort(cur.begin(), cur.end(), [&](int a, int b) { return pc(a) > pc(b); });
            }
            for (int ni : cur) {
                if (foundLeaf >= 0) break;
                // Hand the frame back BETWEEN EXPANSIONS as well, not only between layers. A
                // layer's cost is the cap's (measured 1.7 s per layer at cap 120, lv22's band),
                // and Windows calls a window that has not pumped for five seconds dead -- so
                // layer granularity alone would put the ghost window back the moment somebody
                // raised the cap. This is the same kind of boundary as the one at the end of the
                // layer: an expansion begins by restoring, so the world between two of them is
                // not a state anything reads.
                if (g_sliceMs > 0 && sliceExpired()) co_await std::suspend_always{};
                postmortem::g_secPhaseNode = ni;
                postmortem::g_secPhase = 2;
                CheckpointObject* base = nullptr;
                if (!g_snapOn) {
                    base = (size_t)ni < g_cps.size() ? g_cps[(size_t)ni] : nullptr;
                    if (!base) continue;
                } else if ((size_t)ni >= snaps.size() || snaps[(size_t)ni].empty()) {
                    continue;
                }
                for (int branch = 0; branch < 2; ++branch) {
                    g_died = false;          // pick up this step's death verdict
                    if (g_snapOn) {
                        const auto snapT0 = std::chrono::steady_clock::now();   // print only
                        // Deaths are swallowed, so no wake is needed. Only the periodic
                        // phase pull-back remains, as a safety valve.
                        if (g_rephase > 0 && restores > 0
                            && restores % g_rephase == 0) rephase();
                        // Restore the world first. Restoring the player triggers no
                        // collision test, but restoring the world goes through setPosition,
                        // so section re-registration runs. Keep the order fixed.
                        postmortem::g_secPhase = 10;
                        if (g_worldOn && (size_t)ni < worlds.size())
                            psnap::restoreWorld(this, g_movSet, worlds[(size_t)ni]);
                        postmortem::g_secPhase = 11;
                        psnap::restore(m_player1, this, snaps[(size_t)ni].data());
                        postmortem::g_secPhase = 12;
                        psnap::restoreEM(this, pulses[(size_t)ni]);
                        postmortem::g_secPhase = 13;
                        if (states[(size_t)ni]) psnap::restoreState(this, *states[(size_t)ni]);
                        postmortem::g_secPhase = 14;
                        if (touches[(size_t)ni])
                            psnap::restoreTouch(m_player1, *touches[(size_t)ni], this);
                        postmortem::g_secPhase = 15;
                        if ((size_t)ni < acts.size()) psnap::restoreAct(acts[(size_t)ni]);
                        postmortem::g_secPhase = 16;
                        // The wave trail is not in the snapshot (see g_trailClear)
                        if (secsolve::g_trailClear) {
                            for (PlayerObject* p : {m_player1, m_player2})
                                if (p && p->m_waveTrail && p->m_waveTrail->m_pointArray)
                                    p->m_waveTrail->m_pointArray->removeAllObjects();
                        }
                        // Buttons are not injected (`m_holdingButtons` is PRESERVE). Put
                        // them into a known state by the same steps as the checkpoint path,
                        // then feed the branch input.
                        g_injecting = true;
                        this->handleButton(false, 1, true);
                        g_injecting = false;
                        g_held = 0;
                        // A branch that keeps holding re-holds without an edge (see the
                        // note on secArmHold)
                        if (g_nodes[(size_t)ni].in) secArmHold();
                        // Dash is not carried either (the mask does not copy pointers, so
                        // `m_dashRing` is lost). See the note on DashState.
                        if ((size_t)ni < g_dash.size()) {
                            secRestoreDash(g_dash[(size_t)ni]);
                            secRestoreJb(g_dash[(size_t)ni]);
                        }
                        // The boost accumulator is not in the psnap mask either
                        // (see g_accel). Same field, same reason, both paths.
                        if (m_player1 && (size_t)ni < g_accel.size()) {
                            m_player1->m_accelerationOrSpeed = g_accel[(size_t)ni];
                            m_player1->m_touchedPad = g_pad[(size_t)ni] != 0;
                        }
                        // ...both bodies (see g_dash2).
                        secRestoreP2((size_t)ni, false);
                        // Rebuild the candidate list. On the checkpoint path this happens by
                        // itself through the 2 frozen steps, but the psnap path has no
                        // freeze.
                        if (secsolve::g_visRefresh) {
                            this->preUpdateVisibility(0.f);
                            this->updateVisibility(0.f);
                        }
                        ++restores;
                        g_secSnapMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - snapT0).count();
                    } else {
                        secRestoreFrom(pl, base, g_nodes[(size_t)ni].in);
                        ++restores;
                        // Dash is not in the checkpoint. Restore it here
                        if ((size_t)ni < g_dash.size()) {
                            secRestoreDash(g_dash[(size_t)ni]);
                            secRestoreJb(g_dash[(size_t)ni]);
                        }
                        // ...and the exact y velocity, which the restore puts
                        // back on the 0.001 grid (hole 3, measured:
                        // 1.9815 comes back 1.982). One restore loses half a
                        // thousandth; a search restores ONCE PER TICK, so a
                        // 233-deep chain rounds 233 times. That is the
                        // difference between "restore once and run 300 ticks
                        // bit-identically" -- which this snapshot does -- and
                        // a search from the same head dying at depth 233.
                        if ((size_t)ni < g_vy.size() && m_player1)
                            m_player1->m_yVelocity = g_vy[(size_t)ni];
                        // ...and the boost accumulator and its pad flag, which
                        // the restore does not carry at all (see g_accel). Put
                        // back with the same cfg switch off by default? No --
                        // unconditionally, like the dash: a node whose boost
                        // budget is a leftover from whichever node was stepped
                        // last is not that node.
                        if (m_player1 && (size_t)ni < g_accel.size()) {
                            m_player1->m_accelerationOrSpeed = g_accel[(size_t)ni];
                            m_player1->m_touchedPad = g_pad[(size_t)ni] != 0;
                        }
                        // ...both bodies (see g_dash2).
                        secRestoreP2((size_t)ni, true);
                        // Idle the frozen ticks with "the input that node was holding"
                        // (physics does not advance so the state is unchanged, but button
                        // continuity is kept)
                        for (int f = 0; f < snapFreeze; ++f) {
                            secStep(g_nodes[(size_t)ni].in, dt); ++steps;
                        }
                        // Stacking check. Should be the identity -- the same node's state is
                        // merely restored again with psnap after the checkpoint restore. If
                        // the output moves, psnap is the side that breaks things.
                        if (g_overlay && (size_t)ni < snaps.size()
                            && !snaps[(size_t)ni].empty()) {
                            if (g_overlay & 1)
                                psnap::restore(m_player1, this,
                                               snaps[(size_t)ni].data());
                            if (g_overlay & 2) psnap::restoreEM(this, pulses[(size_t)ni]);
                            if ((g_overlay & 4) && states[(size_t)ni])
                                psnap::restoreState(this, *states[(size_t)ni]);
                            if ((g_overlay & 8) && touches[(size_t)ni])
                                psnap::restoreTouch(m_player1, *touches[(size_t)ni], this);
                        }
                    }
                    // Whether m_collisionLog* actually holds content (cfg `seccollog=1`).
                    // "Hits the same object 2 ticks late" has the shape of the collision
                    // continuation bookkeeping, so it is the main suspect, but if empty the
                    // suspect vanishes entirely. Measure whether it is used before writing a
                    // restore for it.
                    if (secsolve::g_colLog && m_player1 && restores % 2000 == 1) {
                        auto cnt = [](cocos2d::CCDictionary* d) {
                            return d ? (int)d->count() : -1;
                        };
                        char cb[200];
                        snprintf(cb, sizeof(cb),
                                 "seccollog: d=%d top=%d bottom=%d left=%d right=%d "
                                 "snapped=%s collided=%s",
                                 depth, cnt(m_player1->m_collisionLogTop),
                                 cnt(m_player1->m_collisionLogBottom),
                                 cnt(m_player1->m_collisionLogLeft),
                                 cnt(m_player1->m_collisionLogRight),
                                 m_player1->m_objectSnappedTo ? "yes" : "no",
                                 m_player1->m_collidedObject ? "yes" : "no");
                        writeResult(cb);
                    }
                    if (g_winShift) {
                        this->m_leftSectionIndex += g_winShift;
                        this->m_rightSectionIndex += g_winShift;
                    }
                    // Label the spine's own substep so `robodbg` prints one line
                    // per layer instead of one per node (a layer is ~100 wide).
                    g_stepSpine = g_spineOn && ni == g_spine
                                  && branch == spinePlanHeld(depth);
                    g_stepDepth = depth;
                    {
                        const auto stepT0 = std::chrono::steady_clock::now();   // print only
                        postmortem::g_secPhase = 20;
                        secStepBegin(pl);
                        postmortem::g_secPhase = 21;
                        secStep(branch, dt); ++steps;
                        postmortem::g_secPhase = 22;
                        // An object the step put at a position that is not a number: the game's
                        // world in this search is no longer one the game can be in (see
                        // secsolve::g_nonFinite), and every branch after this one steps in it.
                        if (secsolve::g_nonFinite > 0) {
                            char nb[240];
                            snprintf(nb, sizeof(nb), "object uid %d (id %d) went to position (%g, %g) on "
                                     "the step from node %d, branch %d", secsolve::g_nonFiniteUid,
                                     secsolve::g_nonFiniteId, secsolve::g_nonFiniteX, secsolve::g_nonFiniteY,
                                     ni, branch);
                            throw std::runtime_error(nb);
                        }
                        // cfg secthrow: the containment's own test (Config::secThrowKind).
                        if (g_cfg.secThrowKind != 0 && depth == g_cfg.secThrowDepth && !nxt.empty()
                            && secsolve::g_secThrowsDone < (g_cfg.secThrowKind == 3 ? 2 : 1)) {
                            const bool alloc = g_cfg.secThrowKind == 2
                                               || (g_cfg.secThrowKind == 3 && secsolve::g_secThrowsDone == 1);
                            ++secsolve::g_secThrowsDone;
                            if (alloc) throw std::bad_alloc();
                            throw std::length_error("cfg secthrow: a length_error on purpose");
                        }
                        g_secStepMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - stepT0).count();
                    }
                    g_stepSpine = false;
                    auto* p = m_player1;
                    // Death is seen via the flag set by destroyPlayer. Nothing is actually
                    // killed, so m_isDead is not set (on the checkpoint path it is set as
                    // before). Branches that escaped upward out of bounds are discarded
                    // here (see the note on g_maxY). GD carries them a bit higher before
                    // killing, but they eat cap the whole time, so treat them as dead as
                    // soon as it is known.
                    if (p && killAboveY > 0.0
                        && (double)p->getPositionY() > killAboveY) {
                        ++layDead;
                        deadMaxX = std::max(deadMaxX, (double)p->getPositionX());
                        needWake = true;
                        continue;
                    }
                    if (!p || g_died || p->m_isDead) {
                        ++layDead;
                        if (p) deadMaxX = std::max(deadMaxX, (double)p->getPositionX());
                        needWake = true;   // only meaningful on the checkpoint path
                        continue;
                    }
                    // Where the physics stands, not where the node was moved for drawing during a
                    // mirror transition (psnap::physPosition).
                    const cocos2d::CCPoint at = psnap::physPosition(p, this);
                    const double px = at.x;
                    const double py = at.y;
                    const double pv = (double)p->m_yVelocity;
                    // cfg seccoins: the coins this step touched, and a coin left behind.
                    uint32_t bitsHere = coinsOn ? coinBits[(size_t)ni] : 0u;
                    if (coinsOn) {
                        const cocos2d::CCRect pr = p->getObjectRect();
                        // Forward = x is the travel coordinate and only grows: the gameplay frame
                        // is 0 (the search's miss prune, dp cli.hpp, asks the same) and no rotation
                        // has run since the head. Not the camera angle -- a Camera Rotate turns
                        // the view and not the travel, and the angle is not wrapped: over lv22's
                        // coin 1 (x=10,513, frame 0) it stands at 360.0 (measured at a head at
                        // t=8,175), so no branch was ever cut for passing that coin.
                        const bool forward = !p->m_isGoingLeft && !p->m_isSideways
                                             && coinFrame == 0
                                             && g_rotGameplayCalls == coinRotAt;
                        bool missed = false;
                        for (size_t c = 0; c < coinObj.size(); ++c) {
                            const uint32_t bit = 1u << c;
                            if (bitsHere & bit) continue;
                            const cocos2d::CCRect cr = coinObj[c]->getObjectRect();
                            // Past it for good: forward, beyond its far edge + 15 (coinmiss's
                            // bound), and only for a coin nothing brings the player or the coin
                            // back to (coinFinal, a Move still ahead).
                            const bool past = forward && px > cr.getMaxX() + 15.f && coinFinal[c]
                                              && !(c < solver::g_coinMoveX.size()
                                                   && solver::g_coinMoveX[c] > -1e8f
                                                   && px <= solver::g_coinMoveX[c]);
                            // not there now: passing it is no miss
                            if (coinObj[c]->m_isGroupDisabled) {
                                // ...nor a pickup of the coin this rung was fired for (cfg
                                // secrungcoinoff): the bit said "done", and the rung's leaf test
                                // read it as "taken" -- SubZero 4003's third coin, its group off
                                // for want of a fourth press, was "taken" by six spliced leaves
                                // and credited by GD on none.
                                if (past && !(secsolve::g_rungCoinOff
                                              && (int)c == secsolve::g_rungCoin))
                                    bitsHere |= bit;
                                continue;
                            }
                            if (pr.intersectsRect(cr)) { bitsHere |= bit; continue; }
                            if (past) missed = true;
                        }
                        if (missed) {
                            ++layDead;
                            ++coinMissBranches;
                            deadMaxX = std::max(deadMaxX, px);
                            needWake = true;
                            continue;
                        }
                    }
                    // deadband: a branch passing through the band in a forbidden mode is
                    // treated as dead (see the note on secsolve::g_secBands). Placed before
                    // the goal test -- so that an arc that "crosses and survives" inside the
                    // band is not returned as a solution.
                    if (!secsolve::g_secBands.empty()) {
                        bool banned = false;
                        for (const auto& db : secsolve::g_secBands)
                            if (px >= db.x0 && px <= db.x1
                                && py >= db.y0 && py <= db.y1
                                && (db.mode < 0 || modeIdx(p) != db.mode)) {
                                banned = true;
                                break;
                            }
                        if (banned) {
                            ++layDead;
                            deadMaxX = std::max(deadMaxX, px);
                            needWake = true;
                            continue;
                        }
                    }
                    layMaxX = std::max(layMaxX, px);
                    layMinX = std::min(layMinX, px);
                    layMinY = std::min(layMinY, py);
                    layMaxY = std::max(layMaxY, py);
                    if (reachedGoal(px, py, depth)) {
                        // After the cutoff, discard without evaluating (see the note on
                        // g_maxDoomed). The search continues but the cost does not grow.
                        if (g_maxDoomed > 0 && doomedLeaves >= g_maxDoomed)
                            continue;
                        // cfg dpseccoinrung: a rung fired for a coin is answered only by a leaf
                        // that has it.
                        if (coinsOn && secsolve::g_rungCoin >= 0
                            && !(bitsHere & (1u << secsolve::g_rungCoin))) {
                            ++coinLessLeaves;
                            continue;
                        }
                        g_nodes.push_back({ni, (uint8_t)branch,
                                           (uint8_t)(p->m_isDashing ? 1 : 0),
                                           (float)py, (float)pv, (float)px,
                                           (uint16_t)secsolve::cntNow(this),
                                           (uint8_t)(p->m_vehicleSize < 0.9f ? 1 : 0),
                                           (uint8_t)(secDualP2() ? 1 : 0), secDualY2()});
                        g_dash.emplace_back();
                        secCaptureDash(g_dash.back());
                        g_vy.push_back(p ? p->m_yVelocity : 0.0);
                        g_accel.push_back(p ? p->m_accelerationOrSpeed : 0.0);
                        g_pad.push_back(p && p->m_touchedPad ? 1 : 0);
                        secPushP2();
                        coinBits.push_back(bitsHere);
                        g_cps.push_back(nullptr);
                        if (keepSnaps) {
                            snaps.emplace_back(); pulses.emplace_back();
                            states.emplace_back(); touches.emplace_back();
                            acts.emplace_back();
                        }
                        actSigs.push_back(0);
                        if (g_worldOn) worlds.emplace_back();
                        const int cand = (int)g_nodes.size() - 1;
                        const int st = evalLeaf(cand);
                        // A RUNG splices what it finds into the plan the loop flies, so only a
                        // leaf the plain replay reproduces is an answer; an unverified one is
                        // passed over like a dead-end exit and the search goes on. (The one-way
                        // handoff stops on it as before -- there it is a report, not a splice.)
                        // It matters under psnap branching, whose one error is a missed death.
                        if (st == 1 && g_secRung) {
                            ++unverLeaves;
                            needWake = true;
                            continue;
                        }
                        if (st != 2) {           // SOLVED or UNVERIFIED stops the search
                            foundLeaf = cand;
                            foundX = st == 0 ? verifyX : px;
                            foundDepth = depth;
                            break;
                        }
                        // Do not stop at a dead-end exit. The search continues. The
                        // cross-check returned the world to the section head, so the next
                        // expansion starts from a restore on either path (checkpoint and
                        // psnap alike).
                        ++doomedLeaves;
                        needWake = true;
                        // Keep the last dead-end exit for the report (foundLeaf stays unset)
                        foundX = verifyX;
                        continue;
                    }
                    const int cntHere = secsolve::cntNow(this);
                    // The objects this child has used (psnap only: on the checkpoint path the
                    // restore brings them back itself). Taken here, before the dedupe, because
                    // the key needs the signature; a child that is kept moves the bytes into
                    // `acts` instead of scanning them again.
                    std::vector<uint8_t> actHere;
                    uint64_t actSigHere = 0;
                    postmortem::g_secPhase = 30;
                    if (g_snapOn && psnap::g_snapAct) actSigHere = psnap::captureActSig(actHere);
                    postmortem::g_secPhase = 31;
                    // (cfg seccoins: a state that has the coin is not the same state as one
                    // that has not) -- and nor is one that has taken a ring, pad or portal
                    // the other has not (cfg secactkey) -- and, in a dual section, nor is one
                    // whose player 2 is elsewhere (cfg secp2key).
                    long long p2Here = 0;
                    if (secsolve::g_p2Key) {
                        if (auto* q = secDualP2())
                            p2Here = keyOf(secDualY2(), q->m_yVelocity, (int)modeIdx(q),
                                           q->m_vehicleSize < 0.9f ? 1 : 0,
                                           q->m_isUpsideDown ? 1 : 0, 0, 0.0,
                                           q->m_isDashing ? 1 : 0, 0);
                    }
                    const long long k = keyOf(py, pv, (int)modeIdx(p),
                                              p->m_vehicleSize < 0.9f ? 1 : 0,
                                              p->m_isUpsideDown ? 1 : 0, branch,
                                              px, p->m_isDashing ? 1 : 0,
                                              cntHere)
                                        ^ (long long)((unsigned long long)bitsHere
                                                      * 0x9E3779B97F4A7C15ULL)
                                        ^ (psnap::g_actKey
                                               ? (long long)(actSigHere * psnap::kActKeyMul)
                                               : 0LL)
                                        ^ (long long)((unsigned long long)p2Here
                                                      * 0xD6E8FEB86659FD93ULL);
                    // The spine is the verified solution's own continuation:
                    // this parent is the spine and this branch is what the plan
                    // does at this tick. It is exempt from the dedupe -- see
                    // the note at g_spineOn for why a window the solution
                    // crosses can otherwise come back EXHAUSTED.
                    // The spine is the verified solution's own continuation: this
                    // parent is the spine and this branch is what the plan does at
                    // this tick. Exempt from the dedupe -- see the note at
                    // g_spineOn for why a window the solution crosses can
                    // otherwise come back EXHAUSTED.
                    const bool isSpine =
                        g_spineOn && ni == g_spine && branch == spinePlanHeld(depth);
                    if (!seen.insert(k).second && !isSpine) { ++layDup; continue; }
                    // No cap discard here. Two children per parent, so the collected count
                    // is naturally bounded by 2*|cur|. Pruning happens once the layer is
                    // complete (below).
                    if (g_snapOn) {
                        g_nodes.push_back({ni, (uint8_t)branch,
                                           (uint8_t)(p->m_isDashing ? 1 : 0),
                                           (float)py, (float)pv, (float)px,
                                           (uint16_t)cntHere,
                                           (uint8_t)(p->m_vehicleSize < 0.9f ? 1 : 0),
                                           (uint8_t)(secDualP2() ? 1 : 0), secDualY2()});
                        g_dash.emplace_back();
                        secCaptureDash(g_dash.back());
                        g_vy.push_back(p ? p->m_yVelocity : 0.0);
                        g_accel.push_back(p ? p->m_accelerationOrSpeed : 0.0);
                        g_pad.push_back(p && p->m_touchedPad ? 1 : 0);
                        secPushP2();
                        coinBits.push_back(bitsHere);
                        g_cps.push_back(nullptr);
                        snaps.emplace_back();
                        pulses.emplace_back();
                        states.push_back(newState());
                        touches.push_back(std::make_unique<psnap::Touch>());
                        acts.emplace_back(std::move(actHere));
                        actSigs.push_back(actSigHere);
                        actSigSeen.insert(actSigHere);
                        postmortem::g_secPhase = 40;
                        psnap::capture(p, this, snaps.back());
                        postmortem::g_secPhase = 41;
                        psnap::captureEM(this, pulses.back());
                        postmortem::g_secPhase = 42;
                        psnap::captureState(this, *states.back());
                        postmortem::g_secPhase = 43;
                        psnap::captureTouch(p, *touches.back(), this);
                        postmortem::g_secPhase = 44;
                        if (g_worldOn) {
                            worlds.emplace_back();
                            psnap::captureWorld(g_movSet, worlds.back());
                        }
                        postmortem::g_secPhase = 45;
                        ++cpMade;
                    } else {
                        CheckpointObject* cp = markCheckpointAtPhys(pl);
                        if (!cp) continue;             // if it cannot be made, drop that branch
                        cp->retain(); ++cpMade;
                        g_nodes.push_back({ni, (uint8_t)branch,
                                           (uint8_t)(p->m_isDashing ? 1 : 0),
                                           (float)py, (float)pv, (float)px,
                                           (uint16_t)cntHere,
                                           (uint8_t)(p->m_vehicleSize < 0.9f ? 1 : 0),
                                           (uint8_t)(secDualP2() ? 1 : 0), secDualY2()});
                        g_dash.emplace_back();
                        secCaptureDash(g_dash.back());
                        g_vy.push_back(p ? p->m_yVelocity : 0.0);
                        g_accel.push_back(p ? p->m_accelerationOrSpeed : 0.0);
                        g_pad.push_back(p && p->m_touchedPad ? 1 : 0);
                        secPushP2();
                        coinBits.push_back(bitsHere);
                        actSigs.push_back(0);
                        g_cps.push_back(cp);
                        // For the stacking check. Taken at the same instant as the checkpoint
                        if (g_overlay) {
                            snaps.emplace_back(); pulses.emplace_back();
                            states.push_back(newState());
                            touches.push_back(std::make_unique<psnap::Touch>());
                            acts.emplace_back();
                            psnap::capture(p, this, snaps.back());
                            psnap::captureEM(this, pulses.back());
                            psnap::captureState(this, *states.back());
                            psnap::captureTouch(p, *touches.back(), this);
                            psnap::captureAct(acts.back());
                        }
                    }
                    nxt.push_back((int)g_nodes.size() - 1);
                    if (isSpine) g_spineNext = nxt.back();
                    cpPeak = std::max(cpPeak, (long long)(cur.size() + nxt.size()));
                }
            }
            postmortem::g_secPhase = 50;
            postmortem::g_secPhaseNode = -1;
            // Prune the layer to the cap. Never take the first cap in insertion order.
            // Children are ordered "parent order x branch order", which is roughly
            // ascending y (if parents are in y order, their children come low branch then
            // high branch). Cutting the tail drops only the top of the frontier every
            // layer. Measured at lv20 x=6342: with granularity 0.25 + cap 200 the band
            // collapsed to 6px (197.7..202.2), while coarse granularity with no cap hit
            // (cap 400) gave 54px (197.7..251.3) -- the upside-down result that finer is
            // worse. Sort by y and take at even spacing, always keeping both band edges.
            // Hold the spine out of the pruning entirely and put it back after.
            // Exempting it inside each branch of the cap would mean touching
            // both of them and their release paths; lifting it out cannot drop
            // it by accident, and the layer being cap+1 wide costs nothing.
            // cfg seckeepsize: a branch that changed player 1's size is not the route asked for,
            // and neither is the spine once it does.
            if (secsolve::g_keepP1Size >= 0) {
                size_t w = 0;
                for (int ni : nxt) {
                    if ((int)g_nodes[(size_t)ni].mini == secsolve::g_keepP1Size) nxt[w++] = ni;
                    else { releaseCp(ni); ++secsolve::g_keepDropped; }
                }
                nxt.resize(w);
            }
            int spineHeld = -1;
            if (g_spineOn && g_spineNext >= 0) {
                auto it = std::find(nxt.begin(), nxt.end(), g_spineNext);
                if (it != nxt.end()) { spineHeld = *it; nxt.erase(it); }
            }
            bool capByX = false;
            // The order the cap reads a family in: y, then vy.
            auto yThenVy = [&](int a, int b) {
                const auto& na = g_nodes[(size_t)a];
                const auto& nb = g_nodes[(size_t)b];
                if (na.y != nb.y) return na.y < nb.y;
                return na.vy < nb.vy;
            };
            // cfg seccover: q of the nodes in v (sorted by yThenVy) that cover the (y, vy)
            // plane -- one per cell of the finest square grid (over v's own ranges) that has at
            // most q - 2 occupied cells, the first in the sorted order of each, and both y edges.
            // Cells the grid leaves over are filled evenly in the sorted order. The button is a
            // further axis, as it is in the dedupe key: a cube resting on a block at (y, 0) jumps
            // next tick if it is held and stays if not, and sharing a cell the one kept would be
            // whichever sorted first. (Not measured to decide a search yet; the key's reason.)
            auto pickCover = [&](const std::vector<int>& v, size_t q) {
                std::vector<int> out;
                if (q == 0) return out;
                if (v.size() <= q) { out = v; return out; }
                if (q == 1) { out.push_back(v[v.size() / 2]); return out; }
                if (q == 2) { out.push_back(v.front()); out.push_back(v.back()); return out; }
                float ylo = 1e30f, yhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
                float y2lo = 1e30f, y2hi = -1e30f;
                bool withY2 = false;   // player 2's y as a further axis (cfg secp2key)
                for (int ni : v) {
                    const auto& n = g_nodes[(size_t)ni];
                    ylo = std::min(ylo, n.y); yhi = std::max(yhi, n.y);
                    vlo = std::min(vlo, n.vy); vhi = std::max(vhi, n.vy);
                    if (secsolve::g_p2Key && n.dual) {
                        withY2 = true;
                        y2lo = std::min(y2lo, n.y2); y2hi = std::max(y2hi, n.y2);
                    }
                }
                const double ys = std::max(1e-6, (double)(yhi - ylo));
                const double vs = std::max(1e-6, (double)(vhi - vlo));
                const double y2s = std::max(1e-6, (double)(y2hi - y2lo));
                auto cellOf = [&](int ni, long long k) {
                    const auto& n = g_nodes[(size_t)ni];
                    const long long cy =
                        std::min(k - 1, (long long)((n.y - ylo) / ys * (double)k));
                    const long long cv =
                        std::min(k - 1, (long long)((n.vy - vlo) / vs * (double)k));
                    long long c = cy * (k + 1) + cv;
                    if (withY2)
                        c = c * (k + 1)
                            + (n.dual ? std::min(k - 1, (long long)((n.y2 - y2lo) / y2s
                                                                     * (double)k))
                                      : k);
                    return c * 2 + (n.in ? 1 : 0);
                };
                auto occupied = [&](long long k) {
                    std::unordered_set<long long> c;
                    for (int ni : v) c.insert(cellOf(ni, k));
                    return c.size();
                };
                long long lo = 1, hi = 8192;
                while (lo < hi) {
                    const long long mid = (lo + hi + 1) / 2;
                    if (occupied(mid) + 2 <= q) lo = mid; else hi = mid - 1;
                }
                std::unordered_set<long long> cells;
                std::unordered_set<int> in;
                out.reserve(q);
                out.push_back(v.front()); in.insert(v.front());
                out.push_back(v.back()); in.insert(v.back());
                for (int ni : v) {
                    if (out.size() >= q) break;
                    if (!cells.insert(cellOf(ni, lo)).second) continue;
                    if (in.insert(ni).second) out.push_back(ni);
                }
                if (out.size() < q) {
                    std::vector<int> rest;
                    for (int ni : v) if (!in.count(ni)) rest.push_back(ni);
                    const size_t need = q - out.size();
                    for (size_t i = 0; i < need && i < rest.size(); ++i) {
                        const size_t idx = need > 1
                            ? (size_t)((double)i * (double)(rest.size() - 1)
                                       / (double)(need - 1) + 0.5)
                            : rest.size() / 2;
                        if (in.insert(rest[idx]).second) out.push_back(rest[idx]);
                    }
                }
                return out;
            };
            // The cap proper: `list` down to `cap` nodes, the others released.
            auto pruneTo = [&](std::vector<int>& list, size_t cap) {
                if (list.size() > cap) {
                    // Do not crush the x families. With two exits of a rotated gameplay
                    // section, families 40px apart in x line up at the same depth. Ordering by
                    // y alone and taking at even spacing keeps the y-band edges but can drop a
                    // whole family. In a section where x decides the phase (= passing the
                    // moving spikes), that is fatal.
                    // Cut x into buckets and take from the buckets round-robin.
                    // Buckets are cut by x and the "kind of state". While dashing, y is fixed,
                    // so with y-ordered even spacing those nodes get buried inside and dropped.
                    // The rarer the kind, the less it may be dropped (it is the path).
                    std::unordered_map<long long, std::vector<int>> buckets;
                    for (int ni : list) {
                        const auto& nd = g_nodes[(size_t)ni];
                        // cnt is a family too (same reason as dash): branches that hit are rare
                        // and get buried among pass-through branches under y-even spacing. They
                        // are the path.
                        // ...and so is a coin taken (cfg seccoins): the branches that took it are
                        // few, and they are the route.
                        const long long b = (long long)std::llround(nd.x / 16.0)
                                            + (nd.dash ? 1000003LL : 0LL)
                                            + (long long)nd.cnt * 2000003LL
                                            + (coinsOn ? (long long)coinBits[(size_t)ni] * 3000017LL
                                                       : 0LL)
                                            // ...and the set of objects used (cfg secactkey): the
                                            // branch that kept a ring for later is the rare one.
                                            + (psnap::g_actKey && (size_t)ni < actSigs.size()
                                                   ? (long long)(actSigs[(size_t)ni] % 1000003ull)
                                                         * 5000011LL
                                                   : 0LL);
                        buckets[b].push_back(ni);
                    }
                    if (buckets.size() > 1) {
                        for (auto& kv : buckets) {
                            std::sort(kv.second.begin(), kv.second.end(),
                                      [&](int a, int b2) {
                                          const auto& na = g_nodes[(size_t)a];
                                          const auto& nb = g_nodes[(size_t)b2];
                                          if (na.y != nb.y) return na.y < nb.y;
                                          return na.vy < nb.vy;
                                      });
                        }
                        std::vector<long long> keys;
                        keys.reserve(buckets.size());
                        for (auto& kv : buckets) keys.push_back(kv.first);
                        std::sort(keys.begin(), keys.end());
                        std::vector<int> keepv;
                        keepv.reserve(cap);
                        std::unordered_set<int> kept;
                        // The share per bucket. Even a small family always gets one.
                        const size_t per = std::max<size_t>(1, cap / keys.size());
                        for (long long k : keys) {
                            auto& v = buckets[k];
                            const size_t take = std::min(per, v.size());
                            if (secsolve::g_cover) {
                                for (int ni : pickCover(v, take))
                                    if (keepv.size() < cap && kept.insert(ni).second)
                                        keepv.push_back(ni);
                                continue;
                            }
                            for (size_t i = 0; i < take && keepv.size() < cap; ++i) {
                                const size_t idx = take > 1
                                    ? (size_t)((double)i * (double)(v.size() - 1)
                                               / (double)(take - 1) + 0.5)
                                    : v.size() / 2;
                                if (kept.insert(v[idx]).second) keepv.push_back(v[idx]);
                            }
                        }
                        // Fill the remainder in y order (raises the band's resolution)
                        if (keepv.size() < cap) {
                            std::vector<int> rest;
                            for (int ni : list) if (!kept.count(ni)) rest.push_back(ni);
                            std::sort(rest.begin(), rest.end(), [&](int a, int b2) {
                                return g_nodes[(size_t)a].y < g_nodes[(size_t)b2].y;
                            });
                            const size_t need = cap - keepv.size();
                            if (secsolve::g_cover) {
                                std::sort(rest.begin(), rest.end(), yThenVy);
                                for (int ni : pickCover(rest, need))
                                    if (kept.insert(ni).second) keepv.push_back(ni);
                            }
                            for (size_t i = 0; !secsolve::g_cover && i < need && i < rest.size();
                                 ++i) {
                                const size_t idx = need > 1
                                    ? (size_t)((double)i * (double)(rest.size() - 1)
                                               / (double)(need - 1) + 0.5)
                                    : rest.size() / 2;
                                if (kept.insert(rest[idx]).second)
                                    keepv.push_back(rest[idx]);
                            }
                        }
                        for (int ni : list)
                            if (!kept.count(ni)) { releaseCp(ni); ++layCap; ++capTotal; }
                        list.swap(keepv);
                        capByX = true;
                    }
                }
                if (list.size() > cap) {
                    // Which axis the "band" forms on depends on the section. A normal section
                    // spreads in y, but in a rotated gameplay section y is the clock and nearly
                    // equal across branches; the spread is in x. Ordering by y and taking at
                    // even spacing then means taking by "the ordering within nearly-equal y",
                    // defeating the purpose of keeping the band edges. Order along the axis
                    // that spreads.
                    double ylo = 1e18, yhi = -1e18, xlo = 1e18, xhi = -1e18;
                    for (int ni : list) {
                        const auto& n = g_nodes[(size_t)ni];
                        ylo = std::min(ylo, (double)n.y); yhi = std::max(yhi, (double)n.y);
                        xlo = std::min(xlo, (double)n.x); xhi = std::max(xhi, (double)n.x);
                    }
                    capByX = (xhi - xlo) > (yhi - ylo);
                    std::sort(list.begin(), list.end(), [&](int a, int b) {
                        const auto& na = g_nodes[(size_t)a];
                        const auto& nb = g_nodes[(size_t)b];
                        if (capByX) {
                            if (na.x != nb.x) return na.x < nb.x;
                            if (na.y != nb.y) return na.y < nb.y;
                            return na.vy < nb.vy;
                        }
                        if (na.y != nb.y) return na.y < nb.y;
                        return na.vy < nb.vy;
                    });
                    const size_t n = list.size();
                    std::vector<int> keepv;
                    keepv.reserve(cap);
                    std::unordered_set<int> kept;
                    const bool cover = secsolve::g_cover && !capByX;
                    if (cover)
                        for (int ni : pickCover(list, cap))
                            if (kept.insert(ni).second) keepv.push_back(ni);
                    for (size_t i = 0; !cover && i < cap; ++i) {
                        const size_t idx = cap > 1
                            ? (size_t)((double)i * (double)(n - 1) / (double)(cap - 1) + 0.5)
                            : n / 2;
                        if (kept.insert(list[idx]).second) keepv.push_back(list[idx]);
                    }
                    for (int ni : list)
                        if (!kept.count(ni)) { releaseCp(ni); ++layCap; ++capTotal; }
                    list.swap(keepv);
                }
            };
            // cfg secsizefam: the two sizes of player 1 are pruned apart, half the cap each (what
            // one cannot fill goes to the other).
            if (secsolve::g_sizeFam && nxt.size() > g_cap) {
                std::vector<int> fam[2];
                for (int ni : nxt) fam[g_nodes[(size_t)ni].mini ? 1 : 0].push_back(ni);
                if (!fam[0].empty() && !fam[1].empty()) {
                    const size_t q0 = std::min(fam[0].size(),
                                               std::max(g_cap / 2,
                                                        g_cap - std::min(g_cap, fam[1].size())));
                    const size_t q1 = std::min(fam[1].size(), g_cap - q0);
                    pruneTo(fam[0], q0);
                    pruneTo(fam[1], q1);
                    nxt.swap(fam[0]);
                    nxt.insert(nxt.end(), fam[1].begin(), fam[1].end());
                }
            }
            pruneTo(nxt, g_cap);
            // ...and the spine goes back in, whatever the pruning decided.
            if (spineHeld >= 0) nxt.push_back(spineHeld);
            // ---- Detect and repair (cfg `secverifyevery`) --------------------
            // Match the frontier nodes by replaying their input sequence from the section
            // checkpoint. psnap's errors only go in the direction of missing deaths, so
            // dropping them here leaves a frontier that is reachable on the real game only.
            // Surviving nodes have their snapshots retaken from the real state after the
            // replay (if they had drifted, that fixes them there).
            postmortem::g_secPhase = 55;
            if (g_snapOn && g_verifyEvery > 0 && !nxt.empty()
                && depth % g_verifyEvery == 0) {
                postmortem::g_secPhase = 60;
                std::vector<int> ok;
                ok.reserve(nxt.size());
                int vDead = 0, vFixed = 0, vAnchored = 0;
                double vMaxDy = 0.0;
                std::vector<uint8_t> seq;
                // Replay from the checkpoint of the previous cross-check point. This only
                // moves the replay origin closer; the input sequence replayed is unchanged
                // (equivalence is decided by the input sequence). Only branches without an
                // ancestor checkpoint replay from the section head.
                std::vector<int> anc((size_t)nxt.size(), -1);
                std::unordered_map<int, CheckpointObject*> newAnchors;
                std::unordered_map<int, std::vector<uint8_t>> newAnchorSnaps;
                if (g_anchor) {
                    for (size_t k = 0; k < nxt.size(); ++k) {
                        int a = nxt[k];
                        for (int s = 0; s < g_verifyEvery && a > 0; ++s)
                            a = g_nodes[(size_t)a].parent;
                        if (a > 0 && g_anchors.count(a)) anc[k] = a;
                    }
                }
                // cfg secdriftwhere: for the node that drifted most in this pass, the first
                // step at which the replay left the node's own ancestors, and the state on
                // both sides there (psnapDrift says how far a branch ended up, not where).
                struct DriftAt { int node = -1, depth = -1; double dy = 0.0, yR = 0.0,
                                 yP = 0.0, vyR = 0.0, vyP = 0.0, xR = 0.0; int mode = -1,
                                 up = -1; float size = 0.f; };
                DriftAt worstDrift;
                std::vector<int> path;
                for (size_t k = 0; k < nxt.size(); ++k) {
                    const int ni = nxt[k];
                    const int a = g_anchor ? anc[k] : -1;
                    seq.clear();
                    path.clear();
                    for (int i = ni; i > 0 && i != a; i = g_nodes[(size_t)i].parent) {
                        seq.push_back(g_nodes[(size_t)i].in);
                        path.push_back(i);
                    }
                    std::reverse(seq.begin(), seq.end());
                    std::reverse(path.begin(), path.end());
                    DriftAt here;
                    auto originCheck = [&](int origin) {
                        if (!psnap::g_snapAct || origin < 0 || (size_t)origin >= acts.size())
                            return;
                        const auto ul = psnap::originUsedLost(acts[(size_t)origin]);
                        ++originChecks;
                        originUsed += ul.first;
                        originLost += ul.second;
                    };
                    // GD's checkpoint does not keep which rings, pads and portals were used
                    // (saveActiveSaveObjects covers only the objects at +0x3808), so the replay
                    // would start in a world where everything used before its origin is fresh
                    // again -- and since a9adab3 the node takes its flags from this replay. Put
                    // back the origin's own flags first, as the expansion path does.
                    if (a > 0) {
                        secRestoreFrom(pl, g_anchors[a], g_nodes[(size_t)a].in);
                        if (psnap::g_snapAct && (size_t)a < acts.size())
                            psnap::restoreAct(acts[(size_t)a]);
                        originCheck(a);
                        ++restores; ++vAnchored;
                        // cfg secjbkeep: the origin's own m_jumpBuffered, both bodies, in the
                        // place the expansion path writes it (before the frozen ticks)
                        if ((size_t)a < g_dash.size()) secRestoreJb(g_dash[(size_t)a]);
                        if (auto* q = secP2(); q && (size_t)a < secsolve::g_dash2.size())
                            secRestoreJb(secsolve::g_dash2[(size_t)a], q);
                        for (int f = 0; f < freeze; ++f) {
                            secStep(g_nodes[(size_t)a].in, dt); ++steps;
                        }
                        cpTakeLoad();
                        // ...and the origin's own player (g_cpPlayer).
                        const auto it = anchorSnaps.find(a);
                        if (secsolve::g_cpPlayer && it != anchorSnaps.end() && !it->second.empty())
                            psnap::restore(m_player1, this, it->second.data());
                        if (it != anchorSnaps.end())
                            cpReport(4, "checkanchor", (long long)depth, it->second);
                    } else {
                        secRestoreFrom(pl, g_ckpt, g_headHeld); ++restores;
                        if (psnap::g_snapAct && !acts.empty()) psnap::restoreAct(acts[0]);
                        originCheck(0);
                        for (int f = 0; f < freeze; ++f) {
                            secStep(g_headHeld, dt); ++steps;
                        }
                        cpTakeLoad();
                        if (secsolve::g_cpPlayer && !snaps.empty() && !snaps[0].empty())
                            psnap::restore(m_player1, this, snaps[0].data());
                        cpReport(3, "checkhead", (long long)depth, secsolve::g_headPlain);
                    }
                    bool dead = false;
                    for (size_t s = 0; s < seq.size(); ++s) {
                        g_died = false;
                        secStep((int)seq[s], dt); ++steps;
                        if (!m_player1 || m_player1->m_isDead || g_died) {
                            dead = true; break;
                        }
                        if (g_cfg.secDriftWhere && here.node < 0) {
                            const auto& an = g_nodes[(size_t)path[s]];
                            const double yR = m_player1->getPositionY();
                            if (std::fabs(yR - (double)an.y) > 0.5) {
                                here.node = path[s];
                                here.depth = depth - (int)(seq.size() - 1 - s);
                                here.yR = yR; here.yP = an.y;
                                here.vyR = m_player1->m_yVelocity; here.vyP = an.vy;
                                here.xR = m_player1->getPositionX();
                                here.mode = (int)modeIdx(m_player1);
                                here.up = m_player1->m_isUpsideDown ? 1 : 0;
                                here.size = m_player1->m_vehicleSize;
                            }
                        }
                    }
                    if (dead) { ++vDead; releaseCp(ni); continue; }
                    auto* vp = m_player1;
                    // x as well: a drift in x alone (on a custom level, the mirror transition) passed a
                    // y/vy-only check untouched and the node kept its wrong x.
                    const cocos2d::CCPoint rat = psnap::physPosition(vp, this);
                    const double rx = rat.x;
                    const double ry = rat.y;
                    const double rv = (double)vp->m_yVelocity;
                    const double dx = std::fabs(rx - (double)g_nodes[(size_t)ni].x);
                    const double dy = std::fabs(ry - (double)g_nodes[(size_t)ni].y);
                    const double dv = std::fabs(rv - (double)g_nodes[(size_t)ni].vy);
                    const double dmax = std::max(dx, std::max(dy, dv));
                    vMaxDy = std::max(vMaxDy, dmax);
                    if (here.node >= 0 && dmax > worstDrift.dy) {
                        worstDrift = here;
                        worstDrift.dy = dmax;
                    }
                    if (dx > g_verifyTol || dy > g_verifyTol || dv > g_verifyTol) ++vFixed;
                    g_nodes[(size_t)ni].x = (float)rx;
                    g_nodes[(size_t)ni].y = (float)ry;
                    g_nodes[(size_t)ni].vy = (float)rv;
                    // Retake from the real state. Skipping this means the match passed but
                    // the continuation grows from the old snapshot.
                    psnap::capture(vp, this, snaps[(size_t)ni]);
                    psnap::captureEM(this, pulses[(size_t)ni]);
                    if (!states[(size_t)ni]) states[(size_t)ni] = newState();
                    if (!touches[(size_t)ni]) touches[(size_t)ni] = std::make_unique<psnap::Touch>();
                    psnap::captureState(this, *states[(size_t)ni]);
                    psnap::captureTouch(vp, *touches[(size_t)ni], this);
                    // ...and which objects the real replay has used, which is what the node
                    // carries from here on (36ef322 left the psnap-side flags in place).
                    if ((size_t)ni < acts.size() && psnap::g_snapAct) {
                        const uint64_t sig = psnap::captureActSig(acts[(size_t)ni]);
                        if ((size_t)ni < actSigs.size()) actSigs[(size_t)ni] = sig;
                        actSigSeen.insert(sig);
                    }
                    if (g_worldOn)
                        psnap::captureWorld(g_movSet, worlds[(size_t)ni]);
                    // Origin of the next cross-check. The real state is here right now, so
                    // make it here.
                    if (g_anchor) {
                        if (CheckpointObject* ac = markCheckpointAtPhys(pl)) {
                            ac->retain();
                            newAnchors[ni] = ac;
                            if (secsolve::g_cpPlayer || psnap::g_cpCheck)
                                newAnchorSnaps[ni] = snaps[(size_t)ni];
                        }
                    }
                    ok.push_back(ni);
                }
                // Discard and replace the old generation of anchors. Letting them pile up
                // stacks 0.95MB x cap per generation. Release them reliably here.
                for (auto& kv : g_anchors) {
                    if (kv.second) kv.second->release();
                    // ...and the used-object bytes releaseCp kept for this generation's replay
                    // origins, once the node itself is gone (its snapshot freed).
                    const int an = kv.first;
                    if (!newAnchors.count(an) && (size_t)an < acts.size()
                        && (size_t)an < snaps.size() && snaps[(size_t)an].empty())
                        std::vector<uint8_t>().swap(acts[(size_t)an]);
                }
                g_anchors.swap(newAnchors);
                anchorSnaps.swap(newAnchorSnaps);
                // A measurement of how much psnap lied. The prior gates (sweep, moving
                // objects, moving portals) are all just guesses at "may it be used", and
                // lv22's ball section passed through every one of them and produced
                // maxDiff 14.4px. The caller can look at this number and re-solve with the
                // checkpoint.
                maxVerifyDrift = std::max(maxVerifyDrift, vMaxDy);
                char vb[240];
                snprintf(vb, sizeof(vb),
                         "secfix: d=%d checked=%zu dead=%d drift=%d maxDiff=%.4f "
                         "kept=%zu anchored=%d",
                         depth, nxt.size(), vDead, vFixed, vMaxDy, ok.size(),
                         vAnchored);
                writeResult(vb);
                if (g_cfg.secDriftWhere && worstDrift.node >= 0) {
                    char db[320];
                    snprintf(db, sizeof(db),
                             "secdrift: d=%d worst=%.3f firstAt d=%d t=%lld x=%.2f "
                             "yReal=%.3f yPsnap=%.3f vyReal=%.3f vyPsnap=%.3f mode=%d up=%d "
                             "size=%.2f",
                             depth, worstDrift.dy, worstDrift.depth,
                             (long long)(g_ckptTick + 1 + worstDrift.depth), worstDrift.xR,
                             worstDrift.yR, worstDrift.yP, worstDrift.vyR, worstDrift.vyP,
                             worstDrift.mode, worstDrift.up, worstDrift.size);
                    writeResult(db);
                }
                nxt.swap(ok);
                if (nxt.empty()) { deepest = depth; spineLied(depth); break; }
            }
            deepest = depth;
            spineLied(depth);
            if (g_log) {
                // Layer fingerprint. For directly reading, from one run of each, at which
                // layer the checkpoint path and the psnap path first split. Comparing
                // solutions cannot locate it -- it depends on which leaf crossed the goal
                // first.
                uint64_t fp = 1469598103934665603ull;
                for (int ni : nxt) {
                    const auto& nd = g_nodes[(size_t)ni];
                    const uint8_t* b = (const uint8_t*)&nd.y;
                    for (size_t j = 0; j < sizeof(float) * 2; ++j) {
                        fp ^= b[j]; fp *= 1099511628211ull;
                    }
                    fp ^= nd.in; fp *= 1099511628211ull;
                }
                int cntLo = 65535, cntHi = 0;
                for (int ni : nxt) {
                    const int c = (int)g_nodes[(size_t)ni].cnt;
                    cntLo = std::min(cntLo, c);
                    cntHi = std::max(cntHi, c);
                }
                if (cntLo > cntHi) { cntLo = 0; cntHi = 0; }
                // lms / lrest: what this layer COST, not just what it produced.
                // Without them "the depth rate varies 17-fold between windows"
                // cannot be split into "restores got slower" and "there are more
                // of them per layer", and those two have different fixes.
                const double layerMs = (std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count()
                    - layerT0 * 1000.0);
                size_t secLive = 0, secPhys = 0;
                postmortem::g_secPhase = 70;
                sectionCensus(pl, secLive, secPhys);
                char lb[768];
                snprintf(lb, sizeof(lb),
                         "seclayer: d=%d parents=%zu keep=%zu dead=%d dup=%d "
                         "capped=%d x=%.1f y=%.1f..%.1f deadX=%.1f fp=%016llx "
                         "xr=%.1f..%.1f axis=%c cnt=%d..%d spine=%d spineY=%.1f "
                         "lms=%.0f lrest=%lld lrestms=%.0f nodes=%zu dedup=%zu "
                         "wsMB=%zu actN=%d secLive=%zu secPhys=%zu "
                         "lstorems=%.0f lresetms=%.0f",
                         depth, cur.size(), nxt.size(), layDead, layDup, layCap,
                         layMaxX, layMinY > 1e8 ? 0.0 : layMinY,
                         layMaxY < -1e8 ? 0.0 : layMaxY, deadMaxX,
                         (unsigned long long)fp,
                         layMinX > 1e8 ? 0.0 : layMinX, layMaxX,
                         capByX ? 'x' : 'y', cntLo, cntHi, g_spineNext,
                           g_spineNext >= 0 ? (double)g_nodes[(size_t)g_spineNext].y : -1.0,
                         layerMs, restores - layerR0,
                         g_secRestMs - layerRestMs0, g_nodes.size(), seen.size(),
                         layerWsMB(), runningActionsOn(m_player1),
                         secLive, secPhys,
                         g_secStoreMs - layerStoreMs0,
                         g_secResetMs - layerResetMs0);
                // The player-snapshot path's own split (see g_secSnapMs), appended after the
                // line's last field so readers of the fields before it are untouched, and the
                // wave trail's length at the layer's end (see g_trailClear).
                {
                    char sx[112];
                    const auto* wt = m_player1 ? m_player1->m_waveTrail : nullptr;
                    snprintf(sx, sizeof(sx), " lsnapms=%.0f lstepms=%.0f trail=%d",
                             g_secSnapMs - layerSnapMs0, g_secStepMs - layerStepMs0,
                             wt && wt->m_pointArray ? (int)wt->m_pointArray->count() : -1);
                    strncat(lb, sx, sizeof(lb) - strlen(lb) - 1);
                }
                // ...and, under cfg secsizefam, how many of the kept nodes are normal size.
                if (secsolve::g_sizeFam) {
                    int normal = 0;
                    for (int ni : nxt) normal += g_nodes[(size_t)ni].mini ? 0 : 1;
                    char sf[40];
                    snprintf(sf, sizeof(sf), " normal=%d", normal);
                    strncat(lb, sf, sizeof(lb) - strlen(lb) - 1);
                }
                // ...and what the restores leave behind in the scene (print only): the actions
                // running on the play layer (resetLevel queues a 0.2 s startGameDelayed on every
                // call), and the nodes under it two and three levels down. The restore's cost
                // grows with the number of restores (measured 2026-09-02/03) and something has to
                // be growing with it.
                {
                    long long kids2 = 0, kids3 = 0;
                    if (auto* ch = this->getChildren()) {
                        for (unsigned i = 0; i < ch->count(); ++i) {
                            auto* c = static_cast<cocos2d::CCNode*>(ch->objectAtIndex(i));
                            if (!c) continue;
                            kids2 += c->getChildrenCount();
                            if (auto* gc = c->getChildren())
                                for (unsigned j = 0; j < gc->count(); ++j)
                                    if (auto* g = static_cast<cocos2d::CCNode*>(gc->objectAtIndex(j)))
                                        kids3 += g->getChildrenCount();
                        }
                    }
                    char sn[128];
                    snprintf(sn, sizeof(sn), " plActs=%d plKids=%u kids2=%lld kids3=%lld",
                             runningActionsOn(this), this->getChildrenCount(), kids2, kids3);
                    strncat(lb, sn, sizeof(lb) - strlen(lb) - 1);
                    // The nodes updateShaderLayer (0x2222b0) moves children between, by raw
                    // offset (called twice per restore; 87% of a late restore in a profile).
                    auto kidsAt = [&](size_t off) -> long long {
                        auto* n = *reinterpret_cast<cocos2d::CCNode* const*>(
                            reinterpret_cast<const char*>(this) + off);
                        return n ? (long long)n->getChildrenCount() : -1;
                    };
                    snprintf(sn, sizeof(sn), " shd=%lld,%lld,%lld,%lld,%lld,%lld",
                             kidsAt(gdoff::kLayerObjectParent),
                             kidsAt(gdoff::kLayerAboveShaderParent),
                             kidsAt(gdoff::kLayerObjectLayer),
                             kidsAt(gdoff::kLayerInShaderObjectLayer),
                             kidsAt(gdoff::kLayerAboveShaderObjectLayer),
                             kidsAt(gdoff::kLayerShaderLayer));
                    strncat(lb, sn, sizeof(lb) - strlen(lb) - 1);
                    // resetLevel sorts the children of every child of the node at +0x1028
                    // (qsortAllChildrenWithIndex, 3b8eb0): how many there are, in total, and the
                    // largest one, plus how many of those children are flagged for a reorder.
                    {
                        long long sortKids = -1, sortGrand = 0, sortMax = 0, dirty = 0;
                        // A CCArray, not a node: resetLevel reads its ccArray at +0x38.
                        auto* ch = *reinterpret_cast<cocos2d::CCArray* const*>(
                            reinterpret_cast<const char*>(this) + gdoff::kLayerBatchNodes);
                        if (ch) {
                            sortKids = ch->count();
                            for (unsigned i = 0; i < ch->count(); ++i) {
                                auto* c = static_cast<cocos2d::CCNode*>(ch->objectAtIndex(i));
                                if (!c) continue;
                                const long long k = c->getChildrenCount();
                                sortGrand += k;
                                sortMax = std::max(sortMax, k);
                                if (c->m_bReorderChildDirty) ++dirty;
                            }
                        }
                        snprintf(sn, sizeof(sn), " sort=%lld,%lld,%lld,%lld", sortKids, sortGrand,
                                 sortMax, dirty);
                        strncat(lb, sn, sizeof(lb) - strlen(lb) - 1);
                    }
                }
                writeResult(lb);
                // cfg secdrift=1 (print only): the object furthest from where the level put it,
                // and how many are over 1,000 px away -- which part of the world a player
                // snapshot leaves running (the section vectors' high-water climbs with it).
                if (g_driftLog && this->m_objects) {
                    double farD = 0.0, fdx = 0.0, fdy = 0.0;   // (`far` is a Windows macro)
                    int fuid = -1, fid = -1, n1000 = 0;
                    for (unsigned i = 0; i < this->m_objects->count(); ++i) {
                        auto* o = static_cast<GameObject*>(this->m_objects->objectAtIndex(i));
                        if (!o) continue;
                        const double dx = o->m_positionX - (double)o->m_startPosition.x;
                        const double dy = o->m_positionY - (double)o->m_startPosition.y;
                        const double dd = std::fabs(dx) + std::fabs(dy);
                        if (dd > 1000.0) ++n1000;
                        if (dd > farD) {
                            farD = dd; fdx = dx; fdy = dy;
                            fuid = o->m_uniqueID; fid = o->m_objectID;
                        }
                    }
                    char db[160];
                    snprintf(db, sizeof(db), "secdrift: d=%d far=%.0f uid=%d id=%d dx=%.0f dy=%.0f "
                             "over1000=%d", depth, farD, fuid, fid, fdx, fdy, n1000);
                    writeResult(db);
                }
            }
            for (int ni : cur) releaseCp(ni);          // the previous layer is no longer needed
            if (nxt.empty()) break;
            // The spine advances with the layer. If this layer produced no
            // continuation for it -- the plan's own branch died, which on a
            // verified solution means the section head is not where the plan
            // was -- it simply stops being pinned and the search carries on
            // without it, rather than pinning a stale node forever.
            g_spine = g_spineNext;
            g_spineNext = -1;
            cur.swap(nxt);
            // ---- hand the frame back (see secsolve::SecTask) ----
            // At the layer boundary and nowhere else: `cur` is the layer just finished, every
            // checkpoint of the previous one has been released, and the world is whatever the
            // last expansion's restore left -- the same state the next expansion would restore
            // over anyway. The HUD is written first so the frame that follows draws the
            // progress rather than the phase this started in.
            g_frontierNow = cur.size();
            if (g_sliceMs > 0) {
                char hb[96];
                snprintf(hb, sizeof(hb), "secsolve: layer %d/%d, frontier %zu",
                         depth, g_horizon, cur.size());
                g_hudPhase = hb;
                if (sliceExpired()) co_await std::suspend_always{};
            }
        }
        } catch (const std::length_error& e) {
            giveUp("length_error", e.what());
        } catch (const std::bad_alloc& e) {
            giveUp("bad_alloc", e.what());
        } catch (const std::exception& e) {
            giveUp("other", e.what());
        }
        postmortem::g_secPhase = 0;
        for (int ni : cur) releaseCp(ni);
        releaseAnchors();

        // The leaf cross-check (plain replay + exit survivability) is already done inside
        // the search (`evalLeaf`). Back when it ran once here, a dead-end first leaf to
        // reach the goal ended the search on the spot.

        secsolve::setSearching(false);
        g_noKill = false;
        g_tick = savedTick;
        g_nextInput = savedInput;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        // 512. 256 is not enough. When movSet= was added the buffer stayed 256 and the
        // line was cut just before foundVy. The caller's regex requires everything up to
        // foundVy, so the match failed, and a solution that passed the cross-check was
        // discarded as "no verdict" and re-solved with the checkpoint (2026-08-08; burned
        // an hour of lv20). When adding long lines, check the buffer too.
        char b[704];
        snprintf(b, sizeof(b),
                 "secsolve: %s prim=%s movSet=%zu depth=%d/%d frontier=%zu restores=%lld "
                 "steps=%lld cps=%lld cpPeak=%lld freeze=%d ms=%.0f "
                 "rephases=%lld maskShrinks=%d ckptTick=%lld inputBase=%lld targetX=%.1f "
                 "foundX=%.1f foundTick=%lld foundY=%.3f foundVy=%.3f "
                 "targetY=%.1f targetYDir=%d targetDepth=%d grace=%d/%d "
                 "doomedLeaves=%lld killAboveY=%.0f psnapDrift=%.2f stop=%s",
                 // A solution that failed the cross-check is never called SOLVED. The
                 // caller looks only at the verdict, so rejecting it here makes it
                 // structurally impossible for a false solution to be grafted.
                 // A solution whose exit is a dead end is never called SOLVED (DOOMED).
                 // Even if it reaches the goal, it is no solution if every press from
                 // there dies. Dead-end exits are discarded and the search continues, so
                 // DOOMED appears when "paths to the goal were found, but every one was a
                 // dead end".
                 foundLeaf < 0 ? (doomedLeaves > 0 ? "DOOMED" : "EXHAUSTED")
                               : (g_leafVerified ? "SOLVED" : "UNVERIFIED"),
                 !g_snapOn ? "cp" : (g_worldOn ? "psnap+wsnap" : "psnap"),
                 g_worldOn ? g_movSet.size() : (size_t)0, deepest, g_horizon,
                 cur.size(), restores, steps, cpMade, cpPeak, freeze, ms,
                 rephases, psnap::g_shrinks, (long long)g_ckptTick,
                 // The absolute-tick origin when grafting onto the plain replay =
                 // ckptTick itself. State and input each count differently by one, so it
                 // was decided by measurement, not derivation:
                 //   state: the restore lands on tick ckptTick+1; depth d = tick
                 //          ckptTick+1+d
                 //   input: the branch input at depth d is the plain run's at tick
                 //          ckptTick+d
                 // (the shape of a button taking 2 substeps to reach the physics. Theory
                 //  can tip either way, so it was measured twice independently and agreed:
                 //  the secoff sweep of secverify gave off=-2, the graft-position sweep of
                 //  a SOLVED solution gave base=ckptTick, and in both y/vy matched the
                 //  plain run exactly.)
                 // Never confirm by survival: where the band is wide, several shifts pass
                 // (in lv20 both -2 and -3 survived). Match on foundY/foundVy.
                 (long long)g_ckptTick, g_targetX,
                 g_leafVerified ? verifyX : foundX,
                 // Which tick of the plain run the exit state corresponds to. The state at
                 // depth d is tick ckptTick+1+d. Do not make the caller re-count (today's
                 // lesson).
                 foundLeaf >= 0 ? (long long)(g_ckptTick + 1 + foundDepth) : -1LL,
                 // The seam expectation must be the replay's measured value. The value the
                 // search holds can be off under psnap (measured dy=8.2px). The caller
                 // uses this as check_seam's expectation, so passing a lie breaks the seam
                 // and becomes indistinguishable from a fidelity problem.
                 g_leafVerified ? verifyY
                                : (foundLeaf >= 0
                                   ? (double)g_nodes[(size_t)foundLeaf].y : 0.0),
                 g_leafVerified ? verifyVy
                                : (foundLeaf >= 0
                                   ? (double)g_nodes[(size_t)foundLeaf].vy : 0.0),
                 g_targetY, g_targetYDir, g_targetDepth, graceOk, graceDeadAt,
                 doomedLeaves, killAboveY, maxVerifyDrift, stopWhy);
        writeResult(b);
        // What the object-activation carry cost (psnap::captureAct): objects in reach = bytes
        // per node, the nodes that hold a copy, and the time spent saving and restoring them.
        if (g_snapOn) {
            char ab[320];
            snprintf(ab, sizeof(ab),
                     "secsnapact: on=%d key=%d selfcheck=%d objs=%zu bytesPerNode=%zu nodes=%zu "
                     "cpPeak=%lld captures=%lld restores=%lld capUs=%.0f restUs=%.0f "
                     "p1Objs=%d p2Objs=%d sigs=%zu decos=%zu decoUsed=%d decoEver=%d",
                     psnap::g_snapAct ? 1 : 0, psnap::g_actKey ? 1 : 0, actSelfCheck,
                     psnap::g_actObjs.size(), psnap::g_actObjs.size(),
                     acts.size(), cpPeak, psnap::g_actCaptures, psnap::g_actRestores,
                     psnap::g_actCapUs, psnap::g_actRestUs, psnap::actSeenCount(1),
                     psnap::actSeenCount(2), actSigSeen.size(), psnap::g_actDecos.size(),
                     psnap::actDecosUsed(), psnap::actDecosEver());
            writeResult(ab);
            snprintf(ab, sizeof(ab), "secorigin: checks=%lld used=%lld lost=%lld", originChecks,
                     originUsed, originLost);
            writeResult(ab);
        }
        if (coinsOn) {
            const uint32_t lb = foundLeaf >= 0 ? coinBits[(size_t)foundLeaf] : 0u;
            int took = 0;
            for (size_t c = 0; c < coinObj.size(); ++c)
                if ((lb & (1u << c)) && !(coinBits[0] & (1u << c))) ++took;
            char kb[200];
            snprintf(kb, sizeof(kb), "secsolve: coins - %lld branches left a coin behind; the "
                     "leaf took %d (bits %u -> %u); owed coin %d, %lld leaves without it",
                     coinMissBranches, took, coinBits[0], lb, secsolve::g_rungCoin, coinLessLeaves);
            writeResult(kb);
        }
        if (secsolve::g_psnapLied) {
            char lb[256];
            snprintf(lb, sizeof(lb), "secsolve: the plan's own branch died at t=%lld on the player "
                     "snapshot, where the game flew it to t=%lld - the snapshot parts from the game in "
                     "this window (secsolve::g_spineUntil)%s",
                     secsolve::g_spineLostT, secsolve::g_spineUntil,
                     foundLeaf >= 0 ? "; what it found was replayed from the head" : "");
            writeResult(lb);
        }
        if (secsolve::g_keepP1Size >= 0)
            writeResult("secsolve: kept player 1 " + std::string(secsolve::g_keepP1Size ? "mini" : "normal")
                        + " - " + std::to_string(secsolve::g_keepDropped)
                        + " branches that changed its size dropped (cfg seckeepsize)");
        if (g_boundOn) {
            char bb[200];
            snprintf(bb, sizeof(bb), "secsolve: bound x %.0f..%.0f y %.0f..%.0f - %lld section "
                     "updates skipped outside it", g_boxX0, g_boxX1, g_boxY0, g_boxY1,
                     g_boundSkips);
            writeResult(bb);
        }
        if (g_shaderSkip) {
            char hb[96];
            snprintf(hb, sizeof(hb), "secsolve: shader layer left alone %lld times",
                     g_shaderSkips);
            writeResult(hb);
        }
        {
            char fb[160];
            snprintf(fb, sizeof(fb), "secsolve: frames=%lld between slices %.0f ms (outside the update %.0f ms, overlays %.0f ms)",
                     g_frames, g_frameGapMs, g_outsideMs, g_overlayMs);
            writeResult(fb);
        }
        if (g_shaderSig) {
            writeResult("shadersig: search end " + shaderSig(this));
            g_shaderSigPending = true;
        }
        {   // print only: the per-node vectors' own footprint at the end of the search (every
            // node ever made keeps its element), the nodes still holding a game state, and the
            // process's working set
            size_t live = 0, actBytes = 0;
            for (const auto& s : states) live += s ? 1 : 0;
            for (const auto& v : acts) actBytes += v.capacity();
            const double elemsMB =
                (double)(states.capacity() * sizeof(states[0]) + touches.capacity() * sizeof(touches[0])
                         + pulses.capacity() * sizeof(psnap::EMSnap)
                         + (snaps.capacity() + acts.capacity() + worlds.capacity())
                               * sizeof(std::vector<uint8_t>)) / (1024.0 * 1024.0);
            size_t curMB = 0, peakMB = 0;
            procMemMB(curMB, peakMB);
            char mb[256];
            snprintf(mb, sizeof(mb), "secmem: nodes=%zu liveStates=%zu elemsMB=%.0f actMB=%.0f "
                     "procMB=%zu peakMB=%zu", g_nodes.size(), live, elemsMB,
                     (double)actBytes / (1024.0 * 1024.0), curMB, peakMB);
            writeResult(mb);
        }
        if (foundLeaf >= 0) {
            char vb[280];
            snprintf(vb, sizeof(vb),
                     "secsolve_verify: %s deadAt=%d x=%.1f y=%.3f vy=%.3f "
                     "dy=%.4f dvy=%.4f dx=%.4f",
                     g_leafVerified ? "OK" : "FAIL", g_leafDeadAt, verifyX,
                     verifyY, verifyVy,
                     verifyY - (double)g_nodes[(size_t)foundLeaf].y,
                     verifyVy - (double)g_nodes[(size_t)foundLeaf].vy,
                     verifyX - (double)g_nodes[(size_t)foundLeaf].x);
            writeResult(vb);
            // The first depth at which the search's transitions and the plain replay
            // split. Unless this is -1, the section solver's premise ("checkpoint
            // branching = plain replay") is broken.
            char sb[280];
            snprintf(sb, sizeof(sb),
                     "secsolve_split: depth=%d/%d x %.3f->%.3f y %.3f->%.3f "
                     "vy %.3f->%.3f",
                     splitAt, foundDepth, splitNodeX, splitRepX,
                     splitNodeY, splitRepY, splitNodeVy, splitRepVy);
            writeResult(sb);
        }
        if (foundLeaf >= 0)
            writeResult("secsolve_inputs: " + inputsOf(foundLeaf));
        // ---- a RUNG gives the answer back to the loop (see g_secRung) -------------
        // The one-way handoff below ends the session; a rung splices what the search
        // found into the plan, pins the stretch so the ladder cannot re-plan it, and
        // lets the loop carry on -- GD verifies the new plan the way it verifies any
        // other, so the loop's invariant ("the prefix GD replayed is true") holds.
        if (g_secRung) {
            g_secRung = false;
            // cfg dpsecstate: the probe's answer is a verdict, not a plan (repair.hpp stateFire).
            const int stateKind = g_secState;
            g_secState = 0;
            if (stateKind) {
                secsolve::g_forceP1Size = -1;
                secsolve::g_keepP1Size = -1;
            }
            secsolve::g_on = false;
            secsolve::g_rungCoin = -1;
            if (unverLeaves > 0)
                writeResult("secrung: passed over " + std::to_string(unverLeaves)
                            + " leaf/leaves the plain replay did not reproduce");
            g_cfg.practiceAt = g_secSavePractice;
            g_cfg.checkpointAt = g_secSaveCkpt;
            g_cfg.fastloops = g_secSaveFastloops;
            g_cfg.maxAttempts = g_secSaveMaxAttempts;
            secsolve::g_targetDepth = g_secSaveTargetDepth;
            if (foundLeaf >= 0 && stateKind != 1) {
                // The input at depth d is the plain run's at tick ckptTick + d (the
                // measured pairing quoted at the verdict line above).
                const std::vector<uint8_t> seq = secsolve::inputVecOf(foundLeaf);
                const long long base = (long long)g_ckptTick;
                std::vector<InputCmd> plan;
                uint8_t held = 0;
                for (const InputCmd& c : g_cfg.inputs)
                    if ((long long)c.step < base) { plan.push_back(c); held = c.down ? 1 : 0; }
                for (size_t i = 0; i < seq.size(); ++i)
                    if (seq[i] != held) {
                        plan.push_back(InputCmd{(int)(base + (long long)i), seq[i] != 0});
                        held = seq[i];
                    }
                // cfg dpsecreuse: the rest of the plan this rung was fired from is kept through
                // the search after the splice, not pasted on after the window. Pasted, it died a
                // few ticks past the seam on almost every splice (lv4001, 57 splices, median 72
                // ticks and latterly 1-7): its inputs assume the state the MODEL expected there,
                // and the splice arrives in the state the game produced. So the splice flies as it
                // is, and its death keeps that plan's walk as the rejoin target (g_rjKeepTarget):
                // the next search, from the game's own state, comes back onto the plan where it
                // can and keeps its inputs from there.
                // (Not after a kept window of cfg dpsecstate: the rest of that plan is the size the
                // probe showed the wall will not let through.)
                if (g_cfg.dpSecReuse && stateKind != 2) {
                    dpsolve::g_rjKeepTarget = true;
                    writeResult("secrung: the rest of the plan stays the rejoin target of the "
                                "search after the splice");
                }
                g_cfg.inputs = plan;
                // ...and the loop's own record of the installed plan. The death books g_plan
                // as the plan GD flew: left as the plan the rung was fired from, a crossing
                // made by the splice is credited to that plan (g_best), the ladder keeps its
                // prefix under the next tail, and the fixup recorder resimulates it against
                // GD's recording of the splice -- a record of two plans, not of the model.
                dpsolve::g_plan = plan;
                dpsolve::g_anchorT = base;
                dpsolve::g_anchorX = anchors::row(base) ? anchors::row(base)->x : 0.f;
                // The splice reaches its target, not the end, and joins nothing.
                dpsolve::g_lastTailSolved = false;
                dpsolve::g_planClaimsGoal = false;
                dpsolve::g_rjTailJoined = false;
                // The model's walk of the plan this replaced is not a walk of the splice; the
                // death would keep it as "the plan that died" for the next search to rejoin.
                {
                    std::error_code ec;
                    std::filesystem::remove(dpsolve::g_planPath + ".trace.csv", ec);
                    std::filesystem::remove(dpsolve::g_tailPath + ".trace.csv", ec);
                }
                // Past the spliced ticks AND past the wall: an anchor in between hands the
                // crossing back to the model. If the spliced plan turns out to die before
                // the wall, every rung is pinned out and runLadder drops the pin.
                g_secPin = std::max(base + (long long)seq.size(), g_secPinWall);
                {   // the run counts a flight of this plan as the section search's (dprun via=secsolve)
                    std::vector<std::pair<long long, int>> se;
                    for (const InputCmd& c : plan) se.emplace_back((long long)c.step, c.down ? 1 : 0);
                    dpbridge::runNoteSplice(se, base);
                }
                if (stateKind == 2)   // cfg dpsecstate: the splice is the way on now
                    dpsolve::stateAdopt(plan, base + (long long)seq.size(),
                                        g_nodes[(size_t)foundLeaf].x);
                char rb[224];
                snprintf(rb, sizeof(rb),
                         "secrung: spliced %zu ticks from t=%lld (%zu inputs now), pinned at "
                         "t=%lld - the loop carries on",
                         seq.size(), base, plan.size(), (long long)g_secPin);
                writeResult(rb);
            } else {
                // The handoff installed the deepest plan (g_best) to replay to the section head;
                // that is what GD flies next, so it is the loop's plan too (see g_plan above).
                dpsolve::g_plan = g_cfg.inputs;
                writeResult(stateKind == 1
                                ? std::string("secrung: the probe ") + (foundLeaf >= 0 ? "crossed" : "did not cross")
                                      + " - a verdict, nothing is spliced; the loop carries on from the deepest plan"
                                : std::string("secrung: the search found nothing - the loop carries on from the "
                                              "deepest plan"));
            }
            // The section head is a one-shot of the SESSION: practice mode goes on once and the
            // checkpoint is placed once (resetSessionState clears both), and the one-way handoff
            // ends the session. A rung does not, so the next rung would skip practice mode and
            // its own checkpoint and search from this one's head. Measured (lv4003, old model):
            // a rung fired at t=7700 searched from x=3,836, the first rung's head at t=5184.
            // cfg dpsecrent: what this rung cost, counted (see kWorkSecStep) -- taken while the
            // head tick is still known; the next round starts. (cfg dpseccaptiers: whether a
            // larger cap could search this window differently -- only if this search's cap
            // dropped something and was the cap it was asked for, not the checkpoint path's
            // clamp above, which a larger request would meet the same way.)
            // (The probe and the kept windows of cfg dpsecstate run their own cap tiers, so they
            // never ask for the last window again at a larger cap; an ordinary window that only
            // keeps the run's size -- kind 3 -- is an ordinary window.)
            const bool ownTiers = stateKind == 1 || stateKind == 2;
            // A snapshot search that found nothing after its spine died before its plan did
            // (secsolve::g_spineUntil) is asked again at once on checkpoints -- the same window, the
            // same try: the ladder counts the checkpoint search's answer, not this one's.
            // cfg secredoreach: not when the snapshot search reached the plan's verified death anyway
            // -- its deepest live layer (ckptTick + deepest) at or past the tick the game flew the plan
            // to. The snapshot's lie then did not stop the search short of the wall the game met. The
            // re-ask that found the crossing (level 22's switch band) had its snapshot search dead at
            // t=2,552 against a death at t=2,938; in the cold logs of 09-29/30, 17 of 21 re-asks had
            // reached the death (level 21's wall at t=11,273 in every run: reach 11,276; a dual custom
            // level: 823 against 788), and every one of those checkpoint searches came back
            // EXHAUSTED, after 43-388 s.
            const long long snapReach = (long long)g_ckptTick + deepest;
            const bool reachedDeath = g_cfg.secRedoReach && secsolve::g_spineUntil > 0
                                      && snapReach >= secsolve::g_spineUntil - secsolve::kSpineSlack;
            // cfg seclieskippsnap: a plain snapshot search (its wsnap set held back) caught parting from
            // the game that found nothing marks its window, whose later asks put the set back at once.
            if (secsolve::g_lieSkipPsnap && secsolve::g_psnapLied && secsolve::g_wsnapHeld > 0
                && foundLeaf < 0 && !secsolve::liedWindow(secsolve::g_startTick)) {
                secsolve::g_liedWindows.push_back(secsolve::g_startTick);
                writeResult("secrung: window t=" + std::to_string(secsolve::g_startTick)
                            + " marked (cfg seclieskippsnap): its later asks start with the wsnap set");
            }
            // cfg secwsnapgate: a search that held its wsnap set back and found nothing is asked again
            // at once, the same window with the set put back (and the same coin and spine check); the
            // ladder counts that search's answer. It comes before a checkpoint re-ask, which costs more.
            const bool redoWsnap = secsolve::g_wsnapHeld > 0 && foundLeaf < 0 && stateKind == 0
                                   && g_secLastReq.start >= 0;
            const bool redoCp = !redoWsnap && secsolve::g_psnapLied && foundLeaf < 0 && stateKind == 0
                                && g_secLastReq.start >= 0 && !reachedDeath && g_cfg.secCpFallback;
            if (redoWsnap) {
                // (no checkpoint re-ask line: the wsnap re-ask comes first)
            } else if (secsolve::g_psnapLied && foundLeaf < 0 && stateKind == 0 && reachedDeath)
                writeResult("secrung: not asked again on checkpoints - the snapshot search lived to t="
                            + std::to_string(snapReach) + " against the plan's verified death at t="
                            + std::to_string(secsolve::g_spineUntil) + " (cfg secredoreach)");
            else if (secsolve::g_psnapLied && foundLeaf < 0 && stateKind == 0 && !g_cfg.secCpFallback)
                writeResult("secrung: not asked again on checkpoints (cfg seccpfallback=0) - the loop "
                            "carries on from the deepest plan");
            if (redoCp || redoWsnap) {
                g_secReqStart = g_secLastReq.start;
                g_secReqTarget = g_secLastReq.target;
                g_secReqDepth = g_secLastReq.depth;
                g_secReqHorizon = g_secLastReq.horizon;
                g_secReqCap = g_secLastReq.cap;
                // a coin wall's rung is never checked on checkpoints; the wsnap re-ask keeps its coin
                g_secReqCoin = redoWsnap ? g_secLastReq.coin : -1;
                g_secReqRung = true;
                g_secReqForceCp = redoCp;
                g_secReqForceWsnap = redoWsnap;
                if (redoWsnap) g_secReqSpineUntil = secsolve::g_spineUntil;
                g_secReqPending = true;
                if (redoCp)
                    writeResult("secrung: asking the same window again on checkpoints");
                else
                    writeResult("secrung: asking the same window again with its wsnap set of "
                                + std::to_string(secsolve::g_wsnapHeld)
                                + " objects (cfg secwsnapgate) - the plain snapshot search found nothing");
            } else {
                // (A checkpoint redo that found nothing is not searched again at a larger cap:
                // the ladder goes to its next window, further back -- on that level of 22 the
                // cap-200 retry of the same window only spent the time the run had left.)
                const bool cpRedo = g_secCpRedo;
                dpsolve::autoRungDone(g_ckptTick, steps, secsolve::g_snapOn,
                                      ownTiers ? true : foundLeaf >= 0,
                                      !ownTiers && !cpRedo && capTotal > 0
                                          && (g_secReqCap <= 0
                                              || secsolve::g_cap >= (size_t)g_secReqCap));
                if (ownTiers) dpsolve::stateRungDone(stateKind, foundLeaf >= 0);
            }
            g_secCpRedo = false;
            secsolve::g_wsnapForce = false;
            secsolve::g_wsnapHeld = 0;
            if (g_ckpt) { g_ckpt->release(); g_ckpt = nullptr; }
            g_ckptTick = -1;
            g_headHeld = 0;
            g_practiceOn = false;
            dpsolve::g_stop = false;     // the loop may spawn solves again
            g_paused = redoCp || redoWsnap;   // ...unless a redo is queued: held until it is taken
            g_forceCleanStart = true;    // drop the search's practice mode and checkpoints
            g_hudPhase = "secrung: replaying the spliced plan";
            // cfg secbookfirst: the coin books go back to the searched attempt's HERE, before the
            // reset below starts the next attempt. Left to coinBook's destructor, they went back
            // at co_return -- after resetLevel had already started that attempt and cleared them
            // (hooks_playlayer), or a checkpoint flight had spliced its own (cp_flight) -- so the
            // new attempt began with the old one's: its item counter, its coin pickup ticks, its
            // coinlive latches. GD's counter is 0 at an attempt's head (nothing announces the
            // reset: updateCounters is called only by item triggers), so every row up to the first
            // press carried the old count. SubZero 4003: the flight after a rung whose prefix had
            // pressed three times recorded item 1 = 3 from t=0, and anchors at t=6,612..13,212
            // passed --itembase 1:3 where GD held 0 (the next press there reads itemcnt v=1).
            if (secsolve::g_bookFirst) {
                coinBook.restore();
                coinBook.armed = false;
            }
            if (auto* pl = PlayLayer::get()) pl->resetLevel();
            // ...and only then the screen and the visibility pass (secRenderHold).
            secRenderRelease();
            co_return;                   // the search runs as a coroutine (see the slices)
        }
        // Go through the official end path. Just setting g_sessionOver directly does not
        // honor quitwhendone; the worker lingers and the caller times out
        endSession("secsolve");
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        ++g_pcCalls;   // sole direct indicator that physics runs (goes to the heartbeat)
        // Tick counter: this substep executes as number g_tick
        // Input injection: call handleButton directly before the target tick's substep
        // runs (exact injection in tick space, independent of the queue's
        // m_step/timestamp semantics)
        // While the section solver is running, it holds the button one tick at a time.
        // The plan's input feed is stopped (a restore resets g_tick and g_nextInput to 0,
        // so mixing them would replay the plan again from the section head).
        // The input cursor as this attempt's first tick on the active layer finds it, whatever
        // else has run since the attempt's reset (see g_nextInputAtStart).
        // An attempt sequence: a prelude attempt that reaches its `until` is restarted at the next safe point
        // (plan_io.hpp), as a person pressing restart; one that dies first is restarted by the game.
        if (!g_prelude.empty() && g_started && !g_sessionOver && !g_preludeResetPending
            && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this) {
            const size_t pidx = (size_t)(g_attempt > 0 ? g_attempt - 1 : 0);
            if (pidx < g_prelude.size() && g_prelude[pidx].until >= 0 && g_tick >= g_prelude[pidx].until
                && m_player1 && !m_player1->m_isDead) {
                g_preludeResetPending = true;
                g_preludePendingAttempt = g_attempt;
                writeResult("sequence: prelude attempt " + std::to_string(g_attempt) + " restarts at tick " + std::to_string((long long)g_tick));
            }
        }
        if (!g_attemptFed && !secsolve::g_active
            && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this) {
            g_attemptFed = true;
            g_nextInputAtStart = g_nextInput;
        }
        if (secsolve::g_active) {
            if (secsolve::g_feed != secsolve::g_held) {
                g_injecting = true;
                this->handleButton(secsolve::g_feed != 0, 1, true);
                g_injecting = false;
                secsolve::g_held = secsolve::g_feed;
            }
        // Only the active PlayLayer is fed, as only it advances g_tick (below). A level swapped
        // in (level_slice.hpp) is built and reset while the layer it replaces still runs the rest
        // of its frame, and those updates took the carried plan's inputs up to g_tick from the
        // cursor the new layer's reset had just set to 0: the new layer's first attempt then
        // started past the press at t=0. (The first swap carries no plan yet, so nothing was
        // taken there.) Measured on a custom level that starts as a ship: the copy's deepest plan,
        // flown on the level itself for a wall check, stayed on the ground from t=2 and died at
        // t=88, where the same plan had flown to t=27,528 on the copy and on the level itself --
        // a false parting at t=2 that put objects back around x=1. Fed only here, the flight
        // matched the copy's attempt tick for tick and died at t=27,528 the same way.
        } else if (g_started && !g_sessionOver
                   && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this) {
            while (g_nextInput < g_cfg.inputs.size()
                   && g_cfg.inputs[g_nextInput].step <= g_tick) {
                auto& in = g_cfg.inputs[g_nextInput];
                // A plan's player 2 edge (input bit 1) is a second button of its own only in a two-player level.
                // Elsewhere GD routes a player-2 press to player 1 (takeover.hpp noteInput), which would turn the
                // edge into a jump of player 1 -- so there it is not pressed, and said once.
                // The text of that line is also how the harness tells a package that has this input path from one that
                // does not (py/gdtas/gdbase.py PLAYER2_INPUT_MARK): reword it there too, or the guard lets plans through.
                const bool twoPlayerLevel = m_levelSettings && m_levelSettings->m_twoPlayerMode;
                if (in.p2 && !twoPlayerLevel) {
                    static bool said = false;
                    if (!said) {
                        said = true;
                        writeResult("plan: a player 2 input (bit 1) in a level that is not two-player - not pressed");
                    }
                    ++g_nextInput;
                    continue;
                }
                ev("INJECT_handleButton", in.step, in.down, in.p2);
                g_injecting = true;
                this->handleButton(in.down, 1 /*jump*/, !in.p2 /*player1*/);
                g_injecting = false;
                if (in.down && !in.p2) orbtrace::g_lastPress = g_tick;
                ++solver::g_injThisAttempt;
                ++g_nextInput;
            }
            while (g_nextToggle < g_cfg.toggles.size()
                   && g_cfg.toggles[g_nextToggle].step <= g_tick) {
                auto& tg = g_cfg.toggles[g_nextToggle];
                ev("INJECT_toggle", tg.step);
                auto* p = m_player1;
                if (p) {
                    if (tg.mode == "ship") p->toggleFlyMode(true, true);
                    else if (tg.mode == "ball") p->toggleRollMode(true, true);
                    else if (tg.mode == "ufo") p->toggleBirdMode(true, true);
                    else if (tg.mode == "wave") p->toggleDartMode(true, true);
                    else if (tg.mode == "robot") p->toggleRobotMode(true, true);
                    else if (tg.mode == "spider") p->toggleSpiderMode(true, true);
                    else if (tg.mode == "swing") p->toggleSwingMode(true, true);
                    else if (tg.mode == "cube") {
                        p->toggleFlyMode(false, true);
                    }
                }
                ++g_nextToggle;
            }
        }
        ev("processCommands", dt, isHalfTick, isLastTick);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        // Only the active PlayLayer advances the session bookkeeping. A scene swap takes
        // effect next frame, so within the same frame the old layer's remaining updates
        // still run and would dirty g_tick / the input cursor
        if (static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) return;
        // TRIED AND REMOVED (2026-09-01): skipping this counter for the frozen
        // updates after a checkpoint restore. The decompilation says resetLevel
        // sets layer+0x32a4 = 2 and getModifiedDelta then hands the next two
        // updates a dt of 0, which would let a restore spend two ticks without
        // advancing physics -- and that would explain hole 2's residual exactly
        // (with the dash restored the run matches for 22 ticks and ends 2 short
        // of 24, on a CLASSIC ball, where a dash has no duration and ends only
        // on release).
        //
        // It does not happen HERE. Instrumented and measured: skipped=0, and
        // the first update after the restore already carries dt=0.004167. So
        // whatever the freeze does, processCommands does not see it, no ticks
        // are spent on it, and the two-tick gap has another cause.
        ++g_tick;
        // cfg `robodbg=t0,t1`: one line per substep in a tick range, printed by
        // BOTH the plain replay and the section search because both come
        // through here. The robot's charge accumulator is the named suspect for
        // the spine coming apart three ticks into a sustained hold -- the audit
        // lists m_accelerationOrSpeed as saved by the checkpoint, but hole 2
        // taught that saved is not restored, and a search restores every tick.
        // Printed by NAME rather than by offset: the out-of-bounds latch cost a
        // run today for being read at the offset a decompilation appeared to
        // give (0x187, actually 0xC38).
        // In a search a layer is ~100 nodes wide, so only the spine's own substep
        // is printed (g_stepSpine, set beside the step). The plain replay has no
        // spine and prints every tick in the range, which is what it should be
        // compared against.
        //
        // The range is in GAME ticks on both sides, but g_tick does not mean the
        // same thing inside a search: the restore does not rewind it, so it
        // counts every step of every node and runs away from the level's own
        // clock within the first layer. The search line is labelled with the tick
        // its state corresponds to instead. The mapping was MEASURED by matching
        // y between the two sides (they agree to the last digit for over a
        // hundred layers, so it is unambiguous): the spine's state after the step
        // at depth d is the head's state at g_ckptTick + d. That is the same
        // alignment the tracking comparison uses -- which is written there as
        // ckpt+d+1 because it counts from the tick the window ASKED for, and the
        // checkpoint lands on the tick after it (secstart 5387 -> ckptTick 5388).
        const bool inSearch = secsolve::g_active;
        const long long tEff = inSearch ? g_ckptTick + g_stepDepth : g_tick;
        const bool roboLine = g_cfg.roboDbg0 >= 0 && m_player1
                              && (inSearch ? g_stepSpine : true);
        if (roboLine && tEff >= g_cfg.roboDbg0 && tEff <= g_cfg.roboDbg1) {
            // x and what drives it, and the level's mirror state: layer+0x418 is the flag
            // toggleFlipped writes, layer+0x41c the transition it tweens (0x2467d0 in 2.2081).
            // On a custom level (2026-09-26): a section search's x runs away the tick the player touches
            // the mirror portal (id 46), where GD's own run goes on at 1.2978 px a tick.
            const auto* lb = reinterpret_cast<const uint8_t*>(this);
            char rb[384];
            snprintf(rb, sizeof(rb),
                     "robodbg: t=%lld sec=%d d=%d y=%.4f vy=%.4f accel=%.6f "
                     "pad=%d ground=%d held=%d x=%.4f spd=%.4f left=%d side=%d plat=%d "
                     "flip=%d mirror=%.5f dt=%.6f",
                     (long long)tEff, inSearch ? 1 : 0,
                     inSearch ? g_stepDepth : -1,
                     m_player1->getPositionY(), m_player1->m_yVelocity,
                     m_player1->m_accelerationOrSpeed,
                     m_player1->m_touchedPad ? 1 : 0,
                     m_player1->m_isOnGround ? 1 : 0, secsolve::g_held,
                     m_player1->getPositionX(), m_player1->m_playerSpeed,
                     m_player1->m_isGoingLeft ? 1 : 0, m_player1->m_isSideways ? 1 : 0,
                     m_player1->m_isPlatformer ? 1 : 0, lb[gdoff::kLayerUnkBool10] ? 1 : 0,
                     *reinterpret_cast<const float*>(lb + gdoff::kLayerLevelFlipping), (double)dt);
            writeResult(rb);
        }
        // Keep what this pass does over each snapshot's
        // verification window, so the restored run can be held against the head
        // run without replaying the level a second time.
        if (g_cfg.snapVerify > 0 && m_player1) {
            const SnapState now{(long long)g_tick,
                                m_player1->getPositionX(),
                                m_player1->getPositionY(),
                                m_player1->m_yVelocity};
            if (g_snapProbe) g_snapProbe->push_back(now);
            else if (!g_snapVerified)
                for (auto& s : g_snaps)
                    if (g_tick > s.tick && g_tick <= s.tick + g_cfg.snapVerify)
                        s.head.push_back(now);
        }
        // Real positions of moving geometry (cfg `grouptrace=1`). This sits right after
        // ++g_tick so it gets the same tick numbers as dump/trace -- the model matches the
        // two on the same clock
        // Not inside a section search: g_tick there counts every step of every node (see tEff),
        // so its rows would sit on ticks past the level. They never reached a committed recording
        // (the rung's reset starts the attempt over), but each step of the search paid for them.
        if (grouptrace::g_on && g_started && !g_sessionOver && !secsolve::g_active)
            grouptrace::tick(g_tick);
        // The player's own hitbox rect (cfg `hitboxtrace=1`, pbox line). The
        // collidedWithObject side only shows "the tick something was touched", so emit one
        // line whenever mode/size changes, checkable even without a hit (a few dozen lines
        // per level)
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && m_player1) {
            static int lastMode = -1;
            static float lastSize = -1.f;
            const int md = m_player1->m_isShip ? 1 : m_player1->m_isBall ? 2
                         : m_player1->m_isBird ? 3 : m_player1->m_isDart ? 4
                         : m_player1->m_isRobot ? 5 : m_player1->m_isSpider ? 6
                         : m_player1->m_isSwing ? 7 : 0;
            if (md != lastMode || m_player1->m_vehicleSize != lastSize) {
                lastMode = md; lastSize = m_player1->m_vehicleSize;
                CCRect pr = m_player1->getObjectRect();
                char b[192];
                snprintf(b, sizeof(b),
                    "pbox: t=%lld mode=%d size=%.3f rect=(%.3f,%.3f,%.3f,%.3f)",
                    (long long)g_tick, md, m_player1->m_vehicleSize,
                    pr.origin.x, pr.origin.y, pr.size.width, pr.size.height);
                writeResult(b);
            }
        }
        // Have GD itself enumerate "what could kill the player right now"
        // (cfg `hitboxtrace=1` + the hbfrom/hbto window, `dmg:` line).
        //
        // Why it is needed: searching objrects around the death spot does not work for
        // objects that were moved (static cx,cy stay as at entry;
        // gd-locked-object-position-lies). grouptrace also does not track m_objectType
        // 7/20/22/31, so a culprit there shows up in neither. GD's damagingObjectsInRect
        // pulls by section and does not filter by type -- i.e. this is the only ground
        // truth table.
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && m_player1
            && g_tick >= g_cfg.hbFrom
            && (g_cfg.hbTo == 0 || g_tick <= g_cfg.hbTo)) {
            const CCRect pr = m_player1->getObjectRect();
            CCRect q = pr;
            q.origin.x -= 30.f; q.origin.y -= 30.f;
            q.size.width += 60.f; q.size.height += 60.f;
            // enabledGroups=true. With false, grouped objects drop out entirely -- that is
            // why lv22 x=16,165 looked like "0 solids", while the real culprit was the
            // solid block with groups=1 (uid 6271).
            CCArray* a = this->damagingObjectsInRect(q, true);
            {
                char h[160];
                snprintf(h, sizeof(h),
                    "dmgn: t=%lld n=%d q=(%.1f,%.1f,%.1f,%.1f)",
                    (long long)g_tick, a ? (int)a->count() : -1,
                    q.origin.x, q.origin.y, q.size.width, q.size.height);
                writeResult(h);
            }
            // Query the solid side with the same question. Spawned objects and objects
            // excluded by type are in neither objrects nor grouptrace, so only the section
            // pull is true.
            if (CCArray* s = this->staticObjectsInRect(q, true)) {
                for (auto* o : CCArrayExt<GameObject*>(s)) {
                    if (!o) continue;
                    const CCRect r = o->getObjectRect();
                    char b[224];
                    snprintf(b, sizeof(b),
                        "stat: t=%lld uid=%d id=%d type=%d rect=(%.3f,%.3f,%.3f,%.3f)",
                        (long long)g_tick, (int)o->m_uniqueID, (int)o->m_objectID,
                        (int)o->m_objectType, r.origin.x, r.origin.y,
                        r.size.width, r.size.height);
                    writeResult(b);
                }
            }
            if (a) {
                for (auto* o : CCArrayExt<GameObject*>(a)) {
                    if (!o) continue;
                    const CCRect r = o->getObjectRect();
                    char b[224];
                    snprintf(b, sizeof(b),
                        "dmg: t=%lld uid=%d id=%d type=%d rect=(%.3f,%.3f,%.3f,%.3f)"
                        " prect=(%.3f,%.3f,%.3f,%.3f)",
                        (long long)g_tick, (int)o->m_uniqueID, (int)o->m_objectID,
                        (int)o->m_objectType, r.origin.x, r.origin.y,
                        r.size.width, r.size.height,
                        pr.origin.x, pr.origin.y, pr.size.width, pr.size.height);
                    writeResult(b);
                }
            }
        }
        // The player's oriented hitbox (cfg `hitboxtrace=1`, pobb line, every tick).
        //
        // Why the rect alone is not enough: the hazard test read from the disassembly is a
        // dedicated loop inside GJBaseGameLayer::checkCollisions (win 0x2137f0):
        //
        //   1. AABB: player->getObjectRect() vs the other's rect (strict comparison)
        //   2. if the other has m_shouldUseOuterOb, OBB-vs-OBB overlap
        //   3. if it passes, this->destroyPlayer(player, obj)
        //
        // a two-stage setup. collidedWithObject is never taken (measured: not one
        // hbox/hbin line on the lethal tick). Stage 1 is visible via pbox, but the
        // player-side box of stage 2 appeared nowhere. OBB2D is assembled by
        // GameObject::updateOrientedBox from m_width*..*scaleX / m_height*..*scaleY and
        // the rotation, so emitting the real 4 corners is faster and surer than guessing
        // the dimensions from outside.
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && m_player1) {
            static int pobbLines = 0;
            if (++pobbLines <= 40000) {
                if (OBB2D* ob = m_player1->getOrientedBox()) {
                    const auto& c = ob->m_corners;
                    // The 3 flags that decide the rotation's sign + the speed.
                    // runNormalRotation (win 0x38d220), the real thing:
                    //   m_rotationSpeed = 180 * s(+0x9bf) * s(+0x9c2) * s(+0x9c3)
                    //                     * [+0xb84] * speedArg / k
                    //   k = 0.4333333 (player size 1.0) / 0.3333333 (otherwise)
                    // 180/0.4333333 = 415.3846 deg/s = 1.730769 deg/tick,
                    // 180/0.3333333 = 540 deg/s = 2.25 deg/tick -- exact match with
                    // measurement. The magnitude is closed, so only these 3 signs remain.
                    // Their names cannot be pulled from bindings, so emit raw bytes.
                    const char* pb = reinterpret_cast<const char*>(m_player1);
                    const int f1 = (int)(unsigned char)pb[gdoff::kPlayerUpsideDown];
                    const int f2 = (int)(unsigned char)pb[gdoff::kPlayerGoingLeft];
                    const int f3 = (int)(unsigned char)pb[gdoff::kPlayerSideways];
                    float rmul = 0.f, rspd = 0.f;
                    std::memcpy(&rmul, pb + gdoff::kPlayerGravityMod, sizeof(float));
                    std::memcpy(&rspd, pb + gdoff::kPlayerRotationSpeed, sizeof(float));
                    char b[512];
                    snprintf(b, sizeof(b),
                        "pobb: t=%lld size=%.3f rot=%.3f "
                        "c=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f) "
                        "f=%d,%d,%d mul=%.4f rspd=%.4f",
                        (long long)g_tick, m_player1->m_vehicleSize,
                        m_player1->getRotation(),
                        c[0].x, c[0].y, c[1].x, c[1].y,
                        c[2].x, c[2].y, c[3].x, c[3].y,
                        f1, f2, f3, rmul, rspd);
                    writeResult(b);
                }
            }
        }
        // Dump the PlayerObject flag region as raw bytes (cfg `hitboxtrace=1` +
        // the `hbfrom`/`hbto` window, `pflags:` line).
        //
        // Why it is needed: on 2026-08-19 we hit the contradiction that "the same state
        // (gravity-flipped ship, zero input, sp0.9, gravityMod=1, vsize=1, and pobb's
        // 0x9bf/0x9c2/0x9c3 identical too) takes different ship acceleration branches on
        // the calibration rig vs the corpus". GD appears to have a second
        // gravity flag, but it has no name in bindings, so diffing raw bytes is the only
        // way. 0x9a0..0x9e0 is the band collidedWithObjectInternal uses for the mode test
        // (0x9b9 ship / 0x9ba UFO / 0x9bb ball / 0x9bc wave / 0x9bd robot /
        // 0x9be spider / 0x9c4 swing / 0x9bf gravity).
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && m_player1
            && g_tick >= g_cfg.hbFrom
            && (g_cfg.hbTo == 0 || g_tick <= g_cfg.hbTo)) {
            const char* pb = reinterpret_cast<const char*>(m_player1);
            // 0x600-0xa20 in chunks of 0x80 bytes (384 characters per line)
            for (int base = (int)gdoff::kPlayerFlagsDumpBegin;
                 base < (int)gdoff::kPlayerFlagsDumpEnd; base += 0x80) {
                char b[600];
                int n = snprintf(b, sizeof(b), "pflags: t=%lld o=%03x",
                                 (long long)g_tick, base);
                for (int off = base; off < base + 0x80
                                     && n < (int)sizeof(b) - 8; ++off)
                    n += snprintf(b + n, sizeof(b) - n, " %02x",
                                  (unsigned)(unsigned char)pb[off]);
                writeResult(b);
            }
        }
        // What the player is standing on (cfg `standtrace=1`, stand line).
        //
        // Emitted to name invisible surfaces. At lv20 x=6,342 GD lands the mini ship
        // (onGround=1 vy=0) and it then slides down at -1.01/tick to its death, yet the
        // surface is in neither objrects nor grouptrace (0 objects of type 0/2/25/47 in
        // x 6,320-6,390 / y 230-320, and 0 moving ones). The object does not exist at load
        // time, so instead of reverse-lookup by position, ask for the pointer GD holds.
        // Emit only on ticks where it changed (a few dozen lines per level).
        if (g_cfg.standTrace && g_started && !g_sessionOver && m_player1) {
            static int lastSnap = -2, lastSlope = -2, lastTgt = -2;
            auto* p = m_player1;
            GameObject* sn = p->m_objectSnappedTo;
            GameObject* sl = static_cast<GameObject*>(p->m_currentSlope);
            // The raw field holding the spider's teleport target.
            // Disassembly (2.208): the body of PlayerObject::spiderTestJump is at
            // 0x393ff0; inside it, 0x394200 accumulates the running maximum into
            // `[this+0x968]` with maxsd, and writes the object it will land on to
            // [this+0x5e8] (right after 0x39083B). m_objectSnappedTo stays stuck at
            // t=1,469 and is never updated, so the teleport target surface can only be
            // named by this raw field. 0x960/0x968 are the candidates' min/max.
            GameObject* tg =
                *reinterpret_cast<GameObject**>((char*)p + gdoff::kPlayerCurrentSlope2);
            const double tgLo =
                *reinterpret_cast<double*>((char*)p + gdoff::kPlayerCollidedBottomMaxY);
            const double tgHi =
                *reinterpret_cast<double*>((char*)p + gdoff::kPlayerCollidedLeftMaxX);
            const int snId = sn ? sn->m_uniqueID : -1;
            const int slId = sl ? sl->m_uniqueID : -1;
            const int tgId = tg ? tg->m_uniqueID : -1;
            if (snId != lastSnap || slId != lastSlope || tgId != lastTgt) {
                lastSnap = snId; lastSlope = slId; lastTgt = tgId;
                char b[352];
                auto pos = [](GameObject* o, char* dst, size_t n) {
                    if (!o) { snprintf(dst, n, "-"); return; }
                    auto q = o->getPosition();
                    auto r = o->getObjectRect();
                    snprintf(dst, n, "uid%d id%d type%d @%.2f,%.2f %.2fx%.2f",
                             o->m_uniqueID, o->m_objectID, (int)o->m_objectType,
                             q.x, q.y, r.size.width, r.size.height);
                };
                char sb[160], lb[160], tb[160];
                pos(sn, sb, sizeof(sb));
                pos(sl, lb, sizeof(lb));
                pos(tg, tb, sizeof(tb));
                char b2[560];
                snprintf(b2, sizeof(b2),
                         "stand: t=%lld y=%.3f onG=%d snap=[%s] slope=[%s] "
                         "tgt=[%s] lo=%.3f hi=%.3f",
                         (long long)g_tick, m_player1->getPositionY(),
                         (int)m_player1->m_isOnGround, sb, lb, tb, tgLo, tgHi);
                (void)b;
                writeResult(b2);
            }
        }
        // Detection of missed speed portals. Detection runs always (no side effects).
        // Going as far as rejection only happens with cfg `speedgate=1`
        if (g_started && !g_sessionOver && m_player1) {
            speedgate::build(this);
            speedgate::check(m_player1, g_tick);   // detect and report only (no side effects)
        }
        // Record speed change points (cfg `syncprobe=1`). If one was missed, the steps
        // shift from that point on
        if (g_syncProbe && g_started && !g_sessionOver && m_player1) {
            static float s_lastSpd = -1.f;
            float spd = (float)m_player1->m_playerSpeed;
            if (spd != s_lastSpd) {
                s_lastSpd = spd;
                char sp[160];
                snprintf(sp, sizeof(sp), "speed: %.2f at tick=%lld x=%.0f y=%.0f",
                    spd, (long long)g_tick, m_player1->getPositionX(),
                    m_player1->getPositionY());
                writeResult(sp);
            }
        }
        // Count idle ticks while dead (evidence for whether they can be skipped). One
        // line, so always collected
        if (g_started && !g_sessionOver && m_player1)
            deadstat::tick(m_player1->m_isDead);
        // Real positions of moving gates (cfg `gatetrace`). Where a moving block is at any
        // time can only be seen from real positions
        if (solver::g_gateTrace && !solver::g_gateObjs.empty()
            && g_tick >= solver::g_gateT0 && g_tick <= solver::g_gateT1
            && (solver::g_gateStride <= 1 || (g_tick % solver::g_gateStride) == 0)) {
            if (!solver::g_gateHdrDone) {
                solver::g_gateHdrDone = true;
                for (auto* o : solver::g_gateObjs) {
                    if (!o) continue;
                    char hb[160];
                    auto r = o->getObjectRect();
                    snprintf(hb, sizeof(hb),
                        "gateobj: id=%d uid=%d x=%.2f y=%.2f rect=%.2f,%.2f,%.2f,%.2f",
                        o->m_objectID, o->m_uniqueID,
                        o->getPositionX(), o->getPositionY(),
                        r.origin.x, r.origin.y, r.size.width, r.size.height);
                    writeResult(hb);
                }
            }
            std::string s = "gate: tick=" + std::to_string(g_tick);
            if (m_player1) {
                char pb[224];
                // Emit the real hitbox and GD's own collision record (lcol)
                auto pr = m_player1->getObjectRect();
                auto* g = m_player1->m_maybeLastGroundObject;
                snprintf(pb, sizeof(pb),
                    " px=%.1f py=%.1f prect=%.2f,%.2f,%.2f,%.2f ground=%d@%.0f,%.0f"
                    " lcol=%d,%d,%d,%d lk=%d%d",
                    m_player1->getPositionX(), m_player1->getPositionY(),
                    pr.origin.x, pr.origin.y, pr.size.width, pr.size.height,
                    g ? g->m_objectID : -1,
                    g ? g->getPositionX() : 0.f, g ? g->getPositionY() : 0.f,
                    m_player1->m_lastCollisionBottom, m_player1->m_lastCollisionTop,
                    m_player1->m_lastCollisionLeft, m_player1->m_lastCollisionRight,
                    (int)m_player1->m_isLocked, (int)m_player1->m_controlsDisabled);
                s += pb;
            }
            for (auto* o : solver::g_gateObjs) {
                if (!o) continue;
                char b[96];
                // Also emit the effective hitbox state (even with the position fixed, a
                // toggle can remove the hitbox).
                // Flag sequence = m_isDisabled, m_isHide, visible, m_isGroupDisabled,
                // m_isGroupDisabledTemp, m_hasExtendedCollision, m_isPassable
                snprintf(b, sizeof(b), " [%d %.0f,%.0f %d%d%d%d%d%d%d]",
                    o->m_objectID, o->getPositionX(), o->getPositionY(),
                    (int)o->m_isDisabled, (int)o->m_isHide, (int)o->isVisible(),
                    (int)o->m_isGroupDisabled, (int)o->m_isGroupDisabledTemp,
                    (int)o->m_hasExtendedCollision, (int)o->m_isPassable);
                s += b;
            }
            writeResult(s);
        }
        // State injection probe (cfg `colprobe`, explained in solver.hpp). Injection
        // happens after tick T's physics update, so it takes effect from the T+1
        // transition
        if (solver::g_colProbe && m_player1 && !g_sessionOver) {
            if (g_tick == solver::g_cpT && !solver::g_cpInjected
                && !m_player1->m_isDead) {
                solver::g_cpInjected = true;
                const double y = solver::g_cpY0
                    + (double)(g_attempt - 1) * solver::g_cpYStep;
                m_player1->setPositionY((float)y);
                m_player1->m_yVelocity = solver::g_cpVy;
                solver::g_cpEndTick = g_tick + solver::g_cpN;
                char ib[128];
                snprintf(ib, sizeof(ib), "cprobe: inject a=%d t=%lld x=%.2f y=%.3f vy=%.3f",
                    g_attempt, (long long)g_tick, m_player1->getPositionX(),
                    y, solver::g_cpVy);
                writeResult(ib);
            }
            if (solver::g_cpInjected && g_tick > solver::g_cpT
                && g_tick <= solver::g_cpEndTick) {
                char cb[176];
                snprintf(cb, sizeof(cb),
                    "cprobe: a=%d t=%lld x=%.2f y=%.3f vy=%.3f dead=%d lcol=%d,%d,%d,%d",
                    g_attempt, (long long)g_tick,
                    m_player1->getPositionX(), m_player1->getPositionY(),
                    (double)m_player1->m_yVelocity, (int)m_player1->m_isDead,
                    m_player1->m_lastCollisionBottom, m_player1->m_lastCollisionTop,
                    m_player1->m_lastCollisionLeft, m_player1->m_lastCollisionRight);
                writeResult(cb);
                if (g_tick == solver::g_cpEndTick && !m_player1->m_isDead) {
                    // Observation done. If still alive, kill and advance to the next attempt
                    if (auto* pl = PlayLayer::get())
                        pl->destroyPlayer(m_player1, m_player1);
                }
            }
        }
        // State injection and cutoff for analysis (`inject=` / `stopat=` in plan_in.txt).
        // Injection happens after tick T's physics update, so it takes effect from the
        // T+1 transition. A tool to remove the need to "first solve a plan that reaches
        // that state"
        if (m_player1 && !g_sessionOver && !m_player1->m_isDead) {
            // Discard injections whose tick was skipped (T was jumped over by a checkpoint
            // restore etc.). If kept, they suddenly fire at a much later tick and look
            // like an unexplained branch
            while (g_injectNext < g_injects.size()
                   && g_injects[g_injectNext].tick < g_tick) ++g_injectNext;
            while (g_injectNext < g_injects.size()
                   && g_injects[g_injectNext].tick == g_tick) {
                auto& s = g_injects[g_injectNext++];
                if (s.hasX)  m_player1->setPositionX((float)s.x);
                if (s.hasY)  m_player1->setPositionY((float)s.y);
                if (s.hasVy) m_player1->m_yVelocity = s.vy;
                char ib[192];
                snprintf(ib, sizeof(ib),
                    "inject: t=%lld x=%.3f y=%.3f vy=%.3f mode=%s",
                    (long long)g_tick, m_player1->getPositionX(),
                    m_player1->getPositionY(), (double)m_player1->m_yVelocity,
                    modeStr(m_player1));
                writeResult(ib);
            }
            // Cutoff: report the state, then kill. The report must come first --
            // destroyPlayer emits a death: line, and in the reverse order the caller
            // cannot distinguish "cutoff" from "a real death"
            if (g_stopAt >= 0 && g_tick == g_stopAt && !g_stopFired) {
                char sb[256];
                snprintf(sb, sizeof(sb),
                    "stop: attempt=%d tick=%lld x=%.3f y=%.3f yvel=%.3f mode=%s "
                    "onGround=%d vsize=%.3f",
                    g_attempt, (long long)g_tick, m_player1->getPositionX(),
                    m_player1->getPositionY(), (double)m_player1->m_yVelocity,
                    modeStr(m_player1), (int)m_player1->m_isOnGround,
                    (double)m_player1->m_vehicleSize);
                writeResult(sb);
                g_stopFired = true;   // never twice per attempt (the directive itself is kept)
                if (auto* pl = PlayLayer::get())
                    pl->destroyPlayer(m_player1, m_player1);
            }
        }
        // Observing the trigger of automatic checkpoint placement (cfg `ckpttrace=1`).
        // Catches at tick resolution the moment GD decides it "wants to place one"
        // (m_shouldTryPlacingCheckpoint). The actual creation/storage is picked up by the
        // PlayLayer-side hook, so this is for transitions only
        if (solver::ckpttrace::g_on && g_started && !g_sessionOver && m_player1) {
            int tryNow = m_player1->m_shouldTryPlacingCheckpoint ? 1 : 0;
            int toNow  = m_player1->m_checkpointTimeout ? 1 : 0;
            if (tryNow != solver::ckpttrace::g_lastTry
                || toNow != solver::ckpttrace::g_lastTimeout) {
                // Do not flood result.txt if transitions turn out to happen every tick.
                // Always say the cap was hit (cutting silently looks like "nothing
                // happened after that")
                if (++solver::ckpttrace::g_flagLines > solver::ckpttrace::kFlagCap) {
                    if (solver::ckpttrace::g_flagLines == solver::ckpttrace::kFlagCap + 1)
                        writeResult("ckpt: flag   (capped at "
                            + std::to_string(solver::ckpttrace::kFlagCap)
                            + " lines; further transitions not logged)");
                    solver::ckpttrace::g_lastTry = tryNow;
                    solver::ckpttrace::g_lastTimeout = toNow;
                } else {
                char cb[224];
                snprintf(cb, sizeof(cb),
                    "ckpt: flag   tick=%lld x=%.2f try=%d->%d timeout=%d->%d "
                    "lastCkptT=%.6f totalT=%.6f",
                    (long long)g_tick, m_player1->getPositionX(),
                    solver::ckpttrace::g_lastTry, tryNow,
                    solver::ckpttrace::g_lastTimeout, toNow,
                    m_player1->m_lastCheckpointTime, m_player1->m_totalTime);
                writeResult(cb);
                solver::ckpttrace::g_lastTry = tryNow;
                solver::ckpttrace::g_lastTimeout = toNow;
                }
            }
        }
        // In-memory per-tick record of grounded/mode/coords (used by diag's traj and
        // clearance::observe). Always recorded during a session. g_started/!g_sessionOver
        // are required: without them the per-tick recording keeps running in normal play
        // after the session ends
        if (g_started && !g_sessionOver && m_player1) {
            if ((long long)solver::g_log.size() <= g_tick)
                solver::g_log.resize(g_tick + 1);
            // In dual, record p2's grounded state too (one input moves both bodies, so a
            // press is meaningful if p2 is grounded even while p1 is airborne)
            bool dual = m_gameState.m_isDualMode;
            // Have it say once whether the flag is really set (so it is visible from
            // outside that it is in effect)
            if (dual && !solver::g_dualSeen) {
                solver::g_dualSeen = true;
                writeResult("dual: entered at tick=" + std::to_string((long long)g_tick)
                    + " x=" + std::to_string((int)m_player1->getPositionX())
                    + " p2=" + (m_player2 ? "yes" : "NO"));
            }
            solver::g_log[g_tick] = {
                m_player1->getPositionX(),
                m_player1->getPositionY(),
                (uint8_t)(m_player1->m_isOnGround ? 1 : 0),
                solver::modeOf(m_player1),
                (uint8_t)(m_player1->m_isOnGround2 ? 1 : 0),
                (float)m_player1->m_yVelocity,
                (uint8_t)(dual ? 1 : 0),
                (uint8_t)((dual && m_player2 && m_player2->m_isOnGround) ? 1 : 0)
            };
        }
        // 10-second heartbeat for plain replay. A plain replay writes nothing to
        // result.txt until it finishes, so this keeps the outer freeze detector
        // (no-update guard) from wrongly killing a healthy verification run
        if (g_started && !g_sessionOver && m_player1) {
            static auto s_last = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_last).count() >= 10) {
                s_last = now;
                writeResult("replay: t=" + std::to_string(g_tick)
                    + " x=" + std::to_string(m_player1->getPositionX())
                    + " y=" + std::to_string(m_player1->getPositionY()));
            }
        }
        // ...and a STALL guard on the same gate. A plan that leaves the player wedged
        // never ends its attempt. GD has no timeout of its own, so the fast loop spins
        // on a run that is already over and eats the rest of the budget. Measured on
        // lv22 (2026-08-24): a plan whose tail left the player stuck in a rotated frame
        // held BOTH coordinates at (20115.000000, 829.285767) to the last digit for 6.9
        // MILLION ticks and was still going when the session was killed -- one iteration
        // took the remaining 11 minutes of a 25-minute run. offBoardTick cannot see it:
        // the player is on the board, it just is not moving.
        //
        // Both coordinates have to stand still. The travel axis is x in an upright frame
        // and y in a turned one, so either one alone stands still legitimately and the
        // pair does not. The comparison is exact on purpose -- any motion at all, down
        // to the last bit, is a run that is still going.
        //
        // The threshold started at 30,000 ("only has to beat forever"), but the glitch
        // wedges turned out to be ROUTINE -- 76 of them in one run, each paying the whole
        // fuse in fast-loop time -- so it is now economical instead: 3,000 is 4.5x the
        // longest legitimate still-stand that ends a run in the corpus (668 ticks, lv22's
        // end-of-level pin). The
        // pause/freeze paths above return before g_tick advances, so a paused or
        // held-still session never accumulates any of it, and a new attempt (g_tick
        // going backwards) rearms it.
        // `!m_isDead`: a corpse is perfectly still through GD's death-to-reset delay,
        // and the fast loop pumps tens of thousands of ticks through that window --
        // counted, the guard re-fires on the corpse and every downstream tick number
        // (stall report, wedge credit, depth score) inherits the inflation.
        // !noDeath: the bootstrap recording pass (and any other nodeath observation
        // run) swallows its deaths and legitimately runs the whole level and beyond --
        // the overlong guard was killing it at 40k ticks forever, and the session never
        // got past attempt one.
        // !secsolve::g_active: a section search's steps are not the attempt's ticks. They count
        // on in g_tick, and on a custom level's last search (head t=27,072) the overlong guard
        // fired at 40,001 inside it and killed whichever branch was being stepped.
        if (g_started && !g_sessionOver && m_player1 && !m_player1->m_isDead
            && !g_cfg.noDeath && !secsolve::g_active) {
            static float s_stallX = 0.f, s_stallY = 0.f;
            static long long s_stallSince = -1;
            // The END-ZONE pin is not a stall: GD locks the player at the end wall
            // while the scheduler-driven completion sequence plays, and under the fast
            // loop thousands of "stillness" ticks pass in a frame or two of real time.
            // The guards wait ENDZONE_KILL_AFTER_SEC of WALL time for a locked player
            // (measured: the fr110 lv11 solution was being stall-killed mid-clear at
            // x=29,503, and the armed completion then leaked into the next attempt as
            // an endscreen at t=0). A HUNG sequence -- the wrong-mode zombie that
            // never completes -- runs past the bound and is put down as before.
            // The lock clock is armed HERE, per tick, not only at the frame
            // boundary: one frame is fastloops x (fastdt*240) = 7,200 ticks, so
            // the pin and the 3,000-tick stillness both happen INSIDE one batch
            // and a boundary-only clock never sees the lock at all (measured:
            // the first build of this fix changed nothing on lv11).
            if (m_player1->m_isLocked && m_player1->getPositionX() > 500.f
                && g_endzoneLockStart.time_since_epoch().count() == 0)
                g_endzoneLockStart = std::chrono::steady_clock::now();
            const double lockWall =
                (m_player1->m_isLocked
                 && g_endzoneLockStart.time_since_epoch().count() != 0)
                    ? std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - g_endzoneLockStart).count()
                    : -1.0;
            const bool endzoneGrace = lockWall >= 0.0 && lockWall < ENDZONE_KILL_AFTER_SEC;
            // ...and once the grace has run out the sequence is hung, so a short
            // stillness suffices -- at 1 tick/frame the old 3,000 would cost most of a
            // minute of wall time per zombie attempt.
            const bool endzoneHung = lockWall >= ENDZONE_KILL_AFTER_SEC;
            // ...and an attempt that simply never ends. The MOVING form of the zombie
            // glitch bobs on a platform at 100%% forever -- no stillness, no death, no
            // completion -- and the fast loop pumped 1.7M ticks through one before
            // anything noticed. No legitimate attempt on these levels exceeds ~22k
            // ticks, so 40k is pure pathology.
            static int s_overFired = -1;   // once per attempt -- this runs per TICK, and
                                           // unlatched it wrote 1,800 lines per frame
            // ...except that a level can be longer than that: one custom level runs ~52,000 ticks (84,333 px at
            // 1.3 px a tick and up), and this put down every plan that was still alive at 40,000, which the
            // loop read as a death and, over the empty floor it had run on, as a void run to close. The bound
            // is the longer of 40,000 and the level's length at the slowest speed (0.7x = 0.909 px a tick).
            static long long s_stallBound = 40000;
            static int s_boundAttempt = -1;   // read once per attempt, on its first tick past 40,000
            if (g_tick > 40000 && s_boundAttempt != g_attempt) {
                s_boundAttempt = g_attempt;
                s_stallBound = 40000;
                if (g_cfg.stallBound)
                if (auto* plb = PlayLayer::get())
                    s_stallBound = std::max<long long>(40000, (long long)((double)plb->m_levelLength / 0.9) + 4000);
            }
            if (g_tick > 40000 && g_tick > s_stallBound && s_overFired != g_attempt && !endzoneGrace) {
                s_overFired = g_attempt;
                char ob2[160];
                snprintf(ob2, sizeof(ob2), "stall: attempt=%d has run %lld ticks without "
                         "ending - forcing it over", g_attempt, (long long)g_tick);
                writeResult(ob2);
                if (auto* pl = PlayLayer::get()) {
                    pl->destroyPlayer(m_player1, m_player1);
                    g_stallResetPending = true;
                }
            }
            const float px = m_player1->getPositionX();
            const float py = m_player1->getPositionY();
            if (s_stallSince < 0 || px != s_stallX || py != s_stallY
                || g_tick < s_stallSince) {
                s_stallX = px; s_stallY = py; s_stallSince = g_tick;
            } else if ((g_tick - s_stallSince >= 3000 && !endzoneGrace)
                       || (endzoneHung && g_tick - s_stallSince >= 300)) {
                char sb[256];
                snprintf(sb, sizeof(sb),
                    "stall: attempt=%d the player has not moved since t=%lld "
                    "(x=%.3f y=%.3f) - ending the attempt",
                    g_attempt, (long long)s_stallSince, (double)px, (double)py);
                writeResult(sb);
                s_stallSince = -1;
                if (auto* pl = PlayLayer::get()) {
                    pl->destroyPlayer(m_player1, m_player1);
                    // ...and destroyPlayer alone is NOT a guarantee, so a reset is
                    // forced as well. In the glitch state this guard exists for,
                    // the level believes itself finished and GD's destroy books the
                    // death without ever scheduling the restart: measured
                    // 2026-08-25, g_tick kept counting across "attempts" (57,751 ->
                    // 98,522 -> ..., +40,771 each) and the loop scored those
                    // inflated ticks as the deepest runs ever seen.
                    // DEFERRED to the frame boundary, not called here: a reset from
                    // inside the fast loop lets the rest of this frame's batch run
                    // on the fresh attempt with a stale input cursor -- measured as
                    // garbage deaths at t=660/1,837 on a prefix that had verified
                    // clean dozens of times, each one feeding false fixups.
                    g_stallResetPending = true;
                }
            }
        }
        // Measure the "uncontrollable boundary" of the end zone (cfg `endtrace=1`).
        // The completion flag (m_hasCompletedLevel) is set after the pull-in has made the
        // player uncontrollable = it is not the end of controllability, so measure which
        // flag drops first
        if (g_cfg.endTrace && g_started && !g_sessionOver && m_player1) {
            static long long s_lastTick = -1;
            static int s_prev = -1, s_lines = 0;
            if (g_tick <= s_lastTick) { s_prev = -1; s_lines = 0; } // the attempt changed
            s_lastTick = g_tick;
            auto* pl = PlayLayer::get();
            int lk = m_player1->m_isLocked ? 1 : 0;
            int co = m_player1->m_controlsDisabled ? 1 : 0;
            int cp = (pl && pl->m_hasCompletedLevel) ? 1 : 0;
            int dd = m_player1->m_isDead ? 1 : 0;
            int st = lk | (co << 1) | (cp << 2) | (dd << 3);
            if (st != s_prev && ++s_lines <= 40) {
                writeResult("endtrace: t=" + std::to_string(g_tick)
                    + " x=" + std::to_string(m_player1->getPositionX())
                    + " y=" + std::to_string(m_player1->getPositionY())
                    + " vy=" + std::to_string((float)m_player1->m_yVelocity)
                    + " locked=" + std::to_string(lk)
                    + " ctrlOff=" + std::to_string(co)
                    + " completed=" + std::to_string(cp)
                    + " dead=" + std::to_string(dd)
                    + " mode=" + modeStr(m_player1));
            }
            s_prev = st;
        }
        // Our own coin pickup test: a coordinate hit within a conservative radius counts
        // as picked up and the pickup tick is recorded (independent of GD's in-checkpoint
        // coin state. On a checkpoint restore, pickups after the restore tick are voided)
        if (g_started && !g_sessionOver && g_cfg.coinMode && m_player1) {
            float px = m_player1->getPositionX();
            float py = m_player1->getPositionY();
            // Pickups only during "normal motion" (see g_prevTickX in solver.hpp).
            // m_hasCompletedLevel alone cannot exclude the pull-in animation (it is set
            // too late), so the player-side uncontrollable flags
            // (locked / controlsDisabled) are also part of the condition
            float dxTick = px - solver::g_prevTickX;
            bool completed = false;
            if (auto* pl = PlayLayer::get()) completed = pl->m_hasCompletedLevel;
            bool uncontrolled = m_player1->m_isLocked || m_player1->m_controlsDisabled;
            bool normalMotion = solver::g_prevTickX > -1e8f
                && dxTick > 0.2f && dxTick < 5.0f && !completed && !uncontrolled;
            solver::g_prevTickX = px;
            // cfg routeprereq: when each coin's prerequisite came on in this attempt. Not inside a
            // section search, whose branches are not the attempt's (the same reason the coin cut
            // below stays off there).
            if (g_cfg.routePrereq && route::g_built && !secsolve::g_active && !secsolve::g_on)
                route::sample(g_tick);
            // ...and the coins' own track for this attempt (solver::g_coinPosLog, read by the
            // loop's closest-approach test): a row when a coin moves or switches.
            if (solver::g_coinPosLog.size() == solver::g_coins.size()
                && solver::g_coinObjs.size() == solver::g_coins.size())
                for (size_t i = 0; i < solver::g_coins.size(); ++i) {
                    GameObject* o = solver::g_coinObjs[i];
                    if (!o) continue;
                    // The hit box, not getPosition(): a Move trigger's displacement shows in the
                    // object rect straight away (the grouptrace reads the same), while the node
                    // position of SubZero 4002's third coin still said y=2,103 after it had
                    // dropped to 1,503 -- the track then placed its "final spot" up there.
                    const auto lr = o->getObjectRect();
                    const float lx = lr.origin.x + lr.size.width * 0.5f;
                    const float ly = lr.origin.y + lr.size.height * 0.5f;
                    const uint8_t on = o->m_isGroupDisabled ? 0 : 1;
                    auto& lg = solver::g_coinPosLog[i];
                    if (lg.empty() || lg.back().x != lx || lg.back().y != ly || lg.back().on != on)
                        lg.push_back({(long long)g_tick, lx, ly, on});
                }
            // WHERE EACH COIN IS, not where it loaded, said once as the player
            // comes level with it. The two differ whenever a group has moved it,
            // and every other line here prints the load-time row -- which is how
            // lv21's third coin read as "sitting at (21813,191) and refusing to
            // be collected" when the question was whether it is there at all.
            // Sized here rather than at the attempt head: the coins are not
            // extracted until buildPois runs, which is later in that same reset.
            if (solver::g_coinLiveSaid.size() != solver::g_coins.size())
                solver::g_coinLiveSaid.assign(solver::g_coins.size(), 0);
            // cfg `coinwatch=1`: say it every 1,200 ticks instead of once, so a
            // coin whose rect MOVES during the run is caught at the tick it
            // moves rather than at the one tick the player happens to pass it.
            for (size_t i = 0; i < solver::g_coins.size(); ++i) {
                const bool watch = g_cfg.coinWatch && (g_tick % 1200) == 0;
                if (solver::g_coinLiveSaid[i] && !watch) continue;
                if (!watch && std::abs(px - solver::g_coins[i].x) > 80.f) continue;
                solver::g_coinLiveSaid[i] = 1;
                if (auto* arr = this->m_objects)
                    for (auto* o : CCArrayExt<GameObject*>(arr)) {
                        if (!o || o->m_uniqueID != solver::g_coins[i].uid) continue;
                        const auto lp = o->getPosition();
                        const auto lr = o->getObjectRect();
                        char lb[208];
                        snprintf(lb, sizeof(lb),
                                 "coinlive: t=%lld uid=%d load=(%.1f,%.1f) pos=(%.2f,%.2f) "
                                 "rect=(%.2f,%.2f) %.1fx%.1f disabled=%d",
                                 (long long)g_tick, solver::g_coins[i].uid,
                                 solver::g_coins[i].x, solver::g_coins[i].y, lp.x, lp.y,
                                 lr.origin.x + lr.size.width * 0.5f,
                                 lr.origin.y + lr.size.height * 0.5f,
                                 lr.size.width, lr.size.height,
                                 o->m_isGroupDisabled ? 1 : 0);
                        writeResult(lb);
                        break;
                    }
            }
            for (size_t i = 0; i < solver::g_coins.size()
                 && normalMotion && i < solver::g_coinPickupTick.size(); ++i) {
                if (solver::g_coinPickupTick[i] >= 0) continue;
                if (std::abs(px - solver::g_coins[i].x) <= solver::COIN_RADIUS
                    && std::abs(py - solver::g_coins[i].y) <= solver::COIN_RADIUS)
                    solver::g_coinPickupTick[i] = g_tick;
            }
        }
        // cfg `coinroute`: an attempt that has passed a coin GD never credited cannot become an
        // all-coins clear, so it ends here -- reported first, then killed, the same order as
        // `stopat` above so the death: line that follows can be told apart. The bound is the
        // search's own miss prune (dp cli.hpp): past the coin's far edge by the player's half,
        // where no overlap is possible any more. 15 is the largest half any mode has, so a smaller
        // body is let go a little late, never early. Not while a job is in flight: the level is
        // held still then, and a checkpoint flight's deaths mean something else (ckOnDeath).
        if (g_started && !g_sessionOver && g_cfg.coinRoute && m_player1 && !m_player1->m_isDead
            && !solver::g_coinMissFired && !solver::g_hasRotGameplay
            && !dpsolve::g_recordAttempt && !dpsolve::g_running.load()
            // A section search judges its own coins (cfg seccoins), in practice mode where GD
            // credits none -- and the replay that takes a rung to its head (secsolve::g_on from
            // the handoff to the rung's end) is not an attempt of the loop's either: SubZero 4002,
            // the prefix of a goal-claiming plan without the first coin was cut at x=1,822 on every
            // try and never reached the head (1,200 attempts in 30 minutes).
            && !secsolve::g_active && !secsolve::g_on
            // ...and only for a plan that CLAIMS every coin. The loop flies its
            // partial plans too -- that is how it learns where the route goes --
            // and a partial plan misses coins by construction, so ending those
            // attempts at the first coin stops the loop from ever seeing past
            // it. Measured on lv21 (2026-09-20): the flown plan's own model
            // trace was 38 px below the third coin, i.e. the model never claimed
            // it, and the attempt was still cut there every round.
            && dpsolve::g_planClaimsGoal) {
            bool completed = false;
            if (auto* pl = PlayLayer::get()) completed = pl->m_hasCompletedLevel;
            const float px = m_player1->getPositionX();
            for (size_t i = 0; !completed && i < solver::g_coins.size()
                               && i < solver::g_coinGdTick.size(); ++i) {
                if (solver::g_coinGdTick[i] >= 0) continue;
                const auto& c = solver::g_coins[i];
                if (px <= c.x + c.hw + 15.f) continue;
                // cfg coinmissrev: not a coin the search itself calls missed when passed --
                // something past it turns the player round (SubZero 4002's third coin, dropped
                // 600 px at x=31,899 and taken on the way back, was cut here on the way out at
                // t=20,206 every time the plan claimed it).
                if (g_cfg.coinMissRev
                    && std::find(solver::g_coinNoMiss.begin(), solver::g_coinNoMiss.end(), c.uid)
                           != solver::g_coinNoMiss.end())
                    continue;
                // cfg coinmissmove: a Move that carries this coin is still ahead, so where it
                // will be is not settled yet (solver::g_coinMoveX). Past the Move, a coin the
                // search does not call missed (something beyond turns the player round) still
                // has the way back, which x alone cannot bound, so it is left to the posterior
                // signal (coinmisspost) instead. 4002's third coin drops at x=31,899 and the
                // player turns at about 31,953: "until the Move" alone cut it at 31,900.
                if (g_cfg.coinMissMove && i < solver::g_coinMoveX.size()
                    && solver::g_coinMoveX[i] > -1e8f
                    && (px <= solver::g_coinMoveX[i]
                        || std::find(solver::g_coinNoMiss.begin(), solver::g_coinNoMiss.end(),
                                     c.uid) != solver::g_coinNoMiss.end()))
                    continue;
                char mb[192];
                snprintf(mb, sizeof(mb), "coinmiss: t=%lld x=%.2f y=%.2f coin=%zu uid=%d at "
                         "(%.1f,%.1f)", (long long)g_tick, px, m_player1->getPositionY(), i,
                         c.uid, c.x, c.y);
                writeResult(mb);
                solver::g_coinMissFired = true;   // once per attempt, however GD answers
                solver::g_coinMissIdx = (int)i;
                if (g_cfg.dpSolve) dpsolve::g_coinMissPending = true;   // only onDeath reads it
                if (auto* pl = PlayLayer::get()) pl->destroyPlayer(m_player1, m_player1);
                break;
            }
        }
        // ...and a coin that only a counting tap's gate switches on, once GD has
        // passed the Stop that shuts the tap's window with the count still short,
        // or -- the count made -- the coin's far edge on that same channel's run
        // without the coin (solver::g_coinGates, handed over by dp: lv22's ship
        // glides 66 px under its third coin there). The search prunes the same
        // branch (dp cli.hpp); this is its twin on the flown attempt. Unlike the
        // miss above it holds on a level with rotated gameplay: the Stop fires
        // once, so turning round does not open the window again. "Passed" is
        // read the way GD's own queue walk reads it -- the stop's channel active
        // (layer+0x33c, as the dump's rotch column reads it) and the player's
        // world coordinate past the point in that channel's direction.
        if (g_started && !g_sessionOver && g_cfg.coinRoute && m_player1 && !m_player1->m_isDead
            && !solver::g_coinMissFired && !solver::g_coinGates.empty()
            && !dpsolve::g_recordAttempt && !dpsolve::g_running.load() && !secsolve::g_active
            && !secsolve::g_on && dpsolve::g_planClaimsGoal) {
            const int ch =
                *reinterpret_cast<int const*>(reinterpret_cast<char const*>(this)
                                               + gdoff::kLayerCurrentChannel);
            const double wx = m_player1->getPositionX(), wy = m_player1->getPositionY();
            for (const solver::CoinGate& g : solver::g_coinGates) {
                if (ch != g.chan) continue;
                auto passed = [&](double px, double py) {
                    return g.dir == 1 ? wy >= py
                         : g.dir == 2 ? wy <= py
                         : g.dir == 3 ? wx <= px
                                      : wx >= px;
                };
                bool got = false;
                for (size_t i = 0; i < solver::g_coins.size() && i < solver::g_coinGdTick.size(); ++i)
                    if (solver::g_coins[i].uid == g.uid && solver::g_coinGdTick[i] >= 0) got = true;
                if (got) continue;
                const auto it = solver::g_itemCounts.find(g.item);
                const int have = it == solver::g_itemCounts.end() ? 0 : it->second;
                // the shut point with the count short, or -- the gate having
                // opened -- the coin's far edge on the same run without it
                const bool shortAtShut = passed(g.x, g.y) && have < g.need;
                const bool pastCoin = g.miss && passed(g.mx, g.my);
                if (!shortAtShut && !pastCoin) continue;
                char mb[256];
                snprintf(mb, sizeof(mb), "coingatemiss: t=%lld x=%.2f y=%.2f coin uid=%d: %s"
                         " (item %d is %d of %d; chan %d, shut (%.0f,%.0f), far edge (%.0f,%.0f))",
                         (long long)g_tick, wx, wy, g.uid,
                         shortAtShut ? "window shut short" : "passed the coin",
                         g.item, have, g.need, g.chan, g.x, g.y, g.mx, g.my);
                writeResult(mb);
                solver::g_coinMissFired = true;   // once per attempt, as the miss above
                if (g_cfg.dpSolve) dpsolve::g_coinMissPending = true;
                if (auto* pl = PlayLayer::get()) pl->destroyPlayer(m_player1, m_player1);
                break;
            }
        }
        // End-of-tick record of what a re-anchor would need (Stage C). Same instant as the dump
        // row below and for the same reason -- the state has to be settled -- but deliberately
        // NOT behind `notrace`: the dump is a diagnostic, this is what the next iteration of the
        // repair loop resumes the search from.
        // Not inside a section search (see tEff): its rows would land on the ticks just past the
        // section head, one per step of every node, growing the buffer by tens of thousands of
        // rows per search. The attempt the rung starts afterwards clears them (onAttemptStart),
        // so the ladder never read them, but every step paid for them.
        if (g_started && !g_sessionOver && g_cfg.dpSolve && m_player1 && !secsolve::g_active)
            anchors::record(this, g_tick);
        // cfg `areatrace`: the listed objects' positions at this same instant (see g_areaT0).
        // The pointers are looked up again whenever the object array changes (a new level in
        // a one-session run) or the window starts.
        if (g_started && !g_sessionOver && !g_areaUids.empty() && this->m_objects
            && g_tick >= g_areaT0 && g_tick <= g_areaT1) {
            if (g_areaArr != this->m_objects || g_tick == g_areaT0
                || g_areaObjs.size() != g_areaUids.size()) {
                g_areaArr = this->m_objects;
                g_areaObjs.assign(g_areaUids.size(), nullptr);
                for (unsigned i = 0; i < g_areaArr->count(); ++i) {
                    auto* o = static_cast<GameObject*>(g_areaArr->objectAtIndex(i));
                    if (!o) continue;
                    for (size_t k = 0; k < g_areaUids.size(); ++k)
                        if (o->m_uniqueID == g_areaUids[k]) g_areaObjs[k] = o;
                }
            }
            for (size_t k = 0; k < g_areaObjs.size(); ++k) {
                if (!g_areaObjs[k]) continue;
                // The rect's centre, which is what grouptrace records and what collisions
                // use. getPosition() alone does not move under an Area Move: the area
                // effect is applied as an offset the position does not include (measured on
                // lv22 uid 6269: position fixed at 16155 while the rect went to 16369).
                const auto r = g_areaObjs[k]->getObjectRect();
                const auto pos = g_areaObjs[k]->getPosition();
                char ab[160];
                snprintf(ab, sizeof(ab), "area: t=%lld uid=%d cx=%.4f cy=%.4f px=%.4f py=%.4f",
                         (long long)g_tick, g_areaUids[k],
                         r.origin.x + r.size.width * 0.5, r.origin.y + r.size.height * 0.5,
                         pos.x, pos.y);
                writeResult(ab);
            }
        }
        // cfg `slopetrace`: the slope-ride bytes at this same instant (see g_slopeT0).
        if (g_started && !g_sessionOver) anchors::slopeTrace(this, g_tick);
        // ...and, while a checkpoint flight is in the air (cfg `dpcheck`), the tick its inputs run
        // out on. Here rather than at the frame boundary: one frame of the fast loop is thousands
        // of ticks, and an inputless flight left to drift that far either dies a death that means
        // nothing or walks into the stall guard (repair.hpp ckTick).
        if (g_started && !g_sessionOver && g_cfg.dpSolve && g_cfg.dpCheck)
            dpsolve::ckTick(g_tick);
        // ...and the seek bar's own, much smaller record: one x per tick, which is what turns
        // "five seconds earlier" into a place on the bar. Not the anchors buffer, which is
        // twenty-odd fields wide and only exists during a solve.
        //
        // NOT AFTER THE SESSION ENDS. g_tick keeps counting for as long as the level is left
        // standing -- the results screen is not a stopped game, it is a game still being updated
        // once a frame -- and the bar's axis is the deepest tick it has seen. Reported 2026-08-28:
        // sit on the clear screen and the axis grows without limit, so the whole run's marks creep
        // leftwards and the playhead slides back off the end of a run that is over. The run ended
        // at the tick it ended at; that is where the bar stops.
        //
        // NOR INSIDE A SECTION SEARCH, for the same reason: a restore does not rewind g_tick (see
        // tEff above), so it counts every step of every node -- tens of thousands per search --
        // and the bar's axis grew by that much each time a rung ran, crushing the run's marks
        // to the left, while the search's scattered x values landed on ticks past the level.
        if (g_started && !g_sessionOver && m_player1 && !secsolve::g_active) {
            itermap::trackX(g_tick, m_player1->getPositionX());
            seekcp::noteX(g_tick, m_player1->getPositionX(), m_player1->getPositionY());   // cfg seekcp: head run kept, restored checked
        }
        // GD's max-gameplay-y bound (layer+0x36a8, updateMaxGameplayY), logged
        // on change. On dynamic-height levels the bound moves with the world,
        // and whether it does -- and when -- decides how --maxplayy has to be
        // carried (one value vs a per-tick track like --bandtrack). A handful
        // of lines per attempt; not gated on dpsolve so a plain replay shows it.
        if (g_started && !g_sessionOver) {
            const float mp = *reinterpret_cast<float const*>(
                reinterpret_cast<char const*>(this) + gdoff::kLayerMaxGameplayY);
            static float mpLast = -1.f;
            if (std::fabs(mp - mpLast) > 0.5f) {
                mpLast = mp;
                writeResult("maxplayy: t=" + std::to_string(g_tick)
                            + " v=" + std::to_string(mp));
            }
        }
        // End-of-tick state dump (after the player's physics is settled). Not inside a section
        // search: one row per step of every node, on ticks that are not the level's (see tEff).
        if (g_started && !g_sessionOver && !secsolve::g_active
            && !g_cfg.noTrace && g_dump.is_open() && m_player1) {
            auto* p = m_player1;
            auto pos = p->getPosition();
            g_dump << g_frame << ',' << g_attempt << ',' << g_tick
                   << ',' << pos.x << ',' << pos.y
                   << ',' << p->m_yVelocity << ',' << p->getRotation()
                   << ',' << modeStr(p) << ',' << p->m_isUpsideDown
                   << ',' << p->m_isOnGround << ',' << p->m_isOnGround2
                   << ',' << p->m_isDead << ',' << p->m_playerSpeed
                   << ',' << p->m_gravityMod << ',' << p->m_platformerXVelocity
                   // vsize: the size portal (1.0=normal 0.6=mini). Needed for re-anchoring
                   << ',' << p->m_vehicleSize
                   // gy1/gy2: real positions of the ground/ceiling layers. They move, and
                   // they act as a standing surface even with no object there
                   // (getGroundY() returns 0, so emit the real positions)
                   << ',' << (this->m_groundLayer
                                  ? this->m_groundLayer->getPositionY() : -1.f)
                   << ',' << (this->m_groundLayer2
                                  ? this->m_groundLayer2->getPositionY() : -1.f)
                   // dual/p2*: the second body in dual (while not in dual, p2 is
                   // off-screen, so read it together with the dual flag)
                   << ',' << (m_gameState.m_isDualMode ? 1 : 0)
                   << ',' << (m_player2 ? m_player2->getPositionY() : -1.f)
                   << ',' << (m_player2 ? m_player2->m_yVelocity : 0.f)
                   << ',' << (m_player2 ? (int)m_player2->m_isUpsideDown : 0)
                   << ',' << (m_player2 ? (int)m_player2->m_isOnGround : 0)
                   << ',' << (m_player2 ? (int)m_player2->m_isDead : 0)
                   // pmin/pmax: GD's own flight band (getMinPortalY / getMaxPortalY).
                   //
                   // Emitted to remove the last x dependency of re-anchoring. When leveldp
                   // resumes with --start, it rebuilds the band from "the last mode portal
                   // before x0". That knows nothing about which lane was taken, so it also
                   // picks up portals that were never passed through. Measured (lv20
                   // 2026-08-09): the wave passes under the portal at (4445,303)
                   // (y=114..160; the box is y[253,353]), yet the model rewrote the band
                   // to [150,450], flew 34px too high from then on, and stopped dying
                   // where GD kills. fixup was grinding, buying back 4 ticks at a time.
                   // Emitting GD's values directly removes the guessing.
                   << ',' << this->getMinPortalY()
                   << ',' << this->getMaxPortalY()
                   // snapuid/snapdist: stair-snap state (the uid of m_objectSnappedTo and
                   // m_snapDistance). The last x that re-anchoring cannot carry.
                   //
                   // leveldp's --start resumes with snapObj=null, so at the first stair
                   // right after the anchor the pattern match fails (the gate needs "the
                   // object last stood on") and one snap is dropped. From then on that run
                   // trails GD by 1px forever.
                   // Measured (2026-08-10, the same 400-tick anchors as quick_regress,
                   // lv1-20): 64 sections split with dy=dvy=0 and dx=-1.0000 exactly, all
                   // on mode 0 surfaces. Every one is before the tick where the same
                   // level's "anchorless replay" splits = an artifact of the anchor, not
                   // physics. -1 = standing on nothing.
                   << ',' << (p->m_objectSnappedTo
                                  ? p->m_objectSnappedTo->m_uniqueID : -1)
                   << ',' << p->m_snapDistance
                   // camscale: the divisor [this+0x1a8] of getMaxPortalY (win 0x213770).
                   // The real thing is `getMinPortalY() + [this+0x2ec] / [this+0x1a8]`,
                   // where [+0x2ec] is the mode's band height. That is, the height of the
                   // invisible ceiling is divided by the camera scale. This is why, in
                   // lv22's swing section, pmax-pmin moves smoothly 324.000 -> 405.000
                   // (= height 300 divided by 0.92593 and 0.74074). Read by raw offset
                   // because bindings has no name for it
                   << ',' << *reinterpret_cast<const float*>(
                                 reinterpret_cast<const char*>(this) + gdoff::kLayerCameraZoom)
                   // gframe: current rotation (0/1/2/3 = 0/90/180/270).
                   // Needed for re-anchoring. Starting with --start inside a rotated
                   // gameplay section, the model has no way to know which orientation it
                   // is running in (the rotate triggers count as already passed), resumes
                   // in frame 0 and falls off within 1 tick. lv22's anchor at t=11,340
                   // "followed for 1 tick" because of this.
                   << ',' << (int)g_gameFrame
                   // ctrlOff: m_controlsDisabled. When the Options trigger (id 2899) sets
                   // it, buttons are ignored entirely (no press, and no cube held
                   // re-jump either). lv22 has 4 such sections of 764/354/425/378 ticks,
                   // where only the model was jumping (fixcensus's
                   // m0/g1/gdg1/sp0.9/air/in1 family = edvy -11.180).
                   // Needed in the dump because a section anchor can start inside such a
                   // window (2899 is a one-shot on x crossing, so with --start inside the
                   // window the model has no way to know).
                   << ',' << (p->m_controlsDisabled ? 1 : 0)
                   // camx/camy: the camera (= the negated position of m_objectLayer).
                   // Emitted to measure the trigger firing gate. The 10 firings of id 2899
                   // cannot be explained by x crossing alone (there are passes over the
                   // same x that do not fire), nor by distance to the player's y
                   // (no fire at delta=1,236, fire at delta=1,324). GD wakes triggers when
                   // they enter the screen, so the remaining variable is the camera
                   // itself. Emit the raw values and verify the rule afterwards.
                   << ',' << (this->m_objectLayer
                                  ? -this->m_objectLayer->getPositionX() : 0.f)
                   << ',' << (this->m_objectLayer
                                  ? -this->m_objectLayer->getPositionY() : 0.f)
                   // p2ground2: the SECOND body's second contact flag, the partner of
                   // p2ground exactly as onGround2 is of onGround. GD's ground flag is
                   // sticky for the flying modes and the pair plus a still velocity is what
                   // actually says "resting" (see grounded_of / groundedOf). p1 has had both
                   // since the beginning; p2 had only one, so an anchor built from this dump
                   // could not apply the same rule to the second body.
                   // Appended at the END of the row on purpose -- readers index the earlier
                   // columns by position.
                   << ',' << (m_player2 ? (int)m_player2->m_isOnGround2 : 0)
                   // p2mode/p2vsize: THE SECOND BODY'S GAME MODE AND SIZE. Neither
                   // was emitted anywhere -- not here, not in leveldp's trace, not in
                   // `--start`. So the loop compared p2 on y and vy alone and called
                   // that agreement.
                   // [correction 2026-08-28] "while the model does carry mode2/mini2
                   // (swapHalves swaps them)" stood here and was HALF WRONG: State
                   // had mode2, but size was still on stepBoth's shared list, so the
                   // model had no second-body size to be told about. Both ends were
                   // built afterwards -- State::mini2, and the 26th/27th --start
                   // fields -- so the sentence is true now and was not then.
                   //
                   // That blind spot fits the corpus' largest grind exactly. lv16
                   // t=13,017 is hit 26 times: GD kills p2 twelve ticks before the
                   // model does and the fixup pass writes nothing, because "apart
                   // there by 0.00/0.00" is all it can see. GD names the killer --
                   // a 2.6x4.8 spike (uid 6563) at dy=-10.30 -- which needs an
                   // effective player half above 7.90 to reach: 9 (mini) or 15 does,
                   // the wave's hazard rect(5,3) does not. A mode the model has
                   // wrong is the first suspect, and lv16's dual span carries lone
                   // single-height portals (a ball at 19,219 / a ship at 20,499),
                   // which is precisely how the two halves come to differ.
                   //
                   // Emitted to TEST that, not on the strength of it. Appended at
                   // the end of the row like p2ground2: readers index by position.
                   //
                   // Measured as soon as the column existed: on lv16's solution the
                   // two bodies are in the same mode and size for all 5,557 dual
                   // ticks, so the anchor's assumption holds for the OFFICIAL corpus.
                   // It does not hold in general -- differing modes and sizes between
                   // the halves are ordinary in custom levels, which this solver
                   // already clears cold. So the assumption is a corpus artefact, and
                   // the column is what will show it the moment a custom level needs
                   // it.
                   << ',' << (m_player2 ? modeStr(m_player2) : "-")
                   << ',' << (m_player2 ? m_player2->m_vehicleSize : 0.f)
                   // p2x: and the same question one level down. The model gives the
                   // pair a SINGLE x (State::xAbs; swapHalves does not swap it), so
                   // two bodies at different x would not be representable at all
                   // rather than merely mis-anchored. Whether GD can produce that is
                   // not known, and guessing either way is worse than a column that
                   // answers it.
                   << ',' << (m_player2 ? m_player2->getPositionX() : -1.f)
                   // rotch/rotidx/rotrev: GD's OWN rotation-queue state, so a 2900
                   // that the model fires and GD does not can be asked why instead
                   // of guessed at. checkSpawnObjects walks one channel from a
                   // per-channel cursor: the active channel is layer+0x33c (int),
                   // the cursor layer+0x348 and the reverse flag layer+0x388, both
                   // unordered_maps keyed by channel. The bindings name none of the
                   // three, so they are read by offset.
                   //
                   // find() only. GD's own access is operator[], which INSERTS a
                   // missing key; a print that did the same would change the state
                   // it is printing. A missing key is -1, never 0, because 0 is a
                   // real cursor value.
                   //
                   // The size check is the layout check: MSVC's unordered_map is
                   // 0x40 bytes, which is exactly the distance between the two.
                   << ',' << [this]() {
                          // gd::unordered_map is std::unordered_map on Windows and GD's
                          // GNU-STL layout on Android (0x38 bytes there).
                          static_assert(sizeof(gd::unordered_map<int, int>)
                                        == gdoff::kLayerSpawnChannel1 - gdoff::kLayerSpawnChannel0);
                          static_assert(sizeof(gd::unordered_map<int, bool>)
                                        == gdoff::kLayerSpawnChannel1 - gdoff::kLayerSpawnChannel0);
                          auto const* base = reinterpret_cast<char const*>(this);
                          const int ch =
                              *reinterpret_cast<int const*>(base + gdoff::kLayerCurrentChannel);
                          auto const& idx = *reinterpret_cast<gd::unordered_map<int, int> const*>(
                              base + gdoff::kLayerSpawnChannel0);
                          auto const& rev = *reinterpret_cast<gd::unordered_map<int, bool> const*>(
                              base + gdoff::kLayerSpawnChannel1);
                          auto const i = idx.find(ch);
                          auto const r = rev.find(ch);
                          return std::to_string(ch) + ','
                              + std::to_string(i == idx.end() ? -1 : i->second) + ','
                              + std::to_string(r == rev.end() ? -1 : (int)r->second);
                      }()
                   // firedw: for each uid in cfg dpwatchfired, the object's +0x28e byte --
                   // the flag checkSpawnObjects (0x21aad8) tests before it calls
                   // triggerObject, while advancing the cursor either way. A 2900 whose
                   // cursor moves but whose channel does not is explained by this byte or
                   // by nothing on that path, so it is asked rather than inferred.
                   // Always present so the header never depends on when the cfg is read:
                   // "-" with no uids, "uid:v|uid:v" otherwise, -1 for a uid not found.
                   // The uid -> object lookup walks m_objects once per attempt. Read only.
                   << ',' << [this]() -> std::string {
                          if (g_cfg.dpWatchFired.empty()) return "-";
                          static void const* layer = nullptr;
                          static long long attempt = -1;
                          static std::vector<std::pair<int, GameObject*>> objs;
                          if (layer != this || attempt != (long long)g_attempt
                              || objs.size() != g_cfg.dpWatchFired.size()) {
                              layer = this;
                              attempt = (long long)g_attempt;
                              objs.clear();
                              for (int uid : g_cfg.dpWatchFired) {
                                  GameObject* found = nullptr;
                                  if (m_objects)
                                      for (auto* o : CCArrayExt<GameObject*>(m_objects))
                                          if (o && o->m_uniqueID == uid) { found = o; break; }
                                  objs.push_back({uid, found});
                              }
                          }
                          std::string s;
                          for (auto const& [uid, o] : objs) {
                              if (!s.empty()) s += '|';
                              s += std::to_string(uid) + ':' + std::to_string(
                                  o ? (int)*(reinterpret_cast<uint8_t const*>(o) + gdoff::kObjGroupDisabled) : -1);
                          }
                          return s;
                      }()
                   // bandst/bandmode/camoffy: WHICH BAND pmin/pmax ARE. getMinPortalY
                   // (0x213690) and getMaxPortalY (0x213770) take branch A when
                   // [this+0x23c] != 0 and [this+0x2a0] == 0 -- the band is then read off
                   // the ground layers, (y -+ 1) / zoom + camY, and moves with the camera --
                   // and otherwise branch B, the portal's centre [+0x308] -+ 0.5 * H / zoom
                   // + [+0x1b4], where +0x1b4 is the game state's +0xc (tweened by
                   // 0x236010). checkCollisions clamps the flying modes to whichever it
                   // returns, gated only on the teleport byte (player+0x560), so whether a
                   // recorded band is a wall for another plan depends on the branch and
                   // not on whether the value sits on the 30 grid: zoom alone takes a
                   // portal band off the grid (SubZero 4003's spider band 1354.5/1651.5 is
                   // 270 / 0.9091 high). Read by raw offset; bindings name none of the three.
                   << ',' << *reinterpret_cast<const float*>(
                                 reinterpret_cast<const char*>(this) + gdoff::kLayerUnkPoint17Y)
                   << ',' << (int)*(reinterpret_cast<const uint8_t*>(this) + gdoff::kLayerUnkBool1)
                   << ',' << *reinterpret_cast<const float*>(
                                 reinterpret_cast<const char*>(this) + gdoff::kLayerCameraOffsetY)
                   // freemode/bandforce: the two layer bytes checkCollisions reads before it
                   // clamps at all (0x2139b8). [+0x311] set = no band clamp for any mode;
                   // it is the Free Mode byte (property 111) that playerWillSwitchMode
                   // (0x212ef0) copies from each mode portal it takes. [+0x422] set = clamp
                   // even outside the six flying modes. Read by raw offset.
                   << ',' << (int)*(reinterpret_cast<const uint8_t*>(this) + gdoff::kLayerFreeMode)
                   << ',' << (int)*(reinterpret_cast<const uint8_t*>(this) + gdoff::kLayerDualMode)
                   // p2rot: the SECOND body's sprite angle. `rot` has only ever been p1's, and a
                   // turned object's test reads the body's own angle -- for a mirrored UFO pair,
                   // the negative of p1's -- so the second body's portal and hazard timing could
                   // not be checked against anything. Appended at the END like every column
                   // since p2ground2: readers index the earlier ones by position.
                   << ',' << (m_player2 ? m_player2->getRotation() : 0.f)
                   // p2ctrlOff: the SECOND body's m_controlsDisabled. ctrlOff is player 1's, and
                   // the Options trigger switches the two players separately
                   // (processOptionsTrigger 0x223d50: +0x754 player 1, +0x758 player 2), so a
                   // window that stops only p1 left p2 jumping in GD while the model held both.
                   << ',' << ((m_player2 && m_player2->m_controlsDisabled) ? 1 : 0)
                   << '\n';
        }
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        ev("processQueuedButtons", dt, clearInputQueue);
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }

    // Record the fact that a switch (touch trigger) was touched (cfg `trigtrace=1`).
    // Which trigger was stepped on at which tick makes run lineages distinguishable
    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        // touchfire (sweep.hpp, --rectouch): GD fires a touch trigger here -- collisionCheckObjects'
        // object types 0x14 / 0x2d; 0x217f50 stores (uid, player) in the effect manager's triggered set
        // and calls triggerObject only when the pair is not there yet -- so the first call per uid in an
        // attempt is the fire. Read-only: the uid and tick are taken before the call. Unconditional like
        // the recorders in hooks_player.cpp, so carrying nothing never depends on a flag.
        if (object) {
            ++touchfire::g_calls;
            if (player == m_player1) {
                auto& m = touchfire::g_first;
                if (m.find(object->m_uniqueID) == m.end()) m[object->m_uniqueID] = (int)g_tick;
            } else {
                ++touchfire::g_p2;
            }
        }
        if (g_trigTrace && g_started && !g_sessionOver && object
            && player == m_player1) {
            static int s_logs = 0;
            if (++s_logs <= 60) {
                auto p = object->getPosition();
                writeResult("trigger: id=" + std::to_string(object->m_objectID)
                    + " x=" + std::to_string((int)p.x)
                    + " y=" + std::to_string((int)p.y)
                    + " tick=" + std::to_string((long long)g_tick)
                    + " attempt=" + std::to_string(g_attempt));
            }
        }
        // When and with which rects a contact trigger such as a speed portal fired
        // (cfg `hitboxtrace=1`, `ptt:` line). Two cases where the firing tick was 1 tick
        // off from the model (lv18 t=14,613 / lv19 t=20,310) could not be fitted by a
        // single convention using the objrects boxes, so have GD dump both rects it uses
        // on the spot
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && object
            && player == m_player1) {
            static int lines = 0;
            if (++lines <= 2000) {
                const auto orct = object->getObjectRect();
                const auto prct = player->getObjectRect();
                char b[256];
                snprintf(b, sizeof(b),
                         "ptt: t=%lld uid=%d id=%d orect=(%.3f,%.3f,%.3f,%.3f) "
                         "prect=(%.3f,%.3f,%.3f,%.3f)",
                         (long long)g_tick, object->m_uniqueID,
                         object->m_objectID,
                         orct.origin.x, orct.origin.y,
                         orct.size.width, orct.size.height,
                         prct.origin.x, prct.origin.y,
                         prct.size.width, prct.size.height);
                writeResult(b);
            }
        }
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Replay mode: block real input other than injections (still recorded).
        // !g_sessionOver is required: without it the block persists after the session
        // ends and manual play stops working
        // ...except a person's, once a takeover has given them the input (mod/takeover.hpp).
        if (g_cfg.blockInput && g_started && !g_sessionOver && !g_injecting
            && !takeover::personDriving()) {
            ev("handleButton_BLOCKED", down, button, isPlayer1);
            return;
        }
        ev("handleButton", down, button, isPlayer1);
        // The person's presses, at the tick GD applies them: g_tick is the substep about to run
        // (or, between frames, the next one), which is the step an injection of the same press
        // is fed at.
        if (!g_injecting && takeover::personDriving()) takeover::noteInput(down, button, isPlayer1);
        // Remember the raw press for the slope trace. Jump only (button 1) and
        // player 1 only: the second body follows the same input in these levels,
        // and a per-half press would need the dual input split, which no probe
        // here varies.
        if (button == 1 && isPlayer1) g_btnDown = down;
        // cfg batchlog (print only): the tick each press and release reaches GD at
        if (itermap::g_batchFrom >= 0 && g_tick >= itermap::g_batchFrom && g_tick <= itermap::g_batchTo
            && g_started && !g_sessionOver)
            writeResult("batch: button t=" + std::to_string((long long)g_tick) + " down=" + std::to_string(down ? 1 : 0)
                        + " btn=" + std::to_string(button) + " p1=" + std::to_string(isPlayer1 ? 1 : 0)
                        + " run=" + (seekcp::g_from < 0 ? std::string("head") : "cp@" + std::to_string(seekcp::g_from)));
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        bool isP1 = (player == m_player1);
        // Do not skip checkCollisions wholesale here: GD's collision state would go stale,
        // and the first test right after releasing noclip misdetects and dies instantly.
        // Pass-through is done via the no-op on the collidedWithObject side
        if (isP1) ev("checkCollisions", dt);
        return GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
    }

    // A section solve makes checkpoints by the tens of thousands (markCheckpoint) and stores them
    // again and again (secRestoreFrom). GD's storeCheckpoint evicts the oldest past 50 and
    // removeCheckpoint drops the last, and both take the checkpoint's physical object out of its
    // section with removeObjectFromSection -- which never marks the object as out (the
    // decompilation: it swap-removes by the object's own indices, +0x278/+0x27c/+0x270, and only
    // tests +0x278 >= 0 on entry). A checkpoint the search still holds, evicted once and stored
    // again, is evicted or removed a second time, and the second removal takes some OTHER object's
    // slot and the section's count down with it. After one rung the section table was broken for
    // good: in a worker with the visibility pass on (skiprender=0), updateVisibility faulted on
    // every frame from the reset after the rung (libcocos2d+0x2ba3, reading 0x10), the replay drew
    // nothing but the player, and GD's own retry crashed at the same instruction in
    // resetLevelVariables (user report on the panel, 2026-09-25). While a section solve is on,
    // a removed object is marked out, so a second removal is GD's own no-op.
    void removeObjectFromSection(GameObject* object) {
        GJBaseGameLayer::removeObjectFromSection(object);
        static_assert(offsetof(GameObject, m_outerSectionIndex) == gdoff::kObjOuterSectionIndex,
                      "the index removeObjectFromSection tests on entry");
        if (secsolve::g_on && object) object->m_outerSectionIndex = -1;
    }

    // cfg secbound (see secsolve::g_boundOn): inside a search, an object outside the level's box
    // keeps the section it had. Only the re-bucketing is skipped; the object's position is
    // whatever the game made it.
    void updateObjectSection(GameObject* object) {
        if (secsolve::g_active && object
            && !(std::isfinite(object->m_positionX) && std::isfinite(object->m_positionY)
                 && std::fabs(object->m_positionX) < secsolve::kPosLimit
                 && std::fabs(object->m_positionY) < secsolve::kPosLimit)) {
            if (secsolve::g_nonFinite++ == 0) {
                secsolve::g_nonFiniteUid = object->m_uniqueID;
                secsolve::g_nonFiniteId = object->m_objectID;
                secsolve::g_nonFiniteX = object->m_positionX;
                secsolve::g_nonFiniteY = object->m_positionY;
#ifdef GEODE_IS_WINDOWS
                // who asked for the re-bucketing (the caller in the game is the frame after this one)
                postmortem::emit("secnan: an object at a non-finite position handed to re-bucketing");
                postmortem::writeBacktrace("secnan:");
#endif
            }
            return;
        }
        if (secsolve::g_active && secsolve::g_boundOn && object
            && (object->m_positionX < secsolve::g_boxX0 || object->m_positionX > secsolve::g_boxX1
                || object->m_positionY < secsolve::g_boxY0
                || object->m_positionY > secsolve::g_boxY1)) {
            ++secsolve::g_boundSkips;
            return;
        }
        GJBaseGameLayer::updateObjectSection(object);
    }

    // cfg secshaderskip (see secsolve::g_shaderSkip): inside a search, the shader layer stays as
    // it is. The first real reset after the search puts it back through this same call.
    void updateShaderLayer(float dt) {
        if (secsolve::g_active && secsolve::g_shaderSkip) {
            ++secsolve::g_shaderSkips;
            return;
        }
        GJBaseGameLayer::updateShaderLayer(dt);
    }
};
