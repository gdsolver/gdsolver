#pragma once
// --dumpstate / --start-state: a model state handed from one call to the next, whole.
//
// An anchor built from the game's recording carries what the dump has a column for (--start's
// fields, --anchor-state's payload, the spent and touched lists, the rotation queue's seed). What it
// cannot carry is the model's own history: the rings and pads a body has used, each touch box's fire
// tick, the x-crossing and rotation triggers' fire state, the stick tie, the moving geometry's last
// placement. A call that wants to go on from where the MODEL stood -- the loop re-anchoring on its own
// replay instead of the game's -- needs all of it, and an anchor that restarts the model where it
// stood must reproduce the uninterrupted replay to the bit from there on.
//
// The file is binary and private to one build: State is copied as it is (sizeof checked), its twelve
// Obj pointers as (frame, list, index) with the object's uid beside it to check the decode, the
// trigger fire state by index (uid checked), and the moving geometry of the state's frame as
// Dynamics::saveCarry leaves it. Written by the --replay walk after the tick it names; read once,
// after the call's own setup, in place of the anchor's seeding (cli.hpp).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dp/model.hpp"

namespace dp {

// --dumpstate <tick>:<path>: the --replay walk writes its state after that tick to the file.
inline long long g_dumpStateT = -1;
inline std::string g_dumpStatePath;
// --start-state <path>: the call starts from the state in the file (its tick is the call's t0, which
// --start must name too: the call's setup reads --start before this is applied).
inline std::string g_startStatePath;

struct StateFile {
    static constexpr uint32_t kMagic = 0x54534447u;   // "GDST"
    static constexpr uint32_t kVersion = 5;
    int64_t t = -1;
    float dxF = 0.f;
    State s{};
    uint64_t refs[12] = {};
    int32_t refUid[12] = {};
    std::vector<int32_t> autoUid, autoFireT, autoSpread;
    std::vector<double> autoFireX, autoLock;
    std::vector<int32_t> rotFired, rotRev;
    int32_t carryFrame = -1;
    int64_t carryT = -1;
    std::vector<double> carry;
    uint8_t preBtnSet = 0;
    double preBtnY = 0.0;
    // each touch box's level-wide first entry (dynamics.hpp g_touchFireT): the touch parts' clocks read it
    std::vector<int32_t> touchFireT;
    // ...and what the ORIGINAL call's anchor setup decided from the recording, which the placement keeps
    // reading (dynamics.hpp): the boxes still sliding at that anchor (g_recPhase), the recorded entry and
    // the recording's own bit per box, and the tick re-timing starts from. Set up again at the new --start
    // they would describe that tick's recording instead of the walk the state came from.
    TouchMask recPhase{};
    std::vector<int32_t> touchRecEntry, recTouchBit;
    int64_t touchRetimeFrom = 0;
    // the touch boxes' uids by bit: a call that windows its boxes from its own anchor (more boxes than
    // kTouchBits) numbers them differently, and every per-bit field is moved to this call's numbering
    std::vector<int32_t> touchUids;
    // The control-off windows the original call kept (t0:t1 flattened): --ctrltouch drops a window whose
    // start x, projected from the call's own anchor, lies over a Touch box, so set up again from another
    // tick the same --ctrlwin keeps another set (a custom level: the window 3,222:3,394 dropped from the
    // anchor at 1,894 and kept from 2,538, and the press at 3,258 jumped in one and not the other).
    std::vector<int64_t> ctrlWin, ctrlWin2;
};

// The twelve Obj pointers of a State, in a fixed order.
inline void stateObjSlots(State& s, const Obj** out[12]) {
    out[0] = &s.snapObj; out[1] = &s.usedOrb; out[2] = &s.snapObj2; out[3] = &s.usedOrb2;
    for (int k = 0; k < 4; ++k) out[4 + k] = &s.usedPad[k];
    for (int k = 0; k < 4; ++k) out[8 + k] = &s.usedPad2[k];
}

// (frame, list, index) of an object in one of the frame levels that exist, 0 for null; `ok` goes
// false for a pointer into none of them.
inline uint64_t stateEncodeObj(const Obj* p, int32_t& uid, bool& ok) {
    uid = -1;
    if (!p) return 0;
    for (int f = 0; f < 4; ++f) {
        Level* lv = levelOfFrame(f);
        if (!lv) continue;
        const std::vector<Obj>* ls[8];
        Model::objLists(*lv, ls);
        for (int k = 0; k < 8; ++k) {
            const std::vector<Obj>& v = *ls[k];
            if (!v.empty() && p >= v.data() && p < v.data() + v.size()) {
                uid = p->uid;
                return 1 + ((uint64_t)f << 60) + ((uint64_t)k << 56) + (uint64_t)(p - v.data());
            }
        }
    }
    ok = false;
    return 0;
}

inline const Obj* stateDecodeObj(Level& L0, uint64_t c, int32_t uid, bool& ok) {
    if (!c) return nullptr;
    c -= 1;
    const int f = (int)((c >> 60) & 3), k = (int)((c >> 56) & 15);
    const size_t i = (size_t)(c & ((1ull << 56) - 1));
    const std::vector<Obj>* ls[8];
    Model::objLists(frameLevel(L0, f), ls);
    if (k < 0 || k >= 8) { ok = false; return nullptr; }
    const std::vector<Obj>& v = *ls[k];
    if (i < v.size() && v[i].uid == uid) return &v[i];
    for (const Obj& o : v)   // the lists moved under it: the uid decides
        if (o.uid == uid) return &o;
    ok = false;
    return nullptr;
}

// The state after tick t and the world it stands in.
inline StateFile captureState(const State& s, long long t, float dxF, Level& L0, bool& ok) {
    StateFile f;
    f.t = t;
    f.dxF = dxF;
    f.s = s;
    const Obj** slots[12];
    stateObjSlots(f.s, slots);
    for (int k = 0; k < 12; ++k) {
        f.refs[k] = stateEncodeObj(*slots[k], f.refUid[k], ok);
        *slots[k] = nullptr;
    }
    for (const AutoTrig& A : g_autoTrig) {
        f.autoUid.push_back(A.uid);
        f.autoFireT.push_back(A.fireT);
        f.autoSpread.push_back(A.fireSpreadT);
        f.autoFireX.push_back(A.fireX);
        f.autoLock.push_back(A.lockLastX);
    }
    for (const RotTrig& r : g_rotTrig) {
        f.rotFired.push_back(r.firedT);
        f.rotRev.push_back(r.revT);
    }
    f.carryFrame = (int)s.frame & 3;
    f.carryT = t;
    frameLevel(L0, f.carryFrame).dyn.saveCarry(f.carry);
    f.preBtnSet = g_preBtnSet ? 1 : 0;
    f.preBtnY = g_preBtnY;
    f.touchFireT.assign(g_touchFireT.begin(), g_touchFireT.end());
    f.recPhase = g_recPhase;
    f.touchRecEntry.assign(g_touchRecEntry.begin(), g_touchRecEntry.end());
    f.recTouchBit.assign(g_recTouchBit.begin(), g_recTouchBit.end());
    f.touchRetimeFrom = g_touchRetimeFrom;
    for (const TouchTrig& T : g_touch) f.touchUids.push_back(T.uid);
    for (const auto& w : g_ctrlWin) { f.ctrlWin.push_back(w.first); f.ctrlWin.push_back(w.second); }
    for (const auto& w : g_ctrlWin2) { f.ctrlWin2.push_back(w.first); f.ctrlWin2.push_back(w.second); }
    return f;
}

namespace statefile_io {
template <class T> inline void put(std::FILE* o, const T& v) { std::fwrite(&v, sizeof v, 1, o); }
template <class T> inline void putVec(std::FILE* o, const std::vector<T>& v) {
    const uint64_t n = v.size();
    put(o, n);
    if (n) std::fwrite(v.data(), sizeof(T), (size_t)n, o);
}
template <class T> inline bool get(std::FILE* in, T& v) { return std::fread(&v, sizeof v, 1, in) == 1; }
template <class T> inline bool getVec(std::FILE* in, std::vector<T>& v) {
    uint64_t n = 0;
    if (!get(in, n) || n > (1ull << 32)) return false;
    v.resize((size_t)n);
    return n == 0 || std::fread(v.data(), sizeof(T), (size_t)n, in) == (size_t)n;
}
}   // namespace statefile_io

inline bool writeStateFile(const std::string& path, const StateFile& f) {
    using namespace statefile_io;
    std::FILE* o = std::fopen(path.c_str(), "wb");
    if (!o) return false;
    put(o, StateFile::kMagic);
    put(o, StateFile::kVersion);
    const uint64_t sz = sizeof(State);
    put(o, sz);
    put(o, f.t);
    put(o, f.dxF);
    std::fwrite(&f.s, sizeof(State), 1, o);
    std::fwrite(f.refs, sizeof f.refs, 1, o);
    std::fwrite(f.refUid, sizeof f.refUid, 1, o);
    putVec(o, f.autoUid); putVec(o, f.autoFireT); putVec(o, f.autoSpread);
    putVec(o, f.autoFireX); putVec(o, f.autoLock);
    putVec(o, f.rotFired); putVec(o, f.rotRev);
    put(o, f.carryFrame); put(o, f.carryT); putVec(o, f.carry);
    put(o, f.preBtnSet); put(o, f.preBtnY);
    putVec(o, f.touchFireT);
    put(o, f.recPhase); putVec(o, f.touchRecEntry); putVec(o, f.recTouchBit); put(o, f.touchRetimeFrom);
    putVec(o, f.touchUids);
    putVec(o, f.ctrlWin); putVec(o, f.ctrlWin2);
    const bool good = std::ferror(o) == 0;
    std::fclose(o);
    return good;
}

// `why` names the first thing that did not fit (empty when the file was read whole).
inline bool readStateFile(const std::string& path, StateFile& f, std::string& why) {
    using namespace statefile_io;
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) { why = "cannot open"; return false; }
    uint32_t magic = 0, ver = 0;
    uint64_t sz = 0;
    bool ok = get(in, magic) && get(in, ver) && get(in, sz);
    if (ok && magic != StateFile::kMagic) { why = "not a state file"; ok = false; }
    else if (ok && ver != StateFile::kVersion) { why = "another version"; ok = false; }
    else if (ok && sz != sizeof(State)) { why = "another build (sizeof State differs)"; ok = false; }
    if (ok) {
        ok = get(in, f.t) && get(in, f.dxF)
             && std::fread(&f.s, sizeof(State), 1, in) == 1
             && std::fread(f.refs, sizeof f.refs, 1, in) == 1
             && std::fread(f.refUid, sizeof f.refUid, 1, in) == 1
             && getVec(in, f.autoUid) && getVec(in, f.autoFireT) && getVec(in, f.autoSpread)
             && getVec(in, f.autoFireX) && getVec(in, f.autoLock)
             && getVec(in, f.rotFired) && getVec(in, f.rotRev)
             && get(in, f.carryFrame) && get(in, f.carryT) && getVec(in, f.carry)
             && get(in, f.preBtnSet) && get(in, f.preBtnY)
             && getVec(in, f.touchFireT)
             && get(in, f.recPhase) && getVec(in, f.touchRecEntry) && getVec(in, f.recTouchBit)
             && get(in, f.touchRetimeFrom) && getVec(in, f.touchUids)
             && getVec(in, f.ctrlWin) && getVec(in, f.ctrlWin2);
        if (!ok) why = "short file";
    }
    std::fclose(in);
    return ok;
}

