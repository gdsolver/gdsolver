// collide_test: dp/src/dp/collide.hpp against the game's own entry / exit bytes of the collision pass (mod cfg ctraw=t0,t1 -> ctraw.csv).
//   collide_test <ctraw.csv> <objrects.txt> [--from T] [--to T] [--show N] [--mask name,...] [--verbose]
// For every row it builds the player image and layer words from the entry bytes, runs the ported pass on the objects around the player, and compares
// the exit bytes field by field (4-byte words). Prints the words that differ and how many ticks each differs on.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/collide_test.cpp
#include "dp/collide.hpp"

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

struct Row {
    long long tick = 0;
    int who = 1;
    float dt = 0.25f;
    bool ignore = false;
    int ret = 0;
    std::vector<uint8_t> preP, postP;
    std::string preRest, postRest;
};

static const size_t kPtrOffs[] = {0x5e8, 0x5f0, 0x600, 0x608, 0x610, 0x618, 0x678, 0x680, 0x890};

// image + pointer uids -> the in-image convention (uid + 1, 0 = null)
static Mem makeImage(const std::vector<uint8_t>& p, const std::string& rest) {
    Mem m(p.size());
    m.b = p;
    const auto parts = split(rest, ';');
    std::map<size_t, int> uid;
    {
        std::istringstream is(parts[1]);
        std::string tok;
        while (is >> tok) {
            const auto c = tok.find(':');
            uid[std::stoul(tok.substr(0, c), nullptr, 16)] = std::stoi(tok.substr(c + 1));
        }
    }
    for (size_t off : kPtrOffs) {
        auto it = uid.find(off);
        m.put<int64_t>(off, it == uid.end() || it->second < 0 ? 0 : (int64_t)it->second + 1);
    }
    return m;
}

