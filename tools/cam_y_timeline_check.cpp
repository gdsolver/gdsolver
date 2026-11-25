// cam_y_timeline_check <dump.csv> ...: the Y side of dp/src/dp/cam_model.hpp (zoom, offset Y, time warp, static camera on/off, top / bottom Camera Edge) stepped along the dump's own player x track (attempt 1),
// against the game's values in the same rows (c1a8, c1b4, tw, c23c, edgeT, edgeB).  The level file is read from the autorun.cfg beside the dump (levelfile=).  A tick is "known" while no touch- / spawn-triggered
// camera trigger can have fired yet (runMax < unknownMinX): only those are counted in the second figure, all ticks in the first.  Observation only: this reads the model, it changes nothing.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "dp/cam_model.hpp"
using namespace dp::gdcam;

static std::vector<std::string> split(const std::string& s) { std::vector<std::string> o; std::string t; std::stringstream ss(s); while (std::getline(ss, t, ',')) o.push_back(t); return o; }
int main(int argc, char** argv) {
    const char* fields[6] = {"zoom", "offY", "tw", "static", "edgeT", "edgeB"};
    long long T[2][6] = {}, K[2][6] = {}, N[2] = {};   // [all, known][field]
    for (int a = 1; a < argc; ++a) {
        std::string path = argv[a]; std::string dir = path.substr(0, path.rfind('\\'));
        std::string lvl; { std::ifstream c(dir + "\\autorun.cfg"); std::string l; while (std::getline(c, l)) { while (!l.empty() && (l.back() == '\r')) l.pop_back(); if (l.rfind("levelfile=", 0) == 0) lvl = l.substr(10); } }
        CamModel M; if (lvl.empty() || !M.load(lvl)) { std::printf("%s: no level file (%s)\n", path.c_str(), lvl.c_str()); continue; }
        std::ifstream f(path, std::ios::binary); std::string line; std::getline(f, line);
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        std::map<std::string, int> col; { auto h = split(line); for (size_t i = 0; i < h.size(); ++i) col[h[i]] = (int)i; }
        auto num = [&](const std::vector<std::string>& r, const char* k) { auto it = col.find(k); if (it == col.end() || (size_t)it->second >= r.size()) return 0.0; return std::strtod(r[it->second].c_str(), nullptr); };
        CamState cs; bool first = true; long long t[2][6] = {}, k[2][6] = {}, n[2] = {};
        while (std::getline(f, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            auto r = split(line); if (r.size() <= (size_t)col["attempt"] || r[col["attempt"]] != "1") continue;
            float zoom = 1.0f; CamYTick y;
            if (first && std::getenv("PRESTEP")) { M.step(cs, 0.0, &zoom, &y); }   // the game's tick 0 (before the first dumped row) takes the triggers at x 0 and steps their tweens once
            first = false;
            M.step(cs, num(r, "x"), &zoom, &y);
            const bool known = !(M.unknownMinX <= cs.runMax);
            const bool ok[6] = {std::fabs((double)y.zoom - num(r, "c1a8")) < 1e-4, std::fabs((double)y.offY - num(r, "c1b4")) < 1e-3, std::fabs((double)y.tw - num(r, "tw")) < 1e-6,
                                y.staticY == (num(r, "c23c") != 0), y.edgeTGrp == (int)num(r, "edgeT"), y.edgeBGrp == (int)num(r, "edgeB")};
            static int shown = 0; if (known && !ok[0] && num(r, "tick") > 20 && (shown++ < 3 || num(r, "tick") > 1500) && shown < 14) std::printf("  zoom miss tick %s model %.6f dump %.6f x %.2f\n", r[col["tick"]].c_str(), (double)y.zoom, num(r, "c1a8"), num(r, "x"));
            for (int w = 0; w < 2; ++w) { if (w == 1 && !known) continue; ++n[w]; for (int i = 0; i < 6; ++i) { ++t[w][i]; k[w][i] += ok[i]; } }
        }
        std::printf("%s (%lld ticks, %lld known):", path.c_str(), n[0], n[1]);
        for (int i = 0; i < 6; ++i) std::printf(" %s %lld/%lld (known %lld/%lld)", fields[i], k[0][i], t[0][i], k[1][i], t[1][i]);
        std::printf("\n");
        for (int w = 0; w < 2; ++w) { N[w] += n[w]; for (int i = 0; i < 6; ++i) { T[w][i] += t[w][i]; K[w][i] += k[w][i]; } }
    }
    std::printf("TOTAL all %lld known %lld:", N[0], N[1]);
    for (int i = 0; i < 6; ++i) std::printf(" %s %lld/%lld (known %lld/%lld)", fields[i], K[0][i], T[0][i], K[1][i], T[1][i]);
    std::printf("\n");
    return 0;
}
