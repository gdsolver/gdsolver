// S2 stage 2: the block a dp State carries for the self-running shadow (State::sl). Only the layout is here, so that state.hpp can hold it without the
// shadow's code; what it means and how it is filled is dp/shadow_state.hpp. All zero = not used (kSlValid clear): a State that never ran the shadow.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace dp {
namespace gdcol {

struct SlRange { uint16_t lo, hi; };
inline constexpr SlRange kSlRanges[] = {
    {0x38, 0x40}, {0x4c, 0x54}, {0x11c, 0x120}, {0x2fc, 0x304}, {0x358, 0x36c}, {0x390, 0x39c}, {0x4d0, 0x4d8}, {0x560, 0x564},
    {0x568, 0x5a4}, {0x5d0, 0x5d8}, {0x5e0, 0x5f0}, {0x600, 0x610}, {0x628, 0x648}, {0x650, 0x658}, {0x660, 0x664}, {0x670, 0x674},
    {0x678, 0x698}, {0x6a0, 0x6a4}, {0x6f8, 0x700}, {0x728, 0x734}, {0x7c0, 0x7d8}, {0x7e0, 0x7f4}, {0x808, 0x810}, {0x828, 0x830}, {0x838, 0x84c},
    {0x890, 0x898}, {0x908, 0x90c}, {0x938, 0x95c}, {0x960, 0x970}, {0x980, 0x984}, {0x98c, 0x994}, {0x9a4, 0x9d4}, {0x9d8, 0x9e8},
    {0x9ec, 0x9f0}, {0x9f8, 0xa20}, {0xa24, 0xa28}, {0xa30, 0xa34}, {0xa88, 0xa94}, {0xa98, 0xaa4}, {0xaa8, 0xab0}, {0xab8, 0xac0},
    {0xac4, 0xac8}, {0xae0, 0xae8}, {0xaec, 0xafc}, {0xb40, 0xb50}, {0xb58, 0xb60}, {0xb7c, 0xb9c}, {0xba8, 0xbac}, {0xc40, 0xc48}};
inline constexpr size_t slWordCount() {
    size_t n = 0;
    for (const SlRange& r : kSlRanges) n += (size_t)(r.hi - r.lo) / 4;
    return n;
}
inline constexpr size_t kSlWords = slWordCount();
static_assert(kSlWords == 171, "the carried words (S2 stage 0's footprint, and +0x6f8)");
// ...and a range's place among the carried words and its length in bytes, as constants: shadow_state.hpp's slPackWords / slUnpackWords copy each range with one
// fixed-size move (a block is packed and unpacked at every stepped tick; word by word through the image's byte vector it was 0.13 + 0.24 us of a 2.8 us step)
inline constexpr size_t kSlRangeCount = sizeof(kSlRanges) / sizeof(kSlRanges[0]);
inline constexpr bool slRangesWhole() { for (const SlRange& r : kSlRanges) if (r.hi <= r.lo || (r.hi - r.lo) % 4) return false; return true; }
static_assert(slRangesWhole(), "every carried range is a whole number of 4-byte words");
inline constexpr size_t slRangeStart(size_t ri) { size_t k = 0; for (size_t i = 0; i < ri; ++i) k += (size_t)(kSlRanges[i].hi - kSlRanges[i].lo) / 4; return k; }
template <size_t I> inline constexpr size_t kSlRangeStart = slRangeStart(I);
template <size_t I> inline constexpr size_t kSlRangeBytes = (size_t)(kSlRanges[I].hi - kSlRanges[I].lo);
template <size_t I> inline constexpr size_t kSlRangeLo = (size_t)kSlRanges[I].lo;
// the index in ShadowLite::w of the carried word at image offset `off` (-1 when it is not carried)
inline constexpr int slWordIndex(size_t off) {
    int k = 0;
    for (const SlRange& r : kSlRanges) {
        if (off >= r.lo && off < r.hi) return k + (int)((off - r.lo) / 4);
        k += (int)((r.hi - r.lo) / 4);
    }
    return -1;
}
inline bool slCarried(size_t off) {
    for (const SlRange& r : kSlRanges)
        if (off >= r.lo && off < r.hi) return true;
    return false;
}

inline constexpr int kSlGate = 12, kSlUsed = 4, kSlArr = 4, kSlNow = 4, kSlEver = 24, kSlAct = 8;   // (the closed loop's maxima over 130 windows: 8, 2, 3, 3, 14, 4)
inline constexpr int kSlEverMax = 64;   // the array's size; --shadowevercap (constants.hpp) lets `ever` use all of it, else the first kSlEver
inline constexpr float kSlPruneMargin = 60.0f;
// P9 stage 1 (restructure draft 7.18): the activated list ran out of room in 19,013 blocks of c139771614 and 550 of c133224341 (cap 8; gate / used / arr / now / ever never did), and the prune margin made 2,478 blocks of lv22
// ambiguous. The array holds kSlActMax; the cap in force is g_slActCap (--shadowactcap N, default kSlAct = 8: a block packs as before) and the margin g_slPruneMargin (--shadowprunemargin X, default 60).
inline constexpr int kSlActMax = 48;
inline int g_slActCap = kSlAct;
inline float g_slPruneMargin = kSlPruneMargin;

// flags
enum : uint8_t { kSlValid = 1, kSlDashOn = 2, kSlDashFirst = 4, kSlSpeedDirty = 8, kSlRngOn = 16, kSlPlatNoted = 32, kSlOverflow = 64, kSlAmbiguous = 128 };
// flags2
enum : uint8_t { kSlEverW = 1, kSlPruneW = 2, kSlLastBtn = 4, kSlSeedMovers = 8, kSlHeadPending = 16, kSlCtrlOff = 32, kSlSpiderPass = 64, kSlSpiderEarly = 128 };   // kSlHeadPending: shadow_step.hpp (a ship / UFO row whose own step's head has not run); kSlSpiderPass: the body's spider stamp (+0x828) was last written by the collision pass, not by a button (--shadowspiderjump)
// flags3
enum : uint8_t { kSl3EverRev = 1 };
// ly
enum : uint8_t { kLyFree = 1, kLyNoSnap = 2, kLyDual = 4, kLyBandOn = 8, kLyHeightKnown = 16, kLyPortalKnown = 32, kLyW764Known = 64, kLyFlipped = 128 };
// ly2
enum : uint8_t { kLyFlipKnown = 1, kLyFlipTween = 2, kLyOn = 4 };

// Every member defaults to zero, so a State that never ran the shadow carries an all-zero block (kSlValid clear).
struct ShadowLite {
    double layerTime = 0.0;   // GJBaseGameLayer +0x3c8, the clock the head of a step adds to (not always the image's +0xaa8: the update writes that too)
    uint32_t w[kSlWords] = {};
    int32_t gate[kSlGate] = {}, used[kSlUsed] = {}, arr[kSlArr] = {}, now[kSlNow] = {}, ever[kSlEverMax] = {}, act[kSlActMax] = {};
    float everW = 0.f, pruneW = 0.f;
    // E9 (11-18): the other direction of ever's rule. everW says "every static seedable object whose right edge is left of it was seen" (a body travelling right); with kSl3EverRev set the block was seeded
    // by a body travelling LEFT and everWr says "every one whose left edge is right of it was seen". The rule used to assume forward travel at every seed, so a reverse anchor (lv22 t=13400, rev 1) took
    // the objects AHEAD of it for passed ones and the first of them it met (the mini portal uid 6445) did not fire.
    float everWr = 0.f, dashSpinRate = 0.f;
    float dashRot = 0.f, dashElapsed = 0.f, pendingSpeed = 0.f;
    uint32_t rng = 0;
    float lyCentre = 0.f, lyHeight = 0.f, lyLastPortalY = 0.f, lyW764 = 0.f, lyFlipAmt = 0.f, lyFlipFrom = 0.f, lyFlipTo = 0.f, lyFlipEl = 0.f;
    int32_t lyLastMode = 0, lyLastPortalUid = 0;
    int32_t fired[5] = {};   // --shadowfireorder: the rings fired, newest first (SelfShadow::fireOrder), nFired of them
    uint8_t nGate = 0, nUsed = 0, nArr = 0, nNow = 0, nEver = 0, nAct = 0, nFired = 0;
    uint8_t flags = 0, flags2 = 0, hold = 0, frameSteps = 0, prevFrameSteps = 0, ly = 0, ly2 = 0;
    uint8_t seedId0 = 0, seedId1 = 0, flags3 = 0;   // the seed's movers list this block refers to (SeedLists), 0 = none: low and high byte of its id; flags3: kSl3EverRev
    uint8_t dropId0 = 0, dropId1 = 0;               // the activated movers this block left behind (actDropLists), 0 = none: slPack
    uint8_t everDropId0 = 0, everDropId1 = 0;       // ...and the movers of `ever` it left behind (the same table)
};
static_assert(sizeof(ShadowLite) % 8 == 0, "ShadowLite packs into whole 8-byte rows");

// The seed's movers lists (the movers behind the player at a seed, sorted uids). A block that carries kSlSeedMovers names its list by id, so the list belongs to the block and not to whichever thread
// seeded last (a per-thread list was rewritten by every later seed on the thread, and a state stepped on another thread read another seed's list: the search depended on the schedule). Lists
// are interned by content and never change once published. A reader never touches anything that grows: the lists are published in a fixed array of atomic pointers (slot id - 1), each stored with release
// after the list is complete and read with acquire; the container that owns the lists (a deque) and the content index are only read and written under the mutex, by the writers. clear() may run only while
// no thread reads (the search calls nothing between calls; no block of an earlier call is stepped after it).
class SeedLists {
public:
    static constexpr uint32_t kMax = 65535;
    uint32_t intern(const std::vector<int>& l) {
        if (l.empty()) return 0;
        uint64_t h = 1469598103934665603ULL;
        for (int u : l) { h ^= (uint64_t)(uint32_t)u; h *= 1099511628211ULL; }
        std::lock_guard<std::mutex> g(mu_);
        auto& ids = byHash_[h];
        for (uint32_t id : ids) if (own_[id - 1] == l) return id;
        if (own_.size() >= kMax) { overflow_.fetch_add(1, std::memory_order_relaxed); return (uint32_t)own_.size(); }   // (the last list stands in: counted)
        own_.push_back(l);
        const uint32_t id = (uint32_t)own_.size();
        ids.push_back(id);
        slot_[id - 1].store(&own_.back(), std::memory_order_release);   // (a deque keeps the address of an element it holds when it grows)
        return id;
    }
    // null for 0 or an id not published; the list is immutable
    const std::vector<int>* get(uint32_t id) const {
        if (!id || id > kMax) return nullptr;
        return slot_[id - 1].load(std::memory_order_acquire);
    }
    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        for (size_t k = 0; k < own_.size(); ++k) slot_[k].store(nullptr, std::memory_order_release);
        own_.clear(); byHash_.clear();
    }
    long long overflow() const { return overflow_.load(); }
private:
    std::atomic<const std::vector<int>*> slot_[kMax] = {};
    std::deque<std::vector<int>> own_;   // (the owner: touched under mu_ only)
    std::unordered_map<uint64_t, std::vector<uint32_t>> byHash_;   // (likewise)
    std::mutex mu_;
    std::atomic<long long> overflow_{0};
};
inline SeedLists& seedLists() { static SeedLists S; return S; }
// The activated movers a block has left behind (slPack: sorted uids, interned by content as the seed's movers lists are). GD keeps `activated` on the object, one byte each, with no limit to how many
// are set; the block carries the ones near the body as a list (act) and names the rest here, so a level whose rings move (a custom level: 32,473 movers) does not fill the list with rings thousands of units behind.
inline SeedLists& actDropLists() { static SeedLists S; return S; }
inline uint32_t slDropId(const ShadowLite& sl) { return (uint32_t)sl.dropId0 | ((uint32_t)sl.dropId1 << 8); }
inline void slSetDropId(ShadowLite& sl, uint32_t id) { sl.dropId0 = (uint8_t)(id & 0xff); sl.dropId1 = (uint8_t)((id >> 8) & 0xff); }
inline uint32_t slEverDropId(const ShadowLite& sl) { return (uint32_t)sl.everDropId0 | ((uint32_t)sl.everDropId1 << 8); }
inline void slSetEverDropId(ShadowLite& sl, uint32_t id) { sl.everDropId0 = (uint8_t)(id & 0xff); sl.everDropId1 = (uint8_t)((id >> 8) & 0xff); }
inline uint32_t slSeedId(const ShadowLite& sl) { return (uint32_t)sl.seedId0 | ((uint32_t)sl.seedId1 << 8); }
inline void slSetSeedId(ShadowLite& sl, uint32_t id) { sl.seedId0 = (uint8_t)(id & 0xff); sl.seedId1 = (uint8_t)((id >> 8) & 0xff); }

