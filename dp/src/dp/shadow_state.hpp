// S2 stage 2: the self-running shadow's state (selfshadow.hpp) as one fixed-size, trivially copyable block -- ShadowLite -- that a dp State can carry, and the
// round trip between it and a SelfShadow. Nothing here changes the shadow's code: unpack() builds the SelfShadow a step runs on, pack() reads back what the step
// left, and a step run between the two is the same step as without them (tools/self_test.cpp ST_ROUNDTRIP=1 checks that, window by window).
//
// What is carried:
//   - the player image's words that a step can carry over to the next one: every word that changed in the 115 widecap8 windows outside self_test's ignored
//     ranges, every word the shadow reads before it writes it in a step unless no window ever changed it and the shadow never writes it, and the two object
//     slots the shadow keeps as uid + 1 (+0x590 the dash ring, +0xa88 the last gravity object), and +0x6f8 (the touched-ramp map's size: the pass reads whether it
//     is zero; the closed loop found it). 171 words, 684 bytes (S2 stage 0, out_track2\footprint_words.txt).
//     Every other word comes from a template image (the level's: a seed image or collide_tmpl.gen.hpp's), so it must be one the shadow never reads before it
//     writes it, or one that never changes;
//   - the sets: gate (the last pass's overlaps), usedRings (+0x850), the touching rings (RingTouches arr, in first-contact order, and now), the holding bytes;
//   - the once-per-attempt memory `ever` (FUN_14031ef50's map, which nothing sweeps), as a rule plus a short list (AUD-670: an object that leaves the
//     neighbourhood and comes back must still be in it):
//       ever = { static seedable objects whose rect's right edge is left of everW }   (seedMemory's rule at a seed; off when a run starts at the level start)
//            u { the seed's movers that were behind the player }                     (one list per seed, shared by every state of the run: seedMovers)
//            u ever[0..nEver)                                                         (the objects this state's own passes added)
//     An entry of the list is dropped only when it is a static object whose right edge is more than kSlPruneMargin behind the player's rect, and pruneW then
//     remembers the largest such edge: a later step whose player rect reaches back past pruneW could meet a dropped object again, and marks the state
//     ambiguous (flag) instead of answering silently. A mover is never dropped (it can come back);
//   - the objects a ring jump marked activated (GObj::activated, the world's +0x3d2), the same way: a list, dropped by the same rule;
//   - the dash's sprite angle and clock, the pending speed, the frame step counters, the generator, and (for leveldp) the layer words ShadowLayer works out.
// A list that would overflow sets the overflow flag; the state is then not exact, and a caller that needs exactness does not use it.
#pragma once
#include "dp/selfshadow.hpp"
#include "dp/shadow_layer.hpp"
#include "dp/shadow_lite.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

