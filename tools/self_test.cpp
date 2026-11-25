// self_test: dp/src/dp/selfshadow.hpp, the self-running one-step shadow, against the game's own bytes (mod cfg btraw=t0,t1 and ctraw=t0,t1 of the same run).
//   self_test <btraw.csv> <objrects.txt> <ctraw.csv> [--from T] [--to T] [--open] [--show N] [--verbose]
// For each tick of the window the shadow is given the button edges btraw saw (the calls of pushButton / releaseButton on button 1) and nothing else, and what it produces is compared with
//   - the pass's entry image  (ctraw pre)           : the buttons, the update phase and the logs, from the head of the step
//   - the pass's exit image   (ctraw post)          : ... and the collision pass
//   - the next step's head    (btraw pre of tick+1) : ... and the clock, the rotation, the position the next step reads
// Closed form (default): the shadow carries its own image from tick to tick; the first tick is seeded from the head image btraw has. --open: every tick starts again from the game's head image.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/self_test.cpp
#include "dp/selfshadow.hpp"
#include "dp/selfshadow_dual.hpp"
#include "dp/shadow_layer.hpp"
#include "dp/shadow_state.hpp"
#include "touch_truth.hpp"
#include "dp/trigtouch.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace dp::gdcol;

static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> r;
    std::string cur;
    for (char ch : s) {
        if (ch == d) { r.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    r.push_back(cur);
    return r;
}
static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> b(s.size() / 2);
    auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)(v(s[2 * i]) * 16 + v(s[2 * i + 1]));
    return b;
}
static bool ignored(size_t o) {
    static const bool showAll = std::getenv("ST_ALL") != nullptr;   // ST_ALL=1: compare every word, to see what the ignore list hides
    if (showAll) return false;
    if (o >= 0x820 && o < 0x828) return true;           // the colours startDashing sets white (the glow / second colour, 0x4f3b20 lines 88-93): sprite state, no physics reads them
    if (o >= 0xc50) return true;                         // past sizeof(PlayerObject) = 3152 = 0xc50: the capture copies 0xc80 bytes, and what follows the object is the next heap block, which the game writes to as it likes (w_132170790_4390: +0xc78 held 0x35322c3052e02511 in one capture and 0x3f8000003cfcf98a in another)
    if (o >= 0x700 && o < 0x728) return true;           // the rest of the unordered_map whose size is at +0x6f8 (checked separately, below): its list head / bucket vector pointers (+0x700..+0x710) and mask / bucket count (+0x718 = 63, +0x720 = 64 once it has grown from 7 / 8) are the container's own bookkeeping (w_140490938_10030 t=10200)
    if (o >= 0x998 && o < 0x9a4) return true;
    if (o >= 0x800 && o < 0x808) return true;
    if (o >= 0x8c && o < 0xa4) return true;
    if (o >= 0x111 && o < 0x11c) return true;
    if (o == 0x14) return true;
    if (o >= 0x40 && o < 0x48) return true;             // the node's scale: the size-portal tween (updateInternalActions) animates it; the physics reads +0x394 / +0x398
    if (o == 0x7ee || o == 0x7ef) return true;
    if (o >= 0x1f0 && o < 0x230) return true;
    if ((o >= 0x148 && o < 0x150) || (o >= 0x1d8 && o < 0x1dc) || (o >= 0xc6c && o < 0xc70)) return true;
    if (o >= 0x810 && o < 0x818) return true;
    if ((o >= 0xa08 && o < 0xa10) || (o >= 0xa34 && o < 0xa3c)) return true;   // the last touched object's node position: the dump holds the rect centre, which differs for a hitbox that is offset from its node (gravity toggle id 2926)
    if (o >= 0xc60 && o < 0xc78) return true;           // four floats rewritten at the head of every frame (rand-looking values); no reader found in the decompiles
    if (o >= 0xd8 && o < 0xe0) return true;             // a heap pointer
    if (o >= 0x590 && o < 0x598) return true;           // the dash ring the player dashes from: a heap pointer in the game, uid + 1 in the shadow
    if (o >= 0x818 && o < 0x820) return true;           // the dash start flash's tint timers (visual)
    if (o >= 0x9f0 && o < 0x9f4) return true;           // updateDashAnimation's frame counter (the sprite frame, visual)
    if (o >= 0x734 && o < 0x740) return true;           // a heap container the size-portal effect fills (visual)
    // the containers btraw keeps apart (used rings, touching array, holding map) and the heap pointers of the 200-entry y history
    if ((o >= 0x850 && o < 0x890) || (o >= 0xa40 && o < 0xa88) || (o >= 0xbc0 && o < 0xbd0) || (o >= 0xac8 && o < 0xad0) || (o >= 0xa88 && o < 0xa90) || (o >= 0x5e8 && o < 0x620) || (o >= 0x678 && o < 0x688) || (o >= 0x6f8 && o < 0x700) || (o >= 0x890 && o < 0x898)) return true;   // ... and the size of the touched-ramp map (+0x6f8; the game counts the heap pointer in +0x678, the seeded image carries it as a number) ... and the object pointer slots (the game's are heap pointers, the shadow's uid + 1)
    return false;
}

struct Snap {
    std::vector<uint8_t> img;
    struct R { int uid, type, rev, f740; };
    std::vector<R> arr;
    std::set<int> used, touched;
    std::string hold;
    bool hasSlots = false;                   // cfg btslots=1: the sixth section, "off:uid" for each pointer slot of the image that points at an object (hex offsets)
    std::map<size_t, int> slots;
};
static Snap parseSnap(const std::string& s) {
    Snap n;
    const auto parts = split(s, ';');
    if (parts.size() < 5) return n;
    n.img = unhex(parts[0]);
    { std::istringstream is(parts[1]); std::string tok; while (is >> tok) { const auto f = split(tok, ':'); if (f.size() >= 4) n.arr.push_back({std::atoi(f[0].c_str()), std::atoi(f[1].c_str()), std::atoi(f[2].c_str()), std::atoi(f[3].c_str())}); } }
    { std::istringstream is(parts[2]); int u; while (is >> u) n.used.insert(u); }
    { std::istringstream is(parts[3]); int u; while (is >> u) n.touched.insert(u); }
    n.hold = parts.size() > 4 ? parts[4] : "---";
    if (parts.size() > 5) { n.hasSlots = true; std::istringstream is(parts[5]); std::string tok; while (is >> tok) { const auto f = split(tok, ':'); if (f.size() == 2) { const int u = std::atoi(f[1].c_str()); if (u >= 0) n.slots[(size_t)std::strtoull(f[0].c_str(), nullptr, 16)] = u; } } }
    return n;
}

static std::vector<size_t> diff(const Mem& a, const std::vector<uint8_t>& b) {
    std::vector<size_t> d;
    for (size_t w = 0; w + 4 <= b.size() && w + 4 <= a.b.size(); w += 4) {
        bool x = false;
        for (size_t k = w; k < w + 4; ++k) if (!ignored(k) && a.b[k] != b[k]) x = true;
        if (x) d.push_back(w);
    }
    {   // +0x6f8, the size of the touched-ramp map: only whether it is zero is read (collidedWithObject line 283); the count itself carries the heap pointer of +0x678 in the game
        uint64_t ma = 0, gb = 0;
        if (a.b.size() >= 0x700 && b.size() >= 0x700) { std::memcpy(&ma, a.b.data() + 0x6f8, 8); std::memcpy(&gb, b.data() + 0x6f8, 8); if ((ma != 0) != (gb != 0)) d.push_back(0x6f8); }
    }
    return d;
}

