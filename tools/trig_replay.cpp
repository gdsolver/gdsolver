// trig_replay: dp/src/dp/trigshadow.hpp (closed form) against the game's live objects (mod cfg wtrace=1 with grouptrace=1 -> wtrace.csv).
//   trig_replay <flight dir: dump.csv, wtrace.csv> <reference dir: triggers.txt, objgroups.txt> [--bits groupbits_init.txt] [--maxtick N] [--show K] [--tol 0.002]
// The shadow gets the player's x from the dump (the last attempt) and nothing else from the flight: the objects' state at tick 1 (wtrace's first rows, taken as the start), the group
// bits and counters at the start (groupbits_init.txt), the triggers' columns. Each tick it is compared with the game: every object the shadow's triggers alone act on (`covered`),
// position within --tol or 2 float ulps, rotation within 0.01, on/off equal. Reports, per object, the first tick that differs and how, and the sums.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/trig_replay.cpp
#include "dp/trigshadow_load.hpp"
#include "dp/trigevent_orb.hpp"
#include "dp/trigtouch.hpp"

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

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

// GameObjectType values the player's collision can react to (objrects.txt `type`)
static bool touchableType(int type) {
    switch (type) {
        case 0: case 2: case 3: case 4: case 5: case 6: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19:
        case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38:
        case 39: case 41: case 42: case 43: case 44:
            return true;
    }
    return false;
}
// the reason the shadow does not run a trigger, as a short label (trig_uncovered's)
static std::string trigLabel(const dp::gdtrig::TrigDef& t) {
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
    if (argc < 3) { std::printf("usage: trig_replay <flight dir> <reference dir> [--bits file] [--maxtick N] [--show K] [--tol X]\n"); return 2; }
    const std::string fl = argv[1], ref = argv[2];
    std::string bitsFile;
    long long maxTick = 1LL << 60;
    int show = 12;
    double tol = 0.002;
    bool noFlightCheck = false, flightCheckOnly = false;   // --noflightcheck / --flightcheck: the dump, the recording and the trigger record must be one flight
    int dbgUid = -1;
    std::vector<int> probeUids;   // --probe U,U,..: the shadow's state of these objects (also ones the recording does not follow) in --from .. --upto
    int areaPhase = 1;
    bool noArea = false, showHidden = false, noSpawn = false, doubleRotate = false;
    int spawnPhase = 2;
    std::string camTraceFile;   // camtrace.csv (cfg areadump=1): the game's own camera position per tick; without it the camera comes from dump.csv's layer position / zoom, which only agrees while the zoom is still
    std::string trigFireFile;   // trigfire.csv (cfg trigfire=1): when the game called each trigger -- the touch-triggered ones are fired at those steps
    std::string areaTargetsFile;   // areatargets.txt (cfg areadump=1): without it an Area's target-group index is unknown and none is run
    double winW = 569.0;   // the window's width in points as the Area code reads it (areadump: centre - camL = 258.64 at zoom 1.1 -> 569.0): the screen-centred Areas read it
    bool screenAreas = false, noWindow = false;
    long long dbgUpto = 0, dbgFrom = 0;
    std::string winUidFile;      // --winuids FILE --win T0 T1: uids (one per line) compared with the game inside ticks T0..T1, covered or not (a windowed-coverage probe)
    long long winT0 = 1LL << 60, winT1 = -1;
    std::string uncovDumpFile;   // --uncovdump: per object of the level (uid, type, touchable, covered, touch-fed, the labels of the triggers that keep it uncovered)
    std::string spawnFiresFile;   // --spawnfires F: every spawn-triggered trigger the shadow's own spawn chain fired (step uid id), to be set against trigfire.csv
    int spawnFedMode = 0;       // --spawnfed N: 1 = spawn-triggered triggers the shadow cannot decide are fired from trigfire.csv, 2 = all of them (timing check)
    std::string groupToggleFile;   // --grouptoggles F: the Touch triggers' group on / off calls (tick,group,on; the game's call at tick T is the shadow's step T+1)
    bool noChannels = false;   // --nochannels: leave the 2.2 trigger channels out (a trigger on a channel other than 0 is not run)
    std::string multiFile;   // --multi: multi.txt (cfg stopdump=1): every trigger object's m_isMultiTriggered; without it every trigger is read as multi (spawned as often as asked)
    std::string stopDumpFile;   // --stopdump: stopdump.txt (cfg stopdump=1): each Stop's mode and whether it works on a control ID; without it every Stop is read as a plain stop of its group
    std::string covDumpFile, baseCovFile;   // --covdump: write uid, covered, baseline-covered, touch-fed per object; --basecov: a uid list that replaces the baseline-covered rule
    std::string trigExtRoot;                // --trigextroot R: R/<flight dir name>/trigext.txt and areaeffect.txt (cfg trigext=1 stopdump=1) replace what is otherwise chosen from the record and the level string
    bool noCrossRef = false;                // --nocrossref: the crossing loop reads the dump's x of the step, not the x it started with
    bool noItemCompare = false;             // --noitemcompare: leave the Item Compares unvouched for (as before)
    bool noAreaEdit = false;                // --noareaedit: leave the Edit Area triggers unrun (as before)
    bool noUncoveredAreas = false;          // --nouncoveredareas: the objects a non-run Area reaches through its parents / children stay vouched for (as before 11-01 15:00; the default now does not vouch for them)
    bool noAngleArea = false;               // --noanglearea: an Area Move in angle mode is not run (as before)
    bool noRotFloat = false;                // --norotfloat: an object's own rotation accumulates in double (as before)
    bool noNestedRemap = false;             // --nonestedremap: a trigger no Spawn names keeps its own groups (as before)
    bool noRemapMainObj = false;            // --noremapmainobj: a fed Move-To keeps the dump's main objects through a remap (as before)
    bool noSpawnDelayRemap = false;         // --nospawndelayremap: a fed trigger takes the remap of the last Spawn called, not of the one whose call plus delay lands on the step (as before)
    int attemptNo = 1;                      // --attempt N: the attempt number an Item Compare's mode 5 reads
    bool noBlockers = false;                // --noblockers: leave the trigger objects the dump omits out of the crossing array
    bool diffObj = false;                   // --diffobj
    std::string levelStringFile;            // --levelstring F: the level's .lvl, for the Sequence triggers' lists
    std::string selfTouchLogFile;
    std::string gameEventsFile;             // --gameevents F: the game events a leveldp run raised (env EVDUMP, "t,event:uid:type,..." a line per shadow step); the Event triggers (3604) are run and each event is raised at its step
    bool selfTouch = false;               // --selftouch: the Touch-Triggered triggers' calls decided by TouchDecider from the dump's player and the shadow's own trigger places (not the record's)
    std::vector<int> blockerUids;           // --blockers U,U,..
    bool initCalib = false;                // --initcalib: the groups' counters at the start from the shadow's own first step against the game's first row (an object a tick-0 Toggle switched off is not counted twice)
    bool areaScaleFirstOpt = false;        // --areascalefirst: the Area Scale instances run before the Area Moves (see TrigShadow::areaScaleFirst)
    bool areaEditWideOpt = false;          // --areaeditwide: an Edit Area may also change moveDist / scale / offY / relFade and the tint / opacity fields
    bool areaAngleOpt = false;             // --areaangle: the angle shape at any angle (not only 0)
    bool itemsOpt = false;                 // --items: run the Count / Instant Count triggers; the pickups (items.txt) are collected where the player's box meets them (the item counters fed to the shadow)
    bool areaRadialOpt = false;            // --arearadial: run the Areas of direction type 0 (the distance from the centre point) and the `relative` move along the centre -> object line
    bool settingsInertOpt = false;          // --settingsinert: the Settings object 3662 moves nothing
    bool camLockOpt = false;                // --camlock: run the Moves locked to the camera (needs --camtrace or the dump's layer rows for the camera)
bool areaScaleOpt = false;             // --areascale: run the Area Scale triggers (3008) and compare the objects' sizes with wtrace's w / h
    bool scaleTrig = false;                // --scale: run the Transform triggers (2067) that only scale (needs --levelstring)
    bool pickupTrig = false;               // --pickup: run a Toggle Orb (1594) as a click-fed group toggle (with --selftouch and --orbinputs)
    bool orbDelayOpt = false; int sizeDbgUid = -1;
    std::string orbInputsFile; long long orbLag = 1, orbOv = 1; bool orbHeld = false, orbObb = false, orbMode = false; int orbDbgUid = -1; long long orbDbgFrom = 0, orbDbgTo = 0; double orbHalf = 0.0;   // --orbinputs F: the flight's autorun.cfg (input=tick,1 presses); --orblag N: the call row is the press tick + N (1 measured); --orbov N: the box is the one N rows before the call row (1 measured: the orb has already moved the player at the call row)
    bool remapChain = false;             // --remapchain: run a Spawn with a group remap in the shadow's own chain
    bool areaEOut = false;                  // --areaeout: run an Area Move with a separate ease-out (areas.txt eOut = 1)
    bool collision = false;                 // --collision: run the collision blocks and the Collision triggers (collblocks.txt in the reference directory)
    bool noUnknownAct = false;              // --nounknownact: do not let an activator the trigger dump omits unvouch the spawn-triggered triggers (what exporting the Random-class groups could at best recover)
    bool coverOnly = false;                 // --coveronly: stop after the coverage is known (no comparison)
    double mergePx = 0;   // --mergepx D: timeline k reads x - k*D instead of the x of an earlier tick (a smaller difference than a tick: the crossings may still fall in the same tick)
    int mergeV = 0; long long mergeLag = 12;  // --mergesim V,L: V timelines, timeline k run on the x path of the player L*k ticks late (a different x history, the same speed); how many differ in state at each tick
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--bits") bitsFile = argv[++i];
        else if (a == "--maxtick") maxTick = std::atoll(argv[++i]);
        else if (a == "--show") show = std::atoi(argv[++i]);
        else if (a == "--tol") tol = std::atof(argv[++i]);
        else if (a == "--uid") dbgUid = std::atoi(argv[++i]);
        else if (a == "--probe") { const auto ps = split(argv[++i], ','); for (const auto& s : ps) if (!s.empty()) probeUids.push_back(std::atoi(s.c_str())); }
        else if (a == "--areaphase") areaPhase = std::atoi(argv[++i]);
        else if (a == "--noarea") noArea = true;
        else if (a == "--nospawn") noSpawn = true;
        else if (a == "--doublerotate") doubleRotate = true;
        else if (a == "--spawnphase") spawnPhase = std::atoi(argv[++i]);
        else if (a == "--areatargets") areaTargetsFile = argv[++i];
        else if (a == "--camtrace") camTraceFile = argv[++i];
        else if (a == "--trigfire") trigFireFile = argv[++i];
        else if (a == "--winw") winW = std::atof(argv[++i]);
        else if (a == "--screenareas") screenAreas = true;
        else if (a == "--nowindow") noWindow = true;
        else if (a == "--hidden") showHidden = true;
        else if (a == "--upto") dbgUpto = std::atoll(argv[++i]);
        else if (a == "--from") dbgFrom = std::atoll(argv[++i]);
        else if (a == "--covdump") covDumpFile = argv[++i];
        else if (a == "--stopdump") stopDumpFile = argv[++i];
        else if (a == "--multi") multiFile = argv[++i];
        else if (a == "--nochannels") noChannels = true;
        else if (a == "--grouptoggles") groupToggleFile = argv[++i];
        else if (a == "--spawnfed") spawnFedMode = std::atoi(argv[++i]);
        else if (a == "--spawnfires") spawnFiresFile = argv[++i];
        else if (a == "--uncovdump") uncovDumpFile = argv[++i];
        else if (a == "--winuids") winUidFile = argv[++i];
        else if (a == "--win") { winT0 = std::atoll(argv[++i]); winT1 = std::atoll(argv[++i]); }
        else if (a == "--basecov") baseCovFile = argv[++i];
        else if (a == "--coveronly") coverOnly = true;
        else if (a == "--noflightcheck") noFlightCheck = true;
        else if (a == "--flightcheck") flightCheckOnly = true;
        else if (a == "--nounknownact") noUnknownAct = true;
        else if (a == "--noblockers") noBlockers = true;
        else if (a == "--collision") collision = true;
        else if (a == "--areaeout") areaEOut = true;
        else if (a == "--remapchain") remapChain = true;
        else if (a == "--pickup") { pickupTrig = true; orbHeld = orbObb = orbMode = orbDelayOpt = true; }   // the measured Toggle Orb rule (push + contact path, oriented box, the spider's 27 x 27); --orbplain below turns them off again
        else if (a == "--orbplain") { orbHeld = orbObb = orbMode = orbDelayOpt = false; }
        else if (a == "--scale") scaleTrig = true;
        else if (a == "--areascale") areaScaleOpt = true;
    else if (a == "--settingsinert") settingsInertOpt = true;
    else if (a == "--camlock") camLockOpt = true;
        else if (a == "--arearadial") areaRadialOpt = true;
        else if (a == "--items") itemsOpt = true;
        else if (a == "--areaangle") areaAngleOpt = true;
        else if (a == "--areaeditwide") areaEditWideOpt = true;
        else if (a == "--areascalefirst") areaScaleFirstOpt = true;
        else if (a == "--orbinputs") orbInputsFile = argv[++i];
        else if (a == "--orblag") orbLag = std::atoll(argv[++i]);
        else if (a == "--orbhalf") orbHalf = std::atof(argv[++i]);   // the player's half size for a Toggle Orb, times vsize (collisionCheckObjects tests the 27 x 27 object rect: 13.5), 0 = the 30 x 30 touch rect
        else if (a == "--orbdbg") { orbDbgUid = std::atoi(argv[++i]); orbDbgFrom = std::atoll(argv[++i]); orbDbgTo = std::atoll(argv[++i]); }
        else if (a == "--sizeuid") sizeDbgUid = std::atoi(argv[++i]);   // with --from / --upto: the shadow's Area Scale factors of one object next to the game's w / h
        else if (a == "--orbdelay") orbDelayOpt = true;   // the ordered delays of a chain a Toggle Orb starts one step later (c130820269)
        else if (a == "--orbmode") orbMode = true;   // the player's box for a Toggle Orb is the mode's own (the spider's is 27 x 27)
        else if (a == "--orbobb") orbObb = true;   // a rotated Toggle Orb is tested by its oriented box (SAT) instead of the bounding box
        else if (a == "--orbheld") orbHeld = true;   // a click fires a Toggle Orb the player is on, and so does a button already held when the player reaches it (until the button is released)
        else if (a == "--orbov") orbOv = std::atoll(argv[++i]);   // the overlap is tested at the row (press row + orbov - orblag ... ): 0 = the row of the call, 1 = the row before
        else if (a == "--initcalib") initCalib = true;
        else if (a == "--selftouch") selfTouch = true;
        else if (a == "--diffobj") diffObj = true;
        else if (a == "--levelstring") levelStringFile = argv[++i];
        else if (a == "--selftouchlog") selfTouchLogFile = argv[++i];
        else if (a == "--gameevents") gameEventsFile = argv[++i];
        else if (a == "--blockers") { const auto ps = split(argv[++i], ','); for (const auto& s : ps) if (!s.empty()) blockerUids.push_back(std::atoi(s.c_str())); }
        else if (a == "--nocrossref") noCrossRef = true;
        else if (a == "--noitemcompare") noItemCompare = true;
        else if (a == "--noareaedit") noAreaEdit = true;
        else if (a == "--nospawndelayremap") noSpawnDelayRemap = true;
        else if (a == "--noremapmainobj") noRemapMainObj = true;
        else if (a == "--nonestedremap") noNestedRemap = true;
        else if (a == "--norotfloat") noRotFloat = true;
        else if (a == "--nouncoveredareas") noUncoveredAreas = true;
        else if (a == "--noanglearea") noAngleArea = true;
        else if (a == "--attempt") attemptNo = std::atoi(argv[++i]);
        else if (a == "--trigextroot") trigExtRoot = argv[++i];
        else if (a == "--mergepx") mergePx = std::atof(argv[++i]);
        else if (a == "--mergesim") { const auto ms = split(argv[++i], ','); mergeV = std::atoi(ms[0].c_str()); if (ms.size() > 1) mergeLag = std::atoll(ms[1].c_str()); }
    }
    // the player's x per tick (the last attempt of the dump)
    std::map<long long, double> px, py;
    struct CamRow { double x, y, zoom; };
    std::map<long long, CamRow> camRows;
    {
        std::ifstream f(fl + "/dump.csv");
        std::string line;
        std::getline(f, line);
        const auto head = split(trim(line), ',');
        int cAtt = -1, cTick = -1, cX = -1, cY = -1, cCx = -1, cCy = -1, cCz = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "attempt") cAtt = (int)i; if (head[i] == "tick") cTick = (int)i; if (head[i] == "x") cX = (int)i; if (head[i] == "y") cY = (int)i; if (head[i] == "camx") cCx = (int)i; if (head[i] == "camy") cCy = (int)i; if (head[i] == "camscale") cCz = (int)i; }
        std::map<long long, double> cur, curY;
        std::map<long long, CamRow> curCam;
        long long att = -1;
        while (std::getline(f, line)) {
            const auto c = split(trim(line), ',');
            if ((int)c.size() <= cX) continue;
            const long long a = cAtt >= 0 ? std::atoll(c[(size_t)cAtt].c_str()) : 1;
            if (a != att) { att = a; cur.clear(); curY.clear(); curCam.clear(); }
            if (cCx >= 0 && cCy >= 0 && cCz >= 0 && (int)c.size() > std::max(cCx, std::max(cCy, cCz))) curCam[std::atoll(c[(size_t)cTick].c_str())] = {std::atof(c[(size_t)cCx].c_str()), std::atof(c[(size_t)cCy].c_str()), std::atof(c[(size_t)cCz].c_str())};
            cur[std::atoll(c[(size_t)cTick].c_str())] = std::atof(c[(size_t)cX].c_str());
            if (cY >= 0 && (int)c.size() > cY) curY[std::atoll(c[(size_t)cTick].c_str())] = std::atof(c[(size_t)cY].c_str());
        }
        px = cur; py = curY; camRows = curCam;
    }
    {   // dump.csv, wtrace.csv and trigfire.csv must be one flight: the last attempt of each follows the same player. A trigfire row (tick T, px) is the player's x as the dump has it at tick T + 1 (T itself for a call made before the player's update); the recording
        // (wtrace) cannot run longer than the dump. A mix-up (a recording of one flight with a dump of another: c144489282, 3,589 ticks against 3,540) gave a convincing wrong timeline once.
        std::string why;
        const long long lastDump = px.empty() ? 0 : px.rbegin()->first;
        if (px.empty()) why = "dump.csv holds no rows";
        if (why.empty() && !trigFireFile.empty()) {
            std::ifstream tf(trigFireFile);
            std::string line;
            std::getline(tf, line);
            const auto thead = split(trim(line), ',');
            int cPy = -1;   // trigfirey=1: the player's y at the call
            for (size_t i = 0; i < thead.size(); ++i) if (thead[i] == "py") cPy = (int)i;
            std::vector<std::vector<std::string>> rows;
            long long att = -1;
            while (std::getline(tf, line)) {
                const auto c = split(trim(line), ',');
                if (c.size() < 6) continue;
                const long long a = std::atoll(c[0].c_str());
                if (a != att) { att = a; rows.clear(); }
                rows.push_back(c);
            }
            long long lastTf = 0;
            size_t checked = 0, off = 0, checkedY = 0, offY = 0;
            double worst = 0, worstY = 0;
            for (const auto& c : rows) {
                const long long tk = std::atoll(c[1].c_str());
                lastTf = std::max(lastTf, tk);
                const double rowPx = std::atof(c[4].c_str());
                if (rowPx < 0 || checked >= 400) continue;
                const auto it = px.find(tk + 1), it0 = px.find(tk);   // a call made before the player's update of its tick (a few early calls) shows the x of tick T itself
                if (it == px.end()) continue;
                ++checked;
                double d = std::fabs(it->second - rowPx);
                if (it0 != px.end()) d = std::min(d, std::fabs(it0->second - rowPx));
                if (d > 0.01) { ++off; worst = std::max(worst, d); }
                if (cPy >= 0 && (int)c.size() > cPy && py.count(tk + 1)) {   // the same for y: x follows the tick, y tells two flights of one plan apart
                    ++checkedY;
                    double dy = std::fabs(py[tk + 1] - std::atof(c[(size_t)cPy].c_str()));
                    if (py.count(tk)) dy = std::min(dy, std::fabs(py[tk] - std::atof(c[(size_t)cPy].c_str())));
                    if (dy > 0.01) { ++offY; worstY = std::max(worstY, dy); }
                }
            }
            if (lastTf > lastDump + 100) why = "trigfire.csv runs to tick " + std::to_string(lastTf) + " but dump.csv ends at " + std::to_string(lastDump);
            else if (checkedY >= 5 && offY * 10 > checkedY) why = "trigfire.csv is not the flight of dump.csv: " + std::to_string(offY) + " of " + std::to_string(checkedY) + " rows have a py that is not the dump's y one tick later (worst " + std::to_string(worstY) + " px)";
            else if (checked >= 5 && off * 10 > checked) why = "trigfire.csv is not the flight of dump.csv: " + std::to_string(off) + " of " + std::to_string(checked) + " rows have a px that is not the dump's x one tick later (worst " + std::to_string(worst) + " px)";
        }
        if (why.empty()) {   // the recording's last row
            std::ifstream wf(fl + "/wtrace.csv", std::ios::binary);
            if (wf) {
                wf.seekg(0, std::ios::end);
                const std::streamoff sz = wf.tellg();
                const std::streamoff take = std::min<std::streamoff>(sz, 8192);
                wf.seekg(sz - take);
                std::string tail((size_t)take, '\0');
                wf.read(&tail[0], take);
                while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
                const size_t nl = tail.find_last_of('\n');
                const std::string lastRow = nl == std::string::npos ? tail : tail.substr(nl + 1);
                const long long wTick = std::atoll(lastRow.c_str());
                if (wTick > lastDump + 100) why = "wtrace.csv runs to tick " + std::to_string(wTick) + " but dump.csv ends at " + std::to_string(lastDump);
            }
        }
        if (!why.empty() && !noFlightCheck) { std::printf("REFUSED: the flight files do not belong together: %s (--noflightcheck to run anyway)\n", why.c_str()); return 3; }
        if (flightCheckOnly) { std::printf("flight files agree: dump to tick %lld\n", lastDump); return 0; }
    }
    struct CamTrace { double x, y, zoom; };
    std::map<long long, CamTrace> camTrace;
    if (!camTraceFile.empty()) {
        std::ifstream f(camTraceFile);
        std::string line;
        std::getline(f, line);
        long long att = -1;
        std::map<long long, CamTrace> cur;
        while (std::getline(f, line)) {
            const auto c = split(trim(line), ',');
            if (c.size() < 7) continue;   // a row the run cut off
            const long long a = std::atoll(c[0].c_str());
            if (a != att) { att = a; cur.clear(); }
            cur[std::atoll(c[1].c_str())] = {std::atof(c[2].c_str()), std::atof(c[3].c_str()), std::atof(c[4].c_str())};
        }
        camTrace = cur;
    }
    // the game's objects: tick 1's rows are the start; every later row is a change
    struct Row { long long t; double x, y, rot; int on; double w = 0, h = 0; };
    struct WinRes { long long n = 0, first = -1; double maxErr = 0; };
    std::vector<int> winUids;
    std::map<int, WinRes> winRes;
    if (!winUidFile.empty()) { std::ifstream wf(winUidFile); int u; while (wf >> u) winUids.push_back(u); }
    std::unordered_map<int, std::vector<Row>> game;
    std::vector<int> order;
    {
        std::ifstream f(fl + "/wtrace.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            const auto c = split(trim(line), ',');
            if (c.size() < 6) continue;   // a row the run cut off
            const int uid = std::atoi(c[1].c_str());
            auto& v = game[uid];
            if (v.empty()) order.push_back(uid);
            v.push_back({std::atoll(c[0].c_str()), std::atof(c[2].c_str()), std::atof(c[3].c_str()), std::atof(c[4].c_str()), std::atoi(c[5].c_str()), c.size() > 7 ? std::atof(c[6].c_str()) : 0.0, c.size() > 7 ? std::atof(c[7].c_str()) : 0.0});
        }
    }
    std::vector<TrigTracked> trackedObjs;
    std::unordered_map<int, int> firstOn;   // --initcalib: the game's `on` of the first row, kept when the shadow is loaded with every counter at 0
    for (int uid : order) { const Row& r0 = game[uid].front(); firstOn[uid] = r0.on; trackedObjs.push_back({uid, r0.x, r0.y, r0.rot, initCalib ? 1 : r0.on}); }
    TrigLoadOpts lo;
    lo.ref = ref; lo.bitsFile = bitsFile; lo.areaTargetsFile = areaTargetsFile; lo.stopDumpFile = stopDumpFile;
    if (!trigExtRoot.empty()) {
        std::string nm = fl;
        while (!nm.empty() && (nm.back() == '/' || nm.back() == '\\')) nm.pop_back();
        const size_t sl = nm.find_last_of("/\\");
        lo.extDir = trigExtRoot + "/" + (sl == std::string::npos ? nm : nm.substr(sl + 1));
    }
    if (!noBlockers && !trigFireFile.empty()) { std::ifstream tf0(trigFireFile); std::string tl; std::getline(tf0, tl); while (std::getline(tf0, tl)) { const auto tc = split(trim(tl), ','); if (tc.size() >= 7) lo.firedUids.insert(std::atoi(tc[2].c_str())); } }
    lo.areaPhase = areaPhase; lo.spawnPhase = spawnPhase; lo.attemptNo = attemptNo;
    lo.noArea = noArea; lo.noSpawn = noSpawn; lo.doubleRotate = doubleRotate; lo.screenAreas = screenAreas; lo.noWindow = noWindow; lo.noChannels = noChannels;
    lo.noItemCompare = noItemCompare; lo.noAreaEdit = noAreaEdit; lo.noUncoveredAreas = noUncoveredAreas; lo.noAngleArea = noAngleArea; lo.noRotFloat = noRotFloat;
    lo.camLock = camLockOpt; lo.settingsInert = settingsInertOpt; lo.noNestedRemap = noNestedRemap; lo.noRemapMainObj = noRemapMainObj; lo.noSpawnDelayRemap = noSpawnDelayRemap; lo.noBlockers = noBlockers; lo.collision = collision; lo.areaEOut = areaEOut; lo.remapChain = remapChain; lo.pickup = pickupTrig; lo.scaleTrig = scaleTrig; lo.areaScale = areaScaleOpt; lo.areaRadial = areaRadialOpt; lo.items = itemsOpt; lo.areaAngle = areaAngleOpt; lo.areaEditWide = areaEditWideOpt; lo.levelString = levelStringFile;
    TrigShadow S;
    const TrigLoadResult loaded = loadTrigShadow(S, lo, trackedObjs);
    const std::set<int>& untracked = loaded.untracked;
    const bool haveBits = loaded.haveBits;
    std::map<std::pair<long long, int>, int> fedTeleportSpawned;   // (step, uid) -> the record's spawnTriggered of that call
    std::vector<std::pair<long long, int>> fedTeleports;   // (the shadow's step, uid) of every Teleport the game called (trigfire.csv), whoever started it: a Touch trigger's or a Spawn's that the shadow cannot decide itself
    if (!trigFireFile.empty()) {   // the touch-triggered triggers' fires, from the game's own record (attempt = the last one in the file); the shadow's step is the game's tick + 1
        std::ifstream tf(trigFireFile);
        std::string line;
        std::getline(tf, line);
        { const auto hn = split(trim(line), ','); if (std::find(hn.begin(), hn.end(), std::string("via")) != hn.end()) S.setRecordComplete(true); }   // the hooks of the overriding classes were on (a `via` column; trigfirey=1 adds `py,mode` after it)
        long long att = -1;
        std::map<long long, std::vector<int>> cur, spawnCur;
        std::map<long long, double> crossRef;
        std::unordered_set<int> crossingUids; for (const dp::gdtrig::TrigDef& d : S.triggers()) if (!d.touch && !d.spawn) crossingUids.insert(d.uid);
        while (std::getline(tf, line)) {
            const auto c = split(trim(line), ',');
            if (c.size() < 6) continue;
            const long long a = std::atoll(c[0].c_str());
            if (a != att) { att = a; cur.clear(); spawnCur.clear(); crossRef.clear(); }
            cur[std::atoll(c[1].c_str()) + 1].push_back(std::atoi(c[2].c_str()));
            if (c[5] == "1") spawnCur[std::atoll(c[1].c_str()) + 1].push_back(std::atoi(c[2].c_str()));
            if (!noCrossRef && c[5] == "0" && crossingUids.count(std::atoi(c[2].c_str()))) crossRef.emplace(std::atoll(c[1].c_str()) + 1, std::atof(c[4].c_str()));   // the first crossing row of a tick: the player's x the loop started with
        }
        for (const auto& kv : cur) for (int u : kv.second) { for (const dp::gdtrig::TrigDef& d : S.triggers()) if (d.uid == u && d.id == 3022) { fedTeleports.push_back({kv.first, u}); fedTeleportSpawned[{kv.first, u}] = d.spawn; break; } }
        S.setCrossRef(crossRef);
        if (selfTouch) S.setTouchEvents(std::map<long long, std::vector<int>>()); else S.setTouchEvents(cur);   // --selftouch: the touches are decided below from the dump's player and the shadow's own trigger places
        S.setSpawnEvents(spawnCur, spawnFedMode);
    }
    if (!multiFile.empty()) {
        std::ifstream mf(multiFile);
        std::string ml;
        std::getline(mf, ml);
        std::unordered_map<int, bool> mm;
        while (std::getline(mf, ml)) { const auto c = split(trim(ml), ','); if (c.size() >= 2) mm[std::atoi(c[0].c_str())] = c[1] != "0"; }
        S.setMultiMap(mm);
    }
    long long gtogRead = 0;
    if (!groupToggleFile.empty()) {
        std::ifstream gf(groupToggleFile);
        if (!gf) { std::printf("cannot read %s\n", groupToggleFile.c_str()); return 2; }
        std::string gl;
        while (std::getline(gf, gl)) { const auto c = split(trim(gl), ',');  if (c.size() >= 3 && !c[0].empty() && (std::isdigit((unsigned char)c[0][0]) || c[0][0] == '-')) { S.addGroupToggleEvent(std::atoll(c[0].c_str()) + 1, std::atoi(c[1].c_str()), c[2] != "0"); ++gtogRead; } }
    }
    if (noUnknownAct) S.unknownActivators = false;
    if (selfTouch && trigFireFile.empty()) S.setTouchEvents(std::map<long long, std::vector<int>>());
    long long gameEventsRead = 0;
    if (!gameEventsFile.empty()) {   // --gameevents: the shadow runs the Event triggers and is handed the events of the file (a line "t,event:uid:type,...": the step t, each token's first number the GJGameEvent)
        std::ifstream ef(gameEventsFile);
        if (!ef) { std::printf("cannot read %s\n", gameEventsFile.c_str()); return 2; }
        S.setEventsFed();
        std::string el;
        while (std::getline(ef, el)) {
            const auto c = split(trim(el), ',');
            if (c.size() < 2 || c[0].empty() || !std::isdigit((unsigned char)c[0][0])) continue;
            for (size_t k = 1; k < c.size(); ++k) { S.addGameEvent(std::atoll(c[0].c_str()), std::atoi(c[k].c_str())); ++gameEventsRead; }
        }
    }
    S.finish();
    if (areaScaleFirstOpt) S.areaScaleFirst = true;
    if (orbDelayOpt) S.orbDelay = 1.0 / 240.0;
    // --selftouch: the Touch-Triggered triggers' calls are decided here (dp/src/dp/trigtouch.hpp TouchDecider) instead of read from the record: the player's box (the dump's place, 30 x 30 times the size, axis aligned: the
    // game's own touch rect, `ptt: prect`) against each touch trigger's hitbox at the shadow's own place; a rising edge is a call. A jump of the player (a teleport) puts the overlap into the same pass: the call is one tick later.
    dp::gdtouch::TouchDecider touchDec;
    std::vector<int> orbUids; std::set<long long> pressAt; std::set<int> orbDone; std::map<long long, int> btnEv; bool orbConsumed = false, orbArmed = false;   // btnEv: the flight's input events (tick -> 1 press / 0 release)
    struct TouchRect { double cx, cy, w, h, ox, oy; double rot = 0, w0 = 0, h0 = 0; };   // (w0 / h0 are already times the object's scale: scx / scy are folded in below)   // (rot, w0, h0: the object's own rotation and unrotated size, for a rotated Toggle Orb: GD tests a rotated object by its oriented box)
    std::map<int, TouchRect> touchRects;
    std::map<long long, double> pvs; std::set<long long> pSpider, pNoContact, pGround;   // pNoContact: rows of ship / ufo / wave / swing, where playerTouchedRing does not call ringJump itself (a push does)
    struct Pickup { int uid, item; double cx, cy, w, h; bool done = false; };
    std::vector<Pickup> pickups; long long pickupsTaken = 0;
    if (itemsOpt) {   // items.txt: uid,id,cx,cy,w,h,item,points,pickup,toggle,subcount -- the rows with pickup = 1 are the collectibles
        std::ifstream ifs(ref + "/items.txt"); std::string il; std::getline(ifs, il);
        while (std::getline(ifs, il)) { const auto c = split(trim(il), ','); if (c.size() >= 9 && c[8] == "1") pickups.push_back({std::atoi(c[0].c_str()), std::atoi(c[6].c_str()), std::atof(c[2].c_str()), std::atof(c[3].c_str()), std::atof(c[4].c_str()), std::atof(c[5].c_str())}); }
        std::printf("items: %zu pickups\n", pickups.size());
    }
    long long selfTouchCalls = 0, selfTouchJumps = 0;
    std::FILE* selfTouchLog = selfTouchLogFile.empty() ? nullptr : std::fopen(selfTouchLogFile.c_str(), "w");   // --selftouchlog F: "game tick, uid" of every call (the game's own ptt: t=)
    const bool touchBlock = selfTouch || (pickupTrig && !orbInputsFile.empty());   // (--pickup with --orbinputs but without --selftouch: the record still feeds the other touch triggers, only the Toggle Orbs are decided here)
    if (touchBlock) {
        for (const dp::gdtrig::TrigDef& d : S.triggers()) if (d.touch && (selfTouch || (pickupTrig && d.id == 1594))) touchDec.touchUids.insert(d.uid);
        if (pickupTrig)   // a Toggle Orb is decided by a click, not by an overlap
            for (const dp::gdtrig::TrigDef& d : S.triggers()) if (d.touch && d.id == 1594 && d.togon >= 0) orbUids.push_back(d.uid);   // (taken out of the overlap calls once the hitboxes are read, below)
        if (!orbInputsFile.empty()) {   // the flight's autorun.cfg: input=tick,1 is a press
            std::ifstream inf(orbInputsFile); std::string il;
            while (std::getline(inf, il)) { il = trim(il); if (il.rfind("input=", 0) != 0) continue; const auto c = split(il.substr(6), ','); if (c.size() >= 2) { btnEv[std::atoll(c[0].c_str())] = c[1] == "1" ? 1 : 0; if (c[1] == "1") pressAt.insert(std::atoll(c[0].c_str())); } }
        }
        touchDec.onceOnly = true;
        if (!multiFile.empty()) { std::ifstream mf(multiFile); std::string ml; std::getline(mf, ml); while (std::getline(mf, ml)) { const auto c = split(trim(ml), ','); if (c.size() >= 2 && c[1] != "0") touchDec.multi.insert(std::atoi(c[0].c_str())); } }
        {   // the hitboxes (objrects.txt cx, cy, w, h) and the place of the object at the start (the shadow's), to carry the box with it
            std::ifstream rf(ref + "/objrects.txt"); std::string line; std::getline(rf, line);
            const auto head = split(trim(line), ',');
            int cUid = -1, cCx = -1, cCy = -1, cW = -1, cH = -1, cRot = -1, cW0 = -1, cH0 = -1, cSx = -1, cSy = -1;
            for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "scx") cSx = (int)i; if (head[i] == "scy") cSy = (int)i; if (head[i] == "rot") cRot = (int)i; if (head[i] == "w0") cW0 = (int)i; if (head[i] == "h0") cH0 = (int)i; if (head[i] == "uid") cUid = (int)i; if (head[i] == "cx") cCx = (int)i; if (head[i] == "cy") cCy = (int)i; if (head[i] == "w") cW = (int)i; if (head[i] == "h") cH = (int)i; }
            while (cUid >= 0 && cW >= 0 && std::getline(rf, line)) {
                const auto c = split(trim(line), ',');
                if ((int)c.size() <= std::max(std::max(cUid, cCx), std::max(cCy, std::max(cW, cH)))) continue;
                const int uid = std::atoi(c[(size_t)cUid].c_str());
                if (!touchDec.touchUids.count(uid)) continue;
                TouchRect r{std::atof(c[(size_t)cCx].c_str()), std::atof(c[(size_t)cCy].c_str()), std::atof(c[(size_t)cW].c_str()), std::atof(c[(size_t)cH].c_str()), 0.0, 0.0};
                const auto bi = S.byUid.find(uid);
                if (bi != S.byUid.end()) { r.ox = S.objs[bi->second].x; r.oy = S.objs[bi->second].y; }
                if (cRot >= 0 && cW0 >= 0 && cH0 >= 0 && (int)c.size() > std::max(cRot, std::max(cW0, cH0))) { r.rot = std::atof(c[(size_t)cRot].c_str()); r.w0 = std::atof(c[(size_t)cW0].c_str()); r.h0 = std::atof(c[(size_t)cH0].c_str()); if (cSx >= 0 && (int)c.size() > cSx) { const double sx = std::atof(c[(size_t)cSx].c_str()); if (sx > 0.0) r.w0 *= sx; } if (cSy >= 0 && (int)c.size() > cSy) { const double sy = std::atof(c[(size_t)cSy].c_str()); if (sy > 0.0) r.h0 *= sy; } }
                touchRects[uid] = r;
            }
        }
        for (int u : orbUids) touchDec.touchUids.erase(u);
        std::ifstream df(fl + "/dump.csv"); std::string line; std::getline(df, line);
        const auto head = split(trim(line), ',');
        int cAtt = -1, cTick = -1, cVs = -1, cMode = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "attempt") cAtt = (int)i; if (head[i] == "tick") cTick = (int)i; if (head[i] == "vsize") cVs = (int)i; if (head[i] == "mode") cMode = (int)i; }
        long long att = -1;
        while (cVs >= 0 && std::getline(df, line)) {
            const auto c = split(trim(line), ',');
            if ((int)c.size() <= std::max(cVs, cTick)) continue;
            const long long a = cAtt >= 0 ? std::atoll(c[(size_t)cAtt].c_str()) : 1;
            if (a != att) { att = a; pvs.clear(); }
            pvs[std::atoll(c[(size_t)cTick].c_str())] = std::atof(c[(size_t)cVs].c_str());
            if (cMode >= 0 && (int)c.size() > cMode && (c[(size_t)cMode] == "ship" || c[(size_t)cMode] == "ufo" || c[(size_t)cMode] == "wave" || c[(size_t)cMode] == "swing")) pNoContact.insert(std::atoll(c[(size_t)cTick].c_str()));
            if (c.size() > 9 && c[9] == "1") pGround.insert(std::atoll(c[(size_t)cTick].c_str()));   // onGround (dump column 9): a push on the ground is used up by the jump it starts
            if (cMode >= 0 && (int)c.size() > cMode && c[(size_t)cMode] == "spider") pSpider.insert(std::atoll(c[(size_t)cTick].c_str()));   // (collisionCheckObjects tests the spider's 27 x 27 rect; the other modes' 30 x 30: measured with the ccl: line, watchuid)
        }
        std::printf("selftouch: %zu touch triggers (%zu with a hitbox), %zu multi\n", touchDec.touchUids.size(), touchRects.size(), touchDec.multi.size());
    }
    if (initCalib) {   // the first row is already after the triggers of the level's first tick (the game's tick 0): an object the first row says is off is -1 only where the shadow's own first step does not switch it off
        TrigShadow T = S;
        const auto p1 = px.find(1);
        if (p1 != px.end()) {
            const auto q1 = py.find(1);
            T.step(1, p1->second, q1 == py.end() ? 0.0 : q1->second);
            size_t offFix = 0, onFix = 0, same = 0;
            for (const auto& kv : firstOn) {
                const auto bi = S.byUid.find(kv.first);
                if (bi == S.byUid.end() || untracked.count(kv.first)) continue;
                const bool shOn = T.objs[bi->second].on();
                if (shOn && !kv.second) { S.objs.mut(bi->second).counter -= 1; ++offFix; }
                else if (!shOn && kv.second) { S.objs.mut(bi->second).counter += 1; ++onFix; }
                else ++same;
            }
            std::printf("initcalib: the shadow's first step against the game's first row: %zu objects agree, %zu start off (counter -1), %zu the shadow turns off that the game has on (counter +1)\n", same, offFix, onFix);
        }
    }
    for (int bu : blockerUids) {   // --blockers U,U,..: is the trigger run by the shadow, and which activators keep it from being
        const dp::gdtrig::TrigDef* ti = nullptr;
        for (const dp::gdtrig::TrigDef& d : S.triggers()) if (d.uid == bu) { ti = &d; break; }
        if (!ti) { std::printf("blockers: uid %d is not a trigger of the table\n", bu); continue; }
        std::printf("blockers: uid %d id %d target %d spawn %d touch %d modelled %d blocked by:", bu, ti->id, ti->target, (int)ti->spawn, (int)ti->touch, S.modelled(bu) ? 1 : 0);
        for (int b : S.blockersOf(bu)) std::printf(" %d", b);
        std::printf("\n");
    }
    if (!trigFireFile.empty()) { size_t nm = 0; for (const dp::gdtrig::TrigDef& d : S.triggers()) if (S.modelled(d.uid)) ++nm; std::printf("trigfire record: complete %d, triggers run %zu of %zu\n", S.recordComplete() ? 1 : 0, nm, S.triggers().size()); }
    std::set<int> baseSet;
    bool haveBaseSet = false;
    if (!baseCovFile.empty()) {
        std::ifstream bf(baseCovFile);
        if (!bf) { std::printf("cannot read %s\n", baseCovFile.c_str()); return 2; }
        std::string bl;
        while (std::getline(bf, bl)) { const auto c = split(trim(bl), ' '); if (c.size() >= 3 && c[1] == "1" && c[2] == "1") baseSet.insert(std::atoi(c[0].c_str())); }
        haveBaseSet = true;
    }
    if (!covDumpFile.empty()) {
        std::ofstream of(covDumpFile);
        for (size_t i = 0; i < S.objs.size(); ++i) of << S.objs[i].uid << ' ' << (S.coveredObject(i) ? 1 : 0) << ' ' << (S.coveredObject(i) && S.coveredObjectBase(i) ? 1 : 0) << ' ' << (S.touchFedObject(i) ? 1 : 0) << '\n';
    }
    if (!uncovDumpFile.empty()) {
        std::unordered_map<int, std::set<std::string>> why;   // group -> the labels of the triggers the shadow does not run that act on it
        for (const dp::gdtrig::TrigDef& tr : S.triggers()) {
            if (S.ign(tr.id) || S.modelledT(tr)) continue;
            std::string lbl = trigLabel(tr);
            if (tr.spawn) { const std::set<int> bl = S.blockersOf(tr.uid); if (!bl.empty()) { lbl += "<-"; bool first = true; for (int b : bl) { lbl += (first ? "" : "/") + std::to_string(b); first = false; } } }
            why[tr.target].insert(lbl);
            if (tr.center) why[tr.center].insert(lbl);
        }
        // the roots of a trigger: the crossing / touch triggers that start it, found by walking the activators back (a trigger is started by any trigger whose target group it is in).
        // A root is a trigger that is not spawn-triggered; dyn = the chain passes a Count / Collision / Pickup / Touch / Random ... (when it fires is not its position), unr = a spawn-triggered
        // trigger nothing in the dump names (or a cycle)
        // mob = a root whose own groups are the target of a trigger that moves objects (Move / Rotate / Follow / Transform / Area / Keyframe): the root is itself moved, so its x in the dump
        // says nothing about when the player crosses it (a custom level: a Move at x 14685 in a group another Move carries off fires at tick 7630, with the player 6000 units short)
        struct Roots { double mn = 1e18, mx = -1e18; int n = 0, dyn = 0, unr = 0, mob = 0; };
        std::unordered_map<int, std::vector<size_t>> byTarget;   // group -> the triggers that name it (target / centre / sequence list)
        for (size_t k = 0; k < S.triggers().size(); ++k) {
            const dp::gdtrig::TrigDef& y = S.triggers()[k];
            byTarget[y.target].push_back(k);
            if (y.center && y.center != y.target) byTarget[y.center].push_back(k);
            for (const auto& e : y.seqTargets) byTarget[e.first].push_back(k);
        }
        std::unordered_map<int, std::vector<size_t>> byTargetOnly;   // group -> the triggers whose target it is
        for (size_t k = 0; k < S.triggers().size(); ++k) byTargetOnly[S.triggers()[k].target].push_back(k);
        std::unordered_map<int, Roots> rootMemo;
        std::set<int> rootBusy;
        std::function<Roots(const dp::gdtrig::TrigDef&)> rootsOf = [&](const dp::gdtrig::TrigDef& x) -> Roots {
            const auto mi = rootMemo.find(x.uid);
            if (mi != rootMemo.end()) return mi->second;
            Roots r;
            auto add = [&](double cx) { r.mn = std::min(r.mn, cx); r.mx = std::max(r.mx, cx); ++r.n; };
            if (rootBusy.count(x.uid)) { r.unr = 1; return r; }
            rootBusy.insert(x.uid);
            if (x.touch || x.id == 1595 || x.id == 1611 || x.id == 1815 || x.id == 2068 || x.id == 1912 || x.id == 3607 || x.id == 3608 || x.id == 3609 || x.id == 3604) r.dyn = 1;
            if (!x.spawn) {
                add(x.cx);
                static const std::set<int> kMover = {901, 1346, 1347, 2067, 3006, 3007, 3008, 3009, 3010, 3011, 3012, 3013, 3014, 3015, 3016, 3032, 3033};
                const auto bi = S.byUid.find(x.uid);
                if (bi != S.byUid.end())
                    for (int g : S.objs[bi->second].groups) {
                        const auto ti = byTargetOnly.find(g);
                        if (ti == byTargetOnly.end()) continue;
                        for (size_t k : ti->second) { const dp::gdtrig::TrigDef& y = S.triggers()[k]; if (y.uid != x.uid && kMover.count(y.id)) { r.mob = 1; break; } }
                        if (r.mob) break;
                    }
            } else {
                bool any = false;
                const auto bi = S.byUid.find(x.uid);
                if (bi != S.byUid.end()) {
                    std::set<size_t> seen;
                    for (int g : S.objs[bi->second].groups) {
                        const auto ti = byTarget.find(g);
                        if (ti == byTarget.end()) continue;
                        for (size_t k : ti->second) {
                            if (!seen.insert(k).second) continue;
                            const dp::gdtrig::TrigDef& y = S.triggers()[k];
                            if (y.uid == x.uid) continue;
                            static const std::set<int> kAct2 = {1268, 1611, 1811, 1815, 1817, 1912, 2068, 3604, 3607, 3608, 3609, 3614, 3615, 1595, 1594, 1598, 1614, 3640, 3643, 3620};
                            if (!kAct2.count(y.id)) continue;
                            const Roots q = rootsOf(y);
                            any = true;
                            r.mn = std::min(r.mn, q.mn); r.mx = std::max(r.mx, q.mx); r.n += q.n; r.dyn |= q.dyn; r.unr |= q.unr; r.mob |= q.mob;
                            if (y.id != 1268) r.dyn = 1;
                        }
                    }
                }
                if (!any) r.unr = 1;
            }
            rootBusy.erase(x.uid);
            rootMemo[x.uid] = r;
            return r;
        };
        std::unordered_map<int, Roots> whyRoots;   // group -> the roots of the unrun triggers acting on it
        for (const dp::gdtrig::TrigDef& tr : S.triggers()) {
            if (dp::gdtrig::TrigShadow::ignorableTrigger(tr.id) || S.modelledT(tr)) continue;
            const Roots q = rootsOf(tr);
            for (int g : {tr.target, tr.center}) {
                if (!g) continue;
                Roots& w = whyRoots[g];
                w.mn = std::min(w.mn, q.mn); w.mx = std::max(w.mx, q.mx); w.n += q.n; w.dyn |= q.dyn; w.unr |= q.unr; w.mob |= q.mob;
            }
        }
        std::ofstream of(uncovDumpFile);
        std::ifstream rf(ref + "/objrects.txt");
        std::string line;
        std::getline(rf, line);
        const auto head = split(trim(line), ',');
        int cUid = -1, cType = -1;
        for (size_t i = 0; i < head.size(); ++i) { if (head[i] == "uid") cUid = (int)i; if (head[i] == "type") cType = (int)i; }
        while (cUid >= 0 && cType >= 0 && std::getline(rf, line)) {
            const auto c = split(trim(line), ',');
            if ((int)c.size() <= std::max(cUid, cType)) continue;
            const int uid = std::atoi(c[(size_t)cUid].c_str()), type = std::atoi(c[(size_t)cType].c_str());
            int cov = 1, fed = 0;
            std::string labs;
            const auto bi = S.byUid.find(uid);
            if (bi != S.byUid.end()) {
                cov = S.coveredObject(bi->second) ? 1 : 0;
                fed = (cov && S.touchFedObject(bi->second)) ? 1 : 0;
                if (!cov) {
                    std::set<std::string> ls;
                    for (int g : S.objs[bi->second].groups) { const auto w = why.find(g); if (w != why.end()) ls.insert(w->second.begin(), w->second.end()); }
                    if (ls.empty()) { const int ub = S.uncoveredBy(uid); std::string al = "AreaGroupEdited"; if (ub) { const auto ti = S.triggerIndex(ub); if (ti >= 0) al = "AreaVia:" + trigLabel(S.triggers()[(size_t)ti]); } ls.insert(al); }
                    for (const auto& l : ls) labs += (labs.empty() ? "" : "|") + l;
                }
            }
            int acted = 0, tracked = 1;
            if (bi != S.byUid.end()) { acted = S.actedOnByModelled(bi->second) ? 1 : 0; tracked = untracked.count(uid) ? 0 : 1; }
            std::string fr;
            if (fed) { for (const std::string& s : S.fedReasonsOfObject(bi->second)) fr += (fr.empty() ? "" : "|") + s; }
            Roots ro;   // the roots of everything that keeps the object uncovered (columns: n, min cx, max cx, dyn, unr; n = 0: none found)
            if (bi != S.byUid.end() && !cov) {
                for (int g : S.objs[bi->second].groups) { const auto w = whyRoots.find(g); if (w != whyRoots.end()) { ro.mn = std::min(ro.mn, w->second.mn); ro.mx = std::max(ro.mx, w->second.mx); ro.n += w->second.n; ro.dyn |= w->second.dyn; ro.unr |= w->second.unr; ro.mob |= w->second.mob; } }
                if (labs.rfind("AreaVia:", 0) == 0 || labs == "AreaGroupEdited") {
                    const int ub = S.uncoveredBy(uid);
                    const auto ti = ub ? S.triggerIndex(ub) : -1;
                    if (ti >= 0) { const Roots q = rootsOf(S.triggers()[(size_t)ti]); ro.mn = std::min(ro.mn, q.mn); ro.mx = std::max(ro.mx, q.mx); ro.n += q.n; ro.dyn |= q.dyn; ro.unr |= q.unr; ro.mob |= q.mob; }
                    else ro.unr = 1;
                }
            }
            of << uid << ' ' << type << ' ' << (touchableType(type) ? 1 : 0) << ' ' << cov << ' ' << fed << ' ' << (labs.empty() ? "-" : labs) << ' ' << (fr.empty() ? "-" : fr) << ' ' << acted << ' ' << tracked
               << ' ' << ro.n << ' ' << (ro.n ? ro.mn : 0.0) << ' ' << (ro.n ? ro.mx : 0.0) << ' ' << ro.dyn << ' ' << ro.unr << ' ' << ro.mob << '\n';
        }
    }
    if (coverOnly) {
        size_t nc = 0, nb = 0;
        for (size_t i = 0; i < S.objs.size(); ++i) { if (S.coveredObject(i)) ++nc; if (S.coveredObject(i) && S.coveredObjectBase(i)) ++nb; }
        size_t nu = 0, nuT = 0;   // covered objects the game's recording does not follow (they join the world from the static dump and are never compared)
        for (size_t i = 0; i < S.objs.size(); ++i) if (S.coveredObject(i) && untracked.count(S.objs[i].uid)) { ++nu; if (S.touchFedObject(i)) ++nuT; }
        std::printf("objects %zu, covered %zu, baseline-covered %zu, covered but not followed by the recording (never compared) %zu (touch/spawn-record fed %zu)\n", S.objs.size(), nc, nb, nu, nuT);
        return 0;
    }
    // per object: the game's value at tick t = the last row at or before t
    struct Cursor { size_t i = 0; };
    std::unordered_map<int, Cursor> cur;
    size_t nCovered = 0;
    for (size_t i = 0; i < S.objs.size(); ++i) if (S.coveredObject(i)) ++nCovered;
    std::printf("objects %zu, covered %zu (the shadow's triggers alone act on them), triggers in the queue; player x for %zu ticks%s\n", S.objs.size(), nCovered, px.size(), haveBits ? "" : " (no bits file: counters from the first row)");
    struct First { long long t = -1; int kind = 0; double sd = 0, gd = 0; int actor = -1, actorId = 0; };
    std::unordered_map<int, First> firstBad;
    std::unordered_map<int, double> maxErr;   // per object, the largest position error it ever had (on/off wrong = 1e9)
    long long compared = 0, bad = 0, sizeTicks = 0;
    std::set<int> sizeObjs;   // --areascale: the objects an Area Scale factor was in force on (and the object-ticks)
    long long lastTick = 0;
    for (const auto& kv : px) lastTick = std::max(lastTick, kv.first);
    lastTick = std::min(lastTick, maxTick);
    if (mergeV > 0) {
        // V copies of the finished shadow; copy k reads the x of tick t - k*L. Per tick: how many distinct dynamic states (dynHash), the commands in flight, the size of a delta snapshot.
        std::vector<TrigShadow> V(S.objs.empty() ? 0 : (size_t)mergeV, S);
        const auto init = S.objs;   // (a CowPages copy: it shares S's pages until a step writes them)
        std::vector<long long> hist((size_t)mergeV + 1, 0);
        long long ticks = 0, sumDistinct = 0, maxSnap = 0, sumSnap = 0, inflightTicks = 0, sumCmds = 0, maxCmds = 0, areaTicks = 0, sumDistinctNoPlayer = 0;
        double stepMs = 0, hashMs = 0;
        auto setCam = [&](TrigShadow& Z, long long t) {
            const auto ct = camTrace.find(t);
            const auto cr = camRows.find(t);
            if (ct != camTrace.end() && ct->second.zoom > 0) {
                const float z = (float)ct->second.zoom;
                Z.cam.l = ct->second.x; Z.cam.b = ct->second.y; Z.cam.w = (double)((float)winW / z); Z.cam.h = (double)(320.0f / z); Z.cam.ok = true;
            } else if (cr != camRows.end() && cr->second.zoom > 0) {
                const float z = (float)cr->second.zoom;
                Z.cam.l = (double)((float)cr->second.x / z); Z.cam.b = (double)((float)cr->second.y / z); Z.cam.w = (double)((float)winW / z); Z.cam.h = (double)(320.0f / z); Z.cam.ok = true;
            }
        };
        for (long long t = 1; t <= lastTick; ++t) {
            if (px.find(t) == px.end()) continue;
            std::vector<uint64_t> hs;
            const auto t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < mergeV; ++k) {
                long long tk = mergePx != 0 ? t : std::max(1LL, t - (long long)k * mergeLag);
                auto p = px.find(tk);
                while (p == px.end() && tk < t) p = px.find(++tk);
                const auto q = py.find(tk);
                setCam(V[(size_t)k], t);
                V[(size_t)k].step(t, p->second - (mergePx != 0 ? (double)k * mergePx : 0.0), q == py.end() ? 0.0 : q->second);
            }
            const auto t1 = std::chrono::steady_clock::now();
            size_t cm = 0;
            std::vector<uint64_t> hs2;
            for (int k = 0; k < mergeV; ++k) { hs.push_back(V[(size_t)k].dynHash()); hs2.push_back(V[(size_t)k].dynHash(false)); cm += V[(size_t)k].activeCommands(); }
            std::sort(hs2.begin(), hs2.end());
            sumDistinctNoPlayer += (long long)(std::unique(hs2.begin(), hs2.end()) - hs2.begin());
            const auto t2 = std::chrono::steady_clock::now();
            stepMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
            hashMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
            std::sort(hs.begin(), hs.end());
            const long long d = (long long)(std::unique(hs.begin(), hs.end()) - hs.begin());
            ++ticks; sumDistinct += d; ++hist[(size_t)d];
            if (cm) ++inflightTicks;
            if (V[0].areasActive()) ++areaTicks;
            sumCmds += (long long)cm; maxCmds = std::max(maxCmds, (long long)cm);
            if (t % 16 == 0) {   // the delta snapshot of timeline 0: the objects that differ from the load state (48 B each) and the rest
                long long n = 0;
                for (size_t i = 0; i < V[0].objs.size(); ++i) {
                    const ObjState& a = V[0].objs[i]; const ObjState& b = init[i];
                    if (a.x != b.x || a.y != b.y || a.rot != b.rot || a.counter != b.counter || a.ax != b.ax || a.ay != b.ay) ++n;
                }
                const long long sz = n * 48 + (long long)V[0].dynPrivBytes();
                maxSnap = std::max(maxSnap, sz); sumSnap += sz;
            }
        }
        std::printf("mergesim V=%d lag=%lld ticks=%lld: distinct states per tick mean %.3f; ticks by distinct count:", mergeV, mergeLag, ticks, ticks ? (double)sumDistinct / (double)ticks : 0.0);
        for (int d = 1; d <= mergeV; ++d) std::printf(" %d:%lld", d, hist[(size_t)d]);
        std::printf("\nmergesim: ticks with a command in flight in some timeline %lld (%.1f%%), commands in flight mean %.2f max %lld (summed over %d timelines)\n", inflightTicks, ticks ? 100.0 * (double)inflightTicks / (double)ticks : 0.0, ticks ? (double)sumCmds / (double)ticks : 0.0, maxCmds, mergeV);
        std::printf("mergesim: distinct states per tick mean %.3f when the player's previous place is left out of the value\n", ticks ? (double)sumDistinctNoPlayer / (double)ticks : 0.0);
        std::printf("mergesim: ticks with an Area Move in force %lld (%.1f%%)\n", areaTicks, ticks ? 100.0 * (double)areaTicks / (double)ticks : 0.0);
        std::printf("mergesim: delta snapshot of one timeline mean %.1f KB max %.1f KB (every 16th tick); step %.3f ms per timeline-tick, hash %.3f ms per timeline-tick\n", (double)sumSnap / 1024.0 / std::max(1.0, (double)ticks / 16.0), (double)maxSnap / 1024.0, ticks ? stepMs / ((double)ticks * mergeV) : 0.0, ticks ? hashMs / ((double)ticks * mergeV) : 0.0);
        return 0;
    }
    for (long long t = 1; t <= lastTick; ++t) {
        const auto p = px.find(t);
        if (p == px.end()) continue;
        {
            const auto q = py.find(t);
            const auto ct = camTrace.find(t);
            const auto cr = camRows.find(t);
            if (ct != camTrace.end() && ct->second.zoom > 0) {   // the game's own camera position (+0x410), the window as the Area code reads it
                const float z = (float)ct->second.zoom;
                S.cam.l = ct->second.x; S.cam.b = ct->second.y; S.cam.w = (double)((float)winW / z); S.cam.h = (double)(320.0f / z); S.cam.ok = true;
            } else if (cr != camRows.end() && cr->second.zoom > 0) {   // the view in object space: the layer's offset and the window, both over the zoom
                const float z = (float)cr->second.zoom;   // GD keeps the camera position and the size in float
                S.cam.l = (double)((float)cr->second.x / z); S.cam.b = (double)((float)cr->second.y / z); S.cam.w = (double)((float)winW / z); S.cam.h = (double)(320.0f / z); S.cam.ok = true;
            }
            if (touchBlock) {   // this row's overlap of the player's box with the touch triggers; a call is the rising edge (one per touch), made in the pass of the game's tick t - 1 (the shadow's step t), or of tick t after a teleport
                const double pxv = p->second, pyv = q == py.end() ? 0.0 : q->second;
                const auto vv = pvs.find(t);
                const double half = 15.0 * (vv != pvs.end() && vv->second > 0.0 ? vv->second : 1.0), halfTouch = half;
                std::vector<std::pair<int, int>> fired;
                for (const auto& kv : touchRects) {
                    if (!selfTouch) break;
                    if (std::find(orbUids.begin(), orbUids.end(), kv.first) != orbUids.end()) continue;
                    const TouchRect& r = kv.second;
                    double cx = r.cx, cy = r.cy;
                    const auto bi = S.byUid.find(kv.first);
                    if (bi != S.byUid.end()) { cx += S.objs[bi->second].x - r.ox; cy += S.objs[bi->second].y - r.oy; }
                    if (std::fabs(pxv - cx) <= half + r.w * 0.5 && std::fabs(pyv - cy) <= half + r.h * 0.5) fired.push_back({0x14, kv.first});
                }
                // a Toggle Orb (1594, object type 36: playerTouchedRing). Two ways in (PlayerObject::pushButton / ringJump, decompile): (1) the push: pushButton fires the rings the player touches at the click's tick (the press of the
                // input list at the game tick F = t - orbLag; the box is the one before the call row, the orb has already moved the player at the call row); (2) the contact (--orbheld): playerTouchedRing calls ringJump when the player
                // reaches the orb, and ringJump reads the snapshot pushButton took (+0x991 / +0x992), not the button: armed from a push until the release has been seen by an earlier tick, one Toggle Orb per push (+0x994)
                if (!orbUids.empty()) {
                    const long long F = t - orbLag;
                    const bool pushNow = pressAt.count(F) > 0;
                    if (pushNow) orbConsumed = false;
                    bool armed = false;
                    if (orbHeld) { auto it = btnEv.lower_bound(F); armed = it != btnEv.begin() && std::prev(it)->second == 1 && !pGround.count(std::prev(it)->first - 1); }   // (and the push was not on the ground: the click that starts a jump is spent)   // the last event strictly before F is a press
                    for (int u : orbUids) {
                        const auto ri = touchRects.find(u);
                        if (ri == touchRects.end() || (!touchDec.multi.count(u) && orbDone.count(u))) continue;
                        const TouchRect& r = ri->second;
                        double cx = r.cx, cy = r.cy;
                        const auto bi = S.byUid.find(u);
                        if (bi != S.byUid.end()) { cx += S.objs[bi->second].x - r.ox; cy += S.objs[bi->second].y - r.oy; }
                        // the pure producer (dp/trigevent_orb.hpp, a transcription of this rule): the player's place at a row, the orb's place, the push / contact inputs
                        auto playerAt = [&](long long row) {
                            dp::gdtrig::OrbPlayer pl;
                            const auto pp = px.find(row), pq = py.find(row);
                            if (pp == px.end() || pq == py.end()) { pl.valid = false; return pl; }
                            pl.x = pp->second; pl.y = pq->second;
                            const auto vi = pvs.find(row); pl.vsize = vi != pvs.end() && vi->second > 0.0 ? vi->second : 1.0;
                            pl.spider = pSpider.count(row) > 0;
                            return pl;
                        };
                        dp::gdtrig::OrbRect orect; orect.cx = cx; orect.cy = cy; orect.w = r.w; orect.h = r.h; orect.rot = r.rot; orect.w0 = r.w0; orect.h0 = r.h0;
                        dp::gdtrig::OrbInput oin;
                        oin.pushNow = pushNow; oin.armed = armed; oin.consumed = orbConsumed; oin.noContact = pNoContact.count(t) > 0;
                        oin.done = orbDone.count(u) > 0; oin.multi = touchDec.multi.count(u) > 0; oin.contactPath = orbHeld;
                        const dp::gdtrig::OrbCall ocall = dp::gdtrig::orbDecide(orect, playerAt(t - orbOv), playerAt(t), oin, orbObb, orbMode, orbHalf);
                        bool fire = ocall.fire;
                        const bool hPush = ocall.push;
                        const bool hContact = ocall.fire && !ocall.push;                        if (orbDbgUid == u && t >= orbDbgFrom && t <= orbDbgTo) std::printf("  orbdbg t=%lld orb %d at (%.3f, %.3f) push %d armed %d consumed %d done %d hitPush %d hitContact %d\n", t, u, cx, cy, (int)pushNow, (int)armed, (int)orbConsumed, (int)orbDone.count(u), (int)hPush, (int)hContact);
                        if (fire) { orbDone.insert(u); orbConsumed = true; S.addTouchEvent(t, u, hPush); ++selfTouchCalls; if (selfTouchLog) std::fprintf(selfTouchLog, "%lld %d\n", t - 1, u); }
                    }
                }                const std::vector<int> calls = touchDec.step(fired);
                bool jump = false;
                { const auto pp = px.find(t - 1), pq = py.find(t - 1); if (pp != px.end() && pq != py.end() && (std::fabs(pxv - pp->second) > 30.0 || std::fabs(pyv - pq->second) > 40.0)) jump = true; }
                for (int u : calls) { S.addTouchEvent(jump ? t + 1 : t, u); ++selfTouchCalls; if (jump) ++selfTouchJumps; if (selfTouchLog) std::fprintf(selfTouchLog, "%lld %d\n", jump ? t : t - 1, u); }
            }
            if (itemsOpt) {   // a pickup (items.txt) is collected where the player's box first meets its rect: the collision pass of the game's tick t - 1 (the dump's row t), the counter changes at the shadow's step t
                const double pxv2 = p->second, pyv2 = q == py.end() ? 0.0 : q->second;
                const auto vv2 = pvs.find(t);
                const double half2 = 15.0 * (vv2 != pvs.end() && vv2->second > 0.0 ? vv2->second : 1.0);
                for (Pickup& pk : pickups) if (!pk.done && std::fabs(pxv2 - pk.cx) <= half2 + pk.w * 0.5 && std::fabs(pyv2 - pk.cy) <= half2 + pk.h * 0.5) { pk.done = true; ++pickupsTaken; S.addItemEvent(t, pk.item, 1); }
            }
            S.step(t, p->second, q == py.end() ? 0.0 : q->second);
        }
        if (!probeUids.empty() && t <= dbgUpto && t >= dbgFrom)
            for (int u : probeUids) {
                const auto bi = S.byUid.find(u);
                if (bi == S.byUid.end()) { std::printf("  probe t=%lld uid %d: not in the shadow\n", t, u); continue; }
                const ObjState& o = S.objs[bi->second];
                std::printf("  probe t=%lld uid %d: shadow (%.5f, %.5f, rot %.5f, on %d)\n", t, u, o.x, o.y, o.rot, (int)o.on());
            }
        if (dbgUid >= 0 && t <= dbgUpto && t >= dbgFrom) {   // --uid U --upto T: the shadow's and the game's state of one object, tick by tick
            const auto bi = S.byUid.find(dbgUid);
            if (bi != S.byUid.end()) {
                const ObjState& o = S.objs[bi->second];
                auto& v = game[dbgUid];
                Cursor& c = cur[dbgUid];
                while (c.i + 1 < v.size() && v[c.i + 1].t <= t) ++c.i;
                const Row& g = v[c.i];
                std::printf("  [fired %lld, cmds %zu, queue %zu/%zu] ", S.fired, S.activeCommands(), S.queueIndex(), S.queued());
                std::printf("  t=%lld px=%.4f shadow (%.5f, %.5f, rot %.5f, on %d) game (%.5f, %.5f, rot %.5f, on %d)\n", t, p->second, o.x, o.y, o.rot, (int)o.on(), g.x, g.y, g.rot, g.on);
            }
        }
        for (size_t i = 0; i < S.objs.size(); ++i) {
            if (!S.coveredObject(i) || untracked.count(S.objs[i].uid)) continue;
            const ObjState& o = S.objs[i];
            auto& v = game[o.uid];
            Cursor& c = cur[o.uid];
            while (c.i + 1 < v.size() && v[c.i + 1].t <= t) ++c.i;
            const Row& g = v[c.i];
            ++compared;
            if (areaScaleOpt && (o.asx != 1.0f || o.asy != 1.0f)) { ++sizeTicks; sizeObjs.insert(o.uid); }
            if (sizeDbgUid == o.uid && t >= dbgFrom && t <= dbgUpto) std::printf("  sizedbg t=%lld uid %d asx %.7f asy %.7f game w %.6f h %.6f (first row w %.6f h %.6f)\n", t, o.uid, (double)o.asx, (double)o.asy, g.w, g.h, game[o.uid].front().w, game[o.uid].front().h);
            auto ulp = [](double x) { float f = (float)std::fabs(x); return 2.0 * (double)(std::nextafter(f, 1e30f) - f); };
            int kind = 0;
            double sd = 0, gd = 0;
            if (o.on() != (g.on != 0)) { kind = 3; sd = o.on(); gd = g.on; }
            else if (std::fabs(o.x - g.x) > std::max(tol, ulp(g.x)) || std::fabs(o.y - g.y) > std::max(tol, ulp(g.y))) {
                kind = (std::fabs(o.x - g.x) > std::fabs(o.y - g.y)) ? 1 : 2;
                sd = kind == 1 ? o.x : o.y; gd = kind == 1 ? g.x : g.y;
            } else if (areaScaleOpt && sizeObjs.count(o.uid) && game[o.uid].front().w > 0.0 && std::fabs(o.rot - game[o.uid].front().rot) < 0.01 && (std::fabs(std::max(0.0, game[o.uid].front().w * (double)o.asx) - g.w) > 0.01 || std::fabs(std::max(0.0, game[o.uid].front().h * (double)o.asy) - g.h) > 0.01)) {
                kind = 5; sd = std::max(0.0, game[o.uid].front().w * (double)o.asx); gd = g.w;   // the object's size: the first row's (the start) times the Area Scale factors
            } else {   // an angle is the same angle 360 degrees on (the game holds -90 as 270 after a Transform has rebuilt the rotation from the matrix)
                double dr = std::fmod(o.rot - g.rot, 360.0); if (dr > 180.0) dr -= 360.0; else if (dr < -180.0) dr += 360.0;
                if (std::fabs(dr) > 0.01) { kind = 4; sd = o.rot; gd = g.rot; }
            }
            if (kind == 1 || kind == 2) { const double e = std::max(std::fabs(o.x - g.x), std::fabs(o.y - g.y)); double& m = maxErr[o.uid]; if (e > m) m = e; }
            else if (kind == 3) maxErr[o.uid] = 1e9;
            else if (kind == 5) { double& m = maxErr[o.uid]; const double e = std::fabs(sd - gd); if (e > m) m = e; }
            if (kind) {
                ++bad;
                First& f = firstBad[o.uid];
                if (f.t < 0) { f.t = t; f.kind = kind; f.sd = sd; f.gd = gd; f.actor = o.lastTrig; f.actorId = o.lastTrigId; }
            }
        }
        if (t >= winT0 && t <= winT1)   // --winuids / --win: the objects the shadow does not vouch for, compared with the game inside the window only (position and on / off)
            for (int uid : winUids) {
                const auto bi = S.byUid.find(uid);
                const auto gi = game.find(uid);
                if (bi == S.byUid.end() || gi == game.end() || gi->second.empty()) continue;
                const ObjState& o = S.objs[bi->second];
                auto& v = gi->second;
                Cursor& c = cur[uid];
                while (c.i + 1 < v.size() && v[c.i + 1].t <= t) ++c.i;
                const Row& g = v[c.i];
                WinRes& w = winRes[uid];
                ++w.n;
                double e = std::max(std::fabs(o.x - g.x), std::fabs(o.y - g.y));
                if (o.on() != (g.on != 0)) e = 1e9;
                if (e > w.maxErr) w.maxErr = e;
                if (e > tol && w.first < 0) w.first = t;
            }
    }
    // a summary by kind of first difference
    std::map<int, long long> byKind;
    for (const auto& kv : firstBad) ++byKind[kv.second.kind];
    if (scaleTrig) std::printf("scale: %lld place changes of %zu objects made by Transform triggers\n", S.sclMoves, S.sclMoved.size());
    if (itemsOpt) std::printf("items: %lld pickups collected, %lld counter changes applied\n", pickupsTaken, S.itemsFed);
    if (areaScaleOpt) std::printf("areascale: a size factor in force on %zu objects (%lld object-ticks), compared with wtrace's w / h\n", sizeObjs.size(), sizeTicks);
    if (selfTouch) std::printf("selftouch: %lld calls decided (%lld after a teleport)\n", selfTouchCalls, selfTouchJumps);
    if (selfTouchLog) std::fclose(selfTouchLog);
    std::printf("compared %lld object-ticks, %lld differ; objects with a difference %zu of %zu covered\n", compared, bad, firstBad.size(), nCovered);
    if (!winUids.empty()) {   // WINPROBE uid ticks-compared first-tick-over-tol max-error (the window's ticks only)
        size_t ok = 0, badN = 0, nocmp = 0;
        for (int u : winUids) { const auto it = winRes.find(u); if (it == winRes.end() || it->second.n == 0) { ++nocmp; std::printf("WINPROBE %d 0 -1 0\n", u); continue; } if (it->second.first < 0) ++ok; else ++badN; std::printf("WINPROBE %d %lld %lld %.6f\n", u, it->second.n, it->second.first, it->second.maxErr); }
        std::printf("WINSUMMARY window %lld..%lld uids %zu agree %zu differ %zu not-compared %zu\n", winT0, winT1, winUids.size(), ok, badN, nocmp);
    }
    {   // --diffobj: one machine-readable line per object with a difference: DIFFOBJ uid first-tick kind(1 x, 2 y, 3 on/off, 4 rot) max-error(1e9 = on/off) last-acting-trigger-id
        double worst = 0;
        for (const auto& kv : firstBad) { const auto me = maxErr.find(kv.first); const double e = me == maxErr.end() ? 0.0 : me->second; if (e < 1e8 && e > worst) worst = e; if (diffObj) std::printf("DIFFOBJ %d %lld %d %.6f %d\n", kv.first, kv.second.t, kv.second.kind, me == maxErr.end() ? 0.0 : me->second, kv.second.actorId); }
        std::printf("SUMMARY compared %lld bad %lld objdiff %zu covered %zu maxerr %.6f\n", compared, bad, firstBad.size(), nCovered, worst);
    }
    {   // the split the acceptance asks for: objects the shadow vouched for before Area Moves were run, and those that came with them
        size_t baseCov = 0, newCov = 0, baseBad = 0, newBad = 0, baseBig = 0, newBig = 0, baseMid = 0, newMid = 0;
        for (size_t i = 0; i < S.objs.size(); ++i) {
            if (!S.coveredObject(i)) continue;
            const bool base = haveBaseSet ? baseSet.count(S.objs[i].uid) > 0 : S.coveredObjectBase(i);
            (base ? baseCov : newCov)++;
            if (firstBad.count(S.objs[i].uid)) (base ? baseBad : newBad)++;
            const auto me = maxErr.find(S.objs[i].uid);
            if (me != maxErr.end() && me->second > 1.0) (base ? baseBig : newBig)++;
            if (me != maxErr.end() && me->second > 0.01) (base ? baseMid : newMid)++;
        }
        std::printf("split: baseline-covered %zu objects with a difference %zu; newly covered %zu objects with a difference %zu\n", baseCov, baseBad, newCov, newBad);
        std::printf("split-big: of those, objects that were ever more than 1 px off (or on/off wrong): baseline-covered %zu, newly covered %zu\n", baseBig, newBig);
        std::printf("split-mid: objects that were ever more than 0.01 px off (or on/off wrong): baseline-covered %zu, newly covered %zu\n", baseMid, newMid);
        size_t fedCov = 0, selfCov = 0, fedBad = 0, selfBad = 0, fedMid = 0, selfMid = 0, fedBig = 0, selfBig = 0;   // the touch-fed ones are covered given the game's record of when touch triggers were called
        for (size_t i = 0; i < S.objs.size(); ++i) {
            if (!S.coveredObject(i)) continue;
            const bool fed = S.touchFedObject(i);
            (fed ? fedCov : selfCov)++;
            if (firstBad.count(S.objs[i].uid)) (fed ? fedBad : selfBad)++;
            const auto me = maxErr.find(S.objs[i].uid);
            if (me != maxErr.end() && me->second > 0.01) (fed ? fedMid : selfMid)++;
            if (me != maxErr.end() && me->second > 1.0) (fed ? fedBig : selfBig)++;
        }
        std::printf("split-fed: touch-fed covered %zu diff %zu (>0.01px %zu, >1px %zu) ; self-computed covered %zu diff %zu (>0.01px %zu, >1px %zu)\n", fedCov, fedBad, fedMid, fedBig, selfCov, selfBad, selfMid, selfBig);
    }
    const char* names[] = {"", "x", "y", "on/off", "rotation", "size"};
    for (const auto& kv : byKind) std::printf("  first difference is %s: %lld objects\n", names[kv.first], kv.second);
    if (showHidden) for (const auto& e : S.events) std::printf("trigger %d (id %d) fired in step %lld : %s\n", e.uid, e.id, e.step, e.raw.c_str());
    if (!spawnFiresFile.empty()) {
        std::ofstream sf(spawnFiresFile);
        std::unordered_map<int, int> idOf;
        for (const dp::gdtrig::TrigDef& d : S.triggers()) idOf[d.uid] = d.id;
        for (const auto& e : S.spawnFires) sf << e.first << ' ' << e.second << ' ' << idOf[e.second] << '\n';
        for (const auto& e : S.spawnReach) sf << e.first << ' ' << e.second << ' ' << idOf[e.second] << '\n';   // the spawn-triggered triggers the shadow does not run, that its chain reached (the camera triggers)
        for (const dp::gdtrig::TrigDef& d : S.triggers()) if (d.spawn && S.modelledT(d)) sf << "M " << d.uid << ' ' << d.id << ' ' << S.isFedSpawn(d.uid) << '\n';   // the spawn-triggered triggers the shadow runs (and whether the record fires them)
    }
    if (showHidden) std::printf("step-convention: a step is trigfire's tick + 1 (the dump's tick); \"fired in step\" and \"(the game's call) ... in step\" use the same numbering in every version since c383cb3b\n");
    if (showHidden) for (const auto& e : fedTeleports) {
        std::string raw;
        for (const dp::gdtrig::TrigDef& d : S.triggers()) if (d.uid == e.second) { raw = d.raw; break; }
        bool own = false;
        for (const auto& ev : S.events) if (ev.uid == e.second && ev.step == e.first) own = true;
        std::printf("teleport (the game's call) trigger %d (id 3022) in step %lld%s : %skind=%s\n", e.second, e.first, own ? " [the shadow fires it too]" : "", raw.c_str(), fedTeleportSpawned[{e.first, e.second}] ? "spawn" : "crossing-or-touch");
    }
    if (showHidden) for (const auto& h : S.gravChanges) std::printf("gravity %.4f (player %d, trigger %d) in step %lld\n", h.value, h.player, h.uid, h.step);
    if (showHidden) for (const auto& h : S.ctrlChanges) std::printf("player %d controls %s in step %lld\n", h.player + 1, h.disabled ? "disabled" : "enabled", h.step);
    if (showHidden) for (const auto& h : S.hiddenChanges) std::printf("player %s in step %lld\n", h.second ? "hidden" : "shown", h.first);
    std::printf("shadow fired %lld triggers; the queue held a trigger back on %lld steps\n", S.fired, S.queueWaits);
    if (!groupToggleFile.empty()) std::printf("group toggles: %lld read, %lld applied (fed apart from the fired)\n", gtogRead, S.groupToggleFed);
    {   // by the last trigger that acted on the object before it first differed
        std::map<int, std::pair<long long, std::map<int, long long>>> byActor;   // trigger uid -> (objects, by kind)
        std::map<int, int> actorId;
        for (const auto& kv : firstBad) {
            auto& e = byActor[kv.second.actor];
            ++e.first;
            ++e.second[kv.second.kind];
            actorId[kv.second.actor] = kv.second.actorId;
        }
        std::vector<std::pair<long long, int>> top;
        for (const auto& kv : byActor) top.push_back({kv.second.first, kv.first});
        std::sort(top.rbegin(), top.rend());
        {   // the objects nothing the shadow runs ever acted on, yet the game changed: which triggers (any id) name their groups?
            std::map<int, long long> idsNamingThem;
            std::map<int, long long> noTrigger;
            int shown2 = 0;
            for (const auto& kv : firstBad) {
                if (kv.second.actor != -1) continue;
                const ObjState& o = S.objs[S.byUid.at(kv.first)];   // (a shared map is read with at() / find(); `[]` is for the vector tables)
                std::set<int> ids;
                for (const TrigDef& t : S.triggers()) {
                    bool hit = false;
                    for (int g : o.groups) if (t.target == g || t.center == g) hit = true;
                    if (hit) ids.insert(t.id);
                }
                if (ids.empty()) ++noTrigger[kv.second.kind];
                for (int id : ids) ++idsNamingThem[id];
                if (shown2++ < 4) {
                    std::printf("    e.g. uid %d t=%lld %s: groups", kv.first, kv.second.t, names[kv.second.kind]);
                    for (int g : o.groups) std::printf(" %d", g);
                    std::printf("; trigger ids naming them:");
                    for (int id : ids) std::printf(" %d", id);
                    std::printf("\n");
                }
            }
            std::printf("  of the objects no shadow trigger acted on: trigger ids naming their groups (objects):");
            for (const auto& kv : idsNamingThem) std::printf(" %d:%lld", kv.first, kv.second);
            std::printf("; named by none:");
            for (const auto& kv : noTrigger) std::printf(" %s %lld", names[kv.first], kv.second);
            std::printf("\n");
        }
        std::printf("  by the last trigger that acted on the object (uid, object id: objects; kinds):\n");
        for (int k = 0; k < 8 && k < (int)top.size(); ++k) {
            const int a = top[(size_t)k].second;
            std::printf("    trigger %d (id %d): %lld objects;", a, actorId[a], top[(size_t)k].first);
            for (const auto& kk : byActor[a].second) std::printf(" %s %lld", names[kk.first], kk.second);
            std::printf("\n");
        }
    }
    std::vector<std::pair<long long, int>> early;
    for (const auto& kv : firstBad) early.push_back({kv.second.t, kv.first});
    std::sort(early.begin(), early.end());
    for (int k = 0; k < show && k < (int)early.size(); ++k) {
        const First& f = firstBad[early[(size_t)k].second];
        std::printf("  uid %d first differs at t=%lld in %s: shadow %.6f game %.6f\n", early[(size_t)k].second, f.t, names[f.kind], f.sd, f.gd);
    }
    return 0;
}