namespace dp {
namespace gdcol {

// What pack / unpack need to know about the world beyond the SelfShadow: which objects move, the static objects the seed rule can name (in order of their
// rect's right edge), the seed's movers, and which objects this side has marked activated.
struct SlWorld {
    std::vector<GObj>* world = nullptr;
    std::vector<uint8_t> mover;          // per world index: the caller moves it (never under the seed rule, never dropped from a list)
    std::vector<uint32_t> byRight;       // static seedable objects, by the right edge of their rect
    std::vector<float> rights;
    std::vector<uint32_t> byLeft;        // the objects a step can mark activated, by the left edge of their rect (the activated scan): the rings -- ringJump
    std::vector<float> lefts;            // (buttons.hpp) is the only writer of GObj::activated, and it is handed only objects of the ring types the pass
                                         // registers (collide.hpp playerTouchedRing's case) or a uid of the seed's ring array -- not movers (scanned whole)
    std::vector<int32_t> idxOfUid;       // uid -> world index (-1 = none), dense; empty when the uids are not unique or too sparse (then the shadow's map)
    std::vector<uint32_t> actTmp;        // pack's scratch
    std::vector<int> dropTmp, actKeep;   // pack's scratch: the movers a list left behind, the uids the list keeps
    std::vector<int> seedMovers;         // sorted uids
    std::vector<uint32_t> moverIdx;      // the world indices of the movers
    std::vector<uint32_t> actIdx;        // world indices whose `activated` this side has set (cleared by the next unpack)
    // slUnpack leaves ever's everW rule part out of SelfShadow::ever and has the pass ask it (SelfShadow::everRule): the set {static seedable objects whose
    // right edge is in [matLo, everW)} that it inserted object by object at every unpack. Only with unique uids (idxOfUid); leveldp's shadow_step turns it on,
    // self_test and shadow_state_test (which compare `ever` itself) do not.
    bool lazyEver = false;
    // slPack's scan of the movers' `activated` (all of them, filed by their load position: 1,234 objects read at every pack on a custom level) reads instead
    // a superset of the movers that can be activated: every writer that sets a mover's flag true adds it (noteTrue; SelfShadow::setActivated through
    // actTrueLog), and the scan drops the ones it finds false. trackActivated() turns it on (leveldp's shadow_step) while no flag is set; unique uids only
    // (the scan's result is sorted by uid). ST_ACTCHECK=1: every pack also runs the full scan and aborts when the two differ.
    bool trackAct = false;
    std::vector<uint32_t> actCand, actTrueLog;
    std::vector<uint8_t> actCandFlag;
    void trackActivated() {
        trackAct = !idxOfUid.empty();
        actCand.clear(); actTrueLog.clear();
        actCandFlag.assign(trackAct ? world->size() : 0, 0);
    }
    void noteTrue(uint32_t i) {
        if (!trackAct || !mover[i] || actCandFlag[i]) return;
        actCandFlag[i] = 1;
        actCand.push_back(i);
    }
    // ever's rule part for a uid, as slUnpack's loop over byRight from lower_bound(matLo) to everW would have held it (byRight: the non-movers that are
    // seedable; their rects never change, so right() is the value `rights` was sorted by)
    static bool everRuleSays(const void* w, float lo, float hi, bool rev, int uid) {
        const SlWorld& W = *static_cast<const SlWorld*>(w);
        if (uid < 0 || (size_t)uid >= W.idxOfUid.size()) return false;
        const int32_t wi = W.idxOfUid[(size_t)uid];
        if (wi < 0 || W.mover[(size_t)wi] || !seedable((*W.world)[(size_t)wi])) return false;
        if (rev) return !g_everPayloadOn && (*W.world)[(size_t)wi].rx > hi;   // (a body seeded travelling left: the objects to its right were passed; with the anchor payload it alone decides what lay behind)
        const float r = W.right((size_t)wi);
        if (g_everPayloadOn) return false;   // (the anchor payload decides what lay behind the anchor, all of it: a member is held by Ctx::everBase, any other was not seen)
        return !(r < lo) && r < hi;
    }

