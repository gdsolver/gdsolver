// spider_test: dp/src/dp/spider.hpp (the spider's teleport) and the ground press of upd.hpp, against the game's own pushButton calls (mod cfg btraw=t0,t1 -> btraw.csv).
//   spider_test <btraw.csv> <objrects.txt> <ctraw.csv> [--from T] [--to T] [--show N] [--verbose]
// Only the rows of fn 1 (pushButton) whose entry image is a spider. The level's solids and hazards come from objrects.txt (static positions: a moving block is where the dump saw it first), the
// flying band and the free-mode byte from the nearest row of ctraw.csv of the same run. The exit image is compared word by word, outside the containers btraw keeps apart.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/spider_test.cpp
#include "dp/buttons.hpp"
#include "dp/upd.hpp"

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
template <class T> static T at(const std::vector<uint8_t>& b, size_t o) { T v; std::memcpy(&v, b.data() + o, sizeof(T)); return v; }

struct Snap {
    std::vector<uint8_t> img;
    struct R { int uid, type, rev, f740; };
    std::vector<R> arr;
    std::set<int> used, touched;
    std::string hold;
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
    return n;
}

// bytes that are not physics (the same list as tools/upd_test.cpp)
static bool ignored(size_t o) {
    if (o >= 0x998 && o < 0x9a4) return true;          // rand()
    if (o >= 0x800 && o < 0x808) return true;          // _ftime64
    if (o >= 0x8c && o < 0xa4) return true;            // colour words
    if (o >= 0x111 && o < 0x11c) return true;
    if (o == 0x14) return true;
    if (o == 0x7ee || o == 0x7ef) return true;
    if (o >= 0x1f0 && o < 0x230) return true;
    if ((o >= 0x148 && o < 0x150) || (o >= 0x1d8 && o < 0x1dc) || (o >= 0xc6c && o < 0xc70)) return true;
    if (o >= 0x810 && o < 0x818) return true;
    return false;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: spider_test btraw.csv objrects.txt ctraw.csv [--from T] [--to T] [--show N] [--verbose]\n"); return 2; }
    long long from = 0, to = 1LL << 60;
    int show = 12;
    bool verbose = false;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from") from = std::atoll(argv[++i]);
        else if (a == "--to") to = std::atoll(argv[++i]);
        else if (a == "--show") show = std::atoi(argv[++i]);
        else if (a == "--verbose") verbose = true;
        else if (a == "--debug") spider::debugOn() = true;
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
            o.rot = (float)num(v, "rot"); o.w0 = (float)num(v, "w0"); o.h0 = (float)num(v, "h0");
            o.sy0 = num(v, "sy0"); o.sy1 = num(v, "sy1");
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
    }
    // the layer: the flying band and the free-mode byte, by tick (ctraw rows)
    std::map<long long, Layer> layers;
    {
        std::ifstream f(argv[3]);
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            const auto v = split(line, ',');
            if (v.size() < 8) continue;
            const auto parts = split(v[6], ';');
            if (parts.size() < 8) continue;
            Layer L;
            const auto pq = split(parts[7], ':');
            L.maxPortalY = std::stof(pq[0]);
            L.minPortalY = std::stof(pq[1]);
            L.freeMode = unhex(parts[3])[0x311 - 0x300] != 0;
            L.dual = unhex(parts[3])[0x422 - 0x300] != 0;
            layers[std::atoll(v[1].c_str())] = L;
        }
    }
    std::ifstream f(argv[1]);
    std::string line;
    std::getline(f, line);
    long long rows = 0, wrong = 0, jumped = 0;
    std::map<size_t, long long> words;
    std::map<int, long long> unsupportedKeys;
    int shown = 0;
    while (std::getline(f, line)) {
        const auto cols = split(line, ',');
        if (cols.size() < 8) continue;
        const long long tick = std::atoll(cols[1].c_str());
        const int fn = std::atoi(cols[3].c_str()), arg = std::atoi(cols[4].c_str());
        if (fn != 1 || tick < from || tick > to) continue;
        Snap pre = parseSnap(cols[6]), post = parseSnap(cols[7]);
        if (pre.img.size() < 0xc80 || post.img.size() != pre.img.size()) continue;
        if (!pre.img[po::ModeSpider]) continue;
        Mem img(pre.img.size());
        img.b = pre.img;
        Ctx c(img);
        {
            auto it = layers.upper_bound(tick);
            if (it != layers.begin()) --it;
            if (it != layers.end()) c.L = it->second;
        }
        std::vector<GObj> world = objs;
        RingTouches rt;
        for (const auto& r : pre.arr) rt.arr.push_back(r.uid);
        rt.now = pre.touched;
        c.objs = &world;
        c.p22 = true;
        std::set<int> used = pre.used;
        ButtonsBody b;
        b.c = &c;
        b.touch = &rt;
        b.usedRings = &used;
        for (int k = 1; k <= 3; ++k) b.holding[k] = pre.hold.size() >= 3 && pre.hold[(size_t)k - 1] == '1';
        ButtonsHooks hk;
        hk.updateJump = [&](Ctx& cc, float dt) { upd::updateJump(cc, dt); };
        int viaRing = 0;
        hk.spiderTestJump = [&](Ctx& cc, const GObj*) { ++viaRing; return spider::spiderTestJump(cc); };
        pushButton(b, arg, hk);
        if (verbose) std::printf("t=%lld: spider ring hook calls %d, teleports %d, mine 368/369 = %02x %02x, game %02x %02x, pre %02x %02x\n", tick, viaRing, spider::callCount(), img.b[0x368], img.b[0x369], post.img[0x368], post.img[0x369], pre.img[0x368], pre.img[0x369]);
        ++rows;
        std::vector<size_t> diffs;
        for (size_t w = 0; w + 4 <= post.img.size(); w += 4) {
            if ((w >= 0x850 && w < 0x890) || (w >= 0xa40 && w < 0xa88) || (w >= 0xbc0 && w < 0xbd0)) continue;
            bool d = false;
            for (size_t k = w; k < w + 4; ++k) if (!ignored(k) && img.b[k] != post.img[k]) d = true;
            if (d) diffs.push_back(w);
        }
        const bool moved = std::memcmp(pre.img.data() + po::PosX, post.img.data() + po::PosX, 8) != 0 || pre.img[po::Upside] != post.img[po::Upside];
        if (moved) ++jumped;
        for (const auto& u : c.unsupportedBy) ++unsupportedKeys[u.first];
        for (size_t w : diffs) ++words[w];
        if (!diffs.empty() || verbose) {
            if (!diffs.empty()) ++wrong;
            if (!diffs.empty() && shown < show) {
                ++shown;
                std::printf("WRONG t=%lld arg=%d %s y %.4f -> game %.4f mine %.4f upside %d -> game %d mine %d  words:", tick, arg, moved ? "[teleport]" : "[no move]",
                            at<float>(pre.img, po::PosY), at<float>(post.img, po::PosY), at<float>(img.b, po::PosY), pre.img[po::Upside], post.img[po::Upside], img.b[po::Upside]);
                int n = 0;
                for (size_t w : diffs) {
                    if (n++ >= 10) { std::printf(" ..."); break; }
                    uint32_t g, m;
                    std::memcpy(&g, post.img.data() + w, 4);
                    std::memcpy(&m, img.b.data() + w, 4);
                    std::printf(" %zx(game %08x mine %08x)", w, g, m);
                }
                std::printf("\n");
            } else if (verbose) {
                std::printf("ok t=%lld arg=%d %s\n", tick, arg, moved ? "[teleport]" : "[no move]");
            }
        }
    }
    std::printf("spider pushButton rows %lld, wrong %lld, rows where the player moved or flipped %lld\n", rows, wrong, jumped);
    std::vector<std::pair<long long, size_t>> v;
    for (const auto& w : words) v.push_back({w.second, w.first});
    std::sort(v.rbegin(), v.rend());
    std::printf("words that differ (count of rows):");
    int n = 0;
    for (const auto& p : v) { if (n++ >= 12) break; std::printf(" %zx:%lld", p.second, p.first); }
    std::printf("\n");
    for (const auto& u : unsupportedKeys) std::printf("unsupported[%d]: %lld rows\n", u.first, u.second);
    return 0;
}
