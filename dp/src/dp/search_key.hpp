#pragma once
#include "dp/thread_pool.hpp"
#include <charconv>
#include <tuple>
#include <type_traits>

namespace dp {

// Suffix maximum of every surface top at or ahead of an x bucket. A player that
// is airborne, upside down, and already above everything left in the level can
// never land again -- it falls upward forever. That is not a death in GD, so
// nothing pruned it and the frontier filled up with escapees: on lv9 all 56
// surviving states at t=7370 were at y=511..697 climbing out, while the states
// in the actual corridor had died. g_yBound only caught them 120 px above the
// level, far too late to keep the corridor branches alive.
inline std::vector<double> g_topAhead;   // per 30 px bucket
inline double g_bucketX0 = 0.0;
inline double topAheadAt(double x) {
    if (g_topAhead.empty()) return 1e9;
    long long i = (long long)((x - g_bucketX0) / 30.0);
    if (i < 0) i = 0;
    if (i >= (long long)g_topAhead.size()) return -1e9;
    return g_topAhead[(size_t)i];
}
// Rotated-frame version. frame 0 is the same as above. The right thing is to
// DO THE SAME TEST IN THE FRAME'S COORDINATES, not to disable it: disabled, the
// "can never come back" branches were left unpruned and squeezed the frontier
// at lv22's cold entry (measured 2026-08-15).
inline double topAheadAtF(int f, double x) {
    if ((f & 3) == 0) return topAheadAt(x);
    const auto& slot = g_frameLv[(size_t)(f & 3)];
    if (!slot || slot->topAhead.empty()) return 1e9;
    long long i = (long long)((x - slot->bucketX0) / 30.0);
    if (i < 0) i = 0;
    if (i >= (long long)slot->topAhead.size()) return -1e9;
    return slot->topAhead[(size_t)i];
}
// For the --slopedbg `escapee:` print: the bucket topAheadAtF reads and the
// table's length (i >= n: it answered -1e9 from past the end; n == 0: no table).
inline void topAheadIndex(int f, double x, long long& i, long long& n) {
    const std::vector<double>* t = nullptr;
    double x0 = 0.0;
    if ((f & 3) == 0) {
        t = &g_topAhead;
        x0 = g_bucketX0;
    } else {
        const auto& slot = g_frameLv[(size_t)(f & 3)];
        if (slot) { t = &slot->topAhead; x0 = slot->bucketX0; }
    }
    n = t ? (long long)t->size() : 0;
    i = (t && n) ? std::max(0LL, (long long)((x - x0) / 30.0)) : -1;
}
// ship dedupe granularity (cube is always 0.5px / 0.1). Ship layers are the
// only ones that saturate the cap, so these two knobs set the runtime.
inline double g_shipYq = 0.5;   // multiplier: 0.5 -> 2px bins
inline double g_shipVq = 2.5;   // multiplier: 2.5 -> 0.4 bins
// cube/ball dedupe granularity. Fixed at 2.0 / 10.0 (0.5 px, 0.1 vy) until lv9
// turned up a 7 px gap the frontier could not thread -- worth being able to
// sharpen it there without paying for it on every level.
inline double g_cubeYq = 2.0;
inline double g_cubeVq = 10.0;
// Dedupe multiplier for a dual's second body while it is in open air (see
// stepBoth / keyOf). 0.125 = 4 px / 0.8 vy bins instead of 0.5 px / 0.1.
inline double g_dualFreeQ = 0.125;
// --heldkeyread (off): key `held` only for a body whose next tick reads it or writes it from the
// button -- ship, UFO, wave and swing (and every dual) -- instead of in every mode. Only the wave
// and flight branches write `held` (without --heldall), so in a cube, ball, robot or spider it
// is whatever the last flight section left, read by nothing, and it split those cells: measured
// by --searchcensus over the 22 official levels and 7 customs, 15% of cube cells, 21% of ball
// and 6% of spider cells were kept apart by `held` alone.
inline constexpr bool kDefHeldKeyRead = false;
inline bool g_heldKeyRead = kDefHeldKeyRead;
// --keyxq / --keyxqfly (default 4.0 = the 0.25 px x bucket): the x-phase bucket multiplier for
// the ground modes (cube, ball, robot, spider) and for the flying ones (ship, UFO, wave, swing).
// The x term keeps the snap phases apart (see keyOf); --searchcensus measured it as the largest
// single divider of the frontier (56% of cube cells, 59% of ship cells).
inline constexpr double kDefKeyXq = 4.0;
inline double g_keyXq = kDefKeyXq;
inline double g_keyXqFly = kDefKeyXq;

// TRIED AND REVERTED (2026-08-03): one bit per nearby portal in the key,
// saying "is this state lined up to fire it right now" (the step's own y-box
// test plus "would it change anything"). The proposal came from lv18's
// x=20,386.6 (70% of the level), where the plan sits at y=321.000 for 47 ticks
// with the player's bottom at 312.00 against a RegularSize portal whose top
// edge is 311.99 -- it misses the portal by 0.01 px and stays mini. The theory
// was that the two worlds (touch / miss) are both still mini when they split,
// differ by 0.01 px in y, and land in the same 2 px ship bin, so the dedupe
// kills the branch before `mini` can ever tell them apart.
// MEASURED, and the theory is wrong: the branch is NOT being killed. At the
// anchor t=14,200 (x=20,183, from the driver's own GD dump) the frontier at
// t=14,500 already holds 416 states that took the portal stack WITHOUT the
// bits; with them it holds 312, and both builds report SOLVED to x=29,151 from
// there. Cold from t=0 both builds die at the same tick and the same x
// (t=18,330, x=24,721.0) and the arena grows 0.25%.
// (The 17-level cold regression it was run against came back 16/17 with lv9
// STUCK at x=21,225, but that is NOT this change's doing -- lv9 fails at the
// same x on the build without the bits. It is a pre-existing break.)
// Reverted for having no measured benefit, on the same grounds as the
// `s.action` experiment recorded at the end of the key below. What actually
// happens at that portal: the touching branch exists, the driver just never
// re-anchors far enough back to take it.

// GD's `upsideDown` for a state whose `flip` is the model's frame-local sign.
// `toFrame` gives frame 1 v = +X and frame 3 v = -X, so frame 3 is the mirror.
// Measured on lv22 with the trace's own flip/frame columns against GD's dump:
// the two agree everywhere up to t=4,664 and are opposite for the whole frame-3
// section. Only the places that read or write GD's own statement -- the gravity
// portals -- convert; the physics keeps using `flip`.
inline uint8_t gdUpOf(const State& s) {
    return (s.frame == 3) ? (uint8_t)!s.flip : s.flip;
}

// A HELD state near a toggle block it has not fired (g_pressWin, triggers.hpp). Frame 0 only:
// the windows are frame-0 x ranges, and the one block that needs this (SubZero 4003's uid 4001)
// sits in frame 0.
inline bool nearPressBox(const State& s) {
    if (g_pressWin.empty() || s.frame != 0) return false;
    const double x = (double)s.xAbs;
    for (const PressWin& w : g_pressWin)
        if (x >= w.lo && x <= w.hi && !s.trig.test(w.bit)) return true;
    return false;
}
// Whether the flight band clamps this state: the modes whose clamps read it (step.hpp's
// ship/UFO/swing, ball/spider and wave branches), for either body of a dual. A cube and a robot
// never read it, and GD's pmin/pmax there are camera state that carries the run's history, so two
// flights of one plan record different values (SubZero 4001, 2026-09-26). The loop records the band
// only around these modes (repair.hpp) and the key and the cap classes use it only here.
inline bool bandClamps(const State& s) {
    auto m = [](int k) { return k == 1 || k == 2 || k == 3 || k == 4 || k == 6 || k == 7; };
    return m(s.mode) || (s.dual && m(s.mode2));
}
// Named, quantised dimensions of a cell. Group identity (dx/trig/frame/rev)
// remains outside this key. Equality never compares a digest or struct padding.
struct SearchKey {
    int32_t x = 0, y = 0, vy = 0, y2 = 0, vy2 = 0;
    int32_t bandFloor = 0, bandHeight = 0, slopeUid0 = 0, slopeUidNow = 0;
    int64_t exitVy = 0;
    uint32_t taps = 0;
    uint8_t mode = 0, mini = 0, flip = 0, dual = 0, mode2 = 0, mini2 = 0, flip2 = 0;
    uint8_t ringHold = 0, pressSpent = 0, ringHold2 = 0, pressSpent2 = 0;
    uint8_t hover = 0, dash = 0, action = 0, jumpBuf = 0;
    uint8_t slopeT = 0, slopeT2 = 0, landed = 0, landed2 = 0;
    uint8_t flap = 0, noTerm = 0, boost = 0, boost2 = 0;
    uint8_t fgArm = 0, ogLinger = 0, holdDead = 0, armed = 0, spiderAge = 0;
    uint8_t coins = 0, items = 0, frameChg = 0, ceilT = 0, ceilM4 = 0;
    uint8_t held = 0, grounded = 0;
    uint8_t dash2 = 0, spiderTap = 0, a1cLatch = 0, seat = 0, freshArm = 0;
    std::array<uint64_t, GravLatch::kWords> portals{}, portals2{};
    uint64_t pressFired = 0, pressHeld = 0;
    // Zero means at rest; a live bucket is fireB/4 + 1, including tick zero.
    std::array<uint16_t, kTouchBits> movingFire{};

