// cam_y_static_check <dump.csv> ...: dp/src/dp/cam_y_static.hpp against the static camera's point (+0x1e4) and rate in the dump, per static tick.  Each level's directory also holds statev.csv
// (row index, easing key 30, key 85 of the trigger that started each tween -- picked in python from the level file; the dp takes them from the fired trigger).
// Tween segments are found as in t2\statself.py: a follow / bezier segment restarts when (target, group, duration, bz, tangent, flags) change or the previous tick had neither flag; a plain one when the target changes.
// Start point = the row's +0x1dc, target = +0x1ec, live group y = the dump's gyY.  Tolerance 0.01 (follow / bezier), 0.05 (plain); the rate is compared on follow ticks (max(1, dump rateY)).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "dp/cam_y_static.hpp"
using namespace dp::gdcam;

static std::vector<std::string> split(const std::string& s) { std::vector<std::string> o; std::string t; std::stringstream ss(s); while (std::getline(ss, t, ',')) o.push_back(t); return o; }
int main(int argc, char** argv) {
    long long tot[3][2] = {}, rt[2] = {};
    const char* names[3] = {"follow", "bezier", "plain"};
    for (int a = 1; a < argc; ++a) {
        std::string path = argv[a]; std::ifstream f(path, std::ios::binary); std::string line;
        std::getline(f, line); if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        std::map<std::string, int> col; { auto h = split(line); for (size_t i = 0; i < h.size(); ++i) col[h[i]] = (int)i; }
        std::vector<std::vector<std::string>> rows;
        while (std::getline(f, line)) { while (!line.empty() && line.back() == '\r') line.pop_back(); auto r = split(line); if (r.size() > (size_t)col["attempt"] && r[col["attempt"]] == "1") rows.push_back(std::move(r)); }
        std::map<size_t, std::pair<int, float>> ev;
        { std::ifstream e(path.substr(0, path.rfind('\\')) + "\\statev.csv"); while (std::getline(e, line)) { auto v = split(line); if (v.size() == 3) ev[(size_t)std::atoll(v[0].c_str())] = {std::atoi(v[1].c_str()), (float)std::atof(v[2].c_str())}; } }
        auto num = [&](const std::vector<std::string>& r, const char* k) { auto it = col.find(k); if (it == col.end() || (size_t)it->second >= r.size()) return 0.0; return std::strtod(r[it->second].c_str(), nullptr); };
        CamYStatic st; bool have = false; double keyv[7] = {}; bool pseg = false; double ptarget = 0;
        long long ok[3] = {}, n[3] = {};
        for (size_t i = 1; i < rows.size(); ++i) {
            const auto& r = rows[i]; const auto& p = rows[i - 1];
            if (num(r, "c23c") == 0) { have = false; pseg = false; continue; }
            const bool fol = num(r, "c22c") != 0, bz = num(r, "c264") != 0;
            int kind;
            CamYTween tw; tw.follow = fol; tw.bezier = bz; tw.dur = (float)num(r, "c20c"); tw.speed = (float)num(r, "c224"); tw.tangent = (float)num(r, "c204"); tw.bz = (float)num(r, "c26c"); tw.target = (float)num(r, "c1ec");
            bool start = false;
            if (fol || bz) {
                pseg = false;
                const double key[7] = {num(r, "c1ec"), num(r, "c234"), num(r, "c20c"), num(r, "c26c"), num(r, "c204"), (double)fol, (double)bz};
                bool same = have; for (int j = 0; j < 7; ++j) if (key[j] != keyv[j]) same = false;
                if (!same || (num(p, "c22c") == 0 && num(p, "c264") == 0)) start = true;
                for (int j = 0; j < 7; ++j) keyv[j] = key[j];
                kind = bz ? 1 : 0;
            } else {
                const double pk = num(r, "c1ec");
                if (!pseg || ptarget != pk || num(p, "c23c") == 0) start = true;
                pseg = true; ptarget = pk; kind = 2;
            }
            if (start) { auto it = ev.find(i); if (it != ev.end()) { tw.ease = it->second.first; tw.easeRate = it->second.second; } st.fire(tw, (float)num(r, "c1dc")); st.tw.ease = tw.ease; st.tw.easeRate = tw.easeRate; have = true; }
            if (!have) continue;
            float rate; const float v = st.step((float)num(r, "gyY"), &rate);
            const bool good = std::fabs((double)v - num(r, "c1e4")) < (kind == 2 ? 0.05 : 0.01);
            ++n[kind]; ok[kind] += good;
            if (fol) { ++rt[1]; rt[0] += std::fabs((double)rate - std::max(1.0, num(r, "rateY"))) < 1e-3 * std::max(1.0, (double)rate); }
        }
        std::printf("%s: follow %lld/%lld bezier %lld/%lld plain %lld/%lld\n", path.c_str(), ok[0], n[0], ok[1], n[1], ok[2], n[2]);
        for (int k = 0; k < 3; ++k) { tot[k][0] += ok[k]; tot[k][1] += n[k]; }
    }
    for (int k = 0; k < 3; ++k) std::printf("TOTAL %-7s %lld / %lld\n", names[k], tot[k][0], tot[k][1]);
    std::printf("TOTAL follow rate %lld / %lld\n", rt[0], rt[1]);
    return 0;
}
