#pragma once
// The trigger shadow's loader: everything static about a level (objects, groups, triggers and their extra columns, Areas and their tables, Stops, channels) read from the level's dump files into a
// TrigShadow, and nothing from a flight: no player x, no trigfire.csv, no wtrace.csv. Moved out of tools/trig_replay.cpp unchanged so that the tool and the search run the same code.
// The caller adds what a flight gives (a recording's touch / spawn events, the crossing reference) and calls S.finish().
#include "dp/trigshadow.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace dp {
namespace gdtrig {

inline std::vector<std::string> tsSplit(const std::string& s, char d) {
    std::vector<std::string> out;
    size_t n = 1;
    for (char ch : s) if (ch == d) ++n;
    out.reserve(n);
    size_t from = 0;
    for (;;) {
        const size_t at = s.find(d, from);
        if (at == std::string::npos) { out.emplace_back(s, from, std::string::npos); break; }
        out.emplace_back(s, from, at - from);
        from = at + 1;
    }
    return out;
}
// A file read once: its lines as getline gives them (the caller trims and splits what it reads). A file that cannot be opened has no lines, which reads as an empty first line and no rows, as a failed getline did.
struct TsLines {
    std::vector<std::string> lines;
    const std::string& at(size_t i) const { static const std::string none; return i < lines.size() ? lines[i] : none; }
};
inline TsLines tsReadLines(const std::string& path) {
    TsLines r;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) r.lines.push_back(line);
    return r;
}
// A level string read once: its objects (the text between the ';'), as every parser of it splits them. ok = false: the file could not be opened.
struct TsLevelObjs {
    bool ok = false;
    std::vector<std::string> objs;
};
inline TsLevelObjs tsReadLevelObjs(const std::string& path) {
    TsLevelObjs r;
    std::ifstream lf(path, std::ios::binary);
    if (!lf) return r;
    std::stringstream ss; ss << lf.rdbuf();
    r.ok = true;
    r.objs = tsSplit(ss.str(), ';');
    return r;
}
inline std::string tsTrim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

inline std::map<int, std::string> g_areaWhy;   // Area Move uid -> why the shadow does not run it ("" = it does)

// an object the game's own recording follows: its place at the start (tick 1's row) and whether it was on
struct TrigTracked { int uid = 0; double x = 0, y = 0, rot = 0; int on = 1; };

struct TrigLoadOpts {
    std::string ref;             // the level's dump directory: objgroups.txt, objrects.txt, triggers.txt, areas.txt, areafx.txt, rotgameplay.txt
    std::string bitsFile;        // groupbits_init.txt (the group bits and counters at the start), "" = none
    std::string areaTargetsFile; // areatargets.txt
    std::string stopDumpFile;    // stopdump.txt
    std::string extDir;          // the directory of trigext.txt / areaeffect.txt / areaedit.txt, "" = none
    std::unordered_set<int> firedUids;   // trigger objects a record shows called (only used without trigext)
    int areaPhase = 1, spawnPhase = 2, attemptNo = 1;
    bool noArea = false, noSpawn = false, doubleRotate = false, screenAreas = false, noWindow = false, noChannels = false;
    bool noItemCompare = false, noAreaEdit = false, noUncoveredAreas = false, noAngleArea = false, noRotFloat = false;
    bool noNestedRemap = false, noRemapMainObj = false, noSpawnDelayRemap = false, noBlockers = false;
    bool settingsInert = false;  // the Settings object 3662 moves nothing (TrigShadow::ign)
    bool camLock = false;        // run the Moves locked to the camera (lockcamx / lockcamy): TrigDef::camLockOk
    bool items = false;          // run the Count (1611) and Instant Count (1811) triggers: the item counters are fed from outside (TrigShadow::addItemEvent); the Count's multi-activate comes from the level string (key 104)
    bool areaEditWide = false;   // an Edit Area (3011..3015) may also change the Area's moveDist / scale x, y / offY / relFade and its tint / opacity / colour fields (the last are not read by the shadow)
    bool areaAngle = false;      // run an Area Move in the angle shape at any angle (areas.txt angle: 0 down, 180 up ...), not only 0
    bool areaRadial = false;     // run an Area (Move / Scale) of direction type 0 (the distance from the centre point) and the `relative` angle shape (areas.txt offY / relFade)
    bool areaScale = false;      // run an Area Scale (3008, areas.txt scaleX / scaleY) that is a constant shape: the objects' size factor per step (ObjState::asx / asy)
    bool scaleTrig = false;      // run a Transform trigger (2067) that only scales: its scale and divide flags are read from the level string (levelString), matched by place
    bool pickup = false;         // run a Pickup trigger (1594, touch) in the shadow: a touch of its object switches its target group on / off (the triggers.txt actgrp column)
    bool remapChain = false;     // run a Spawn with a group remap in the shadow's own chain (the keys handed down the call), instead of leaving its chain to the record
    bool areaEOut = false;       // run an Area Move that has a separate ease-out (areas.txt eOut = 1; eOutType / eOutRate / eOutBuf)
    bool random = false;         // run the Random (1912) and Advanced Random (2068) triggers: the trigger LCG is drawn in the shadow (TrigShadow::rng_); needs the level string (scale=) and, for the Advanced Random, trigext.txt
    bool randomPin = false;      // the first step's draws use the in-reset seed 0x48c67641 and the stream restarts at 0 after it (the mod's rngearly / rngfix); false = the whole stream from 0
    bool collision = false;      // run the collision blocks and the Collision triggers (collblocks.txt in the dump directory: uid,blockid of each collision block, object id 1816)
    std::string levelString;     // the level's string (a .lvl): the Sequence triggers' lists (keys 435..439) are read from it, matched to the dump's uids by place; "" = Sequences are not run
};
struct SeqParams { std::vector<std::pair<int, int>> targets; int mode = 0, resetMode = 0; double minInt = 0, reset = 0; bool unique = false; };