// ---- where the blocks live (S2 stage 2c) ----
// A dp State does not hold its ShadowLite: it holds State::slRef, a handle into this store, so that the State the search copies, sorts and dedupes stays the
// model's 696 B. The store is two pools of fixed 1000-byte slots. A tick writes each child's block into the active pool (any thread; a slot is taken with
// one atomic add) and the child carries the handle. Between two layers the search collects (gc): the new frontier's blocks stay in the pool they were
// written to, their handles take that pool's new tag, and the other pool -- the frontier before -- is retired and becomes the active one, so the store
// holds the current layer and the children being stepped, never an older layer. (Before 2026-11-18 the kept blocks were copied into the other pool.)
// The older layers need nothing: a search keeps its plan as the arena's (parent, input) nodes, and every walk of a plan (the witness, a replay) steps the
// shadow again from the root, seeding it the way the search did.
// A handle is the pool's 10-bit tag (1..1023) and the slot + 1 (22 bits: 4M slots a pool; a put past that is refused, counted, and the state carries no
// block). A handle whose pool has been retired is answered with nothing (`lost`, counted and printed): the state is then seeded again from the model's
// fields, as any state the shadow has no block for. Only a State held across 511 gc's (a gc issues two tags) could meet its tag again; nothing that is stepped is held so (the
// ladder's saves keep their blocks by value). Nothing of this exists with the flag off (g_slStore stays null, every slRef 0).
class SlStore {
public:
    static constexpr uint32_t kIdxBits = 22, kIdxMask = (1u << kIdxBits) - 1, kChunkBits = 10, kChunk = 1u << kChunkBits, kMaxChunks = (1u << kIdxBits) >> kChunkBits;