// The parts of an applied file that a search moves on: the moving geometry of the state's frame and
// the pre-button record. A walk from the file's tick after the search puts them back first.
inline void restoreStateWorld(const StateFile& f, Level& L0) {
    if (f.carryFrame >= 0) {
        Dynamics& D = frameLevel(L0, f.carryFrame).dyn;
        if (f.carry.size() == D.objs.size()) {
            D.loadCarry(f.carry, (int)f.carryT);
            D.resetCursors();
        }
    }
    g_preBtnSet = f.preBtnSet != 0;
    g_preBtnY = f.preBtnY;
}

// Put the file's state into the call: the start State (its pointers decoded into this call's
// levels), the triggers' fire state, the moving geometry of its frame and the spider's pre-warp y.
// False, with `why`, when the level does not match the file (another level, or another set of
// triggers): nothing is half-applied then except the State itself, which the caller discards.
inline bool applyStateFile(const StateFile& f, Level& L0, State& init, float& dxF, std::string& why) {
    if (f.autoUid.size() != g_autoTrig.size()) { why = "the x-crossing triggers differ in number"; return false; }
    for (size_t a = 0; a < g_autoTrig.size(); ++a)
        if (g_autoTrig[a].uid != f.autoUid[a]) { why = "the x-crossing triggers differ"; return false; }
    if (f.rotFired.size() != g_rotTrig.size()) { why = "the rotation triggers differ in number"; return false; }
    State s = f.s;
    const Obj** slots[12];
    stateObjSlots(s, slots);
    bool ok = true;
    for (int k = 0; k < 12; ++k) *slots[k] = stateDecodeObj(L0, f.refs[k], f.refUid[k], ok);
    if (!ok) { why = "an object the state names is not in this level"; return false; }
    // The touch boxes by uid: file bit b -> this call's bit, -1 where this call did not load the box.
    std::vector<int> bitTo(f.touchUids.size(), -1);
    bool same = f.touchUids.size() == g_touch.size();
    {
        std::unordered_map<int, int> cur;
        for (size_t b = 0; b < g_touch.size(); ++b) cur[g_touch[b].uid] = (int)b;
        for (size_t b = 0; b < f.touchUids.size(); ++b) {
            const auto it = cur.find(f.touchUids[b]);
            bitTo[b] = it == cur.end() ? -1 : it->second;
            same = same && bitTo[b] == (int)b;
        }
    }
    if (!same) {
        int lost = 0;
        TouchMask trig{};
        uint16_t fireB[kTouchBits] = {};
        for (size_t b = 0; b < bitTo.size() && b < (size_t)kTouchBits; ++b) {
            if (!s.trig.test((int)b)) continue;
            if (bitTo[b] < 0) { ++lost; continue; }
            trig |= touchBit(bitTo[b]);
            fireB[bitTo[b]] = s.fireB[b];
        }
        s.trig = trig;
        std::copy(fireB, fireB + kTouchBits, s.fireB);
        std::printf("startstate: the touch boxes are numbered differently here (%zu in the file, %zu loaded); "
                    "%d fired box(es) not loaded in this call\n", f.touchUids.size(), g_touch.size(), lost);
    }
    // a per-box array of the file in this call's numbering (`blank` where nothing maps)
    auto moved = [&](const std::vector<int32_t>& v, int blank) {
        std::vector<int32_t> out((size_t)kTouchBits, blank);
        for (size_t b = 0; b < v.size() && b < bitTo.size(); ++b)
            if (bitTo[b] >= 0) out[(size_t)bitTo[b]] = v[b];
        if (same) return v;
        return out;
    };
    init = s;
    if (f.dxF > 0.f) dxF = f.dxF;
    for (size_t a = 0; a < g_autoTrig.size(); ++a) {
        AutoTrig& A = g_autoTrig[a];
        A.fireT = f.autoFireT[a];
        A.fireSpreadT = f.autoSpread[a];
        A.fireX = f.autoFireX[a];
        A.lockLastX = f.autoLock[a];
    }
    for (size_t r = 0; r < g_rotTrig.size(); ++r) {
        g_rotTrig[r].firedT = f.rotFired[r];
        g_rotTrig[r].revT = f.rotRev[r];
    }
    restoreStateWorld(f, L0);
    const std::vector<int32_t> fireT = moved(f.touchFireT, -1), recEntry = moved(f.touchRecEntry, -1),
                               recBit = moved(f.recTouchBit, -1);
    for (size_t b = 0; b < g_touchFireT.size() && b < fireT.size(); ++b) g_touchFireT[b] = fireT[b];
    for (size_t b = 0; b < g_touchRecEntry.size() && b < recEntry.size(); ++b) g_touchRecEntry[b] = recEntry[b];
    for (size_t b = 0; b < g_recTouchBit.size() && b < recBit.size(); ++b) g_recTouchBit[b] = recBit[b];
    if (same) {
        g_recPhase = f.recPhase;
    } else {
        g_recPhase = TouchMask{};
        for (size_t b = 0; b < bitTo.size() && b < (size_t)kTouchBits; ++b)
            if (f.recPhase.test((int)b) && bitTo[b] >= 0) g_recPhase |= touchBit(bitTo[b]);
    }
    g_touchRetimeFrom = f.touchRetimeFrom;
    auto wins = [](const std::vector<int64_t>& v) {
        std::vector<std::pair<long long, long long>> w;
        for (size_t k = 0; k + 1 < v.size(); k += 2) w.emplace_back((long long)v[k], (long long)v[k + 1]);
        return w;
    };
    g_ctrlWin = wins(f.ctrlWin);
    g_ctrlWin2 = wins(f.ctrlWin2);
    return true;
}

}   // namespace dp
