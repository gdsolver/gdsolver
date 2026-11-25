#pragma once
// P7-0 (restructure draft section 7.16): the canonical serializer of a State and layerdigest v2.
//
// v1 (--layerdigest) hashes a PROJECTION of each state (AUD-856). v2 serializes EVERY field of the State in a fixed order, so that "the layer is bit-for-bit the same set of states" can be said and a
// differing field can be named. What is NOT in it, and why:
//   - State::parent (an arena index: it is renumbered when the arena is compacted, so it differs between runs that are the same search). The lineage is checked by the plan and the trace md5, not here.
//   - State::evgate (the digest of the game events a lineage raised for the level's Event triggers, 11-19): skipped until the search reads it, so a layer of a level with no Event trigger serializes as it did. The next change that
//     reads it makes it a serialized field (a version step of the record: fields.json "version").
//   - padding between members (never written; the tiling check below says exactly how many bytes there are).
//   - the pointers' addresses: a pointer to an Obj is written as that object's uid (-1 for null), the spill chain as the uids along it. (An Obj without a uid column has uid -1: two such objects are not told apart.)
//   - slRef / slRef2 (handles into SlStore, numbered by allocation order): written as gdcol::slKeyBitsOf(ref), what the search tells apart; the rest of a shadow block is not serialized (only under --shadowstep).
// The order of the states in a layer does not matter: records are sorted by their bytes (lexicographic) before they are hashed or dumped, so the digest is a function of the SET. Floats are written as their raw
// bits, so +0.0 and -0.0 differ; a NaN is counted (printed at the end), not refused. Little-endian host.
//
// THE FIELD TABLE is DP_STATE_FIELDS below, one row per member of State in declaration order: kind 'i' bytes as they are, 'f' float(s), 'P' pointer(s) to Obj, 'S' the spill chain, 'L' a shadow-store handle,
// 'K' skipped. tools/check_state_serial.py compares it with the members of struct State in state.hpp (run it after touching State), and the static_assert below fails when the members' sizes plus the padding
// stop adding up to sizeof(State), so a member added without a row is caught twice.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dp/state.hpp"
#include "dp/speed.hpp"