    // A new slot in the active pool, its handle in `ref`. Thread-safe against other puts; not against gc.
    // A thread takes kBatch slots of the pool at a time and hands them out itself: one counter that every stepping thread added to for every block
    // was a cache line passed round the threads once a stepped tick. The slots of a batch a thread does not use before the next gc stay unwritten
    // (no handle names them; Pool::n counts them, which only bounds what a handle may name). A batch belongs to one store (id_) between two gcs
    // (epoch_): after a gc, or for another store, the thread takes a new one.
    static constexpr uint32_t kBatch = 32;
    ShadowLite* alloc(uint32_t& ref) {
        struct Mine { uint64_t store = 0, epoch = 0; uint32_t next = 0, end = 0, tag = 0; int pool = 0; };
        static thread_local Mine m;
        const uint64_t ep = epoch_.load(std::memory_order_relaxed);
        if (m.store != id_ || m.epoch != ep || m.next == m.end) {
            Pool& P = pool_[act_];
            const uint32_t i0 = P.n.fetch_add(kBatch, std::memory_order_relaxed);
            m.store = id_; m.epoch = ep; m.next = i0; m.end = i0 + kBatch; m.tag = P.tag; m.pool = act_;
        }
        const uint32_t i = m.next++;
        if (i + 1 >= kIdxMask) { ref = 0; overflow_.fetch_add(1, std::memory_order_relaxed); return &scratch(); }
        ref = (m.tag << kIdxBits) | (i + 1);
        return pool_[m.pool].slot(i);
    }
    uint32_t put(const ShadowLite& v) {
        uint32_t ref = 0;
        ShadowLite* p = alloc(ref);
        std::memcpy(static_cast<void*>(p), &v, sizeof v);
        return ref;
    }
    // The block of a handle; null for 0 and for a handle into a retired pool (counted).
    const ShadowLite* get(uint32_t ref) const {
        if (!ref) return nullptr;
        const uint32_t tag = ref >> kIdxBits, i = (ref & kIdxMask) - 1;
        for (const Pool& P : pool_)
            if (P.tag == tag && i < P.n.load(std::memory_order_relaxed)) return P.at(i);
        lost_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // Between layers, single-threaded: keep the blocks of `live` (each State's slRef rewritten), retire everything else. The retired pool keeps chunks
    // for about the next frontier and frees the rest.
    // Since 2026-11-18 the kept blocks stay where they are: the layer's children were written into the active pool, so the new frontier's blocks are there
    // already. The active pool takes a new tag (every handle that is not rewritten here dies, as it did when the pool was retired), each live handle is
    // rewritten to that tag with its slot unchanged, and the OTHER pool -- the frontier before this layer, nobody's any more -- is emptied and becomes the
    // active one for the next layer's children. Copying every live block into the other pool at every layer (the gc before) was 0.35 s of a 3.7 s search on
    // Back On Track, single-threaded. A live handle still into the other pool (a state no put of this layer rewrote) has its block moved over, as before.
    // What the search sees is the same: a handle is only ever resolved to its block's bytes (slOf), never compared or ordered by its value.
    template <class States>
    void gc(States& live) {
        Pool& cur = pool_[act_];
        Pool& old = pool_[act_ ^ 1];
        const uint32_t curTag = cur.tag, oldTag = old.tag;
        const uint32_t curN0 = cur.n.load(std::memory_order_relaxed), oldN = old.n.load(std::memory_order_relaxed);
        const uint32_t newTag = nextTag(curTag);
        auto keep = [&](uint32_t& ref) {
            if (!ref) return;
            const uint32_t tag = ref >> kIdxBits, i = (ref & kIdxMask) - 1;
            if (tag == curTag && i < curN0) { ref = (newTag << kIdxBits) | (i + 1); return; }   // in place
            if (oldTag && tag == oldTag && i < oldN) {                                            // a block of the pool that is going: moved into this one
                const uint32_t k = cur.n.fetch_add(1, std::memory_order_relaxed);
                if (k + 1 >= kIdxMask) { ref = 0; overflow_.fetch_add(1, std::memory_order_relaxed); return; }
                std::memcpy(static_cast<void*>(cur.slot(k)), old.at(i), sizeof(ShadowLite));
                ref = (newTag << kIdxBits) | (k + 1);
                return;
            }
            lost_.fetch_add(1, std::memory_order_relaxed);   // a handle into a retired pool
            ref = 0;
        };
        for (auto& s : live) keep(s.slRef);
        // --shadowdual: player 2's block (State::slRef2) is kept the same way; without this every layer of a search dropped it and player 2 was seeded from
        // the model again on the next tick (lv16: 2,827,976 of 2,834,982 dual ticks). 0 without the flag.
        for (auto& s : live) keep(s.slRef2);
        const uint32_t curN = cur.n.load(std::memory_order_relaxed);
        cur.tag = newTag;
        peak_ = std::max<uint64_t>(peak_, (uint64_t)curN);
        old.n.store(0, std::memory_order_relaxed);
        old.tag = nextTag(newTag);
        old.trim(((size_t)curN * 5 / 4 + kChunk) >> kChunkBits);
        act_ ^= 1;
        ++gcs_;
        epoch_.fetch_add(1, std::memory_order_relaxed);   // (the threads' batches were slots of the pool that was active)
    }
    long long lost() const { return lost_.load(); }
    long long overflow() const { return overflow_.load(); }
    long long gcs() const { return gcs_; }
    uint64_t peakSlots() const { return std::max<uint64_t>(peak_, (uint64_t)pool_[act_].n.load()); }
    size_t heldBytes() const { return (pool_[0].chunks() + pool_[1].chunks()) * (size_t)kChunk * sizeof(ShadowLite); }

private:
    // Chunks a trim gives up are kept for the next pool that needs one (up to kSpareMax) instead of going back to the heap: a layer's children
    // outgrow what gc kept for about the last frontier, so every layer took a fresh megabyte chunk and the next gc freed it again (and the heap
    // handed back pages that the next layer's first writes faulted in). Pools count only the chunks they hold, so heldBytes is as before.
    struct Spare {
        static constexpr size_t kSpareMax = 64;
        std::mutex mu;
        std::vector<ShadowLite*> v;
        ShadowLite* take() { std::lock_guard<std::mutex> g(mu); if (v.empty()) return nullptr; ShadowLite* p = v.back(); v.pop_back(); return p; }
        void give(ShadowLite* p) {
            { std::lock_guard<std::mutex> g(mu); if (v.size() < kSpareMax) { v.push_back(p); return; } }
            ::operator delete(static_cast<void*>(p));
        }
        ~Spare() { for (ShadowLite* p : v) ::operator delete(static_cast<void*>(p)); }
    };
    struct Pool {
        std::unique_ptr<std::atomic<ShadowLite*>[]> c{new std::atomic<ShadowLite*>[kMaxChunks]};
        std::atomic<uint32_t> n{0};
        uint32_t tag = 0;
        std::mutex mu;
        uint32_t top = 0;         // (under mu) one past the highest chunk that may be held
        Spare* spare = nullptr;   // (the store's; null: chunks go back to the heap)
        Pool() { for (uint32_t k = 0; k < kMaxChunks; ++k) c[k].store(nullptr, std::memory_order_relaxed); }
        ~Pool() { spare = nullptr; trim(0); }
        ShadowLite* slot(uint32_t i) {
            std::atomic<ShadowLite*>& a = c[i >> kChunkBits];
            ShadowLite* p = a.load(std::memory_order_acquire);
            if (!p) {
                std::lock_guard<std::mutex> g(mu);
                p = a.load(std::memory_order_relaxed);
                if (!p) {   // raw storage: a slot is always written whole before it is read
                    if (spare) p = spare->take();
                    if (!p) p = static_cast<ShadowLite*>(::operator new(sizeof(ShadowLite) * (size_t)kChunk));
                    a.store(p, std::memory_order_release);
                    top = std::max(top, (i >> kChunkBits) + 1);
                }
            }
            return p + (i & (kChunk - 1));
        }
        const ShadowLite* at(uint32_t i) const { return c[i >> kChunkBits].load(std::memory_order_acquire) + (i & (kChunk - 1)); }
        size_t chunks() const { size_t k = 0; for (uint32_t j = 0; j < kMaxChunks; ++j) k += c[j].load(std::memory_order_relaxed) != nullptr; return k; }
        // (`top`: no chunk at or above it is held, so a trim walks the chunks there can be and not all 4096 pointers -- that walk was 10 us of every gc)
        void trim(size_t keep) {
            std::lock_guard<std::mutex> g(mu);
            const uint32_t k0 = (uint32_t)std::min<size_t>(keep, kMaxChunks);
            for (uint32_t j = k0; j < top; ++j) {
                ShadowLite* p = c[j].exchange(nullptr, std::memory_order_relaxed);
                if (!p) continue;
                if (spare) spare->give(p); else ::operator delete(static_cast<void*>(p));
            }
            if (top > k0) top = k0;
        }
    };
    static uint32_t nextTag(uint32_t t) { return t % 1023 + 1; }
    static ShadowLite& scratch() { static thread_local ShadowLite s; return s; }
    Spare spare_;   // (before the pools: destroyed after them)
    Pool pool_[2];
    int act_ = 0;
    mutable std::atomic<long long> lost_{0};
    std::atomic<long long> overflow_{0};
    long long gcs_ = 0;
    uint64_t peak_ = 0;
    std::atomic<uint64_t> epoch_{1};   // one more after every gc
    static uint64_t newId() { static std::atomic<uint64_t> n{0}; return n.fetch_add(1, std::memory_order_relaxed) + 1; }
    const uint64_t id_ = newId();      // this store among all the stores of the process (a new store may stand where an old one stood)
public:
    SlStore() { pool_[0].tag = 1; pool_[0].spare = &spare_; pool_[1].spare = &spare_; }
};
inline SlStore* g_slStore = nullptr;   // made by shadowStepInit (--shadowstep); null with the flag off
inline const ShadowLite* slOf(uint32_t ref) { return (ref && g_slStore) ? g_slStore->get(ref) : nullptr; }

// A frontier's blocks kept by value outside the store (--ladderprefix's saves): the first block whole, and for every block the 4-byte words that differ from
// it. The blocks of one layer share most of their words (the clock, the frame counters, the level's constant words, the lists' tails), so a save holds
// tens of bytes a state instead of 1000.
struct SlDiffRun {
    static constexpr size_t kW = sizeof(ShadowLite) / 4;
    static_assert(sizeof(ShadowLite) % 4 == 0, "whole words");
    ShadowLite base{};
    bool haveBase = false;
    std::vector<uint32_t> data;   // per block: the count n (0xffffffff = no block), then n pairs (word index, word)
    std::vector<uint32_t> at;     // per block: where its entry starts in data
    void add(const ShadowLite* p) {
        at.push_back((uint32_t)data.size());
        if (!p) { data.push_back(0xffffffffu); return; }
        if (!haveBase) { std::memcpy(static_cast<void*>(&base), p, sizeof base); haveBase = true; }
        uint32_t w[kW], b[kW];
        std::memcpy(w, p, sizeof w);
        std::memcpy(b, &base, sizeof b);
        const size_t n0 = data.size();
        data.push_back(0);
        for (uint32_t k = 0; k < (uint32_t)kW; ++k)
            if (w[k] != b[k]) { data.push_back(k); data.push_back(w[k]); ++data[n0]; }
        s_blocks().fetch_add(1, std::memory_order_relaxed);
        s_words().fetch_add(data[n0], std::memory_order_relaxed);
    }
    // what the saves have held so far: blocks, and words that differed from their run's first (printed with the store's counts)
    static std::atomic<long long>& s_blocks() { static std::atomic<long long> n{0}; return n; }
    static std::atomic<long long>& s_words() { static std::atomic<long long> n{0}; return n; }
    bool get(size_t i, ShadowLite& out) const {
        if (i >= at.size()) return false;
        const uint32_t* d = data.data() + at[i];
        if (d[0] == 0xffffffffu) return false;
        uint32_t w[kW];
        std::memcpy(w, &base, sizeof w);
        for (uint32_t j = 0; j < d[0]; ++j) w[d[1 + 2 * j]] = d[2 + 2 * j];
        std::memcpy(static_cast<void*>(&out), w, sizeof w);
        return true;
    }
    size_t bytes() const { return sizeof(*this) + data.capacity() * 4 + at.capacity() * 4; }
};

// --shadowstep: the dedupe key's part that only the shadow's own state holds (search_key.hpp modelKeyOf). The shadow steps a tick as the tail of GD's step t
// and the head of step t+1, so a press shows in the body (y, vy, grounded) one model tick later than in the model's own tick: the pressed child and the
// released child of a tick have the same x, y, vy and grounded, and differ only in what the buttons wrote -- the holding bits, the button the next head
// compares against, and the press-state bytes of the image (+0x98d H, +0x98e S, +0x991 Hm, +0x992 Sm, +0x993 slot A: the carried words +0x98c / +0x990).
// Without this the dedupe kept one of the two (the first, the released one) and every jump was lost (the 2.8 parents a layer of 2026-10-31). The dash bits
// are in for the same reason the model keys `dashing`. 0 for a state the shadow did not step (sl all zero), so every key without the flag is bit-identical.
// --searchcensus only: which parts of slKeyBits count (1 press-state bytes, 2 holding bits, 4 last button, 8 dash bits); all of them, always, outside the census.
inline uint32_t g_slKeyMask = 0xF;
// ...and which bytes of the press part count (bit k = byte k of +0x98d..+0x993: H, S, Hp, Rp, Hm, Sm, slot A); all of them outside the census
inline uint32_t g_slPressBytes = 0x7F;
// The press bytes Hp (+0x98f) and Rp (+0x990) are left out of the key of a level with no Event trigger. updateJump reads them only to raise gameEventTriggered (events 8 / 9 / 12 / 16 ...),
// which acts through Event triggers (id 3604) alone; they are the previous tick's H and the previous HB, copied at the end of updateJump, so two states alike in H, S and the rest differ in them only by
// which tick the button came down on. g_levelHasEventTrig is true until the loader has seen the level's triggers and found none.
// ...and Hm (+0x991), Sm (+0x992) and slot A (+0x993) are left out always. They are copies of H, S and a clear flag that pushButton (always, for a button 1 press: 0x98d := 0x101 and then
// 0x991 := 0x98d, 0x992 := 0x98e, 0x993 := 0) and the end of PlayerObject::update (0x991 := 0x98d, 0x992 := 0x98e, 0x993 := 0) rewrite, and ringJump (0x4f6f50) alone reads them -- reached from pushButton after
// its rewrite or from the collision pass after the update of the same step. A value a state carries into the next step is overwritten before anything reads it.
// (Both were the switches --slkeynoevents and --slkeydeadcopies, in the default bundle until they were folded in; see dp/removed_flags.txt.)
// --spidergrid (on; --no-spidergrid is the old scan): spider.hpp answers the spider teleport's two world queries from an x-column index, not a scan of every object (the same answer)
inline bool g_spiderGrid = true;
inline bool g_levelHasEventTrig = true;
inline uint64_t slKeyBits(const ShadowLite& sl) {
    if (!(sl.flags & kSlValid)) return 0;
    constexpr int i98c = slWordIndex(0x98c), i990 = slWordIndex(0x990);
    static_assert(i98c >= 0 && i990 >= 0, "the press-state bytes are carried");
    uint64_t press = (g_slKeyMask & 1) ? ((uint64_t)(sl.w[i98c] & 0xffffff00u) | ((uint64_t)sl.w[i990] << 32)) : 0;
    uint32_t pb = g_slPressBytes;
    pb &= ~(16u | 32u | 64u);                                         // Hm, Sm, slot A: rewritten before they are read
    if (!g_levelHasEventTrig) pb &= ~(4u | 8u);                      // Hp, Rp: read only to raise game events, and this level listens to none
    if (pb != 0x7F) {
        uint64_t keep = 0;
        for (int k = 0; k < 7; ++k) if (pb & (1u << k)) keep |= 0xFFull << (8 * (k + 1));
        press &= keep;
    }   // (+0x98c itself is the frame-head byte: the same for the whole layer)
    const uint64_t bits = ((g_slKeyMask & 2) ? (uint64_t)sl.hold : 0) | ((g_slKeyMask & 4) ? ((uint64_t)(sl.flags2 & kSlLastBtn) << 4) : 0) | ((g_slKeyMask & 8) ? ((uint64_t)(sl.flags & (kSlDashOn | kSlDashFirst)) << 8) : 0);
    return ((press + 1) * 0x9E3779B97F4A7C15ull) ^ ((bits + 1) * 0xC2B2AE3D27D4EB4Full) ^ 0x5851F42D4C957F2Dull;
}
// ...of a State's handle (0 for no block)
inline uint64_t slKeyBitsOf(uint32_t ref) {
    const ShadowLite* p = slOf(ref);
    return p ? slKeyBits(*p) : 0;
}

}  // namespace gdcol
}  // namespace dp