// The Sequence triggers (id 3607) of a level string, by the uid objrects.txt gives them: a trigger of the string is matched to the objrects row of id 3607 at the same place (the string's y is the game's less 90:
// both are tried and the one that matches more is kept; nearest within 6 px, each row once).
inline std::unordered_map<int, SeqParams> parseSequences(const TsLevelObjs& lvl, const TsLines& objrects) {
    std::unordered_map<int, SeqParams> out;
    if (!lvl.ok) return out;
    struct Str { double x = 0, y = 0; SeqParams p; };
    std::vector<Str> strs;
    for (const std::string& o : lvl.objs) {
        if (o.compare(0, 7, "1,3607,") != 0) continue;
        const auto f = tsSplit(o, ',');
        Str s; bool ok = false;
        for (size_t i = 0; i + 1 < f.size(); i += 2) {
            const std::string& k = f[i]; const std::string& v = f[i + 1];
            if (k == "2") s.x = std::atof(v.c_str());
            else if (k == "3") { s.y = std::atof(v.c_str()); ok = true; }
            else if (k == "435") { const auto g = tsSplit(v, '.'); for (size_t j = 0; j + 1 < g.size(); j += 2) s.p.targets.push_back({std::atoi(g[j].c_str()), std::atoi(g[j + 1].c_str())}); }
            else if (k == "436") s.p.mode = std::atoi(v.c_str());
            else if (k == "437") s.p.minInt = std::atof(v.c_str());
            else if (k == "438") s.p.reset = std::atof(v.c_str());
            else if (k == "439") s.p.resetMode = std::atoi(v.c_str());
            else if (k == "505") s.p.unique = std::atoi(v.c_str()) != 0;
        }
        if (ok) strs.push_back(s);
    }
    struct Row { int uid; double cx, cy; };
    std::vector<Row> rows;
    {
        const auto head = tsSplit(tsTrim(objrects.at(0)), ',');
        int cId = -1, cUid = -1, cCx = -1, cCy = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "id") cId = (int)i; if (head[i] == "uid") cUid = (int)i; if (head[i] == "cx") cCx = (int)i; if (head[i] == "cy") cCy = (int)i; }
        for (size_t li = 1; cId >= 0 && cUid >= 0 && li < objrects.lines.size(); ++li) {
            const auto c = tsSplit(tsTrim(objrects.lines[li]), ',');
            if ((int)c.size() <= std::max(std::max(cId, cUid), std::max(cCx, cCy)) || c[(size_t)cId] != "3607") continue;
            rows.push_back({std::atoi(c[(size_t)cUid].c_str()), std::atof(c[(size_t)cCx].c_str()), std::atof(c[(size_t)cCy].c_str())});
        }
    }
    std::unordered_map<int, SeqParams> best;
    for (double dy : {0.0, 90.0}) {
        std::unordered_map<int, SeqParams> cur; std::unordered_set<int> used;
        for (const Str& s : strs) {
            int bu = -1; double bd = 1e18;
            for (const Row& r : rows) { if (used.count(r.uid)) continue; const double d = std::fabs(r.cx - s.x) + std::fabs(r.cy - (s.y + dy)); if (d < bd) { bd = d; bu = r.uid; } }
            if (bu >= 0 && bd <= 6.0) { used.insert(bu); cur[bu] = s.p; }
        }
        if (cur.size() > best.size()) best = cur;
    }
    return best;
}
// The Random (1912) and Advanced Random (2068) triggers of a level string, matched to objrects.txt's uids by place as parseSequences does. They only name the groups they may start: the shadow does not run them
// (the draw needs the attempt's RNG state), but a spawn-triggered trigger in one of their groups is then known to wait for them, and the rest of the level's spawn-triggered triggers are not un-vouched.
// 1912 Random (EffectGameObject::triggerObject case 0x778): the string's key 51 (group 1) and 71 (group 2); 2068 Advanced Random (RandTriggerGameObject): key 152, "group.weight.group.weight ...".
struct RandParams { std::vector<std::pair<int, int>> targets; double chance = 0.0; };
inline std::unordered_map<int, RandParams> parseRandoms(const TsLevelObjs& lvl, const TsLines& objrects, const char* idStr) {
    if (!lvl.ok) return {};
    const bool adv = std::strcmp(idStr, "2068") == 0;
    const std::string pre = std::string("1,") + idStr + ",";
    struct Str { double x = 0, y = 0; RandParams p; };
    std::vector<Str> strs;
    for (const std::string& o : lvl.objs) {
        if (o.compare(0, pre.size(), pre) != 0) continue;
        const auto f = tsSplit(o, ',');
        Str s; bool ok = false;
        for (size_t i = 0; i + 1 < f.size(); i += 2) {
            const std::string& k = f[i]; const std::string& v = f[i + 1];
            if (k == "2") s.x = std::atof(v.c_str());
            else if (k == "3") { s.y = std::atof(v.c_str()); ok = true; }
            else if (adv && k == "152") { const auto g = tsSplit(v, '.'); for (size_t j = 0; j + 1 < g.size(); j += 2) s.p.targets.push_back({std::atoi(g[j].c_str()), std::atoi(g[j + 1].c_str())}); }
            else if (!adv && k == "10") s.p.chance = std::atof(v.c_str());
            else if (!adv && (k == "51" || k == "71")) { const int g = std::atoi(v.c_str()); if (g > 0) s.p.targets.push_back({g, 1}); }
        }
        if (ok) strs.push_back(s);
    }
    struct Row { int uid; double cx, cy; };
    std::vector<Row> rows;
    {
        const auto head = tsSplit(tsTrim(objrects.at(0)), ',');
        int cId = -1, cUid = -1, cCx = -1, cCy = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "id") cId = (int)i; if (head[i] == "uid") cUid = (int)i; if (head[i] == "cx") cCx = (int)i; if (head[i] == "cy") cCy = (int)i; }
        for (size_t li = 1; cId >= 0 && cUid >= 0 && li < objrects.lines.size(); ++li) {
            const auto c = tsSplit(tsTrim(objrects.lines[li]), ',');
            if ((int)c.size() <= std::max(std::max(cId, cUid), std::max(cCx, cCy)) || c[(size_t)cId] != idStr) continue;
            rows.push_back({std::atoi(c[(size_t)cUid].c_str()), std::atof(c[(size_t)cCx].c_str()), std::atof(c[(size_t)cCy].c_str())});
        }
    }
    std::unordered_map<int, RandParams> best;
    for (double dy : {0.0, 90.0}) {
        std::unordered_map<int, RandParams> cur; std::unordered_set<int> used;
        for (const Str& s : strs) {
            int bu = -1; double bd = 1e18;
            for (const Row& r : rows) { if (used.count(r.uid)) continue; const double d = std::fabs(r.cx - s.x) + std::fabs(r.cy - (s.y + dy)); if (d < bd) { bd = d; bu = r.uid; } }
            if (bu >= 0 && bd <= 6.0) { used.insert(bu); cur[bu] = s.p; }
        }
        if (cur.size() > best.size()) best = cur;
    }
    return best;
}
// The objects of a level string that draw from the trigger LCG without the shadow running them: a Spawn Particle (3608, EffectGameObject::triggerObject case 0xe18) and a Spawn with a delay variation (1268, key 556: +0x784,
// SpawnTriggerGameObject::triggerObject 0x4b91f0 draws one number when it is not 0). While there is one, the shadow's stream would drift from the game's: Random / Advanced Random are not run.
inline size_t countOtherDraws(const TsLevelObjs& lvl) {
    if (!lvl.ok) return 0;
    size_t n = 0;
    for (const std::string& o : lvl.objs) {
        if (o.compare(0, 7, "1,3608,") == 0) { ++n; continue; }
        if (o.compare(0, 7, "1,1268,") != 0) continue;
        const auto f = tsSplit(o, ',');
        for (size_t i = 0; i + 1 < f.size(); i += 2) if (f[i] == "556" && std::atof(f[i + 1].c_str()) != 0.0) { ++n; break; }
    }
    return n;
}
struct SclParams { double sx = 1.0, sy = 1.0, skx = 0.0, sky = 0.0; bool divX = false, divY = false, onlyMove = false, relScale = false, relRot = false; };
// The Transform triggers (id 2067) of a level string, by the uid objrects.txt gives them (the same place match as parseSequences). Keys (bindings m_objectScaleX ... m_relativeScale): 150 / 151 scale x / y, 450 / 451 skew x / y, 133 only move, 153 / 154 divide x / y, 452 relative rotation, 577 relative scale.
inline std::unordered_map<int, SclParams> parseTransforms(const TsLevelObjs& lvl, const TsLines& objrects) {
    std::unordered_map<int, SclParams> out;
    if (!lvl.ok) return out;
    struct Str { double x = 0, y = 0; SclParams p; };
    std::vector<Str> strs;
    for (const std::string& o : lvl.objs) {
        if (o.compare(0, 7, "1,2067,") != 0) continue;
        const auto f = tsSplit(o, ',');
        Str s; bool ok = false;
        for (size_t i = 0; i + 1 < f.size(); i += 2) {
            const std::string& k = f[i]; const std::string& v = f[i + 1];
            if (k == "2") s.x = std::atof(v.c_str());
            else if (k == "3") { s.y = std::atof(v.c_str()); ok = true; }
            else if (k == "150") s.p.sx = std::atof(v.c_str());
            else if (k == "151") s.p.sy = std::atof(v.c_str());
            else if (k == "133") s.p.onlyMove = std::atoi(v.c_str()) != 0;
            else if (k == "153") s.p.divX = std::atoi(v.c_str()) != 0;
            else if (k == "154") s.p.divY = std::atoi(v.c_str()) != 0;
            else if (k == "577") s.p.relScale = std::atoi(v.c_str()) != 0;
            else if (k == "452") s.p.relRot = std::atoi(v.c_str()) != 0;
            else if (k == "450") s.p.skx = std::atof(v.c_str());
            else if (k == "451") s.p.sky = std::atof(v.c_str());
        }
        if (ok) strs.push_back(s);
    }
    struct Row { int uid; double cx, cy; };
    std::vector<Row> rows;
    {
        const auto head = tsSplit(tsTrim(objrects.at(0)), ',');
        int cId = -1, cUid = -1, cCx = -1, cCy = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "id") cId = (int)i; if (head[i] == "uid") cUid = (int)i; if (head[i] == "cx") cCx = (int)i; if (head[i] == "cy") cCy = (int)i; }
        for (size_t li = 1; cId >= 0 && cUid >= 0 && li < objrects.lines.size(); ++li) {
            const auto c = tsSplit(tsTrim(objrects.lines[li]), ',');
            if ((int)c.size() <= std::max(std::max(cId, cUid), std::max(cCx, cCy)) || c[(size_t)cId] != "2067") continue;
            rows.push_back({std::atoi(c[(size_t)cUid].c_str()), std::atof(c[(size_t)cCx].c_str()), std::atof(c[(size_t)cCy].c_str())});
        }
    }
    std::unordered_map<int, SclParams> best;
    for (double dy : {0.0, 90.0}) {
        std::unordered_map<int, SclParams> cur; std::unordered_set<int> used;
        for (const Str& s : strs) {
            int bu = -1; double bd = 1e18;
            for (const Row& r : rows) { if (used.count(r.uid)) continue; const double d = std::fabs(r.cx - s.x) + std::fabs(r.cy - (s.y + dy)); if (d < bd) { bd = d; bu = r.uid; } }
            if (bu >= 0 && bd <= 6.0) { used.insert(bu); cur[bu] = s.p; }
        }
        if (cur.size() > best.size()) best = cur;
    }
    return best;
}// The Count triggers (id 1611) of a level string that are multi-activate (key 104), by uid (the place match of parseTransforms)
inline std::unordered_set<int> parseCountMulti(const TsLevelObjs& lvl, const TsLines& objrects) {
    std::unordered_set<int> out;
    if (!lvl.ok) return out;
    struct Str { double x = 0, y = 0; bool multi = false; };
    std::vector<Str> strs;
    for (const std::string& o : lvl.objs) {
        if (o.compare(0, 7, "1,1611,") != 0) continue;
        const auto f = tsSplit(o, ',');
        Str s; bool ok = false;
        for (size_t i = 0; i + 1 < f.size(); i += 2) {
            if (f[i] == "2") s.x = std::atof(f[i + 1].c_str());
            else if (f[i] == "3") { s.y = std::atof(f[i + 1].c_str()); ok = true; }
            else if (f[i] == "104") s.multi = std::atoi(f[i + 1].c_str()) != 0;
        }
        if (ok) strs.push_back(s);
    }
    struct Row { int uid; double cx, cy; };
    std::vector<Row> rows;
    {
        const auto head = tsSplit(tsTrim(objrects.at(0)), ',');
        int cId = -1, cUid = -1, cCx = -1, cCy = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "id") cId = (int)i; if (head[i] == "uid") cUid = (int)i; if (head[i] == "cx") cCx = (int)i; if (head[i] == "cy") cCy = (int)i; }
        for (size_t li = 1; cId >= 0 && cUid >= 0 && li < objrects.lines.size(); ++li) {
            const auto c = tsSplit(tsTrim(objrects.lines[li]), ',');
            if ((int)c.size() <= std::max(std::max(cId, cUid), std::max(cCx, cCy)) || c[(size_t)cId] != "1611") continue;
            rows.push_back({std::atoi(c[(size_t)cUid].c_str()), std::atof(c[(size_t)cCx].c_str()), std::atof(c[(size_t)cCy].c_str())});
        }
    }
    std::unordered_set<int> best;
    for (double dy : {0.0, 90.0}) {
        std::unordered_set<int> cur, used;
        for (const Str& s : strs) {
            int bu = -1; double bd = 1e18;
            for (const Row& r : rows) { if (used.count(r.uid)) continue; const double d = std::fabs(r.cx - s.x) + std::fabs(r.cy - (s.y + dy)); if (d < bd) { bd = d; bu = r.uid; } }
            if (bu >= 0 && bd <= 6.0) { used.insert(bu); if (s.multi) cur.insert(bu); }
        }
        if (cur.size() >= best.size()) best = cur;
    }
    return best;
}struct TrigLoadResult { std::set<int> untracked; bool haveBits = false; std::unordered_map<int, std::vector<int>> groupsOf; };