static Layer makeLayer(const std::string& rest) {
    const auto parts = split(rest, ';');
    Layer L;
    auto f32 = [](const std::vector<uint8_t>& b, size_t o) { float v; std::memcpy(&v, b.data() + o, 4); return v; };
    const auto A = unhex(parts[2]), B = unhex(parts[3]), C = unhex(parts[4]), D = unhex(parts[5]), E = unhex(parts[6]);
    L.speedA = f32(A, 8);
    L.speedB = f32(A, 12);
    L.freeMode = B[0x311 - 0x300] != 0;
    L.dual = B[0x422 - 0x300] != 0;
    L.w764 = f32(C, 4);
    L.f317c = D[0xc] != 0;
    L.f317e = D[0xe] != 0;
    L.scaleX = f32(E, 0);
    L.scaleY = f32(E, 4);
    L.ceilWord = f32(E, 8);
    const auto pq = split(parts[7], ':');
    L.maxPortalY = std::stof(pq[0]);
    L.minPortalY = std::stof(pq[1]);
    return L;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: collide_test ctraw.csv objrects.txt [--from T] [--to T] [--show N] [--verbose]\n"); return 2; }
    long long from = 0, to = 1LL << 60;
    int show = 12;
    bool verbose = false, onlySup = false, skipStart = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from") from = std::atoll(argv[++i]);
        else if (a == "--to") to = std::atoll(argv[++i]);
        else if (a == "--show") show = std::atoi(argv[++i]);
        else if (a == "--verbose") verbose = true;
        else if (a == "--only-supported") onlySup = true;
        else if (a == "--skip-start") skipStart = true;
    }
    // objects
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
            o.rot = (float)num(v, "rot"); o.w0 = (float)num(v, "w0"); o.h0 = (float)num(v, "h0");
            objs.push_back(o);
        }
    }
    // The box of a rotated object: the per-id ratio of the unrotated hitbox to (w0, h0), read off the instances that are not rotated (or whose rotation can be solved).
    {
        std::map<int, std::pair<float, float>> ratio;
        for (auto& o : objs) {
            if (o.w0 <= 0 || o.h0 <= 0) continue;
            const bool quarter = std::fmod(std::fabs(o.rot), 90.0f) == 0.0f;
            if (quarter) {
                const bool odd = std::fmod(std::fabs(o.rot), 180.0f) != 0.0f;
                const float bw = odd ? o.rh : o.rw, bh = odd ? o.rw : o.rh;
                ratio[o.objectId] = {bw / (o.w0 * o.scaleX), bh / (o.h0 * o.scaleY)};
            } else if (!ratio.count(o.objectId)) {
                const float ang = -o.rot * 0.017453292f;
                const float cs = std::fabs(std::cos(ang)), sn = std::fabs(std::sin(ang)), det = cs * cs - sn * sn;
                if (std::fabs(det) > 0.01f) {
                    const float bw = (o.rw * cs - o.rh * sn) / det, bh = (o.rh * cs - o.rw * sn) / det;
                    ratio[o.objectId] = {bw / (o.w0 * o.scaleX), bh / (o.h0 * o.scaleY)};
                }
            }
        }
        for (auto& o : objs) {
            if (!o.oriented || std::fmod(std::fabs(o.rot), 90.0f) == 0.0f) continue;
            auto it = ratio.find(o.objectId);
            if (it != ratio.end()) { o.bw = it->second.first * o.w0 * o.scaleX; o.bh = it->second.second * o.h0 * o.scaleY; }
        }
    }    std::fprintf(stderr, "%zu objects\n", objs.size());
    std::ifstream f(argv[1]);
    std::string line;
    std::getline(f, line);
    long long rows = 0, bad = 0, supRows = 0, supBad = 0;
    std::map<size_t, long long> diffTicks;
    std::map<size_t, std::vector<long long>> diffAt;
    long long unsup = 0, diedMismatch = 0;
    std::map<int, long long> unsupBy;
    std::set<int> gate; long long gateTick = -10;
    std::set<int> ever;   // the once-per-contact gate carried from the previous tick when the rows are consecutive
    while (std::getline(f, line)) {
        const auto v = split(line, ',');
        if (v.size() < 8) continue;
        Row r;
        r.tick = std::atoll(v[1].c_str());
        if (r.tick < from || r.tick > to) continue;
        if (skipStart && r.tick <= 13) continue;   // the first thirteen ticks of a run: the game returns 1 from the pass while the level is still starting
        r.who = std::atoi(v[2].c_str());
        r.dt = (float)std::atof(v[3].c_str());
        r.ignore = std::atoi(v[4].c_str()) != 0;
        r.ret = std::atoi(v[5].c_str());
        const auto pre = split(v[6], ';'), post = split(v[7], ';');
        r.preP = unhex(pre[0]);
        r.postP = unhex(post[0]);
        Mem pimg = makeImage(r.preP, v[6]);
        const Mem gimg = makeImage(r.postP, v[7]);
        Ctx c(pimg);
        c.L = makeLayer(v[6]);
        c.objs = &objs;
        // candidates: everything whose rect is within 80 px of the player, in uid order
        float px, py, pw, ph;
        playerRect(pimg, px, py, pw, ph);
        std::vector<GObj*> cand;
        for (auto& o : objs)
            if (o.rx <= px + pw + 80 && px - 80 <= o.rx + o.rw && o.ry <= py + ph + 80 && py - 80 <= o.ry + o.rh) cand.push_back(&o);
        if (r.tick == gateTick + 1) c.gateIn = &gate;
        c.everIn = &ever;
        const int rv = checkCollisions(c, r.dt, r.ignore, cand);
        gate = c.gateNow; gateTick = r.tick;
        ever.insert(c.everNow.begin(), c.everNow.end());
        ++rows;
        unsup += c.unsupported;
        for (auto& kv : c.unsupportedBy) unsupBy[kv.first] += kv.second;
        bool any = false;
        const bool skipRow = onlySup && c.unsupported != 0;
        if ((rv != 0) != (r.ret != 0)) ++diedMismatch;
        for (size_t o = 0; o + 4 <= pimg.b.size(); o += 4) {
            if (!skipRow && std::memcmp(pimg.b.data() + o, gimg.b.data() + o, 4) != 0) {
                // ignore the sprite / node words the port does not model
                if ((o >= 0x358 && o < 0x370) || (o >= 0x9c && o < 0xa8) || (o >= 0x110 && o < 0x120) || (o >= 0x38 && o < 0x40) || o < 0x30 || (o >= 0xa88 && o < 0xa90) || (o >= 0xa58 && o < 0xa60) || (o >= 0x908 && o < 0x909) || (o >= 0x40 && o < 0x48) || (o >= 0x8c && o < 0x9c) || (o >= 0x980 && o < 0x981)) continue;
                diffTicks[o]++;
                if (diffAt[o].size() < 6) diffAt[o].push_back(r.tick);
                any = true;
            }
        }
        if (any) ++bad;
        if (c.unsupported == 0) { ++supRows; if (any) ++supBad; }
        if (verbose && any) {
            std::printf("t=%lld ret=%d/%d why=%d:", r.tick, rv, r.ret, c.why);
            for (auto& f : c.fired) std::printf(" <%d:%d>", f.first, f.second);
            std::printf(" ");
            for (size_t o = 0; o + 4 <= pimg.b.size(); o += 4)
                if (std::memcmp(pimg.b.data() + o, gimg.b.data() + o, 4) != 0 && !((o >= 0x358 && o < 0x370) || (o >= 0x9c && o < 0xa8) || (o >= 0x110 && o < 0x120) || (o >= 0x38 && o < 0x40) || o < 0x30 || (o >= 0xa88 && o < 0xa90) || (o >= 0xa58 && o < 0xa60) || (o >= 0x908 && o < 0x909) || (o >= 0x40 && o < 0x48) || (o >= 0x8c && o < 0x9c) || (o >= 0x980 && o < 0x981))) {
                    uint32_t a, b, pre0;
                    std::memcpy(&a, pimg.b.data() + o, 4);
                    std::memcpy(&b, gimg.b.data() + o, 4);
                    std::memcpy(&pre0, r.preP.data() + o, 4);
                    std::printf(" [%zx pre %08x gd %08x port %08x]", o, pre0, b, a);
                }
            std::printf("\n");
        }
    }
    std::printf("rows %lld, rows with a differing word %lld, return-value mismatches %lld, unsupported calls %lld; rows that ran only ported code %lld, of which differ %lld\n", rows, bad, diedMismatch, unsup, supRows, supBad);
    std::printf("unsupported by object type:");
    for (auto& kv : unsupBy) std::printf(" %d:%lld", kv.first, kv.second);
    std::printf("\n");
    for (const auto& kv : diffTicks) {
        std::printf("  +0x%zx differs on %lld ticks, e.g.", kv.first, kv.second);
        for (long long t : diffAt[kv.first]) std::printf(" %lld", t);
        std::printf("\n");
    }
    return 0;
}