    static bool seedable(const GObj& o) { return !(o.type == 0 || o.type == 2 || o.type == 7 || o.type == 0x15 || o.type == 0x19 || o.type == 0x27); }
    float right(size_t i) const { const GObj& o = (*world)[i]; return o.rx + o.rw; }
    // `isMover[i]`: the caller moves world[i]
    static bool ringType(int t) { return t == 0xb || t == 0xc || t == 0xd || t == 0x1d || t == 0x20 || t == 0x23 || t == 0x24 || t == 0x25 || t == 0x26 || t == 0x2b; }
    // `extraAct`: uids a step may hand to ringJump besides the ring types (the seed's ring array)
    void build(std::vector<GObj>* w, const std::vector<uint8_t>& isMover, const std::vector<int>& extraAct = {}) {
        world = w;
        mover = isMover;
        mover.resize(w->size(), 0);
        byRight.clear(); rights.clear(); byLeft.clear(); lefts.clear(); actIdx.clear(); moverIdx.clear(); idxOfUid.clear();
        trackAct = false; actCand.clear(); actTrueLog.clear(); actCandFlag.clear();
        for (size_t i = 0; i < w->size(); ++i) if (mover[i]) moverIdx.push_back((uint32_t)i);
        for (size_t i = 0; i < w->size(); ++i)
            if (!mover[i] && seedable((*w)[i])) byRight.push_back((uint32_t)i);
        std::sort(byRight.begin(), byRight.end(), [&](uint32_t a, uint32_t b) { return right(a) < right(b) || (right(a) == right(b) && a < b); });
        for (uint32_t i : byRight) rights.push_back(right(i));
        // the dense uid index (unique uids only: SelfShadow's map names one object per uid, and so must this)
        {
            int mx = -1;
            bool ok = true;
            for (const GObj& o : *w) { if (o.uid < 0) { ok = false; break; } mx = std::max(mx, o.uid); }
            if (ok && mx >= 0 && (size_t)mx <= 4 * w->size() + 65536) {
                idxOfUid.assign((size_t)mx + 1, -1);
                for (size_t i = 0; i < w->size() && ok; ++i) {
                    int32_t& e = idxOfUid[(size_t)(*w)[i].uid];
                    if (e >= 0) ok = false; else e = (int32_t)i;
                }
                if (!ok) idxOfUid.clear();
            }
        }
        for (size_t i = 0; i < w->size(); ++i) {
            if (mover[i]) continue;
            const GObj& o = (*w)[i];
            if (ringType(o.type) || std::find(extraAct.begin(), extraAct.end(), o.uid) != extraAct.end()) byLeft.push_back((uint32_t)i);
        }
        std::sort(byLeft.begin(), byLeft.end(), [&](uint32_t a, uint32_t b) { return (*w)[a].rx < (*w)[b].rx || ((*w)[a].rx == (*w)[b].rx && a < b); });
        for (uint32_t i : byLeft) lefts.push_back((*w)[i].rx);   // (static objects' rects never change)
    }
    // uid -> world index as SelfShadow::worldIndexOf answers it (-1 = none)
    long long indexOf(const SelfShadow& sh, int uid) const {
        if (!idxOfUid.empty()) return (uid >= 0 && (size_t)uid < idxOfUid.size()) ? (long long)idxOfUid[(size_t)uid] : -1;
        return sh.worldIndexOf(uid);
    }
};

// The player rect's left edge in an image: playerRect's x without its write to the image's rect cache (refreshRect's width when the cache is
// stale, +0x360 else; the same float expressions, so the same bits as playerRect on a copy, which this used to make: 3,200 bytes a call).
inline float slPlayerLeft(const Mem& m) {
    return SelfShadow::playerLeftOf(m);
}

// Is `uid` (world index wi, -1 = not in the world) in ever by the rule part (everW / the seed's movers)?
inline bool slRuleSays(const ShadowLite& sl, const SlWorld& W, int uid, long long wi, bool second = false) {
    if (g_everPayloadOn && (second ? g_everPayloadSet2 : g_everPayloadSet).count(uid)) return true;   // (the anchor payload holds it: Ctx::everBase says seen)
    if ((sl.flags3 & kSl3EverRev) && (sl.flags2 & kSlEverW)) {   // E9: seeded travelling left
        if (wi >= 0 && !W.mover[(size_t)wi] && !g_everPayloadOn && SlWorld::seedable((*W.world)[(size_t)wi]) && (*W.world)[(size_t)wi].rx > sl.everWr) return true;
    } else
    if (wi >= 0 && !W.mover[(size_t)wi] && (sl.flags2 & kSlEverW) && SlWorld::seedable((*W.world)[(size_t)wi]) && W.right((size_t)wi) < sl.everW && !g_everPayloadOn) return true;
    if (sl.flags2 & kSlSeedMovers) { const std::vector<int>* sm = seedLists().get(slSeedId(sl)); if (sm && std::binary_search(sm->begin(), sm->end(), uid)) return true; }
    return false;
}

// The image words.
// (each range is one move of a size known at compile time -- shadow_lite.hpp kSlRangeStart / kSlRangeBytes; the same bytes as the word-by-word loops over kSlRanges that were here)
template <size_t... I> inline void slPackWordsAt(const uint8_t* img, uint32_t* w, std::index_sequence<I...>) {
    (std::memcpy(w + kSlRangeStart<I>, img + kSlRangeLo<I>, kSlRangeBytes<I>), ...);
}
template <size_t... I> inline void slUnpackWordsAt(const uint32_t* w, uint8_t* img, std::index_sequence<I...>) {
    (std::memcpy(img + kSlRangeLo<I>, w + kSlRangeStart<I>, kSlRangeBytes<I>), ...);
}
inline void slPackWords(const Mem& img, ShadowLite& sl) {
    slPackWordsAt(img.b.data(), sl.w, std::make_index_sequence<kSlRangeCount>{});
}
inline void slUnpackWords(const ShadowLite& sl, const std::vector<uint8_t>& tmpl, Mem& img) {
    img.b = tmpl;
    slUnpackWordsAt(sl.w, img.b.data(), std::make_index_sequence<kSlRangeCount>{});
}

// The world's `activated` flags a step left set, as the world indices W.actIdx lists them: the ones this side had set (still set), and any object the step marked near the
// player (a ring is marked by the jump it gives). The next slUnpack clears exactly W.actIdx, so a step that ends without a slPack (a death, a decline after the step) must call
// this too: a flag the step set that the list does not name stays on the thread's world and is read as fired by the next state that thread steps (the new model's search was
// not deterministic at 8 threads for that reason; --threads 1 was).
inline std::vector<uint32_t>& slTrackActivated(const SelfShadow& sh, SlWorld& W) {
    const Mem& img = sh.img;
    std::vector<uint32_t>& act = W.actTmp;
    act.clear();
    for (uint32_t i : W.actIdx) if ((*W.world)[i].activated) act.push_back(i);
    {
        const float xs[3] = {!sh.ptButtons.ok ? img.f32(po::PosX) : sh.ptButtons.posX, sh.atExit.b.empty() ? img.f32(po::PosX) : sh.atExit.f32(po::PosX), img.f32(po::PosX)};
        const float lo = std::min({xs[0], xs[1], xs[2]}) - 400.0f, hi = std::max({xs[0], xs[1], xs[2]}) + 400.0f;
        auto it = std::lower_bound(W.lefts.begin(), W.lefts.end(), lo - 2000.0f);   // (a wide object starts far to the left)
        for (size_t k = (size_t)(it - W.lefts.begin()); k < W.byLeft.size() && W.lefts[k] <= hi; ++k) {
            const uint32_t i = W.byLeft[k];
            const GObj& o = (*W.world)[i];
            if (o.activated && o.rx + o.rw >= lo && std::find(act.begin(), act.end(), i) == act.end()) act.push_back(i);
        }
        if (W.trackAct) {   // (the movers that can be activated: SlWorld::actCand)
            for (uint32_t i : W.actTrueLog) W.noteTrue(i);
            W.actTrueLog.clear();
            size_t keep = 0;
            for (size_t k = 0; k < W.actCand.size(); ++k) {
                const uint32_t i = W.actCand[k];
                if (!(*W.world)[i].activated) { W.actCandFlag[i] = 0; continue; }
                W.actCand[keep++] = i;
                if (std::find(act.begin(), act.end(), i) == act.end()) act.push_back(i);
            }
            W.actCand.resize(keep);
            static const bool check = std::getenv("ST_ACTCHECK") != nullptr;
            if (check)
                for (uint32_t i : W.moverIdx)
                    if ((*W.world)[i].activated && std::find(act.begin(), act.end(), i) == act.end()) { std::fprintf(stderr, "ST_ACTCHECK: mover %u (uid %d) activated, not in the tracked list\n", i, (*W.world)[i].uid); std::abort(); }
        } else
            for (uint32_t i : W.moverIdx)   // movers are filed by their load position: look at them all
                if ((*W.world)[i].activated && std::find(act.begin(), act.end(), i) == act.end()) act.push_back(i);
    }
    std::sort(act.begin(), act.end(), [&](uint32_t a, uint32_t b) { return (*W.world)[a].uid < (*W.world)[b].uid; });
    W.actIdx = act;
    return act;
}

// SelfShadow -> ShadowLite (after a step, or after a seed). `sl` holds the block the step started from (its rule part is kept), and is overwritten.
// which list ran out of room when a block was packed (printed with the shadowstep counts): gate, used, arr, now, ever, act
inline std::atomic<long long> g_slOverBy[6] = {};
// `movedInPass`: the pass moved the body itself (a Teleport portal fired in it, shadowAdvance's teleFresh). The step's images from before the move stand at another place -- on another axis after a frame turn,
// 825 px behind it in the case measured -- and say nothing of how far back the body reached afterwards, so the ambiguity test below reads the pass's exit and the image now only.
inline void slPack(const SelfShadow& sh, SlWorld& W, ShadowLite& sl, bool movedInPass = false) {
    SsMark sm;
    const Mem& img = sh.img;
    slPackWords(img, sl);
    sm(17);
    bool over = (sl.flags & kSlOverflow) != 0;
    auto put = [&](int32_t* dst, uint8_t& n, int cap, const auto& src, int which) {
        n = 0;
        for (int u : src) {
            if (n >= cap) { over = true; ++g_slOverBy[which]; break; }
            dst[n++] = u;
        }
    };
    {   // the gate is read for multi-activate objects only; an object the world does not know stays
        std::vector<int> keep;
        for (int u : sh.gate) {
            const long long wi = W.indexOf(sh, u);
            if (wi < 0 || (*W.world)[(size_t)wi].multiAct) keep.push_back(u);
        }
        put(sl.gate, sl.nGate, kSlGate, keep, 0);
    }
    put(sl.used, sl.nUsed, kSlUsed, sh.usedRings, 1);
    sl.nFired = 0;
    for (int u : sh.fireOrder) { if (sl.nFired >= 5) break; sl.fired[sl.nFired++] = u; }
    put(sl.arr, sl.nArr, kSlArr, sh.rt.arr, 2);
    put(sl.now, sl.nNow, kSlNow, sh.rt.now, 3);
    // the player's rect left edge now: a static object more than kSlPruneMargin behind it can leave the lists
    const float pruneAt = slPlayerLeft(img) - g_slPruneMargin;
    auto keepOrPrune = [&](int uid, long long wi) -> bool {   // true: keep in the list
        if (wi < 0 || W.mover[(size_t)wi]) return true;
        const float r = W.right((size_t)wi);
        if (!(r < pruneAt)) return true;
        if (!(sl.flags2 & kSlPruneW) || r > sl.pruneW) sl.pruneW = r;
        sl.flags2 |= kSlPruneW;
        (void)uid;
        return false;
    };
    // ever: what the rule does not already say
    // A mover leaves a list as a static object does, once its rect's right edge is more than the margin behind the player's -- but it can come back, so the block names the ones it left
    // behind (actDropLists, one list for `ever` and one for the activated) and every pack looks at where they are now: one that is no longer behind returns to its list, and if the step
    // just run could have met it (its right edge at or past the leftmost the player's rect reached in the step) the pass ran without it and the block is ambiguous, as it is for a static
    // object the body reached back to.
    // [M] a custom level anchor 18741: the activated list (cap 8) ran out of room in 2,266 blocks, every one of them on a mover (20,132 of 20,785 entries; the oldest 1,482..2,684 units behind),
    // and the 1,357 seeds from the State that followed lost what the pass knew; with the cap at 16 the list grew to 17 (the oldest 4,095 behind), and once the activated list held, `ever`
    // (cap 64) ran out of room in 1,322 blocks the same way.
    const float reach = movedInPass ? std::min(slPlayerLeft(sh.atExit), slPlayerLeft(img))
                                    : std::min({sh.ptButtons.left, sh.ptEntry.left, slPlayerLeft(sh.atExit), slPlayerLeft(img)});
    // `drop`: the block's list of the ones left behind (loaded, then written back when it changed); `keepU`: the uids the list keeps, with the ones that came back
    auto dropLoad = [&](uint32_t id, std::vector<int>& drop, std::vector<int>& keepU) -> bool {
        drop.clear(); keepU.clear();
        if (id) { if (const std::vector<int>* d = actDropLists().get(id)) drop = *d; }
        bool changed = false;
        for (size_t k = 0; k < drop.size();) {
            const long long wi = W.indexOf(sh, drop[k]);
            if (wi < 0 || W.right((size_t)wi) < pruneAt) { ++k; continue; }
            if (!(W.right((size_t)wi) < reach)) sl.flags |= kSlAmbiguous;
            keepU.push_back(drop[k]);
            drop.erase(drop.begin() + (long long)k); changed = true;
        }
        return changed;
    };
    auto dropLeave = [&](std::vector<int>& drop, int u) -> bool {   // true: the list changed
        if (std::binary_search(drop.begin(), drop.end(), u)) return false;
        drop.insert(std::upper_bound(drop.begin(), drop.end(), u), u);
        return true;
    };
    auto dropStore = [&](const std::vector<int>& drop) -> uint32_t {
        if (drop.empty()) return 0;
        const uint32_t id = actDropLists().intern(drop);
        const std::vector<int>* got = actDropLists().get(id);
        if (!got || *got != drop) { over = true; return 0; }   // (the table is full: the block does not know what it left behind)
        return id;
    };
    {
        std::vector<int>& drop = W.dropTmp; std::vector<int>& keepU = W.actKeep;
        bool changed = dropLoad(slEverDropId(sl), drop, keepU);
        for (int u : sh.ever) {
            const long long wi = W.indexOf(sh, u);
            if (slRuleSays(sl, W, u, wi, sh.p2Phase)) continue;
            if (wi >= 0 && W.mover[(size_t)wi]) {
                if (W.right((size_t)wi) < pruneAt) { if (dropLeave(drop, u)) changed = true; continue; }
            } else if (!keepOrPrune(u, wi)) continue;
            if (std::find(keepU.begin(), keepU.end(), u) == keepU.end()) keepU.push_back(u);
        }
        std::sort(keepU.begin(), keepU.end());
        sl.nEver = 0;
        for (int u : keepU) {
            if (sl.nEver >= kSlEverMax) { over = true; ++g_slOverBy[4]; break; }
            sl.ever[sl.nEver++] = u;
        }
        if (changed) slSetEverDropId(sl, dropStore(drop));
    }
    sm(19);
    // activated: the ones this side set before the step, and any object the step marked near the player (a ring is marked by the jump it gives)
    std::vector<uint32_t>& act = slTrackActivated(sh, W);
    {
        std::vector<int>& drop = W.dropTmp; std::vector<int>& keepU = W.actKeep;
        bool changed = dropLoad(slDropId(sl), drop, keepU);
        for (uint32_t i : act) {
            const int u = (*W.world)[i].uid;
            if (W.mover[(size_t)i]) {
                if (W.right((size_t)i) < pruneAt) { if (dropLeave(drop, u)) changed = true; continue; }
            } else if (!keepOrPrune(u, (long long)i)) continue;
            if (std::find(keepU.begin(), keepU.end(), u) == keepU.end()) keepU.push_back(u);
        }
        std::sort(keepU.begin(), keepU.end());
        sl.nAct = 0;
        for (int u : keepU) {
            if (sl.nAct >= g_slActCap) { over = true; ++g_slOverBy[5]; break; }
            sl.act[sl.nAct++] = u;
        }
        if (changed) slSetDropId(sl, dropStore(drop));
    }
    // what reached back past a dropped object in this step (the three images of the step, and the image now)
    if (sl.flags2 & kSlPruneW) {
        // [M] a custom level:4600, a ship leaving frame 3 through a Teleport portal (uid 8437) on tick 4799 (x 6570 -> 7395): the buttons / entry images' left edge (6555) fell behind pruneW (6645, what the 825 px jump
        // pruned), the block was flagged ambiguous, and tick 4800 was seeded from the model instead of carrying the pass's own memory on.
        const float mn = movedInPass ? std::min(slPlayerLeft(sh.atExit), slPlayerLeft(img))
                                     : std::min({sh.ptButtons.left, sh.ptEntry.left, slPlayerLeft(sh.atExit), slPlayerLeft(img)});
        if (mn <= sl.pruneW) sl.flags |= kSlAmbiguous;
    }
    uint8_t f = (uint8_t)(sl.flags & (kSlAmbiguous));
    f |= kSlValid;
    if (sh.dashOn) f |= kSlDashOn;
    if (sh.dashFirstDone) f |= kSlDashFirst;
    if (sh.speedDirty) f |= kSlSpeedDirty;
    if (sh.rngOn) f |= kSlRngOn;
    if (sh.platformerNoted) f |= kSlPlatNoted;
    if (over) f |= kSlOverflow;
    sl.flags = f;
    sl.hold = 0;
    for (int k = 1; k <= 3; ++k) if (sh.holding[k]) sl.hold |= (uint8_t)(1u << k);
    sl.dashRot = sh.dashRot; sl.dashElapsed = sh.dashElapsed; sl.pendingSpeed = sh.pendingSpeed;
    sl.rng = sh.rng;
    sl.layerTime = sh.layerTime;
    sl.frameSteps = (uint8_t)std::min(255, sh.frameSteps);
    sl.prevFrameSteps = (uint8_t)std::min(255, sh.prevFrameSteps);
    sm(18);
}

// ShadowLite -> SelfShadow over a template image. The `ever` set is built for the objects whose rect's right edge is at least `matLo` (the rule part: those
// further left cannot meet the player in this step; a caller that may teleport passes the lowest value it needs, or -inf).
// `withActivated` false (--shadowdual's player 2): the world's `activated` flags are left as they are (player 1's block sets them; the two share one world).
inline void slUnpack(const ShadowLite& sl, const std::vector<uint8_t>& tmpl, SlWorld& W, SelfShadow& sh, float matLo, bool withActivated = true) {
    SsMark sm;
    slUnpackWords(sl, tmpl, sh.img);
    sm(15);
    sh.gate.clear(); sh.usedRings.clear(); sh.rt.arr.clear(); sh.rt.now.clear(); sh.ever.clear();
    sh.fireOrder.clear();
    for (int i = 0; i < sl.nFired && i < 5; ++i) sh.fireOrder.push_back(sl.fired[i]);
    for (int i = 0; i < sl.nGate; ++i) sh.gate.insert(sl.gate[i]);
    for (int i = 0; i < sl.nUsed; ++i) sh.usedRings.insert(sl.used[i]);
    for (int i = 0; i < sl.nArr; ++i) sh.rt.arr.push_back(sl.arr[i]);
    for (int i = 0; i < sl.nNow; ++i) sh.rt.now.insert(sl.now[i]);
    for (int i = 0; i < sl.nEver; ++i) sh.ever.insert(sl.ever[i]);
    if (sl.flags2 & kSlSeedMovers) { if (const std::vector<int>* sm = seedLists().get(slSeedId(sl))) sh.ever.insert(sm->begin(), sm->end()); }
    sh.everRule.on = false; sh.everRule.rev = false;
    if ((sl.flags2 & kSlEverW) && (sl.flags3 & kSl3EverRev)) {   // E9: a block seeded travelling left: the rule is "left edge right of everWr"
        if (W.lazyEver && !W.idxOfUid.empty()) {
            sh.everRule.on = true; sh.everRule.rev = true; sh.everRule.lo = matLo; sh.everRule.hi = sl.everWr; sh.everRule.w = &W; sh.everRule.says = &SlWorld::everRuleSays;
        } else if (!g_everPayloadOn) {
            for (size_t i = 0; i < W.world->size(); ++i) { const GObj& o = (*W.world)[i]; if (!W.mover[i] && SlWorld::seedable(o) && o.rx > sl.everWr) sh.ever.insert(o.uid); }
        }
    } else if ((sl.flags2 & kSlEverW) && W.lazyEver && !W.idxOfUid.empty()) {
        sh.everRule.on = true; sh.everRule.lo = matLo; sh.everRule.hi = sl.everW; sh.everRule.w = &W; sh.everRule.says = &SlWorld::everRuleSays;
    } else if (sl.flags2 & kSlEverW) {
        auto it = std::lower_bound(W.rights.begin(), W.rights.end(), matLo);
        for (size_t k = (size_t)(it - W.rights.begin()); k < W.byRight.size() && W.rights[k] < sl.everW; ++k) sh.ever.insert((*W.world)[W.byRight[k]].uid);
    }
    if (withActivated) for (uint32_t i : W.actIdx) { (*W.world)[i].activated = false; sh.activatedChangedAt(i); }   // (the shadow's reused neighbourhood copies read these objects' `activated` again)
    if (withActivated) W.actIdx.clear();
    for (int i = 0; withActivated && i < sl.nAct; ++i) {
        const long long wi = W.indexOf(sh, sl.act[i]);
        if (wi < 0) continue;
        (*W.world)[(size_t)wi].activated = true;
        sh.activatedChangedAt((uint32_t)wi);
        W.noteTrue((uint32_t)wi);
        W.actIdx.push_back((uint32_t)wi);
    }
    for (int k = 1; k <= 3; ++k) sh.holding[k] = (sl.hold >> k) & 1;
    sh.dashOn = (sl.flags & kSlDashOn) != 0;
    sh.dashFirstDone = (sl.flags & kSlDashFirst) != 0;
    sh.speedDirty = (sl.flags & kSlSpeedDirty) != 0;
    sh.rngOn = (sl.flags & kSlRngOn) != 0;
    sh.platformerNoted = (sl.flags & kSlPlatNoted) != 0;
    sh.dashRot = sl.dashRot; sh.dashElapsed = sl.dashElapsed; sh.pendingSpeed = sl.pendingSpeed;
    sh.rng = sl.rng;
    sh.frameSteps = sl.frameSteps;
    sh.prevFrameSteps = sl.prevFrameSteps;
    sh.layerTime = sl.layerTime;
    sm(16);
}

// A seed: the rule part of ever and the seed's movers from SelfShadow::seedMemory's sets (which it has just filled from the image and the world). `rx`: the
// player rect's left edge seedMemory used. The seed's movers go to W.seedMovers; the rest of what seedMemory put in ever beyond the rule (the objects
// overlapped now and at the start of the update) is packed as the state's own list by slPack.
inline void slSeedRule(const SelfShadow& sh, SlWorld& W, ShadowLite& sl) {
    const bool turned = sh.img.u8(po::Sideways);
    if (turned && sh.seedTurnX <= 0.0f) {   // --shadowturnedever (constants.hpp): a turned frame whose turn the caller does not know: no rule part there (x is its jump axis, not the travel axis)
        sl.flags2 &= (uint8_t)~(kSlEverW | kSlSeedMovers); slSetSeedId(sl, 0); W.seedMovers.clear();
        return;
    }
    const float rx = turned ? sh.seedTurnX : slPlayerLeft(sh.img);   // (a turned frame: what lay before the turn)
    // E9: a body seeded travelling left (not a turned frame) has passed what lies to its right: everWr = the right edge of its rect
    const bool revSeed = !turned && sh.img.u8(po::Leftward) != 0;
    if (revSeed) { sl.everWr = rx + (sh.img.u8(0x368) ? sh.img.f32(0x2fc) * sh.img.f32(0x488) * sh.img.f32(0x394) : sh.img.f32(0x360)); sl.flags3 |= kSl3EverRev; }
    else { sl.everWr = 0.0f; sl.flags3 = (uint8_t)(sl.flags3 & ~kSl3EverRev); }
    sl.everW = rx;
    sl.flags2 |= kSlEverW;
    W.seedMovers.clear();
    for (int u : sh.ever) {
        const long long wi = sh.worldIndexOf(u);
        if (wi >= 0 && W.mover[(size_t)wi] && (revSeed ? (*W.world)[(size_t)wi].rx > sl.everWr : W.right((size_t)wi) < rx)) W.seedMovers.push_back(u);
    }
    std::sort(W.seedMovers.begin(), W.seedMovers.end());
    if (!W.seedMovers.empty()) { sl.flags2 |= kSlSeedMovers; slSetSeedId(sl, seedLists().intern(W.seedMovers)); }
    else { sl.flags2 &= (uint8_t)~kSlSeedMovers; slSetSeedId(sl, 0); }
}

// The layer words ShadowLayer carries.
// --shadowfireorder: the dual's player-2 block holds no record of its own; unpacking it must not empty player 1's (shadow_step.hpp, the dual birth / second body)
inline void slUnpackKeepFire(const ShadowLite& sl, const std::vector<uint8_t>& tmpl, SlWorld& W, SelfShadow& sh, float matLo, bool withActivated = true) {
    const std::vector<int> keep = sh.fireOrder;
    slUnpack(sl, tmpl, W, sh, matLo, withActivated);
    sh.fireOrder = keep;
}
inline void slPackLayer(const ShadowLayer& L, ShadowLite& sl) {
    uint8_t a = 0, b = kLyOn;
    if (L.freeMode) a |= kLyFree;
    if (L.noSnap) a |= kLyNoSnap;
    if (L.dual) a |= kLyDual;
    if (L.bandOn) a |= kLyBandOn;
    if (L.heightKnown) a |= kLyHeightKnown;
    if (L.lastPortalKnown) a |= kLyPortalKnown;
    if (L.w764Known) a |= kLyW764Known;
    if (L.flipped) a |= kLyFlipped;
    if (L.flipKnown) b |= kLyFlipKnown;
    if (L.flipTween) b |= kLyFlipTween;
    sl.ly = a; sl.ly2 = b;
    sl.lyCentre = L.centre; sl.lyHeight = L.height; sl.lyLastPortalY = L.lastPortalY; sl.lyW764 = L.w764;
    sl.lyFlipAmt = L.flipAmt; sl.lyFlipFrom = L.flipFrom; sl.lyFlipTo = L.flipTo; sl.lyFlipEl = L.flipEl;
    sl.lyLastMode = L.lastMode; sl.lyLastPortalUid = L.lastPortalUid;
}
inline void slUnpackLayer(const ShadowLite& sl, ShadowLayer& L) {
    L.freeMode = sl.ly & kLyFree; L.noSnap = sl.ly & kLyNoSnap; L.dual = sl.ly & kLyDual; L.bandOn = sl.ly & kLyBandOn;
    L.heightKnown = sl.ly & kLyHeightKnown; L.lastPortalKnown = sl.ly & kLyPortalKnown; L.w764Known = sl.ly & kLyW764Known; L.flipped = sl.ly & kLyFlipped;
    L.flipKnown = sl.ly2 & kLyFlipKnown; L.flipTween = sl.ly2 & kLyFlipTween;
    L.centre = sl.lyCentre; L.height = sl.lyHeight; L.lastPortalY = sl.lyLastPortalY; L.w764 = sl.lyW764;
    L.flipAmt = sl.lyFlipAmt; L.flipFrom = sl.lyFlipFrom; L.flipTo = sl.lyFlipTo; L.flipEl = sl.lyFlipEl;
    L.lastMode = sl.lyLastMode; L.lastPortalUid = sl.lyLastPortalUid;
}

}  // namespace gdcol
}  // namespace dp