// `tracked`: the objects a recording follows (empty = none: every grouped object of objrects.txt joins from the static dump)
inline TrigLoadResult loadTrigShadow(TrigShadow& S, const TrigLoadOpts& o, const std::vector<TrigTracked>& tracked) {
    TrigLoadResult R;
    const std::string& ref = o.ref;
    const std::string& bitsFile = o.bitsFile;
    const std::string& areaTargetsFile = o.areaTargetsFile;
    const std::string& stopDumpFile = o.stopDumpFile;
    const std::string& extDir = o.extDir;
    const std::unordered_set<int>& firedUids = o.firedUids;
    const int areaPhase = o.areaPhase, spawnPhase = o.spawnPhase, attemptNo = o.attemptNo;
    const bool noArea = o.noArea, noSpawn = o.noSpawn, doubleRotate = o.doubleRotate, screenAreas = o.screenAreas, noWindow = o.noWindow, noChannels = o.noChannels;
    const bool noItemCompare = o.noItemCompare, noAreaEdit = o.noAreaEdit, noUncoveredAreas = o.noUncoveredAreas, noAngleArea = o.noAngleArea, noRotFloat = o.noRotFloat;
    const bool noNestedRemap = o.noNestedRemap, noRemapMainObj = o.noRemapMainObj, noSpawnDelayRemap = o.noSpawnDelayRemap, noBlockers = o.noBlockers;
    std::unordered_map<int, std::vector<int>>& groupsOf = R.groupsOf;
    std::unordered_map<int, SeqParams> seqP;   // the Sequence triggers of the level string (o.levelString), by uid
    std::unordered_map<int, RandParams> randP;   // the Advanced Random triggers of the level string, by uid
    size_t nSeq = 0, nRand = 0, nNamed = 0;
    bool randRun = false;   // Random / Advanced Random are run (--trigshadowopts random, and the level has no other draw from the trigger LCG)
    // groups
    const TsLines objrectsFile = tsReadLines(ref + "/objrects.txt");   // read once: every pass below (and the parsers of the level string) reads these lines
    TsLevelObjs levelObjs; bool levelObjsRead = false;
    auto lvlObjs = [&]() -> const TsLevelObjs& { if (!levelObjsRead) { levelObjs = tsReadLevelObjs(o.levelString); levelObjsRead = true; } return levelObjs; };
    std::unordered_map<int, long long> rankOf;   // a line of objgroups.txt = the level's own object order
    {
        std::ifstream f(ref + "/objgroups.txt");
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream is(line);
            int uid;
            if (!(is >> uid)) continue;
            rankOf[uid] = (long long)rankOf.size();
            int g;
            while (is >> g) groupsOf[uid].push_back(g);
        }
    }
    // bits and counters at the start
    std::set<int> bitsOff;
    std::unordered_map<int, int> cnt;
    bool& haveBits = R.haveBits;
    if (!bitsFile.empty()) {
        std::ifstream f(bitsFile);
        std::string line;
        while (std::getline(f, line)) {
            const auto c = tsSplit(tsTrim(line), ',');
            if (c.empty()) continue;
            if (c[0] == "off" && c.size() > 1) bitsOff.insert(std::atoi(c[1].c_str()));
            else if (c[0] == "cnt" && c.size() > 2) cnt[std::atoi(c[1].c_str())] = std::atoi(c[2].c_str());
            else if (c[0] == "groupbits") haveBits = true;
        }
    }
    std::set<int>& untracked = R.untracked;
    std::set<int> trackedUids;
    for (const TrigTracked& tr : tracked) trackedUids.insert(tr.uid);
    std::unordered_set<int> inObjrects;   // the uids the level's dump has: an object the recording follows that is not among them is one the game made while it ran (a helper of the player: one per level, (0, 105) at tick 1 and elsewhere at tick 3), not the level's
    {
        const auto head = tsSplit(tsTrim(objrectsFile.at(0)), ',');
        int cUid = -1;
        for (size_t i = 0; i < head.size(); ++i) if (head[i] == "uid") cUid = (int)i;
        for (size_t li = 1; cUid >= 0 && li < objrectsFile.lines.size(); ++li) { const auto c = tsSplit(tsTrim(objrectsFile.lines[li]), ','); if ((int)c.size() > cUid) inObjrects.insert(std::atoi(c[(size_t)cUid].c_str())); }
    }
    for (const TrigTracked& r0 : tracked) {
        const int uid = r0.uid;
        if (!inObjrects.empty() && !inObjrects.count(uid)) untracked.insert(uid);
        const auto g = groupsOf.find(uid);
        // the counter at the start: the game's, else -1 for an object the first row says is off
        int counter = 0;
        const auto ci = cnt.find(uid);
        if (ci != cnt.end()) counter = ci->second;
        else if (r0.on == 0) counter = -1;
        S.addObject(uid, r0.x, r0.y, r0.rot, counter, g == groupsOf.end() ? std::vector<int>() : g->second, rankOf.count(uid) ? rankOf[uid] : -1);
    }
    // the grouped objects the game's recording does not follow (decorations: a Rotate's centre is often one) join the world from the static dump, so that what moves them moves them here too; they are not compared
    {
        const auto head = tsSplit(tsTrim(objrectsFile.at(0)), ',');
        int cUid = -1, cCx = -1, cCy = -1, cRot = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "uid") cUid = (int)i; if (head[i] == "cx") cCx = (int)i; if (head[i] == "cy") cCy = (int)i; if (head[i] == "rot") cRot = (int)i; }
        for (size_t li = 1; cUid >= 0 && li < objrectsFile.lines.size(); ++li) {
            const auto c = tsSplit(tsTrim(objrectsFile.lines[li]), ',');
            if ((int)c.size() <= std::max(std::max(cUid, cCx), std::max(cCy, cRot))) continue;
            const int uid = std::atoi(c[(size_t)cUid].c_str());
            if (trackedUids.count(uid)) continue;
            const auto g = groupsOf.find(uid);
            if (g == groupsOf.end()) continue;
            int counter = 0;
            const auto ci = cnt.find(uid);
            if (ci != cnt.end()) counter = ci->second;
            else for (int gg : g->second) if (bitsOff.count(gg)) --counter;   // the bits dump lists counters only for the objects the recording follows: an object in a group that is off at the start is -1 per such group
            S.addObject(uid, std::atof(c[(size_t)cCx].c_str()), std::atof(c[(size_t)cCy].c_str()), std::atof(c[(size_t)cRot].c_str()), counter, g->second, rankOf.count(uid) ? rankOf[uid] : -1);
            untracked.insert(uid);
        }
    }
    {   // the object types (objrects.txt `type`): a solid / type 21 / type 25 object keeps its own rotation when a Rotate turns its group
        const auto head = tsSplit(tsTrim(objrectsFile.at(0)), ',');
        int cUid = -1, cType = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "uid") cUid = (int)i; if (head[i] == "type") cType = (int)i; }
        for (size_t li = 1; cUid >= 0 && cType >= 0 && li < objrectsFile.lines.size(); ++li) {
            const auto c = tsSplit(tsTrim(objrectsFile.lines[li]), ',');
            if ((int)c.size() <= std::max(cUid, cType)) continue;
            S.setObjectType(std::atoi(c[(size_t)cUid].c_str()), std::atoi(c[(size_t)cType].c_str()));
        }
    }
    for (int g : bitsOff) S.setGroupBit(g, false);
    {   // levelsettings.txt: a level of the old version (the 2.2 fixes off) runs a Rotate in place of a running one of the same (target, centre) -- GJEffectManager byte +0x820, see TrigShadow::rotReplace
        std::ifstream lf(ref + "/levelsettings.txt"); std::string line; int fr = -1, fg = -1, fn = -1;
        while (std::getline(lf, line)) {
            line = tsTrim(line);
            const auto val = [&](const char* key) -> int { const size_t n = std::strlen(key); return line.compare(0, n, key) == 0 ? std::atoi(line.c_str() + n) : -1; };
            const int g1 = val("fixGravityBug="), n1 = val("fixNegativeScale="), r1 = val("fixRadiusCollision=");
            if (r1 >= 0) fr = r1;
            if (g1 >= 0) fg = g1;
            if (n1 >= 0) fn = n1;
        }
        S.rotReplace = fr == 0 && fg == 0 && fn == 0;
    }
    S.areaPhase = areaPhase;
    S.spawnModel = !noSpawn;
    S.floatRotate = !doubleRotate;
    S.spawnPhase = spawnPhase;
    S.sectionWindow = !noWindow;
    S.settingsInert = o.settingsInert;
    S.spawnDelayRemap = !noSpawnDelayRemap;
    S.remapMainObj = !noRemapMainObj;
    S.nestedRemap = !noNestedRemap;
    S.rotFloat = !noRotFloat;
    S.uncoveredAreasOn = !noUncoveredAreas;
    std::unordered_map<int, AreaDef> areas;
    std::unordered_map<int, int> areaIndex;   // trigger uid -> its target-group index (areatargets.txt `T` row's key through the `M` rows)
    std::unordered_map<unsigned, int> mapIndex;
    std::vector<std::pair<int, char>> flagUids;   // areatargets.txt X / K / P rows, applied once the objects are in
    std::unordered_map<int, unsigned> trigKey;
    if (!areaTargetsFile.empty()) {
        std::ifstream tf(areaTargetsFile);
        std::string line;
        while (std::getline(tf, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream is(line);
            std::string kind;
            is >> kind;
            if (kind == "X" || kind == "K" || kind == "P") { int u; while (is >> u) flagUids.push_back({u, kind[0]}); }
            else if (kind == "T") { int u; unsigned key; if (is >> u >> key) trigKey[u] = key; }
            else if (kind == "M") { unsigned key; int ix, cnt; if (is >> key >> ix >> cnt) mapIndex[key] = ix; }
            else if (kind == "A") {
                int i; is >> i;
                std::vector<int> v;
                int u;
                while (is >> u) v.push_back(u);
                S.addAreaArray(i, std::move(v));
            } else if (kind == "D") {
                int i; is >> i;
                std::unordered_map<int, std::vector<int>> d;
                std::string tok;
                while (is >> tok) {
                    const size_t c = tok.find(':');
                    if (c == std::string::npos) continue;
                    auto& v = d[std::atoi(tok.substr(0, c).c_str())];
                    std::string kids = tok.substr(c + 1);
                    size_t p = 0;
                    while (p < kids.size()) {
                        size_t q = kids.find(',', p);
                        if (q == std::string::npos) q = kids.size();
                        if (q > p) v.push_back(std::atoi(kids.substr(p, q - p).c_str()));
                        p = q + 1;
                    }
                }
                S.addAreaDict(i, std::move(d));
            }
        }
    }
    for (const auto& fu : flagUids) S.setAreaFlag(fu.first, fu.second);
    {   // areavar.txt (the mod's cfg areavar=1 observation, one section per attempt): the layer's variation table V (the non-zero entries) and the objects' variance indices; the first section is read.
        // An Area with a variation term is run only when this file is there (TrigShadow::varKnown); without it nothing changes
        std::ifstream f(ref + "/areavar.txt");
        if (f) {
            std::string line; std::vector<float> tab(2000, 0.0f); std::unordered_map<int, int> idx; int sections = 0; bool any = false;
            while (std::getline(f, line)) {
                if (!line.empty() && line[0] == '#') { if (++sections > 1) break; continue; }
                char kind[16] = {0}; int a = 0; double b = 0;
                if (std::sscanf(line.c_str(), "%15s %d %lf", kind, &a, &b) != 3) continue;
                if (!std::strcmp(kind, "table")) { if (a >= 0 && a < 2000) { tab[(size_t)a] = (float)b; any = true; } }
                else if (!std::strcmp(kind, "object")) idx[a] = (int)b;
            }
            if (any) S.setVariance(std::move(tab), idx);
        }
    }
    if (!S.varKnown() && !inObjrects.empty()) {   // no areavar.txt: what the mod pins while it drives (rngfix) gives the same values. V is the LCG from state 0 (2000 draws: 2 * (r / 32767) - 1); an object's index is the draw its place
        // takes, in uid order over the level's objects (objrects.txt), from state 2531011: (int)((r / 32767.0f) * 1900.0f). Measured against the mod's areavar.txt on custom level A, custom level B and custom level C (117293 / 68635 / 38433 listed
        // objects): 0 differences, every unlisted object's draw is 0, the table equal in all 2000 entries (notes\gd_recording_dependencies_1113_gd04.md; spec 1030 section 2)
        std::vector<int> us(inObjrects.begin(), inObjrects.end());
        std::sort(us.begin(), us.end());
        std::vector<float> tab(2000);
        uint64_t st = 0;
        for (int j = 0; j < 2000; ++j) { st = st * 0x343fdULL + 0x269ec3ULL; const float r = (float)((st >> 16) & 0x7fffULL); tab[(size_t)j] = 2.0f * (r / 32767.0f) - 1.0f; }
        std::unordered_map<int, int> idx;
        uint64_t s2 = 2531011ULL;
        for (int u : us) { s2 = s2 * 0x343fdULL + 0x269ec3ULL; const float r = (float)((s2 >> 16) & 0x7fffULL); const int v = (int)((r / 32767.0f) * 1900.0f); if (v != 0) idx[u] = v; }
        S.setVariance(std::move(tab), idx);
    }
    {   // areas.txt: the Area Moves whose shape the shadow runs
        std::ifstream f(ref + "/areas.txt");
        std::string line;
        std::getline(f, line);
        const auto head = tsSplit(tsTrim(line), ',');
        std::map<std::string, int> col;
        for (size_t i = 0; i < head.size(); ++i) col[head[i]] = (int)i;
        auto has = [&](const char* n) { return col.count(n) > 0; };
        bool ok = !noArea;
        for (const char* n : {"uid", "id", "target", "special", "center", "dirType", "xy", "inbound", "len", "lenV", "off", "offV", "modFront", "modBack", "deadzone", "moveX", "moveXV", "moveY", "moveYV", "twoDir", "relative", "eInType", "eInRate", "eInBuf", "eOut"}) if (!has(n)) ok = false;
        while (ok && std::getline(f, line)) {
            const auto c = tsSplit(tsTrim(line), ',');
            if ((int)c.size() <= col["eOut"]) continue;
            auto D = [&](const char* n) { return std::atof(c[(size_t)col[n]].c_str()); };
            auto I = [&](const char* n) { return std::atoi(c[(size_t)col[n]].c_str()); };
            if (I("id") == 3008 && o.areaScale && has("scaleX") && has("scaleXV") && has("scaleY") && has("scaleYV")) {   // an Area Scale of a constant shape
                AreaDef a;
                a.uid = I("uid"); a.target = I("target"); a.special = I("special"); a.center = I("center"); a.dirType = I("dirType"); a.inbound = I("inbound") != 0; a.twoDir = I("twoDir") != 0;
                a.len = D("len"); a.off = D("off"); a.modFront = I("twoDir") ? -std::fabs(D("modFront")) : D("modFront"); a.modBack = D("modBack"); a.deadzone = D("deadzone");
                a.eInType = I("eInType"); a.eInRate = D("eInRate"); a.eInBuf = I("eInBuf");
                a.scaleKind = true; a.scaleX = D("scaleX"); a.scaleY = D("scaleY");
                if (has("offY")) a.offY = D("offY");
                a.lenV = D("lenV"); a.offV = D("offV"); if (has("offYV")) a.offYV = D("offYV");   // the variation terms (areavar.txt: the layer's table and the objects' indices); without them such an Area is not run
                const bool shape = (a.dirType == 1 || a.dirType == 2 || (a.dirType == 0 && o.areaRadial && has("offY") && has("offYV") && (D("offYV") == 0 || S.varKnown()))) && (!(a.lenV != 0 || a.offV != 0 || a.offYV != 0) || S.varKnown()) && D("scaleXV") == 0 && D("scaleYV") == 0 && I("relative") == 0 && I("eOut") == 0 && a.eInBuf != -3
                                   && (a.special == -1 || (a.special == 0 && a.center > 0) || (a.special <= -3 && a.special >= -11 && screenAreas)) && (int)a.len > 0;
                if (areaIndex.empty()) for (const auto& tk : trigKey) { const auto mi = mapIndex.find(tk.second); if (mi != mapIndex.end()) areaIndex[tk.first] = mi->second; }
                if (shape && !areaTargetsFile.empty() && areaIndex.count(a.uid) && areaIndex[a.uid] < 1) { a.index = 0; areas[a.uid] = a; S.addArea(a); continue; }
            }
            if (I("id") != 3006) {   // an Area Rotate / Scale is never run: the objects it reaches through its target-group arrays are told to the shadow as not vouched for
                const int aid = I("id");
                if ((aid == 3007 || aid == 3008) && !areaTargetsFile.empty()) {
                    if (areaIndex.empty()) for (const auto& tk : trigKey) { const auto mi = mapIndex.find(tk.second); if (mi != mapIndex.end()) areaIndex[tk.first] = mi->second; }
                    const auto ai = areaIndex.find(I("uid"));
                    if (ai != areaIndex.end()) S.addUncoveredArea(ai->first, ai->second);
                }
                continue;
            }
            AreaDef a;
            a.uid = I("uid"); a.target = I("target"); a.special = I("special"); a.center = I("center"); a.dirType = I("dirType"); a.inbound = I("inbound") != 0; a.twoDir = I("twoDir") != 0;
            a.len = D("len"); a.off = D("off"); a.modFront = I("twoDir") ? -std::fabs(D("modFront")) : D("modFront");   /* processAreaEffects: two-sided sets the front multiplier negative, so the far side reads |s| */ a.modBack = D("modBack"); a.deadzone = D("deadzone"); a.moveX = D("moveX"); a.moveY = D("moveY");
            a.eInType = I("eInType"); a.eInRate = D("eInRate"); a.eInBuf = I("eInBuf");
            const bool eOutCols = has("eOutType") && has("eOutRate") && has("eOutBuf");
            if (o.areaEOut && eOutCols && I("eOut") != 0) { a.eOutOn = true; a.eOutType = I("eOutType"); a.eOutRate = D("eOutRate"); a.eOutBuf = I("eOutBuf"); }
            const bool hasRel = has("relative") && has("relFade") && has("offY") && has("offYV");
            if (hasRel) { a.offY = D("offY"); a.relative = I("relative") != 0; a.relFade = D("relFade"); }
            a.lenV = D("lenV"); a.offV = D("offV"); a.moveXV = D("moveXV"); a.moveYV = D("moveYV");
            if (has("offYV")) a.offYV = D("offYV");
            if (has("moveDistV")) a.moveDistV = D("moveDistV");
            if (has("angleV")) a.angleV = D("angleV");
            const bool varFine = !a.anyVar() || S.varKnown();   // a variation term needs the table and the indices (areavar.txt); without them the Area is left to the record as before
            // the distance-at-an-angle shape; `relative` (direction type 0) moves along the centre -> object line, whatever the angle
            const bool relShape = !noAngleArea && hasRel && a.dirType == 0 && I("relative") != 0 && I("xy") == 0 && (D("moveDist") != 0 || a.moveDistV != 0) && varFine && D("moveX") == 0 && D("moveY") == 0;
            const bool angleShape = relShape || (!noAngleArea && I("xy") == 0 && (D("angle") == 0 || o.areaAngle) && varFine && (D("moveDist") != 0 || a.moveDistV != 0) && D("moveX") == 0 && D("moveY") == 0);
            if (angleShape) { a.angleMode = true; a.moveDist = D("moveDist"); a.angle = relShape ? 0.0 : D("angle"); }
            const bool dirOk = a.dirType == 1 || a.dirType == 2 || (a.dirType == 0 && hasRel && o.areaRadial && (D("offYV") == 0 || S.varKnown()));
            const bool shape = dirOk && (I("xy") == 1 || angleShape) && varFine && (I("relative") == 0 || relShape) && (I("eOut") == 0 || a.eOutOn) && a.eOutBuf != -3
                               && (a.special == -1 || (a.special == 0 && a.center > 0) || (a.special <= -3 && a.special >= -11 && screenAreas)) && a.eInBuf != -3 && (int)a.len > 0;
            {   // why the shadow does not run this Area Move (the labels trig_uncovered gave)
                std::string w;
                if (!(a.dirType == 1 || a.dirType == 2)) w += "+dir" + std::to_string(a.dirType);
                if (I("xy") != 1 && !angleShape) w += "+angle";
                if (D("lenV") != 0 || D("offV") != 0 || D("moveXV") != 0 || D("moveYV") != 0) w += "+variation";
                if (I("relative") != 0) w += "+relative";
                if (I("eOut") != 0 && !a.eOutOn) w += "+eOut";
                if (!(a.special == -1 || (a.special == 0 && a.center > 0) || (a.special <= -3 && a.special >= -11))) w += "+centre" + std::to_string(a.special);
                if (a.eInBuf == -3) w += "+easeUnknown";
                if ((int)a.len <= 0) w += "+len0";
                g_areaWhy[a.uid] = w;
            }
            // T30: an Area Move that displaces nothing (distance, x and y with their variations all 0: lv22 uid 10705, length 1500 +- 500, centre 304, the group 330 of the Toggled walls 6269 / 6331 ...) is run by nothing and
            // reaches no object in a way that counts: it is not left as an uncovered Area (the objects it names keep their coverage) -- the length / offset / angle variation only changes where it reaches, not what it does
            if (!shape && has("moveDist") && has("moveDistV") && D("moveDist") == 0 && D("moveDistV") == 0 && D("moveX") == 0 && D("moveXV") == 0 && D("moveY") == 0 && D("moveYV") == 0) {
                S.addNoopTrigger(a.uid);
                g_areaWhy[a.uid] = "+nodisplacement";
                continue;
            }
            if (areaIndex.empty()) for (const auto& tk : trigKey) { const auto mi = mapIndex.find(tk.second); if (mi != mapIndex.end()) areaIndex[tk.first] = mi->second; }
            if (shape && !areaTargetsFile.empty() && areaIndex.count(a.uid)) {
                a.index = areaIndex[a.uid];
                if (S.areaTablesFor(a.index)) { areas[a.uid] = a; S.addArea(a); }
            }
            if (!areas.count(a.uid) && !areaTargetsFile.empty() && areaIndex.count(a.uid)) S.addUncoveredArea(a.uid, areaIndex[a.uid]);
            if (g_areaWhy[a.uid].empty() && !areas.count(a.uid)) g_areaWhy[a.uid] = (areaTargetsFile.empty() || !areaIndex.count(a.uid)) ? "+notargets" : "+notables";
        }
    }
    std::unordered_map<int, std::pair<int, bool>> stopModes;   // Stop uid -> (mode, uses a control ID)
    if (!stopDumpFile.empty()) {
        std::ifstream sf(stopDumpFile);
        std::string sl;
        std::getline(sf, sl);
        while (std::getline(sf, sl)) {
            const auto c = tsSplit(tsTrim(sl), ',');
            if (c.size() < 6 || c[1] != "1616") continue;
            stopModes[std::atoi(c[0].c_str())] = {std::atoi(c[3].c_str()), c[4] != "0"};
        }
    }
    // trigext.txt (cfg trigext=1): one row per trigger object, those triggers.txt leaves out too
    struct ExtRow { int actgrp = 0; int id = 0, ord = 0, chan = 0, spawn = 0, touch = 0, tp1 = 0, tp2 = 0, mva = 0, target = 0, center = 0, item1 = 0, item2 = 0, i1mode = 0, i2mode = 0, tmode = 0, res1 = 0, res2 = 0, res3 = 0, rnd1 = 0, rnd2 = 0, sgn1 = 0, sgn2 = 0; double cx = 0, cy = 0, mod1 = 0, mod2 = 0, tol = 0; std::vector<unsigned> raw; };   // raw: itemraw's words from +0x740 (hex), of an Item trigger / a Pickup trigger / a Teleport
    std::unordered_map<int, ExtRow> extRows;
    if (!extDir.empty()) {
        std::ifstream ef(extDir + "/trigext.txt");
        std::string el;
        if (ef && std::getline(ef, el)) {
            const auto eh = tsSplit(tsTrim(el), ',');
            std::map<std::string, int> ec;
            for (size_t i = 0; i < eh.size(); ++i) ec[eh[i]] = (int)i;
            bool eok = true;
            for (const char* n : {"uid", "id", "cx", "cy", "ord", "chan", "spawn", "touch", "tp1", "tp2", "mva"}) if (!ec.count(n)) eok = false;
            while (eok && std::getline(ef, el)) {
                const auto c = tsSplit(tsTrim(el), ',');
                if ((int)c.size() < (int)eh.size()) continue;
                ExtRow r;
                r.id = std::atoi(c[(size_t)ec["id"]].c_str()); r.cx = std::atof(c[(size_t)ec["cx"]].c_str()); r.cy = std::atof(c[(size_t)ec["cy"]].c_str());
                r.ord = std::atoi(c[(size_t)ec["ord"]].c_str()); r.chan = std::atoi(c[(size_t)ec["chan"]].c_str()); r.spawn = std::atoi(c[(size_t)ec["spawn"]].c_str()); r.touch = std::atoi(c[(size_t)ec["touch"]].c_str());
                if (ec.count("target") && ec.count("center") && ec.count("item1") && ec.count("tol")) {
                    r.target = std::atoi(c[(size_t)ec["target"]].c_str()); r.center = std::atoi(c[(size_t)ec["center"]].c_str());
                    r.item1 = std::atoi(c[(size_t)ec["item1"]].c_str()); r.item2 = std::atoi(c[(size_t)ec["item2"]].c_str()); r.i1mode = std::atoi(c[(size_t)ec["i1mode"]].c_str()); r.i2mode = std::atoi(c[(size_t)ec["i2mode"]].c_str());
                    r.mod1 = std::atof(c[(size_t)ec["mod1"]].c_str()); r.mod2 = std::atof(c[(size_t)ec["mod2"]].c_str()); r.tol = std::atof(c[(size_t)ec["tol"]].c_str());
                    r.res1 = std::atoi(c[(size_t)ec["res1"]].c_str()); r.res2 = std::atoi(c[(size_t)ec["res2"]].c_str()); r.res3 = std::atoi(c[(size_t)ec["res3"]].c_str());
                    r.rnd1 = std::atoi(c[(size_t)ec["rnd1"]].c_str()); r.rnd2 = std::atoi(c[(size_t)ec["rnd2"]].c_str()); r.sgn1 = std::atoi(c[(size_t)ec["sgn1"]].c_str()); r.sgn2 = std::atoi(c[(size_t)ec["sgn2"]].c_str());
                }
                if (ec.count("tgtmode")) r.tmode = std::atoi(c[(size_t)ec["tgtmode"]].c_str());
                if (ec.count("itemraw") && c[(size_t)ec["itemraw"]] != "-") for (const std::string& h : tsSplit(c[(size_t)ec["itemraw"]], ':')) r.raw.push_back((unsigned)std::strtoul(h.c_str(), nullptr, 16));
                if (ec.count("actgrp")) r.actgrp = std::atoi(c[(size_t)ec["actgrp"]].c_str());
                if (ec.count("item1") && ec.count("item2")) { r.item1 = std::atoi(c[(size_t)ec["item1"]].c_str()); r.item2 = std::atoi(c[(size_t)ec["item2"]].c_str()); }
                r.tp1 = std::atoi(c[(size_t)ec["tp1"]].c_str()); r.tp2 = std::atoi(c[(size_t)ec["tp2"]].c_str()); r.mva = std::atoi(c[(size_t)ec["mva"]].c_str());
                extRows[std::atoi(c[(size_t)ec["uid"]].c_str())] = r;
            }
        }
    }
    const bool haveExt = !extRows.empty();
    // An Item Compare (3620) the shadow can run: it reads item counters (mode 1) that no Count / Instant Count / Item Edit / Pickup / Time Event of the level writes (so they stay 0), or the attempt number (mode 5),
    // and nothing else (mode 2 timers, 3 points and 4 time are not had)
    std::set<int> compareOk;
    // T57: the item values the shadow keeps. An item's number is had when every trigger that writes it is one the shadow runs (a Pickup 1817, an Item Edit 3619, an Item Persist 3641 whose own reads are had too: a fixpoint),
    // and nothing else writes it (a Time Event 3614 names an item it counts; Collision 1815's item fields are collision BLOCK ids, not items; Count 1611 / 1811 only read). `itemUnknown` is the set that is not had;
    // runPk / runEd / runPs are the writers that run.
    std::set<int> itemUnknown, runPk, runEd, runPs;
    if (haveExt && !noItemCompare) {
        auto raw = [](const ExtRow& r, size_t i) -> unsigned { return i < r.raw.size() ? r.raw[i] : 0u; };
        auto flagByte = [&](const ExtRow& r, size_t w, int b) -> int { return (int)((raw(r, w) >> (8 * b)) & 0xffu); };
        for (const auto& kv : extRows) if (kv.second.id == 3614) itemUnknown.insert(kv.second.item1);   // (the other writers are decided below)
        // what a value read of mode `mode` from `item` needs: mode 1 an item that is had, mode 5 the attempt number; timers (2), points (3) and the main time (4) are not had
        auto readHad = [&](int mode, int item) { return mode == 5 || (mode == 1 && !itemUnknown.count(item)); };
        // an Item trigger's two inputs, as activateItemEditTrigger / activateItemCompareTrigger decide which of them is read
        auto usedItem = [](int mode, int item) { return !(mode == 0 || (item < 1 && (unsigned)(mode - 3) > 1u)); };
        std::set<int> bad;   // writers the shadow does not run
        if (attemptNo != 1)   // a persistent item carries its value from the attempt before: not known
            for (const auto& kv : extRows) if (kv.second.id == 3641 && flagByte(kv.second, 13, 0) != 0) itemUnknown.insert(kv.second.item1);
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto& kv : extRows) {
                const ExtRow& r = kv.second;
                if (r.id != 1817 && r.id != 3619 && r.id != 3641) continue;
                if (bad.count(kv.first)) continue;
                bool ok = r.touch == 0;
                if (r.id == 1817) {   // count +0x740, mode +0x744, override byte +0x749, factor +0x74c
                    const int mode = (int)raw(r, 1);
                    ok = ok && r.raw.size() >= 4 && mode >= 0 && mode <= 2;
                    const bool override = flagByte(r, 2, 1) != 0;
                    if (ok && (mode != 0 || !override) && itemUnknown.count(r.item1)) ok = false;   // it reads its own item's number
                } else if (r.id == 3619) {
                    const int tm = r.tmode < 1 ? 1 : r.tmode;
                    ok = ok && tm == 1;
                    const bool u1 = usedItem(r.i1mode, r.item1), u2 = usedItem(r.i2mode, r.item2);
                    if (ok && u1 && !readHad(r.i1mode, r.item1)) ok = false;
                    if (ok && u2 && !readHad(r.i2mode, r.item2)) ok = false;
                    if (ok && r.res1 != 0 && itemUnknown.count(r.target)) ok = false;   // it combines the result with the target's own number
                } else {   // 3641: byte flags +0x774 enable, +0x775 all, +0x776 reset, +0x777 timer
                    ok = ok && r.raw.size() >= 14 && flagByte(r, 13, 3) == 0;
                    if (ok && flagByte(r, 13, 1) != 0 && flagByte(r, 13, 2) != 0) for (const auto& kv2 : extRows) if (kv2.second.id == 3641 && bad.count(kv2.first)) ok = false;   // "all" + reset: the persistent set must be the game's
                }
                if (!ok) {
                    bad.insert(kv.first); changed = true;
                    if (r.id == 1817 || r.id == 3641) itemUnknown.insert(r.item1);
                    else if (r.id == 3619) itemUnknown.insert(r.target);
                    if (r.id == 3641) for (const auto& kv2 : extRows) if (kv2.second.id == 3641) itemUnknown.insert(kv2.second.item1);   // (an "all" row may reset any of them)
                }
            }
        }
        for (const auto& kv : extRows) {
            const ExtRow& r = kv.second;
            if (bad.count(kv.first)) continue;
            if (r.id == 1817) runPk.insert(kv.first); else if (r.id == 3619) runEd.insert(kv.first); else if (r.id == 3641) runPs.insert(kv.first);
        }
        for (const auto& kv : extRows) {
            const ExtRow& r = kv.second;
            if (r.id != 3620) continue;
            const int m1 = r.i1mode < 1 ? 1 : r.i1mode;
            const bool use2 = !(r.i2mode == 0 || (r.item2 < 1 && (unsigned)(r.i2mode - 3) > 1u));
            if (readHad(m1, r.item1) && (!use2 || readHad(r.i2mode, r.item2))) compareOk.insert(kv.first);
        }
    }
    bool haveAreaFx = false;
    if (haveExt) {   // areaeffect.txt (cfg stopdump=1): uid,id,effectid,useeffectid of every Area trigger
        std::ifstream af(extDir + "/areaeffect.txt");
        std::unordered_map<int, int> fx;
        std::string al;
        if (af && std::getline(af, al)) {
            while (std::getline(af, al)) { const auto c = tsSplit(tsTrim(al), ','); if (c.size() >= 3) fx[std::atoi(c[0].c_str())] = std::atoi(c[2].c_str()); }
            haveAreaFx = !fx.empty();
            S.setAreaEffectIds(fx);
        }
    }
    bool collisionBlocksLoaded = false;
    if (o.collision) {   // collblocks.txt (uid,blockid): the collision blocks (object id 1816, level string key 80); their rect size is objrects.txt's w, h
        std::ifstream cf(ref + "/collblocks.txt");
        std::unordered_map<int, int> bid;
        std::string cl;
        while (cf && std::getline(cf, cl)) { const auto c = tsSplit(tsTrim(cl), ','); if (c.size() >= 2 && c[0] != "uid") bid[std::atoi(c[0].c_str())] = std::atoi(c[1].c_str()); }
        if (!bid.empty()) {
            const auto head = tsSplit(tsTrim(objrectsFile.at(0)), ',');
            int cUid = -1, cW = -1, cH = -1;
            for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "uid") cUid = (int)i; if (head[i] == "w") cW = (int)i; if (head[i] == "h") cH = (int)i; }
            std::map<int, std::pair<double, double>> sz;
            for (size_t li = 1; cUid >= 0 && cW >= 0 && cH >= 0 && li < objrectsFile.lines.size(); ++li) {
                const auto c = tsSplit(tsTrim(objrectsFile.lines[li]), ',');
                if ((int)c.size() <= std::max(cUid, std::max(cW, cH))) continue;
                const int u = std::atoi(c[(size_t)cUid].c_str());
                if (bid.count(u)) sz[u] = {std::atof(c[(size_t)cW].c_str()), std::atof(c[(size_t)cH].c_str())};
            }
            // the layer's array order = the level's own object order (objgroups.txt's line)
            std::vector<std::pair<long long, int>> order;
            for (const auto& kv : bid) order.push_back({rankOf.count(kv.first) ? rankOf[kv.first] : (long long)kv.first + (1LL << 40), kv.first});
            std::sort(order.begin(), order.end());
            for (const auto& ou : order) { const auto z = sz.find(ou.second); if (z != sz.end()) S.addCollisionBlock(ou.second, bid[ou.second], z->second.first, z->second.second); }
            collisionBlocksLoaded = !S.collBlocks.empty();
            S.collisionModel = collisionBlocksLoaded;
            std::printf("collision: %zu collision blocks from collblocks.txt\n", S.collBlocks.size());
        }
    }
    std::set<int> editOkUid;   // Edit Area triggers (areaedit.txt, cfg trigext=1) whose every changed field is one the shadow keeps
    if (haveExt && !noAreaEdit) {
        std::ifstream ef(extDir + "/areaedit.txt");
        std::string el;
        if (ef && std::getline(ef, el)) {
            const auto eh = tsSplit(tsTrim(el), ',');
            std::map<std::string, int> ec;
            for (size_t i = 0; i < eh.size(); ++i) ec[eh[i]] = (int)i;
            bool eok = true;
            for (const char* n : {"uid", "id", "target", "useeffectid", "dur", "intype", "inrate", "inbuf", "raw740"}) if (!ec.count(n)) eok = false;
            while (eok && std::getline(ef, el)) {
                const auto c = tsSplit(tsTrim(el), ',');
                if ((int)c.size() < (int)eh.size()) continue;
                AreaEdit e;
                e.uid = std::atoi(c[(size_t)ec["uid"]].c_str()); e.target = std::atoi(c[(size_t)ec["target"]].c_str()); e.useEffect = std::atoi(c[(size_t)ec["useeffectid"]].c_str()) != 0;
                e.dur = (float)std::atof(c[(size_t)ec["dur"]].c_str()); e.type = std::atoi(c[(size_t)ec["intype"]].c_str()); e.rate = (float)std::atof(c[(size_t)ec["inrate"]].c_str()); e.buf = std::atoi(c[(size_t)ec["inbuf"]].c_str());
                std::vector<unsigned> w;
                for (const std::string& h : tsSplit(c[(size_t)ec["raw740"]], ':')) w.push_back((unsigned)std::strtoul(h.c_str(), nullptr, 16));
                auto word = [&](unsigned off) -> unsigned { const size_t k = (off - 0x740u) / 4u; return k < w.size() ? w[k] : 0u; };
                auto isF = [](unsigned v) { float f; std::memcpy(&f, &v, 4); return f == -99.0f; };
                // loadTransitions' slots: index -> the trigger's word and whether it is a float or an int there; only those the AreaDef keeps are run (the others must be unset)
                struct Slot { int idx; unsigned off; bool flt; bool kept; };
                static const Slot slots[] = {{0, 0x744, false, true}, {1, 0x748, false, false}, {2, 0x74c, false, true}, {3, 0x750, false, false}, {4, 0x7c8, true, false}, {5, 0x7cc, true, false}, {6, 0x7f0, true, true},
                                             {7, 0x75c, false, false}, {8, 0x760, false, false}, {9, 0x774, false, false}, {10, 0x778, false, false}, {11, 0x7a8, false, true}, {12, 0x7ac, false, false}, {13, 0x7b0, false, true},
                                             {14, 0x7b4, false, false}, {15, 0x764, true, false}, {16, 0x768, true, false}, {17, 0x76c, true, false}, {18, 0x770, true, false}, {19, 0x7dc, true, false}, {20, 0x7e0, true, false},
                                             {21, 0x7d0, true, false}, {22, 0x7d4, true, false}, {23, 0x7e4, true, false}, {24, 0x7e8, true, false}, {25, 0x754, false, false}, {26, 0x758, false, false}, {27, 0x78c, true, false}};
                bool ok = true;
                for (const Slot& s : slots) {
                    const unsigned v = word(s.off);
                    const bool unset = s.flt ? isF(v) : (int)v == -99;
                    if (unset) continue;
                    const bool wide = o.areaEditWide && (s.idx == 7 || s.idx == 15 || s.idx == 17 || (s.idx >= 21 && s.idx <= 25) || s.idx == 27);   // moveDist, scale x / y, offY, relFade (AreaDef keeps them) and the tint / opacity slots (nothing the shadow reads)
                    if (!s.kept && !wide) { ok = false; continue; }
                    float fv;
                    if (s.flt) std::memcpy(&fv, &v, 4); else fv = (float)(int)v;
                    e.fields.push_back({s.idx, fv});
                }
                if ((word(0x7ec) & 0xff00u) && !o.areaEditWide) ok = false;   // +0x7ed: the colour triple (slots 28..30), not kept (a colour: nothing the shadow reads, with --areaeditwide)
                std::sort(e.fields.begin(), e.fields.end());
                S.addAreaEditInfo(e.uid, e.target, e.useEffect);
                if (ok) { S.addAreaEdit(e); editOkUid.insert(e.uid); }
            }
        }
    }
    if (!haveAreaFx) {   // areafx.txt: uid,effectId of every Area trigger (made from the level string, oneoff/py/areafx_from_lvl.py): what an Area Stop (3024) matches
        std::ifstream af(ref + "/areafx.txt");
        std::unordered_map<int, int> fx;
        std::string al;
        if (af && std::getline(af, al)) {
            while (std::getline(af, al)) { const auto c = tsSplit(tsTrim(al), ','); if (c.size() >= 2) fx[std::atoi(c[0].c_str())] = std::atoi(c[1].c_str()); }
            haveAreaFx = !fx.empty();
            S.setAreaEffectIds(fx);
        }
    }
    // the items something the shadow does not run writes (an Item Edit 3619, an Item Persist 3641 [item / item2 of their rows], a Pickup Trigger 1817 [items.txt]): a Count on one of them is left to the record
    std::set<int> itemsWrittenElsewhere;
    if (o.items) {
        auto scan = [&](const std::string& file, bool trigRows) {
            std::ifstream f(file); std::string line; std::getline(f, line);
            const auto head = tsSplit(tsTrim(line), ','); int cId = -1, cIt = -1, cIt2 = -1;
            for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "id") cId = (int)i; if (head[i] == "item") cIt = (int)i; if (head[i] == "item2") cIt2 = (int)i; }
            while (cId >= 0 && cIt >= 0 && std::getline(f, line)) {
                const auto c = tsSplit(tsTrim(line), ',');
                if ((int)c.size() <= std::max(cId, std::max(cIt, cIt2))) continue;
                const int id = std::atoi(c[(size_t)cId].c_str());
                if (trigRows ? (id == 3619 || id == 3641) : id == 1817) { itemsWrittenElsewhere.insert(std::atoi(c[(size_t)cIt].c_str())); if (cIt2 >= 0 && trigRows) itemsWrittenElsewhere.insert(std::atoi(c[(size_t)cIt2].c_str())); }
            }
        };
        scan(ref + "/triggers.txt", true); scan(ref + "/items.txt", false);
    }
    std::unordered_set<int> cntMultiSet;   // the Count triggers of the level string with multi-activate (o.items)
    if (o.items && !o.levelString.empty()) cntMultiSet = parseCountMulti(lvlObjs(), objrectsFile);
    std::unordered_map<int, SclParams> sclP;   // the Transform triggers of the level string (o.scaleTrig), by uid
    if (o.scaleTrig && !o.levelString.empty()) sclP = parseTransforms(lvlObjs(), objrectsFile);
    {
        std::ifstream f(ref + "/triggers.txt");
        std::string line;
        std::getline(f, line);
        const auto head = tsSplit(tsTrim(line), ',');
        std::map<std::string, int> col;
        for (size_t i = 0; i < head.size(); ++i) col[head[i]] = (int)i;
        auto need = [&](const char* n) { return col.count(n) ? col[n] : -1; };
        const int cUid = need("uid"), cId = need("id"), cCx = need("cx"), cTarget = need("target"), cCenter = need("center"), cTouch = need("touch"), cSpawn = need("spawn"), cDur = need("dur"),
                  cOx = need("ox"), cOy = need("oy"), cEase = need("ease"), cRate = need("erate"), cDeg = need("deg"), cT360 = need("t360"), cChan = need("chan"), cTogon = need("togon"), cActgrp = need("actgrp"),
                  cItem = need("item"), cCount = need("count"), cCmode = need("cmode"), cActg = need("actgrp"), cLx = need("lockx"), cLy = need("locky"), cLcx = need("lockcamx"), cLcy = need("lockcamy"), cLrot = need("lockrot"), cMmx = need("movemodx"), cMmy = need("movemody"),
                  cCy2 = need("cy"), cSdelay = need("sdelay"), cSord = need("sord"), cSordd = need("sordd"), cOrd = need("ord"), cRemap = need("remap"), cResetRemap = need("resetremap"), cSilent = need("silent"),
                  cCtrobj = need("ctrobj"), cMvtgt = need("mvtgt"), cMvaxis = need("mvaxis"), cTmodctr = need("tmodctr"), cDynmode = need("dynmode"), cDirdist = need("dirdist"), cEvents = need("events"), cEvx = need("evextra");
        while (std::getline(f, line)) {
            const auto c = tsSplit(tsTrim(line), ',');
            if ((int)c.size() <= cLcy) continue;
            auto I = [&](int k) { return k >= 0 ? std::atoi(c[(size_t)k].c_str()) : 0; };
            auto D = [&](int k) { return k >= 0 ? std::atof(c[(size_t)k].c_str()) : 0.0; };
            TrigDef t;
            t.uid = I(cUid); t.id = I(cId); t.areaOk = areas.count(t.uid) > 0; t.target = I(cTarget); t.center = I(cCenter); t.chan = I(cChan); t.touch = I(cTouch); t.spawn = I(cSpawn);
            t.togon = cTogon >= 0 ? std::atoi(c[(size_t)cTogon].c_str()) : -1;
            if (t.id == 3619 || t.id == 3641 || t.id == 1817) { if (runEd.count(t.uid) || runPk.count(t.uid) || runPs.count(t.uid)) continue; }   // T57: one the shadow runs is built from trigext.txt below (its `target` is an item, not a group)
            if (t.id == 2067) { const auto sp = sclP.find(t.uid); if (sp != sclP.end() && !sp->second.onlyMove && sp->second.skx == 0.0 && sp->second.sky == 0.0) { t.sclOk = true; t.sclRel = sp->second.relScale; t.sclX = sp->second.sx == 0.0 ? 1.0 : sp->second.sx; t.sclY = sp->second.sy == 0.0 ? 1.0 : sp->second.sy; if (sp->second.divX) t.sclX = 1.0 / t.sclX; if (sp->second.divY) t.sclY = 1.0 / t.sclY; } }
            if (o.pickup && t.id == 1594 && t.touch && cActgrp >= 0) t.togon = I(cActgrp) != 0 ? 1 : 0;
            if (t.id == 1594 && t.touch && cActgrp >= 0) t.actgrpCol = I(cActgrp) != 0 ? 1 : 0;
            { const int cSponly = need("sponly"); if (t.id == 1594 && t.touch && cSponly >= 0 && I(cSponly) == 1) t.spawnOnly = true; }   // the 59th column, m_isSpawnOnly (triggers.hpp TrigRow::sponly)
            if (o.items && (t.id == 1611 || t.id == 1811) && cItem >= 0 && cCount >= 0 && cActg >= 0 && (t.id == 1611 || cCmode >= 0)) { t.cntItem = I(cItem); t.cntCount = I(cCount); t.cntMode = t.id == 1811 ? I(cCmode) : 0; t.cntAct = I(cActg) != 0; t.cntMulti = cntMultiSet.count(t.uid) > 0; t.cntOk = !t.touch && !(haveExt && !noItemCompare ? itemUnknown.count(t.cntItem) > 0 : itemsWrittenElsewhere.count(t.cntItem) > 0); }
            t.ease = I(cEase); t.t360 = I(cT360);
            t.cx = D(cCx); t.cy = D(cCy2); t.dur = D(cDur); t.ox = D(cOx); t.oy = D(cOy); t.erate = D(cRate); t.deg = D(cDeg);
            t.lockPlayer = I(cLx) != 0 || I(cLy) != 0;
            t.lockPX = I(cLx) != 0; t.lockPY = I(cLy) != 0;
            t.modX = D(cMmx) == 0.0 ? 1.0 : (double)(float)D(cMmx); t.modY = D(cMmy) == 0.0 ? 1.0 : (double)(float)D(cMmy);
            t.lockCam = I(cLcx) != 0 || I(cLcy) != 0;
        t.lockCX = I(cLcx) != 0; t.lockCY = I(cLcy) != 0; t.camLockOk = o.camLock;
            t.lockrot = I(cLrot) != 0;
            t.sdelay = D(cSdelay); t.sord = I(cSord) != 0; t.sordd = I(cSordd) != 0; t.ord = I(cOrd); t.silent = I(cSilent) != 0; t.remap = cRemap >= 0 && c[(size_t)cRemap] != "-" && !c[(size_t)cRemap].empty();
            if (t.id == 1268 && cResetRemap >= 0 && cResetRemap < (int)c.size()) {
                const std::string& reset = c[(size_t)cResetRemap];
                if (reset == "0" || reset == "1") t.resetRemap = I(cResetRemap);
            }
            t.ctrCol = cCtrobj >= 0; t.ctrObj = I(cCtrobj);
            if (t.id == 3604 && cEvents >= 0 && cEvx >= 0 && (int)c.size() > std::max(cEvents, cEvx)) {   // an Event trigger: its events (';'-separated, '-' none) and the extra IDs (a:b); run when it listens for something with the extra IDs 0 / 0 (EffectGameObject +0x754 * 10000 + +0x758 is the key)
                const std::string& es = c[(size_t)cEvents];
                if (es != "-") for (size_t p = 0; p < es.size();) { const size_t q = es.find(';', p); t.evList.push_back(std::atoi(es.c_str() + p)); if (q == std::string::npos) break; p = q + 1; }
                int x1 = 0, x2 = 0; std::sscanf(c[(size_t)cEvx].c_str(), "%d:%d", &x1, &x2);
                t.evKey = x1 * 10000 + x2;
                t.evOk = !t.touch && !t.evList.empty() && x1 == 0 && x2 == 0 && t.target != 0;
            }
            t.areaStopOk = t.id == 3024 && haveAreaFx;
            t.editOk = t.id >= 3011 && t.id <= 3015 && editOkUid.count(t.uid) > 0;
            if (haveExt) { const auto xi = extRows.find(t.uid); if (xi != extRows.end()) { t.mvaCol = true; t.mvaObj = xi->second.mva; t.tpCol = true; t.tpPlayer = xi->second.tp1 != 0 || xi->second.tp2 != 0; } }
            if (t.id == 3620 && haveExt) {   // an Item Compare the dump lists (it has a true group): its fields from trigext.txt, run when every value it reads is had (T57: the rows of triggers.txt never got this, only the ones it leaves out)
                const auto xi = extRows.find(t.uid);
                if (xi != extRows.end()) {
                    const ExtRow& r = xi->second;
                    t.i1mode = r.i1mode; t.i2mode = r.i2mode; t.item1 = r.item1; t.item2 = r.item2; t.mod1 = (float)r.mod1; t.mod2 = (float)r.mod2; t.tol = (float)r.tol;
                    t.res1 = r.res1; t.res2 = r.res2; t.res3 = r.res3; t.rnd1 = r.rnd1; t.rnd2 = r.rnd2; t.sgn1 = r.sgn1; t.sgn2 = r.sgn2; t.itemCmp = compareOk.count(t.uid) > 0;
                }
            }
            if (t.id == 1815 && haveExt && collisionBlocksLoaded) {   // blocks A / B and "activate group" from trigext.txt (item1, item2, actgrp)
                const auto xi = extRows.find(t.uid);
                if (xi != extRows.end()) { t.collA = xi->second.item1; t.collB = xi->second.item2; t.collActivate = xi->second.actgrp != 0; t.collOk = true; }
            }
            t.mvtgt = I(cMvtgt) > 0; t.mvaxis = I(cMvaxis); t.tmodctr = I(cTmodctr); t.dynmode = I(cDynmode) > 0; t.dirDist = D(cDirdist);
            if (t.remap) {   // "a:a:b:flag;..." : the group a is b in what this Spawn spawns
                for (const std::string& e : tsSplit(c[(size_t)cRemap], ';')) {
                    const auto q = tsSplit(e, ':');
                    if (q.size() >= 3) t.remapTo.push_back({std::atoi(q[0].c_str()), std::atoi(q[2].c_str())});
                }
            }
            t.remapOk = o.remapChain && t.id == 1268 && t.remap && !t.remapTo.empty();
            if (t.id == 1616) { const auto sm = stopModes.find(t.uid); if (sm != stopModes.end()) { t.stopMode = sm->second.first; t.stopCtrl = sm->second.second; } }
            if (t.id == 3022) continue;   // read from objrects.txt below (with the columns the event reports)
            S.addTrigger(t);
        }
    }
    {   // Time Warp triggers (id 1935) are in the game's trigger array too but not in triggers.txt: objrects.txt has them, with the factor in `tw`
        const auto head = tsSplit(tsTrim(objrectsFile.at(0)), ',');
        std::map<std::string, int> col;
        for (size_t i = 0; i < head.size(); ++i) col[head[i]] = (int)i;
        bool ok = true;
        // the Rotate Gameplay triggers' channel switches (rotgameplay.txt: swarm = m_changeChannel, swch = m_targetChannelID); with the file the 2.2 trigger channels are run
        std::map<int, std::tuple<int, int, int>> rgSw;   // uid -> (m_changeChannel, m_targetChannelID, m_channelOnly)
        if (!noChannels) {
            std::ifstream rf(ref + "/rotgameplay.txt");
            if (rf) {
                std::string rl;
                std::getline(rf, rl);
                const auto rh = tsSplit(tsTrim(rl), ',');
                std::map<std::string, int> rc;
                for (size_t i = 0; i < rh.size(); ++i) rc[rh[i]] = (int)i;
                if (rc.count("uid") && rc.count("swarm") && rc.count("swch") && rc.count("chanOnly")) {
                    S.setChannelModel(true);
                    while (std::getline(rf, rl)) {
                        const auto c = tsSplit(tsTrim(rl), ',');
                        if ((int)c.size() <= rc["swch"] || c[(size_t)rc["swarm"]] == "-") continue;
                        rgSw[std::atoi(c[(size_t)rc["uid"]].c_str())] = std::make_tuple(std::atoi(c[(size_t)rc["swarm"]].c_str()), std::atoi(c[(size_t)rc["swch"]].c_str()), std::atoi(c[(size_t)rc["chanOnly"]].c_str()));
                    }
                }
            }
        }
        std::unordered_set<int> listedUids; for (const TrigDef& tt : S.triggers()) listedUids.insert(tt.uid);
        if (!o.levelString.empty() && haveExt) seqP = parseSequences(lvlObjs(), objrectsFile);
        if (!o.levelString.empty() && haveExt) randP = parseRandoms(lvlObjs(), objrectsFile, "2068");
        if (o.random && !o.levelString.empty()) {   // Random (1912): its chance from the level string; Advanced Random (2068): run below. Not while the level has draws the shadow does not make
            const size_t od = countOtherDraws(lvlObjs());
            if (od) std::printf("random: %zu objects draw from the trigger LCG without being run (Spawn Particle / Spawn delay variation): Random and Advanced Random are not run\n", od);
            else {
                randRun = true; S.rngPinFirst_ = o.randomPin; S.drawOnFollow = true;
                for (const auto& kv : parseRandoms(lvlObjs(), objrectsFile, "1912")) S.markRandom(kv.first, kv.second.chance);
            }
        }   // the Advanced Randoms' group lists: the groups they may start (they are not run)
        size_t nBlockers = 0;
        for (const char* n : {"id", "uid", "cx", "touch", "spawn", "chan", "tw"}) if (!col.count(n)) ok = false;   // 1935 Time Warp (the factor in `tw`) and 1612 / 1613 hide / show the player
        for (size_t li = 1; ok && li < objrectsFile.lines.size(); ++li) {
            const auto c = tsSplit(tsTrim(objrectsFile.lines[li]), ',');
            if ((int)c.size() <= col["tw"]) continue;
            const int oid = std::atoi(c[(size_t)col["id"]].c_str());
            // (3608 Spawn Particle is not an activator: EffectGameObject::triggerObject case 0xe18 moves a particle group and draws random numbers, it spawns no group)
            if (oid == 1912 || oid == 2068 || oid == 3607 || oid == 3609 || oid == 3614 || oid == 3615 || oid == 3620) {   // (3620, an Item Compare, spawns the group of its true or false side: c113256289's loop 28109 -> 28110 -> 28113 runs on it) activators the trigger dump leaves out (they name groups in properties it does not export)
                bool listed = false;
                for (const TrigDef& tt : S.triggers()) if (tt.uid == std::atoi(c[(size_t)col["uid"]].c_str())) { listed = true; break; }
                if (oid == 3620 && compareOk.count(std::atoi(c[(size_t)col["uid"]].c_str()))) listed = true;   // an Item Compare the shadow runs is not an activator it is missing
                if (oid == 3607 && seqP.count(std::atoi(c[(size_t)col["uid"]].c_str()))) { const auto xi = extRows.find(std::atoi(c[(size_t)col["uid"]].c_str())); if (xi != extRows.end() && xi->second.touch == 0) listed = true; }   // a Sequence the shadow runs (the ext loop below adds it)
                if (oid == 2068 && randP.count(std::atoi(c[(size_t)col["uid"]].c_str())) && extRows.count(std::atoi(c[(size_t)col["uid"]].c_str()))) listed = true;   // an Advanced Random whose list the level string gave names the groups it may start (the ext loop below adds it as a trigger the shadow does not run): the level's other spawn-triggered triggers are not un-vouched (a touch-triggered one too: it only names groups)
                if ((oid == 3609 || oid == 3614 || oid == 3615) && extRows.count(std::atoi(c[(size_t)col["uid"]].c_str()))) listed = true;   // an Instant Collision / Timer / Time Event the dump leaves out: its groups come from trigext.txt (target / center) and it is added below as a trigger that names them
                if (!listed) { S.unknownActivators = true; S.unlistedActivators.push_back({std::atoi(c[(size_t)col["uid"]].c_str()), oid}); }   // (81be161f put this statement inside the comment above: from then on no level had unknownActivators)
            }
            // the crossing array holds every trigger object, the ones the trigger dump omits too (Color 899, Static Camera 2923, ...): one that is not reached yet holds the rest of the array back (checkSpawnObjects stops
            // at it), so the omitted ones go in as entries that do nothing (id 0). 2016 and 3642 are objects of the same kind that the record never shows called.
            if (!noBlockers && !haveExt && col.count("type") && col.count("cy") && c[(size_t)col["type"]] == "20" && oid != 1935 && oid != 1612 && oid != 1613 && oid != 2899 && oid != 2900 && oid != 2066 && oid != 3022 && oid != 2016 && oid != 3642
                && std::atoi(c[(size_t)col["touch"]].c_str()) == 0) {   // (a spawn-triggered one too: an ordered Spawn times its group from the leftmost spawn-triggered trigger, an omitted one included)
                const int buid = std::atoi(c[(size_t)col["uid"]].c_str());
                if (!listedUids.count(buid) && firedUids.count(buid)) {
                    TrigDef b; b.id = 0; b.uid = buid; b.cx = std::atof(c[(size_t)col["cx"]].c_str()); b.cy = std::atof(c[(size_t)col["cy"]].c_str()); b.chan = std::atoi(c[(size_t)col["chan"]].c_str()); b.spawn = std::atoi(c[(size_t)col["spawn"]].c_str());
                    S.addTrigger(b); ++nBlockers; continue;
                }
            }
            if (oid != 1935 && oid != 1612 && oid != 1613 && oid != 2899 && oid != 2900 && oid != 2066 && oid != 3022) continue;
            if (oid == 2900 && !S.channelModel()) continue;
            TrigDef t;
            t.id = oid;
            if (oid == 2900) {
                const auto sw = rgSw.find(std::atoi(c[(size_t)col["uid"]].c_str()));
                if (sw != rgSw.end()) { t.rgSwarm = std::get<0>(sw->second); t.rgSwch = std::get<1>(sw->second); t.rgChanOnly = std::get<2>(sw->second) != 0; }
                const int gd = col.count("gnddir") && (int)c.size() > col["gnddir"] ? std::atoi(c[(size_t)col["gnddir"]].c_str()) : 0;
                t.rgDir = gd;
            }
            if (oid == 3022) for (const char* n : {"tpy", "tpg", "tpix", "tpiy", "tpex", "tpey", "tpfm", "tpf", "tpang", "tprmod", "tprmin", "tprmax", "tpsave", "tprx", "tpry", "tpfm", "touch", "spawn"}) if (col.count(n) && (int)c.size() > col[n]) t.raw += std::string(n) + "=" + c[(size_t)col[n]] + " ";
            if (oid == 2066 && col.count("gmod")) { t.gmod = std::atof(c[(size_t)col["gmod"]].c_str()); t.gpl = col.count("gpl") ? std::atoi(c[(size_t)col["gpl"]].c_str()) : 0; }
            if (oid == 2899 && col.count("optp1")) { t.optp1 = std::atoi(c[(size_t)col["optp1"]].c_str()); t.optp2 = col.count("optp2") ? std::atoi(c[(size_t)col["optp2"]].c_str()) : 0; t.optudg = col.count("optudg") ? std::atoi(c[(size_t)col["optudg"]].c_str()) : 0; }
            t.uid = std::atoi(c[(size_t)col["uid"]].c_str());
            t.cx = std::atof(c[(size_t)col["cx"]].c_str());
            t.cy = col.count("cy") ? std::atof(c[(size_t)col["cy"]].c_str()) : 0.0;
            t.touch = std::atoi(c[(size_t)col["touch"]].c_str());
            t.spawn = std::atoi(c[(size_t)col["spawn"]].c_str());
            t.chan = std::atoi(c[(size_t)col["chan"]].c_str());
            t.tw = std::atof(c[(size_t)col["tw"]].c_str());
            S.addTrigger(t);
        }
    }
    if (haveExt && !noBlockers) {   // every trigger object of the level that the shadow has no row for goes into the array as an entry that does nothing (id 0): a crossing one not reached yet holds the rest back, a spawn-triggered one counts in an ordered spawn's leftmost x
        std::unordered_set<int> have; for (const TrigDef& tt : S.triggers()) have.insert(tt.uid);
        size_t nb = 0, nCmp = 0;
        for (const auto& kv : extRows) {
            const ExtRow& r = kv.second;
            const bool namedOnly = r.id == 2068 || r.id == 3609 || r.id == 3614 || r.id == 3615;   // an activator the shadow does not run: only the groups it may start are kept (a touch-triggered one too)
            if (have.count(kv.first) || (r.touch != 0 && !namedOnly)) continue;
            if (r.id == 1935 || r.id == 1612 || r.id == 1613 || r.id == 2899 || r.id == 2900 || r.id == 2066 || r.id == 3022) continue;   // those are read from objrects.txt above
            TrigDef b; b.id = 0; b.uid = kv.first; b.cx = r.cx; b.cy = r.cy; b.chan = r.chan; b.spawn = r.spawn; b.ord = r.ord;
            if (r.id == 3620) {   // an Item Compare: the shadow runs it when everything it reads is had
                b.id = 3620; b.target = r.target; b.center = r.center; b.i1mode = r.i1mode; b.i2mode = r.i2mode; b.item1 = r.item1; b.item2 = r.item2; b.mod1 = (float)r.mod1; b.mod2 = (float)r.mod2; b.tol = (float)r.tol;
                b.res1 = r.res1; b.res2 = r.res2; b.res3 = r.res3; b.rnd1 = r.rnd1; b.rnd2 = r.rnd2; b.sgn1 = r.sgn1; b.sgn2 = r.sgn2; b.itemCmp = compareOk.count(kv.first) > 0;
                ++nCmp;
            }
            if (r.id == 1817 && runPk.count(kv.first)) {   // T57: a Pickup trigger: count +0x740, mode +0x744, override byte +0x749, factor float +0x74c
                b.id = 1817; b.pkItem = r.item1; b.pkCount = (int)r.raw[0]; b.pkMode = (int)r.raw[1]; b.pkOverride = ((r.raw[2] >> 8) & 0xffu) != 0; float fct; std::memcpy(&fct, &r.raw[3], 4); b.pkFactor = fct; b.pkOk = true; b.touch = r.touch;
            }
            if (r.id == 3619 && runEd.count(kv.first)) {   // T57: an Item Edit (its fields are the Item Compare's; `target` is the item written)
                b.id = 3619; b.pkItem = r.target; b.edTmode = r.tmode < 1 ? 1 : r.tmode; b.i1mode = r.i1mode; b.i2mode = r.i2mode; b.item1 = r.item1; b.item2 = r.item2; b.mod1 = (float)r.mod1; b.mod2 = (float)r.mod2; b.tol = (float)r.tol;
                b.res1 = r.res1; b.res2 = r.res2; b.res3 = r.res3; b.rnd1 = r.rnd1; b.rnd2 = r.rnd2; b.sgn1 = r.sgn1; b.sgn2 = r.sgn2; b.edOk = true; b.touch = r.touch;
            }
            if (r.id == 3641 && runPs.count(kv.first)) {   // T57: an Item Persist: flags +0x774 enable, +0x775 all, +0x776 reset
                b.id = 3641; b.pkItem = r.item1; b.psEnable = ((r.raw[13]) & 0xffu) != 0; b.psAll = ((r.raw[13] >> 8) & 0xffu) != 0; b.psReset = ((r.raw[13] >> 16) & 0xffu) != 0; b.psOk = true; b.touch = r.touch;
            }
            if (r.id == 3607) {   // a Sequence whose list the level string gave
                const auto sp = seqP.find(kv.first);
                if (sp != seqP.end() && !sp->second.targets.empty()) {
                    b.id = 3607; b.seqOk = true; b.seqTargets = sp->second.targets; b.seqMode = sp->second.mode; b.seqResetMode = sp->second.resetMode; b.seqMinInt = sp->second.minInt; b.seqReset = sp->second.reset; b.seqUnique = sp->second.unique;
                    ++nSeq;
                }
            }
            if (r.id == 2068) {   // an Advanced Random whose list the level string gave: the groups it may start, not run
                const auto rp = randP.find(kv.first);
                if (rp != randP.end() && !rp->second.targets.empty()) { b.id = 2068; b.seqTargets = rp->second.targets; b.touch = r.touch; b.randOk = randRun; ++nRand; }
            }
            if (r.id == 3609 || r.id == 3614 || r.id == 3615) {   // an Instant Collision (true / false groups = target / center), a Timer or a Time Event (its target group): named, not run
                b.id = r.id; b.target = r.target; b.center = r.center; b.touch = r.touch; ++nNamed;
            }
            if (r.id == 3016 && randRun) { b.id = 3016; b.target = r.target; }   // an Advanced Follow the dump leaves out: kept as the call that makes its draw (TrigShadow::drawOnFollow), run as nothing else
            S.addTrigger(b); ++nb;
        }
        if (nRand || nNamed) std::printf("named activators: %zu Advanced Random (group lists from the level string), %zu Instant Collision / Timer / Time Event (target / center from trigext); none is run\n", nRand, nNamed);
        if (nSeq) std::printf("sequences: %zu Sequence triggers run (their lists from the level string)\n", nSeq);
        std::printf("trigext: %zu trigger objects, %zu without a row in triggers.txt go in as entries that do nothing (%zu Item Compares, %zu of them run)\n", extRows.size(), nb, nCmp, compareOk.size());
        if (!runPk.empty() || !runEd.empty() || !runPs.empty() || !itemUnknown.empty())
            std::printf("items: %zu Pickup, %zu Item Edit, %zu Item Persist run; %zu items are not had (a writer the shadow does not run)\n", runPk.size(), runEd.size(), runPs.size(), itemUnknown.size());
        S.setAttempt(attemptNo);
    }

    return R;
}

}  // namespace gdtrig
}  // namespace dp
