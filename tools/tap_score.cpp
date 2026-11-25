// Scores dp/src/dp/trigtouch.hpp's TapDecider (the Touch trigger, id 1595, decided from the button edges) against the game's own group toggles.
//   cl /nologo /std:c++17 /EHsc /I dp\src tools\tap_score.cpp /Fe:tap_score.exe
//   tap_score <dir> <lid> <plan.txt> [events_out.txt]
// <dir> holds <lid>_result.txt (cfg hitboxtrace=1: `togl:` lines, trig = the Tap's uid), <lid>_triggers.txt, <lid>_objgroups.txt, <lid>_trigfire.csv (cfg trigfire=1).
// The truth is the game's, the inputs are the plan's button edges and the crossing / Stop ticks of trigfire.csv; the decider supplies the rest. With events_out, every event is written as
// `tick,group,on` -- the form trigshadow's setGroupToggleEvents is given (gd-04): tick = the game tick of the button call (the `togl:` line's t), applied by it at the head of step tick + 1.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "dp/trigtouch.hpp"

static std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> o; std::string cur; std::istringstream is(s);
    while (std::getline(is, cur, c)) o.push_back(cur);
    if (!s.empty() && s.back() == c) o.push_back("");
    return o;
}
static std::string rstrip(std::string s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back(); return s; }

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "tap_score <dir> <lid> <plan.txt> [events_out.txt]\n"); return 2; }
    const std::string dir = argv[1], lid = argv[2], planPath = argv[3];
    const std::string base = dir + "\\" + lid + "_";
    // plan edges
    std::vector<std::pair<long long, int>> plan;
    {
        std::ifstream f(planPath); std::string l;
        while (std::getline(f, l)) { long long t; int b; if (std::sscanf(l.c_str(), "input=%lld,%d", &t, &b) == 2) plan.push_back({t, b}); }
    }
    std::sort(plan.begin(), plan.end());
    std::vector<std::pair<long long, int>> edges; int held = 0;
    for (auto& p : plan) if (p.second != held) { edges.push_back({p.first, p.second ? 1 : 2}); held = p.second; }
    // triggers.txt
    std::map<int, std::map<std::string, std::string>> trig;
    {
        std::ifstream f(base + "triggers.txt"); std::string l; std::getline(f, l);
        const auto h = split(rstrip(l), ',');
        while (std::getline(f, l)) {
            const auto v = split(rstrip(l), ',');
            if (v.size() < h.size()) continue;
            std::map<std::string, std::string> r; for (size_t i = 0; i < h.size(); ++i) r[h[i]] = v[i];
            trig[std::atoi(v[0].c_str())] = r;
        }
    }
    std::map<int, std::set<std::string>> og;
    {
        std::ifstream f(base + "objgroups.txt"); std::string l; std::getline(f, l);
        while (std::getline(f, l)) {
            for (char& c : l) if (c == ',') c = ' ';
            std::istringstream is(l); std::string a; if (!(is >> a) || a.empty() || !isdigit((unsigned char)a[0])) continue;
            std::string g; while (is >> g) og[std::atoi(a.c_str())].insert(g);
        }
    }
    // trigfire.csv: attempt,tick,uid,id,px,spawnTriggered,via
    struct Fire { int att; long long tick; int uid, id; };
    std::vector<Fire> fires; int maxAtt = 0;
    {
        std::ifstream f(base + "trigfire.csv"); std::string l; std::getline(f, l);
        while (std::getline(f, l)) {
            const auto v = split(rstrip(l), ','); if (v.size() < 4) continue;
            fires.push_back({std::atoi(v[0].c_str()), std::atoll(v[1].c_str()), std::atoi(v[2].c_str()), std::atoi(v[3].c_str())});
            maxAtt = std::max(maxAtt, fires.back().att);
        }
    }
    // the truth, and the cut-off: the log keeps 2,000 `togl:` lines, and nothing after the level's completion counts
    std::map<int, std::set<std::tuple<long long, int, int>>> truth; int ntogl = 0; long long lastT = 0, completeT = -1;
    {
        std::ifstream f(base + "result.txt"); std::string l;
        while (std::getline(f, l)) {
            long long t; int g, on, trg, ctl;
            if (std::sscanf(l.c_str(), "togl: t=%lld g=%d on=%d trig=%d ctl=%d", &t, &g, &on, &trg, &ctl) == 5) { ++ntogl; lastT = t; truth[trg].insert({t, g, on}); }
            else if (completeT < 0 && l.rfind("complete:", 0) == 0) { const auto p = l.find("tick="); if (p != std::string::npos) completeT = std::atoll(l.c_str() + p + 5); }
        }
    }
    long long lim = ntogl >= 2000 ? lastT : (long long)4e18;
    if (completeT >= 0) lim = std::min(lim, completeT);

    dp::gdtouch::TapDecider dec;
    for (auto& kv : trig) {
        auto& r = kv.second;
        if (r["id"] == "1595" && r["touch"] == "0" && r["spawn"] == "0") dec.taps[kv.first] = {std::atoi(r["target"].c_str()), std::atoi(r["thold"].c_str()), std::atoi(r["ttog"].c_str())};
    }
    dec.startAttempt();
    for (auto& fr : fires) {
        if (fr.att != maxAtt) continue;
        if (fr.id == 1595) dec.crossed(fr.uid, fr.tick);
        if (fr.id == 1616) {
            const auto it = trig.find(fr.uid); if (it == trig.end()) continue;
            for (auto& tp : dec.taps) { const auto g = og.find(tp.first); if (g != og.end() && g->second.count(it->second["target"])) dec.stopAt(tp.first, fr.tick); }
        }
    }
    std::map<int, std::set<std::tuple<long long, int, int>>> pred;
    std::ofstream out; if (argc > 4) out.open(argv[4]);
    for (auto& e : edges) {
        if (e.first > lim) break;
        for (auto& ev : dec.step(e.first, e.second)) {
            pred[ev.uid].insert({e.first, ev.target, ev.on ? 1 : 0});
            if (out) out << e.first << "," << ev.target << "," << (ev.on ? 1 : 0) << "\n";
        }
    }
    int totP = 0, totT = 0, ok = 0, extra = 0, miss = 0;
    for (auto& tp : dec.taps) {
        std::set<std::tuple<long long, int, int>> tru; for (auto& e : truth[tp.first]) if (std::get<0>(e) <= lim) tru.insert(e);
        auto& pr = pred[tp.first];
        int o = 0, x = 0, m = 0;
        for (auto& e : pr) { if (tru.count(e)) ++o; else ++x; }
        for (auto& e : tru) if (!pr.count(e)) ++m;
        std::printf("%s uid %d: pred %zu truth %zu ok %d extra %d miss %d\n", lid.c_str(), tp.first, pr.size(), tru.size(), o, x, m);
        totP += (int)pr.size(); totT += (int)tru.size(); ok += o; extra += x; miss += m;
    }
    std::printf("TOTAL %s pred %d truth %d ok %d extra %d miss %d\n", lid.c_str(), totP, totT, ok, extra, miss);
    return (extra || miss) ? 1 : 0;
}