    // Keep comparison, hashing and trace encoding on the same field list.
    template <class Self>
    static auto fields(Self& k) {
        return std::tie(k.x, k.y, k.vy, k.y2, k.vy2, k.bandFloor, k.bandHeight,
                        k.slopeUid0, k.slopeUidNow, k.exitVy, k.taps,
                        k.mode, k.mini, k.flip, k.dual, k.mode2, k.mini2, k.flip2,
                        k.ringHold, k.pressSpent, k.ringHold2, k.pressSpent2,
                        k.hover, k.dash, k.action, k.jumpBuf, k.slopeT, k.slopeT2,
                        k.landed, k.landed2, k.flap, k.noTerm, k.boost, k.boost2,
                        k.fgArm, k.ogLinger, k.holdDead, k.armed, k.spiderAge,
                        k.coins, k.items, k.frameChg, k.ceilT, k.ceilM4,
                        k.held, k.grounded, k.dash2, k.spiderTap, k.a1cLatch, k.seat,
                        k.freshArm, k.portals, k.portals2,
                        k.pressFired, k.pressHeld, k.movingFire);
    }
    // Compare all canonical dimensions, not the storage representation.
    bool operator==(const SearchKey& b) const { return fields(*this) == fields(b); }
    // Negate the same complete equality used by the dedupe tables.
    bool operator!=(const SearchKey& b) const { return !(*this == b); }
    // Order full keys for the cell-cap and clearance diagnostics.
    bool operator<(const SearchKey& b) const { return fields(*this) < fields(b); }
};

// Visit integer fields and array elements without inspecting padding.
template <class Key, class Fn>
inline void keyWords(Key& k, Fn fn) {
    auto visit = [&](auto& field) {
        using T = std::remove_cv_t<std::remove_reference_t<decltype(field)>>;
        if constexpr (std::is_integral_v<T>) fn(field);
        else for (auto& word : field) fn(word);
    };
    std::apply([&](auto&... field) { (visit(field), ...); }, SearchKey::fields(k));
}

// Hashes select buckets only; complete equality resolves every collision.
struct SearchKeyHash {
    // Mix the canonical field list in order; no hash value means "empty".
    size_t operator()(const SearchKey& k) const {
        uint64_t h = 0xCBF29CE484222325ull;
        keyWords(k, [&](auto v) {
            uint64_t x = (uint64_t)v;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            h = (h ^ (x ^ (x >> 31))) * 0x100000001B3ull;
        });
        return (size_t)h;
    }
};

// Versioned full keys let rejoinfull reject legacy digest-only traces.
inline std::string keyText(const SearchKey& k) {
    std::ostringstream out;
    out << "v1" << std::hex;
    keyWords(k, [&](auto v) { out << ':' << (uint64_t)v; });
    return out.str();
}

// Decode exactly one canonical full key; truncated, extra and old keys fail.
inline bool parseKeyText(const std::string& text, SearchKey& result) {
    std::istringstream in(text);
    std::string word;
    if (!std::getline(in, word, ':') || word != "v1") return false;
    SearchKey k;
    bool ok = true;
    keyWords(k, [&](auto& v) {
        if (!ok || !std::getline(in, word, ':') || word.empty()) { ok = false; return; }
        uint64_t n = 0;
        const auto r = std::from_chars(word.data(), word.data() + word.size(), n, 16);
        if (r.ec != std::errc{} || r.ptr != word.data() + word.size()) { ok = false; return; }
        v = (std::remove_reference_t<decltype(v)>)n;
    });
    if (!ok || keyText(k) != text) return false;
    result = k;
    return true;
}

// Preserve the existing grid and conditional dimensions; t is the owning layer.
inline SearchKey keyOf(const State& s, long long t) {
    SearchKey k;
    const bool flying = s.mode == 1 || s.mode == 3;
    const double ys = flying ? g_shipYq : g_cubeYq;
    const double vs = flying ? g_shipVq : g_cubeVq;
    const bool flyX = flying || s.mode == 4 || s.mode == 7;
    k.x = (int32_t)std::lround(s.xAbs * (flyX ? g_keyXqFly : g_keyXq));
    k.y = (int32_t)std::lround(s.y * ys);
    k.vy = (int32_t)std::lround(s.vy * vs);
    k.mode = s.mode; k.mini = s.mini; k.flip = s.flip; k.dual = s.dual;
    k.flip2 = s.flip2;
    k.ringHold = s.ringHold; k.pressSpent = s.pressSpent;
    if (s.dual) {
        const double q = s.freeHalf ? g_dualFreeQ : 1.0;
        k.y2 = (int32_t)std::lround(s.y2 * (ys * q));
        k.vy2 = (int32_t)std::lround(s.vy2 * (vs * q));
        k.mode2 = s.mode2; k.mini2 = s.mini2;
        // Each body consumes its own press, even though the button is shared.
        k.ringHold2 = s.ringHold2; k.pressSpent2 = s.pressSpent2;
        k.boost2 = s.boost2;
        if (s.onSlope2) { k.slopeT2 = s.slopeT2; k.landed2 = s.rideLanded2; }
    }
    if (bandClamps(s)) {
        k.bandFloor = (int32_t)std::lround(s.bandFloor);
        if ((s.bandBranch & 3) == 1)
            k.bandHeight = (int32_t)std::lround(
                kBandRetracted - (kBandRetracted - ((double)s.bandCeil - (double)s.bandFloor))
                                 * bandProgress(s.bandAnim));
    }
    k.hover = s.rHover; k.dash = s.dashing;
    k.dash2 = s.dual && s.dashing2;
    if (s.rHover || s.dashing || k.dash2) k.action = s.action;
    k.spiderTap = s.pSpiderTap;
    k.a1cLatch = (s.a1cLatch & 1u) | (s.dual ? (s.a1cLatch & 2u) : 0u);
    k.seat = s.seatT > 0;
    k.freshArm = s.armT == 0;
    if (g_airPress && s.mode == 0) k.jumpBuf = s.jumpBuf;
    if (s.onSlope) {
        k.slopeT = s.slopeT; k.landed = s.rideLanded;
        k.slopeUid0 = s.slopeUid0; k.slopeUidNow = s.slopeUidNow;
    }
    k.flap = s.pFlap; k.noTerm = s.pNoTerm; k.boost = s.boost;
    k.exitVy = (int64_t)((double)s.pExitVy * 1000.0);
    k.fgArm = s.fgArm; k.ogLinger = s.ogLinger; k.holdDead = s.holdDead;
    k.armed = s.armT < kArmTicks;
    if (s.spiderJumpT < kSpiderJumpGraceTicks) k.spiderAge = s.spiderJumpT + 1;
    k.coins = s.coins; k.items = (uint8_t)popCount32(s.items); k.taps = s.taps;
    k.frameChg = s.frameChg; k.ceilT = s.ceilT;
    if (s.ceilT) k.ceilM4 = s.ceilM4;
    if (!g_heldKeyRead || s.dual || flyX) k.held = s.held;
    k.grounded = s.grounded;
    for (int w = 0; w < GravLatch::kWords; ++w) {
        k.portals[w] = s.portalLatch.word(w);
        k.portals2[w] = s.portalLatch2.word(w);
    }
    // A fired box and a pending hold differ before next tick's trig grouping.
    if (s.frame == 0) {
        for (const PressWin& w : g_pressWin) {
            if (s.xAbs < w.lo || s.xAbs > w.hi) continue;
            const uint64_t bit = uint64_t{1} << w.bit;
            if (s.trig.test(w.bit)) k.pressFired |= bit;
            else if (s.action) k.pressHeld |= bit;
        }
    }
    // Separate fire buckets matter only while their own motion is still live.
    const size_t n = std::min<size_t>(g_touchMoveTicks.size(), kTouchBits);
    for (size_t b = 0; b < n; ++b) {
        if (!s.trig.test(b) || g_touchMoveTicks[b] <= 0
            || t - (long long)s.fireB[b] >= g_touchMoveTicks[b]) continue;
        if (g_keyCensus) ++g_keyCount[b];
        k.movingFire[b] = (uint16_t)((s.fireB[b] >> 2) + 1);
    }
    return k;
}

// Dense keys keep the sparse bucket array small. Empty buckets have an explicit
// index sentinel, so a zero hash or a completely zero key is a normal cell.
template <class Value, class Hash = SearchKeyHash>
struct SearchKeyMap {
    std::vector<uint32_t> buckets;
    std::vector<uint32_t> touched;
    std::vector<SearchKey> keys;
    std::vector<Value> vals;
    static constexpr uint32_t empty = UINT32_MAX;

