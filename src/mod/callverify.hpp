// serve command `call` (cfg callverify=1, off by default, observation only): GD's own function on a scratch object, for verifying a port.
//
// The shadow (dp/src/dp/collide.hpp, upd.hpp, ...) is a copy of GD's code; this asks GD itself. A caller (the MCP tool gd_call, gdmcp.sweep) writes data/call_in.txt:
//     fn <name>                         one of the functions in kAllowed
//     arg <name> <value>                dt (float), forced (0/1; collidedWithObjectInternal: its skipCheck)
//                                       noobj (0/1): collidedWithObjectInternal with a null object
//     rect <x> <y> <w> <h>            collidedWithObjectInternal: the cocos2d::CCRect it is called with
//     patch p <offset> <hex>            bytes laid over the scratch PlayerObject, after it is made
//     patch s <offset> <hex>            ... over the scratch slope GameObject
// and sends `call` through cmd.txt. The mod makes a NEW scratch PlayerObject (and slope object) for every call -- the state a call leaves is never read by the next, which is the lesson of the
// in-place reset that kept the previous values -- applies the patches, calls the function once and writes data/call_out.txt:
//     ok 1 | ok 0 <why>
//     ret <value>                       (functions that return one)
//     player_in <hex>, slope_in <hex>   the bytes the function is entered with (the scratch object as made, the patches over it): what to hand the port
//     player <hex>                      the PlayerObject's bytes after the call (sizeof(PlayerObject))
//     slope <hex>                       the slope object's bytes
//
// What keeps it away from the running game:
//   - it runs on the main thread, inside the layer's update, from the command poll (the same place every other serve command runs);
//   - the scratch player has no game layer (PlayerObject::create(1, 1, nullptr, <a scratch CCLayer on no scene>, false), and +0xc28 is cleared after the patches): the functions guard their layer calls on it
//     (spiderTestJump: `if (m_gameLayer != 0)`), so nothing reaches the PlayLayer; the real player and the level are not read or written;
//   - the scratch objects are not added to any node, so no particle, sound or trail is drawn;
//   - only the functions in kAllowed can be named. A function is added only after its decompile has been read for writes through the layer pointer and for the singleton
//     PlayLayer (collidedWithSlopeInternal reaches PlayLayer::destroyPlayer on a lethal slope contact; the caller keeps m_slopeIsHazard off).
#pragma once
#include <Geode/Geode.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace callverify {

inline bool g_on = false;   // cfg callverify=1

inline std::string toHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}