namespace dp {
namespace stser {

#define DP_STATE_FIELDS(X) \
    X('f', y) \
    X('f', vy) \
    X('i', mode) \
    X('i', held) \
    X('i', grounded) \
    X('i', flip) \
    X('f', xAbs) \
    X('P', snapObj) \
    X('f', snapDist) \
    X('f', dx2) \
    X('P', usedOrb) \
    X('i', ringHold) \
    X('i', pressSpent) \
    X('i', onSlope) \
    X('i', slopeT) \
    X('i', rideLanded) \
    X('i', seatT) \
    X('i', prevC6q) \
    X('f', slopeM) \
    X('f', lawSv) \
    X('f', followF) \
    X('i', followOk) \
    X('f', halfPrev) \
    X('f', halfPrev2) \
    X('i', followOwn) \
    X('i', followN) \
    X('i', tpArrive) \
    X('i', ctrlT) \
    X('i', rev2) \
    X('i', seatSpd) \
    X('i', gravTouch) \
    X('i', tpTouch) \
    X('i', pcRel) \
    X('i', pcTouch) \
    X('i', revFired) \
    X('i', flipT) \
    X('i', armT) \
    X('i', modeT) \
    X('i', slideT) \
    X('i', slideT2) \
    X('i', spiderJumpT) \
    X('f', bandFloor) \
    X('f', bandCeil) \
    X('i', bandAnim) \
    X('i', bandBranch) \
    X('f', bandRefY) \
    X('f', dx) \
    X('i', trig) \
    X('i', trigT) \
    X('i', tpMarkA) \
    X('i', portalLatch) \
    X('i', portalLatch2) \
    X('i', fireB) \
    X('f', lockOff) \
    X('i', rotSpent) \
    X('i', rotChan) \
    X('i', tpSkip) \
    X('i', ceilLatch) \
    X('i', rotRev) \
    X('i', pFlap) \
    X('i', pSpiderTap) \
    X('i', pNoTerm) \
    X('i', boost) \
    X('i', ceilMode) \
    X('i', tpBack) \
    X('f', pExitVy) \
    X('f', tpFromX) \
    X('i', slopeUid0) \
    X('i', slopeUidNow) \
    X('i', dual) \
    X('i', freeHalf) \
    X('i', seatT2) \
    X('i', stickFb) \
    X('f', y2) \
    X('f', vy2) \
    X('f', slopeM2) \
    X('f', snapDist2) \
    X('i', grounded2) \
    X('i', flip2) \
    X('i', ringHold2) \
    X('i', onSlope2) \
    X('i', pressSpent2) \
    X('i', slopeT2) \
    X('i', rideLanded2) \
    X('i', boost2) \
    X('i', mode2) \
    X('i', ceilT2) \
    X('i', ceilM42) \
    X('i', mini2) \
    X('i', slopeUid02) \
    X('i', slopeUidNow2) \
    X('i', tpMarkB) \
    X('P', snapObj2) \
    X('P', usedOrb2) \
    X('P', usedPad2) \
    X('P', usedPad) \
    X('i', mini) \
    X('i', rHover) \
    X('i', dashing) \
    X('i', action2) \
    X('f', dashSlope) \
    X('i', frame) \
    X('i', frameChg) \
    X('i', ceilT) \
    X('i', ceilM4) \
    X('f', rot) \
    X('f', rotPrev) \
    X('i', rotNeg) \
    X('i', ceilPin) \
    X('i', fgArm) \
    X('i', coins) \
    X('i', taps) \
    X('i', items) \
    X('K', parent) \
    X('i', action) \
    X('i', latLock) \
    X('i', rev) \
    X('i', tight) \
    X('i', jumpBuf) \
    X('f', rotStep) \
    X('i', groundUid) \
    X('f', rotGm) \
    X('i', usedOrbOld) \
    X('i', usedOrbOld2) \
    X('i', ogLinger) \
    X('i', holdDead) \
    X('i', holdDead2) \
    X('i', jumpBuf2) \
    X('i', usedOrbHist) \
    X('i', usedOrbHist2) \
    X('i', portSeen) \
    X('i', portSeen2) \
    X('i', touchRing) \
    X('i', touchRing2) \
    X('i', touchRingT) \
    X('i', touchRingT2) \
    X('i', edgeAge) \
    X('i', rotBoost) \
    X('i', spdFired) \
    X('i', spdFired2) \
    X('i', hitG) \
    X('i', hitG2) \
    X('i', rotNeg2) \
    X('i', a1cLatch) \
    X('i', dashing2) \
    X('i', flipT2) \
    X('i', modeT2) \
    X('i', rHover2) \
    X('i', ball728) \
    X('i', dashNeg) \
    X('i', dashF) \
    X('i', dashAng) \
    X('f', rot2) \
    X('f', rotStep2) \
    X('f', dashSlope2) \
    X('i', stickSlope) \
    X('i', spdMore) \
    X('i', spdMore2) \
    X('f', revOff) \
    X('f', revCarry) \
    X('S', spdSpill) \
    X('i', xcId) \
    X('i', xcLag) \
    X('i', xcOver) \
    X('i', tapsX) \
    X('i', gdHS) \
    X('i', gdHSb) \
    X('i', gdAux) \
    X('i', gdHS2) \
    X('L', slRef) \
    X('L', slRef2) \
    X('i', dxHist) \
    X('i', tog) \
    X('i', xgate) \
    X('f', bandOffY) \
    X('K', evgate) \
    X('K', evKey) \
    X('K', tapN) \
    X('K', tapAge) \
    X('K', tapWin) \
    X('K', prevWX) \
    X('K', prevWY)

struct Field { const char* name; char kind; uint32_t size; };
inline const std::vector<Field>& fields() {
    static const std::vector<Field> f = {
#define X(k, n) {#n, k, (uint32_t)sizeof(State::n)},
        DP_STATE_FIELDS(X)
#undef X
    };
    return f;
}
constexpr size_t kCoveredBytes = 0
#define X(k, n) + sizeof(State::n)
    DP_STATE_FIELDS(X)
#undef X
    ;
constexpr size_t kStatePadBytes = 37;   // (+4: tapWin's tail, 20 bytes of evKey / tapN / tapWin in a State that grew by 24) bytes the table does not cover: 21 of alignment padding (12 still empty; revFired took 2; --camy's camTwStart 4, camPortal 1, camY 4 sit in the rest, with no row of their own) + 8 for --camy's camDy / camTan at the end (bandOffY, --camyband branch B, took 4 of the 8 bytes the State grew by and has a row: the 4 left are alignment)
#ifndef DP_STATE_SERIAL_PROBE
static_assert(kCoveredBytes + kStatePadBytes == sizeof(State),
              "State's members and the padding no longer add up to sizeof(State): a member was added, removed or resized. Add its row to DP_STATE_FIELDS (dp/state_serial.hpp), run tools/check_state_serial.py, "
              "and set kStatePadBytes to sizeof(State) - kCoveredBytes.");
#endif

struct Stats { long long states = 0, nanFields = 0; };
inline Stats g_serialStats;
struct Report { ~Report() { if (g_serialStats.states) std::printf("layerdigest2: %lld states serialized, %lld NaN float fields\n", g_serialStats.states, g_serialStats.nanFields); } };
inline Report g_report;

inline void put(std::vector<uint8_t>& o, const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; o.insert(o.end(), b, b + n); }
inline void putU32(std::vector<uint8_t>& o, uint32_t v) { put(o, &v, 4); }
inline void putI32(std::vector<uint8_t>& o, int32_t v) { put(o, &v, 4); }
inline void putPtr(std::vector<uint8_t>& o, const Obj* p) { putI32(o, p ? (int32_t)p->uid : -1); }

template <char K> struct Ser;
template <> struct Ser<'i'> { static void run(std::vector<uint8_t>& o, const void* p, size_t n) { put(o, p, n); } };
template <> struct Ser<'f'> {
    static void run(std::vector<uint8_t>& o, const void* p, size_t n) {
        const float* f = (const float*)p;
        for (size_t i = 0; i < n / 4; ++i) if (std::isnan(f[i])) ++g_serialStats.nanFields;
        put(o, p, n);
    }
};
template <> struct Ser<'P'> {
    static void run(std::vector<uint8_t>& o, const void* p, size_t n) {
        const Obj* const* a = (const Obj* const*)p;
        for (size_t i = 0; i < n / sizeof(const Obj*); ++i) putPtr(o, a[i]);
    }
};
template <> struct Ser<'S'> {
    static void run(std::vector<uint8_t>& o, const void* p, size_t) {
        const SpdSpillNode* n = *(const SpdSpillNode* const*)p;
        std::vector<int32_t> uids;
        for (; n && uids.size() < 64; n = n->prev) uids.push_back(n->uid);
        putU32(o, (uint32_t)uids.size());
        for (int32_t u : uids) putI32(o, u);
    }
};
template <> struct Ser<'L'> {
    static void run(std::vector<uint8_t>& o, const void* p, size_t) { const uint64_t k = gdcol::slKeyBitsOf(*(const uint32_t*)p); put(o, &k, 8); }
};
template <> struct Ser<'K'> { static void run(std::vector<uint8_t>&, const void*, size_t) {} };

// one state -> its record (appended to `o`)
inline void serializeState(const State& s, std::vector<uint8_t>& o) {
    ++g_serialStats.states;
#define X(k, n) Ser<k>::run(o, &s.n, sizeof(s.n));
    DP_STATE_FIELDS(X)
#undef X
}

// the byte range of every field in a record (the table's order; the spill chain is variable): for comparing two records field by field
inline void fieldSlices(const std::vector<uint8_t>& rec, std::vector<std::pair<uint32_t, uint32_t>>& out) {
    out.clear();
    uint32_t off = 0;
    for (const Field& fl : fields()) {
        uint32_t len = 0;
        switch (fl.kind) {
            case 'i': case 'f': len = fl.size; break;
            case 'P': len = (uint32_t)(fl.size / sizeof(const Obj*) * 4); break;
            case 'L': len = 8; break;
            case 'S': { uint32_t c = 0; if (off + 4 <= rec.size()) std::memcpy(&c, rec.data() + off, 4); len = 4 + 4 * c; break; }
            default: len = 0; break;
        }
        out.push_back({off, len});
        off += len;
    }
}

// 128 bits from two differently seeded 64-bit streams (a digest for comparing runs, not a cryptographic hash)
struct Dig128 {
    uint64_t a = 0x243F6A8885A308D3ULL, b = 0x13198A2E03707344ULL, n = 0;
    static uint64_t rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
    void word(uint64_t v) {
        a = (a ^ v) * 0x9E3779B97F4A7C15ULL; a ^= a >> 32; a = rotl(a, 27) * 0xC2B2AE3D27D4EB4FULL;
        b = (b + v) * 0xFF51AFD7ED558CCDULL; b = rotl(b, 31) ^ (b >> 29); b *= 0x94D049BB133111EBULL;
        ++n;
    }
    void bytes(const uint8_t* p, size_t len) {
        word((uint64_t)len);
        size_t i = 0;
        for (; i + 8 <= len; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); word(v); }
        if (i < len) { uint64_t v = 0; std::memcpy(&v, p + i, len - i); word(v); }
    }
    void finish() { word(n); word(~n); }
};

// the layer: every record, sorted by bytes. `dig` is hashed over the sorted records, `recs` (if non-null) receives them.
inline void layerRecords(const std::vector<State>& layer, std::vector<std::vector<uint8_t>>& recs) {
    recs.clear(); recs.reserve(layer.size());
    for (const State& s : layer) { recs.emplace_back(); recs.back().reserve(768); serializeState(s, recs.back()); }
    std::sort(recs.begin(), recs.end());   // vector<uint8_t> < is lexicographic, a shorter prefix first
}
inline Dig128 layerDigest(const std::vector<std::vector<uint8_t>>& recs) {
    Dig128 d;
    d.word((uint64_t)recs.size());
    for (const auto& r : recs) d.bytes(r.data(), r.size());
    d.finish();
    return d;
}

// the layout of one record for tools/layer_diff.py: name, kind, and the byte length of a fixed field (0 = variable: the spill chain is u32 count + count x i32)
inline void writeFieldsJson(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "{\"version\": 2, \"state_bytes\": %zu, \"fields\": [\n", sizeof(State));
    bool first = true;
    for (const Field& fl : fields()) {
        int len = 0;
        switch (fl.kind) {
            case 'i': case 'f': len = (int)fl.size; break;
            case 'P': len = (int)(fl.size / sizeof(const Obj*) * 4); break;
            case 'L': len = 8; break;
            case 'S': len = 0; break;
            default: len = 0; break;
        }
        std::fprintf(f, "%s  {\"name\": \"%s\", \"kind\": \"%c\", \"bytes\": %d}", first ? "" : ",\n", fl.name, fl.kind, len);
        first = false;
    }
    std::fprintf(f, "\n]}\n");
    std::fclose(f);
}

}  // namespace stser
}  // namespace dp