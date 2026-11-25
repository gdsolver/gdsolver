// buttons_replay: dp/src/dp/buttons.hpp against the game's own calls (mod cfg btraw=t0,t1 -> btraw.csv).
//   buttons_replay <btraw.csv> [--from T] [--to T] [--show N] [--fn K] [--verbose]
// For every row it builds the image, the touching array, the used-rings set, the touched set and the holding map from the entry snapshot, runs the ported call (fn 1 pushButton, 2 releaseButton, 3 releaseAllButtons,
// 4 enablePlayerControls, 5 resetTouchedRings) and compares the exit snapshot: the image field by field (4-byte words), the array, the sets, the holding map. A call that needed a hook that is not ported
// (updateJump(0), the spider's jump, the dash ...) is marked `hook` and counted apart: its image words are listed but do not make the row "wrong".
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/buttons_replay.cpp
#include "dp/buttons.hpp"

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

struct Snap {
    std::vector<uint8_t> img;
    struct R { int uid, type, rev, f740; };
    std::vector<R> arr;
    std::set<int> used, touched;
    std::string hold;   // "ddd": '-' none, 0, 1 for buttons 1..3
};

static Snap parseSnap(const std::string& s) {
    Snap n;
    const auto parts = split(s, ';');
    if (parts.size() < 5) return n;   // a row the session cut short
    n.img = unhex(parts[0]);
    {
        std::istringstream is(parts[1]);
        std::string tok;
        while (is >> tok) {
            const auto f = split(tok, ':');
            if (f.size() < 4) continue;
            n.arr.push_back({std::atoi(f[0].c_str()), std::atoi(f[1].c_str()), std::atoi(f[2].c_str()), std::atoi(f[3].c_str())});
        }
    }
    { std::istringstream is(parts[2]); int u; while (is >> u) n.used.insert(u); }
    { std::istringstream is(parts[3]); int u; while (is >> u) n.touched.insert(u); }
    n.hold = parts.size() > 4 ? parts[4] : "---";
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: buttons_replay <btraw.csv> [--from T] [--to T] [--show N] [--fn K] [--verbose]\n"); return 2; }
    long long from = 0, to = 1LL << 60;
    int show = 12, onlyFn = 0;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from") from = std::atoll(argv[++i]);
        else if (a == "--to") to = std::atoll(argv[++i]);
        else if (a == "--show") show = std::atoi(argv[++i]);
        else if (a == "--fn") onlyFn = std::atoi(argv[++i]);
        else if (a == "--verbose") verbose = true;
    }
    std::ifstream f(argv[1]);
    std::string line;
    std::getline(f, line);   // header
    struct Tally { long long rows = 0, wrong = 0, hook = 0, wrongSets = 0; std::map<size_t, long long> words; };
    std::map<int, Tally> byFn;
    std::map<std::string, long long> hooksUsed;
    int shown = 0;
    long long total = 0;
    while (std::getline(f, line)) {
        const auto cols = split(line, ',');
        if (cols.size() < 8) continue;
        const long long tick = std::atoll(cols[1].c_str());
        const int who = std::atoi(cols[2].c_str()), fn = std::atoi(cols[3].c_str()), arg = std::atoi(cols[4].c_str()), ret = std::atoi(cols[5].c_str());
        if (tick < from || tick > to || (onlyFn && fn != onlyFn)) continue;
        if (verbose) std::fprintf(stderr, "row t=%lld fn=%d arg=%d\n", tick, fn, arg);
        Snap pre = parseSnap(cols[6]), post = parseSnap(cols[7]);
        if (pre.img.size() < 0xc80 || post.img.size() != pre.img.size()) continue;
        Mem img(pre.img.size());
        img.b = pre.img;
        Ctx c(img);
        std::vector<GObj> objs;
        RingTouches rt;
        for (const auto& r : pre.arr) {
            GObj o;
            o.uid = r.uid;
            o.type = r.type;
            o.reverse = r.rev != 0;
            objs.push_back(o);
            rt.arr.push_back(r.uid);
        }
        rt.now = pre.touched;
        c.objs = &objs;
        c.p22 = true;
        std::set<int> used = pre.used;
        ButtonsBody b;
        b.c = &c;
        b.touch = &rt;
        b.usedRings = &used;
        for (int k = 1; k <= 3; ++k) b.holding[k] = pre.hold.size() >= 3 && pre.hold[(size_t)k - 1] == '1';
        ButtonsHooks hk;
        bool hooked = false;
        std::string hookName;
        hk.updateJump = [&](Ctx&, float) { hooked = true; hookName = "updateJump"; };
        int myRet = -1;
        switch (fn) {
        case 1: myRet = pushButton(b, arg, hk) ? 1 : 0; break;
        case 2: myRet = releaseButton(b, arg, hk) ? 1 : 0; break;
        case 3: releaseAllButtons(b, hk); break;
        case 4: enablePlayerControls(b, hk); break;
        case 5: if (arg) rt.clearAll(); else rt.reset(); break;
        default: continue;
        }
        if (c.unsupported) { hooked = true; if (hookName.empty()) hookName = "unsupported"; }
        Tally& t = byFn[fn];
        ++t.rows;
        ++total;
        // the exit snapshot against what the port left
        std::vector<size_t> diffs;
        for (size_t w = 0; w + 4 <= post.img.size(); w += 4) {
            // the three containers the port keeps as sets (the used-rings set +0x850, the touching array and the touched set +0xa40, the holding map +0xbc0): their words are bookkeeping, compared as sets below
            if ((w >= 0x850 && w < 0x890) || (w >= 0xa40 && w < 0xa88) || (w >= 0xbc0 && w < 0xbd0)) continue;
            if (std::memcmp(img.b.data() + w, post.img.data() + w, 4) != 0) diffs.push_back(w);
        }
        std::string holdMine = "---";
        for (int k = 1; k <= 3; ++k) holdMine[(size_t)k - 1] = b.holding[k] ? '1' : (pre.hold[(size_t)k - 1] == '-' && !b.holding[k]) ? '-' : '0';
        bool setsOk = used == post.used;
        std::vector<int> postArr;
        for (const auto& r : post.arr) postArr.push_back(r.uid);
        if (fn != 5) setsOk = setsOk && rt.arr == postArr;
        else setsOk = setsOk && rt.arr == postArr && rt.now == post.touched;
        if (fn == 1 || fn == 2) setsOk = setsOk && (ret < 0 || myRet == ret);
        setsOk = setsOk && holdMine == post.hold;
        if (hooked) {
            ++t.hook;
            ++hooksUsed[hookName];
        }
        for (size_t w : diffs) ++t.words[w];
        const bool wrong = (!diffs.empty() && !hooked) || !setsOk;
        if (wrong) {
            ++t.wrong;
            if (!setsOk) ++t.wrongSets;
            if (shown < show) {
                ++shown;
                std::printf("WRONG t=%lld who=%d fn=%d arg=%d ret(game)=%d ret(port)=%d%s sets=%s words:", tick, who, fn, arg, ret, myRet, hooked ? " [hook]" : "", setsOk ? "ok" : "DIFFER");
                int n = 0;
                for (size_t w : diffs) {
                    if (n++ >= 8) { std::printf(" ..."); break; }
                    uint32_t g, m;
                    std::memcpy(&g, post.img.data() + w, 4);
                    std::memcpy(&m, img.b.data() + w, 4);
                    std::printf(" %zx(game %08x port %08x)", w, g, m);
                }
                std::printf("\n");
            }
        } else if (verbose && !diffs.empty()) {
            std::printf("hook-row t=%lld fn=%d arg=%d words:", tick, fn, arg);
            for (size_t w : diffs) std::printf(" %zx", w);
            std::printf("\n");
        }
    }
    std::printf("rows %lld\n", total);
    for (const auto& kv : byFn) {
        const Tally& t = kv.second;
        std::printf("fn %d: rows %lld, wrong %lld (sets/array/ret %lld), rows that needed a hook %lld\n", kv.first, t.rows, t.wrong, t.wrongSets, t.hook);
        std::vector<std::pair<long long, size_t>> v;
        for (const auto& w : t.words) v.push_back({w.second, w.first});
        std::sort(v.rbegin(), v.rend());
        std::printf("   words that differ (count of rows):");
        int n = 0;
        for (const auto& p : v) { if (n++ >= 10) break; std::printf(" %zx:%lld", p.second, p.first); }
        std::printf("\n");
    }
    for (const auto& h : hooksUsed) std::printf("hook %s: %lld rows\n", h.first.c_str(), h.second);
    return 0;
}