inline bool fromHex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.resize(s.size() / 2);
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < out.size(); ++i) {
        const int a = v(s[2 * i]), b = v(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}

inline void fail(const std::string& path, const std::string& why) {
    std::ofstream o(path, std::ios::trunc);
    o << "ok 0 " << why << "\n";
}

// Reads inPath, runs the one function, writes outPath. Returns the line for result.txt.
inline std::string run(const std::string& inPath, const std::string& outPath) {
    static bool busy = false;
    if (!g_on) { fail(outPath, "refused: cfg callverify=1 is off"); return "call: refused - cfg callverify=1 is off"; }
    if (busy) { fail(outPath, "refused: a call is running"); return "call: refused - reentered"; }
    struct Guard { Guard() { busy = true; } ~Guard() { busy = false; } } guard;

    std::ifstream in(inPath);
    if (!in) { fail(outPath, "no call_in.txt"); return "call: no input file"; }
    std::string fn;
    float dt = 0.25f;
    bool forced = false;
    bool noObj = false;   // collidedWithObjectInternal with a null object: the rect alone decides (the port's rect path)
    float rect[4] = {0.f, 0.f, 0.f, 0.f};
    struct Patch { char who; size_t off; std::vector<uint8_t> bytes; };
    std::vector<Patch> patches;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string k;
        ss >> k;
        if (k == "fn") ss >> fn;
        else if (k == "arg") {
            std::string name, val;
            ss >> name >> val;
            if (name == "dt") dt = (float)std::atof(val.c_str());
            else if (name == "forced") forced = val == "1";
            else if (name == "noobj") noObj = val == "1";
            else { fail(outPath, "unknown arg " + name); return "call: unknown arg " + name; }
        } else if (k == "rect") {
            ss >> rect[0] >> rect[1] >> rect[2] >> rect[3];
        } else if (k == "patch") {
            std::string who, off, hex;
            ss >> who >> off >> hex;
            Patch p;
            p.who = who.empty() ? '?' : who[0];
            p.off = (size_t)std::strtoull(off.c_str(), nullptr, 0);
            if ((p.who != 'p' && p.who != 's') || !fromHex(hex, p.bytes)) { fail(outPath, "bad patch line"); return "call: bad patch line"; }
            patches.push_back(std::move(p));
        }
    }
    static const char* kAllowed[] = {"collidedWithSlopeInternal", "collidedWithObjectInternal", "updateSlopeRotation"};
    bool allowed = false;
    for (const char* a : kAllowed) if (fn == a) allowed = true;
    if (!allowed) { fail(outPath, "function not in the allowed list: " + fn); return "call: function not allowed: " + fn; }

    // PlayerObject::init adds its trail to the CCLayer it is given (a null layer crashes at 0x4e6bcf): a scratch layer that is on no scene takes it
    cocos2d::CCLayer* scratchLayer = cocos2d::CCLayer::create();
    if (scratchLayer) scratchLayer->retain();
    PlayerObject* player = scratchLayer ? PlayerObject::create(1, 1, nullptr, scratchLayer, false) : nullptr;
    if (!player) { if (scratchLayer) scratchLayer->release(); fail(outPath, "PlayerObject::create returned null"); return "call: create failed"; }
    player->retain();
    GameObject* slope = nullptr;
    if (fn == "collidedWithSlopeInternal" || (fn == "collidedWithObjectInternal" && !noObj)) {
        slope =GameObject::createWithKey(1);
        if (!slope) { player->release(); scratchLayer->release(); fail(outPath, "GameObject::createWithKey returned null"); return "call: slope create failed"; }
        slope->retain();
    }
    const size_t psz = sizeof(PlayerObject), ssz = sizeof(GameObject);
    uint8_t* pb = reinterpret_cast<uint8_t*>(player);
    uint8_t* sb = reinterpret_cast<uint8_t*>(slope);
    // The scratch objects own their sprites, vtables and containers: a patch that puts anything else into such a pointer slot (the usual accident: a byte image copied from the running game)
    // makes the first virtual call read a dangling address and takes the worker down (2026-10-29, setPosition under collidedWithObjectInternal). A word of the 8-aligned grid that holds a
    // heap pointer in the freshly made object is therefore never changed. A pointer slot is a word whose value is above 32 bits, below 2^48 AND points at committed memory: the shape alone also
    // fits a flag byte sharing its 8-byte word with others (+0x9c5 reads 2^40), and a value written elsewhere is judged by where it lands, not by its shape. `ptr_slots_p` / `ptr_slots_s` list the slots.
    auto ptrLike = [](uint64_t v) {
        if (!(v > 0xFFFFFFFFull && (v >> 48) == 0)) return false;
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mbi;
        return VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(v)), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS);
#else
        return true;
