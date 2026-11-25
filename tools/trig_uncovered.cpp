// trig_uncovered: which objects the trigger shadow (dp/src/dp/trigshadow.hpp) cannot vouch for, and what keeps them from being covered.
//   trig_uncovered <reference dir: triggers.txt, objgroups.txt, objrects.txt> [--tsv out]
// An object is uncovered when one of its groups is the target (or Rotate centre) of a trigger the shadow does not run. Counted per level and per trigger kind:
// grouped objects, uncovered ones, and how many of those the player can touch (solid / hazard / portal / pad / orb / collectible ... by the object's type).
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/trig_uncovered.cpp
#include "dp/trigshadow.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace dp::gdtrig;

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
static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

// GameObjectType values the player's collision can react to (GJGameObject types as objrects.txt `type` has them)
static bool touchable(int type) {
    switch (type) {
        case 0: case 2: case 3: case 4: case 5: case 6: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19:
        case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38:
        case 39: case 41: case 42: case 43: case 44:
            return true;
    }
    return false;
}

static std::map<int, std::string> g_areaWhy;
// the reason the shadow does not run a trigger, as a short label
static std::string label(const TrigDef& t) {
    std::string s = "id" + std::to_string(t.id);
    switch (t.id) {
        case 901: s = "Move"; break;
        case 1346: s = "Rotate"; break;
        case 1049: s = "Toggle"; break;
        case 1935: s = "TimeWarp"; break;
        case 3006: case 3007: case 3008: s = "Area" + std::to_string(t.id); break;
        case 1347: s = "Follow"; break;
        case 3016: s = "AdvFollow"; break;
        case 3032: s = "Keyframe"; break;
        case 1268: s = "Spawn"; break;
        case 1611: s = "Count"; break;
        case 1815: s = "Collision"; break;
        case 1616: s = "Stop"; break;
        case 1913: s = "Shake"; break;
        case 2067: s = "Scale"; break;
        case 3600: s = "End"; break;
        case 1595: s = "Touch"; break;
        case 3012: s = "AdvRandom"; break;
        case 1912: s = "Random"; break;
        case 2068: s = "Pickup"; break;
        case 2066: s = "GravityTrig"; break;
        case 3022: s = "Teleport"; break;
    }
    if (t.chan) s += "+chan";
    if (t.touch) s += "+touch";
    if (t.spawn) s += "+spawn";
    if (t.lockPlayer) s += "+lockplayer";
    if (t.lockCam) s += "+lockcam";
    if (t.id == 901 && t.center) s += "+center";
    if (t.id == 3006) { const auto w = g_areaWhy.find(t.uid); s += w == g_areaWhy.end() ? "+norow" : w->second; }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: trig_uncovered <reference dir> [--tsv out]\n"); return 2; }
    const std::string ref = argv[1];
    std::string tsv, areaTargetsFile;
    for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--tsv") tsv = argv[++i]; else if (std::string(argv[i]) == "--areatargets") areaTargetsFile = argv[++i];

    std::unordered_map<int, std::vector<int>> groupsOf;
    {
        std::ifstream f(ref + "/objgroups.txt");
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream is(line);
            int uid;
            if (!(is >> uid)) continue;
            int g;
            auto& v = groupsOf[uid];
            while (is >> g) v.push_back(g);
        }
    }
    std::unordered_map<int, int> typeOf;
    long long nObjects = 0;
    {
        std::ifstream f(ref + "/objrects.txt");
        std::string line;
        std::getline(f, line);
        const auto head = split(trim(line), ',');
        int cUid = -1, cType = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "uid") cUid = (int)i; if (head[i] == "type") cType = (int)i; }
        while (cUid >= 0 && cType >= 0 && std::getline(f, line)) {
            const auto c = split(trim(line), ',');
            if ((int)c.size() <= std::max(cUid, cType)) continue;
            typeOf[std::atoi(c[(size_t)cUid].c_str())] = std::atoi(c[(size_t)cType].c_str());
            ++nObjects;
        }
    }
    TrigShadow S;
    {   // the objects (positions do not matter for coverage) with their groups, in the level's order
        long long rank = 0;
        std::ifstream f(ref + "/objgroups.txt");
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream is(line);
            int uid;
            if (!(is >> uid)) continue;
            std::vector<int> gs;
            int g;
            while (is >> g) gs.push_back(g);
            S.addObject(uid, 0, 0, 0, 0, gs, rank++);
        }
    }
    // areatargets.txt (cfg areadump=1): the layer's (key -> index) map, the arrays and the dictionaries -- without it an Area's target-group index is unknown and none is run
    std::unordered_map<int, unsigned> trigKey;
    std::unordered_map<unsigned, int> mapIndex;
    bool haveTargets = false;
    if (!areaTargetsFile.empty()) {
        std::ifstream tf(areaTargetsFile);
        std::string line;
        while (std::getline(tf, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream is(line);
            std::string kind;
            is >> kind;
            haveTargets = true;
            if (kind == "T") { int u; unsigned key; if (is >> u >> key) trigKey[u] = key; }
            else if (kind == "M") { unsigned key; int ix, cnt; if (is >> key >> ix >> cnt) mapIndex[key] = ix; }
            else if (kind == "A") { int i; is >> i; std::vector<int> v; int u; while (is >> u) v.push_back(u); S.addAreaArray(i, std::move(v)); }
            else if (kind == "D") {
                int i; is >> i;
                std::unordered_map<int, std::vector<int>> d;
                std::string tok;
                while (is >> tok) {
                    const size_t c = tok.find(':');
                    if (c == std::string::npos) continue;
                    auto& v = d[std::atoi(tok.substr(0, c).c_str())];
                    const std::string kids = tok.substr(c + 1);
                    size_t p = 0;
                    while (p < kids.size()) { size_t q = kids.find(',', p); if (q == std::string::npos) q = kids.size(); if (q > p) v.push_back(std::atoi(kids.substr(p, q - p).c_str())); p = q + 1; }
                }
                S.addAreaDict(i, std::move(d));
            }
        }
    }
    std::map<int, std::string> areaWhy;   // uid -> why the shadow does not run this Area Move ("" = it does)
    std::set<int> areaOkUid;   // Area Moves whose shape the shadow runs (the same test as tools/trig_replay.cpp --screenareas)
    {
        std::ifstream f(ref + "/areas.txt");
        std::string line;
        std::getline(f, line);
        const auto head = split(trim(line), ',');
        std::map<std::string, int> col;
        for (size_t i = 0; i < head.size(); ++i) col[head[i]] = (int)i;
        bool ok = true;
        for (const char* n : {"uid", "id", "target", "special", "center", "dirType", "xy", "inbound", "twoDir", "len", "lenV", "off", "offV", "modFront", "modBack", "deadzone", "moveX", "moveY", "moveXV", "moveYV", "relative", "eInType", "eInRate", "eInBuf", "eOut"}) if (!col.count(n)) ok = false;
        while (ok && std::getline(f, line)) {
            const auto c = split(trim(line), ',');
            if ((int)c.size() <= col["eOut"]) continue;
            auto D = [&](const char* n) { return std::atof(c[(size_t)col[n]].c_str()); };
            auto I = [&](const char* n) { return std::atoi(c[(size_t)col[n]].c_str()); };
            if (I("id") != 3006) continue;
            const int sp = I("special"), cen = I("center"), dt = I("dirType");
            std::string w;
            if (!(dt == 1 || dt == 2)) w += "+dir" + std::to_string(dt);
            if (I("xy") != 1) w += "+angle";
            if (D("lenV") != 0 || D("offV") != 0 || D("moveXV") != 0 || D("moveYV") != 0) w += "+variation";
            if (I("relative") != 0) w += "+relative";
            if (I("eOut") != 0) w += "+eOut";
            if (!(sp == -1 || (sp == 0 && cen > 0) || (sp <= -3 && sp >= -11))) w += "+centre" + std::to_string(sp);
            if (I("eInBuf") == -3) w += "+easeUnknown";
            if ((int)D("len") <= 0) w += "+len0";
            int index = -1;
            if (w.empty()) {
                const auto tk = trigKey.find(I("uid"));
                const auto mi = tk == trigKey.end() ? mapIndex.end() : mapIndex.find(tk->second);
                if (!haveTargets || mi == mapIndex.end()) w += "+notargets";
                else { index = mi->second; if (!S.areaTablesFor(index)) w += "+notables"; }
            }
            areaWhy[I("uid")] = w;
            if (w.empty()) {
                AreaDef a;
                a.uid = I("uid"); a.target = I("target"); a.special = sp; a.center = cen; a.dirType = dt; a.inbound = I("inbound") != 0; a.twoDir = I("twoDir") != 0;
                a.len = D("len"); a.off = D("off"); a.modFront = I("twoDir") ? -std::fabs(D("modFront")) : D("modFront"); a.modBack = D("modBack"); a.deadzone = D("deadzone");
                a.moveX = D("moveX"); a.moveY = D("moveY"); a.eInType = I("eInType"); a.eInRate = D("eInRate"); a.eInBuf = I("eInBuf"); a.index = index;
                S.addArea(a);
                areaOkUid.insert(a.uid);
            }
        }
    }
    g_areaWhy = areaWhy;
    std::vector<TrigDef> trigs;
    {
        std::ifstream f(ref + "/triggers.txt");
        std::string line;
        std::getline(f, line);
        const auto head = split(trim(line), ',');
        std::map<std::string, int> col;
        for (size_t i = 0; i < head.size(); ++i) col[head[i]] = (int)i;
        auto need = [&](const char* n) { return col.count(n) ? col[n] : -1; };
        const int cUid = need("uid"), cId = need("id"), cTarget = need("target"), cCenter = need("center"), cTouch = need("touch"), cSpawn = need("spawn"), cChan = need("chan"),
                  cLx = need("lockx"), cLy = need("locky"), cLcx = need("lockcamx"), cLcy = need("lockcamy"), cMmx = need("movemodx"), cMmy = need("movemody"),
                  cSdelay = need("sdelay"), cSord = need("sord"), cSordd = need("sordd"), cOrd = need("ord"), cRemap = need("remap"), cSilent = need("silent");
        while (std::getline(f, line)) {
            const auto c = split(trim(line), ',');
            if ((int)c.size() <= cLcy) continue;
            auto I = [&](int k) { return k >= 0 ? std::atoi(c[(size_t)k].c_str()) : 0; };
            TrigDef t;
            t.uid = I(cUid); t.id = I(cId); t.areaOk = areaOkUid.count(t.uid) > 0; t.target = I(cTarget); t.center = I(cCenter); t.chan = I(cChan); t.touch = I(cTouch); t.spawn = I(cSpawn);
            t.lockPlayer = I(cLx) != 0 || I(cLy) != 0;
            t.lockPX = I(cLx) != 0; t.lockPY = I(cLy) != 0;
            { const double mx = cMmx >= 0 ? std::atof(c[(size_t)cMmx].c_str()) : 0.0, my = cMmy >= 0 ? std::atof(c[(size_t)cMmy].c_str()) : 0.0; t.modX = mx == 0.0 ? 1.0 : (double)(float)mx; t.modY = my == 0.0 ? 1.0 : (double)(float)my; }
            t.lockCam = I(cLcx) != 0 || I(cLcy) != 0;
            t.sdelay = cSdelay >= 0 ? std::atof(c[(size_t)cSdelay].c_str()) : 0.0; t.sord = I(cSord) != 0; t.sordd = I(cSordd) != 0; t.ord = I(cOrd); t.silent = I(cSilent) != 0; t.remap = cRemap >= 0 && c[(size_t)cRemap] != "-" && !c[(size_t)cRemap].empty();
            trigs.push_back(t);
            S.addTrigger(t);
        }
    }
    S.finish();   // the shadow's own rules (what it runs, what it cannot vouch for)
    // group -> the labels of the shadow-less triggers that act on it (an attribution; the verdict is S.coveredObject)
    std::unordered_map<int, std::set<std::string>> why;
    for (const TrigDef& t : trigs) {
        if (TrigShadow::ignorableTrigger(t.id) || S.modelledT(t)) continue;
        why[t.target].insert(label(t));
        if (t.center) why[t.center].insert(label(t));
    }    long long grouped = 0, uncovered = 0, uncoveredTouch = 0, groupedTouch = 0;
    std::map<std::string, std::pair<long long, long long>> byLabel;      // label -> (objects it uncovers, touchable ones)
    std::map<std::string, std::pair<long long, long long>> only;         // objects uncovered by exactly one label
    for (const auto& kv : groupsOf) {
        if (kv.second.empty()) continue;
        ++grouped;
        const auto ty = typeOf.find(kv.first);
        const bool touch = ty != typeOf.end() && touchable(ty->second);
        if (touch) ++groupedTouch;
        const auto bi = S.byUid.find(kv.first);
        if (bi == S.byUid.end() || S.coveredObject(bi->second)) continue;
        std::set<std::string> labs;
        for (int g : kv.second) { const auto w = why.find(g); if (w != why.end()) labs.insert(w->second.begin(), w->second.end()); }
        if (labs.empty()) labs.insert("AreaGroupEdited");   // the shadow does not vouch for it, no trigger names its group: an Area it runs sits in a group something else edits
        ++uncovered;
        if (touch) ++uncoveredTouch;
        for (const auto& l : labs) { ++byLabel[l].first; if (touch) ++byLabel[l].second; }
        if (labs.size() == 1) { ++only[*labs.begin()].first; if (touch) ++only[*labs.begin()].second; }
    }
    std::printf("objects %lld, grouped %lld (touchable %lld), uncovered %lld (touchable %lld)\n", nObjects, grouped, groupedTouch, uncovered, uncoveredTouch);
    for (const auto& kv : byLabel) std::printf("  %-28s objects %8lld touchable %8lld | only this %8lld touchable %8lld\n", kv.first.c_str(), kv.second.first, kv.second.second, only[kv.first].first, only[kv.first].second);
    if (!tsv.empty()) {
        std::ofstream o(tsv);
        o << "label\tobjects\ttouchable\tonly_objects\tonly_touchable\n";
        for (const auto& kv : byLabel) o << kv.first << "\t" << kv.second.first << "\t" << kv.second.second << "\t" << only[kv.first].first << "\t" << only[kv.first].second << "\n";
        o << "#total\t" << grouped << "\t" << groupedTouch << "\t" << uncovered << "\t" << uncoveredTouch << "\n";
    }
    return 0;
}
