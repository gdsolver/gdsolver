// upd_test: dp/src/dp/upd.hpp (the update phase) against the game's own entry / exit bytes of PlayerObject::update (mod cfg ctupd=t0,t1 -> ctupd.csv).
//   upd_test <ctupd.csv> [--from T] [--to T] [--verbose] [--show N]
// Per row the entry image is given to upd::playerUpdate(dt) and the result is compared with the exit image byte by byte, leaving out the fields that are not physics
// (rand(), the wall clock, the colours / sprite words). A row passes when no compared byte differs. The summary lists the offsets that differ and on how many rows.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/upd_test.cpp
#include "dp/upd.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
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

// bytes that are not physics (see upd.hpp's header)
static bool ignored(size_t o) {
    if (o >= 0x998 && o < 0x9a4) return true;          // rand()
    if (o >= 0x800 && o < 0x808) return true;          // _ftime64
    if (o >= 0x8c && o < 0xa4) return true;            // colour words
    if (o >= 0x111 && o < 0x11c) return true;
    if (o == 0x14) return true;                         // CCObject word (reference / script bookkeeping)
    if (o == 0x7ee || o == 0x7ef) return true;          // particle-system on/off flags (update's sprite part)
    if (o >= 0x1f0 && o < 0x230) return true;          // sprite texture-rect / colour words (robot, spider)
    if ((o >= 0x148 && o < 0x150) || (o >= 0x1d8 && o < 0x1dc) || (o >= 0xc6c && o < 0xc70)) return true;   // the colour fade update starts with (the first block of update)
    if (o >= 0x810 && o < 0x818) return true;           // ...and its end time
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: upd_test ctupd.csv [--from T] [--to T] [--verbose]\n"); return 2; }
    long long from = 0, to = 1LL << 60;
    bool verbose = false;
    std::string rotPath;   // --rot <ctraw.csv of the same run>: the pass's exit of tick T -> upd::updateRotation -> compared with the next update's entry (rotation words only)
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from") from = std::atoll(argv[++i]);
        else if (a == "--to") to = std::atoll(argv[++i]);
        else if (a == "--verbose") verbose = true;
        else if (a == "--rot") rotPath = argv[++i];
    }
    std::map<long long, std::vector<uint8_t>> passPost;
    if (!rotPath.empty()) {
        std::ifstream rf(rotPath);
        std::string rl;
        std::getline(rf, rl);
        while (std::getline(rf, rl)) {
            const auto v = split(rl, ',');
            if (v.size() < 8) continue;
            passPost[std::atoll(v[1].c_str())] = unhex(split(v[7], ';')[0]);
        }
    }
    std::ifstream f(argv[1]);
    std::string line;
    std::getline(f, line);
    long long rows = 0, bad = 0, rotRows = 0, rotBad = 0;
    std::map<long long, std::vector<uint8_t>> updPre;
    std::map<long long, float> updDt;
    if (!rotPath.empty()) {
        std::ifstream f2(argv[1]);
        std::string l2;
        std::getline(f2, l2);
        while (std::getline(f2, l2)) {
            const auto v = split(l2, ',');
            if (v.size() < 6) continue;
            updPre[std::atoll(v[1].c_str())] = unhex(v[4]);
            updDt[std::atoll(v[1].c_str())] = (float)std::atof(v[3].c_str());
        }
        for (auto& kv : passPost) {
            auto it = updPre.find(kv.first + 1);
            if (it == updPre.end()) continue;
            Mem P(kv.second.size());
            P.b = kv.second;
            upd::updateRotation(P, updDt[kv.first + 1]);
            ++rotRows;
            const auto& want = it->second;
            bool bad1 = false;
            for (size_t o : {(size_t)0x38, (size_t)0x39, (size_t)0x3a, (size_t)0x3b, (size_t)0x728, (size_t)0x729, (size_t)0x72a, (size_t)0x72b, (size_t)0x730, (size_t)0x731, (size_t)0x670})
                if (P.b[o] != want[o]) bad1 = true;
            if (bad1) {
                ++rotBad;
                if (verbose && rotBad <= 10) {
                    float a, b2, p0; std::memcpy(&a, &P.b[0x38], 4); std::memcpy(&b2, &want[0x38], 4); std::memcpy(&p0, &kv.second[0x38], 4);
                    for (size_t o : {(size_t)0x38, (size_t)0x728, (size_t)0x730, (size_t)0x670}) {
                        bool d = false;
                        for (size_t k = 0; k < 4; ++k) if (P.b[o + k] != want[o + k]) d = true;
                        if (d) { uint32_t x, y; std::memcpy(&x, &P.b[o], 4); std::memcpy(&y, &want[o], 4); std::printf("  +0x%zx mine %08x game %08x\n", o, x, y); }
                    }
                    {
                        double vy; std::memcpy(&vy, &kv.second[0x9a8], 8); float r728; std::memcpy(&r728, &kv.second[0x728], 4);
                        std::printf("  exit: a14 %d vy %.4f upside %d 9b9 %d 9b8 %d 730 %d 728 %g 670 %d plat %d | next a14 %d 9b9 %d\n", kv.second[0xa14], vy, kv.second[0x9c7], kv.second[0x9b9], kv.second[0x9b8], kv.second[0x730], (double)r728, kv.second[0x670], kv.second[0xb78], want[0xa14], want[0x9b9]);
                    }
                    std::printf("rot t=%lld: pass exit %.6f mine %.6f game(next entry) %.6f | modes ship %d ufo %d ball %d wave %d robot %d spider %d swing %d\n", kv.first, (double)p0, (double)a, (double)b2, kv.second[0x9c1], kv.second[0x9c2], kv.second[0x9c3], kv.second[0x9c4], kv.second[0x9c5], kv.second[0x9c6], kv.second[0x9cc]);
                }
            }
        }
        std::printf("rotation rows %lld, differing %lld\n", rotRows, rotBad);
    }
    std::map<size_t, long long> byOff;
    std::map<int, long long> unsup;
    while (std::getline(f, line)) {
        const auto v = split(line, ',');
        if (v.size() < 6) continue;
        const long long tick = std::atoll(v[1].c_str());
        if (tick < from || tick > to) continue;
        const float dt = (float)std::atof(v[3].c_str());
        const auto pre = unhex(v[4]);
        const auto post = unhex(v[5]);
        Mem P(pre.size());
        P.b = pre;
        Ctx c(P);
        upd::playerUpdate(c, dt, [](Ctx& cc, float d) { upd::updateJump(cc, d); });
        for (auto& kv : c.unsupportedBy) unsup[kv.first] += 1;
        ++rows;
        bool any = false;
        std::vector<size_t> offs;
        for (size_t o = 0; o < post.size() && o < P.b.size(); ++o) {
            if (ignored(o)) continue;
            if (P.b[o] != post[o]) { offs.push_back(o); byOff[o]++; any = true; }
        }
        if (any) {
            ++bad;
            if (verbose && bad <= 12) {
                std::printf("t=%lld dt=%g: %zu bytes differ:", tick, (double)dt, offs.size());
                size_t shown = 0;
                for (size_t o : offs) { if (shown++ > 14) break; std::printf(" +0x%zx[%02x/%02x]", o, P.b[o], post[o]); }
                std::printf("\n");
                double a, b2, p0; float ya, yb, y0; std::memcpy(&a, &P.b[0x9a8], 8); std::memcpy(&b2, &post[0x9a8], 8); std::memcpy(&p0, &pre[0x9a8], 8);
                std::memcpy(&ya, &P.b[0x50], 4); std::memcpy(&yb, &post[0x50], 4); std::memcpy(&y0, &pre[0x50], 4);
                std::printf("     vy pre %.6f mine %.6f game %.6f | y pre %.5f mine %.5f game %.5f | mode: ship %d ufo %d ball %d wave %d robot %d spider %d swing %d up %d G %d H %d\n", p0, a, b2, (double)y0, (double)ya, (double)yb, pre[0x9c1], pre[0x9c2], pre[0x9c3], pre[0x9c4], pre[0x9c5], pre[0x9c6], pre[0x9cc], pre[0x9c7], pre[0x9c9], pre[0x98d]);
            }
        }
    }
    std::printf("rows %lld, rows with any differing byte %lld\n", rows, bad);
    std::vector<std::pair<long long, size_t>> order;
    for (auto& kv : byOff) order.push_back({kv.second, kv.first});
    std::sort(order.rbegin(), order.rend());
    for (size_t i = 0; i < order.size() && i < 80; ++i) std::printf("  +0x%zx differs on %lld rows\n", order[i].second, order[i].first);
    for (auto& kv : unsup) std::printf("  unsupported[%d] on %lld rows\n", kv.first, kv.second);
    return 0;
}