#endif
    };
    auto slotsOf = [&](const uint8_t* b, size_t n) {
        std::string s;
        for (size_t o = 0; o + 8 <= n; o += 8) { uint64_t v; std::memcpy(&v, b + o, 8); if (ptrLike(v)) s += (s.empty() ? "" : ",") + std::to_string(o); }
        return s;
    };
    const std::string pSlots = slotsOf(pb, psz), sSlots = sb ? slotsOf(sb, ssz) : std::string();
    // the patches go onto COPIES first and reach the objects only if none touches a slot: an object whose pointer was zeroed or replaced would crash in its own destructor when released
    std::vector<uint8_t> workP(pb, pb + psz), workS(sb ? sb : pb, (sb ? sb : pb) + (sb ? ssz : 0));
    const std::vector<uint8_t> origP = workP, origS = workS;
    auto refuse = [&](const std::string& why) {
        if (slope) slope->release();
        player->release();
        scratchLayer->release();
        std::ofstream f(outPath, std::ios::trunc);
        f << "ok 0 " << why << "\n" << "ptr_slots_p " << pSlots << "\n" << "ptr_slots_s " << sSlots << "\n";
        return std::string("call: ") + why;
    };
    for (const Patch& p : patches) {
        std::vector<uint8_t>& base = p.who == 'p' ? workP : workS;
        if ((p.who == 's' && !slope) || p.off + p.bytes.size() > base.size()) return refuse("patch outside the object");
        std::memcpy(base.data() + p.off, p.bytes.data(), p.bytes.size());
    }
    for (int w = 0; w < 2; ++w) {
        const std::vector<uint8_t>& now = w == 0 ? workP : workS;
        const std::vector<uint8_t>& was = w == 0 ? origP : origS;
        for (size_t o = 0; o + 8 <= was.size(); o += 8) {
            if (std::memcmp(now.data() + o, was.data() + o, 8) == 0) continue;
            uint64_t a, b;
            std::memcpy(&a, was.data() + o, 8);
            std::memcpy(&b, now.data() + o, 8);
            char why[220];
            if (ptrLike(a)) {
                snprintf(why, sizeof(why), "refused: the patches change the word at %c+0x%zx, which holds a heap pointer of the scratch object", w == 0 ? 'p' : 's', o);
                return refuse(why);
            }
            if (ptrLike(b)) {   // a value that points at live memory, written where no pointer was: the same dangling read waiting to happen, judged by where it points, not by its shape
                snprintf(why, sizeof(why), "refused: the patches write an address of committed memory into the word at %c+0x%zx", w == 0 ? 'p' : 's', o);
                return refuse(why);
            }
        }
    }
    std::memcpy(pb, workP.data(), psz);
    if (sb) std::memcpy(sb, workS.data(), ssz);
    std::memset(pb + 0xc28, 0, 8);   // m_gameLayer: none, whatever a patch laid there
    // collidedWithObjectInternal 0x4efd50 reaches the SINGLETON PlayLayer (destroyPlayer) when a hazard is hit and +0x7ed is 0 (decompile line 1198; +0x7ed is read nowhere else in it);
    // with +0x7ed = 1 the same branch only sets +0x98c. The real level is never reached, and player_in shows the 1 the function was entered with.
    if (fn == "collidedWithObjectInternal") pb[0x7ed] = 1;
    // collidedWithSlopeInternal reaches PlayLayer::destroyPlayer on a lethal (spiked) slope contact: a slope with m_slopeIsHazard set is refused here, after the patches, before the call
    if (fn == "collidedWithSlopeInternal" && slope && slope->m_slopeIsHazard) {
        slope->release();
        player->release();
        scratchLayer->release();
        fail(outPath, "refused: the slope object has m_slopeIsHazard set (the call would reach PlayLayer::destroyPlayer)");
        return "call: refused - slope hazard";
    }

    const std::string pIn = toHex(pb, psz), sIn = slope ? toHex(sb, ssz) : std::string();   // the bytes the function is entered with (the scratch object as made, with the patches over it)
    std::string ret;
    if (fn == "collidedWithSlopeInternal") {
        player->collidedWithSlopeInternal(dt, slope, forced);
    } else if (fn == "collidedWithObjectInternal") {
        ret = player->collidedWithObjectInternal(dt, slope /* null with arg noobj 1 */, cocos2d::CCRect(rect[0], rect[1], rect[2], rect[3]), forced) ? "1" : "0";
    } else if (fn == "updateSlopeRotation") {
        player->updateSlopeRotation(dt);
    }

    std::ofstream o(outPath, std::ios::trunc);
    o << "ok 1\n";
    if (!ret.empty()) o << "ret " << ret << "\n";
    o << "player_in " << pIn << "\n";
    if (slope) o << "slope_in " << sIn << "\n";
    if (slope) o << "slope_hazard_off " << (reinterpret_cast<uint8_t*>(&slope->m_slopeIsHazard) - sb) << "\n";   // where the refused byte lives, for a caller that builds a slope
    o << "player " << toHex(pb, psz) << "\n";
    if (slope) o << "slope " << toHex(sb, ssz) << "\n";
    o << "ptr_slots_p " << pSlots << "\n";
    if (slope) o << "ptr_slots_s " << sSlots << "\n";
    o << "sizes " << psz << " " << ssz << "\n";
    o.close();
    if (slope) slope->release();
    player->release();
    scratchLayer->release();
    return "call: " + fn + " done";
}

}  // namespace callverify