static const bool ST_ONLAG = std::getenv("ST_NOONLAG") == nullptr;   // the group toggle (wtrace's on column) reads one row behind the positions; ST_NOONLAG=1 gives the old reading
static const bool ST_NOSIZE = std::getenv("ST_NOSIZE") != nullptr;   // ST_NOSIZE=1: ignore the w, h columns of wtrace.csv
int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: self_test btraw.csv objrects.txt ctraw.csv [--from T] [--to T] [--open] [--show N] [--verbose]\n"); return 2; }
    long long from = 0, to = 1LL << 60;
    bool open = false, verbose = false;
    long long traceFrom = 1, traceTo = 0, wtShift = 1;   // the world rows labelled t + 1 are the positions during step t (measured)
    bool noWt = false;
    
    int show = 10;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from") from = std::atoll(argv[++i]);
        else if (a == "--to") to = std::atoll(argv[++i]);
        else if (a == "--open") open = true;
        else if (a == "--show") show = std::atoi(argv[++i]);
        else if (a == "--verbose") verbose = true;
        else if (a == "--nowt") noWt = true;
        else if (a == "--wtshift") wtShift = std::atoll(argv[++i]);
        else if (a == "--trace") { traceFrom = std::atoll(argv[++i]); traceTo = std::atoll(argv[++i]); }
    }
    std::vector<GObj> objs;
    {
        std::ifstream f(argv[2]);
        std::string line;
        std::getline(f, line);
        const auto hdr = split(line, ',');
        std::map<std::string, size_t> col;
        for (size_t i = 0; i < hdr.size(); ++i) col[hdr[i]] = i;
        auto num = [&](const std::vector<std::string>& v, const char* n, double d = 0) { auto it = col.find(n); return it == col.end() || it->second >= v.size() || v[it->second].empty() ? d : std::stod(v[it->second]); };
        while (std::getline(f, line)) {
            const auto v = split(line, ',');
            if (v.size() < 8) continue;
            GObj o;
            o.objectId = (int)num(v, "id");
            o.type = (int)num(v, "type");
            const double cx = num(v, "cx"), cy = num(v, "cy"), w = num(v, "w"), h = num(v, "h");
            o.uid = (int)num(v, "uid", -1);
            o.cx = (float)cx; o.cy = (float)cy; o.prevCx = o.cx; o.prevCy = o.cy;
            o.rw = (float)w; o.rh = (float)h; o.rx = (float)(cx - w / 2); o.ry = (float)(cy - h / 2);
            o.radius = (float)num(v, "radius");
            o.scaleX = (float)num(v, "scx", 1);
            o.scaleY = (float)num(v, "scy", 1);
            o.slopeDir = (int)num(v, "sdir");
            o.slopeUphill = num(v, "sup") != 0;
            o.slopeHazard = num(v, "shz") != 0;
            o.passable = num(v, "nocol") != 0;
            o.oriented = num(v, "rot") != 0;
            o.flipY = num(v, "flipy") != 0;
            o.multiAct = num(v, "multiact") != 0;
            o.touchTrig = num(v, "touch") != 0;
            o.extCol = num(v, "extcol") != 0;
            o.flipX = num(v, "flipx") != 0;
            o.reverse = num(v, "rev") != 0;
            o.groupCount = (int)num(v, "groups");
            o.rot = (float)num(v, "rot"); o.w0 = (float)num(v, "w0"); o.h0 = (float)num(v, "h0");
            o.sy0 = num(v, "sy0"); o.sy1 = num(v, "sy1");
            o.tpEx = (float)num(v, "tpex"); o.tpEy = (float)num(v, "tpey"); o.tpY = (float)num(v, "tpy");
            o.tpRX = (float)num(v, "tprx"); o.tpRY = (float)num(v, "tpry");
            o.tpGrav = (int)num(v, "tpg"); o.tpKeepX = num(v, "tpix") != 0; o.tpKeepY = num(v, "tpiy") != 0; o.tpSave = num(v, "tpsave") != 0;
            o.tpFm = (int)num(v, "tpfm"); o.tpF = (float)num(v, "tpf");
            o.tpAng = (float)num(v, "tpang"); o.tpRmod = (float)num(v, "tprmod"); o.tpRmin = (float)num(v, "tprmin"); o.tpRmax = (float)num(v, "tprmax");
            objs.push_back(o);
        }
        if (const char* obbPath = std::getenv("ST_OBB")) {
            std::ifstream ob(obbPath);
            std::string ol;
            std::getline(ob, ol);
            std::map<int, std::vector<float>> corners;
            while (std::getline(ob, ol)) {
                const auto v = split(ol, ',');
                if (v.size() < 14) continue;
                std::vector<float> c;
                for (size_t i = 6; i < 14; ++i) c.push_back((float)std::atof(v[i].c_str()));
                corners[std::atoi(v[0].c_str())] = c;
            }
            for (auto& o : objs) {
                auto it = corners.find(o.uid);
                if (it == corners.end()) continue;
                o.hasGc = true;
                for (int q = 0; q < 8; ++q) o.gc[q] = it->second[q];
                o.gcx = o.cx; o.gcy = o.cy;
            }
        }
        {   // forceblocks.txt beside objrects.txt: uid,id,type,cx,cy,rot,scalex,scaley,radius,rectw,recth,force,minforce,maxforce,relative,range,forceid
            std::string dir = argv[2];
            const size_t slash = dir.find_last_of("/\\");
            dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
            std::ifstream fb(dir + "forceblocks.txt");
            std::string fl;
            std::getline(fb, fl);
            std::map<int, GObj*> byUid;
            for (auto& o : objs) byUid[o.uid] = &o;
            while (std::getline(fb, fl)) {
                const auto v = split(fl, ',');
                if (v.size() < 17) continue;
                auto it = byUid.find(std::atoi(v[0].c_str()));
                if (it == byUid.end()) continue;
                GObj& o = *it->second;
                o.fbForce = (float)std::atof(v[11].c_str()); o.fbMin = (float)std::atof(v[12].c_str()); o.fbMax = (float)std::atof(v[13].c_str());
                o.fbRelative = std::atoi(v[14].c_str()) != 0; o.fbRange = std::atoi(v[15].c_str()) != 0; o.fbId = std::atoi(v[16].c_str());
            }
        }
        std::map<int, std::pair<float, float>> ratio;
        for (auto& o : objs) {
            if (o.w0 <= 0 || o.h0 <= 0) continue;
            if (std::fmod(std::fabs(o.rot), 90.0f) == 0.0f) {
                const bool odd = std::fmod(std::fabs(o.rot), 180.0f) != 0.0f;
                ratio[o.objectId] = {(odd ? o.rh : o.rw) / (o.w0 * o.scaleX), (odd ? o.rw : o.rh) / (o.h0 * o.scaleY)};
            }
        }
        for (auto& o : objs) {
            if (!o.oriented || std::fmod(std::fabs(o.rot), 90.0f) == 0.0f) continue;
            auto it = ratio.find(o.objectId);
            if (it != ratio.end()) { o.bw = it->second.first * o.w0 * o.scaleX; o.bh = it->second.second * o.h0 * o.scaleY; }
        }
    }
    // ctraw: the pass's entry / exit per tick, and the layer
    struct Pass { std::vector<uint8_t> pre, post; Layer L; bool hasL = false; int n = 4; float c308 = 0.0f; bool f310 = false, f312 = false, f418 = false; };   // c308 / f310 / f312 / f418: layer +0x308 / +0x310 / +0x312 / +0x418 (ST_OWNLAYER's seed)   // n: sub-steps in this pass's frame (ctraw prints dt with six digits: 1 / dt rounds to the count)
    std::map<long long, Pass> passes;
    std::map<long long, Pass> passes2;   // player 2's (ctraw rows with who = 2)
    Layer L;
    bool haveLayer = false;
    {
        std::ifstream f(argv[3]);
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            const auto v = split(line, ',');
            if (v.size() >= 8 && std::atoi(v[2].c_str()) == 2) {   // player 2 (cfg ctrawp2=1): its pass's entry / exit, compared under ST_DUAL
                Pass p2;
                p2.pre = unhex(split(v[6], ';')[0]);
                p2.post = unhex(split(v[7], ';')[0]);
                passes2[std::atoll(v[1].c_str())] = p2;
                continue;
            }
            if (v.size() < 8 || std::atoi(v[2].c_str()) != 1) continue;
            const long long tick = std::atoll(v[1].c_str());
            const auto parts = split(v[6], ';');
            Pass p;
            { const double dtc = std::atof(v[3].c_str()); if (dtc > 0.0) p.n = std::max(1, (int)std::lround(1.0 / dtc)); }
            p.pre = unhex(parts[0]);
            p.post = unhex(split(v[7], ';')[0]);
            if (parts.size() >= 8) {   // the layer's words as the game held them at the pass: the flying band moves with the portals the layer handles, so it is an input of the step
                auto f32 = [](const std::vector<uint8_t>& b, size_t o) { float x; std::memcpy(&x, b.data() + o, 4); return x; };
                const auto A = unhex(parts[2]), B = unhex(parts[3]), C = unhex(parts[4]), D = unhex(parts[5]), E = unhex(parts[6]);
                Layer Lt;
                Lt.speedA = f32(A, 8); Lt.speedB = f32(A, 12);
                Lt.freeMode = B[0x311 - 0x300] != 0; Lt.dual = B[0x422 - 0x300] != 0;
                Lt.w764 = f32(C, 4);
                if (B.size() >= 0x120) Lt.flipAmt = f32(B, 0x41c - 0x300);
                Lt.f317c = D[0xc] != 0; Lt.f317e = D[0xe] != 0;
                Lt.scaleX = f32(E, 0); Lt.scaleY = f32(E, 4); Lt.ceilWord = f32(E, 8);
                const auto pq = split(parts[7], ':');
                Lt.maxPortalY = std::stof(pq[0]); Lt.minPortalY = std::stof(pq[1]);
                p.L = Lt; p.hasL = true;
                if (B.size() >= 0x13) { p.c308 = f32(B, 8); p.f310 = B[0x10] != 0; p.f312 = B[0x12] != 0; if (B.size() > 0x118) p.f418 = B[0x118] != 0; }
                if (!haveLayer) { L = Lt; haveLayer = true; }
            }
            passes[tick] = p;
        }
    }
    // btraw: per tick, the head image (the first row's entry) and the button-1 edges
    // the game's frame boundaries, read from the pre images: +0x98c..: the position the last frame ended with (+0xa98, +0xa9c) changes at the head of a frame
    std::set<long long> headTicks;
    {
        const std::vector<uint8_t>* prevPre = nullptr; long long prevT = 0;
        for (auto& pk : passes) {
            const auto& pre = pk.second.pre;
            if (prevPre && pk.first == prevT + 1 && pre.size() >= 0xaa0 && prevPre->size() >= 0xaa0 && std::memcmp(pre.data() + 0xa98, prevPre->data() + 0xa98, 4) != 0) headTicks.insert(pk.first);
            prevPre = &pk.second.pre; prevT = pk.first;
        }
    }
    struct TickRows { Snap head; bool haveHead = false; int headFn = 0; uint8_t press = 0, release = 0; int other = 0; };
    std::map<long long, TickRows> ticks;
    std::map<long long, TickRows> ticks2;   // player 2's head images (btraw who = 2)
    {
        std::ifstream f(argv[1]);
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            const auto cols = split(line, ',');
            if (cols.size() < 8) continue;
            const long long label = std::atoll(cols[1].c_str());
            const int fn = std::atoi(cols[3].c_str()), arg = std::atoi(cols[4].c_str());
            if (std::atoi(cols[2].c_str()) == 2) {   // player 2 (cfg btrawp2=1): its head image, by the same tick rule, for a window that starts inside a dual
                if (fn == 3 || fn == 4 || ((fn == 1 || fn == 2) && arg != 1)) continue;
                TickRows& t2 = ticks2[fn == 5 ? label : label + 1];
                if (!t2.haveHead || (fn != 5 && t2.headFn == 5)) { Snap s = parseSnap(cols[6]); if (s.img.size() >= 0xc80 && !(t2.haveHead && t2.headFn != 5)) { t2.head = s; t2.haveHead = true; t2.headFn = fn; } }
                continue;
            }
            if (std::atoi(cols[2].c_str()) != 1) continue;
            if (fn == 3 || fn == 4 || ((fn == 1 || fn == 2) && arg != 1)) { ++ticks[label + 1].other; continue; }   // releaseAll / enablePlayerControls, and the platformer buttons: their rows are not at a step head
            const long long tick = fn == 5 ? label : label + 1;   // the ctraw tick of the pass these calls belong to (measured: the calls of processCommands carry the tick before)
            TickRows& tr = ticks[tick];
            if (!tr.haveHead || (fn != 5 && tr.headFn == 5)) { Snap s = parseSnap(cols[6]); if (s.img.size() >= 0xc80 && !(tr.haveHead && tr.headFn != 5)) { tr.head = s; tr.haveHead = true; tr.headFn = fn; } }
            if (fn == 1 && arg == 1) tr.press = 1;
            else if (fn == 2 && arg == 1) tr.release = 1;
            else if (fn == 1 || fn == 2 || fn == 3 || fn == 4) ++tr.other;
        }
    }
    if (ticks.empty() || !haveLayer || passes.empty()) {
        std::printf("status: UNOBSERVED (btraw rows %zu, ctraw rows %zu, layer %s): nothing was compared\n", ticks.size(), passes.size(), haveLayer ? "read" : "missing");
        return 3;
    }
    // wtrace.csv beside objrects.txt (cfg grouptrace=1 wtrace=1): the live node position of every tracked (moved) object, a row on any change; applied as a shift of the object's rect
    struct WRow { long long t; float x, y, rot; int on; float w, h; bool hasLast; float lx, ly; };   // w, h: the rect size the game held (0 = column absent: an older capture)
    std::map<int, std::vector<WRow>> wt;
    std::map<int, size_t> wtPtr;
    std::map<int, size_t> objIdx;
    std::vector<GObj> baseObjs = objs;
    for (size_t i = 0; i < objs.size(); ++i) objIdx[objs[i].uid] = i;
    if (!noWt) {
        std::string dir = argv[2];
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
        std::ifstream wl(dir + "wtrace_last.csv");   // a capture of a run that ended in a death: wtrace.csv is the attempt GD started after it (a header), wtrace_last.csv the attempt that died
        std::ifstream wf(wl ? dir + "wtrace_last.csv" : dir + "wtrace.csv");
        std::string line;
        std::getline(wf, line);
        while (std::getline(wf, line)) {
            const auto v = split(line, ',');
            if (v.size() < 6) continue;
            wt[std::atoi(v[1].c_str())].push_back({std::atoll(v[0].c_str()), (float)std::atof(v[2].c_str()), (float)std::atof(v[3].c_str()), (float)std::atof(v[4].c_str()), std::atoi(v[5].c_str()), v.size() > 7 ? (float)std::atof(v[6].c_str()) : 0.0f, v.size() > 7 ? (float)std::atof(v[7].c_str()) : 0.0f, v.size() > 10, v.size() > 10 ? (float)std::atof(v[8].c_str()) : 0.0f, v.size() > 10 ? (float)std::atof(v[9].c_str()) : 0.0f});   // lx, ly: the live object's +0x4d0 / +0x4d4 (cfg wtlast)
        }
    }
    // hwwatch.csv beside objrects.txt (cfg hwwatch=<offset>): the ticks at which the game wrote 4 bytes into the player's field at ST_HWOFF (default 0xb8c) and the value it wrote -- a trigger the shadow does not run (the gravity trigger
    // writes +0xb8c); fed back at the point of the step where the layer does such writes
    std::map<long long, unsigned> hwWrites;
    long long hwUsed = 0;   // how many of the game's recorded writes were put into the shadow (a window that is IDENTICAL with injected values is not a match of the shadow's own code)
    size_t hwOffset = std::getenv("ST_HWOFF") ? (size_t)std::strtoull(std::getenv("ST_HWOFF"), nullptr, 0) : 0xb8c;
    if (std::getenv("ST_HW")) {   // opt-in (ST_HW=1): a stale hwwatch.csv of another offset left in a capture folder must not be injected silently
        std::string dir = argv[2];
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
        std::ifstream hf(dir + "hwwatch.csv");
        std::string hl;
        std::getline(hf, hl);
        while (std::getline(hf, hl)) {
            const auto v = split(hl, ',');
            if (v.size() < 9) continue;
            hwWrites[std::atoll(v[0].c_str())] = (unsigned)std::strtoul(v[8].c_str(), nullptr, 16);
        }
    }
    std::function<void(size_t)> onMoved;   // the shadow's section index re-files a mover whose centre changed (SelfShadow::moverMoved)
    auto applyMovers = [&](long long T) {
        for (auto& kv : wt) {
            auto oi = objIdx.find(kv.first);
            if (oi == objIdx.end()) continue;
            size_t& p = wtPtr[kv.first];
            const auto& rows = kv.second;
            if (rows.empty() || rows.front().t > T) continue;
            while (p + 1 < rows.size() && rows[p + 1].t <= T) ++p;
            const WRow& r = rows[p];
            const WRow& r0 = rows.front();
            GObj& o = objs[oi->second];
            const GObj& b = baseObjs[oi->second];
            const float dx = r.x - r0.x, dy = r.y - r0.y;
            const float cx0 = o.cx, cy0 = o.cy;
            if (!r.hasLast) { o.prevCx = o.cx; o.prevCy = o.cy; }   // +0x4d0 / +0x4d4: where it was before this move (a capture without wtlast: the previous reading)
            o.cx = r.x + (b.cx - r0.x); o.cy = r.y + (b.cy - r0.y);
            if (r.hasLast) { o.prevCx = r.lx + (b.cx - r0.x); o.prevCy = r.ly + (b.cy - r0.y); }   // cfg wtlast: the live object's own last position (moveObjects sets it only for a Move action's group, and only when the layer counter differs)   // the game: node position + box offset; the rect: centre - size / 2
            const bool sized = r.w > 0.0f && r.h > 0.0f && !ST_NOSIZE;   // a Scale trigger changes the rect: the game's own size, not the one the level started with
            if (sized) { o.rw = r.w; o.rh = r.h; }
            if (sized && b.radius > 0.0f && b.rw > 0.0f && b.rh > 0.0f && r.rot == r0.rot) { o.scaleX = b.scaleX * (r.w / b.rw); o.scaleY = b.scaleY * (r.h / b.rh); }   // a round hazard's radius follows the node scale (circleHit: max(scaleX, scaleY) * radius), which a Scale trigger has changed
            o.gscale = (sized && b.rw > 0.0f && b.rh > 0.0f) ? 0.5f * (r.w / b.rw + r.h / b.rh) : 1.0f;   // the corners GD gave us are for the size the level started with
            o.rx = o.cx - (sized ? r.w : b.rw) * 0.5f; o.ry = o.cy - (sized ? r.h : b.rh) * 0.5f;
            (void)dx; (void)dy;
            if (r.rot != r0.rot || b.rot != r0.rot) {   // a turning object: the rect is the bounding box of the box at its angle, the oriented box is the box itself
                float ubw = b.bw, ubh = b.bh;
                if (!(ubw > 0.0f && ubh > 0.0f)) {
                    const bool right = std::fmod(std::fabs(b.rot), 90.0f) == 0.0f;
                    if (right) { const bool odd = std::fmod(std::fabs(b.rot), 180.0f) != 0.0f; ubw = odd ? b.rh : b.rw; ubh = odd ? b.rw : b.rh; }
                    else { ubw = b.w0 * b.scaleX; ubh = b.h0 * b.scaleY; }
                }
                if (sized && b.rw > 0.0f && b.rh > 0.0f && !(b.rot != 0.0f)) { ubw *= r.w / b.rw; ubh *= r.h / b.rh; }   // the unrotated box at the size the game holds now
                const float ang = r.rot * 0.017453292f;
                const float cs = std::fabs(std::cos(ang)), sn = std::fabs(std::sin(ang));
                o.bw = ubw; o.bh = ubh; o.rot = r.rot; o.oriented = true;
                o.rw = ubw * cs + ubh * sn; o.rh = ubw * sn + ubh * cs;
                o.rx = o.cx - o.rw * 0.5f; o.ry = o.cy - o.rh * 0.5f;
            } else if (sized && o.oriented && !o.hasGc && b.rw > 0.0f && b.rh > 0.0f && std::fabs(r.w / b.rw - r.h / b.rh) < 1e-3f && std::fmod(std::fabs(b.rot), 90.0f) != 0.0f) {   // a rotated box that stands still while a Scale trigger shrinks it: the oriented box shrinks about its centre as well (objrects' w0 / h0 are the size the level started with)
                const bool known = b.bw > 0.0f && b.bh > 0.0f;
                o.bw = (known ? b.bw : b.w0 * b.scaleX) * (r.w / b.rw);
                o.bh = (known ? b.bh : b.h0 * b.scaleY) * (r.h / b.rh);
            }
            {   // the group toggle: read one row behind the positions (a toggle the layer runs after the pass of its step is on for the NEXT pass: w_116791791_1822, the force block 3645 at t=2029 / 2030)
                const WRow* ro = &r;
                if (ST_ONLAG) { size_t qq = p; while (qq > 0 && rows[qq].t > T - 1) --qq; ro = &rows[qq]; if (ro->t > T - 1) ro = &rows.front(); }
                if (ro->on == 0) o.inactive = true; else if (ro->on == 1) o.inactive = false;
            }
            if (onMoved && (std::memcmp(&o.cx, &cx0, 4) != 0 || std::memcmp(&o.cy, &cy0, 4) != 0)) onMoved(oi->second);
        }
    };
    SelfShadow sh;
    sh.keepImages = true;   // (this tool compares the whole images at the buttons and at the pass's entry)
    // ST_DUAL=1 (--shadowdual's port, dp/selfshadow_dual.hpp): player 2 is stepped with player 1 and compared with the game's player-2 rows (cfg btrawp2 / ctrawp2)
    const bool dualOn = std::getenv("ST_DUAL") != nullptr;
    DualShadow ds;
    ds.sh = &sh;
    long long p2Compared = 0, p2BadEntry = 0, p2BadExit = 0, p2First = -1, p2Missing = 0, p2Extra = 0, p2FirstMissing = -1, p2FirstExtra = -1;
    std::map<size_t, long long> w2Entry, w2Exit, w2cEntry, w2cExit;
    long long p2BadCarried = 0, p2FirstCarried = -1;
    struct P2Byte { bool seen = false, gMoved = false, mMoved = false; uint8_t g0 = 0, m0 = 0; long long bad = 0, first = -1; };
    std::map<size_t, P2Byte> p2Bytes;
    long long p2PhysFirst = -1; std::string p2PhysTxt;
    int p2Shown = 0;
    {   // the frame phase: a98 (the position the last frame ended with) changes at the first step of the next frame
        std::vector<uint8_t> prevPre; long long prevT = -1;
        for (auto& pk : passes) {
            if (!prevPre.empty() && pk.first == prevT + 1 && std::memcmp(prevPre.data() + 0xa98, pk.second.pre.data() + 0xa98, 8) != 0) { sh.frameEndMod = (int)(prevT % sh.frameLen); break; }
            prevPre = pk.second.pre; prevT = pk.first;
        }
    }
    sh.world = &objs;
    sh.index();
    {   // the objects applyMovers moves: the shadow files every other object in its section cell once
        std::vector<int> mv;
        for (const auto& kv : wt) if (objIdx.count(kv.first)) mv.push_back(kv.first);
        sh.setMovers(mv, true);
        onMoved = [&sh](size_t i) { sh.moverMoved(i); };
    }
    {   // levelsettings.txt beside objrects.txt: the circle test's version and dynamic level height
        std::string dir = argv[2];
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
        std::ifstream ls(dir + "levelsettings.txt");
        std::string line;
        while (std::getline(ls, line)) {
            if (line.rfind("fixRadiusCollision=", 0) == 0) L.f1cf = line.back() == '1';
            else if (line.rfind("dynamicLevelHeight=", 0) == 0) sh.dynHeight = line.back() == '1';
            else if (line.rfind("platformerMode=", 0) == 0) sh.platformerLevel = line.back() == '1';
        }
    }
    sh.L = L;
    sh.prepareNeighbourhood();   // the section index (dynHeight is known now): built once here, not inside the first step
    // ST_ROUNDTRIP (S2 stage 2, dp/shadow_state.hpp): bit 1 = the closed loop -- the shadow is packed into a ShadowLite after every step and the next step runs on
    // a SelfShadow unpacked from it over the seed's image (stdout must not change); bit 2 = every btraw head image of the window is seeded, packed and unpacked,
    // and the carried words and the sets compared (stderr `rtlit:`). Off: nothing below runs.
    const int rtMode = std::getenv("ST_ROUNDTRIP") ? std::atoi(std::getenv("ST_ROUNDTRIP")) : 0;
    SlWorld rtW;
    ShadowLite rtSl;
    std::vector<uint8_t> rtTmpl;
    long long rtSteps = 0, rtOver = 0, rtAmb = 0, rtFirstOver = -1, rtFirstAmb = -1;
    int rtMaxEver = 0, rtMaxAct = 0, rtMaxGate = 0, rtMaxUsed = 0, rtMaxArr = 0, rtMaxNow = 0;
    long long litHeads = 0, litWordsBad = 0, litSetsBad = 0, litEverBad = 0, litOver = 0;
    std::set<size_t> litUncarried;
    std::vector<uint8_t> litTmpl;
    SelfShadow rtLit, rtBack;   // bit 2's scratch shadows (the same world, their own sets)
    SlWorld rtLitW;
    if (rtMode) {
        std::vector<uint8_t> mvFlag(objs.size(), 0);
        for (const auto& kv : wt) { auto oi = objIdx.find(kv.first); if (oi != objIdx.end()) mvFlag[oi->second] = 1; }
        rtW.build(&objs, mvFlag);
        rtLitW.build(&objs, mvFlag);
        rtLit.world = &objs; rtLit.index(); rtLit.dynHeight = sh.dynHeight;
        rtBack.world = &objs; rtBack.index();
    }
    bool rngAttemptStart = false;   // ST_RNG=a0: the generator is 0 at every attempt start (the mod's rngfix pins it; measured by hwabs), advanced once per earlier pick of a group of several
    {   const char* rs = std::getenv("ST_RNG");   // the exit group of a Teleport trigger: triggers.txt (target) + objgroups.txt (members, in uid order), the generator's state before the window's first pick from ST_RNG
        std::string dir = argv[2];
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
        std::map<int, std::vector<int>> members;
        { std::ifstream og(dir + "objgroups.txt"); std::string ln; std::getline(og, ln);
          while (std::getline(og, ln)) { std::istringstream is(ln); int u = 0; if (!(is >> u)) continue; std::string rest; std::getline(is, rest); for (char& ch : rest) if (ch == ',' || ch == '.' || ch == ';') ch = ' '; std::istringstream gs(rest); int g; while (gs >> g) members[g].push_back(u); } }
        { std::ifstream tf(dir + "triggers.txt"); std::string ln; std::getline(tf, ln); int tcol = -1; { std::istringstream hs(ln); std::string c; int i = 0; while (std::getline(hs, c, ',')) { if (c == "target") tcol = i; ++i; } }
          while (tcol >= 0 && std::getline(tf, ln)) { std::istringstream is(ln); std::string c; int i = 0, uid = -1, id = 0, tg = 0; while (std::getline(is, c, ',')) { if (i == 0) uid = std::atoi(c.c_str()); else if (i == 1) id = std::atoi(c.c_str()); else if (i == tcol) tg = std::atoi(c.c_str()); ++i; } if (id == 3022 && members.count(tg)) sh.tpExit[uid] = members[tg]; } }
        if (rs) { rngAttemptStart = std::string(rs) == "a0"; sh.rng = rngAttemptStart ? 0u : (uint32_t)std::strtoull(rs, nullptr, 0); sh.rngOn = true; }
        if (!std::getenv("ST_MEMBERORDER")) for (auto& kv : sh.tpExit) std::reverse(kv.second.begin(), kv.second.end());   // measured (hwwatch on the generator + the landing y of 76 picks): the target group array holds its objects in descending uid order
        if (rs) std::printf("exit groups: %zu teleport triggers, rng state %08x\n", sh.tpExit.size(), (unsigned)sh.rng);
    }
    const bool physOn = std::getenv("ST_PHYS") != nullptr; long long physFirst = -1, physSteps = 0; double physMaxX = 0, physMaxY = 0, physMaxV = 0;   // ST_PHYS=1: where the difference reaches x, y or vy (entry image)
    const char* touchCmp = std::getenv("ST_TOUCHCMP"); dp::gdtouch::TouchDecider touchDec; std::set<std::pair<long long, int>> touchCalls; std::set<int> touchUid; std::map<long long, std::set<int>> touchOverlap; long long touchFirst = -1, touchLast = -1;
    if (touchCmp) {   // triggers.txt beside objrects.txt (the trigger export): its `touch` column is the trigger's own Touch Triggered option; objrects' column is 1 on every ring
        std::string dir = argv[2]; const size_t slash = dir.find_last_of("/\\"); dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
        std::ifstream tf(dir + "triggers.txt"); std::string ln; std::getline(tf, ln); int tcol = -1; { std::istringstream hs(ln); std::string c; int i = 0; while (std::getline(hs, c, ',')) { if (c == "touch") tcol = i; ++i; } }
        while (tcol >= 0 && std::getline(tf, ln)) { std::istringstream is(ln); std::string c; int i = 0, uid = -1, tv = 0; while (std::getline(is, c, ',')) { if (i == 0) uid = std::atoi(c.c_str()); else if (i == tcol) tv = std::atoi(c.c_str()); ++i; } if (tv == 1) touchUid.insert(uid); }
    }
    // ST_OWNLAYER=1 (S2 stage 2 preparation, dp/shadow_layer.hpp): the layer words the portals decide -- Free Mode (+0x311), dual (+0x422), the flying band
    // (getMinPortalY / getMaxPortalY, branch B) and +0x764 -- and the level's ceiling word (+0x37a8, result.txt's maxplayy) are the shadow's own instead of ctraw's.
    // Still copied: the zoom (+0x1a8 / +0x1ac), the mirror tween (+0x41c), +0x317c / +0x317e, the grid scales; from dump.csv per tick: camOffY (+0x1b4) and
    // the static camera's +0x23c / +0x2a0 (branch A). Where the shadow cannot give the band (branch A, a dual, the seed's height unknown) ctraw's is used and counted.
    const bool ownLayer = std::getenv("ST_OWNLAYER") != nullptr;
    ShadowLayer own;
    bool ownSeeded = false;
    struct DumpLayer { float bandst = 0.0f, camOffY = 0.0f; int bandmode = 0; };
    std::map<long long, DumpLayer> dumpLayer;
    std::map<int, std::pair<bool, float>> portalFree;   // uid -> (objrects `free`, cy as placed)
    bool ceilKnown = false; float ceilOwn = 0.0f;
    struct OwnCmp { long long bad = 0, first = -1; };
    OwnCmp cmpFree, cmpDual, cmpMin, cmpMax, cmpW764, cmpCeil, cmpFlip;
    long long bandCopied[4] = {0, 0, 0, 0}, w764Copied = 0, flipCopied = 0, ownSteps = 0, ownPortals = 0;   // band copied: branch A, dual, height unknown, no dump row
    if (ownLayer) {
        std::string dir = argv[2];
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
        {
            std::ifstream df(dir + "dump.csv");
            std::string line;
            std::getline(df, line);
            const auto hdr = split(line, ',');
            int cAtt = -1, cTick = -1, cSt = -1, cMode = -1, cOff = -1;
            for (size_t i = 0; i < hdr.size(); ++i) { if (hdr[i] == "attempt") cAtt = (int)i; else if (hdr[i] == "tick") cTick = (int)i; else if (hdr[i] == "bandst") cSt = (int)i; else if (hdr[i] == "bandmode") cMode = (int)i; else if (hdr[i] == "camoffy") cOff = (int)i; }
            while (cAtt >= 0 && cTick >= 0 && cSt >= 0 && cMode >= 0 && cOff >= 0 && std::getline(df, line)) {
                const auto v = split(line, ',');
                if ((int)v.size() <= std::max({cAtt, cTick, cSt, cMode, cOff}) || v[(size_t)cAtt] != "1") continue;
                DumpLayer d; d.bandst = (float)std::atof(v[(size_t)cSt].c_str()); d.bandmode = std::atoi(v[(size_t)cMode].c_str()); d.camOffY = (float)std::atof(v[(size_t)cOff].c_str());
                dumpLayer[std::atoll(v[(size_t)cTick].c_str())] = d;
            }
        }
        {
            std::ifstream of(argv[2]);
            std::string line;
            std::getline(of, line);
            const auto hdr = split(line, ',');
            int cUid = -1, cFree = -1, cCy = -1;
            for (size_t i = 0; i < hdr.size(); ++i) { if (hdr[i] == "uid") cUid = (int)i; else if (hdr[i] == "free") cFree = (int)i; else if (hdr[i] == "cy") cCy = (int)i; }
            while (cUid >= 0 && cCy >= 0 && std::getline(of, line)) {
                const auto v = split(line, ',');
                if ((int)v.size() <= std::max(cUid, cCy)) continue;
                portalFree[std::atoi(v[(size_t)cUid].c_str())] = {cFree >= 0 && (int)v.size() > cFree && v[(size_t)cFree] == "1", (float)std::atof(v[(size_t)cCy].c_str())};
            }
        }
        {
            std::ifstream rf(dir + "result.txt");
            std::string rl;
            while (std::getline(rf, rl)) if (rl.rfind("maxplayy: ", 0) == 0) { const size_t p = rl.find(" v="); if (p != std::string::npos) { ceilOwn = (float)std::atof(rl.c_str() + p + 3); ceilKnown = true; } break; }   // (the first maxplayy line: the level's value at t=1)
        }
    }
    long long compared = 0, badEntry = 0, badExit = 0, badNext = 0, otherCalls = 0;
    // A capture of a run that ended in a death: result.txt beside the files says at which tick. The shadow stops at the kill (destroyPlayer's own writes -- +0x9c8, the previous position, the dirty bytes -- are not modelled), so at that step it is asked one thing: did it die too. A death anywhere else is a false death.
    // the Options triggers' timeline (gd-04's trig_replay --hidden: "player N controls disabled|enabled in step T"), from --events <file> or ST_EVENTS or events.txt beside objrects.txt: player 1's lines are the shadow's input
    std::map<long long, int> ctrlEvents;
    std::map<long long, float> gravEvents;   // "gravity V (player 0, trigger U) in step T": player 1's +0xb8c := V in step T (+ ST_GRAVSHIFT)
    std::set<long long> tpSpawn;   // the steps whose Teleport trigger is spawn-fired: it runs between the end of one step and the next, so the previous-position floats keep the old position
    std::map<long long, int> tpGameEvents;   // "teleport (the game's call) trigger U (id 3022) in step T": the game's own calls; when a timeline has any, they replace the shadow's "fired" lines
    std::map<long long, int> tpEvents;  // "trigger N fired in step T" (a Teleport trigger, from trigfire): the shadow's step is T + ST_TRIGSHIFT (default 1)
    {
        std::string ev = std::getenv("ST_EVENTS") ? std::getenv("ST_EVENTS") : "";
        if (ev.empty()) { std::string dd = argv[2]; const size_t ds = dd.find_last_of("/\\"); dd = ds == std::string::npos ? "" : dd.substr(0, ds + 1); std::ifstream probe(dd + "events.txt"); if (probe) ev = dd + "events.txt"; }
        if (!ev.empty()) {
            std::ifstream ef(ev); std::string el;
            while (std::getline(ef, el)) {
                int pl = 0; char what[16] = {0}; long long st = 0;
                { float gv = 0; int gp = 0, gu = 0; long long gs = 0; if (std::sscanf(el.c_str() + (el.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0), "gravity %f (player %d, trigger %d) in step %lld", &gv, &gp, &gu, &gs) == 4) { if (gp == 0) gravEvents[gs + (std::getenv("ST_GRAVSHIFT") ? std::atoll(std::getenv("ST_GRAVSHIFT")) : 0)] = gv; continue; } }
                { int tu = 0; long long ts = 0; const char* gc = std::strstr(el.c_str(), "(the game's call) trigger"); if (gc && std::sscanf(gc, "(the game's call) trigger %d (id 3022) in step %lld", &tu, &ts) == 2) { { const bool sp = std::strstr(el.c_str(), "spawn=1") != nullptr; const long long sh1 = std::getenv("ST_TRIGSHIFT") ? std::atoll(std::getenv("ST_TRIGSHIFT")) : (sp ? -1 : 0); tpGameEvents[ts + sh1] = tu; if (sp) tpSpawn.insert(ts + sh1); } continue; } }
                { int tu = 0; long long ts = 0; const char* tp = std::strstr(el.c_str(), "trigger"); if (tp && std::sscanf(tp, "trigger %d fired in step %lld", &tu, &ts) == 2) { tpEvents[ts + (std::getenv("ST_TRIGSHIFT") ? std::atoll(std::getenv("ST_TRIGSHIFT")) : 0)] = tu; continue; } }
                const char* pp = std::strstr(el.c_str(), "player");   // (the first line can carry a byte-order mark)
                if (pp && std::sscanf(pp, "player %d controls %15s in step %lld", &pl, what, &st) == 3 && pl == 1) ctrlEvents[st] = std::string(what) == "disabled" ? 1 : 2;
            }
            if (!tpGameEvents.empty()) tpEvents = tpGameEvents;
            if (rngAttemptStart && !passes.empty()) { const long long first = passes.begin()->first; for (const auto& kv : tpEvents) { if (kv.first >= first) break; auto ex = sh.tpExit.find(kv.second); if (ex != sh.tpExit.end() && ex->second.size() > 1) sh.rng = sh.rng * 0x343fdu + 0x269ec3u; } }
        }
    }
    long long deathTick = -1, deathAgree = 0, deathMiss = 0, falseDeath = 0, firstFalseDeath = -1; int deathWhy = 0, deathUid = 0;
    {
        std::string rd = argv[2];
        const size_t rs = rd.find_last_of("/\\");
        rd = rs == std::string::npos ? "" : rd.substr(0, rs + 1);
        std::ifstream rf(rd + "result.txt");
        std::string rl;
        while (std::getline(rf, rl)) if (rl.rfind("death: attempt=1 tick=", 0) == 0) { deathTick = std::atoll(rl.c_str() + 22); break; }
    }
    std::map<size_t, long long> wEntry, wExit, wNext;
    std::map<int, long long> unsup;
    int shown = 0;
    bool seeded = false;
    const long long firstHead = [&] { for (auto& kv : ticks) if (kv.second.haveHead) return kv.first; return 0LL; }();
    for (auto& pk : passes) {
        const long long t = pk.first;
        if (t > to || t < firstHead) continue;
        const bool counting = t >= from;   // a capture that starts before --from (btraw / ctraw from the last input before the window): the shadow runs from the first head it has and only the steps from --from on are compared and counted
        if (!counting && std::getenv("ST_NOWARM")) continue;
        TickRows empty;
        auto tit = ticks.find(t);
        const TickRows& tr0 = tit != ticks.end() ? tit->second : empty;
        if (!open && !seeded && !tr0.haveHead) continue;   // the closed form starts from a head image: a start tick without one used to run the shadow on an empty image and call the window IDENTICAL (found 2026-10-29)
        auto pit = passes.find(t);
        if (open && !tr0.haveHead) continue;
        std::pair<const long long, TickRows> kv(t, tr0);
        const Snap& head = kv.second.head;
        if (!seeded || open) {
            sh.img.b = head.img;
            if (head.hasSlots && !std::getenv("ST_NOSLOTS")) {   // the image holds the game's heap addresses in its object slots; the shadow keeps uid + 1 there: the recorded uids put the objects the game means back (a cube's SnappedTo, the ground, the ceiling...)
                for (size_t off : {(size_t)0x5e8, (size_t)0x5f0, (size_t)0x600, (size_t)0x608, (size_t)0x610, (size_t)0x618, (size_t)0x680, (size_t)0x890}) {
                    auto sit = head.slots.find(off);
                    sh.img.put<int64_t>(off, sit == head.slots.end() ? 0 : (int64_t)sit->second + 1);
                }
            }
            sh.rt.arr.clear();
            for (const auto& r : head.arr) sh.rt.arr.push_back(r.uid);
            sh.rt.now = head.touched;
            sh.usedRings = head.used;
            for (int k = 1; k <= 3; ++k) sh.holding[k] = head.hold.size() >= 3 && head.hold[(size_t)k - 1] == '1';
            sh.layerTime = sh.img.f64(po::Time) - sh.stepSeconds;
            if (!seeded) { if (!wt.empty()) applyMovers(t + wtShift - 1); sh.seedMemory(); if (const char* wu = std::getenv("ST_WATCH")) std::fprintf(stderr, "seed t=%lld ever has %s: %d gate: %d\n", t, wu, (int)sh.ever.count(std::atoi(wu)), (int)sh.gate.count(std::atoi(wu))); }   // the seed looks at the objects where they are now, not where the level put them
            seeded = true;
            if (dualOn && !ds.everBorn && pit->second.hasL && pit->second.L.dual) {   // a window that starts inside a dual: player 2 from its own head image (btraw who = 2), seeded as player 1 is
                auto t2 = ticks2.find(t);
                if (t2 != ticks2.end() && t2->second.haveHead) {
                    const Snap& h2 = t2->second.head;
                    ds.seedTwo(h2.img);
                    swapPlayer(sh, ds.two);
                    if (h2.hasSlots) for (size_t off : {(size_t)0x5e8, (size_t)0x5f0, (size_t)0x600, (size_t)0x608, (size_t)0x610, (size_t)0x618, (size_t)0x680, (size_t)0x890}) { auto sit = h2.slots.find(off); sh.img.put<int64_t>(off, sit == h2.slots.end() ? 0 : (int64_t)sit->second + 1); }
                    sh.rt.arr.clear(); for (const auto& r : h2.arr) sh.rt.arr.push_back(r.uid);
                    sh.rt.now = h2.touched; sh.usedRings = h2.used;
                    for (int k = 1; k <= 3; ++k) sh.holding[k] = h2.hold.size() >= 3 && h2.hold[(size_t)k - 1] == '1';
                    sh.seedMemory();
                    swapPlayer(sh, ds.two);
                    std::printf("dual: player 2 seeded from its head image at t=%lld\n", t);
                } else std::printf("dual: the window starts inside a dual with no player-2 head image at t=%lld (player 2 not modelled)\n", t);
            }
            if ((rtMode & 1) && rtTmpl.empty()) { rtTmpl = sh.img.b; rtSl = ShadowLite{}; slSeedRule(sh, rtW, rtSl); slPack(sh, rtW, rtSl); }   // the seed's image is the template
        }
        SelfIn in;
        in.tick = t;
        in.press1 = kv.second.press ? 1 : 0;
        if (!std::getenv("ST_MODFRAMES")) { in.dt = (float)(((1.0 / 60.0) / pit->second.n) * 60.0); sh.stepSeconds = (double)in.dt / 60.0; }   // the clock advances by the float dt over 60 (a frame of 40 steps: 0.0004166666728764312)
        if (!std::getenv("ST_MODFRAMES")) { in.head = headTicks.count(t) ? 1 : 0; in.end = passes.count(t + 1) ? (headTicks.count(t + 1) ? 1 : 0) : -1; }   // ST_MODFRAMES=1: the old fixed four-step frames
        in.release1 = kv.second.release ? 1 : 0;        if (std::getenv("ST_CTRLAUTO")) {   // ST_CTRLAUTO=1: each event's phase from the game's own bytes -- if +0xa33 already has the new value at the exit of the previous pass the event ran inside it (apply there, "late"), else between the passes (apply before the buttons)
            auto ce = ctrlEvents.find(t + 1);
            auto pp = passes.find(t);
            if (ce != ctrlEvents.end() && pp != passes.end() && pp->second.post.size() > 0xa34 && pp->second.pre.size() > 0xa34 && pp->second.post[0xa33] == (ce->second == 1 ? 1 : 0) && pp->second.pre[0xa33] != pp->second.post[0xa33]) { in.ctrl = ce->second; in.ctrlLate = 1; }
            else { auto ce2 = ctrlEvents.find(t); if (ce2 != ctrlEvents.end()) in.ctrl = ce2->second; }
        } else if (std::getenv("ST_CTRLAFTER")) { auto ce = ctrlEvents.find(t + 1); if (ce != ctrlEvents.end()) in.ctrlAfter = ce->second; }   // ST_CTRLAFTER=1: the event runs after the previous step's pass was recorded (a spawn / timed Options trigger)
        else if (std::getenv("ST_CTRLLATE")) { auto ce = ctrlEvents.find(t + 1); if (ce != ctrlEvents.end()) { in.ctrl = ce->second; in.ctrlLate = 1; } }   // ST_CTRLLATE=1: the event is applied at the END of the previous step's pass (a touch trigger: the game's byte changes inside the pass)
        else { auto ce = ctrlEvents.find(t); if (ce != ctrlEvents.end()) in.ctrl = ce->second; }
        { auto te = tpEvents.find(t); if (te != tpEvents.end() && (tpSpawn.count(t) || std::getenv("ST_TPHEAD")) && !(tpSpawn.count(t) && std::getenv("ST_TPEND"))) { in.tpUid = te->second; in.tpSpawn = tpSpawn.count(t) ? 1 : 0; } }
        { auto te = tpEvents.find(t + 1); if (te != tpEvents.end() && tpSpawn.count(t + 1) && std::getenv("ST_TPEND")) in.tpEndUid = te->second; }   // ST_TPEND=1: a spawn-fired trigger at the very end of the step before (the next head has it)   // a spawn-fired trigger runs at the head of its step
        { auto te = tpEvents.find(t + 1); if (te != tpEvents.end() && !tpSpawn.count(t + 1) && !std::getenv("ST_TPHEAD")) in.tpLateUid = te->second; }   // an x-crossing one at the end of the step before (checkSpawnObjects follows the pass)
        { auto ge = gravEvents.find(t); if (ge != gravEvents.end()) { in.hasGrav = 1; in.gravMod = ge->second; } }
        otherCalls += kv.second.other;
        sh.worldPhase = [&](long long T) {
            if (!wt.empty()) applyMovers(T + wtShift);
            auto hit = hwWrites.find(T - 1);   // the trigger writes a game run recorded (hwwatch.csv): measured to land after the collision pass of the tick it is labelled with (post image old, next pre image new), i.e. before the next update
            if (hit != hwWrites.end()) { sh.img.put<uint32_t>(hwOffset, hit->second); ++hwUsed; }
        };
        if (!wt.empty()) applyMovers(t + wtShift - 1);   // the buttons phase runs before the layer moves the objects: they are where the last step left them
        static const bool layerFreeze = std::getenv("ST_LAYERFREEZE") != nullptr;   // ST_LAYERFREEZE=1 (S2 stage 1, fair arm): the layer words below are taken from ctraw at the shadow's first step only (tick F, the model's start) and held
        static bool layerTaken = false;                                              // after that; what the game's layer does to them inside the window the shadow must work out itself, as the model does
        if (pit->second.hasL && !(layerFreeze && layerTaken)) {   // copied: speedA/B, freeMode, dual, w764, flipAmt, f317c/e, scaleX/Y, ceilWord, maxPortalY/minPortalY (f1cf stays the levelsettings value)
            const bool f1 = sh.L.f1cf; sh.L = pit->second.L; sh.L.f1cf = f1;
            if (layerFreeze) { layerTaken = true; std::fprintf(stderr, "layerfreeze: layer taken at t=%lld\n", t); }
        }
        DumpLayer dl; bool dlHave = false; float zoomIn = 0.0f;
        if (ownLayer && pit->second.hasL) {   // ST_OWNLAYER: the ported words are the shadow's (the rest stays as ctraw has it, or as ST_LAYERFREEZE took it at the first step)
            const Layer& G = pit->second.L;
            static DumpLayer dlFrozen; static bool dlFrozenHave = false, dlTaken = false;   // with ST_LAYERFREEZE the dump's inputs (camOffY, +0x23c / +0x2a0) are held from the first step as well
            if (!(layerFreeze && dlTaken)) { auto di = dumpLayer.find(t); dlFrozenHave = di != dumpLayer.end(); if (dlFrozenHave) dlFrozen = di->second; dlTaken = true; }
            dl = dlFrozen; dlHave = dlFrozenHave;
            zoomIn = sh.L.speedA;   // the zoom the ported band divides by: ctraw's, or the frozen one
            const bool branchA = dl.bandst != 0.0f && dl.bandmode == 0;
            if (!ownSeeded) { own.seed(G.freeMode, pit->second.f312, G.dual, pit->second.f310, pit->second.c308, G.w764, G.speedA, dl.camOffY, branchA || !dlHave, G.minPortalY, G.maxPortalY); own.seedFlip(pit->second.f418, G.flipAmt); ownSeeded = true; }
            else own.advanceFlip((float)sh.stepSeconds);   // the mirror tween advances once per step before the pass
            if (own.flipKnown) sh.L.flipAmt = own.flipAmt; else ++flipCopied;
            sh.L.freeMode = own.freeMode;
            sh.L.dual = own.dual;
            if (ceilKnown) sh.L.ceilWord = ceilOwn;
            float mn = 0.0f, mx = 0.0f;
            if (dlHave && own.band(zoomIn, dl.camOffY, branchA, mn, mx)) { sh.L.minPortalY = mn; sh.L.maxPortalY = mx; }
            else ++bandCopied[!dlHave ? 3 : branchA ? 0 : own.dual ? 1 : 2];
            if (own.w764Known) sh.L.w764 = own.w764; else ++w764Copied;
            if (counting) {
                ++ownSteps;
                auto cmp = [&](OwnCmp& c, bool same) { if (!same) { ++c.bad; if (c.first < 0) c.first = t; } };
                auto feq = [](float a, float b) { return std::memcmp(&a, &b, 4) == 0; };
                cmp(cmpFree, sh.L.freeMode == G.freeMode); cmp(cmpDual, sh.L.dual == G.dual);
                cmp(cmpMin, feq(sh.L.minPortalY, G.minPortalY)); cmp(cmpMax, feq(sh.L.maxPortalY, G.maxPortalY));
                cmp(cmpW764, feq(sh.L.w764, G.w764)); cmp(cmpCeil, feq(sh.L.ceilWord, G.ceilWord)); cmp(cmpFlip, feq(sh.L.flipAmt, G.flipAmt));
                if (std::getenv("ST_OWNLAYERV") && !(sh.L.freeMode == G.freeMode && sh.L.dual == G.dual && feq(sh.L.minPortalY, G.minPortalY) && feq(sh.L.maxPortalY, G.maxPortalY) && feq(sh.L.w764, G.w764)))
                    std::fprintf(stderr, "ownlayer t=%lld free %d/%d dual %d/%d min %.9g/%.9g max %.9g/%.9g w764 %.9g/%.9g (mine/game) centre %.9g H %.9g known %d zoom %.9g off %.9g A %d\n", t, (int)sh.L.freeMode, (int)G.freeMode, (int)sh.L.dual, (int)G.dual, sh.L.minPortalY, G.minPortalY, sh.L.maxPortalY, G.maxPortalY, sh.L.w764, G.w764, own.centre, own.height, (int)own.heightKnown, zoomIn, dl.camOffY, (int)branchA);
            }
        }
        if (const char* mv = std::getenv("ST_MOVER")) {   // ST_MOVER=uid:tick -- the mover as the pass will see it (cx cy and last cx cy), to compare two runs that disagree
            int mu = 0; long long mt = 0;
            if (std::sscanf(mv, "%d:%lld", &mu, &mt) == 2 && t == mt) for (const GObj& o : objs) if (o.uid == mu) std::fprintf(stderr, "mover %d at t=%lld before the step: c=(%.6f,%.6f) last=(%.6f,%.6f) rect y %.6f h %.6f\n", mu, t, o.cx, o.cy, o.prevCx, o.prevCy, o.ry, o.rh);
        }
        if ((rtMode & 2) && tr0.haveHead) {   // the literal round trip of this tick's head image (the world as it stands; the main shadow is not touched)
            const Snap& hd = tr0.head;
            SelfShadow& a = rtLit;
            a.img.b = hd.img;
            if (hd.hasSlots) for (size_t off : {(size_t)0x5e8, (size_t)0x5f0, (size_t)0x600, (size_t)0x608, (size_t)0x610, (size_t)0x618, (size_t)0x680, (size_t)0x890}) { auto sit = hd.slots.find(off); a.img.put<int64_t>(off, sit == hd.slots.end() ? 0 : (int64_t)sit->second + 1); }
            a.rt.arr.clear(); for (const auto& r : hd.arr) a.rt.arr.push_back(r.uid);
            a.rt.now = hd.touched; a.usedRings = hd.used;
            for (int k = 1; k <= 3; ++k) a.holding[k] = hd.hold.size() >= 3 && hd.hold[(size_t)k - 1] == '1';
            a.seedMemory();
            if (litTmpl.empty()) litTmpl = a.img.b;
            ShadowLite q;
            slSeedRule(a, rtLitW, q);
            rtLitW.actIdx.clear();
            slPack(a, rtLitW, q);
            rtLitW.actIdx.clear();   // (unpack then only sets flags pack found set: the world is not changed)
            slUnpack(q, litTmpl, rtLitW, rtBack, -3.0e38f);
            ++litHeads;
            if (q.flags & kSlOverflow) ++litOver;
            bool wb = false;
            for (size_t o = 0; o + 4 <= 0xc80; o += 4) {
                if (std::memcmp(rtBack.img.b.data() + o, a.img.b.data() + o, 4) == 0) continue;
                if (slCarried(o)) wb = true; else litUncarried.insert(o);
            }
            if (wb) ++litWordsBad;
            bool hb = false; for (int k = 1; k <= 3; ++k) hb |= a.holding[k] != rtBack.holding[k];
            if (a.gate != rtBack.gate || a.usedRings != rtBack.usedRings || a.rt.arr != rtBack.rt.arr || a.rt.now != rtBack.rt.now || hb) ++litSetsBad;
            if (!(q.flags & kSlOverflow) && a.ever != rtBack.ever) ++litEverBad;
        }
        if ((rtMode & 1) && !rtTmpl.empty()) slUnpack(rtSl, rtTmpl, rtW, sh, -3.0e38f);
        SelfOut out, outH2, out2;
        if (dualOn) { ds.head(in, out, outH2); ds.tail(in, out, out2); }
        else out = sh.step(in);
        if ((rtMode & 1) && !rtTmpl.empty()) {
            slPack(sh, rtW, rtSl);
            ++rtSteps;
            if ((rtSl.flags & kSlOverflow) && rtFirstOver < 0) rtFirstOver = t;
            if ((rtSl.flags & kSlAmbiguous) && rtFirstAmb < 0) rtFirstAmb = t;
            if (rtSl.flags & kSlOverflow) ++rtOver;
            if (rtSl.flags & kSlAmbiguous) ++rtAmb;
            rtMaxEver = std::max(rtMaxEver, (int)rtSl.nEver); rtMaxAct = std::max(rtMaxAct, (int)rtSl.nAct); rtMaxGate = std::max(rtMaxGate, (int)rtSl.nGate);
            rtMaxUsed = std::max(rtMaxUsed, (int)rtSl.nUsed); rtMaxArr = std::max(rtMaxArr, (int)rtSl.nArr); rtMaxNow = std::max(rtMaxNow, (int)rtSl.nNow);
        }
        if (ownLayer && ownSeeded && pit->second.hasL) {   // the portals the pass took, in its order, then +0x764 := getMaxPortalY() at the step's end
            for (const auto& f : out.fired) {
                if (f.first < 1000) continue;
                const int ty = f.first - 1000;
                const auto pf = portalFree.find(f.second);
                const bool fr = pf != portalFree.end() && pf->second.first;
                const float sy = pf != portalFree.end() ? pf->second.second : 0.0f;
                if (std::getenv("ST_OWNLAYERV") && (ty == 5 || ty == 6 || ty == 0x10 || ty == 0x13 || ty == 0x1a || ty == 0x1b || ty == 0x21 || ty == 0x29 || ty == 0x17 || ty == 0x18)) std::fprintf(stderr, "ownlayer portal t=%lld type %d uid %d free %d y %.9g\n", t, ty, f.second, (int)fr, sy);
                if (ty == 5 || ty == 6 || ty == 0x10 || ty == 0x13 || ty == 0x1a || ty == 0x1b || ty == 0x21 || ty == 0x29) { own.modePortal(ty, f.second, sy, fr, false); ++ownPortals; }
                else if ((ty == 0xe || ty == 0xf) && !sh.L.f317c) { own.mirrorPortal(ty == 0xe, false); ++ownPortals; if (std::getenv("ST_OWNLAYERV")) std::fprintf(stderr, "ownlayer mirror t=%lld type %d uid %d\n", t, ty, f.second); }
                else if (ty == 0x17 || ty == 0x18) { own.dualPortal(ty == 0x17, sy, fr, false, ShadowLayer::modeTypeOf(sh.img)); ++ownPortals; }
            }
            // the static camera's branch as the step ends: checkSpawnObjects (which runs a Static Camera trigger) comes before the +0x764 store, so it is the next
            // dump row's (measured: with this row's, the first step after a switch to branch A is one ulp-run off in 3 windows); camOffY stays this row's
            DumpLayer dn = dl; bool dnHave = dlHave;
            if (!layerFreeze) { auto di = dumpLayer.find(t + 1); if (di != dumpLayer.end()) { dn.bandst = di->second.bandst; dn.bandmode = di->second.bandmode; } }
            const bool branchA = dn.bandst != 0.0f && dn.bandmode == 0;
            float mn = 0.0f, mx = 0.0f;
            own.w764Known = dnHave && own.band(zoomIn, dl.camOffY, branchA, mn, mx);
            if (own.w764Known) own.w764 = mx;
        }
        static FILE* const stOut = std::getenv("ST_STATE") ? std::fopen(std::getenv("ST_STATE"), "w") : nullptr;
        if (stOut) {   // ST_STATE=<file> (S2 stage 1, tools/s2_stage1.py): the shadow's own state per step -- after the buttons (b*) and at the pass's exit (e*) -- one row per step, warm-up included; nothing else changes
            static bool hdr = false;
            if (!hdr) { std::fprintf(stOut, "tick,counting,bx,by,bvy,ex,ey,evy,mode,upside,size,dead,died,why,whyuid\n"); hdr = true; }
            const Mem& B = sh.atButtons; const Mem& E = sh.atExit;
            auto modeOf = [](const Mem& m) { return m.u8(po::ModeShip) ? "ship" : m.u8(po::ModeUfo) ? "ufo" : m.u8(po::ModeBall) ? "ball" : m.u8(po::ModeWave) ? "wave" : m.u8(po::ModeRobot) ? "robot" : m.u8(po::ModeSpider) ? "spider" : m.u8(po::ModeSwing) ? "swing" : "cube"; };
            std::fprintf(stOut, "%lld,%d,%.9g,%.9g,%.17g,%.9g,%.9g,%.17g,%s,%d,%.9g,%d,%d,%d,%d\n", t, counting ? 1 : 0, (double)B.f32(po::PosX), (double)B.f32(po::PosY), B.f64(po::Vy), (double)E.f32(po::PosX), (double)E.f32(po::PosY), E.f64(po::Vy), modeOf(B), (int)B.u8(po::Upside), (double)B.f32(0x9f8), (int)E.u8(0x9c8), out.died ? 1 : 0, out.why, out.whyUid);
            std::fflush(stOut);
        }
        if (touchCmp) { std::set<int> now; for (const auto& f : out.fired) { const int ty = f.first % 1000; if ((ty == 0x14 || ty == 0x2d) && touchUid.count(f.second)) now.insert(f.second); } touchOverlap[t] = now; if (const char* tu = std::getenv("ST_TOUCHUID")) if (now.count(std::atoi(tu))) std::printf("  overlap t=%lld uid %s\n", t, tu); touchLast = t; { dp::gdtouch::TouchDecider& d = touchDec; if (d.touchUids.empty()) d.touchUids = touchUid; for (int u : d.step(out.fired)) touchCalls.insert({t, u}); } if (touchFirst < 0) touchFirst = t; }
        for (const auto& u : out.unsupportedBy) unsup[u.first] += u.second;
        if (verbose) { for (const auto& f : out.fired) { int id = -1; for (const GObj& o : objs) if (o.uid == f.second) id = o.objectId; std::printf("  t=%lld fired type %d uid %d id %d%s\n", t, f.first % 1000, f.second, id, f.first >= 1000 ? " (acted)" : ""); } }
        if (!counting) continue;
        if (dualOn) {   // player 2: the game's pass rows (who = 2) against the shadow's player 2, by tick; a pass the game ran and the shadow did not (or the reverse) counts on its own
            auto p2 = passes2.find(t);
            const bool game2 = p2 != passes2.end(), mine2 = out2.ran;
            if (game2 && !mine2) { ++p2Missing; if (p2FirstMissing < 0) p2FirstMissing = t; }
            else if (!game2 && mine2) { ++p2Extra; if (p2FirstExtra < 0) p2FirstExtra = t; }
            else if (game2 && mine2) {
                ++p2Compared;
                const auto d2E = diff(ds.two.atEntry, p2->second.pre);
                const auto d2X = diff(ds.two.atExit, p2->second.post);
                if (!d2E.empty()) ++p2BadEntry;
                if (!d2X.empty()) ++p2BadExit;
                for (size_t w : d2E) ++w2Entry[w];
                for (size_t w : d2X) ++w2Exit[w];
                if ((!d2E.empty() || !d2X.empty()) && p2First < 0) p2First = t;
                {   // ...byte by byte over the carried words: a byte that neither the game nor the shadow ever changes in the window is a constant (a newborn's heap leftovers in
                    // padding, or a constant the birth got wrong), reported apart from the bytes that move
                    for (int ph = 0; ph < 2; ++ph) {
                        const std::vector<uint8_t>& g = ph == 0 ? p2->second.pre : p2->second.post;
                        const std::vector<uint8_t>& m = ph == 0 ? ds.two.atEntry.b : ds.two.atExit.b;
                        if (g.size() < 0xc50 || m.size() < 0xc50) continue;
                        for (size_t b = 0; b < 0xc50; ++b) {
                            if (!slCarried(b) || ignored(b)) continue;   // (the pointer slots and the rest of the ignore list, as for player 1)
                            P2Byte& pb = p2Bytes[b];
                            if (!pb.seen) { pb.seen = true; pb.g0 = g[b]; pb.m0 = m[b]; }
                            if (g[b] != pb.g0) pb.gMoved = true;
                            if (m[b] != pb.m0) pb.mMoved = true;
                            if (g[b] != m[b]) { ++pb.bad; if (pb.first < 0) pb.first = t; }
                        }
                    }
                    auto F = [](const std::vector<uint8_t>& v, size_t o) { float x; std::memcpy(&x, v.data() + o, 4); return x; };
                    auto D = [](const std::vector<uint8_t>& v, size_t o) { double x; std::memcpy(&x, v.data() + o, 8); return x; };
                    const auto& g = p2->second.post; const auto& m = ds.two.atExit.b;
                    if (g.size() >= 0xa00 && m.size() >= 0xa00 && p2PhysFirst < 0 && (F(g, 0x4c) != F(m, 0x4c) || F(g, 0x50) != F(m, 0x50) || D(g, 0x9a8) != D(m, 0x9a8) || g[0x9c7] != m[0x9c7]))
                        { p2PhysFirst = t; p2PhysTxt = "x " + std::to_string(F(g, 0x4c)) + "/" + std::to_string(F(m, 0x4c)) + " y " + std::to_string(F(g, 0x50)) + "/" + std::to_string(F(m, 0x50)) + " vy " + std::to_string(D(g, 0x9a8)) + "/" + std::to_string(D(m, 0x9a8)) + " up " + std::to_string(g[0x9c7]) + "/" + std::to_string(m[0x9c7]) + " (game/mine, exit)"; }
                }
                {   // ...and only the words a step carries to the next (shadow_state.hpp slCarried, the ShadowLite footprint): a newborn player 2 has the game's heap leftovers
                    // and pointers in the rest, which no step reads
                    bool bad = false;
                    for (size_t w : d2E) if (slCarried(w)) { bad = true; ++w2cEntry[w]; }
                    for (size_t w : d2X) if (slCarried(w)) { bad = true; ++w2cExit[w]; }
                    if (bad) { ++p2BadCarried; if (p2FirstCarried < 0) p2FirstCarried = t; }
                }
                if ((!d2E.empty() || !d2X.empty()) && p2Shown < show) {
                    ++p2Shown;
                    const bool en = !d2E.empty();
                    const auto& first = en ? d2E : d2X;
                    const std::vector<uint8_t>& ref = en ? p2->second.pre : p2->second.post;
                    const Mem& mine = en ? ds.two.atEntry : ds.two.atExit;
                    std::printf("p2 t=%lld: entry %zu words, exit %zu words; first (%s):", t, d2E.size(), d2X.size(), en ? "entry" : "exit");
                    int n = 0;
                    for (size_t w : first) {
                        if (std::getenv("ST_P2CARRIED") && !slCarried(w)) continue;   // ST_P2CARRIED=1: only the carried words
                        if (n++ >= (std::getenv("ST_P2WORDS") ? 400 : 8)) { std::printf(" ..."); break; }   // ST_P2WORDS=1: every word
                        uint32_t g, m;
                        std::memcpy(&g, ref.data() + w, 4);
                        std::memcpy(&m, mine.b.data() + w, 4);
                        std::printf(" %zx(game %08x mine %08x)", w, g, m);
                    }
                    std::printf("\n");
                }
            }
        }
        ++compared;
        if (t >= traceFrom && t <= traceTo) {
            auto F = [](const std::vector<uint8_t>& b, size_t o) { float x; std::memcpy(&x, b.data() + o, 4); return x; };
            auto D = [](const std::vector<uint8_t>& b, size_t o) { double x; std::memcpy(&x, b.data() + o, 8); return x; };
            auto line = [&](const char* who, const std::vector<uint8_t>& en, const std::vector<uint8_t>& ex) {
                std::printf("  t=%lld %-4s entry x %.4f y %.4f vy %.4f G %d G2 %d a98 %.3f %.3f up %d | exit y %.4f vy %.4f G %d G2 %d boost %d slope %d bot %d\n", t, who, F(en, 0x4c), F(en, 0x50), D(en, 0x9a8), en[0x9c9], en[0xa14], F(en, 0xa98), F(en, 0xa9c), en[0x9c7], F(ex, 0x50), D(ex, 0x9a8), ex[0x9c9], ex[0xa14], ex[0xa24], ex[0x9b8], *(int*)(ex.data() + 0x5d0));
            };
            line("game", pit->second.pre, pit->second.post);
            line("mine", sh.atEntry.b, sh.atExit.b);
        }
        if (const char* sw = std::getenv("ST_WORDS")) {   // ST_WORDS=tick:0xoff,0xoff -- the 8-byte words at those offsets, game vs mine, entry and exit (pointer slots are not compared otherwise)
            long long wt0 = 0; int pos = 0;
            if (std::sscanf(sw, "%lld:%n", &wt0, &pos) >= 1 && t == wt0) {
                std::string rest = sw + pos;
                for (const auto& tok : split(rest, ',')) {
                    const size_t off = (size_t)std::strtoull(tok.c_str(), nullptr, 16);
                    auto W = [&](const std::vector<uint8_t>& b) { unsigned long long x = 0; if (off + 8 <= b.size()) std::memcpy(&x, b.data() + off, 8); return x; };
                    std::fprintf(stderr, "words t=%lld +0x%zx: entry game %llx mine %llx | exit game %llx mine %llx\n", t, off, W(pit->second.pre), W(sh.atEntry.b), W(pit->second.post), W(sh.atExit.b));
                }
            }
        }
        if (physOn) { auto Fl = [](const std::vector<uint8_t>& b, size_t o) { float x; std::memcpy(&x, b.data() + o, 4); return (double)x; }; auto Db = [](const std::vector<uint8_t>& b, size_t o) { double x; std::memcpy(&x, b.data() + o, 8); return x; }; const double ddx = std::fabs(Fl(sh.atEntry.b, 0x4c) - Fl(pit->second.pre, 0x4c)), ddy = std::fabs(Fl(sh.atEntry.b, 0x50) - Fl(pit->second.pre, 0x50)), ddv = std::fabs(Db(sh.atEntry.b, 0x9a8) - Db(pit->second.pre, 0x9a8)); if (ddx > 0 || ddy > 0 || ddv > 0) { if (physFirst < 0) physFirst = t; physMaxX = std::max(physMaxX, ddx); physMaxY = std::max(physMaxY, ddy); physMaxV = std::max(physMaxV, ddv); ++physSteps; } }
        const auto dE = diff(sh.atEntry, pit->second.pre);
        auto dX = diff(sh.atExit, pit->second.post);
        std::vector<size_t> dN;
        const bool speedPending = sh.speedDirty;   // updateTimeMod runs at the head of the next step; the next head the game wrote already has it
        auto nit = ticks.find(t + 1);
        if (nit != ticks.end() && nit->second.haveHead && !(open == false && false)) {
            Mem nextImg = sh.img;   // the next head is written after updateTimeMod of that step: apply it here instead of excluding its words (it clears and re-sets the ball's spin too)
            if (speedPending) upd::updateTimeMod(nextImg, sh.pendingSpeed);
            dN = diff(nextImg, nit->second.head.img);
            dN.erase(std::remove_if(dN.begin(), dN.end(), [&](size_t w) { return (w >= 0xaa8 && w < 0xab0) || (w == 0x368 && (t + 1) % sh.frameLen == (sh.frameEndMod + 1) % sh.frameLen) ; }), dN.end());   // the clock the next head already advanced; the dirty bytes the next frame's head sets
        }
        if (deathTick >= 0 && t == deathTick) {
            if (out.died) { ++deathAgree; deathWhy = out.why; deathUid = out.whyUid; dX.clear(); dN.clear(); }   // the death step: only the kill is compared
            else ++deathMiss;
        } else if (out.died) { ++falseDeath; if (firstFalseDeath < 0) firstFalseDeath = t; }
        if (!dE.empty()) ++badEntry;
        if (!dX.empty()) ++badExit;
        if (!dN.empty()) ++badNext;
        for (size_t w : dE) ++wEntry[w];
        for (size_t w : dX) ++wExit[w];
        for (size_t w : dN) ++wNext[w];
        if ((!dE.empty() || !dX.empty() || !dN.empty()) && shown < show) {
            ++shown;
            std::printf("t=%lld press %d release %d%s: entry %zu words, exit %zu words, next-head %zu words;", t, (int)in.press1, (int)in.release1, out.died ? " [shadow died: line, uid]" : "", dE.size(), dX.size(), dN.size());
            if (out.died) std::printf(" (%d, %d)", out.why, out.whyUid);
            const auto& first = !dE.empty() ? dE : !dX.empty() ? dX : dN;
            const char* which = !dE.empty() ? "entry" : !dX.empty() ? "exit" : "next";
            const std::vector<uint8_t>& ref = !dE.empty() ? pit->second.pre : !dX.empty() ? pit->second.post : nit->second.head.img;
            const Mem& mine = !dE.empty() ? sh.atEntry : !dX.empty() ? sh.atExit : sh.img;
            std::printf(" first (%s):", which);
            int n = 0;
            for (size_t w : first) {
                if (n++ >= 8) { std::printf(" ..."); break; }
                uint32_t g, m;
                std::memcpy(&g, ref.data() + w, 4);
                std::memcpy(&m, mine.b.data() + w, 4);
                std::printf(" %zx(game %08x mine %08x)", w, g, m);
            }
            std::printf("\n");
        } else if (verbose) {
            std::printf("t=%lld ok\n", t);
        }
    }
    if (touchCmp && std::getenv("ST_TOUCHPTT")) {   // ST_TOUCHPTT=<result.txt of the capture's own run, cfg hitboxtrace=1>: the decider against the game's own playerTouchedTrigger calls (the right truth; trigfire.csv below is a record of when triggers EXECUTED, possibly of another flight)
        std::ifstream pIn(std::getenv("ST_TOUCHPTT"));
        const TouchTruth pt = loadPttTruth(pIn, touchUid);
        const TouchScore sc = scoreTouchCalls(touchCalls, pt.rows, touchFirst, touchLast, [](const std::pair<long long, int>&) { return true; });
        std::printf("touch decider vs ptt: matched %lld, extra %lld, missed %lld (steps %lld..%lld, %zu ptt calls of touch triggers in the file)\n", sc.hit, sc.extra, sc.miss, touchFirst + 1, touchLast, pt.rows.size());
        if (std::getenv("ST_TOUCHV")) {
            for (const auto& p : sc.extras) std::printf("  PTT-EXTRA tick %lld uid %d\n", p.first, p.second);
            for (const auto& p : sc.misses) std::printf("  PTT-MISSED tick %lld uid %d\n", p.first, p.second);
        }
    }
    if (touchCmp) {   // ST_TOUCHCMP=<trigfire.csv>: the touch triggers (objrects touch=1, type 0x14 / 0x2d) the pass overlapped, as rising edges of the overlap, against the game's own calls (trigfire.csv rows of those uids that no spawn made)
        std::fprintf(stderr, "touch: %zu touch=1 triggers in triggers.txt, %zu overlapped in some step\n", touchUid.size(), touchOverlap.size());
        std::ifstream tfIn(touchCmp);
        const TouchTruth tt = loadTouchTruth(tfIn, touchUid, std::getenv("ST_TOUCHATT") ? std::atoi(std::getenv("ST_TOUCHATT")) : -1);   // the last attempt unless ST_TOUCHATT=<n>: trig_replay reads the last one too
        const std::set<std::pair<long long, int>>& truth = tt.rows;
        { size_t inRange = 0; for (const auto& p : truth) if (p.first >= touchFirst - 1 && p.first <= touchLast) ++inRange; std::printf("touch truth: attempt %d of %d, %zu calls, %zu in the window's range\n", tt.attempt, tt.attempts, truth.size(), inRange); }
        {   // the decider (dp/src/dp/trigtouch.hpp): a call at trigfire tick T = the rising edge of the overlap in the pass of tick T; the first pass has no history, so it is not judged
            long long hit = 0, extra = 0, miss = 0;
            auto sameFlight = [&](const std::pair<long long, int>& p) { auto it = tt.px.find(p); auto pa = passes.find(p.first); if (it == tt.px.end() || pa == passes.end() || pa->second.pre.size() < 0x50) return true; float gx; std::memcpy(&gx, pa->second.pre.data() + 0x4c, 4); return std::fabs((double)gx - it->second) < 60.0; };   // a recording of another flight of the level has the player elsewhere at that tick
            long long otherFlight = 0;
            {   // one set of comparable truth rows for all three counts (touch_truth.hpp scoreTouchCalls; tools/touch_truth_test.cpp)
                const TouchScore sc = scoreTouchCalls(touchCalls, truth, touchFirst, touchLast, sameFlight);
                hit = sc.hit; extra = sc.extra; miss = sc.miss; otherFlight = sc.skipped;
                if (std::getenv("ST_TOUCHV")) {
                    for (const auto& p : sc.extras) { int id = -1; for (const GObj& o : objs) if (o.uid == p.second) id = o.objectId; std::printf("  EXTRA tick %lld uid %d id %d\n", p.first, p.second, id); }
                    for (const auto& p : sc.misses) { int id = -1; for (const GObj& o : objs) if (o.uid == p.second) id = o.objectId; std::printf("  MISSED tick %lld uid %d id %d\n", p.first, p.second, id); }
                }
            }
            std::printf("touch decider: matched %lld, extra %lld, missed %lld (steps %lld..%lld, attempt %d of %d; %lld truth rows skipped: the recording's player was elsewhere)\n", hit, extra, miss, touchFirst + 1, touchLast, tt.attempt, tt.attempts, otherFlight);
            if (const char* to = std::getenv("ST_TOUCHOUT")) { std::ofstream o(to, std::ios::app); o << std::getenv("ST_LABEL") << "\t" << hit << "\t" << extra << "\t" << miss << "\n"; }
        }
        for (int sh2 = -2; sh2 <= 3; ++sh2) {
            long long hit = 0, extra = 0, miss = 0; std::set<std::pair<long long, int>> pred;
            for (const auto& kv : touchOverlap) for (int u : kv.second) { auto pv = touchOverlap.find(kv.first - 1); if (pv != touchOverlap.end() && pv->second.count(u)) continue; pred.insert({kv.first - 1 - sh2, u}); }   // predicted trigfire tick
            for (const auto& p : pred) { if (p.first < touchFirst - 1 || p.first > touchLast) continue; if (truth.count(p)) ++hit; else ++extra; }
            for (const auto& p : truth) { if (p.first < touchFirst + 3 || p.first > touchLast - 3) continue; if (!pred.count(p)) ++miss; }
            if (sh2 == 0 && std::getenv("ST_TOUCHV")) { for (const auto& p : pred) if (p.first >= touchFirst - 1 && p.first <= touchLast) { int id = -1; for (const GObj& o : objs) if (o.uid == p.second) id = o.objectId; std::printf("  pred tick %lld uid %d id %d\n", p.first, p.second, id); } for (const auto& p : truth) if (p.first >= touchFirst - 5 && p.first <= touchLast + 5) { int id = -1; for (const GObj& o : objs) if (o.uid == p.second) id = o.objectId; std::printf("  truth tick %lld uid %d id %d\n", p.first, p.second, id); } }
            std::printf("touch rising edge, trigfire tick = step - 1 - (%d): matched %lld, extra %lld, missed %lld (truth rows in range %zu)\n", sh2, hit, extra, miss, truth.size());
        }
    }
    long long expected = 0, expFrom = -1, expTo = -1, firstHeadTick = -1;
    for (auto& pk : passes) { if (pk.first < from || pk.first > to) continue; ++expected; if (expFrom < 0) expFrom = pk.first; expTo = pk.first; }
    for (auto& kv : ticks) if (kv.second.haveHead && kv.first >= from && kv.first <= to) { firstHeadTick = kv.first; break; }
    const bool anyWrong = badEntry || badExit || badNext || deathMiss || falseDeath;
    const char* status = compared == 0 ? "UNOBSERVED" : anyWrong ? "DIFFERS" : "IDENTICAL";
    if (physOn) std::printf("phys: first tick with a position/velocity difference %lld, steps %lld, max |dx| %.6g, max |dy| %.6g, max |dvy| %.6g\n", physFirst, physSteps, physMaxX, physMaxY, physMaxV);
    if (ownLayer) {
        auto pr = [](const char* n, const OwnCmp& c) { std::printf(" %s %lld", n, c.bad); if (c.first >= 0) std::printf(" (first t=%lld)", c.first); };
        std::printf("ownlayer: steps %lld, portals %lld; mismatching steps vs ctraw:", ownSteps, ownPortals);
        pr("free", cmpFree); pr("dual", cmpDual); pr("min", cmpMin); pr("max", cmpMax); pr("w764", cmpW764); pr("ceil", cmpCeil); pr("flip", cmpFlip);
        std::printf("; band copied (branchA %lld, dual %lld, height unknown %lld, no dump row %lld), w764 copied %lld, flip copied %lld, ceil %s\n", bandCopied[0], bandCopied[1], bandCopied[2], bandCopied[3], w764Copied, flipCopied, ceilKnown ? "own" : "copied");
    }
    if (rtMode & 1) std::fprintf(stderr, "roundtrip: steps %lld overflow %lld (first %lld) ambiguous %lld (first %lld) max lists ever %d act %d gate %d used %d arr %d now %d seedMovers %zu everW %s\n", rtSteps, rtOver, rtFirstOver, rtAmb, rtFirstAmb, rtMaxEver, rtMaxAct, rtMaxGate, rtMaxUsed, rtMaxArr, rtMaxNow, rtW.seedMovers.size(), (rtSl.flags2 & kSlEverW) ? "on" : "off");
    if (rtMode & 2) {
        std::fprintf(stderr, "rtlit: heads %lld carried-words-bad %lld sets-bad %lld ever-bad %lld overflow %lld uncarried words that differ from the first head:", litHeads, litWordsBad, litSetsBad, litEverBad, litOver);
        for (size_t o : litUncarried) std::fprintf(stderr, " %zx", o);
        std::fprintf(stderr, "\n");
    }
    std::printf("status: %s (compared %lld of %lld ctraw steps %lld..%lld; first head step %lld)\n", status, compared, expected, expFrom, expTo, firstHeadTick);
    if (sh.platformerLevel) std::printf("platformer: not modelled (levelsettings platformerMode=1: updateMove, animatePlatformerJump, the jump ramp, +0x748, the left / right buttons are not ported)\n");
    if (deathTick >= 0) std::printf("death: game tick %lld, shadow %s (line %d, uid %d); false deaths %lld%s\n", deathTick, deathAgree ? "died at the same tick" : deathMiss ? "did NOT die" : "not reached in the window", deathWhy, deathUid, falseDeath, firstFalseDeath >= 0 ? (" (first at t=" + std::to_string(firstFalseDeath) + ")").c_str() : "");
    if (hwUsed) std::printf("injected: %lld game-recorded write(s) of +0x%zx from hwwatch.csv (the window's result is not the shadow's own)\n", hwUsed, hwOffset);
    std::printf("%s: ticks %lld, entry wrong %lld, exit wrong %lld, next head wrong %lld, other button calls not fed %lld\n", open ? "open" : "closed", compared, badEntry, badExit, badNext, otherCalls);
    auto top = [](const char* name, const std::map<size_t, long long>& m) {
        std::vector<std::pair<long long, size_t>> v;
        for (const auto& w : m) v.push_back({w.second, w.first});
        std::sort(v.rbegin(), v.rend());
        std::printf("  %s words:", name);
        int n = 0;
        for (const auto& p : v) { if (n++ >= 10) break; std::printf(" %zx:%lld", p.second, p.first); }
        std::printf("\n");
    };
    top("entry", wEntry); top("exit", wExit); top("next", wNext);
    if (dualOn) {
        long long game2 = 0;
        for (const auto& kv : passes2) if (kv.first >= from && kv.first <= to) ++game2;
        const char* st2 = p2Compared == 0 ? "UNOBSERVED" : (p2BadEntry || p2BadExit || p2Missing || p2Extra) ? "DIFFERS" : "IDENTICAL";
        std::printf("p2status: %s (compared %lld of the game's %lld player-2 passes; entry wrong %lld, exit wrong %lld, first wrong t=%lld; passes the shadow did not step %lld (first t=%lld), stepped that the game did not %lld (first t=%lld))\n",
                    st2, p2Compared, game2, p2BadEntry, p2BadExit, p2First, p2Missing, p2FirstMissing, p2Extra, p2FirstExtra);
        top("p2 entry", w2Entry); top("p2 exit", w2Exit);
        std::printf("p2carried: %s (passes with a carried word wrong %lld of %lld, first t=%lld)\n", p2Compared == 0 ? "UNOBSERVED" : (p2BadCarried || p2Missing || p2Extra) ? "DIFFERS" : "IDENTICAL", p2BadCarried, p2Compared, p2FirstCarried);
        top("p2 carried entry", w2cEntry); top("p2 carried exit", w2cExit);
        {
            long long liveFirst = -1; std::string live, cst;
            int nLive = 0, nConst = 0;
            for (const auto& kv : p2Bytes) {
                if (!kv.second.bad) continue;
                char b[48];
                std::snprintf(b, sizeof(b), " %zx(g%02x m%02x t%lld)", kv.first, kv.second.g0, kv.second.m0, kv.second.first);
                if (kv.second.gMoved || kv.second.mMoved) { ++nLive; if (nLive <= 24) live += b; if (liveFirst < 0 || kv.second.first < liveFirst) liveFirst = kv.second.first; }
                else { ++nConst; if (nConst <= 24) cst += b; }
            }
            std::printf("p2bytes: %s (carried bytes that move and differ %d, first t=%lld; constant on both sides and different %d)\n", p2Compared == 0 ? "UNOBSERVED" : (nLive || p2Missing || p2Extra) ? "DIFFERS" : "IDENTICAL", nLive, liveFirst, nConst);
            std::printf("  p2 moving bytes:%s\n  p2 constant bytes:%s\n", live.c_str(), cst.c_str());
            std::printf("p2phys: first x / y / vy / gravity difference t=%lld %s\n", p2PhysFirst, p2PhysTxt.c_str());
        }
    }
    for (const auto& u : unsup) std::printf("  unsupported[%d]: %lld\n", u.first, u.second);
    return compared == 0 ? 3 : (anyWrong ? 1 : 0);   // 0 only when something was compared and all of it matched; 3 = nothing observed
}