    // Rehash every occupied entry without relying on its digest being nonzero.
    void rehash(size_t n) {
        buckets.assign(n, empty);
        touched.clear();
        for (uint32_t j = 0; j < (uint32_t)keys.size(); ++j) {
            size_t i = Hash{}(keys[j]) & (n - 1);
            while (buckets[i] != empty) i = (i + 1) & (n - 1);
            buckets[i] = j;
            touched.push_back((uint32_t)i);
        }
    }
    // Reserve at most a quarter load, retaining the current group when growing.
    void ensure(size_t want) {
        size_t n = 1024;
        while (n < want * 4) n <<= 1;
        if (n > buckets.size()) rehash(n);
    }
    // Discard one group; the next lookup can reuse all allocated storage.
    void clear() {
        for (uint32_t i : touched) buckets[i] = empty;
        touched.clear();
        keys.clear(); vals.clear();
    }
    // Report actual occupied cells for the existing search diagnostics.
    size_t size() const { return keys.size(); }
    // Report the bucket allocation, independently of dense key storage.
    size_t bucket_count() const { return buckets.size(); }
    // Account for the larger structured keys in the existing memory limit.
    size_t storageBytes() const {
        return (buckets.capacity() + touched.capacity()) * sizeof(uint32_t)
               + keys.capacity() * sizeof(SearchKey) + vals.capacity() * sizeof(Value);
    }
    // Resolve a hash collision by comparing every canonical field.
    Value& at(const SearchKey& k, Value initial = Value{}) {
        if ((keys.size() + 1) * 4 > buckets.size())
            rehash(buckets.empty() ? 1024 : buckets.size() * 2);
        size_t i = Hash{}(k) & (buckets.size() - 1);
        while (buckets[i] != empty) {
            const uint32_t j = buckets[i];
            if (keys[j] == k) return vals[j];
            i = (i + 1) & (buckets.size() - 1);
        }
        buckets[i] = (uint32_t)keys.size();
        touched.push_back((uint32_t)i);
        keys.push_back(k); vals.push_back(initial);
        return vals.back();
    }
};

// --groupfire (off): the search's speed groups are split by WHEN each box whose chain is still
// moving was entered, not only by which boxes were.
//
// A group places its moving geometry once for every member, from the LATEST fire tick of each
// box (gFireB, cli.hpp). That is conservative for a door, which is less open the later it was
// punched, and the opposite for a box that brings something DOWN onto the player: the member that
// punched it first is shown the object where the last one would have it. Measured on lv22's switch
// band at 1x (2026-09-28, anchor t=2,423): a plan that grazed the red box at x=3,401 passed the
// search alive, while its own witness walk -- the same inputs, placed from the state's own fire
// tick -- died on the ceiling at t=2,756, and GD killed it at 2,754.
//
// The key already separates these states (the fire-tick term above), so splitting the group merges
// nothing new; it only stops them sharing one placement. Exact ticks, not the key's 4-tick bucket,
// which would leave up to three ticks of the same error. A box at rest drops out, as it does from
// the key: its chain has finished the same way for everyone.
inline bool g_groupFire = false;
// --groupxsplit (off): a speed group is also split where its members' x leave a gap across which
// the two parts' object windows cannot meet, so each part builds its windows from its own x span.
// The widest window margin is the speed portals' 80 px (cli.hpp, the group's windows), so the
// windows [lo - 80, hi + dx + 80] of two parts are disjoint exactly when the gap between them is
// wider than 2 * 80 + |dx| (kGroupWinMargin). Nothing narrower is split: members that could still
// share an object stay one group, as before.
//
// The group key holds dx but not x, on the reasoning that members of one speed differ only by
// stair snaps. That stops holding once two populations have taken different speed histories and
// then come back to the same dx: they share the key, one window spans both, and every step walks
// the objects of the whole gap. Measured on lv16's heavy call (--start 3961, all flags): a lineage
// that the --ufolawflap flap kept alive ran at 1.30 while the rest ran at 1.61, both then took the
// 1.05 portal, and from t=19,500 one group spanned 1,700 px (x 31,374, xlo 29,663); those 1,500
// ticks cost 74 s against 5 s without the flag, with 4.8% more steps in the whole call.
//
// Each part's window still covers every member (it is built from that part's own lo..hi), so no
// state loses an object it could reach. What changes when a split happens is the group-level
// bookkeeping that is per part now: the latest fire tick and lock offset each part places its
// geometry from, the order the parts are emitted in, and their separate dedupe maps. Off, or on
// with no gap over the threshold, every state is part 0 and the layer is processed as before.
inline bool g_groupXSplit = false;
constexpr double kGroupWinMargin = 80.0;    // the widest of the group's window margins (speeds)
inline long long g_groupXSplitLayers = 0;   // layers where some group split (printed at the end)
inline int g_groupXSplitMax = 0;            // the most parts one group split into
inline uint64_t groupFireSig(const State& s, long long t) {
    if (!g_groupFire || !s.trig) return 0;
    uint64_t k = 0;
    const size_t n = std::min<size_t>(g_touchMoveTicks.size(), (size_t)kTouchBits);
    for (size_t b = 0; b < n; ++b) {
        if (!s.trig.test(b)) continue;
        const long long moving = g_touchMoveTicks[b];
        if (moving <= 0 || t - (long long)s.fireB[b] >= moving) continue;
        uint64_t h = ((uint64_t)(b + 1) << 32) ^ (uint64_t)s.fireB[b];
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
        h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
        k ^= h ^ (h >> 31);
    }
    return k;
}

}  // namespace dp
