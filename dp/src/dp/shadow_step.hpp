#pragma once
// S2 stage 2 (--shadowstep=all, required: the cli refuses a call without it): the player's tick of every state is the self-running shadow's (selfshadow.hpp); the model's tick that this file
// once ran first has left the source. What the shadow does not own is supplied to it from the model's State and tables: the world the moving objects of grouptrace
// place (prepareGroup), the touch boxes, doors and triggers, and every State field the shadow does not write. The shadow's own fields (x, y, vy,
// mode, gravity, size, grounded, rotation, death) go back into the State, and it carries its state in State::sl (shadow_state.hpp).
//
// A model row t is the player after the buttons of step t (the dump's row, the shadow's atButtons(t)), so one tick is the tail of the game's step t (the
// world, PlayerObject::update, the collision pass, the rotation) and the head of step t+1 (the frame head, the clock, the buttons with the edge between this
// tick's input and the state's own (the last tick's), resetTouchedRings).
//
// Supplied from the model's State into the shadow's inputs: the gravity trigger's value in force over the tick (gravModOf: the x-crossing and touch gravity triggers),
// written into +0xb8c before the update; the x-crossing Teleport triggers, run as the shadow's doTeleport at the end of the pass. The layer words come from ShadowLayer
// (shadow_layer.hpp: the band a portal sets, Free Mode, the mirror tween), the flying band from the State where ShadowLayer does not know its height.
// A dual is stepped with its player 2 (selfshadow_dual.hpp, T.ds), also in a two-player level (the ghost's or the replay's buttons for the other body).
// A state the shadow cannot step (shadowAdvanceImpl's `unsupported`: no shadow world, a dual in reverse travel, a turned frame other than 1 / 3 or a frame change, a platformer
// level) ends its branch; no other tick takes it over. Not supplied: spawn- and touch-fired Teleport triggers, Options (controls) triggers, zoom and camera triggers.
//
// The shadow's tick is the only tick there is: the old tick (stepOne / stepBoth) has left the source, and a call that does not pass --shadowstep is refused (cli.hpp).
#include "dp/fixup.hpp"
#include "dp/shadow_state.hpp"
#include "dp/field_check.hpp"
#include "dp/selfshadow_dual.hpp"
#include "dp/trigevent_togstate.hpp"
#include "dp/game_event.hpp"

#ifndef _WIN32
#include <unistd.h>   // getpid (--shadowquietdump's %p); Linux's headers pull it in by themselves, Android's do not
#endif
#include <ctime>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace dp {

// (g_shadowStepModes is declared in constants.hpp)
inline std::string g_shadowSeedFile;            // --shadowseedfile: the game's image at the anchor (a btraw head row) for the first tick of a replay / search
inline thread_local float t_shDt = 0.25f;           // --shadowwarp (removed): this tick's step dt in 1/60 s
inline thread_local double t_shSec = (1.0 / 60.0) / 4.0;     // ...and its step in seconds (the layer clock)
inline std::atomic<uint64_t> g_shadowGroupGen{0};
struct ShModelDeath { long long n = 0, t = 0; double x = 0, y = 0; int mode = 0, mini = 0; };
inline std::map<std::string, ShModelDeath> g_shModelDeaths;
inline std::string g_shObjPath;   // the objects file of the call (for the table above, whose rows are appended to a file when SHSTEP_MODELDEATHS names one)   // SHSTEP_MODELDEATHS=1: the model-only deaths the shadow overrode, by what the old tick killed on   // prepareGroup counts its calls: a thread re-reads the movers when it changes

inline uint32_t shadowModesOf(const char* s) {
    static const char* names[8] = {"cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"};
    uint32_t m = 0;
    std::string a(s);
    size_t p = 0;
    while (p <= a.size()) {
        size_t q = a.find(',', p);
        if (q == std::string::npos) q = a.size();
        const std::string w = a.substr(p, q - p);
        for (int k = 0; k < 8; ++k) if (w == names[k] || w == std::to_string(k)) m |= 1u << k;
        if (w == "all") m = 0xff;
        p = q + 1;
    }
    return m;
}

// Teleport events: the trigger shadow's Teleport fires of a tick (TrigLive::tpFiresAt) and whether its supply is on, behind pointers because TrigLive is declared by a later header (search_dp.hpp installs them,
// as it does the rollout hooks).
inline const std::vector<int>& (*g_tpEventsHook)(long long) = nullptr;
inline bool (*g_tpEventsSupplyOnHook)() = nullptr;
inline bool g_tpEventsSupplyOn() { return g_tpEventsSupplyOnHook && g_tpEventsSupplyOnHook(); }
inline bool (*g_supBoxPlaceHook)(int uid, double& x, double& y, bool& on) = nullptr;   // search_dp.hpp supBoxPlace: where the trigger shadow's supply has the object this tick, and its on / off (false: supply off, or not a supplied uid)
inline bool (*g_tpEventsModelledHook)(int) = nullptr;   // does the trigger shadow run this trigger (TrigShadow::modelled)
// A spawned Teleport the closed form dated to an x (--tptrigspawn (removed)) is the trigger shadow's to fire, by tick, when its supply is on and it runs the trigger: the x of a delay in seconds is only right while the speed never
// changed in between (a custom level: uid 2851, 2.648 s after x 4905, fires at tick 5085 at x 5155 in the game; the closed form puts it at x 5729 = tick 5465, a second and spurious teleport).
template <class T> inline bool tpSpawnedByShadow(const T& tt) { return tt.spawned && g_tpEventsSupplyOn() && g_tpEventsModelledHook && g_tpEventsModelledHook(tt.uid); }
inline const std::vector<int>& (*g_gravEventsHook)(long long) = nullptr;   // the Gravity triggers (2066) the trigger shadow's timeline fired at a tick (TrigLive::gravFiresAt), as the Teleport hook above
struct ShadowFollowStats { std::atomic<long long> checked{0}, planDiff{0}, stateDiff{0}, printed{0}, active{0}, placedObjs{0}, entries{0}, farObjs{0}; };   // --shadowfollow with the old tick run: the shadow side's plan against the old tick's (step.hpp followPassPlan)
inline ShadowFollowStats g_shFollow;
// The last death the shadow's pass called (observation only: the replay's `REPLAY: shadow-cause=` line). why = the line of collide.hpp's GDCOL_DIED / `c.why = __LINE__` that fired (the rule), uid = the object under test
// then (the killer), t = the step's tick. thread_local: a replay runs on the main thread, the search's worker threads each keep their own and nothing reads them.
struct ShadowDeathNote { long long t = -1; int why = 0, uid = -1; };
inline thread_local ShadowDeathNote g_shDeathNote;
struct ShadowStepStats {
    std::atomic<long long> eligible{0}, stepped{0}, seededFile{0}, seededModel{0}, declinedOther{0}, declinedTeleport{0}, declinedMini{0}, declinedInexact{0}, declinedSlope{0}, declinedSeedCheck{0},
        declinedWarp{0}, declinedDual{0}, declinedFrame{0}, declinedRev{0}, declinedPlat{0}, tpBridged{0}, tpEvFired{0}, tpEvMiss{0}, gravEvSeen{0}, gravEvUnmodelled{0}, gravEvNoBox{0}, gravEvApplied{0}, gravEvOrderRisk{0}, deaths{0}, modelOnlyDeaths{0}, shadowOnlyDeaths{0}, modeDiff{0}, flipDiff{0}, sizeDiff{0}, gravBridged{0}, overflow{0}, ambiguous{0}, ns{0},
        supTouchRan{0}, supTouchDiff{0}, supFolPlaced{0}, supFolFar{0}, supFolNonMover{0}, supFolSlope{0}, supFolMoved{0},
        supTpBackRan{0}, supTpBackDiff{0}, supGravRan{0}, supGravDiff{0}, supGateRefused{0}, supTpTouch{0}, supFixDelta{0}, supFixKill{0}, initCalls{0}, initRebuilds{0}, initNs{0}, buildNs{0}, threadBuilds{0}, threadBuildNs{0}, supCtrlRan{0}, supCtrlDiff{0}, seenTpBack{0}, seenGravTouch{0}, seenTpTouch{0}, seenPcRel{0}, seenPcTouch{0}, seenCtrlT{0};   // --shadowsupply: ticks the old step changed that State field (how much a port of it could matter)   // --shadowsupply: ticks markTouched ran again; those whose touch words differ from the model's
    std::atomic<long long> modelOnlyByKind[3] = {};   // the model-only deaths overridden, by the old tick's kind of death (kDeathPhysics / kDeathRule / kDeathPrune)
    std::atomic<long long> dualSeededModel{0}, dualStepped{0};   // --shadowdual
    std::atomic<long long> seedPrevGiven{0}, seedPrevNone{0};    // shadowSeedFromModel: seeds that took State::prevWX / prevWY as PrevX / PrevY, and seeds that had none (print only)
    // The ticks the shadow handed back to the old tick (it is called only for those), counted by the site that declined (shadow_step.hpp line of the `decline(` call) and, for a mode the shadow
    // is not on (or no base), by the state's mode ([8] = no base). Print only (shadowstep: old-tick fallbacks ... and, with SHSTEP_DECLINES=<file>, one line per count appended to the file).
    static constexpr int kDeclineSites = 2400;
    std::atomic<long long> declineSite[kDeclineSites] = {};
    std::atomic<long long> notModeOn[9] = {};
    std::atomic<long long> eligCause[4] = {};   // the eligibility decline (`not eligible`) by cause, the first that applies of dual / frame / reverse / platformer
};
inline ShadowStepStats g_shStats;
// E9 (11-18): the states the shadow cannot step (the branch ends, there is no old tick to hand them to), by kind.
inline std::atomic<long long> g_shUnsup[8];
inline const char* const kShUnsupName[8] = {"nobase", "frame", "revdual", "dualoff", "framebad", "revmismatch", "rotslot", "platform"};
// the touch-started Reverse boxes the shadow tail fired (modifiers.hpp g_revTouchBoxes): a row `revtouch:fires` in SHSTEP_DECLINES
inline std::atomic<long long> g_shRevTouchFires{0};
// The --sametick tap probe (cli.hpp) takes a trial tick through shadowAdvance. That tick is not a tick of the run: what it adds to the census counters (the rows of SHSTEP_DECLINES: ticks,
// stepped, eligcause, notmodeon, site) is moved here for the scope's lifetime, and written as its own kinds ("probe:ticks", "probe:site:N", ...), so a census of the old-tick fallbacks
// does not count a probe's decline as a tick of the replay. Single-threaded by use (the replay walker); the counters it moves are the census's only.
struct ShProbeCensus {
    long long eligible = 0, stepped = 0, probes = 0;
    long long declineSite[ShadowStepStats::kDeclineSites] = {};
    long long notModeOn[9] = {};
    long long eligCause[4] = {};
};
inline ShProbeCensus g_shProbe;
struct ShProbeScope {
    long long eligible, stepped;
    std::vector<long long> site;
    long long notModeOn[9], eligCause[4];
    ShProbeScope() : eligible(g_shStats.eligible.load()), stepped(g_shStats.stepped.load()), site((size_t)ShadowStepStats::kDeclineSites) {
        for (int l = 0; l < ShadowStepStats::kDeclineSites; ++l) site[(size_t)l] = g_shStats.declineSite[l].load();
        for (int m = 0; m < 9; ++m) notModeOn[m] = g_shStats.notModeOn[m].load();
        for (int k = 0; k < 4; ++k) eligCause[k] = g_shStats.eligCause[k].load();
    }
    ~ShProbeScope() {
        auto move = [](std::atomic<long long>& a, long long before, long long& into) { const long long d = a.load() - before; if (d) { into += d; a -= d; } };
        ++g_shProbe.probes;
        move(g_shStats.eligible, eligible, g_shProbe.eligible);
        move(g_shStats.stepped, stepped, g_shProbe.stepped);
        for (int l = 0; l < ShadowStepStats::kDeclineSites; ++l) move(g_shStats.declineSite[l], site[(size_t)l], g_shProbe.declineSite[l]);
        for (int m = 0; m < 9; ++m) move(g_shStats.notModeOn[m], notModeOn[m], g_shProbe.notModeOn[m]);
        for (int k = 0; k < 4; ++k) move(g_shStats.eligCause[k], eligCause[k], g_shProbe.eligCause[k]);
    }
};
// --shadowcolderive: the collision family's State fields against what the body the pass left says (GD's own bytes)
struct ShadowColStats {
    std::atomic<long long> n{0}, snapUidDiff{0}, snapDistDiff{0}, onSlopeVsOn{0}, onSlopeVsWas{0}, onSlopeTicks{0}, slopeTDiff{0}, slopeTRide{0}, groundDiff{0}, printed{0}, ownSnap{0}, ownMiss{0}, seedSlope{0}, seedSlopeMiss{0}, seedForce{0}, seedRing{0}, seedSnap{0};
    std::atomic<long long> slopeTDelta[9] = {};   // derived - old, -4 .. +4 (an index of 9 or more of them is "other" in [8]: see the compare)
};
inline ShadowColStats g_shCol;
// --shadowsdiff: where the old tick's view (c) and the state before (s) differ in what the shadow's decisions read
struct ShadowSDiff {
    std::atomic<long long> n{0}, dual{0}, rev{0}, frame{0}, rotChan{0}, mode{0}, flip{0}, mini{0}, mdead{0}, eligC{0}, eligS{0}, eligDiff{0}, eligCnotS{0}, eligSnotC{0};
};
inline ShadowSDiff g_shSd;
// --shadowsdiff, the other places the shadow reads the old tick: the touch-word base of the gravity box, the seed check (by reason), the rot hold
struct ShadowSDiff2 {
    std::atomic<long long> trigBaseInvalid{0}, trigBaseDiff{0}, seedChecks{0}, seedDecl{0}, seedDead{0}, seedPos{0}, seedMode{0}, seedFlip{0}, seedVy{0}, seed2Decl{0}, seedSkipped{0},
        rotHolds{0}, rotOk{0}, rotFailDual{0}, rotFailFrame{0}, rotFailSlots{0}, rotFailSide{0}, rotFailUp{0}, rotFailLeft{0};
};
inline ShadowSDiff2 g_shSd2;
// SHPLACE_CHECK=1 (shadowPlaceMovers): placements compared with the sweep of every mover, and the movers the sweep would have switched off that the
// incremental pass left on (each is switched off, so the run goes on as the sweep's)
inline std::atomic<long long> g_shPlaceChecks{0}, g_shPlaceCheckDiff{0};

// What every thread's shadow starts from: the level's objects as self_test reads objrects.txt (and forceblocks.txt beside it), which of them the model moves,
// the template image, the portals' Free Mode bytes, and the anchor's seed.
struct ShadowBase {
    bool ok = false, tried = false;
    std::vector<gdcol::GObj> objs;
    std::unordered_map<int, size_t> idx;
    std::vector<uint8_t> mover;
    std::vector<uint8_t> bigSec;   // --shadowbigsecpost: the --bigsection colliders (g_bigSecObjs / g_bigSecDyn), by world index
    std::vector<int> moverUids;
    std::vector<uint8_t> tmpl;
    std::vector<uint8_t> clean;   // collide_tmpl.gen.hpp's image (a classic level's cube on the floor): what a seed from the model's fields starts from
    std::map<int, std::pair<bool, float>> portalFree;   // uid -> (objrects `free`, cy)
    // the seed (--shadowseedfile)
    bool seedHave = false;
    long long seedT = -1;
    std::vector<uint8_t> seedImg;
    float seedX = 0.f;
    bool seedHasSlots = false;
    std::map<size_t, int> seedSlots;
    std::vector<int> seedArr;
    FlatSet<int> seedNow, seedUsed;
    std::string seedHold;
    int seedPress = 0, seedRelease = 0;
    bool lyHave = false, lyFree = false, lyDual = false, ly310 = false, ly312 = false, ly418 = false;
    float ly308 = 0.f, lyW764 = 0.f, lyZoom = 1.f, lyFlip = 0.f, lyMin = 0.f, lyMax = 0.f;
    float lyCeil = 0.f;   // the layer's MAX GAMEPLAY Y at the anchor (+0x37a8; 0 = not said)
    std::string seedSnap2;   // --shadowdual: player 2's head row at the anchor (the seed file's snap2=, btraw who = 2), when the anchor is inside a dual
};
inline ShadowBase g_shBase;

namespace shstep {
inline std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> v;
    std::string cur;
    for (char ch : s) { if (ch == d) { v.push_back(cur); cur.clear(); } else cur += ch; }
    v.push_back(cur);
    return v;
}
inline std::vector<uint8_t> unhex(const std::string& h) {
    std::vector<uint8_t> b;
    auto hv = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back((uint8_t)(hv(h[i]) * 16 + hv(h[i + 1])));
    return b;
}
}  // namespace shstep

// The directory part of a path, with its separator ("" for a bare name).
inline std::string shadowDirOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

// objrects.txt as tools/self_test.cpp reads it (the same columns), from a stream; `fbPath` is the forceblocks.txt to read beside it
// ("" or a missing file: none, as before).
inline bool shadowLoadObjectsFrom(std::istream& f, const std::string& fbPath, std::vector<gdcol::GObj>& objs, std::map<int, std::pair<bool, float>>& portalFree) {
    using gdcol::GObj;
    std::string line;
    // (a CR a text-mode file read has already dropped; the in-process table has LF ends, a binary read of objrects.txt CRLF)
    auto getRow = [&](std::string& s) { if (!std::getline(f, s)) return false; if (!s.empty() && s.back() == '\r') s.pop_back(); return true; };
    getRow(line);
    const auto hdr = shstep::split(line, ',');
    std::map<std::string, size_t> col;
    for (size_t i = 0; i < hdr.size(); ++i) col[hdr[i]] = i;
    auto num = [&](const std::vector<std::string>& v, const char* n, double d = 0) { auto it = col.find(n); return it == col.end() || it->second >= v.size() || v[it->second].empty() ? d : std::stod(v[it->second]); };
    // GD's anti-cheat spike (level_loader.hpp isAnticheatSpikeRow: the id-8 spike at (0,105) PlayLayer creates last, which never collides) is left out, as the
    // model's loader leaves it out: read as a hazard it killed the shadow on the level's first tick.
    std::vector<std::string> rows;
    int maxUid = -1;
    {
        const auto iu = col.find("uid");
        while (getRow(line)) {
            rows.push_back(line);
            if (iu == col.end()) continue;
            const auto v = shstep::split(line, ',');
            if (iu->second < v.size()) maxUid = std::max(maxUid, std::atoi(v[iu->second].c_str()));
        }
    }
    for (const std::string& row : rows) {
        if (isAnticheatSpikeRow(row, maxUid)) continue;
        const auto v = shstep::split(row, ',');
        if (v.size() < 8) continue;
        GObj o;
        o.objectId = (int)num(v, "id");
        o.type = (int)num(v, "type");
        const double cx = num(v, "cx"), cy = num(v, "cy"), w = num(v, "w"), h = num(v, "h");
        o.uid = (int)num(v, "uid", -1);
        o.cx = (float)cx; o.cy = (float)cy; o.cx0 = o.cx; o.cy0 = o.cy; o.prevCx = o.cx; o.prevCy = o.cy;
        if (col.count("posx") && col.count("posy")) { o.secDx = (float)(num(v, "posx", cx) - cx); o.secDy = (float)(num(v, "posy", cy) - cy); }   // --sectionpos
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
        o.claim = num(v, "claim") != 0;   // (a dump that predates the column has none: no ring claims the touch)
        o.touchTrig = num(v, "touch") != 0;
        if (o.objectId == 2900) {   // Rotate Gameplay: what the layer hands the player's rotateGameplay (--shadowrotapply)
            o.rotMv = (int)num(v, "mvdir"); o.rotGnd = (int)num(v, "gnddir");
            o.rotEdit = num(v, "editvel") != 0; o.rotOvr = num(v, "ovrvel") != 0; o.rotDont = num(v, "dontslide") != 0;
            o.rotVmx = (float)num(v, "vmodx"); o.rotVmy = (float)num(v, "vmody");
        }
        o.extCol = num(v, "extcol") != 0;
        o.flipX = num(v, "flipx") != 0;
        o.reverse = num(v, "rev") != 0;
        o.groupCount = (int)num(v, "groups");
        o.rot = (float)num(v, "rot"); o.w0 = (float)num(v, "w0"); o.h0 = (float)num(v, "h0");
        o.sy0 = num(v, "sy0"); o.sy1 = num(v, "sy1");
        o.tpEx = (float)num(v, "tpex"); o.tpEy = (float)num(v, "tpey"); o.tpY = (float)num(v, "tpy");
        o.tpRX = (float)num(v, "tprx"); o.tpRY = (float)num(v, "tpry");
        o.tpGrav = (int)num(v, "tpg"); o.tpKeepX = num(v, "tpix") != 0; o.tpKeepY = num(v, "tpiy") != 0; o.tpSave = num(v, "tpsave") != 0;
        o.tpFm = (int)num(v, "tpfm"); o.tpF = (float)num(v, "tpf");
        o.tpAng = (float)num(v, "tpang"); o.tpRmod = (float)num(v, "tprmod"); o.tpRmin = (float)num(v, "tprmin"); o.tpRmax = (float)num(v, "tprmax");
        portalFree[o.uid] = {num(v, "free") != 0, (float)cy};
        objs.push_back(o);
    }
    {
        std::ifstream fb(fbPath);
        std::string fl;
        std::getline(fb, fl);
        std::map<int, GObj*> byUid;
        for (auto& o : objs) byUid[o.uid] = &o;
        while (std::getline(fb, fl)) {
            const auto v = shstep::split(fl, ',');
            if (v.size() < 17) continue;
            auto it = byUid.find(std::atoi(v[0].c_str()));
            if (it == byUid.end()) continue;
            GObj& o = *it->second;
            o.fbForce = (float)std::atof(v[11].c_str()); o.fbMin = (float)std::atof(v[12].c_str()); o.fbMax = (float)std::atof(v[13].c_str());
            o.fbRelative = std::atoi(v[14].c_str()) != 0; o.fbRange = std::atoi(v[15].c_str()) != 0; o.fbId = std::atoi(v[16].c_str());
        }
    }
    std::map<int, std::pair<float, float>> ratio;
    for (auto& o : objs) {
        if (o.w0 <= 0 || o.h0 <= 0) continue;
        if (std::fmod(std::fabs(o.rot), 90.0f) == 0.0f) {
            const bool odd = std::fmod(std::fabs(o.rot), 180.0f) != 0.0f;
            ratio[o.objectId] = {(odd ? o.rh : o.rw) / (o.w0 * o.scaleX), (odd ? o.rw : o.rh) / (o.h0 * o.scaleY)};
        }
    }
    for (auto& o : objs) {
        if (!o.oriented || std::fmod(std::fabs(o.rot), 90.0f) == 0.0f) continue;
        // --shadowgcfirst: only where the rect fixes the split badly: W = w|cos| + h|sin|, H = w|sin| + h|cos| has the determinant cos(2 a), which is 0 at 45 degrees (a turned 45 +- ~7 degrees)
        const bool illCond = std::fabs(std::cos(2.0f * o.rot * 0.017453292f)) < 0.25f;
        const bool gcHaz = o.type == 2;   // --shadowgchazard: a hazard does not pulse, its corners are its box whatever the angle
        if ((illCond || gcHaz) && g_obb.count(o.uid)) {   // the game's own corners (obb.txt) before the size taken from an unturned object of the same id
            const auto ob = g_obb.find(o.uid);
            o.hasGc = true;
            for (int q = 0; q < 8; ++q) o.gc[q] = ob->second.g[q];
            o.gcx = o.cx; o.gcy = o.cy;
            o.bw = std::hypot(o.gc[2] - o.gc[0], o.gc[3] - o.gc[1]);   // (the box's own width and height, for a mover that stands at another angle)
            o.bh = std::hypot(o.gc[6] - o.gc[0], o.gc[7] - o.gc[1]);
            continue;
        }
        auto it = ratio.find(o.objectId);
        if (it != ratio.end()) { o.bw = it->second.first * o.w0 * o.scaleX; o.bh = it->second.second * o.h0 * o.scaleY; }
        else {   // no unturned object of this id to take the size from: the game's own corners (--obb), if it dumped them
            const auto ob = g_obb.find(o.uid);
            if (ob != g_obb.end()) {
                o.hasGc = true;
                for (int q = 0; q < 8; ++q) o.gc[q] = ob->second.g[q];
                o.gcx = o.cx; o.gcy = o.cy;
            }
        }
    }
    return true;
}

// The seed file (tools/s2_stage1.py --shadowstep writes it): tick=T, snap=<btraw's sixth column of the head row>, press=0|1, release=0|1 (the button-1 edges of
// step T), layer=free,dual,f310,f312,c308,w764,zoom,f418,flipAmt,min,max,ceil (ctraw's layer words at T).
inline void shadowReadSeed(ShadowBase& B, const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "tick") B.seedT = std::atoll(v.c_str());
        else if (k == "press") B.seedPress = std::atoi(v.c_str());
        else if (k == "release") B.seedRelease = std::atoi(v.c_str());
        else if (k == "snap2") B.seedSnap2 = v;
        else if (k == "snap") {
            const auto parts = shstep::split(v, ';');
            if (parts.size() < 5) continue;
            B.seedImg = shstep::unhex(parts[0]);
            { std::istringstream is(parts[1]); std::string tok; while (is >> tok) { const auto fs = shstep::split(tok, ':'); if (fs.size() >= 4) B.seedArr.push_back(std::atoi(fs[0].c_str())); } }
            { std::istringstream is(parts[2]); int u; while (is >> u) B.seedUsed.insert(u); }
            { std::istringstream is(parts[3]); int u; while (is >> u) B.seedNow.insert(u); }
            B.seedHold = parts[4];
            if (parts.size() > 5) {
                B.seedHasSlots = true;
                std::istringstream is(parts[5]); std::string tok;
                while (is >> tok) { const auto fs = shstep::split(tok, ':'); if (fs.size() == 2) { const int u = std::atoi(fs[1].c_str()); if (u >= 0) B.seedSlots[(size_t)std::strtoull(fs[0].c_str(), nullptr, 16)] = u; } }
            }
        } else if (k == "layer") {
            const auto p = shstep::split(v, ',');
            if (p.size() >= 11) {
                B.lyHave = true;
                B.lyFree = std::atoi(p[0].c_str()) != 0; B.lyDual = std::atoi(p[1].c_str()) != 0; B.ly310 = std::atoi(p[2].c_str()) != 0; B.ly312 = std::atoi(p[3].c_str()) != 0;
                B.ly308 = (float)std::atof(p[4].c_str()); B.lyW764 = (float)std::atof(p[5].c_str()); B.lyZoom = (float)std::atof(p[6].c_str()); B.ly418 = std::atoi(p[7].c_str()) != 0;
                B.lyFlip = (float)std::atof(p[8].c_str()); B.lyMin = (float)std::atof(p[9].c_str()); B.lyMax = (float)std::atof(p[10].c_str());
                if (p.size() >= 12) B.lyCeil = (float)std::atof(p[11].c_str());
            }
        }
    }
    B.seedHave = B.seedT >= 0 && B.seedImg.size() >= 0xc80;
    if (B.seedHave) std::memcpy(&B.seedX, B.seedImg.data() + gdcol::po::PosX, 4);
}

// What g_shBase was built from (shadowStepInit): a call whose key is the same keeps it, any other builds it again.
inline std::string g_shBaseKey;
inline uint64_t g_shBaseGen = 0;   // bumped whenever g_shBase is built: a thread's ShadowThread is built again (shadowThread)
inline thread_local std::vector<int> t_evPreFire;   // the body's fired-ring record as the pass started (newest first): what eventsOfPass compares the record after the pass with
inline uint64_t g_shCallGen = 0;   // bumped by every call's shadowStepInit: a thread's tail memo does not outlive the call that saved it

// With every call's loaded level (its moving objects are the ones the model places from grouptrace). The CLI makes one call per process, or a
// ladder of calls on one level, and the key below is the same for each: the base is built once, as before. The mod makes many calls in one
// process (dpcore.cpp solveInProcess) -- another level in a one-session run, a level slice's copy, a new recording that adds movers -- so the
// base is built again whenever what it is made of changes, and every thread's shadow with it. In-process argv[1] is a placeholder: the objects
// come from the level table the model loads (g_levelCsv), and forceblocks.txt from the path the mod names with --forceids.
inline void shadowStepInit(const char* objrectsPath, const Level& L) {
    if (g_shGpUid.empty()) {
        for (const Obj& o : L.portals) if (o.gpBit >= 0) g_shGpUid.push_back({(int)o.gpBit, o.uid});
        for (const Obj& o : L.dyn.objs) if (o.gpBit >= 0) g_shGpUid.push_back({(int)o.gpBit, o.uid});
    }
    const auto initT0 = std::chrono::steady_clock::now();
    struct InitTimer { std::chrono::steady_clock::time_point t0; ~InitTimer() { ++g_shStats.initCalls; g_shStats.initNs += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(); } } initTimer{initT0};
    ++g_shCallGen;
    const bool inproc = !g_levelCsv.empty();
    // The force blocks' parameters (force, min / max, relative, range, force id): the file the model reads (--forceids, g_forceIdsPath) when it was given, else the
    // file beside the objrects file. The official levels' dumps are named forceblocks_lv<N>.txt (data/), so the plain forceblocks.txt beside objrects_lv22.txt was never
    // found: a force circle (id 3645, range 1) pushed nothing in the shadow and the ball at lv22 t=2142 fell at -0.129 a tick where GD, and the old model, fall at
    // -0.296 (gravity -0.129 and the circle's downward push -0.167). So the sibling is the one the objrects file's own name points at: objrects_lv22.txt ->
    // forceblocks_lv22.txt (quick_regress, fidelity_diff and the other readers of data/ give no --forceids).
    std::string fbPath = g_forceIdsPath;
    if (!inproc && fbPath.empty()) {
        const std::string p = objrectsPath;
        const size_t slash = p.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
        fbPath = shadowDirOf(objrectsPath) + (base.compare(0, 8, "objrects") == 0 ? "forceblocks" + base.substr(8) : std::string("forceblocks.txt"));
    }
    // the movers, in the order they are taken below (before the objects' own filter, which the key does not need)
    std::vector<int> cand;
    for (const Obj& o : L.dyn.objs) cand.push_back(o.uid);
    std::vector<int> candBox;
    for (const ArmBox& b : g_armBoxes) if (!b.live.empty() || b.sup) candBox.push_back(b.uid);
    candBox.push_back(-1);
    for (const FlipHeadBox& b : g_flipHeadBoxes) if (!b.live.empty() || b.sup) candBox.push_back(b.uid);
    for (const FlipHeadBox& b : g_nojumpBoxes) if (!b.live.empty() || b.sup) candBox.push_back(b.uid);
    candBox.push_back(-1);
    for (const SlideBox& b : g_slideBoxes) if (!b.live.empty() || b.sup) candBox.push_back(b.uid);
    candBox.push_back(-1);
    if (g_forceBoxOn) for (const ForceBox& b : g_forceBoxes) if (!b.onT.empty() || !b.live.empty()) candBox.push_back(b.uid);
    candBox.push_back(-1);
    for (const PressRing& r : g_pressRings) if (r.shadowMover()) candBox.push_back(r.uid);
    std::string key;
    {
        uint64_t h = 1469598103934665603ULL;   // FNV-1a
        auto mix = [&](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; } };
        // A file the base reads goes in by its bytes, not its name: the mod names the same DATA_DIR/forceblocks.txt for every call and writes it again
        // between calls, and a seed file can keep its name too, so a key of paths kept a base read from the old content (AUD-745). Its size goes first
        // (-1: no file), so two files never run together. The files are small (forceblocks, the seed) or read once per CLI process (objrects).
        auto mixFile = [&](const std::string& path) {
            std::ifstream f(path, std::ios::binary);
            const std::string b = f ? std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()) : std::string();
            const long long n = f.is_open() ? (long long)b.size() : -1;
            mix(&n, sizeof n);
            mix(b.data(), b.size());
        };
        if (inproc) mix(g_levelCsv.data(), g_levelCsv.size());
        else mixFile(objrectsPath);
        mixFile(fbPath);
        mixFile(g_shadowSeedFile);
        // ...and what shadowThread copies into a thread's shadow when it builds it (levelsettings' dynamicLevelHeight / platformerMode, --levelflags'
        // p22): one level's settings file rewritten under the same name, or a call with another --hsp22, builds the threads again
        const int settings[4] = {g_dynamicLevelHeight, g_platformerMode, g_hsP22, g_enablePlayerSqueeze};   // (enablePlayerSqueeze: the template image's +0xc48 below)
        mix(settings, sizeof settings);
        mix(cand.data(), cand.size() * sizeof(int));
        mix(candBox.data(), candBox.size() * sizeof(int));
        key = (inproc ? std::string("csv:") + std::to_string(g_levelCsv.size()) : std::string("file:") + objrectsPath) + "|fb:" + fbPath + "|seed:" + g_shadowSeedFile
              + "|h:" + std::to_string(h);
    }
    if (g_shBase.tried && key == g_shBaseKey) return;
    ++g_shStats.initRebuilds;
    const auto buildT0 = std::chrono::steady_clock::now();
    struct BuildTimer { std::chrono::steady_clock::time_point t0; ~BuildTimer() { g_shStats.buildNs += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(); } } buildTimer{buildT0};
    g_shBase = ShadowBase{};
    g_shBaseKey = key;
    ++g_shBaseGen;
    ShadowBase& B = g_shBase;
    B.tried = true;
    g_shObjPath = objrectsPath;
    bool read = false;
    if (inproc) { std::istringstream in(g_levelCsv); read = shadowLoadObjectsFrom(in, fbPath, B.objs, B.portalFree); }
    else {   // (the CLI: the objects file as given, the force blocks' file chosen above -- shadowLoadObjects would take forceblocks.txt beside it and miss forceblocks_lv<N>.txt)
        std::ifstream f(objrectsPath);
        read = static_cast<bool>(f) && shadowLoadObjectsFrom(f, fbPath, B.objs, B.portalFree);
    }
    if (!read) { std::printf("shadowstep: cannot read %s: the shadow is off\n", inproc ? "the in-process level table" : objrectsPath); return; }
    for (size_t i = 0; i < B.objs.size(); ++i) B.idx[B.objs[i].uid] = i;
    B.mover.assign(B.objs.size(), 0);
    B.bigSec.assign(B.objs.size(), 0);
    for (const auto& m : g_bigSecObjs) { auto it = B.idx.find(m.first); if (it != B.idx.end()) B.bigSec[it->second] = 1; }
    for (const auto& m : g_bigSecDyn) { auto it = B.idx.find(m.first); if (it != B.idx.end()) B.bigSec[it->second] = 1; }
    for (const Obj& o : L.dyn.objs) {
        auto it = B.idx.find(o.uid);
        if (it == B.idx.end() || B.mover[it->second]) continue;
        B.mover[it->second] = 1;
        B.moverUids.push_back(o.uid);
    }
    // the arm boxes the model reads from the recording (modifiers.hpp) move with their group too, and are not in L.dyn: the shadow places them from their rows (shadowPlaceMovers)
    auto addBox = [&](int uid, size_t nLive, bool sup = false) {   // (sup: a trigger moves it, so with no rows the supply places it, placeBox)
        auto it = B.idx.find(uid);
        if ((nLive == 0 && !sup) || it == B.idx.end() || B.mover[it->second]) return;
        B.mover[it->second] = 1;
        B.moverUids.push_back(uid);
    };
    for (const ArmBox& b : g_armBoxes) addBox(b.uid, b.live.size(), b.sup);
    for (const FlipHeadBox& b : g_flipHeadBoxes) addBox(b.uid, b.live.size(), b.sup);
    for (const FlipHeadBox& b : g_nojumpBoxes) addBox(b.uid, b.live.size(), b.sup);
    for (const SlideBox& b : g_slideBoxes) addBox(b.uid, b.live.size(), b.sup);
    // --forceboxon: a force box a Toggle switches (it carries an on / off timeline) is a mover of the shadow too, placed on / off per tick (shadowPlaceMovers)
    if (g_forceBoxOn) for (const ForceBox& b : g_forceBoxes) addBox(b.uid, !b.live.empty() ? b.live.size() : b.onT.size());   // (a carried box)
    // A toggle ring (1594) a trigger moves is a mover of the shadow's world too. The model keeps the rings as touch boxes (or leaves one out: its group is empty), not in L.dyn, so the world's ring stood
    // where it was loaded. The pass's own press takes ONE toggle ring (ringJump's slot B, buttons.hpp), the first of the rings the body touches: a custom level t=18026, two toggle rings and a yellow ring on
    // one spot (33735,885), uid 197071 (group 276, empty) riding a Move (x 33739.69 on that tick) and uid 197401 (switches a saw's group off) standing still. GD's body touches 197401 two ticks earlier and
    // the press takes it (touchseed 197401:18026); the world's 197071, not moved, was touched on the same tick and took the slot, and the saw stayed on (the model died on it at t=18059).
    {
        size_t nNew = 0, nRows = 0;   // (nNew: the rings this made movers; a ring that already was one - a model mover - is not counted)
        for (const PressRing& r : g_pressRings) {
            if (!r.shadowMover()) continue;
            const size_t before = B.moverUids.size();
            addBox(r.uid, 1);
            if (B.moverUids.size() != before) { ++nNew; if (r.liveT.size() > 1) ++nRows; }
        }
        if (nNew) std::printf("shadowstep: %zu toggle ring(s) of %zu are movers of the world (placed every tick: %zu from recording rows, %zu from the supply or where they were loaded)\n", nNew, g_pressRings.size(), nRows, nNew - nRows);
    }
    // --shadowsupply, Follow Player Y: the objects of a follow group (and the big colliders --bigsection files by their load-time cell) stand where each state's follow pass puts them, so they are movers of the shadow's world (step.hpp t_followPlaced)
    if (g_shadowSupply) {
        for (const FollowY& f : g_followYs) for (const auto& m : f.objs) addBox(m.first, 1);
        for (const auto& m : g_followSecObjs) addBox(m.first, 1);
        for (const auto& m : g_bigSecObjs) addBox(m.first, 1);
        for (const auto& m : g_bigSecDyn) addBox(m.first, 1);
    }
    if (!g_shadowSeedFile.empty()) shadowReadSeed(B, g_shadowSeedFile);
    // the template the carried words are laid over: the seed's image (the level's own constant words), else the generated one
    B.clean.resize(gdcol::tmpl::kImageBytes);
    {
        auto hv = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        for (size_t i = 0; i < gdcol::tmpl::kImageBytes; ++i) B.clean[i] = (uint8_t)(hv(gdcol::tmpl::kImageHex[2 * i]) * 16 + hv(gdcol::tmpl::kImageHex[2 * i + 1]));
    }
    B.tmpl = B.seedHave ? B.seedImg : B.clean;
    // +0xc48 is not a carried word (shadow_lite.hpp kSlRanges stops at 0xc48), so a stepped state's image has the template's: GJBaseGameLayer's level start sets it to
    // `!enablePlayerSqueeze` in both players (RVA 0x34532a / 0x345352) and postCollision's crush rule is skipped while it is set. The generated template holds 1 (a level
    // without Enable Player Squeeze); a level with it needs 0 -- a seed image from the game carries the game's own value.
    if (!B.seedHave && B.tmpl.size() > 0xc48) B.tmpl[0xc48] = g_enablePlayerSqueeze ? 0 : 1;
    if (!gdcol::g_slStore) gdcol::g_slStore = new gdcol::SlStore();   // where the states' blocks live (shadow_lite.hpp), for the life of the process
    B.ok = true;
    std::printf("shadowstep: modes 0x%02x, %zu objects, %zu movers, seed %s\n", (unsigned)g_shadowStepModes, B.objs.size(), B.moverUids.size(),
                B.seedHave ? ("tick " + std::to_string(B.seedT)).c_str() : "none");
}

// The shadow as the last latency-1 tick of a thread left it after its tail (S2 speed): the tail of a latency-1 tick reads the parent and the tick, not
// the input (the input is the head's at the end), so the next call for the same parent (State bytes) on the same tick, with the movers placed the
// same, takes this instead of unpacking and stepping the tail again -- a search steps a parent's two children one after the other on one thread.
// Holds what the rest of a tick (the bridged teleports, the head, the pack) may change: the image, the sets the buttons write, the button / dash /
// clock words, the block and the layer as unpacked, the tail's SelfOut, and the world's `activated` flags (undone from SelfShadow::actUndo).
// gate, ever and the layer words the step reads (sh.L) are not written after the tail. Valid only for the call right after the one that saved it:
// any call that does not take it drops it.
struct TailMemo {
    bool valid = false;
    State s{};
    long long t = 0, genT = 0;
    uint64_t gen = 0;
    gdcol::Mem img, atButtons;
    gdcol::SelfShadow::StepPoint ptButtons;
    FlatSet<int> usedRings, now;
    std::vector<int> arr;
    bool holding[4] = {false, false, false, false};
    bool dashOn = false, dashFirstDone = false, speedDirty = false;
    float dashRot = 0.f, dashElapsed = 0.f, dashSpinRate = 0.f, pendingSpeed = 1.f;
    double layerTime = 0.0;
    int frameSteps = 0, prevFrameSteps = 0;
    long long curTickNow = 0;
    uint32_t rng = 0;
    std::vector<uint32_t> actIdx;
    gdcol::ShadowLite sl{};
    gdcol::ShadowLayer own{};
    gdcol::SelfOut outT;
    bool exact = false, seededM = false, cntInexact = false, cntSeedFile = false, cntGrav = false;
};

// One thread's shadow: its own copy of the world (positions of the movers and the activated flags are per call), the shadow and its section index.
struct ShadowThread {
    bool built = false;
    std::vector<gdcol::GObj> world;
    gdcol::SelfShadow sh;
    gdcol::SlWorld W;
    uint64_t gen = ~0ull;
    long long genT = -1;
    int genFr = -1;   // the frame the movers were placed in (shadowPlaceMovers): a group steps parents of other frames too, and which one came first on a thread decided where the movers stood for the rest
    bool frameBad = false;   // --shadowframe: the last placement met a mover it cannot turn back to the world
    std::vector<uint8_t> seen;
    // shadowPlaceMovers without a sweep of every mover (a level of 32k movers spent 16 % of the search there): the movers the last placement
    // set seen (the only ones whose seen is 1), and every mover whose `inactive` may be false -- that placement's, and the follow objects the
    // supply switched on since. Until the thread's first placement every mover may be on (the world is the base's copy): liveAll.
    std::vector<uint32_t> seenList, liveList;
    bool liveAll = true;
    gdcol::SelfOut outs[3];   // shadowAdvance's SelfOut (the pending head, the tail, the head), reset at each use instead of constructed (a std::map each)
    gdcol::SelfOut& out(int k) { outs[k].reset(); return outs[k]; }
    gdcol::SelfOut outs2[3];  // --shadowdual: player 2's SelfOut of the same three calls, reset at each use likewise
    gdcol::SelfOut& out2(int k) { outs2[k].reset(); return outs2[k]; }
    TailMemo memo;
    std::vector<std::pair<uint32_t, uint8_t>> undo;   // sh.actUndo: the world's `activated` changes since the memo was saved / restored
    gdcol::DualShadow ds;     // --shadowdual: player 2 (its slot) and the layer's dual calls; ds.sh is this thread's shadow
    bool dualSlotClean = false;   // shadowDualLoad has reset ds.two to the template (see there)
    uint64_t tapGen = ~0ull;      // shadowApplyTapGates: the g_togTableGen tapItems was made for
    std::vector<std::pair<uint32_t, uint32_t>> tapItems;   // (world index, index into g_togTable.objs) of the level's objects a Tap switches
};

// g_shBase.idx's answer for a uid (the world index, -1 = none) from the thread's dense index when the uids are unique (SlWorld::idxOfUid: the same
// index the map holds then), without the hash lookup
inline long long shadowIdxOf(const ShadowThread& T, int uid) {
    if (!T.W.idxOfUid.empty()) return (uid >= 0 && (size_t)uid < T.W.idxOfUid.size()) ? (long long)T.W.idxOfUid[(size_t)uid] : -1;
    const auto it = g_shBase.idx.find(uid);
    return it == g_shBase.idx.end() ? -1 : (long long)it->second;
}

inline void shadowMemoSave(ShadowThread& T, const State& s, long long t, const gdcol::ShadowLite& sl, const gdcol::ShadowLayer& own, const gdcol::SelfOut& outT) {
    TailMemo& M = T.memo;
    const gdcol::SelfShadow& sh = T.sh;
    static_assert(std::is_trivially_copyable<State>::value, "the memo's key is the State's bytes");
    std::memcpy(static_cast<void*>(&M.s), &s, sizeof(State)); M.t = t; M.gen = T.gen; M.genT = T.genT;
    M.img.b = sh.img.b; M.ptButtons = sh.ptButtons; if (sh.keepImages) M.atButtons.b = sh.atButtons.b;
    M.usedRings = sh.usedRings; M.now = sh.rt.now; M.arr = sh.rt.arr;
    for (int k = 0; k < 4; ++k) M.holding[k] = sh.holding[k];
    M.dashOn = sh.dashOn; M.dashFirstDone = sh.dashFirstDone; M.speedDirty = sh.speedDirty;
    M.dashRot = sh.dashRot; M.dashElapsed = sh.dashElapsed; M.dashSpinRate = sh.dashSpinRate; M.pendingSpeed = sh.pendingSpeed;
    M.layerTime = sh.layerTime; M.frameSteps = sh.frameSteps; M.prevFrameSteps = sh.prevFrameSteps; M.curTickNow = sh.curTickNow; M.rng = sh.rng;
    M.actIdx = T.W.actIdx;
    std::memcpy(static_cast<void*>(&M.sl), &sl, sizeof sl);
    M.own = own;
    M.outT = outT;
    T.undo.clear();
    M.valid = true;
}
inline void shadowMemoRestore(ShadowThread& T, gdcol::ShadowLite& sl, gdcol::ShadowLayer& own, gdcol::SelfOut& outT) {
    TailMemo& M = T.memo;
    gdcol::SelfShadow& sh = T.sh;
    for (auto it = T.undo.rbegin(); it != T.undo.rend(); ++it) { T.world[it->first].activated = it->second != 0; if (it->second) T.W.noteTrue(it->first); }
    if (!T.undo.empty()) sh.activatedChanged();
    T.undo.clear();
    sh.img.b = M.img.b;
    sh.ptButtons = M.ptButtons; if (sh.keepImages) sh.atButtons.b = M.atButtons.b;
    sh.usedRings = M.usedRings; sh.rt.now = M.now; sh.rt.arr = M.arr;
    for (int k = 0; k < 4; ++k) sh.holding[k] = M.holding[k];
    sh.dashOn = M.dashOn; sh.dashFirstDone = M.dashFirstDone; sh.speedDirty = M.speedDirty;
    sh.dashRot = M.dashRot; sh.dashElapsed = M.dashElapsed; sh.dashSpinRate = M.dashSpinRate; sh.pendingSpeed = M.pendingSpeed;
    sh.layerTime = M.layerTime; sh.frameSteps = M.frameSteps; sh.prevFrameSteps = M.prevFrameSteps; sh.curTickNow = M.curTickNow; sh.rng = M.rng;
    T.W.actIdx = M.actIdx;
    std::memcpy(static_cast<void*>(&sl), &M.sl, sizeof sl);
    own = M.own;
    outT = M.outT;
}
inline thread_local bool t_shAlt = false;   // SHSTEP_PURECHECK: the call runs on the thread's second shadow (a different history)
inline ShadowThread& shadowThreadOf(std::unique_ptr<ShadowThread>& P, uint64_t& baseGen, uint64_t& callGen);
inline ShadowThread& shadowThread() {
    static thread_local std::unique_ptr<ShadowThread> P, PA;
    static thread_local uint64_t baseGen = 0, callGen = 0, baseGenA = 0, callGenA = 0;
    return t_shAlt ? shadowThreadOf(PA, baseGenA, callGenA) : shadowThreadOf(P, baseGen, callGen);
}
inline ShadowThread& shadowThreadOf(std::unique_ptr<ShadowThread>& P, uint64_t& baseGen, uint64_t& callGen) {
    // Built again when the base is (shadowStepInit: another level or another set of movers in the same process); the tail memo is dropped at
    // every new call (it is keyed by a State's bytes, whose slRef names a slot of the store that a later call may have given to another block).
    // ...and at every new call: the thread's world is written while it steps (an object's per-step bytes), and a call of the same level in the same process took it
    // over as the last call left it -- a custom level: a section replayed after another one that had passed the teleport orb at x 7,410 met the orb as a plain ring
    // (seqcall, 4200 then 4600: t=5392 against t=10878 alone)
    if (!P || baseGen != g_shBaseGen || callGen != g_shCallGen) { P = std::make_unique<ShadowThread>(); baseGen = g_shBaseGen; }
    ShadowThread& T = *P;
    if (callGen != g_shCallGen) { callGen = g_shCallGen; T.memo.valid = false; }
    {   // the whole images at the buttons and at the pass's entry are kept only for the two readers that print or dump them (SelfShadow::keepImages)
        static const bool dbg = std::getenv("SHSTEP_DEBUG") != nullptr;
        T.sh.keepImages = dbg || !g_shadowQuietDumpPath.empty();
    }
    { static const bool noMemo = std::getenv("SHSTEP_NOMEMO") != nullptr; if (noMemo) T.memo.valid = false; }   // (debug: find what makes the search depend on the thread)
    if (!T.built) {
        const auto tb0 = std::chrono::steady_clock::now();
        const ShadowBase& B = g_shBase;
        T.world = B.objs;
        T.sh.world = &T.world;
        T.sh.index();
        T.sh.setMovers(B.moverUids, true);
        T.sh.dynHeight = g_dynamicLevelHeight != 0;
        T.sh.platformerLevel = g_platformerMode != 0;
        T.sh.p22 = g_hsP22 != 0;
        T.sh.seedSkipInactive = true;   // (selfshadow.hpp seedMemory)
        T.sh.prepareNeighbourhood();
        T.sh.setNbReuse(std::getenv("SHSTEP_NONB") == nullptr);   // (shadowPlaceMovers says worldChanged whenever it places the movers)
        T.sh.actUndo = &T.undo;  // (the tail memo undoes the head's `activated` writes)
        T.W.build(&T.world, B.mover, B.seedArr);
        T.W.lazyEver = true;     // (ever's rule part is asked by the pass, not inserted at every unpack)
        T.W.trackActivated();    // (pack reads the movers that can be activated, not all of them; the world's flags are all clear here)
        if (T.W.trackAct) T.sh.actTrue = &T.W.actTrueLog;
        T.seen.assign(T.world.size(), 0);
        T.built = true;
        ++g_shStats.threadBuilds;
        g_shStats.threadBuildNs += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tb0).count();
    }
    return T;
}

// A mover this group's windows do not hold is off, and it stands where the level loaded it. It used to keep the place of whichever group this thread had put it in last: nothing read it
// while it was off, except slPack, which asks where the movers of `ever` and of the drop lists are (SlWorld::right) to keep them or let them go -- so what a block held depended on the order
// in which the thread had stepped its states, and the search at 8 threads (each with its own world) did not match the search at 1 (c136_591: 77 of 198 children of one layer).
inline void shadowParkMover(ShadowThread& T, const ShadowBase& B, uint32_t i) {
    gdcol::GObj& G = T.world[i];
    const gdcol::GObj& B0 = B.objs[i];
    G.inactive = true;
    G.cx = B0.cx; G.cy = B0.cy; G.prevCx = B0.prevCx; G.prevCy = B0.prevCy;
    G.rx = B0.rx; G.ry = B0.ry; G.rw = B0.rw; G.rh = B0.rh;
    G.rot = B0.rot; G.oriented = B0.oriented; G.bw = B0.bw; G.bh = B0.bh;
}
// The movers where this group's tick put them (the model's Obj): every mover the group's windows hold is placed and on,
// every other one is off (a mover outside the windows cannot reach the player's rect in this tick).
inline void shadowPlaceMovers(ShadowThread& T, const StepCtx& K, int fr = 0) {
    const uint64_t g = K.sgen;   // (this group's own number: the global counter another thread bumps made a group's placement stand for the next one's, and the search depended on the schedule)
    if (g != 0 && g == T.gen && K.t == T.genT && fr == T.genFr && !(g_shadowSupply && K.fol && !K.fol->empty())) return;   // (--shadowsupply, Follow Player Y: the follow objects stand where THIS state's follow pass put them, so the next call places them again)
    T.gen = g; T.genT = K.t; T.genFr = fr;
    T.frameBad = false;
    const ShadowBase& B = g_shBase;
    // (the boxes, angles and inactive bytes below change without a moverMoved: every mover this placement writes is told to the neighbourhood's index at its end, moversTouched -- the objects it placed and the
    // ones the last placement had on -- so that a reused neighbourhood reads those copies again; it was dropped whole here, at every group's every tick, and copied afresh: 99.96% of the fresh copies, 16% of the calls)
    const bool hadMovers = !T.W.moverIdx.empty(), wasLiveAll = T.liveAll;
    if (T.liveAll) for (uint32_t i : T.W.moverIdx) T.seen[i] = 0;
    else for (uint32_t i : T.seenList) T.seen[i] = 0;
    T.seenList.clear();
    const std::vector<const Obj*>* lists[6] = {K.near, K.ports, K.pads, K.orbs, K.slopes, K.speeds};
    for (const auto* lp : lists) {
        if (!lp) continue;
        for (const Obj* o : *lp) {
            if (!o || !o->dynObj) continue;
            const long long ix = shadowIdxOf(T, o->uid);
            if (ix < 0 || !B.mover[(size_t)ix] || T.seen[(size_t)ix]) continue;
            const size_t i = (size_t)ix;
            T.seen[i] = 1;
            T.seenList.push_back((uint32_t)i);
            gdcol::GObj& G = T.world[i];
            // a turned frame (--shadowframe): the model's Obj is in the frame's (travel, lateral) coordinates; the shadow's world is the level's own
            const double pdx = o->dcx;   // --shadowdcx: the previous x
            const double pcyOwn = !std::isnan(o->pcy) ? (double)o->pcy : o->cy - o->dcy;   // --shadowprevcy: GD's previous position is the last tick's, not this tick's cy less a step the closed forms get wrong
            double wcx = o->cx, wcy = o->cy, wpx = o->cx - pdx, wpy = pcyOwn;
            double whw = o->hw, whh = o->hh;
            if (fr) {
                fromFrame(fr, o->cx, o->cy, wcx, wcy);
                fromFrame(fr, o->cx - pdx, pcyOwn, wpx, wpy);
                if (fr & 1) std::swap(whw, whh);

            }
            const float cx = (float)wcx, cy = (float)wcy;
            const bool moved = G.cx != cx || G.cy != cy;
            G.cx = cx; G.cy = cy;
            G.prevCx = (float)wpx; G.prevCy = (float)wpy;
            G.rw = (float)(2.0 * whw); G.rh = (float)(2.0 * whh);
            G.rx = cx - G.rw * 0.5f; G.ry = cy - G.rh * 0.5f;
            if (o->oriented) {   // a turned mover: this tick's angle and its unturned box (Obj::rot, ohw / ohh), as self_test turns a wtrace row's object
                G.rot = (float)(o->rot - 90.0 * (double)fr);   // (frames.hpp turnObj: the frame's rotation is added to the Obj's)
                if (fr == 0 && o->rotT == (long long)K.t) G.rot = (float)o->rotSup;   // the supply's angle for this tick (search_dp.hpp supplyPlaceMovers; a tick other than the group's own: not supplied)
                G.oriented = true;
                G.bw = (float)(2.0 * o->ohw); G.bh = (float)(2.0 * o->ohh);
                if (B.objs[i].hasGc && B.objs[i].bw > 0.0f && B.objs[i].bh > 0.0f) {   // (hasGc is only set for the ill-conditioned ones, above)
                    G.bw = B.objs[i].bw; G.bh = B.objs[i].bh; }   // --shadowgcfirst: the game's own box size, not the model's solved one
                // GD's box is the object's size times its scale (GameObject::updateOrientedBox, +0x394 / +0x398 x the scale words), so a hazard a Scale has resized since the objrects were written is the dumped
                // box (the game's corners, or the model's solved one) times the scale ratio since then: the timeline's osx / osy over the one the dump holds (supplyPlaceMovers wrote it for this tick).
                // custom level A uid 16076: dumped 0.572 x 9.552, 3.637 x 60.854 at t=14815 (GD's bound, solved with its angle) = x 6.36. custom level B uid 1171 (id 216, turned 126): the dump's box 1.5 x 3.0 against a
                // bound of 15.0 x 16.4 at t=1608, where GD kills a swing copter 2 px inside it and the model's 1.5 x 3.0 box let it pass.
                if (gdev::g_objrectsStartScale && o->type == 2 && o->radius <= 0.0 && fr == 0 && o->sclT == (long long)K.t && o->sclX > 0.0f && o->sclY > 0.0f) {
                    G.bw *= o->sclX; G.bh *= o->sclY;
                }
                G.hasGc = false;   // (--shadowobbcorners: the corners were taken at the load angle, the mover stands at this tick's)
            } else if (o->obbOk && o->radius <= 0.0) {   // a moving box Dynamics::seek keeps the axis (bc, bs) of
                double wbc = o->bc, wbs = o->bs;
                if (fr) fromFrame(fr, o->bc, o->bs, wbc, wbs);   // (the axis is a direction: the frame's turn, no shift)
                G.rot = (float)(-std::atan2(wbs, wbc) * (180.0 / 3.14159265358979));
                G.oriented = true;
                G.bw = (float)(2.0 * o->bhw); G.bh = (float)(2.0 * o->bhh);
                G.hasGc = false;
            } else {   // not turned in this group: the object as the level has it. Left alone, the angle (and the oriented box) of whichever group this thread placed it in last stayed on it -- a later
                       // tick's turned angle on the object of a group before the turn -- and what the player met depended on the order the thread had stepped its states in.
                const gdcol::GObj& B0 = B.objs[i];
                G.rot = B0.rot; G.oriented = B0.oriented; G.bw = B0.bw; G.bh = B0.bh;
                G.hasGc = B0.hasGc;
                if (B0.hasGc) { for (int q = 0; q < 8; ++q) G.gc[q] = B0.gc[q]; G.gcx = B0.gcx; G.gcy = B0.gcy; }
                if (o->rotTurned && !o->oriented) { G.oriented = false; G.hasGc = false; }   // --shadowturnedorb (constants.hpp): a ring a Rotate has turned back to a right angle is a plain rect again
            }
            // A plain rect's own scale, as the saw's below: the rect the recording gives is GD's hitbox at that tick, scale included; where the rect is still the loaded one and the timeline has scaled the
            // object since (Obj::sclX / sclY for this tick, search_dp.hpp supplyPlaceMovers), the hitbox is the loaded rect times the timeline's ratio, about the same centre. The supply wrote that ratio for
            // every supplied mover, but only a saw's radius (circleHit) read it, so a supplied orb past the end of its recording kept the loaded size: a custom level t=4096, green gravity orb uid 194871
            // (id 1022, loaded 25.2 x 25.2), shrunk from t~4086 by the x-crossing Scale uid 29856 (x 6258.5); the search's world kept 25.2, the plan's press at 4095 flipped gravity on it, and GD, whose
            // orb stood at 23.753 (flight 15's recorded rect), did not flip and the cube died at 4113.
            // [M] the recorded rect of that orb in GD's flight: 25.200 (t=4025), 24.822 (4090), 23.925 (4095), 23.753 (4096) -- the hitbox follows the scale.
            // The ratio is the one from BEFORE the tick's step (Obj::sclPX / sclPY): GD moves the object on the step's tick and sizes its hitbox by the scale it had before. [M] a custom level, block uid 37458
            // (id 1, 30 x 30, group 1542 scaled about group 1542 by the x-crossing Scale uid 39885 / 39886 at x 17640): GD's recorded rect at t=12424 is cy 795 -> 794.654 with h still 30.000, at 12425
            // h 28.697; the cube on it is at y 824.654 / 823.672 (dump), on a 30-high box then a 28.697-high one. With this tick's ratio the model sized it 28.697 already at 12424 (y 824.319) and ran
            // a tick ahead of GD (its 12425 = GD's 12426): the 604 section a custom level:12200 diverged at 12424 instead of 12425 (rec and norec, gd06_rsc2_604).
            if (!G.oriented && o->radius <= 0.0 && o->sclT == (long long)K.t) {
                const gdcol::GObj& B2 = B.objs[i];
                if (B2.rw > 0.0f && B2.rh > 0.0f && std::fabs(G.rw - B2.rw) < 1e-3f && std::fabs(G.rh - B2.rh) < 1e-3f) {   // (still the loaded rect: a scaled recorded rect is GD's own and stands)
                    float sx = o->sclPX, sy = o->sclPY;
                    if (fr & 1) std::swap(sx, sy);   // (supplyPlaceMovers swapped them into the frame's axes; the world object is in the level's)
                    if (sx > 0.0f && sy > 0.0f && (sx != 1.0f || sy != 1.0f)) {
                        G.rw = B2.rw * sx; G.rh = B2.rh * sy;
                        G.rx = G.cx - G.rw * 0.5f; G.ry = G.cy - G.rh * 0.5f;
                        T.sh.moverMoved(i);
                    }
                }
            }
            {   // a saw's own scale (circleHit: R = max(scaleX, scaleY) * m_objectRadius): the level's load scale unless the object has been scaled since. Two sources: the rect the recording (or the supply) gives, which
                // is the bound of the unscaled hitbox w0 x h0 turned by the object's angle, times the scale (so scale = rect / (|cos| w0 + |sin| h0), the same on both sides when it is only a scale); and the timeline's
                // own scale ratio (Obj::sclX, the supply: ObjState::osx) when the rect is still the loaded one. a custom level uid 36133: load 0.658, GD's 0.6539 at T=7295 (hbox 34.002 x 35.310 of 52 x 54).
                const gdcol::GObj& B1 = B.objs[i];
                const float s0x = G.scaleX, s0y = G.scaleY;
                G.scaleX = B1.scaleX; G.scaleY = B1.scaleY;
                if (B1.radius > 0.0f && B1.w0 > 0.0f && B1.h0 > 0.0f && G.rw > 0.0f && G.rh > 0.0f && B1.scaleX > 0.0f && B1.scaleX == B1.scaleY) {
                    const double th = (o->rot - 90.0 * (double)fr) * (3.14159265358979 / 180.0);
                    const double cc = std::fabs(std::cos(th)), ss = std::fabs(std::sin(th));
                    const double aw = cc * (double)B1.w0 + ss * (double)B1.h0, ah = ss * (double)B1.w0 + cc * (double)B1.h0;
                    const double sw = (double)G.rw / aw, sh = (double)G.rh / ah;
                    const double q = sw / (double)B1.scaleX;
                    // (only a hitbox that is not square: the recording's rot column is not kept for a hazard, so a turning saw's bound is read at the load angle; for w0 != h0 the two axes of the bound then disagree unless the angle is the loaded one, and the test below refuses it)
                    if (std::fabs(B1.w0 - B1.h0) > 0.5f && std::fabs(sw - sh) <= 2e-3 * sw && q > 0.2 && q < 5.0 && std::fabs(q - 1.0) > 2e-4) { G.scaleX = (float)sw; G.scaleY = (float)sw; }   // the rect says the object is not at its load scale
                    else if (o->sclT == (long long)K.t) { float rx = o->sclX, ry = o->sclY; if (fr & 1) std::swap(rx, ry); G.scaleX = B1.scaleX * rx; G.scaleY = B1.scaleY * ry; }   // still the loaded rect: the timeline's ratio
                }
                if (G.scaleX != s0x || G.scaleY != s0y) T.sh.moverMoved(i);
            }
            G.farHold = false;
            G.inactive = (o->onNew != 0) || (o->recOn == 0);   // --shadowrecon (constants.hpp)   // --shadowonnew: the row that switches a hazard ON is not there for its own tick's pass (Obj::onNew, dynamics.hpp place())
            {   // SHSTEP_FORCEMOVE=uid,tick,cy,prevcy (diagnosis only): put one mover where GD held it at one tick
                static int fu = -1; static long long ft = -1; static double fcy = 0, fpcy = 0; static bool fr = false;
                if (!fr) { fr = true; if (const char* e = std::getenv("SHSTEP_FORCEMOVE")) std::sscanf(e, "%d,%lld,%lf,%lf", &fu, &ft, &fcy, &fpcy); }
                if (fu >= 0 && o->uid == fu && K.t == ft) { G.cy = (float)fcy; G.prevCy = (float)fpcy; G.ry = G.cy - G.rh * 0.5f; T.sh.moverMoved(i); }
            }
            if (moved) T.sh.moverMoved(i);
        }
    }
    // The model keeps the type-0x28 special boxes (the ceiling arm 1859, the dart slide arm 1755, the flip-head arm) apart from the lists above: its own arm counters read
    // them from the recording (modifiers.hpp g_armBoxes / g_slideBoxes / g_flipHeadBoxes, rows ascending in t). A box that a group moves is a mover of the shadow's world too;
    // left out of the lists it was switched off, so the shadow never touched it and GD's arm counter (+0xb80 / +0xb84) never came up: a custom level t=11079, a cube under a sliding
    // 90x30 slab of the same group as a 120x120 id-1859 box, GD squashed the jump against the slab (CollisionTopUid) and the shadow, with the arm at 0, let it through.
    auto placeBox = [&](int uid, const std::vector<ModRow>& live, double bcx, double bcy, double hw, double hh, bool on, bool sup = false) {
        const long long ix = shadowIdxOf(T, uid);
        if (ix < 0 || !B.mover[(size_t)ix] || T.seen[(size_t)ix]) return;
        const size_t i = (size_t)ix;
        double bx = bcx, by = bcy;
        if (!live.empty()) {
            size_t lo = 0, hi = live.size();
            while (lo < hi) { const size_t m = lo + (hi - lo) / 2; if (live[m].t <= (int)K.t) lo = m + 1; else hi = m; }
            if (lo > 0) { bx = (double)live[lo - 1].cx; by = (double)live[lo - 1].cy; }
        } else if (sup && fr == 0 && g_supBoxPlaceHook) {   // no recording rows and a trigger moves the box: where the trigger shadow's timeline has it this tick, and its on / off (M1, a custom level:5800: a dart slide arm of a supplied group stayed at its load-time place with no recording)
            double sx, sy; bool son = true;
            if (g_supBoxPlaceHook(uid, sx, sy, son)) { bx = sx; by = sy; if (!son) on = false; }
        }
        T.seen[i] = 1;
        T.seenList.push_back((uint32_t)i);
        gdcol::GObj& G = T.world[i];
        if (!live.empty()) {   // the recording's on/off at this tick, read where the position is (the last row at or before t)
            size_t lo = 0, hi = live.size();
            while (lo < hi) { const size_t m = lo + (hi - lo) / 2; if (live[m].t <= (int)K.t) lo = m + 1; else hi = m; }
            if (lo > 0 && live[lo - 1].on == 0) on = false;
        }
        const float cx = (float)bx, cy = (float)by;
        const bool moved = G.cx != cx || G.cy != cy;
        G.cx = cx; G.cy = cy; G.prevCx = cx; G.prevCy = cy;
        G.rw = (float)(2.0 * hw); G.rh = (float)(2.0 * hh);
        G.rx = cx - G.rw * 0.5f; G.ry = cy - G.rh * 0.5f;
        G.inactive = !on;
        if (moved) T.sh.moverMoved(i);
    };
    for (const ArmBox& b : g_armBoxes) placeBox(b.uid, b.live, b.cx, b.cy, b.hw, b.hh, true, b.sup);
    for (const FlipHeadBox& b : g_flipHeadBoxes) placeBox(b.uid, b.live, b.cx, b.cy, b.hw, b.hh, true, b.sup);
    for (const FlipHeadBox& b : g_nojumpBoxes) placeBox(b.uid, b.live, b.cx, b.cy, b.hw, b.hh, true, b.sup);
    for (const SlideBox& b : g_slideBoxes) placeBox(b.uid, b.live, b.cx, b.cy, b.hw, b.hh, true, b.sup);
    if (g_forceBoxOn) {
        static const std::vector<ModRow> kNoRows;
        for (const ForceBox& b : g_forceBoxes) {
            if (!b.live.empty()) placeBox(b.uid, b.live, b.cx, b.cy, b.hw, b.hh, forceBoxOnAt(b, K.t));   // --forceboxlive: a carried box stands where the recording has it
            else if (!b.onT.empty()) placeBox(b.uid, kNoRows, b.cx, b.cy, b.hw, b.hh, forceBoxOnAt(b, K.t));
        }
    }
    {   // the toggle rings a trigger moves (ShadowBase: addBox): the recording's row for the tick, or with no rows the supply's place
        static const std::vector<ModRow> kNoRingRows;
        for (const PressRing& r : g_pressRings) {
            if (!r.shadowMover()) continue;
            double bx = r.cx, by = r.cy; bool on = true;
            const long k = r.liveT.size() > 1 ? pressRingRow(r, (long long)K.t) : -1;
            if (k >= 0) { bx = (double)r.liveX[(size_t)k]; by = (double)r.liveY[(size_t)k]; on = r.liveOn[(size_t)k] != 0; }
            placeBox(r.uid, kNoRingRows, bx, by, r.hw, r.hh, on, r.liveT.size() <= 1 && r.sup != 0);
        }
    }
    // Every mover not placed is off. Only the movers that can be on need the write: the last placement's and the follow objects the supply
    // switched on since (liveList); the others are off already. SHPLACE_CHECK=1 compares this with the sweep of every mover it replaces.
    static const bool check = std::getenv("SHPLACE_CHECK") != nullptr;
    if (T.liveAll) {
        for (uint32_t i : T.W.moverIdx) if (!T.seen[i]) shadowParkMover(T, B, i);
        T.liveAll = false;
    } else {
        for (uint32_t i : T.liveList) if (!T.seen[i]) shadowParkMover(T, B, i);
        if (check) {
            long long bad = 0;
            for (uint32_t i : T.W.moverIdx) if (!T.seen[i] && !T.world[i].inactive) { ++bad; T.world[i].inactive = true; }
            ++g_shPlaceChecks;
            g_shPlaceCheckDiff += bad;
        }
    }
    if (hadMovers) { if (wasLiveAll) T.sh.worldChanged(); else T.sh.moversTouched(T.seenList, T.liveList); }   // (T.liveList is still the last placement's here)
    T.liveList.assign(T.seenList.begin(), T.seenList.end());
}

// A narrow Tap's plain Move (gdtrig::g_tapMoves: a Move spawned at every press, read from the trigger's columns) applied to its movers from this state's own press count (State::tapN, tapWin): every press adds
// the Move's offset, eased over its duration (the command's own `eased` of the elapsed share; the first step of a command does not advance its clock, trigshadow.hpp stepCommands), the presses that have run
// their whole duration in full. The timelines hold the group without these presses (the Tap has no chain there); this is laid over the shared placement and the placement is asked for again by the next call, as for
// a Follow Player Y object. Only a mover the placement has switched on (the model's windows hold it); the position is the group's, so the movers' prev goes with it.
inline void shadowApplyTapMoves(ShadowThread& T, const State& s) {
    if (gdtrig::g_tapMoves.empty()) return;
    static const int lat = std::getenv("TAPMOVE_LAT") ? std::atoi(std::getenv("TAPMOVE_LAT")) : 0;
    const ShadowBase& B = g_shBase;
    bool did = false;
    for (const gdtrig::TapMove& m : gdtrig::g_tapMoves) {
        const gdtrig::TogTap* tp = nullptr;
        for (const gdtrig::TogTap& t : gdtrig::g_togTable.taps) if (t.uid == m.tapUid && t.narrow && t.slot >= 0) { tp = &t; break; }
        if (!tp) continue;
        const int n = s.tapN[tp->slot];
        if (n <= 0) continue;
        const int D = (int)std::lround(m.dur * 240.0);
        auto cum = [&](int steps) -> double {   // the share of one press's offset after `steps` steps of its command
            if (steps <= 0) return 0.0;
            if (D <= 0) return 1.0;
            return (double)gdtrig::eased((float)std::min(1.0, (double)(steps - 1) / (double)D), m.kind, (float)m.rate);
        };
        int inWin = 0; double now = 0.0, prev = 0.0;
        for (int i = 0; i < 32; ++i) if ((s.tapWin >> i) & 1u) { ++inWin; now += cum(i + 1 + lat); prev += cum(i + lat); }
        const int done = std::max(0, n - inWin);
        now += (double)done; prev += (double)done;
        const double ddx = m.dx * now, ddy = m.dy * now, pdx = m.dx * prev, pdy = m.dy * prev;
        if (ddx == 0.0 && ddy == 0.0 && pdx == 0.0 && pdy == 0.0) continue;
        for (int uid : m.uids) {
            const long long ix = shadowIdxOf(T, uid);
            if (ix < 0 || !B.mover[(size_t)ix] || !T.seen[(size_t)ix]) continue;
            gdtrig::g_tapMoveApplied.fetch_add(1, std::memory_order_relaxed);
            gdcol::GObj& G = T.world[(size_t)ix];
            G.cx += (float)ddx; G.cy += (float)ddy; G.prevCx += (float)pdx; G.prevCy += (float)pdy;
            G.rx = G.cx - G.rw * 0.5f; G.ry = G.cy - G.rh * 0.5f;
            // unsound: the count has left the key (the player is past the Tap's past x) yet this mover, with its offset on, could still touch the player (its right edge + the cube reaches the player's x): the bound was too small
            if ((double)s.xAbs > gdev::g_tapPastX[tp->slot] && (double)G.cx + 0.5 * (double)std::max(G.rw, G.rh) + kCubeHalf > (double)s.xAbs) gdtrig::g_tapMoveUnsound.fetch_add(1, std::memory_order_relaxed);
            T.sh.moverMoved((size_t)ix);
            did = true;
        }
    }
    if (did) T.gen = 0;   // (the next call places the movers again: the placement is shared by the group's states, this one is this state's)
}

// The level's objects that a Tap (Touch trigger 1595) switches off, held off in this thread's world for the pass: an object is off while any group it sits in is off (GD's toggleGroup counts the
// disabled groups against the object, and the pass skips it, +0x28e). The groups are State::tog (the parent's, which every walk keeps: model.hpp togWorldStep) with the Tap's edge of `edgeInput`
// laid over it when the head that takes the button runs before the pass (edgeInput < 0: no edge now). Only the objects trigevent_togload.hpp lists, so a level without such an object does nothing here.
// A mover is only ever switched off here (shadowPlaceMovers wrote its own `inactive`); when one has been, the next placement is asked for again, because the same group's next state may find the Tap's
// group on and the placement's own value is what has to stand then.
inline void shadowApplyTapGates(ShadowThread& T, const State& s, int edgeInput) {
    const gdtrig::TogTable& TT = gdtrig::g_togTable;
    if (TT.objs.empty()) return;
    if (T.tapGen != gdtrig::g_togTableGen) {
        T.tapItems.clear();
        for (size_t k = 0; k < TT.objs.size(); ++k) {
            const long long ix = shadowIdxOf(T, TT.objs[k].uid);
            if (ix >= 0) T.tapItems.emplace_back((uint32_t)ix, (uint32_t)k);
        }
        T.tapGen = gdtrig::g_togTableGen;
    }
    if (T.tapItems.empty()) return;
    gdtrig::TogBits tog = s.tog;
    if (edgeInput >= 0 && !TT.taps.empty()) {
        // (the crossing's own x twice: no x-gate crosses, only the Tap's button edge acts; the Toggle Orbs of this tick come after the pass)
        uint64_t xg = 0;
        const double px = crossXOf((double)s.xAbs, s.tpBack);
        gdtrig::togStep(TT, tog, xg, px, px, (int)s.action, edgeInput);
    }
    bool changed = false, forcedMover = false;
    for (const auto& it : T.tapItems) {
        bool on = true;
        for (int b : TT.objs[it.second].idx) if (!gdtrig::togGroupOn(TT, tog, b)) { on = false; break; }
        gdcol::GObj& G = T.world[it.first];
        if (g_shBase.mover[it.first]) {
            if (!on) { if (!G.inactive) { G.inactive = true; changed = true; } forcedMover = true; }
        } else if (G.inactive == on) { G.inactive = !on; changed = true; }
    }
    if (changed) T.sh.worldChanged();
    if (forcedMover) T.gen = ~0ull;
}

inline int shadowModeOf(const gdcol::Mem& P) {
    using namespace gdcol;
    if (P.u8(po::ModeShip)) return 1;
    if (P.u8(po::ModeBall)) return 2;
    if (P.u8(po::ModeUfo)) return 3;
    if (P.u8(po::ModeWave)) return 4;
    if (P.u8(po::ModeRobot)) return 5;
    if (P.u8(po::ModeSpider)) return 6;
    if (P.u8(po::ModeSwing)) return 7;
    return 0;
}

// The camera zoom (+0x1a8) and its target (+0x1ac) over the tick, from the model's own Zoom triggers (dynamics.hpp camScaleAt: the x-crossing chain the
// model's band already divides by, at the same x / t its flying code asks). A level without one keeps the anchor's zoom (the seed file's), else 1.
// getMinPortalY's branch B divides the band's height by the zoom (shadow_layer.hpp bandB), so a band taken at zoom 1 was 375 - 300 = 75 px short under a
// zoom of 0.8 (w_132517249_14670: the wave was put on a floor 37.5 px above GD's on the shadow's first tick).
inline float shadowZoomAt(const State& s, long long t, float& target) {
    if (g_zoomTrigs.empty()) { target = g_shBase.seedHave && g_shBase.lyHave ? g_shBase.lyZoom : 1.0f; return target; }
    const double x = (double)s.xAbs;
    const float z = (float)camScaleAt(x, s.dx > 0.f ? (double)s.dx : 1.0, t);
    target = z;
    for (const ZoomTrig& zt : g_zoomTrigs) { if (zt.cx > x) break; target = (float)zt.target; }
    return z;
}

// --camy trace columns lt / lmin / lmax (observation only, never read by the solver): the band the last shadow layer of this thread gave the pass, and its tick.
struct ShadowLayerObs { long long t = -1; float mn = 0.f, mx = 0.f; };
inline thread_local ShadowLayerObs g_shadowLayerObs;

// The layer words a step reads, from the state's ShadowLayer and the model's band.
inline gdcol::Layer shadowLayerFor(const gdcol::ShadowLayer& own, const State& s, long long t) {
    gdcol::Layer L;
    float zt = 1.0f;
    const float zoom = shadowZoomAt(s, t, zt);
    L.speedA = zoom; L.speedB = zt;
    L.scaleX = gdcol::tmpl::kScaleX; L.scaleY = gdcol::tmpl::kScaleY;
    L.ceilWord = g_maxPlayY < 1e17 ? (float)g_maxPlayY : g_shBase.lyCeil > 0.f ? g_shBase.lyCeil : gdcol::tmpl::kCeilWord;   // (--maxplayy, else the anchor's, else 2790)
    L.f1cf = g_fixRadiusCollision != 0;
    L.dynHeight = g_dynamicLevelHeight != 0;
    L.freeMode = own.freeMode;
    L.dual = own.dual;
    L.flipAmt = own.flipKnown ? own.flipAmt : 0.0f;
    float mn = 0.f, mx = 0.f;
    if (own.band(zoom, 0.0f, false, mn, mx)) { L.minPortalY = mn; L.maxPortalY = mx; }
    else {   // the model's band: none is the ground line on both sides
        // The band the state holds -- or, once the first Static Camera exit past the anchor is behind it (the start of this tick: the band the last tick left), the portal's band that
        // the exit puts back in place of the anchor's branch A reading (bands.hpp kBandAnchorA; a mode portal clears the bit)
        float sbF = s.bandFloor, sbC = s.bandCeil;
        if ((s.bandBranch & kBandAnchorA) && g_anchorBandB.have && (double)s.xAbs >= g_anchorBandB.exitX) { sbF = g_anchorBandB.floorY; sbC = g_anchorBandB.ceilY; }
        const bool banded = sbF > 0.f && sbC < 1e8f;
        L.minPortalY = banded ? sbF : 90.0f;
        L.maxPortalY = banded ? sbC : 90.0f;
        // ...under a zoom, the way the model's flying code reads its band (step.hpp --flybandcentre (removed): the stored band is the zoom-1 one about its centre)
        // and getMinPortalY's branch B builds it: min = max(90, centre - H / 2 / zoom), max = min + H / zoom. s_ufo_dn_126365388: zoom 0.8, the model's
        // band 120 / 420, GD's 90 / 465; the shadow seated the UFO on 135.
        if (banded && !g_zoomTrigs.empty() && (s.bandBranch & 3) != 1 && zoom > 0.0f && sbC > sbF) {
            float mn2 = 0.f, mx2 = 0.f;
            gdcol::ShadowLayer::bandB(0.5f * (sbF + sbC), sbC - sbF, zoom, 0.0f, mn2, mx2);
            L.minPortalY = mn2; L.maxPortalY = mx2;
        }
        // ...and a model with no band bounds nothing (the model's band "none" is GD's Free Mode, or a band the model does not hold): the pass's band block
        // (collide.hpp, `!L.freeMode && boundedModes`) would otherwise take 90 / 90 as a floor and a ceiling and pin a ball or a spider to y 75
        // (w_145755012_3430: a free-mode ball after a size portal, GD's pmin = pmax = 90, the shadow's first tick put it on the ceiling of 90).
        // The same for the model's own Free Mode byte (bands.hpp flyBandFree: its band clamps no mode) -- s_swing_dn_113409074: a free swing at y 24,663
        // under a model band of 28,050.. that the model does not clamp to; the shadow put it on that floor.
        if (!banded || flyBandFree(s.bandBranch)) L.freeMode = true;
    }
    // --shadowbandtrack: GD's band from the recording behind --bandtrack, as the old step reads it (bandTrackRowAt). The state's own band is the anchor's or the last portal's and does not follow
    // the camera, which moves the band every tick; a ship riding the ceiling (a custom level t=10704: GD seats it on 412.4, the shadow's band 410.8 never met it) never meets it.
    const bool bandFrameOk = s.frame == 0 || (!s.dual && (s.frame == 1 || s.frame == 3));   // the band is the world's, read in the turned frames too
    if (!L.freeMode && bandFrameOk) {
        double fl = 0.0, ce = 0.0;
        // the pass of tick t clamps against the band the camera left at the end of tick t-1 (the dump's row t-1; the fly floor of the old step reads the same row, bands.hpp): a custom level t=10600..10834 with
        // rows t-1 the ship's y matches GD to the dump's digits, with row t it is 0.007 px off, with t+1 0.014
        // The recording's band is the band of the recorded body: its height is the one its last mode portal wrote (bands.hpp: H = 300 for ship / UFO / wave / swing, 240 ball, 270 the rest, / zoom; the floor and
        // the camera move it, the height stays). A state that did not take that portal holds another height, and the recording's row is not its band: a custom level t=212, the recorded body takes the UFO portal
        // (band 90 / 390) and the spider that keeps to the ceiling does not (GD: pmin / pmax 90 / 360 through t=228, the dump of that flight) -- the row's 390 put the spider's second turn on 376.5 where GD
        // has 346.5, and the model carried a plan to t=968 that GD kills at t=228. The row is read only where its height is the state's own.
        if (bandTrackRowAt(t - 1, fl, ce)) {
            const float stateH = L.maxPortalY - L.minPortalY;
            if (!(stateH > 0.f) || std::fabs((float)(ce - fl) - stateH) <= 0.5f) { L.minPortalY = (float)fl; L.maxPortalY = (float)ce; }
        }
    }
    g_shadowLayerObs = ShadowLayerObs{t, L.minPortalY, L.maxPortalY};
    L.w764 = own.w764Known ? own.w764 : L.maxPortalY;
    if (!L.freeMode && bandFrameOk) {
        double fl2 = 0.0, ce2 = 0.0;
        const float usedH = L.maxPortalY - L.minPortalY;   // (the row's word only where the row is the band in use: its height is the one the layer was given above)
        if (bandTrackRowAt(t - 2, fl2, ce2) && (!(usedH > 0.f) || std::fabs((float)(ce2 - fl2) - usedH) <= 0.5f)) L.w764 = (float)ce2;
    }
    return L;
}

// The turned frame (--shadowframe): the frame the pass runs in (0 = the upright one), and the State's gravity / direction flags as the game's bytes. frames.hpp: frame 1 travels -Y with up = +X, frame 3 travels +Y with up = -X;
// the gravity byte of a frame 3 is the opposite of the State's flip (search_key.hpp gdUpOf), and a frame 1 run forward (rev 0) is Leftward in the game's update (it moves y by -speed). Measured on lv22: gframe 1 -> (Upside 0,
// Leftward 1, Sideways 1), gframe 3 -> (1, 0, 1), a 180 degree turn (the State: frame 0, flip 1, rev 1) -> (1, 1, 0).
inline int shFrameOf(const State& s) { return (!s.dual && (s.frame == 1 || s.frame == 3)) ? (int)s.frame : 0; }
inline uint8_t shGdUp(int fr, uint8_t flip) { return (uint8_t)(fr == 3 ? !flip : (flip ? 1 : 0)); }
inline uint8_t shGdLeft(int fr, uint8_t rev) { return (uint8_t)(fr == 1 ? !rev : (rev ? 1 : 0)); }

// A fresh image from the model's fields over the template (the seed of a state the model stepped last): the mode, size, gravity and speed bytes, the position,
// velocity and rotation, the ground bytes from `grounded`; the slots and the collision logs empty; ever by the seed rule (what is behind the player counts as
// seen) and what the player overlaps now.
// movedAfterPass: the body's pose is one a turn of the frame (a Rotate Gameplay / Teleport the model has taken since the block's pass) put it in after the last collision pass: nothing it overlaps now was touched by
// that pass, so seedMemory counts as seen only what lies entirely behind it, as for a Teleport Orb (s.tpSkip & 8). [M] a custom level:4600, frame 3 -> 0 on tick 4799 (x 6570 -> 7395): GD fires the ship portal (uid
// 12332, x 7401..7435) the new pose overlaps on the first pass after the turn (fresh: F 3360 -> 3330); the reseed below read "overlapped now" as "seen", fired it not fresh, and wrote no band.
// ---- KEYOBS (print only): what State::prevWX / prevWY, which is not in the dedupe key, could cost ----
// A seed from the State makes an object fresh when the body overlaps it now and the rect it had at the start of its last update (prevWX / prevWY) did not (selfshadow.hpp seedMemory). Two states of one key that differ
// in prevWY can answer that differently, and the dedupe keeps one of them. seedFreshSig is the set of the LEVEL's static objects that test makes fresh for a state, as a hash (the rect test alone: not the rotated-box
// test, not the movers, a body's half-size from its mode and mini), and the dedupe (search_dp.hpp, the shard loop) counts the states it drops whose signature differs from the one it keeps.
inline std::atomic<long long> g_prevUnsoundCmp{0}, g_prevUnsoundDiff{0}, g_prevUnsoundNone{0};
inline uint64_t seedFreshSig(const State& s) {
    if (!g_shBase.ok || !(s.prevWX > -1e29f) || !(s.prevWY > -1e29f)) return 0;
    struct Idx { std::vector<uint32_t> byRx; float maxW = 0.f; };
    static const Idx ix = [] { Idx r; for (size_t i = 0; i < g_shBase.objs.size(); ++i) { if (i < g_shBase.mover.size() && g_shBase.mover[i]) continue; r.byRx.push_back((uint32_t)i); r.maxW = std::max(r.maxW, g_shBase.objs[i].rw); }
        std::sort(r.byRx.begin(), r.byRx.end(), [](uint32_t a, uint32_t b) { return g_shBase.objs[a].rx < g_shBase.objs[b].rx; }); return r; }();
    const int sfr = shFrameOf(s);
    double wX = s.xAbs, wY = s.y;
    if (sfr) fromFrame(sfr, (double)s.xAbs, (double)s.y, wX, wY);
    const double h = playerHalf(s.mode, s.mini != 0);
    const double lo = wX - h - ix.maxW - 1.0, hi = wX + h + 1.0;
    uint64_t sig = 0;
    size_t a = 0, b = ix.byRx.size();
    while (a < b) { const size_t m = a + (b - a) / 2; if ((double)g_shBase.objs[ix.byRx[m]].rx < lo) a = m + 1; else b = m; }
    for (size_t k = a; k < ix.byRx.size(); ++k) {
        const gdcol::GObj& o = g_shBase.objs[ix.byRx[k]];
        if ((double)o.rx > hi) break;
        if (o.type == 0 || o.type == 2 || o.type == 7 || o.type == 0x15 || o.type == 0x19 || o.type == 0x27) continue;
        const bool now = o.rx <= wX + h && wX - h <= o.rx + o.rw && o.ry <= wY + h && wY - h <= o.ry + o.rh;
        if (!now) continue;
        const double qx = (double)s.prevWX, qy = (double)s.prevWY;
        const bool before = o.rx <= qx + h && qx - h <= o.rx + o.rw && o.ry <= qy + h && qy - h <= o.ry + o.rh;
        if (!before) sig ^= (uint64_t)(uint32_t)o.uid * 0x9E3779B97F4A7C15ull + 1;
    }
    return sig;
}
// The dedupe dropped `lost` and kept `kept` (one key): does the seed's fresh set differ?
inline void noteDroppedPrev(const State& lost, const State& kept) {
    if (!(lost.prevWY > -1e29f) || !(kept.prevWY > -1e29f)) { ++g_prevUnsoundNone; return; }
    ++g_prevUnsoundCmp;
    if (lost.prevWX == kept.prevWX && lost.prevWY == kept.prevWY) return;
    if (seedFreshSig(lost) != seedFreshSig(kept)) ++g_prevUnsoundDiff;
}
inline void shadowSeedFromModel(ShadowThread& T, const State& s, long long t, gdcol::ShadowLite& sl, bool second = false, bool movedAfterPass = false) {
    using namespace gdcol;
    SelfShadow& sh = T.sh;
    ContactShadow cs;
    ShadowIn in;
    const int sfr = shFrameOf(s);
    double wX = s.xAbs, wY = s.y;
    if (sfr) fromFrame(sfr, (double)s.xAbs, (double)s.y, wX, wY);
    const float gvy = sfr == 3 ? -s.vy : s.vy;   // the State's vy is the velocity along its lateral axis (frame 3: -X); the game's byte is along +X
    in.x = (float)wX; in.y = (float)wY; in.vy = gvy; in.mode = s.mode; in.mini = s.mini != 0; in.flip = shGdUp(sfr, s.flip) != 0; in.grounded = s.grounded != 0;
    in.speed = shadowSpeedOf(s.dx > 0.f ? s.dx : 1.29825f);
    in.fixGrav = g_fixGravityBug != 0;
    cs.img.b = g_shBase.clean;   // (not the anchor's image: its mode's words, a reverse correction in flight (+0x9e0) and the like are not this state's)
    // the seed's own writes (ContactShadow::seed without its template), then the body
    cs.img.put<int64_t>(po::Cf, 0);
    cs.img.u8(po::G, in.grounded ? 1 : 0);
    cs.img.u8(po::G2, in.grounded ? 1 : 0);
    cs.img.u8(po::Boost, (!in.grounded && (in.flip ? gvy < 0.f : gvy > 0.f)) ? 1 : 0);
    cs.img.u8(po::WasOnSlope, 0); cs.img.u8(po::OnSlope, 0);
    for (size_t off : {(size_t)po::CollisionBottomUid, (size_t)po::CollisionTopUid, (size_t)po::PrevBottomUid, (size_t)po::PrevTopUid, (size_t)po::CollisionLeftUid, (size_t)po::CollisionRightUid}) cs.img.put<int32_t>(off, -1);
    for (size_t off : {(size_t)0x5e8, (size_t)0x5f0, (size_t)0x600, (size_t)0x608, (size_t)0x610, (size_t)0x618, (size_t)0x680, (size_t)0x890, (size_t)0x590, (size_t)0xa88}) cs.img.put<int64_t>(off, 0);
    cs.applyBody(in);
    Mem& P = cs.img;
    // The block it last stood on (GD's m_objectSnappedTo, +0x890, uid + 1) and how far its x was from that block's centre (+0x840): checkSnapJumpToObject nudges a cube that lands on
    // the block a fixed stride from that one by up to 1 px in x (collide.hpp). The model keeps both (State::snapObj, snapDist); the seed left the slot empty, and the landing at
    // custom level A t=2264 (GD x +1.0 px against the stride from uid 6477) and its two likes (custom level B t=5542, custom level A t=10656) came out a pixel short.
    if (s.snapObj) { P.put<int64_t>(po::SnappedTo, (int64_t)s.snapObj->uid + 1); P.f64(po::SnapDist, (double)s.snapDist); }
    if (s.grounded) {   // the solid a grounded body touches in the gravity direction is its ground object (+0x608, with +0x660 up) and, with no snapObj in the State, its snapped block (+0x890 / +0x840)
        const double half = playerHalf(in.mode, in.mini);
        const bool dn = !in.flip;
        const gdcol::GObj* best = nullptr; double bestGap = 0.35;
        for (const gdcol::GObj& g : T.world) {
            if (g.type != 0 || g.inactive || g.passable || g.radius > 0.f) continue;
            double gap;
            if (!sfr) {
                if (std::fabs(wX - (double)g.cx) >= (double)g.rw * 0.5 + half - 0.05) continue;
                const double face = dn ? (double)g.ry + (double)g.rh : (double)g.ry;
                gap = std::fabs((dn ? wY - half : wY + half) - face);
                // A mover the world has placed for the step being run stands one tick past the body, which is the anchor's state: where it stood a tick earlier (prevCx / prevCy) is where the body rests on it (lv19 5901: a ship carried down by thin strips moving 0.86 px a tick, the gap to the placed position 0.86, to the previous one 0.0004).
                if (g.prevCx != g.cx || g.prevCy != g.cy) {
                    if (std::fabs(wX - (double)g.prevCx) < (double)g.rw * 0.5 + half - 0.05) {
                        const double facePrev = dn ? (double)g.prevCy + (double)g.rh * 0.5 : (double)g.prevCy - (double)g.rh * 0.5;
                        gap = std::min(gap, std::fabs((dn ? wY - half : wY + half) - facePrev));
                    }
                }
            } else {   // a turned frame (Sideways): gravity runs along x, so the ground is the block whose x face the body touches and whose y span it overlaps (a ball on a block moving along x, lv22 6101: no ground seeded, the first pass fell 0.129)
                if (std::fabs(wY - (double)g.cy) >= (double)g.rh * 0.5 + half - 0.05) continue;
                const double face = dn ? (double)g.rx + (double)g.rw : (double)g.rx;   // up = 0 falls toward -x (frame 1, measured on lv22 6101)
                gap = std::fabs((dn ? wX - half : wX + half) - face);
                if (g.prevCx != g.cx || g.prevCy != g.cy) {   // (the same, for a mover one tick past the anchor)
                    if (std::fabs(wY - (double)g.prevCy) < (double)g.rh * 0.5 + half - 0.05) {
                        const double facePrev = dn ? (double)g.prevCx + (double)g.rw * 0.5 : (double)g.prevCx - (double)g.rw * 0.5;
                        gap = std::min(gap, std::fabs((dn ? wX - half : wX + half) - facePrev));
                    }
                }
            }
            if (gap < bestGap) { bestGap = gap; best = &g; }
        }
        if (best) {
            P.put<int64_t>(po::GroundObj, (int64_t)best->uid + 1); P.u8(0x660, 1);
            if (!s.snapObj) { P.put<int64_t>(po::SnappedTo, (int64_t)best->uid + 1); P.f64(po::SnapDist, wX - (double)best->cx); }
            ++g_shCol.seedSnap;
        }
    }
    upd::updateTimeMod(P, in.speed);   // the speed's own constants (+0x7c0 the x factor, +0x7c8 / +0x7d0 the jump and the gravity): applyBody writes the speed word alone
    P.f32(po::PosX, (float)wX); P.f32(po::PosY, (float)wY);
    P.f32(po::PrevX, (float)wX); P.f32(po::PrevY, (float)wY);
    // The rect the body had at the start of the update that made this state (State::prevWX / prevWY, the game's frame): the once-per-contact test in seedMemory (selfshadow.hpp) reads PrevX / PrevY, and an
    // object first touched by the last step is fresh only if the rect before it did not overlap. With PrevX = x the rect always did, and every object the body overlapped now was "seen" (GB a custom level t=23284: the
    // pad 166360 the body first met at 23283 never fired in a search that seeded here). A state with no such record (an anchor, a state file; the second body of a dual, whose prev is not carried) keeps the old values.
    if (!second && s.prevWX > -1e29f && s.prevWY > -1e29f) { P.f32(po::PrevX, s.prevWX); P.f32(po::PrevY, s.prevWY); ++g_shStats.seedPrevGiven; }
    else ++g_shStats.seedPrevNone;
    P.f32(0xa00, (float)wX); P.f32(0xa04, (float)wY);
    P.f32(0xa98, (float)wX); P.f32(0xa9c, (float)wY);
    P.f64(po::Vy, (double)gvy);
    P.f32(0x38, s.rot);
    P.u8(po::Sideways, sfr ? 1 : 0);
    P.u8(po::Upside, shGdUp(sfr, s.flip));
    // --shadowframe: the velocity-limit exemption byte (+0x95a, State::boost) the State carries: updateJump skips a flier's terminal clamp while it is up (0x4e9b80 L310-408), and a seed with it down clamped a ship launched
    // past 8 at once (a custom level t=20600: 8.64 -> 8.00 where the game's 8.02, 7.92 ... come down by gravity)
    P.u8(0x95a, s.boost ? 1 : 0);
    P.u8(po::Leftward, shGdLeft(sfr, s.rev));   // the direction the model is travelling
    // A cube in the air is turning: the rotation rate (+0x728) and its switch (+0x730) were set when it left the ground, and the model's State has no word for them. Without them the seed's
    // cube stood still for two ticks and then ran two ticks behind GD (a custom level t=2201: rot 17.26 against 18.99, 3.46 degrees at 1.73 per tick). A rotation that is not there (ship,
    // ufo, wave...) is refused by runNormalRotation itself.
    if (!in.grounded && in.mode == 0) {
        runNormalRotation(P, false, 1.0f);
        if (!sfr) {   // the rate the State carries (rotNeg: the spin's sign; rotGm: the gravity value it was staked at)
            const float mag = std::fabs(P.f32(0x728)) * s.rotGm;
            const bool neg = s.rotNeg != 0;
            if (mag != 0.0f) P.f32(0x728, neg ? -mag : mag);
        }
        // --spinseed (constants.hpp): the word the hist payload carries (the mod's cfg histseed2) wins over the estimate above; no word, the estimate (gd-02's --shadowspinseed) or the standard spin stands
        if (!second && g_spinSeed && g_seedSpinKnown && t >= g_seedSpinT && t <= g_seedSpinT + 1) { P.f32(0x728, (float)g_seedSpin / 1000.0f); P.u8(0x730, g_seedSpin != 0 ? 1 : 0); }
    }
    // A ball spins at the stake it was last given (+0x728, set by runBallRotation on the ground / a pad / a portal, by runBallRotation2 in the air after a flip or a ring); the model keeps the
    // stake as State::rotStep (the per-tick step: ballRotRate, ground or air) and the seed left the words empty, so a seeded ball did not turn until its next stake: a custom level t=13800,
    // the ball's angle ran 220 degrees from GD's by the time a tilted pad (SAT depth 0.97 at t=14136) had to be met, and the pad was missed.
    // ...and a ball with no stake in force (rotStep 0: an anchor does not carry it, a slope's launch leaves none) does not turn: the template's spin words are the clean cube's, and a seeded
    // ball turned at +3.755 a tick from the first row where GD's held its angle for a hundred (a custom level t=13801..13900, rot 21.56).
    if (in.mode == 2) { P.u8(0x730, 0); P.u8(0x731, 0); P.f32(0x728, 0.0f); P.u8(0x670, 0); }
    if (in.mode == 2 && s.rotStep > 0.0f) {
        const bool mi = s.mini != 0;
        const double dxr = s.dx > 0.f ? (double)s.dx : 1.29825;
        if (std::fabs((double)s.rotStep - ballRotRate(mi, false, dxr)) < 1e-3) runBallRotation(P, 1.0f);
        else if (std::fabs((double)s.rotStep - ballRotRate(mi, true, dxr)) < 1e-3) runBallRotation2(P);
        else {
            // a ball a slope launched: collidedWithSlopeInternal stakes runBallRotation(f) with f = 1 / cos(the slope's angle) (collide.hpp, `f28`; (f - 1) * 0.7 + 1 past 2), so the step it carries is the
            // ground one times f (0x670 up, +0x72c = f: updateRotation spends dt / 60 * +0x728 * f a tick). The model's rotStep is that product, and with no branch for it the seed left the ball still:
            // a custom level t=19401 (the ball carried 4.1983 = 3.7551 * 1.11803, a 0.5 slope) stood at its angle for the 49 ticks to its landing while GD turned 4.198 a tick, and from the landing on its oriented
            // box ran 205.73 degrees behind GD's: against the thin rotated spike id 216 at t=19651 that box overlapped where GD's (3.7 px apart) lives.
            const double gr = ballRotRate(mi, false, dxr);
            const double f = (double)s.rotStep / gr;
            if (f > 1.0 + 1e-3 && f < 4.0) runBallRotation(P, (float)f);
        }
    }
    P.f32(0xb8c, (float)((!sfr && (!g_gravTrigs.empty() || s.gravTouch)) ? gravModOfCh(crossXOf((double)s.xAbs, g_tpBack ? s.tpBack : 0), g_gravTouch ? s.gravTouch : 0, !g_rotQ.empty() ? (int)s.rotChan : -1, s.rev != 0, (uint32_t)s.trig.word(0)) : 1.0));
    // Inside a dual both players carry the layer's dual flag in +0xaa1 (toggleDualMode 0x31de96 stores it into both); playerIsFallingBugged reads it for a player who is not upside down (d2 = -gravity),
    // so a seed without it called a rising ship "falling" and flew 0.127 a tick where the game's 0.101 (lv16 t=13800, player 1: fv19 0.8 against 1.2, f24 0.5 against 0.4).
    if (s.dual) P.u8(0xaa1, 1);
    // The two arm counters the model keeps as "ticks since the touch" (collide.hpp collisionCheckObjects case 0x28 writes 2, PlayerObject::update steps
    // it down; the model's 0 = touched at this tick's end, so GD's counter after the pass is kArmTicks - ticks): the dart slide (id 1755, +0xb80, State::slideT)
    // and the ceiling arm (id 1859, +0xb84, State::armT). w_139771614_6030: a wave seeded from the model as a time warp ended, its 1755 arm left at 0,
    // fell through the surface GD seats it on for one tick.
    P.i32(0xb80, s.slideT < kArmTicks ? kArmTicks - (int)s.slideT : 0);
    P.i32(0xb84, s.armT < kArmTicks ? kArmTicks - (int)s.armT : 0);
    {   // (frame 0 only: in a turned frame the correction is along the world axis the frame travels, whose sign this has no measurement for)
        P.u8(0x562, g_reverseSync == 0 ? 0 : 1);   // the level's reverseSync (levelsettings; an older dump without the line counts as set, as step.hpp revReflect does): reversePlayer books its debt only with it, and the generated template has it clear
        { P.f64(0x9d8, (double)s.revOff); P.f64(0x9e0, (double)s.revCarry); }   // ...and the correction in flight (a rotated frame too under --revframesign: the body plays in the world's axes and the State's two words are in the world's sign)
    }
    // SEED ESTIMATE, to be replaced by the GD image's own bytes (the file seed): the State does not carry GD's press-latch bytes, only the model's mirror of them, so a model seed has to guess them.
    //  - S (+0x98e) is left as the template has it: seeding it as !pressSpent (the State's definition, hist v6: pressSpent = H && !S) broke custom level A t=3800 and custom level B t=14200 in the customs sections.
    //  - H (+0x98d) follows the button unless the hold was spent. A ball without a measured buffer keeps the spent-hold estimate (lv20 section 9400).
    //    GD Win 2.2091: pushButton writes H at RVA 0x4f6299 even in the air; updateJump checks the landing at 0x4e9e0f and clears H after the flip at 0x4ea11c.
    const bool bufferedBall = !second && !g_swapBodies && g_seedHistT0 >= 0 && t == g_seedHistT0 && s.jumpBuf;
    P.u8(0x98d, (s.action && (in.mode != 2 || bufferedBall) && !s.holdDead) ? 1 : 0);
    { const uint8_t h = P.u8(0x98d); const uint8_t sv = (h && !s.pressSpent) ? 1 : 0; P.u8(0x98e, sv); P.u8(0x991, h); P.u8(0x992, sv); }   // --shadowseedmirror: S, Hm, Sm as pushButton leaves them
    P.u8(0x368, 1); P.u8(0x369, 1);
    if (s.dashing && !P.u8(po::Platformer)) {   // the dash the State is in (a dash ring's line), as startDashing leaves it: +0x9ec, the slope words +0x570 / +0x578 and the angle +0x580
        const double sl = (double)s.dashSlope;
        P.u8(0x9ec, 1);
        P.f64(0x578, sl); P.f64(0x570, std::fabs(sl)); P.f64(0x580, std::atan(sl) * 57.29577951308232);
    }
    sh.img = P;
    sh.fireOrder.clear(); sh.gate.clear(); sh.usedRings.clear(); sh.rt.arr.clear(); sh.rt.now.clear(); sh.ever.clear();
    for (int k = 1; k <= 3; ++k) sh.holding[k] = false;
    sh.holding[1] = s.action != 0;
    sh.dashOn = s.dashing; sh.dashFirstDone = false; sh.dashRot = 0.f; sh.dashElapsed = 0.f;
    // A mid-dash anchor does not carry the gravity and orientation from the last action restart.
    sh.dashSpinRate = sh.dashOn ? gdcol::SelfShadow::dashRate(P) : 0.f;
    sh.speedDirty = false; sh.pendingSpeed = 1.0f;
    sh.frameSteps = (int)((t - 1) % 4) + 1; sh.prevFrameSteps = 4;
    if (g_shadowDashAge && sh.dashOn && !second && g_seedDashAgeKnown && t >= g_seedDashAgeT && t <= g_seedDashAgeT + 1) {   // --shadowdashage: the frames the dash action has run before the anchor
        const long long ts = t - g_seedDashAge;                       // the dash's first row
        const long long hs = ts - (((ts - 1) % 4) + 4) % 4;           // the head of the frame it started in (a head is the step with (tick - 1) % 4 == 0)
        const int heads = (int)((t - hs) / 4);                        // heads since: the first counts nothing
        if (heads >= 1) {
            sh.dashFirstDone = true;
            const float sched = (float)rawTimeWarpAt((double)s.xAbs);
            for (int k = 1; k < heads; ++k) sh.dashElapsed += 0.0166667f * sched;
            sh.dashRot = sh.dashSpinRate * sh.dashElapsed;
        }
    }
    sh.layerTime = (double)t * sh.stepSeconds;
    sh.img.f64(po::Time, sh.layerTime);
    // A turned frame's gravity flip stamp (GD's flipGravity writes +0x808 := the layer's time; collidedWithObjectInternal L1182-1228 re-seats a body that is inside a solid instead of killing it while
    // now - stamp < 0.1 s, and the pass of a cube / ball / robot / spider reads the clock the seed holds, a ship's / UFO's one step later). The seed is built from the State, which keeps the flip only as
    // its clock flipT (ticks since gdUpOf changed, 0 on that tick): without the stamp a mini cube that a Rotate Gameplay turned 17 ticks ago was killed by a plate's side where GD re-seats it
    // (lv22 t=11335 uid 11475, 12177 uid 6313, 12835 uid 6338: GD's state one tick before is the shadow's to the digit, the next row is the re-seat). The window is the model's own (constants.hpp
    // kFlipGraceTicks, measured: 24 ticks earlier lives, 25 dies, i.e. the pass of age flipT + 1 is inside while that is below 25). Every frame (it was turned frames only while an old tick ran beside the shadow): GD flown on a custom level (hitboxtrace with the player's +0x808 / +0x828 / clock added) has +0x808 = +0x828 = 48.441669 at the passes of t=11400..11405 in frame 0, the clock 48.500002 .. 48.520835 = 14..20 ticks old, inside the 24-tick window, and re-seats a spider the band clamp lifted into solid 41612 (y 253.5 -> 241.5) where the seed without the stamp killed it (collide.hpp, the body-inside-a-solid kill); [M] hbox2_133880546.
    if (!second && s.flipT < kFlipGraceTicks)
        sh.img.f64(0x808, sh.layerTime + ((s.mode == 1 || s.mode == 3) ? sh.stepSeconds : 0.0) - (double)((int)s.flipT + 1) * sh.stepSeconds);
    // A CEILING-side ride (the model's ceilT, onSlope = 0, no uid of its own: constants.hpp g_seedUnderUid) is the same ride in GD: one clock +0x598 stamped at the contact, m_isOnSlope set, the ramp's words written. Without these the first pass sees an air -> ramp contact and re-stamps the clock (lv16 8901: a 30-tick ride exits at 4.001 = 14/24 of 6.906).
    // (A first version seeded an upside-down body only, because an UPRIGHT ship under a descending ceiling ramp launched on its first real pass in the 604 harness (a custom level:22200, vy -3.729 where GD has 0). That was the harness's: it inferred a dash from the trajectory form (a ride down a ramp has a constant dy, like a dash) and seeded the ship dashing, whose update moves y by 0x578 * dx. GD's ctraw at 22199-22204 says 0x9ec = 0 and the pass entry y 210.7264557 (vy 0.108), the ride's contact held; with the dash read off the dump column dashing1 the unrestricted seed matches GD over the whole window.)
    // (The second body's ride is the same, its State halves swapped in: g_seedUnderUid2 / g_seedUnderT2, --seedslope2's underside regime; its words come through the block below, which already takes a second body under --seedslope2.)
    const bool underSeed = !s.onSlope && s.ceilT > 0
                           && (second ? (g_seedUnderUid2 >= 0 && t >= g_seedUnderT2 && t <= g_seedUnderT2 + 1)
                                      : (g_seedUnderUid >= 0 && t >= g_seedUnderT && t <= g_seedUnderT + 1));
    if (s.onSlope || underSeed) {   // the ride the model is on: GD's m_isOnSlope / m_wasOnSlope and its one clock per ride (+0x598, stamped by the air -> ramp pass, collide.hpp 911)
        // The model's slopeT is 0 on the contact tick; the clock difference GD's recording point reads is slopeT + 1 there (kRideAgeLag, cli.hpp --anchorride, cfg slopetrace).
        // a custom level t=8901 (GD: Time - 0x598 = 0.0625 s = 15 steps at the jump head, the model's slopeT = 12) and t=25801 hold with this; slopeT + 2 jumps 0.063 too high.
        // The clock the shadow runs the pass on is the dump row's clock + 1 step for a latency-2 call (a ship or a UFO: the head runs FIRST, so the pass reads this tick's Time) and the row's clock for a latency-1 call (the tail first: the pass reads the last head's Time). GD's age (the payload's slopeAge = row clock - stamp) is read in the row's clock, so the stamp a seed writes is layerTime - (A + 1) steps for latency-1 and layerTime - A steps for latency-2 (A = age - 1, cli.hpp): measured on lv16 13101 (a ball: the seeded exit read 12 ride ticks where GD and the natural run read 13) against 8701 (a ship, right with no extra step).
        const int kRideAgeLag = (s.mode == 1 || s.mode == 3) ? 0 : 1;   // --shadowrideage (constants.hpp)
        const int rideT = s.onSlope ? (int)s.slopeT : (int)s.ceilT - 1;   // ceilT is 1 on the contact tick (cli.hpp --anchorride: A + 1), slopeT 0
        const int rideUid = s.onSlope ? (int)s.slopeUidNow : (second ? g_seedUnderUid2 : g_seedUnderUid);
        // A ceiling-side ride is marked only once the primed pass below has seen the ramp take the body: marked without it, the first real pass finds no contact, reads "left a ramp" and launches (a custom level:22200, a steady ride under a descending ceiling ramp: vy -3.729 on the first row where GD has 0).
        auto markRide = [&]() {
            sh.img.u8(po::WasOnSlope, 1); sh.img.u8(po::OnSlope, 1);
            sh.img.f64(0x598, sh.layerTime - (double)(rideT + kRideAgeLag) * sh.stepSeconds);
        };
        if (!underSeed) markRide();
        if (rideUid >= 0 && (!second || g_seedSlope2)) {   // the words GD wrote when the body took this ramp (--seedslope2: the second body's too, from the payload's version 11)
            const long long wi = shadowIdxOf(T, rideUid);
            if (wi >= 0 && (size_t)wi < T.world.size()) {
                // The body sits on the surface the ramp gave it the tick before, to within float noise (the anchor's y is 0.0002 off the recomputed one at a custom level:9400), and the words wanted (the surface, the
                // angle, the ratio, the launch speed) do not depend on y: the pass runs with primeWords, which skips the contact test and the kill check and keeps the writes.
                gdcol::Mem tmp = sh.img;
                tmp.u8(po::WasOnSlope, 0); tmp.u8(po::OnSlope, 0);
                gdcol::Ctx cc(tmp);
                cc.L = sh.L; cc.objs = &T.world; cc.primeWords = true;
                cc.slopeRotation = [](gdcol::Ctx& q, float d) { upd::updateSlopeRotation(q.P, d); };
                gdcol::collidedWithSlope(cc, t_shDt, &T.world[(size_t)wi], true);   // through the turned frame's wrapper (the ramp is turned about the body, which flips its top / bottom)
                if (tmp.u8(po::OnSlope)) {   // the ramp took the body: its words are the ride's
                    if (underSeed) markRide();
                    static const std::pair<size_t, size_t> kWords[] = {{0x688, 8}, {0x690, 4}, {0x694, 1}, {0x938, 8}, {0x940, 8}, {0x948, 8}, {0x950, 8}, {0x958, 1}, {0x959, 1}, {0x95b, 1},
                                                                       {0x9bc, 4}, {0x5f8, 4}, {0x5fc, 1}, {0x6a0, 4}, {0x680, 8}, {0x678, 8}, {0x5e8, 8}, {0xb44, 4}};
                    for (const auto& w : kWords) std::memcpy(sh.img.b.data() + w.first, tmp.b.data() + w.first, w.second);
                    ++g_shCol.seedSlope;
                    static const char* const envSeedDbgLogSw = std::getenv("SEEDDBG_LOG");   // read once: getenv takes the CRT's environment lock (see shDbgUid)
                    if (const char* sdl = envSeedDbgLogSw) if (std::FILE* lf = std::fopen(sdl, "a")) { std::fprintf(lf, "sw t %lld uid %d top %d w938 %.6f w940 %.6f w9bc %.6f w948 %.4f w958 %d w959 %d\n", t, (int)sh.img.i32(0x690), (int)sh.img.u8(0x95b), sh.img.f64(0x938), sh.img.f64(0x940), (double)sh.img.f32(0x9bc), sh.img.f64(0x948), (int)sh.img.u8(0x958), (int)sh.img.u8(0x959)); std::fclose(lf); }
                } else ++g_shCol.seedSlopeMiss;
            }
        }
    }
    if (s.mode == 5 && s.rHover > 0) {   // (a dual's bodies too: there is no old tick to take a dual seed's mismatch)   // --shadowseedrobot (constants.hpp); a dual's hover is not seeded: custom level A:8200 and custom level B:12600 (both dual) clear without it, diverge with it (the budget the model carries there is not GD's)
        const double tw = timeWarpAt((double)s.xAbs);
        const float dtS = (tw != 1.0) ? (float)(0.25 * (double)(float)tw) : 0.25f;
        const double inc = (double)((dtS * 0.9f) * 0.1f);   // updateJump(dt * 0.9): +0x838 += dt * 0.1
        const int spent = std::max(0, (int)robotHoverTicks(tw) - (int)s.rHover);
        double w = 0.0;
        for (int k = 0; k < spent; ++k) w += inc;
        sh.img.f64(0x838, w);
        sh.img.u8(0x9a4, 0);
    }
    // The rings the model has fired are marked activated BEFORE the seed's own passes below: the pass that registers the rings the body overlaps (m_touchingRings, the one that follows the
    // force blocks) asks `activated` like any collision pass, and a ring spent earlier (a custom level t0=17401, ring 2021 fired at 17365, the body still inside its box) was registered with the mark
    // not yet written, so the first press fired it again (vy 9.70 -> 4.57, GD kept 9.70).
    const bool ringP2 = sh.p2Phase;   // --shadowringp2: player 2's seed fills its own fired set and leaves player 1's flags in the world
    if (!ringP2) {
    if (!T.W.actIdx.empty()) sh.activatedChanged();
    for (uint32_t i : T.W.actIdx) T.world[i].activated = false;
    T.W.actIdx.clear();
    }
    // The rings the model has fired (usedOrb, usedOrbOld, usedOrbHist: its five slots): a ring that is not multi-activate is marked activated by the jump it gives
    // (EnhancedGameObject::activatedByPlayer, +0x5b3 / +0x5b4) and answers "fired" to collisionCheckObjects until resetObject / restoreObject (an attempt reset or a checkpoint load) clears
    // the mark, so the shadow must not fire it again. a custom level t=21277: a wave jumped off the yellow ring 1589 at 21266, turned cube at 21270; seeded with none the shadow
    // jumped off the same ring again on the next press.
    {
        const int fired[5] = {s.usedOrb ? s.usedOrb->uid : -1, s.usedOrbOld, s.usedOrbHist[0], s.usedOrbHist[1], s.usedOrbHist[2]};
        if (sh.fireOrder.empty()) for (int u : fired) if (u >= 0) sh.fireOrder.push_back(u);   // --shadowfireorder: a shadow with no record yet starts from the State's slots (newest first)
        for (int u : fired) {
            const auto it = g_shBase.idx.find(u);
            if (u < 0 || it == g_shBase.idx.end() || T.world[it->second].multiAct) continue;
            if (ringP2) { sh.act2.insert(u); continue; }
            T.world[it->second].activated = true;
            T.W.noteTrue((uint32_t)it->second);
            T.W.actIdx.push_back((uint32_t)it->second);
        }
        if (!T.W.actIdx.empty()) sh.activatedChanged();
    }
    if (!second) {
        // The force blocks (Special 0x815 / 0xe3d) the body overlaps were answered by the pass of the tick before: +0xb90 := 2 and their push added to +0xb94 / +0xb98, which the update of this tick applies.
        thread_local std::vector<uint32_t> fbIdx; thread_local uint64_t fbGen = ~0ull; thread_local size_t fbN = 0;
        if (fbGen != g_shBaseGen || fbN != T.world.size()) {
            fbIdx.clear(); fbGen = g_shBaseGen; fbN = T.world.size();
            for (size_t i = 0; i < T.world.size(); ++i) if (T.world[i].type == 0x28 && (T.world[i].objectId == 0x815 || T.world[i].objectId == 0xe3d)) fbIdx.push_back((uint32_t)i);
        }
        if (!fbIdx.empty()) {
            gdcol::Mem tmp = sh.img;
            tmp.i32(0xb90, 0); tmp.f32(0xb94, 0.f); tmp.f32(0xb98, 0.f);
            gdcol::Ctx cc(tmp);
            cc.L = sh.L; cc.objs = &T.world;
            std::vector<gdcol::GObj*> cand, solids, hazards;
            for (uint32_t i : fbIdx) cand.push_back(&T.world[i]);
            gdcol::collisionCheckObjects(cc, 0.25f, cand, solids, hazards);
            if (tmp.i32(0xb90) > 0) {
                std::memcpy(sh.img.b.data() + 0xb90, tmp.b.data() + 0xb90, 4);
                std::memcpy(sh.img.b.data() + 0xb94, tmp.b.data() + 0xb94, 8);
                std::memcpy(sh.img.b.data() + 0xba8, tmp.b.data() + 0xba8, 8);
                ++g_shCol.seedForce;
            }
        }
    }
    if (!second && !s.dual) {   // (not in a dual: a custom level:10200's player 1 diverges at 10203 with the list seeded, 10376 without)   // --shadowseedring (constants.hpp)
        // The rings the body overlaps were registered by the pass of the tick before (playerTouchedRing -> m_touchingRings); the seed started that list empty, so a press in the first step fired nothing (a custom level:6600).
        thread_local std::vector<uint32_t> rgIdx; thread_local uint64_t rgGen = ~0ull; thread_local size_t rgN = 0;
        if (rgGen != g_shBaseGen || rgN != T.world.size()) {
            rgIdx.clear(); rgGen = g_shBaseGen; rgN = T.world.size();
            for (size_t i = 0; i < T.world.size(); ++i) { const int ty = T.world[i].type; if (ty == 0xb || ty == 0xc || ty == 0xd || ty == 0x1d || ty == 0x20 || ty == 0x23 || ty == 0x24 || ty == 0x25 || ty == 0x26 || ty == 0x2b || ty == 0x2e) rgIdx.push_back((uint32_t)i); }
        }
        if (!rgIdx.empty()) {
            gdcol::Mem tmp = sh.img;
            tmp.u8(0x992, 0);   // no press armed: the pass registers the touch and ringJump returns at once
            gdcol::Ctx cc(tmp);
            cc.L = sh.L; cc.objs = &T.world;
            std::vector<gdcol::GObj*> cand, solids, hazards;
            for (uint32_t i : rgIdx) cand.push_back(&T.world[i]);
            gdcol::collisionCheckObjects(cc, 0.25f, cand, solids, hazards);
            for (int u : cc.ringsTouched) { sh.rt.touched(u); ++g_shCol.seedRing; }
            // A cube's (any non-ship, non-UFO) head of the seed's own step is the END of the step before it, which the old model has already run: GJBaseGameLayer's resetTouchedRings has dropped `now`,
            // so the rings stay in the list (m_touchingRings) for the first press but are not re-armed for the next reset. Left in `now`, the ring outlived its last pass by one more step, and a press
            // one step later fired it (lv21 t=4512: the anchored tail flipped on the ring uid 4691 the full-plan replay and the game had left; 7 iterations on the same death at 4534).
            // A ship / UFO seed (latency 2) runs that head at its first step (headPending), which resets then.
            if (!(s.mode == 1 || s.mode == 3)) sh.rt.now.clear();
        }
    }
    static const char* const envSeedDbgLogP2 = std::getenv("SEEDDBG_LOG");   // read once (see shDbgUid)
    if (second) if (const char* sdl = envSeedDbgLogP2) if (std::FILE* lf = std::fopen(sdl, "a")) { std::fprintf(lf, "p2seed t %lld mode %d y %.4f vy %.4f grounded %d snapObj %d snapDist %.4f flip %d G %d SnappedTo %lld Collided %lld\n", (long long)t, (int)s.mode, (double)s.y, (double)s.vy, (int)s.grounded, s.snapObj ? s.snapObj->uid : -1, (double)s.snapDist, (int)s.flip, (int)sh.img.u8(po::G), (long long)sh.img.get<int64_t>(po::SnappedTo), (long long)sh.img.get<int64_t>(po::Collided)); std::fclose(lf); }
    sh.platformerNoted = true;
    sh.seedTurnX = 0.0f;
    if (sh.img.u8(po::Sideways) && !second) {   // the world x of the rotation trigger that turned this frame (the latest spent queue entry that sets s.frame)
        float best = 0.0f;
        for (size_t k = 0; k < g_rotQ.size() && k < 64; ++k) {
            const RotQEntry& e = g_rotQ[k];
            if (e.id != 2900 || e.rotIdx < 0 || e.chanOnly || !((s.rotSpent >> k) & 1ull)) continue;
            const RotTrig& r = g_rotTrig[(size_t)e.rotIdx];
            if (r.frame == (int)s.frame && (float)r.cx > best) best = (float)r.cx;
        }
        if (best > 0.0f) best = std::min(best, slPlayerLeft(sh.img));   // (left of the body as well: the old rule's x test, cut off at the turn)
        sh.seedTurnX = best;
    }
    sh.seedMemory((g_seedHistT0 >= 0 && t == g_seedHistT0 && s.spiderJumpT == 0 && !second) || ((s.tpSkip & 8) != 0 && !second) || (movedAfterPass && !second));   // (--seedtporb: a Teleport Orb moved the body after the last pass)   // --seedspiderpass (constants.hpp)
    // A run's first tick has seen nothing: GD starts an attempt with an empty once-per-contact map, so a portal the player starts inside (a Teleport or a mode portal at
    // x = 0) is fresh at tick 1. seedMemory's \ever\ (what is behind or under the player) is for a seed mid-level.
    const bool levelStart = t <= 1;
    if (levelStart) { sh.ever.clear(); sh.gate.clear(); }
    for (auto it = sh.ever.begin(); it != sh.ever.end();) {   // the movers behind the player are not taken as seen (a model seed has no list of its own for them)
        const long long wi = sh.worldIndexOf(*it);
        if (wi >= 0 && T.W.mover[(size_t)wi] && !sh.gate.count(*it)) it = sh.ever.erase(it); else ++it;
    }
    // --shadowportalseed: a portal the body fired before the anchor (State::portalLatch, which the anchor's `portal=` payload fills from the mod's own activatedByPlayer record) is in GD's once-per-attempt
    // map. The rule part of `ever` knows what lies behind the player and the mover erase above drops the movers, so a portal the body fired and has left AHEAD of it (a teleport sent it back: a custom level t=26595,
    // uid 81803 of a twin pair) was fresh again: the twin 81977 fired at 26634 where GD does not.
    if (!levelStart) {
        const float left = slPlayerLeft(sh.img);
        for (const auto& pr : g_shGpUid) {
            if (!s.portalLatch.test(pr.first)) continue;
            const auto it = g_shBase.idx.find(pr.second);
            if (it == g_shBase.idx.end()) continue;
            const gdcol::GObj& G = (g_shBase.mover[it->second] && (it->second >= T.seen.size() || !T.seen[it->second])) ? g_shBase.objs[it->second] : T.world[it->second];   // (a mover this call did not place stands where this thread placed it last, in whichever frame: the level's own object instead, gd-05 11-12)
            if (G.rx + G.rw >= left) sh.ever.insert(pr.second);   // (behind the player: the rule part already says seen)
        }
    }
    if (!levelStart && g_spentPadSeed) {   // the pads the run fired before the anchor (the caller's --spentpad list, GD's activatedByPlayer) are in the once-per-attempt memory, a mover's included
        const float left = slPlayerLeft(sh.img);
        for (int su : g_spentPad) {
            const auto it = g_shBase.idx.find(su);
            if (it == g_shBase.idx.end()) continue;
            const gdcol::GObj& G = (g_shBase.mover[it->second] && (it->second >= T.seen.size() || !T.seen[it->second])) ? g_shBase.objs[it->second] : T.world[it->second];   // (a mover this call did not place stands where this thread placed it last, in whichever frame: the level's own object instead, gd-05 11-12)
            if (G.multiAct) continue;   // (a multi-activation pad is gated by contact, not by the attempt)
            if (G.rx + G.rw >= left) sh.ever.insert(su);
        }
    }
    sl = ShadowLite{};
    if (!levelStart && !(sh.img.u8(po::Sideways) && sh.seedTurnX <= 0.0f)) {
        sl.everW = (sh.img.u8(po::Sideways)) ? sh.seedTurnX : slPlayerLeft(sh.img); sl.flags2 |= kSlEverW;
        // E9 (11-18): a body seeded travelling left (seedMemory above took the objects to its RIGHT as passed) carries the rule the other way round: everWr = the right edge of its rect, kSl3EverRev
        if (!sh.img.u8(po::Sideways) && sh.img.u8(po::Leftward)) { sl.everWr = slPlayerLeft(sh.img) + (sh.img.u8(0x368) ? sh.img.f32(0x2fc) * sh.img.f32(0x488) * sh.img.f32(0x394) : sh.img.f32(0x360)); sl.flags3 |= kSl3EverRev; }
    }   // (--shadowturnedever: a turned frame has the rule of what lay before its turn, or none when the turn is not known)   // (the rule part of ever is off when a run starts at the level start: shadow_state.hpp)
    // --shadowsupply, the repair loop's fixups: a transition record the old tick matched (fixup.hpp applyFixup) is GD's own answer for this tick, so it is applied to the shadow's body before it is packed:
    // a delta sets y / vy (and the ground flag) from the PARENT's, a kill makes the tick fatal. Without this the model's override is overwritten by the shadow's body below and the loop's records
    // never take effect -- the same death at the same place iteration after iteration (lv22 t=3267, the Steam package's 'stuck at x=4125').
    const bool fxApply = g_shadowSupply;
    if (fxApply && t_fixDelta) {
        sh.img.f32(po::PosY, s.y + t_fixDy);
        sh.img.f64(po::Vy, (double)(s.vy + t_fixDvy));
        if (t_fixG != 255) sh.img.u8(po::G, t_fixG ? 1 : 0);
        ++g_shStats.supFixDelta;
    }
    slPack(sh, T.W, sl);
    if (s.action) sl.flags2 |= kSlLastBtn;
    ShadowLayer own;
    own.heightKnown = false;
    // the Free Mode byte as the model's band branch knows it (bands.hpp kBandFreeKnown / kBandFree; unknown: off, as before)
    own.freeMode = (s.bandBranch & kBandFreeKnown) && (s.bandBranch & kBandFree);
    own.dual = g_shadowDual && s.dual;   // (--shadowdual: a dual state seeded from the model; without the flag a dual never reaches here)
    slPackLayer(own, sl);
}

// The anchor's image (--shadowseedfile) for a state at the seed's tick: the game's head image of step T, run through the head of step T with T's edges.
inline void shadowSeedFromFile(ShadowThread& T, const State& s, gdcol::ShadowLite& sl) {
    using namespace gdcol;
    const ShadowBase& B = g_shBase;
    SelfShadow& sh = T.sh;
    sh.img.b = B.seedImg;
    if (B.seedHasSlots)
        for (size_t off : {(size_t)0x5e8, (size_t)0x5f0, (size_t)0x600, (size_t)0x608, (size_t)0x610, (size_t)0x618, (size_t)0x680, (size_t)0x890}) {
            auto it = B.seedSlots.find(off);
            sh.img.put<int64_t>(off, it == B.seedSlots.end() ? 0 : (int64_t)it->second + 1);
        }
    sh.rt.arr = B.seedArr;
    sh.rt.now = B.seedNow;
    sh.usedRings = B.seedUsed;
    for (int k = 1; k <= 3; ++k) sh.holding[k] = B.seedHold.size() >= 3 && B.seedHold[(size_t)k - 1] == '1';
    sh.layerTime = sh.img.f64(po::Time) - sh.stepSeconds;
    sh.dashOn = false; sh.dashFirstDone = false; sh.dashRot = 0.f; sh.dashElapsed = 0.f; sh.dashSpinRate = 0.f;
    sh.speedDirty = false; sh.pendingSpeed = 1.0f;
    sh.frameSteps = 0; sh.prevFrameSteps = 4;
    sh.platformerNoted = true;
    if (!T.W.actIdx.empty()) sh.activatedChanged();
    for (uint32_t i : T.W.actIdx) T.world[i].activated = false;
    T.W.actIdx.clear();
    sh.seedTurnX = 0.0f; sh.seedMemory();
    sl = ShadowLite{};
    slSeedRule(sh, T.W, sl);
    ShadowLayer own;
    if (B.lyHave) {
        own.seed(B.lyFree, B.ly312, B.lyDual, B.ly310, B.ly308, B.lyW764, B.lyZoom, 0.0f, false, B.lyMin, B.lyMax);
        own.seedFlip(B.ly418, B.lyFlip);
    }
    sh.L = shadowLayerFor(own, s, B.seedT);
    SelfIn in; in.dt = t_shDt;
    in.tick = B.seedT;
    in.press1 = (uint8_t)B.seedPress;
    in.release1 = (uint8_t)B.seedRelease;
    SelfOut out;
    sh.stepHead(in, out);
    // --shadowsupply, the repair loop's fixups: a transition record the old tick matched (fixup.hpp applyFixup) is GD's own answer for this tick, so it is applied to the shadow's body before it is packed:
    // a delta sets y / vy (and the ground flag) from the PARENT's, a kill makes the tick fatal. Without this the model's override is overwritten by the shadow's body below and the loop's records
    // never take effect -- the same death at the same place iteration after iteration (lv22 t=3267, the Steam package's 'stuck at x=4125').
    const bool fxApply = g_shadowSupply;
    if (fxApply && t_fixDelta) {
        sh.img.f32(po::PosY, s.y + t_fixDy);
        sh.img.f64(po::Vy, (double)(s.vy + t_fixDvy));
        if (t_fixG != 255) sh.img.u8(po::G, t_fixG ? 1 : 0);
        ++g_shStats.supFixDelta;
    }
    slPack(sh, T.W, sl);
    if (sh.holding[1]) sl.flags2 |= kSlLastBtn;
    slPackLayer(own, sl);
}

// --shadowdual: player 2 of a state into the thread's DualShadow slot, before player 1 is unpacked or seeded (player 1's block then sets the world's `activated`
// flags, which the two players share). From the state's own block (slRef2) when it is exact; at the anchor from the seed file's snap2= (the game's image);
// else from the model's second-half fields (swapHalves, then the seed player 1 gets from the model). Returns 0 = not a dual, 1 = carried, 2 = the seed file,
// 3 = the model, -1 = nothing to seed from. A state that is not a dual leaves a fresh slot (a player 2 born in this tick starts from the template).
inline int shadowDualLoad(ShadowThread& T, const State& s, long long t, bool atAnchor, gdcol::ShadowLite& sl2) {
    using namespace gdcol;
    DualShadow& ds = T.ds;
    SelfShadow& sh = T.sh;
    ds.sh = &sh;
    sh.dualH = nullptr;   // (the seeds' heads below run each player alone; ds.head / ds.tail hang the hooks on again)
    ds.twoPlayer = g_twoPlayer != 0;
    // A body's pass of a two-player level (g_ghostOn: the search serves one body and the other is a ghost): the ghost's death ends nothing (DualShadow::tail).
    // The flag was declared and read there since the two-player shadow (45d35ca5) but never set, so the idle ghost's first spike ended the branch:
    // custom level A's first call died at t=391 (the ghost's spike uid 70, x 525) and custom level B's at t=942, where v0.7.0 passed (a merged plan, masks 1 / 2).
    ds.ghost2 = g_twoPlayer != 0 && g_ghostOn;
    {   // player 2's image starts from the same +0xc48 (the level's, as player 1's template above: the level start stores it into both players)
        const uint8_t sq = g_enablePlayerSqueeze ? 0 : 1;
        if (ds.tmpl.size() > 0xc48) ds.tmpl[0xc48] = sq;
        if (ds.two.img.b.size() > 0xc48) ds.two.img.b[0xc48] = sq;
    }
    {   // back to what PlayerSlot{} with the template image holds, keeping the storage (a fresh slot was four sets and an image allocated every tick)
        PlayerSlot& p = ds.two;
        // A slot reset by the last call that no player 2 was made in since (everBorn, which this function clears, is set by every path that writes the slot:
        // a load below, a birth) holds the template except the 16 bytes DualShadow::tail copies from player 1 at +0x9d8: those alone go back.
        if (T.dualSlotClean && !ds.everBorn && p.img.b.size() == ds.tmpl.size()) {
            std::memcpy(p.img.b.data() + 0x9d8, ds.tmpl.data() + 0x9d8, 16);
            static const bool check = std::getenv("ST_DUALCHECK") != nullptr;   // (=1: the slot must now be the template, byte for byte)
            if (check && (p.img.b != ds.tmpl || !p.rt.arr.empty() || !p.rt.now.empty() || !p.usedRings.empty() || !p.gate.empty() || !p.ever.empty() || !p.atButtons.b.empty() || !p.atEntry.b.empty() || !p.atExit.b.empty() || p.ptButtons.ok || p.ptEntry.ok)) {
                std::fprintf(stderr, "ST_DUALCHECK: player 2's slot is not the template at t=%lld\n", t);
                std::abort();
            }
        } else p.img.b.assign(ds.tmpl.begin(), ds.tmpl.end());
        T.dualSlotClean = true;
        p.rt.arr.clear(); p.rt.now.clear();
        p.usedRings.clear(); p.gate.clear(); p.ever.clear(); p.everRule = SelfShadow::EverRule{};
        p.fireOrder.clear();   // --shadowfireorder: player 2's fired-ring record is the slot's own; the slot is reused across states and threads, so what the last state left must not reach this one (seeded from the State's usedOrb2 / usedOrbOld2 / usedOrbHist2 below)
        for (int k = 0; k < 4; ++k) p.holding[k] = false;
        p.dashRot = 0.0f; p.dashElapsed = 0.0f; p.dashSpinRate = 0.0f;
        p.dashOn = false; p.dashFirstDone = false;
        p.speedDirty = false; p.pendingSpeed = 1.0f;
        p.atButtons.b.clear(); p.atEntry.b.clear(); p.atExit.b.clear(); p.ptButtons = {}; p.ptEntry = {};
    }
    ds.active = false; ds.everBorn = false; ds.born = false; ds.ended = false;
    sh.act2.clear(); sh.p2Phase = false;   // --shadowringp2 (constants.hpp)
    if (!s.dual) return 0;   // (sl2 is not touched: the caller zeroes it if this tick makes a player 2)
    sl2 = ShadowLite{};
    int how = -1;
    const ShadowLite* p = slOf(s.slRef2);
    if (p && (p->flags & kSlValid) && !(p->flags & (kSlOverflow | kSlAmbiguous))) {
        std::memcpy(static_cast<void*>(&sl2), p, sizeof sl2);
        swapPlayer(sh, ds.two);
        slUnpackKeepFire(sl2, g_shBase.tmpl, T.W, sh, (float)s.xAbs - 600.0f, false);   // --shadowfireorder: player 2's block holds no record, and the unpack must not wipe player 1's
        for (int i = 0; i < sl2.nAct; ++i) sh.act2.insert(sl2.act[i]);   // --shadowringp2: player 2's fired set (packed from act2 below)
        swapPlayer(sh, ds.two);
        how = 1;
    } else if (atAnchor && !g_shBase.seedSnap2.empty()) {
        const auto parts = shstep::split(g_shBase.seedSnap2, ';');
        if (parts.size() >= 5) {
            swapPlayer(sh, ds.two);
            sh.img.b = shstep::unhex(parts[0]);
            sh.rt.arr.clear(); sh.rt.now.clear(); sh.usedRings.clear();
            { std::istringstream is(parts[1]); std::string tok; while (is >> tok) { const auto fs = shstep::split(tok, ':'); if (fs.size() >= 4) sh.rt.arr.push_back(std::atoi(fs[0].c_str())); } }
            { std::istringstream is(parts[2]); int u; while (is >> u) sh.usedRings.insert(u); }
            { std::istringstream is(parts[3]); int u; while (is >> u) sh.rt.now.insert(u); }
            for (int k = 1; k <= 3; ++k) sh.holding[k] = parts[4].size() >= 3 && parts[4][(size_t)k - 1] == '1';
            std::map<size_t, int> slots;
            if (parts.size() > 5) { std::istringstream is(parts[5]); std::string tok; while (is >> tok) { const auto fs = shstep::split(tok, ':'); if (fs.size() == 2) { const int u = std::atoi(fs[1].c_str()); if (u >= 0) slots[(size_t)std::strtoull(fs[0].c_str(), nullptr, 16)] = u; } } }
            for (size_t off : {(size_t)0x5e8, (size_t)0x5f0, (size_t)0x600, (size_t)0x608, (size_t)0x610, (size_t)0x618, (size_t)0x680, (size_t)0x890}) { auto it = slots.find(off); sh.img.put<int64_t>(off, it == slots.end() ? 0 : (int64_t)it->second + 1); }
            sh.dashOn = false; sh.dashFirstDone = false; sh.dashRot = 0.f; sh.dashElapsed = 0.f; sh.dashSpinRate = 0.f; sh.speedDirty = false; sh.pendingSpeed = 1.0f;
            sh.seedTurnX = 0.0f; sh.seedMemory();
            sl2.everW = slPlayerLeft(sh.img); sl2.flags2 |= kSlEverW;   // (seedMemory's rule part, without player 1's list of the seed's movers)
            // ...and the other direction of it, as slSeedRule gives player 1 (E9): a second body seeded travelling LEFT (its own +0x9ca; a dual's two bodies need not run the same way) has passed what lies to
            // its RIGHT, not to its left -- everWr = the right edge of its rect, kSl3EverRev. The forward rule alone counted a gravity portal ahead of it (to its left) as already seen (a custom level t=19436).
            if (sh.img.u8(po::Leftward) != 0 && !sh.img.u8(po::Sideways)) {
                sl2.everWr = slPlayerLeft(sh.img) + (sh.img.u8(0x368) ? sh.img.f32(0x2fc) * sh.img.f32(0x488) * sh.img.f32(0x394) : sh.img.f32(0x360));
                sl2.flags3 |= kSl3EverRev;
            } else { sl2.everWr = 0.0f; sl2.flags3 = (uint8_t)(sl2.flags3 & ~kSl3EverRev); }
            {   // ...run through the head of the seed's step with its edges, as player 1's image is (shadowSeedFromFile); player 1's clock words are set after this
                const ShadowBase& B = g_shBase;
                ShadowLayer own;
                if (B.lyHave) { own.seed(B.lyFree, B.ly312, B.lyDual, B.ly310, B.ly308, B.lyW764, B.lyZoom, 0.0f, false, B.lyMin, B.lyMax); own.seedFlip(B.ly418, B.lyFlip); }
                sh.L = shadowLayerFor(own, s, B.seedT);
                sh.layerTime = sh.img.f64(po::Time) - sh.stepSeconds;
                sh.frameSteps = 0; sh.prevFrameSteps = 4;
                SelfIn in; in.dt = t_shDt;
                in.tick = B.seedT;
                in.press1 = (uint8_t)B.seedPress;
                in.release1 = (uint8_t)B.seedRelease;
                SelfOut out;
                sh.stepHead(in, out);
            }
            swapPlayer(sh, ds.two);
            how = 2;
        }
    }
    if (how < 0) {
        State s2 = s;
        swapHalves(s2);
        if (s.frame == 0 && s.dx2 != 0.f) s2.xAbs = s.xAbs + s.dx2;   // --shadowp2dx: player 2 stands dx2 from player 1 (a Teleport trigger moved player 1 alone), not on player 1's x
        if (s.frame == 0) s2.rev = s.rev2;   // --shadowp2dir: the seed's direction (Leftward, which seedMemory's ever reads) is player 2's own: swapHalves leaves rev with player 1
        const double lt = sh.layerTime; const int fs = sh.frameSteps, pf = sh.prevFrameSteps;
        swapPlayer(sh, ds.two);
        shadowSeedFromModel(T, s2, t, sl2, true);
        if (s.frame == 0) sh.img.u8(po::Leftward, s.rev2 ? 1 : 0);   // --shadowp2dir: player 2's own direction (the image is player 2's while swapped in)
        if (s.frame == 0 && s.rev2) {   // ...and its ever's rule part the other way round (E9 slSeedRule's reverse arm): the seed above wrote the forward rule, the direction is set only now
            sl2.everWr = slPlayerLeft(sh.img) + (sh.img.u8(0x368) ? sh.img.f32(0x2fc) * sh.img.f32(0x488) * sh.img.f32(0x394) : sh.img.f32(0x360));
            sl2.flags3 |= kSl3EverRev;
        }
        swapPlayer(sh, ds.two);
        sh.layerTime = lt; sh.frameSteps = fs; sh.prevFrameSteps = pf;
        how = 3;
    }
    if (how != 3 && ds.two.fireOrder.empty()) {   // a loaded block (slUnpackKeepFire) or the snapshot seed holds no record for player 2: it starts from the State's slots, newest first, as player 1's does (shadowAdvanceImpl's seed). how 3 seeded it already (shadowSeedFromModel with the halves swapped)
        const int fired2[5] = {s.usedOrb2 ? s.usedOrb2->uid : -1, s.usedOrbOld2, s.usedOrbHist2[0], s.usedOrbHist2[1], s.usedOrbHist2[2]};
        for (int u : fired2) if (u >= 0) ds.two.fireOrder.push_back(u);
    }
    {   // the second body's gravity factor (+0xb8c) is the last x-crossing gravity trigger that writes player 2
        const double x2 = crossXOf((double)s.xAbs, g_tpBack ? s.tpBack : 0);
        double v2 = gravMod2At(x2);
        if (g_seedGmod2Known) {   // the anchor said what the second body's factor is; it stands until a player-2 x-crossing trigger past the anchor's x takes over
            bool crossed = false; double m = 1.0;
            for (const auto& g : g_gravTrigs2) { if (g.cx > x2) break; if (g.cx > g_seedGmod2X) { m = g.mod; crossed = true; } }
            v2 = crossed ? m : (double)g_seedGmod2Milli / 1000.0;
        }
        ds.two.img.f32(0xb8c, (float)v2);
    }
    if (g_fixGravityBug != 0) ds.two.img.u8(0x561, 1);   // --shadowp2fixgrav (constants.hpp): the level's fixGravityBug is on both players
    ds.active = true; ds.everBorn = true;
    return how;
}

// SHSTEP_PROF=1 (stderr-free: printed with the counts): where a stepped tick's time goes, in thread-ns per part -- 0 the model's own tick, 1 the movers,
// 2 the block out of the store and unpacked (and a seed), 3 the tail of step t, 4 the layer words and the head of step t+1, 5 the block packed, 6 the block
// put into the store, 7 --shadowsupply's stages after the put (the touch boxes, tpBack, gravTouch, ctrlT, the lock offset), 8 --shadowdual's player-2 load,
// 9 what shadowAdvanceImpl does after the stepped tick's own clock (g_shStats.ns) has stopped, up to its return (the slots, the State's fields read back).
// (The sums are per thread and added up at the print, as SSPROF's are: selfshadow.hpp SsTl.)
struct ShadowProf {
    bool on = std::getenv("SHSTEP_PROF") != nullptr;
};
inline ShadowProf g_shProf;
struct ShProfTl { long long ns[10] = {}; };
struct ShProfReg { std::mutex m; std::vector<ShProfTl*> all; };
inline ShProfReg& shProfReg() { static ShProfReg r; return r; }
inline ShProfTl& shProfTl() {
    static thread_local ShProfTl* t = [] { ShProfTl* p = new ShProfTl(); ShProfReg& r = shProfReg(); std::lock_guard<std::mutex> lk(r.m); r.all.push_back(p); return p; }();
    return *t;
}
inline ShProfTl shProfSum() { ShProfTl s; ShProfReg& r = shProfReg(); std::lock_guard<std::mutex> lk(r.m); for (const ShProfTl* p : r.all) for (int k = 0; k < 10; ++k) s.ns[k] += p->ns[k]; return s; }
struct ShadowProfMark {
    bool on;
    std::chrono::steady_clock::time_point last;
    explicit ShadowProfMark(bool o) : on(o) { if (on) last = std::chrono::steady_clock::now(); }
    void operator()(int k) {
        if (!on) return;
        const auto n = std::chrono::steady_clock::now();
        shProfTl().ns[k] += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(n - last).count();
        last = n;
    }
};

// the Options event the shadow's head takes when the gate changed: 1 = disable (SelfIn::ctrl), 2 = enable, 0 = none
inline int ctrlEdgeOf(bool wasOff, bool nowOff) { return nowOff == wasOff ? 0 : (nowOff ? 1 : 2); }

// --shadowsupply, tpBack: the table entry of a teleport's source -- a portal's or an orb's by uid, a Teleport trigger's on the trigger itself (TpTrig::backIdx)
inline int shadowTpBackIdx(int uid) {
    const int i = tpBackObjIdx(uid);
    if (i >= 0) return i;
    for (const TpTrig& t : g_tpTrigs) if (t.uid == uid) return t.backIdx;
    return -1;
}

// SHSTEP_DEBUG_FILE=path: the SHSTEP_DEBUG lines go to that file (appended, unbuffered) instead of stderr -- quick_regress and the section harnesses drop leveldp's stderr
inline FILE* shDebugOut() {
    static FILE* f = [] { const char* p = std::getenv("SHSTEP_DEBUG_FILE"); FILE* h = p ? std::fopen(p, "a") : nullptr; if (h) std::setvbuf(h, nullptr, _IONBF, 0); return h ? h : stderr; }();
    return f;
}
// SHSTEP_DEBUG=t0,t1, read once: the range every debug print below tests. (std::getenv on the per-tick path -- the print at the end of a stepped tick, the declines -- took the C runtime's
// environment lock on every tick of every thread: most of a 14 us tick, from ee7dfb05 on.)
inline bool shDebugAt(long long t) {
    static const std::pair<long long, long long> r = [] { long long lo = -1, hi = -2; if (const char* e = std::getenv("SHSTEP_DEBUG")) std::sscanf(e, "%lld,%lld", &lo, &hi); return std::make_pair(lo, hi); }();
    return t >= r.first && t <= r.second;
}
// The tick. there is no old tick behind it (E9, 11-18): shadowAdvance steps the tick itself.
inline thread_local bool t_shMemoHit = false;   // (SHSTEP_MEMOCHECK: the last shadowAdvanceImpl took the tail memo)
// --shadowrotapply: the body the pass left after a touch Rotate Gameplay turned it, held for the caller. The old tick's turn (applyRotation) runs AFTER shadowAdvance, in the caller, and does the queue's
// bookkeeping (the spent slot, the channel, the reverse bits, the State's frame / flip / rev and everything it resets); shadowRotFinish, called right after it, puts this body over the old tick's if the
// old tick turned with the same trigger and the three direction bytes agree with the State it left, and otherwise drops the shadow's block (the old tick's turn stands, as it always did).
struct ShadowRotHold {
    bool valid = false;
    long long t = 0;
    uint64_t bits = 0;                 // the queue slots (g_rotQ) of the triggers the pass turned the body with
    float x = 0, y = 0;                // the body, world coordinates
    double vy = 0;
    uint8_t up = 0, left = 0, side = 0, grounded = 0, mini = 0, boost = 0, dashing = 0;
    int mode = 0;
    float rot = 0, dx = 0;
    float rotStep = 0;                 // the ball's spin stake (shBallStakeOf)
    float dashSlope = 0;               // the dash's line (+0x578)
};
// The ball's spin stake in the State's terms (State::rotStep, degrees a tick): updateRotation 0x4ef390 spends dt / 60 * +0x728 a step, times +0x72c while +0x670 is up (a slope's launch), and a tick is
// 1/240 s. A pass's body writes it back with the angle, as the old tick did until it was removed: the stake lives in the image, an exact block carries the image, and a block that is not exact (a
// list of it ran out of room) is seeded from the State, which then had no stake - a ball flipped off the ground stood at its angle until its next stake, and its oriented box was not GD's.
// a custom level t=11512..11533: the ball GD turns 3.2737 a tick stood still for 22 ticks, ran 72 degrees behind, and met a gravity portal turned 45 degrees (uid 195056, the box 50 x 75) a tick after
// GD's (t=11553 against 11552). [M] w99, the y at t=11549 moved by dy: GD takes the portal at 11552 for dy <= +0.3 and at 11553 from +0.6, the ball's box turned by its own angle (an upright 30 x 30
// would take it up to dy = +2.47).
inline float shBallStakeOf(const gdcol::Mem& P) {
    const float w = P.f32(0x728);
    if (w == 0.0f) return 0.0f;
    return std::fabs(w * (P.u8(0x670) ? P.f32(0x72c) : 1.0f)) / 240.0f;
}
inline thread_local ShadowRotHold t_shRotHold;
// The band a Zoom / Free Mode trigger wrote this tick (the block before writeBody): the old tick's turn (applyRotation, called after shadowAdvance) resets the State's band, so a turning tick would lose
// it; shadowRotFinish puts it back (GD's rotation does not touch the band words: a custom level t=4799, the frame 3 -> 0 tick that also fires a Zoom trigger).
struct ShadowTrigBand { bool valid = false; float f = 0.f, c = 0.f; };
inline thread_local ShadowTrigBand t_shTrigBand;
// P9 portal_state (7.22): armT = GD's ceiling-arm counter +0xb84 (the old clock saturates at kArmTicks where the counter is 0), fgArm = the "Special" 2866's counter +0xb88, portalLatch = the carried latch plus the
// gravity portals the shadow's pass fired fresh on this tick (outs[1] is the tail step's record; 1000 added to the type when the once-only gate let it act). One body.
inline void portalStateDerive(const State& s, const ShadowThread& T, const gdcol::SelfShadow& sh, State& d, long long tickForDbg = -1) {
    const gdcol::Mem& P = sh.img;
    const int a = P.i32(0xb84), g = P.i32(0xb88);
    d.armT = a > 0 ? (uint8_t)std::max(0, kArmTicks - a) : (uint8_t)kArmTicks;
    d.fgArm = g > 0 ? (uint8_t)kArmTicks : (uint8_t)0;
    // ...and slideT = the dart-slide arm's counter +0xb80 (id 1755), the same clock as armT. [R] The seed of a body turns State::slideT back into +0xb80 (shadowSeedFromModel: kArmTicks - slideT while
    // slideT < kArmTicks, else 0); the old tick wrote it on a touch and nothing has since its removal, so a body seeded from the State inside the arm's window (a block that is not exact) had the counter
    // at 0 whatever the pass's image held. A State that never met the arm keeps the 255 it was built with (the same "not armed" to the seed as kArmTicks): only a clock that ran is written.
    {
        const int b = P.i32(0xb80);
        if (b > 0) d.slideT = (uint8_t)std::max(0, kArmTicks - b);
        else if (d.slideT < (uint8_t)kArmTicks) d.slideT = (uint8_t)kArmTicks;
    }
    GravLatch L = s.portalLatch;
    for (const auto& pr : g_shGpUid) for (const auto& fr : T.outs[1].fired) if (fr.first >= 1000 && fr.second == pr.second) L.set(pr.first);
    // --slopedbg: one line per gravity portal the pass reached on this tick -- the bit, whether the latch already held it
    // (a record without the +1000 the once-only gate adds), and the mask coming in (py/test_gravity_portal_bit_share.py reads it).
    if (g_slopeDbg && tickForDbg >= 0)
        for (const auto& pr : g_shGpUid) for (const auto& fr : T.outs[1].fired)
            if ((fr.first >= 1000 || s.portalLatch.test(pr.first)) && fr.second == pr.second)
                std::printf("gplatch half=0 t=%lld uid=%d bit=%d spent=%d mask=%s\n", tickForDbg, pr.second, pr.first, fr.first >= 1000 ? 0 : 1, gravLatchHex(s.portalLatch).c_str());
    d.portalLatch = L;
}
// P9 band_timer (7.23): the old tick steps the ground band's tween clock every tick when the level has a static camera and restarts it on a mode portal; the shadow layer's updateDualGround says the same event
inline uint8_t bandTimerDerive(const State& s, const gdcol::ShadowLayer& L) {
    uint8_t a = g_staticCamCol ? bandAnimStep(s.bandAnim) : s.bandAnim;
    if (L.animEvent) a = bandAnimStart(L.animIn);
    return a;
}
// P9 portal family (7.21): the grace clocks from the finished state: gravity flipped on this tick (gdUpOf) -> 0, mode changed -> 0, else the clock up one, saturated (the removed step.hpp, end of the old tick's stepOne)
inline void portalDeriveClocks(const State& s, const State& c, uint8_t& flipT, uint8_t& modeT) {
    flipT = (gdUpOf(c) != gdUpOf(s)) ? 0 : (uint8_t)std::min<int>(kFlipGraceTicks, (int)s.flipT + 1);
    modeT = (c.mode != s.mode) ? 0 : (uint8_t)std::min<int>(kFlipGraceTicks, (int)s.modeT + 1);
}
// The level's object of a uid (the world frame's lists: Model::objLists), for a State pointer the old tick would have found in its window
inline const Obj* shadowObjByUid(int uid, int frame = 0) {
    // The cache is keyed by the Level AND by the generation of g_baseLv: cliMainOnce builds a Level on its own stack, so the capladder's second attempt (and every later cliMainOnce of a process, the mod's included)
    // has a Level at the same address as the last one while its vectors are new; keyed by the address alone the map kept pointers into the freed ones (an access violation in shadowAdvanceImpl's `c.usedOrb->uid`, 1 run in 2 on lv22's it=2 call).
    static std::mutex mu; static const Level* lvC[4] = {nullptr, nullptr, nullptr, nullptr}; static unsigned long long lvG[4] = {0, 0, 0, 0}; static std::unordered_map<int, const Obj*> map[4];
    std::lock_guard<std::mutex> lk(mu);
    const int f = frame & 3;
    const Level* lv = levelOfFrame(f);
    if (!lv) return nullptr;
    if (lv != lvC[f] || lvG[f] != g_levelGen) {
        lvC[f] = lv; lvG[f] = g_levelGen; map[f].clear();
        const std::vector<Obj>* ls[8] = {&lv->objs, &lv->portals, &lv->pads, &lv->orbs, &lv->speeds, &lv->slopes, &lv->coins, &lv->dyn.objs};
        for (const auto* v : ls) for (const Obj& o : *v) map[f].emplace(o.uid, &o);
    }
    const auto it = map[f].find(uid);
    return it == map[f].end() ? nullptr : it->second;
}
// The game events the shadow's pass raised, in the order the game raises them (game_event.hpp): a pad that fresh-bumped gives its type's event, then PadActivated; a ring that fired (a record entry the pass added) gives
// OrbActivated, then its type's event. Player 1's body only (a second player's events are not read).
inline void eventsOfPass(const gdcol::SelfShadow& sh, const std::vector<std::pair<int, int>>& fired, std::vector<gdev::EvTok>& out) {
    out.clear();
    for (const auto& f : fired)
        if (f.first >= 1000 && gdev::isPadType(f.first - 1000)) {
            const int ty = f.first - 1000;
            out.push_back({gdev::gdEventOfType(ty), f.second, ty});
            out.push_back({gdev::kPadActivated, f.second, ty});
        }
    const std::vector<int>& F = sh.fireOrder;
    size_t added = 0;
    for (; added < F.size(); ++added) if (!t_evPreFire.empty() && F[added] == t_evPreFire[0]) break;
    for (size_t i = added; i-- > 0;) {   // the oldest of the new rings first
        const Obj* ro = shadowObjByUid(F[i]);
        const int ty = ro ? (int)ro->type : -1;
        out.push_back({gdev::kOrbActivated, F[i], ty});
        out.push_back({gdev::gdEventOfType(ty), F[i], ty});
    }
}
// (defined below, with the families that use them; shadowAdvanceImpl calls them on a State that no longer depends on the template parameter, so they must be declared before it)
inline void collisionEventsDerive(const State& s, const State& c, const gdcol::Mem& P, const ShadowThread& T, State& d);
inline void collisionEventsDerive2(const State& s, const State& c, const gdcol::Mem& P, State& d);
inline void inputHsDerive(const State& c, const gdcol::Mem& P, bool second, State& d);
inline void staleDerive(const State& s, const State& c, const gdcol::SelfShadow& sh, State& d);
// ...and the same derivation into anything that has the three members it writes (revOff, spiderJumpT, rHover): the stepped tick takes them into a small
// struct, not into a copy of the whole State (two 736-byte copies a tick before 2026-11-18)
struct StaleBooks { decltype(State::revOff) revOff{}; decltype(State::spiderJumpT) spiderJumpT{}; decltype(State::rHover) rHover{}; };
template <class Out> inline void staleDeriveTo(const State& s, const State& c, const gdcol::SelfShadow& sh, Out& d);
inline void probeDumpLineage(const State& s, int input, long long t, int why, int uid);
inline State shadowAdvanceImpl(const State& s, int input, const StepCtx& K, bool& dead, int input2) {
    using namespace gdcol;
    t_shMemoHit = false;
    t_bodyFiredOut.valid = false;   // (set again where the pass's fired rings are known; a step that returns before that leaves the tog update its own test)
    t_shRotHold.valid = false;
    t_shTrigBand.valid = false;
    t_rotBookOn = false; t_rotBookBits = 0;
    ShadowProfMark pm(g_shProf.on);
    // a two-player level (levelSettings twoPlayer): handleButton hands player 1's button to player 1 only and player 2's to player 2 only; the second body's level is the ghost's plan in a search
    // (fixup.hpp's rule, as the old tick took it), else the caller's, else the replay's. -1 in a one-player level: the dual's player 2 takes player 1's edge.
    // (-1: nothing says what player 2 presses -- a search with no ghost, --p2shared (removed): the plan's one button goes to BOTH bodies, as the old tick's `p2in = input`: player 2 takes player 1's edge)
    const int in2e = !g_twoPlayer ? -1 : g_ghostOn ? ghostLevelAt((long long)K.t) : input2 >= 0 ? input2 : g_p2ReplayLvl.empty() ? -1 : secondLevelAt((long long)K.t);
    {   // no old tick has kept this tick's touch words: the touch base is the State before the tick, valid from s, with the y before the tick
        t_trigBefore.valid = true; t_trigBefore.trig = s.trig; t_trigBefore.trigT = s.trigT; std::memcpy(t_trigBefore.fireB, s.fireB, sizeof(s.fireB)); t_trigBefore.preY = (double)s.y;
    }
    {   // the button gates the old tick held (its "no-control window"; the old tick is gone and this is now their only copy), for the shadow's supply code that reads their results (t_inputEff, t_ctrlGate*, the touch-box test's ran / applied flags)
        // (always on: the switch --ctrlwavenow is removed)
        // --ctrlwavenow (removed): ...and a wave's button with no lag. A tick in GD runs update, collisions, the crossing
        // queue (where an Options trigger raises the flag) and then the buttons, so a button on the tick the flag goes up
        // finds it raised. The model's wave turns on the button the state came in with (the tick after), i.e. its input at
        // t is GD's button at the end of t; the ship's thrust, which --ctrlwinlag measured, takes this tick's input and so
        // GD's button of the tick before. The custom level of --ctrlwintrig (removed), its window raised at 13,404: a wave pressed
        // on 13,404 does not climb on 13,405 in GD, where the model climbed with the recorded window passed as well.
        const bool wave = s.mode == 4;
        auto gateFor = [&](bool half2, int in, bool& g) {
            g = false;
            if (g_stopJump && s.pcRel && in) in = 0;
            if (!wave && ctrlBodyInputOffAt(K.t, half2)) { in = 0; g = true; }
            if (!wave && !half2 && g_rotTrig.empty() && ctrlTrigInputOff(K.t, K.x, K.xPrev)) { in = 0; g = true; }
            if (!wave && !half2 && s.frame == 0 && !s.rev && ctrlWinXInputOff(K.t, K.xPrev)) { in = 0; g = true; }
            if (wave && !half2 && s.frame == 0 && !s.rev && ctrlWinXOffNow(K.t, K.x)) { in = 0; g = true; }
            if (wave && ctrlOffNowAt(K.t, half2, K.x)) { in = 0; g = true; }
            uint8_t ctrlTNow = s.ctrlT;   // as step.hpp's -- a wave reads the flag as this tick's move leaves it (frames.hpp ctrlChanCrossed)
            if (wave && !half2 && !g_rotTrig.empty() && s.frame == 0 && !s.dual) { const int cr = ctrlChanCrossed(K.xPrev, K.x, (int)s.rotChan, s.rev != 0); if (cr >= 0) ctrlTNow = (uint8_t)cr; }
            if (ctrlTNow && !half2) { in = 0; g = true; }
            return in;
        };
        bool g1 = false, g2 = false;
        const int in1 = gateFor(false, input, g1);
        int inLast = in1; bool gLast = g1;
        if (s.dual && g_shadowDual) {
            const int r2 = gateFor(true, in2e >= 0 ? in2e : input, g2);
            if (in2e < 0) { inLast = r2; gLast = g2; }   // (a body with its own button: player 1's own result stands in t_inputEff / t_ctrlGateNow, player 2's is t_ctrlGateP2)
            t_ctrlGateP2 = g2;
        }
        t_ctrlGateP1 = g1; t_ctrlGateNow = gLast; t_inputEff = inLast; t_inputEffValid = true;
        t_ctrlLoopRan = !g_ctrlTouchBoxes.empty() && s.frame == 0; t_ctrlBoxApplied = false; t_ctrlPreBox = s.ctrlT;
        { t_gravLoopRan = g_gravTouch && !g_gravTouchBoxes.empty() && s.frame == 0 && !s.dual; t_gravBoxApplied = 0; t_gravPreBox = s.gravTouch; t_gravEvent = 0; }   // --shadowgravbox (constants.hpp): the gravity touch-box test's flags (step.hpp "--gravtouch"), for the supply code below
    }
    State c = s;
    {   // State::prevWX / prevWY: this tick's update starts where s stands (in the game's frame); the child's seed reads it as PrevX / PrevY
        double pwx = (double)s.xAbs, pwy = (double)s.y;
        if (const int pf = shFrameOf(s)) fromFrame(pf, (double)s.xAbs, (double)s.y, pwx, pwy);
        c.prevWX = (float)pwx; c.prevWY = (float)pwy;
    }
    // applyFixup is called by the old tick (stepBoth) and by nothing else, so on a tick the shadow takes alone the loop's records were never looked up, and the
    // thread_locals the supply below reads (t_fixKill / t_fixDelta) kept whatever the last old tick this thread ran had left. A search with --shadowstep=all therefore ignored every kill
    // and delta record: lv21 t=4534, the same death five iterations in a row (fixupcall f0=0 over a 423,568-state call; with the old tick f0=4,956 and the frontier died at the record).
    // The same lookup, with the same gate (no record next to moving geometry), for a single body; a dual still waits for the old tick's own path.
    {
        t_fixIn2 = -1; t_fixKill = false; t_fixDelta = false; g_fxBlockedUid = -1;
        if (!g_fixups.empty() && !s.dual) {
            const Obj* dyn = nearDynObjectPtr(s, K);
            if (!dyn) { State fc = s; bool fd = false; applyFixup(s, input, fc, fd, true, &K); }
            else if (findFixup(g_fixupKills, s, input) || findFixup(g_fixupDeltas, s, input)) g_fxBlockedUid = dyn->uid;
            if (g_fxWatchT >= 0 && K.t >= g_fxWatchT - 1 && K.t <= g_fxWatchT + 1)   // --fxwatch <t>: this thread's lookup at t-1..t+1 (print only)
                std::fprintf(stderr, "fxshadow: Kt=%lld xAbs=%.4f y=%.4f vy=%.4f in=%d mode=%d flip=%d g=%d nearDyn=%d kill=%d delta=%d\n", (long long)K.t, (double)s.xAbs, (double)s.y, (double)s.vy, input, (int)s.mode, (int)s.flip, (int)s.grounded, dyn ? 1 : 0, (int)t_fixKill, (int)t_fixDelta);
        }
    }
    // E9 (11-18): no tick goes back to the old tick any more (the old tick, stepOne / stepBoth, has since left the source). A state the shadow cannot step ends its branch (the tick is dead) and is
    // counted by kind in g_shUnsup, printed once per kind, so a limit shows as a number and not as a quiet wrong answer.
    pm(0);
    auto unsupported = [&](int kind) {
        ++g_shUnsup[kind];
        static std::atomic<unsigned> told{0};
        if (!(told.fetch_or(1u << kind) & (1u << kind))) std::printf("shadowstep: UNSUPPORTED state (%s) at t=%lld: this branch ends here (no old tick to hand it to)\n", kShUnsupName[kind], (long long)K.t);
        if (shDebugAt(K.t)) std::fprintf(shDebugOut(), "shstep t=%lld UNSUPPORTED (%s)\n", K.t, kShUnsupName[kind]);
        c.slRef = 0; c.slRef2 = 0; dead = true;
        return c;
    };
    if (!g_shBase.ok) return unsupported(0);
    ++g_shStats.eligible;
    if (g_shadowSDiff) {   // observation only
        ShadowSDiff& Q = g_shSd;
        ++Q.n;
        if (s.dual != c.dual) ++Q.dual;
        if (s.rev != c.rev) ++Q.rev;
        if (s.frame != c.frame) ++Q.frame;
        if (s.rotChan != c.rotChan) ++Q.rotChan;
        if (s.mode != c.mode) ++Q.mode;
        if (s.flip != c.flip) ++Q.flip;
        if (s.mini != c.mini) ++Q.mini;
        const bool dualOnQ = g_shadowDual;
        auto declOf = [&](uint8_t dual, uint8_t rev, uint8_t frame) {
            const bool revD = (s.rev || rev) && (s.dual || dual);
            const bool frameStepQ = s.frame != 0 && s.frame == frame && (s.frame == 1 || s.frame == 3) && !s.dual && !dual;
            const bool frameD = (s.frame != 0 || frame != 0) && !frameStepQ;
            return ((s.dual || dual) && !dualOnQ) || frameD || revD || g_platformerMode;
        };
        const bool dC = declOf(c.dual, c.rev, c.frame), dS = declOf(s.dual, s.rev, s.frame);
        if (dC) ++Q.eligC;
        if (dS) ++Q.eligS;
        if (dC != dS) { ++Q.eligDiff; if (dC) ++Q.eligCnotS; else ++Q.eligSnotC; }
    }
    // --shadowdual: a dual of a one-player level is stepped with its player 2 (selfshadow_dual.hpp). Still declined: a two-player level (its own buttons and the
    // birth's releaseAllButtons are not ported). Players in modes of different latency (a ship and a cube) are not declined: handleButton hands the edge to both
    // in the same step, so player 2's head runs where player 1's does (the plan schedule's latency is the first player's). Declining them left the next tick to
    // a model seed: o_lv16_13210 t=13499, a ship's vy 0.04 off GD after one.
    // (P9; was --shadowfroms, folded in): the direction / dual / frame bytes the eligibility and the control gates look at are the state BEFORE the tick (s), not what the old tick left (c): the shadow then does not read the old tick for these decisions (a tick the old tick turns, starts or ends a dual in is stepped as s says; the pass's own portals and Rotate Gameplay do the rest)
    const uint8_t cDual = s.dual, cRev = s.rev, cFrame = s.frame;
    const int cRotChan = (int)s.rotChan;
    const bool dualOn = g_shadowDual;
    const bool revDecl = (s.rev || cRev) && (s.dual || cDual);   // one player in reverse travel is stepped
    // --shadowframe: a tick in a turned frame (State::frame 1 or 3) is stepped when the old tick left it in the same frame (a frame change is the old model's) and nobody is dual
    const bool frameStep = s.frame != 0 && s.frame == cFrame && (s.frame == 1 || s.frame == 3) && !s.dual && !cDual;
    const bool frameDecl = (s.frame != 0 || cFrame != 0) && !frameStep;
    if (((s.dual || cDual) && !dualOn) || frameDecl || revDecl || g_platformerMode)
        return unsupported(((s.dual || cDual) && !dualOn) ? 3 : frameDecl ? 1 : revDecl ? 2 : 7);   // (a two-player level and a platformer are refused at the start, cli.hpp)
    // The turned frame (--shadowframe): the model keeps (travel u, lateral v) of its frame, the game the world position and the three direction bytes. frames.hpp: frame 1 travels -Y with up = +X, frame 3 travels +Y with
    // up = -X; the gravity byte of a frame 3 is the opposite of the State's flip (search_key.hpp gdUpOf), and a frame 1 run to the right (rev 0) is Leftward in the game's update (it moves y by -speed).
    const int fr = frameStep ? (int)s.frame : 0;
    auto gdUp = [&](uint8_t flip) -> uint8_t { return shGdUp(fr, flip); };
    auto gdLeft = [&](uint8_t rev) -> uint8_t { return shGdLeft(fr, rev); };
    auto toWorld = [&](double u, double v, double& X, double& Y) { if (fr) fromFrame(fr, u, v, X, Y); else { X = u; Y = v; } };
    auto toFrameXY = [&](double X, double Y, double& u, double& v) { if (fr) toFrame(fr, X, Y, u, v); else { u = X; v = Y; } };
    // --shadowwarp (removed): the step dt and the layer clock of this tick (a warp below 1 scales both; see constants.hpp); every SelfIn below takes t_shDt
    const double twNow = timeWarpAt((double)s.xAbs);
    t_shDt = (twNow != 1.0) ? (float)(0.25 * (double)(float)twNow) : 0.25f;
    t_shSec = (twNow != 1.0) ? ((1.0 / 60.0) * (double)(float)twNow) / 4.0 : (1.0 / 60.0) / 4.0;
    const auto t0c = std::chrono::steady_clock::now();
    ShadowThread& T = shadowThread();
    SelfShadow& sh = T.sh;
    sh.stepSeconds = t_shSec;
    shadowPlaceMovers(T, K, fr);
    shadowApplyTapMoves(T, s);
    static const char* const shDbgUid = std::getenv("SHDBG_UID");   // the environment is read once: getenv takes the CRT's environment lock, and a call per step serialised every search thread (gd-05 11-12)
    if (const char* ue = shDbgUid) if (shDebugAt(K.t)) { const int u = std::atoi(ue); const long long ix = shadowIdxOf(T, u); if (ix >= 0) std::fprintf(shDebugOut(), "shstep t=%lld mover uid %d: act %d type %d rx %.3f rw %.3f cx %.4f cy %.4f prevCy %.4f ry %.4f rh %.2f inactive %d\n", K.t, u, (int)T.world[(size_t)ix].activated, (int)T.world[(size_t)ix].type, (double)T.world[(size_t)ix].rx, (double)T.world[(size_t)ix].rw, (double)T.world[(size_t)ix].cx, (double)T.world[(size_t)ix].cy, (double)T.world[(size_t)ix].prevCy, (double)T.world[(size_t)ix].ry, (double)T.world[(size_t)ix].rh, (int)T.world[(size_t)ix].inactive); }
    if (const char* ue2 = shDbgUid) if (shDebugAt(K.t)) { const int u2 = std::atoi(ue2); for (const std::vector<const Obj*>* lp : {K.near, K.ports, K.pads, K.orbs, K.slopes, K.speeds}) { if (!lp) continue; for (const Obj* o : *lp) if (o && o->uid == u2) std::fprintf(shDebugOut(), "shstep t=%lld model Obj uid %d: dyn %d recOn %d onNew %d onPrev %d cx %.3f cy %.3f hw %.3f hh %.3f\n", K.t, u2, (int)o->dynObj, (int)o->recOn, (int)o->onNew, (int)o->onPrev, (double)o->cx, (double)o->cy, (double)o->hw, (double)o->hh); } }
    if (fr && T.frameBad) return unsupported(4);
    // The block the player last stood on (GD's m_objectSnappedTo) is a pointer in the game and reads wherever the block is now; in the shadow's world a mover outside the model's windows
    // keeps the place it was last given. checkSnapJumpToObject compares the landing block with it (+-1 px in x): a custom level t=5542, the block 133 px behind that had moved 4 px since, so
    // the stride test missed by the 4 px and the nudge GD makes never came. The model's Obj for it holds the current place.
    if (s.snapObj) {
        const long long ix = shadowIdxOf(T, s.snapObj->uid);
        if (ix >= 0 && g_shBase.mover[(size_t)ix]) {
            gdcol::GObj& G = T.world[(size_t)ix];
            const double pdx = s.snapObj->dcx;   // --shadowdcx
            double wcx = s.snapObj->cx, wcy = s.snapObj->cy, wpx = s.snapObj->cx - pdx, wpy = s.snapObj->cy - s.snapObj->dcy;
            if (fr) { fromFrame(fr, s.snapObj->cx, s.snapObj->cy, wcx, wcy); fromFrame(fr, s.snapObj->cx - pdx, s.snapObj->cy - s.snapObj->dcy, wpx, wpy); }   // (the model's Obj is in the turned frame's coordinates)
            const float cx = (float)wcx, cy = (float)wcy;
            if (G.cx != cx || G.cy != cy) {
                G.cx = cx; G.cy = cy; G.prevCx = (float)wpx; G.prevCy = (float)wpy;
                G.rx = cx - G.rw * 0.5f; G.ry = cy - G.rh * 0.5f;
                T.sh.moverMoved((size_t)ix);
                T.gen = 0;   // the world is no longer this group's placement: the next state on this thread places it again (it saw this state's block where the group's stood, and which state a thread stepped first is the schedule's)
            }
        }
    }
    // --shadowridebox: an arm box that rides the player stands on this state's body (or is off once the state's arm has lapsed), not at the recording's row (constants.hpp). The window and the
    // freshness are the old tick's own (step.hpp: stage by the row the tick writes, K.t + 1; fgArm == kArmTicks / armT == 0 = renewed on the previous tick).
    if (!fr) {
        auto rideBox = [&](int uid, bool fresh) {
            const long long ix = shadowIdxOf(T, uid);
            if (ix < 0 || !g_shBase.mover[(size_t)ix]) return;
            gdcol::GObj& G = T.world[(size_t)ix];
            const float cx = fresh ? s.xAbs : G.cx, cy = fresh ? (float)s.y : G.cy;
            if (G.cx == cx && G.cy == cy && G.inactive == !fresh) return;
            G.cx = cx; G.cy = cy; G.prevCx = cx; G.prevCy = cy;
            G.rx = cx - G.rw * 0.5f; G.ry = cy - G.rh * 0.5f;
            G.inactive = !fresh;
            T.sh.worldChanged();
            T.sh.moverMoved((size_t)ix);
            T.gen = 0;   // the world is no longer this group's placement (as the snapped block above)
        };
        for (const FlipHeadBox& b : g_flipHeadBoxes) if (rideStage(b, (int)K.t + 1) == 1) rideBox(b.uid, s.fgArm == (uint8_t)kArmTicks);
        for (const FlipHeadBox& b : g_nojumpBoxes) if (rideStage(b, (int)K.t + 1) == 1) rideBox(b.uid, true);   // --shadownajbox: the 1813 has no State arm; inside its ride it is where the body is
        for (const ArmBox& b : g_armBoxes) if (rideStage(b, (int)K.t + 1) == 1) rideBox(b.uid, s.armT == 0);
    }
    // --shadowfollow (constants.hpp): the pass's plan from the State before the tick and the windows (step.hpp followPassPlan). No old tick run: it is the follow pass -- t_followPlaced and, below, State::followF / followOk / followOwn / followN. The old tick run:
    // the same plan is compared with the old tick's own, and nothing changes.
    float folNew[kFollowYSlots] = {s.followF[0], s.followF[1]}; uint8_t folOk = s.followOk; bool folPlan = false;
    // (in a dual: nobody else puts the follow / big-section objects where the pass does -- t_followPlaced stayed empty and every one of them was switched off: a custom level t=18261, the dual-off portal 16955, a --bigsection object.
    //  The plan is the first body's pass, which is where the old tick's stepBoth runs it.)
    {
        static thread_local std::vector<FollowPlaced> plan;
        followPassPlan(s, K, false, folNew, folOk, plan);
        folPlan = true;
        t_followPlaced = plan;
    }    // --shadowsupply, Follow Player Y: the follow objects of this state, where the old step's follow pass put them (placed by a running command at the state's own follow height, or sent away: far cell)
    if (g_shadowSupply)
        for (const FollowPlaced& fp : t_followPlaced) {
            const long long ix = shadowIdxOf(T, fp.uid);
            if (ix < 0 || !g_shBase.mover[(size_t)ix]) { ++g_shStats.supFolNonMover; continue; }
            gdcol::GObj& G = T.world[(size_t)ix];
            if (fp.far && !(!fr && g_shBase.bigSec[(size_t)ix])) { G.inactive = true; G.farHold = true; T.sh.moverWritten((size_t)ix); ++g_shStats.supFolFar; continue; }   // --shadowbigsecpost (constants.hpp): a far collider of --bigsection stays in the world at the windows' place; the section index decides from the position after the update   // (--shadowspiderrect: the object stays where the windows put it, which is its place)
            if (fp.slope) ++g_shStats.supFolSlope;
            if (fr) {   // (the windows' own place, in the frame's coordinates)
                double wcx, wcy, wpx, wpy;
                fromFrame(fr, fp.cx, fp.cy, wcx, wcy); fromFrame(fr, fp.cx, fp.cy - fp.dcy, wpx, wpy);
                const bool mv = G.cx != (float)wcx || G.cy != (float)wcy;
                G.cx = (float)wcx; G.cy = (float)wcy; G.prevCx = (float)wpx; G.prevCy = (float)wpy;
                G.rx = G.cx - G.rw * 0.5f; G.ry = G.cy - G.rh * 0.5f;
                G.inactive = false;
                T.liveList.push_back((uint32_t)ix);   // (on outside a placement: the next one switches it off unless it places it)
                T.sh.moverWritten((size_t)ix);   // (prevCy, the box and the inactive byte are written whether or not the centre moved)
                if (mv) ++g_shStats.supFolMoved;
                ++g_shStats.supFolPlaced;
                continue;
            }
            const float cy = (float)fp.cy;
            const bool moved = G.cy != cy;
            G.cy = cy; G.prevCy = (float)(fp.cy - fp.dcy); G.ry = cy - G.rh * 0.5f;
            G.inactive = false;
            T.liveList.push_back((uint32_t)ix);
            T.sh.moverWritten((size_t)ix);   // (prevCy, the box and the inactive byte are written whether or not the centre moved)
            if (moved) ++g_shStats.supFolMoved;
            ++g_shStats.supFolPlaced;
        }
    pm(1);
    const bool lat2 = s.mode == 1 || s.mode == 3;
    // The button is the plan's raw level. The old tick's physics reads it after its gates (a Player Control Stop Jump, a no-control window, a crossed or touched Options trigger, State::ctrlT); handing the shadow that gated
    // level as a release made custom level A / custom level B WORSE (customs all 64 -> 72 diverged sections, 177 refused presses): GD keeps the button's hold bookkeeping while the controls are off
    // (handleButton, +0xa33), so a port needs the disabled flag in the shadow's button pass, not a plan edge. Counted here, not applied.
    const bool btn = input != 0;
    if (g_shadowSupply && t_inputEffValid && (t_inputEff != 0) != (input != 0)) ++g_shStats.supGateRefused;
    // The objects a Tap switches stand off while their group is off (GD's handleButton calls GJEffectManager::playerButton in the head, which toggles the group at once): a ship's or a UFO's head
    // (this tick's button) runs before the tail, so the pass of this tick sees the edge; any other mode's head is the end of the tick, so the pass sees the groups as the parent left them.
    shadowApplyTapGates(T, s, lat2 ? input : -1);

    // The tail of a latency-1 tick does not read the input (the head at its end does): the second child of a parent repeats the first one's tail
    // from the memo (shadowTailMemo) instead of stepping it again.
    TailMemo& M = T.memo;
    const bool hit = false;   // (the tail memo is not read: the pass turns the body on a trigger, which a memo of the pass cannot hold)
    if (!hit) M.valid = false;
    t_shMemoHit = hit;
    // --shadowdual: player 2's block. Not filled here (a ShadowLite is 1.4 KB of zeroes to construct, on every tick of every level): shadowDualLoad fills it for a state that is a dual, and a
    // state that turns dual inside this tick has it zeroed right before its pack (below). Nothing else reads it.
    alignas(ShadowLite) unsigned char sl2Raw[sizeof(ShadowLite)];
    ShadowLite& sl2 = *reinterpret_cast<ShadowLite*>(sl2Raw);
    int dualHow = 0;  // shadowDualLoad's answer
    if (dualOn) {
        dualHow = shadowDualLoad(T, s, K.t - 1, g_shBase.seedHave && K.t == g_shBase.seedT + 1 && std::fabs(s.xAbs - g_shBase.seedX) < 50.0f, sl2);
        if (dualHow == 3) ++g_shStats.dualSeededModel;
        T.ds.unlinkOpt = unlinkTrigAt((int)s.frame, (double)s.xAbs, (double)s.y);   // +0x860 as the Options trigger leaves it (toggleDualMode's birth reads this alone)
        T.ds.unlink = dualUnlinkedAt((int)s.frame, (double)s.xAbs, (double)s.y);   // --shadowp2dir: the layer's +0x860 (an Options trigger's Unlink Dual Gravity) as the old tick reads it -- a player 2 that now meets its own objects flips player 1 only while the pair is linked
    }
    pm(8);
    // The state's block, copied out of the store (the step rewrites it); a state with no block starts from the zero block. Copied straight into raw storage: constructing a ShadowLite
    // first was 1.4 KB of zeroes written and then overwritten.
    alignas(ShadowLite) unsigned char slRaw[sizeof(ShadowLite)];
    const ShadowLite* const slStored = slOf(s.slRef);
    ShadowLite& sl = slStored ? *static_cast<ShadowLite*>(std::memcpy(slRaw, slStored, sizeof(ShadowLite))) : *new (slRaw) ShadowLite();
    ShadowLayer own;
    bool exact = false, seededM = false;
    SelfIn inT; inT.dt = t_shDt;
    inT.tick = K.t - 1;
    SelfOut& outT = T.outs[1];
    // --shadowrotapply: the touch Rotate Gameplay triggers the model's queue has spent are the ones the pass must not turn on again (a trigger fires once)
    static thread_local std::vector<int> t_rotSpentUids;
    if (!g_rotQ.empty()) {
        t_rotSpentUids.clear();
        // (a channel-only trigger moves the layer's channel and nothing of the body: GD's rotateGameplay skips the player's half for it, +0x755; the old tick's queue spends it alone)
        for (size_t k = 0; k < g_rotQ.size() && k < 64; ++k) if (((s.rotSpent >> k) & 1ull) || g_rotQ[k].chanOnly) t_rotSpentUids.push_back(g_rotQ[k].uid);
        // --shadowrotbook: a trigger the driver says had fired before the anchor (--spentrot; State::rotSpent only carries it when the queue seed is exact) is spent for the pass too -- the game does not turn on it a second time (a custom level t=13361: uid 9386 fired at t=11924, the section starts at 12600 with rotSpent 0)
        if (g_shadowRotBook) for (int su : g_spentRot) t_rotSpentUids.push_back(su);
        inT.rotApply = true; inT.rotSpent = &t_rotSpentUids;
    }
    // --shadowsupply, the controls: the Options triggers the player's x has crossed (g_ctrlTrigs, the non-gated ones: 1 disables player 1's controls, -1 enables) become the shadow's own disablePlayerControls /
    // enablePlayerControls (+0xa33) at the head that takes the button after the crossing, with the parent's touched Options state (State::ctrlT, from the next tick on). pushButton / releaseButton then keep
    // the hold bookkeeping and do nothing else while the byte is set, as the game's do. The old tick's own gate (recorded windows by tick, the lag rules) is not read: handing the shadow the gated button
    // as a release made two customs worse, and where it said "off" the game had the controls on (a custom level t=5777: ctrlOff 0 in the game's dump, the spider jumped). kSlCtrlOff keeps the state last applied.
    const bool ctrlOn = g_shadowSupply && !s.dual && !cDual && g_rotTrig.empty() && cFrame == 0;
    // --shadowrev: where the supply above is not valid (a level with Rotate Gameplay triggers: its Options triggers fire from the channel queue, not from the player's x), the controls the old tick refused (its recorded
    // windows, the crossed or touched Options triggers: t_ctrlGateNow) are the disabled flag the shadow's heads see -- lv22 t=20150..20526, a mini cube on a ramp that the plan's button would have jumped off
    // ...only while the body runs left or turned (the ticks the supply could not serve), and on after: a flag the old tick's gate raised stays in the image, so the first tick past the window still takes the enable edge
    // --shadowctrlgate (off): ...and on every tick of such a level, not only the turned or reversed ones: a custom level t=1891, a cube on the ground inside a no-control window (ctrlwin 1746:1930) that the plan presses in; the old tick refuses the press, the shadow took it
    const bool dualCtrl = dualOn && s.dual && cDual;   // player 1's gate inside a dual
    const bool ctrlOld = !ctrlOn && ((!s.dual && !cDual) || dualCtrl);
    bool ctrlAny = ctrlOn || ctrlOld;
    bool ctrlBit = false;   // (the state last applied: read from the block once it is loaded)
    bool ctrlBit2 = dualCtrl && (dualHow == 1) && (sl2.flags2 & kSlCtrlOff);   // --shadowdualctrl: player 2's, from its carried block
    auto ctrlWant = [&](double x, bool prev, bool touch) { if (ctrlOld) return dualCtrl ? t_ctrlGateP1 : t_ctrlGateNow; const int st = ctrlTrigStateAt(x); if (st == -1 && prev && !touch && !t_ctrlGateNow) return false; return touch || st == 1 || (st == -1 && prev); };
    if (hit) {
        shadowMemoRestore(T, sl, own, outT);
        exact = M.exact; seededM = M.seededM;
        ctrlBit = (ctrlOn || ctrlOld) && exact && (sl.flags2 & kSlCtrlOff);
        if (ctrlOld && ctrlBit) ctrlAny = true;
        if (M.cntInexact) ++g_shStats.declinedInexact;
        if (M.cntSeedFile) ++g_shStats.seededFile;
        if (M.cntGrav) ++g_shStats.gravBridged;
        ++g_shStats.stepped;
        pm(2);
    } else {
    bool cntInexact = false, cntSeedFile = false, cntGrav = false;
    // The images of the step before this one are the thread's leftover (whatever state it stepped last), not this state's: slPack's ambiguity test reads them (the player's left edge against the
    // dropped objects' edge), so a seed or a latency-2 tick that packs before this call's own tail / head has written them took the flag from another state, and the answer changed with the thread
    // schedule. Empty images are what slPlayerLeft ignores.
    sh.atButtons.b.clear(); sh.atEntry.b.clear(); sh.atExit.b.clear(); sh.ptButtons = {}; sh.ptEntry = {};
    exact = (sl.flags & kSlValid) && !(sl.flags & (kSlOverflow | kSlAmbiguous));   // (sl: the stored block, copied at its declaration)
    ctrlBit = (ctrlOn || ctrlOld) && exact && (sl.flags2 & kSlCtrlOff);
    if (ctrlOld && ctrlBit) ctrlAny = true;
    // A seed from the model's fields is lossy: the counters and contacts GD carries in the image (the 1755 dart-slide arm, a ride, the touch logs) are
    // not in the State. So a model seed is taken only if its first tick lands where the model's own tick does (x, y within 0.01, the same mode, gravity
    // and death); else the model's tick is kept and the next eligible tick tries again. w_139771614_6030: a wave seeded as a time warp ended, GD's arm
    // counter at 2 and the model's slideT saying "not armed", fell 0.87 px through the moving block GD and the model both seat it on.
    if (!exact) {
        if ((sl.flags & kSlValid) && (sl.flags & (kSlOverflow | kSlAmbiguous))) { ++g_shStats.declinedInexact; cntInexact = true; }
        if (g_shBase.seedHave && K.t == g_shBase.seedT + 1 && std::fabs(s.xAbs - g_shBase.seedX) < 50.0f) {   // (a state at the anchor)
            shadowSeedFromFile(T, s, sl);
            ++g_shStats.seededFile; cntSeedFile = true;
        } else {
            shadowSeedFromModel(T, s, K.t - 1, sl);
            seededM = true;
        }
    }
    slUnpack(sl, g_shBase.tmpl, T.W, sh, (float)s.xAbs - 600.0f);
    slUnpackLayer(sl, own);
    t_evPreFire.assign(sh.fireOrder.begin(), sh.fireOrder.end());   // the fired-ring record as this tick's pass starts (the block's, or the State's slots when the body was seeded from the model): the rings the head and the tail add are the ones this tick fired (eventsOfPass)
    // --shadowrev: the old model turned the player round between two ticks (a Rotate Gameplay trigger, which sets State::rev and the gravity as one) -- the image the last pass left does not know it, and a
    // pass from it would go on in the old direction (a custom level t=9472: GD x 15284.07, the pass 15287.30). The model's tick stands; the next tick seeds from the State.
    const bool revMismatch = exact && ((sh.img.u8(po::Leftward) ? 1 : 0) != gdLeft(s.rev)
        || ((sh.img.u8(po::Sideways) ? 1 : 0) != (fr ? 1 : 0) || (sh.img.u8(po::Upside) ? 1 : 0) != gdUp(s.flip)));
    if (revMismatch && !s.dual) {   // --shadowrevseed (constants.hpp): no old tick to decide this tick: the body is seeded from the State (the model's turn) and stepped, as an inexact block is
        // a frame turn (Sideways / Upside differ) moved the body after the block's pass; a bare direction flip (Leftward alone: a Reverse trigger) did not move it
        const bool frameTurned = (sh.img.u8(po::Sideways) ? 1 : 0) != (fr ? 1 : 0) || (sh.img.u8(po::Upside) ? 1 : 0) != gdUp(s.flip);
        shadowSeedFromModel(T, s, K.t - 1, sl, false, frameTurned);
        seededM = true; exact = false;
        slUnpack(sl, g_shBase.tmpl, T.W, sh, (float)s.xAbs - 600.0f);
        t_evPreFire.assign(sh.fireOrder.begin(), sh.fireOrder.end());
        // (own stays what the carried block said: the reseed is the BODY's, from the State. GD's rotateGameplay writes none of +0x308 / +0x2ec / +0x310 / +0x311 / +0x400 ([R] 2.209b, gd-05), and the seed's layer
        // words are the State's defaults -- on 0, no portal known, lastMode -1 -- which emptied the layer on the tick after the turn: [M] a custom level:4600, 4799 pre on 1 known 1 lastMode 5 centre 3360 H 300, 4801 on 0 known 0.)
    } else if (revMismatch) return unsupported(5);
    // the gravity trigger's value in force over this tick (the model's rule: at the tick's starting x)
    if (!fr && (!g_gravTrigs.empty() || s.gravTouch)) {   // (a turned frame: the triggers are listed by the upright frame's x, which the State's travel coordinate is not -- the image keeps its value)
        const float gm = (float)gravModOfCh(crossXOf((double)s.xAbs, g_tpBack ? s.tpBack : 0), g_gravTouch ? s.gravTouch : 0, !g_rotQ.empty() ? (int)s.rotChan : -1, s.rev != 0, (uint32_t)s.trig.word(0));
        if (sh.img.f32(0xb8c) != gm) { sh.img.f32(0xb8c, gm); ++g_shStats.gravBridged; cntGrav = true; }
    }
    pm(2);
    // ---- the tail of step t ----
    own.advanceFlip((float)sh.stepSeconds);
    sh.L = shadowLayerFor(own, s, K.t);
    // The button. The model's input of tick K.t is the plan edge pressed at K.t - lat (cli.hpp's replay schedule: latency 2 for a ship or a UFO, 1 else),
    // and GD takes a plan edge pressed at p in the head of step p + 1. So for a latency-1 mode this tick's input is the button of step K.t (the head at
    // the END of the tick, below), and for a ship or a UFO it is the button of step K.t - 1: the head runs FIRST, then the tail of the same step
    // (s_ship_dn_111832753: with the head at the end the shadow held every ship press one tick too long). kSlHeadPending marks a row whose own step's
    // head has not run yet (a latency-2 tick leaves it so): a latency-1 tick then runs that head first without an edge, and a latency-2 tick after a
    // latency-1 one skips it (that head already took the same plan edge). The edge is this tick's input against the shadow's own last button.
    const bool headPending = exact ? (sl.flags2 & kSlHeadPending) != 0 : (seededM && lat2);
    const bool lastBtn = sh.holding[1];
    if (headPending) {
        SelfIn inP; inP.dt = t_shDt;
        inP.tick = K.t - 1;
        if (lat2) { inP.press1 = (btn && !lastBtn) ? 1 : 0; inP.release1 = (!btn && lastBtn) ? 1 : 0; }
        if (lat2 && ctrlAny) { const bool w = ctrlWant((double)s.xAbs, ctrlBit, s.ctrlT != 0); inP.ctrl = ctrlEdgeOf(ctrlBit, w); ctrlBit = w; }
        if (lat2 && dualCtrl) { const bool w2 = t_ctrlGateP2; inP.ctrl2 = ctrlEdgeOf(ctrlBit2, w2); ctrlBit2 = w2; }
        T.ds.own2 = dualOn && in2e >= 0;
        if (T.ds.own2) { const bool b2 = in2e > 0, l2 = T.ds.two.holding[1]; T.ds.press2 = (lat2 && b2 && !l2) ? 1 : 0; T.ds.release2 = (lat2 && !b2 && l2) ? 1 : 0; }
        SelfOut& outP = T.out(0);
        if (dualOn) { SelfOut& o2 = T.out2(0); T.ds.head(inP, outP, o2); } else
        sh.stepHead(inP, outP);
    }
    outT.reset();
    if (dualOn) {   // both players' tails; player 2's portals join player 1's for the layer words below (a mode portal's band, the dual's end)
        SelfOut& o2 = T.out2(1);
        T.ds.tail(inT, outT, o2);
        for (const auto& f : o2.fired) outT.fired.push_back(f);
        if (T.ds.active || T.ds.ended) ++g_shStats.dualStepped;
    } else
    sh.stepTail(inT, outT);
    ++g_shStats.stepped;
    // (No shadowMemoSave here: the tail memo is not read -- `hit` above is false, the pass turns the body on a trigger -- and saving it was a copy of the image, the block, the layer, the
    // pass's SelfOut and four sets on every latency-1 tick with nobody to take it. The memo's code stays for the day it is read again.)
    }
    // The model's Teleport triggers (x-crossing, step.hpp's rule: the crossing from where the state stood to where the pass left it), run the shadow's way:
    // GJBaseGameLayer::teleportPlayer at the end of the pass (checkSpawnObjects follows it), so the next step's previous position is the exit.
    bool tpAfterExit = false;   // --tpexittouch (removed): a Teleport trigger / touch box moved the body after the pass
    if (!fr && !outT.died && g_teleportTrig && !g_tpTrigs.empty()) {   // (a turned frame: the old step does not cross them either, and their x is not the State's travel coordinate)
        const uint8_t back0 = g_tpBack ? s.tpBack : 0;
        const double xp = crossXOf(crossBaseX(s), back0), xn = crossXOf((double)sh.atExit.f32(po::PosX), back0);
        for (const TpTrig& tt : g_tpTrigs) {
            if (tt.cx > xn) break;
            if (tt.cx <= xp) continue;
            // (always on: the switch --tptrigchan is removed)
            // --tptrigchan (removed): a teleport trigger belongs to a rotation channel (the objrects `chan` column), and GJBaseGameLayer::checkSpawnObjects
            // walks only the ACTIVE channel's bucket, so a trigger of another channel does not fire when the player crosses it. Without the flag the model fires
            // every crossing whatever channel is active. A custom level's teleport trigger sits in channel 1: GD fires it once, when the reversed run of
            // channel 1 crosses it (t=14,020), and not on the way there in channel 0 (t=13,082) nor on the way back in channel 2 (t=14,599), where the model
            // teleported 705 px and ran on from the wrong place. Needs the rotation queue (the state's channel is not tracked without it).
            // On since 2026-10-27: on the 22 custom levels every one still clears, with the record untouched, and the level that holds the
            // trigger drops from 3 secsolve handoffs to 0.
            if (tpSpawnedByShadow(tt)) continue;
            if (!g_rotQ.empty() && (int)cRotChan != tt.chan) continue;
            if (sh.worldIndexOf(tt.uid) < 0) continue;
            sh.doTeleport(tt.uid, false, outT);
            tpAfterExit = true;
            sh.img.f32(0xa00, sh.img.f32(po::PosX));   // (the stores the step made after the pass take the exit)
            sh.img.f32(0xa04, sh.img.f32(po::PosY));
            if (inT.tick % sh.frameLen == sh.frameEndMod) { sh.img.f32(0xa98, sh.img.f32(po::PosX)); sh.img.f32(0xa9c, sh.img.f32(po::PosY)); }
            ++g_shStats.tpBridged;
        }
    }
    // (always on; it fires only when the trigger shadow's supply is on) a Teleport trigger the trigger shadow's timeline fired at this tick (TrigLive::tpFiresAt: a Spawn chain's or a touch's, in firing order) moves the body the way the crossing ones above
    // do -- GJBaseGameLayer::teleportPlayer at the end of the pass (the timeline's crossing loop runs after the player's update, and the game's row of the tick already holds the exit). The trigger must be in the shadow's world.
    if (!fr && !outT.died && g_teleportTrig && g_tpEventsHook && g_tpEventsSupplyOn()) {
        for (const int tuid : g_tpEventsHook((long long)K.t)) {
            bool onQueue = false;   // an x-crossing Teleport is on the model's queue and was applied above
            for (const TpTrig& tt : g_tpTrigs) onQueue = onQueue || (tt.uid == tuid && !tpSpawnedByShadow(tt));
            if (onQueue) continue;
            if (sh.worldIndexOf(tuid) < 0) { ++g_shStats.tpEvMiss; continue; }
            sh.doTeleport(tuid, false, outT);
            tpAfterExit = true;
            sh.img.f32(0xa00, sh.img.f32(po::PosX));
            sh.img.f32(0xa04, sh.img.f32(po::PosY));
            if (inT.tick % sh.frameLen == sh.frameEndMod) { sh.img.f32(0xa98, sh.img.f32(po::PosX)); sh.img.f32(0xa9c, sh.img.f32(po::PosY)); }
            ++g_shStats.tpEvFired;
        }
    }
    // --shadowtprev (constants.hpp): the same crossing on a REVERSED run (step.hpp --tprev (removed): x falls and a trigger fires once the body's x is at or below its own), run the shadow's way, when no old tick has run
    // (always on: the switch --tprev is removed)
    // --tprev (removed): a teleport trigger on a REVERSED run. GJBaseGameLayer::checkSpawnObjects (0x21a8f0) reads a per-channel
    // direction flag: unset, it breaks out of the channel's walk while the player's x is below the trigger's start x (forward);
    // set, while the trigger's x is below the player's -- so a reversed channel fires a trigger once the player's x is at or below
    // its own. A custom level t=14,021: a cube runs left (x falling by 1.95 a tick), the trigger at x 19,350 is crossed between
    // 19,351.79 and 19,349.84, and GD puts the body at the target (18,645, 5,745) that row with vy untouched; the model, which
    // reads no crossing while reversed, ran on at x 19,350. Crossing triggers other than the teleport are left as they were. One
    // body in frame 0.
    if (!fr && s.rev && cRev && !s.dual && !cDual && !outT.died && g_teleportTrig && !g_tpTrigs.empty()) {
        const double xp = (double)s.xAbs, xn = (double)sh.atExit.f32(po::PosX);
        for (size_t ti = g_tpTrigs.size(); ti-- > 0;) {
            const TpTrig& tt = g_tpTrigs[ti];
            if (tt.cx < xn) break;
            if (tt.cx >= xp) continue;
            if (!g_rotQ.empty() && (int)cRotChan != tt.chan) continue;
            if (sh.worldIndexOf(tt.uid) < 0) continue;
            sh.doTeleport(tt.uid, false, outT);
            tpAfterExit = true;
            sh.img.f32(0xa00, sh.img.f32(po::PosX));
            sh.img.f32(0xa04, sh.img.f32(po::PosY));
            if (inT.tick % sh.frameLen == sh.frameEndMod) { sh.img.f32(0xa98, sh.img.f32(po::PosX)); sh.img.f32(0xa9c, sh.img.f32(po::PosY)); }
            ++g_shStats.tpBridged;
        }
    }    // --shadowsupply, tpTouch: a touch-triggered Teleport box the pass overlapped (step.hpp's test: the body after the move, the y the pass tested, closed edges, the box this branch fired last
    // excepted) moves the body at the end of the tick as the crossing trigger above does -- GJBaseGameLayer::teleportPlayer through the shadow's own doTeleport. State::tpTouch keeps the box.
    size_t tpTouchIdx = 0;
    if (!outT.died && g_shadowSupply && g_tpTouch && g_teleportTrig && !g_tpTouchBoxes.empty() && !s.dual && !cDual && cFrame == 0 && !s.rev && !cRev) {
        const Mem& Pb = sh.atExit.b.size() >= 0xc80 ? sh.atExit : sh.img;
        const double bx = (double)Pb.f32(po::PosX), byFree = (double)(sh.ptEntry.ok ? sh.ptEntry.posY : Pb.f32(po::PosY));
        const int bm = shadowModeOf(Pb);
        const bool bmini = Pb.f32(0x9f8) != 1.0f;
        const double ph = (bm == 4 && bmini) ? kWaveContactHalfMini : playerHalf(bm, bmini);
        for (size_t i = 0; i < g_tpTouchBoxes.size(); ++i) {
            const TpTouchBox& b = g_tpTouchBoxes[i];
            if (!b.event && i + 1 != (size_t)s.tpTouch && std::fabs(bx - b.cx) <= b.hw + ph && std::fabs(byFree - b.cy) <= b.hh + ph && sh.worldIndexOf(b.t.uid) >= 0) { tpTouchIdx = i + 1; break; }
        }
        if (!tpTouchIdx && t_tpEvent && t_tpEvent <= g_tpTouchBoxes.size() && sh.worldIndexOf(g_tpTouchBoxes[t_tpEvent - 1].t.uid) >= 0) tpTouchIdx = t_tpEvent;   // an Event trigger's (the old tick's listeners)
        if (tpTouchIdx) {
            sh.doTeleport(g_tpTouchBoxes[tpTouchIdx - 1].t.uid, false, outT);
            tpAfterExit = true;
            sh.img.f32(0xa00, sh.img.f32(po::PosX));
            sh.img.f32(0xa04, sh.img.f32(po::PosY));
            if (inT.tick % sh.frameLen == sh.frameEndMod) { sh.img.f32(0xa98, sh.img.f32(po::PosX)); sh.img.f32(0xa9c, sh.img.f32(po::PosY)); }
            ++g_shStats.tpBridged;
            ++g_shStats.supTpTouch;
        }
    }
    {   // SHSTEP_DEBUG=t0,t1 (stderr): what the pass of each tick in the range fired, and where the movers it overlapped were
        static long long dbgLo = -1, dbgHi = -2;
        static bool dbgRead = false;
        if (!dbgRead) { dbgRead = true; if (const char* e = std::getenv("SHSTEP_DEBUG")) std::sscanf(e, "%lld,%lld", &dbgLo, &dbgHi); }
        if (K.t >= dbgLo && K.t <= dbgHi) {
            std::fprintf(shDebugOut(), "shstep t=%lld rot %.4f entry x %.4f y %.4f vy %.4f up %d | exit y %.4f vy %.4f up %d fired:", K.t, (double)sh.atEntry.f32(0x38), sh.atEntry.f32(po::PosX), sh.atEntry.f32(po::PosY), sh.atEntry.f64(po::Vy), (int)sh.atEntry.u8(po::Upside), sh.atExit.f32(po::PosY), sh.atExit.f64(po::Vy), (int)sh.atExit.u8(po::Upside));
            for (const auto& f : outT.fired) {
                const long long wi = sh.worldIndexOf(f.second);
                const gdcol::GObj* o = wi >= 0 ? &T.world[(size_t)wi] : nullptr;
                std::fprintf(shDebugOut(), " %d:%d%s(%.3f,%.3f %.1fx%.1f%s%s)", f.first % 1000, f.second, f.first >= 1000 ? "*" : "", o ? o->cx : 0.f, o ? o->cy : 0.f, o ? o->rw : 0.f, o ? o->rh : 0.f, (wi >= 0 && g_shBase.mover[(size_t)wi]) ? " mover" : "", (o && o->inactive) ? " off" : "");
            }
            std::fprintf(shDebugOut(), " | ground (uid+1; img = end of this tick, entry = at the pass): img gnd %lld 660 %d coll %lld G %d slope %lld | entry gnd %lld 660 %d coll %lld G %d | exit gnd %lld 660 %d",
                         (long long)sh.img.get<int64_t>(po::GroundObj), (int)sh.img.u8(0x660), (long long)sh.img.get<int64_t>(po::Collided), (int)sh.img.u8(po::G), (long long)sh.img.get<int64_t>(po::SlopeObj),
                         (long long)sh.atEntry.get<int64_t>(po::GroundObj), (int)sh.atEntry.u8(0x660), (long long)sh.atEntry.get<int64_t>(po::Collided), (int)sh.atEntry.u8(po::G),
                         (long long)sh.atExit.get<int64_t>(po::GroundObj), (int)sh.atExit.u8(0x660));
            std::fprintf(shDebugOut(), " | plat vel img 630 %.4f 638 %.4f 628 %.4f a24 %d | entry 630 %.4f 638 %.4f 628 %.4f a24 %d", sh.img.f64(0x630), sh.img.f64(0x638), sh.img.f64(0x628), (int)sh.img.u8(0xa24), sh.atEntry.f64(0x630), sh.atEntry.f64(0x638), sh.atEntry.f64(0x628), (int)sh.atEntry.u8(0xa24));
            std::fprintf(shDebugOut(), " | hit-head b7c img %d entry %d exit %d | b88 img %d entry %d exit %d | H98d img %d entry %d exit %d | S98e img %d entry %d", sh.img.i32(0xb7c), sh.atEntry.i32(0xb7c), sh.atExit.i32(0xb7c), sh.img.i32(0xb88), sh.atEntry.i32(0xb88), sh.atExit.i32(0xb88), (int)sh.img.u8(0x98d), (int)sh.atEntry.u8(0x98d), (int)sh.atExit.u8(0x98d), (int)sh.img.u8(0x98e), (int)sh.atEntry.u8(0x98e));
            std::fprintf(stderr, " | ctrlbit %d gateNow %d effIn %d rawIn %d | ever %zu gate %zu | died %d why %d uid %d a32 %d 9c8 %d speed %.3f x7c0 %.4f | b80 %d b84 %d slideT %d armT %d seeded %d ab8 %.3f\n", (int)ctrlBit, (int)t_ctrlGateNow, t_inputEffValid ? t_inputEff : -1, input, sh.ever.size(), sh.gate.size(), (int)outT.died, outT.why, outT.whyUid, (int)sh.atEntry.u8(0xa32), (int)sh.atExit.u8(0x9c8), sh.atEntry.f32(0x9fc), sh.atEntry.f64(0x7c0),
                         sh.atEntry.i32(0xb80), sh.atEntry.i32(0xb84), (int)s.slideT, (int)s.armT, exact ? 0 : 1, sh.atEntry.f32(0xab8));
        }
    }
    pm(3);
    if (g_shadowRotProbe) {   // observation only: the touch Rotate Gameplay triggers (2900) this pass's player box overlapped (collisionCheckObjects case 0x14 lists them in fired)
        for (const auto& f : outT.fired) {
            if (f.first % 1000 != 0x14) continue;
            const long long wi = sh.worldIndexOf(f.second);
            const gdcol::GObj* o = wi >= 0 ? &T.world[(size_t)wi] : nullptr;
            if (!o || o->objectId != 2900 || !o->touchTrig) continue;
            int qk = -1;   // the trigger's slot in the model's rotation queue, and whether the state has spent it (a trigger fires once)
            for (size_t k = 0; k < g_rotQ.size() && k < 64; ++k) if (g_rotQ[k].uid == f.second) { qk = (int)k; break; }
            const int spent = qk >= 0 ? (int)((s.rotSpent >> qk) & 1ull) : -1;
            std::fprintf(shDebugOut(), "shrotprobe t=%lld uid=%d fresh=%d spent=%d frame=%d entry x %.4f y %.4f exit x %.4f y %.4f trig %.3f,%.3f %.1fx%.1f\n", K.t, f.second, f.first >= 1000 ? 1 : 0, spent, (int)s.frame,
                         sh.ptEntry.posX, sh.ptEntry.posY, sh.atExit.f32(po::PosX), sh.atExit.f32(po::PosY), o->cx, o->cy, o->rw, o->rh);
        }
    }
    {   // TSMT_DBG=t0,t1 (env, off; stderr): the touch-triggered triggers this pass listed (collisionCheckObjects case 0x14 / 0x2d), `fresh` = the pass's own first-act flag (type + 1000), with the body's y at the pass's entry and exit
        static const char* const tsmtDbg2 = std::getenv("TSMT_DBG");
        if (tsmtDbg2) {
            long long a = 0, b = 0; std::sscanf(tsmtDbg2, "%lld,%lld", &a, &b);
            if (K.t >= a && K.t <= b)
                for (const auto& f : outT.fired) {
                    if (f.first % 1000 != 0x14 && f.first % 1000 != 0x2d) continue;
                    const long long wi = sh.worldIndexOf(f.second);
                    if (wi < 0 || !T.world[(size_t)wi].touchTrig) continue;
                    std::fprintf(stderr, "shtouch t=%lld uid=%d fresh=%d entry y %.4f exit y %.4f x %.4f\n", (long long)K.t, f.second, f.first >= 1000 ? 1 : 0,
                                 sh.ptEntry.ok ? (double)sh.ptEntry.posY : -1.0, sh.atExit.b.size() >= 0xc80 ? (double)sh.atExit.f32(po::PosY) : -1.0, (double)sh.img.f32(po::PosX));
                }
        }
    }
    // --shadowrotapply, S3: the pass turned the body with a touch Rotate Gameplay. The turn is printed (the body at the pass's exit), and the body this tick ends with is held (t_shRotHold, at the write-back
    // below) for shadowRotFinish: the caller's applyRotation does the queue's bookkeeping, then the finish puts the body over the old tick's. A tick whose turn the queue cannot take (a trigger that is not a
    // queue slot, a death in the turning pass) is the old tick's, as every turn was before.
    bool holdPending = false;
    uint64_t holdBits = 0;
    if (!outT.rotFired.empty()) {
        const Mem& X = sh.atExit.b.size() >= 0xc80 ? sh.atExit : sh.img;
        bool slots = true;
        for (int u : outT.rotFired) {
            int qk = -1;
            for (size_t k = 0; k < g_rotQ.size() && k < 64; ++k) if (g_rotQ[k].uid == u) { qk = (int)k; break; }
            if (qk < 0) slots = false; else holdBits |= 1ull << qk;
        }
        if (g_shadowRotProbe)
            for (int u : outT.rotFired)
                std::fprintf(shDebugOut(), "shrotapply t=%lld uid=%d slots=%d exit x %.4f y %.4f vy %.4f up %d left %d side %d grounded %d died %d\n", K.t, u, slots ? 1 : 0, X.f32(po::PosX), X.f32(po::PosY), X.f64(po::Vy),
                             (int)X.u8(po::Upside), (int)X.u8(po::Leftward), (int)X.u8(po::Sideways), (int)X.u8(po::G), (int)outT.died);
        if (!slots && !outT.died) { slTrackActivated(sh, T.W); return unsupported(6); }   // (a turn the queue has no slot for; a death in the turning pass is the death below)
        if (!outT.died) holdPending = true;
    }
    // the portals the pass took: the layer words they set, then +0x764 := getMaxPortalY() at the step's end
    bool dbPortal = false, dbDualOn = false, dbFree = false; float dbY = 0.0f; int dbTy = 0;   // the last band-writing portal of the tick (the dual band rule below)
    for (const auto& f : outT.fired) {
        if (f.first < 1000) continue;
        const int ty = f.first - 1000;
        const auto pf = g_shBase.portalFree.find(f.second);
        const bool fr = pf != g_shBase.portalFree.end() && pf->second.first;
        const float sy = pf != g_shBase.portalFree.end() ? pf->second.second : 0.0f;
        if (ty == 5 || ty == 6 || ty == 0x10 || ty == 0x13 || ty == 0x1a || ty == 0x1b || ty == 0x21 || ty == 0x29) { own.modePortal(ty, f.second, sy, fr, false); dbPortal = true; dbDualOn = false; dbFree = fr; dbY = sy; dbTy = ty; }
        else if ((ty == 0xe || ty == 0xf) && !sh.L.f317c) own.mirrorPortal(ty == 0xe, false);
        else if (ty == 0x17 || ty == 0x18) { own.dualPortal(ty == 0x17, sy, fr, false, ShadowLayer::modeTypeOf(sh.img)); if (ty == 0x17) { dbPortal = true; dbDualOn = true; dbFree = fr; dbY = sy; } }
    }
    {
        float mn = 0.f, mx = 0.f;
        own.w764Known = own.band(sh.L.speedA, 0.0f, false, mn, mx);
        if (own.w764Known) own.w764 = mx;
    }
    // The Zoom triggers (GJBaseGameLayer::updateZoom) and Camera Mode triggers (id 2925, +0x311 Free Mode) this tick's x crossing fires: each runs updateDualGround again (shadow_layer.hpp zoomTrigger /
    // freeModeTrigger), after the tick's collisions like every crossing trigger (bands.hpp --camfree (removed)), so the band it writes is the next tick's reading (+0x764 above is the pass's own). The crossing is read
    // on the world x of the body before the tick (the State's frame) and the image after it, as a turned frame's travel coordinate is not the triggers' x. The State's band follows the layer's (zoom 1).
    // [R] updateZoom base+0x33eac0, updateDualGround 0x31a4a0, animateInDualGroundNew 0x31a690; [M] a custom level t=4799 / 4800.
    bool trigBand = false;
    if ((!g_zoomTrigs.empty() || !g_camFrees.empty()) && sh.img.b.size() >= 0xc80) {
        double x0 = 0.0, y0 = 0.0;
        fromFrame((int)s.frame, (double)s.xAbs, (double)s.y, x0, y0);
        const double x1 = (double)sh.img.f32(po::PosX);
        for (const CamFree& cf : g_camFrees) if (cf.cx > x0 && cf.cx <= x1) { own.freeModeTrigger(cf.free != 0); trigBand = true; }
        for (const ZoomTrig& zt : g_zoomTrigs) if (zt.cx > x0 && zt.cx <= x1) { own.zoomTrigger(ShadowLayer::modeTypeOf(sh.img)); trigBand = true; }
        if (trigBand) ++g_camyObs.trigRewrites;   // (cam_y_obs.hpp, the camy line)
    }
    if (trigBand && own.animEvent && own.animIn && own.heightKnown) {   // (the per-level overrides g_shipCeilSet / g_ufoCeil / g_flyFloor were false / 0 / 0 with no setter left since their flags went, 2026-08-31, so the three `not overridden` terms this condition ended with were always true: same condition)
        c.bandFloor = own.centre - own.height * 0.5f; c.bandCeil = own.centre + own.height * 0.5f; c.bandOffY = 0.f;
        t_shTrigBand.valid = true; t_shTrigBand.f = c.bandFloor; t_shTrigBand.c = c.bandCeil;
    }
    auto writeBody = [&](const Mem& P) {
        { double u, v; toFrameXY((double)P.f32(po::PosX), (double)P.f32(po::PosY), u, v); c.xAbs = (float)u; c.y = (float)v; }
        c.vy = fr == 3 ? -(float)P.f64(po::Vy) : (float)P.f64(po::Vy);
        const int m = shadowModeOf(P);
        if (m != (int)c.mode) ++g_shStats.modeDiff;
        c.mode = (uint8_t)m;
        const uint8_t up = gdUp(P.u8(po::Upside) ? 1 : 0);   // (the opposite of the byte in a frame 3: the State's flip)
        if (up != c.flip) ++g_shStats.flipDiff;
        c.flip = up;
        const uint8_t mi = P.f32(0x9f8) != 1.0f ? 1 : 0;
        if (mi != c.mini) ++g_shStats.sizeDiff;
        c.mini = mi;
        c.grounded = P.u8(po::G) ? 1 : 0;
        c.rot = P.f32(0x38);
        if (m == 2) c.rotStep = shBallStakeOf(P);
        // An airborne cube's spin stake (+0x728, on while +0x730) goes back to the two words the seed makes it from: its sign (State::rotNeg) and its size as a multiple of the plain stake
        // (State::rotGm: runNormalRotation 0x4eb4d0 writes 180 * signs * m_gravityMod * arg / k, k 0.4333 full and 0.3333 mini). [R] shadowSeedFromModel runs runNormalRotation on the template and
        // then writes |+0x728| * rotGm with rotNeg's sign over it (frame 0 only); the old tick wrote both and nothing has since its removal, so a cube seeded from the State in the air (a block that is
        // not exact) span at the anchor's sign and size -- or, with no anchor word, forward at the plain size whatever its gravity, direction and gravity value were when it left the ground.
        // The plain stake is written as exactly 1, so a State that only ever had it keeps its bytes.
        if (fr == 0 && m == 0 && !c.grounded && P.u8(0x730)) {
            const float w = P.f32(0x728);
            if (w != 0.0f) {
                const float plain = 180.0f / (mi ? 0.33333334f : 0.43333334f);
                c.rotNeg = w < 0.0f ? 1 : 0;
                c.rotGm = std::fabs(w) == plain ? 1.0f : std::fabs(w) / plain;
            }
        }
        if ((P.u8(po::Leftward) ? 1 : 0) != gdLeft(s.rev)) c.rev = gdLeft(P.u8(po::Leftward) ? 1 : 0);   // (gdLeft is its own inverse)   // --shadowrev: a reverse ring / pad the pass fired turned the body round (a flag the old tick set, a Rotate Gameplay trigger, is not the pass's to undo)
        // The search reads the State's own dash flag (search_dp.hpp: the pruning of an airborne press child, the key): the shadow holds the dash in its own member, which the model's tick does not set when
        // the shadow's ring fire differs from the model's. lv21 t=1894: the shadow fired the dash orb uid 1620 and held vy 0 at y 197.6977, the State said "not dashing", and the search dropped the pressed child.
        c.boost = P.u8(0x95a) ? 1 : 0;   // (the exemption the next old tick reads)
        c.dashing = P.u8(0x9ec) ? 1 : 0;   // GD's own byte (m_isDashing): an anchor taken in the middle of a dash seeds the image's byte, the shadow's bookkeeping flag (sh.dashOn) starts false there
        // ...and the dash's line with it (+0x578, what the seed turns State::dashSlope back into): the old tick wrote the slope when it took the ring, and since its removal a dash the PASS started had
        // none in the State. A body seeded from the State in the middle of such a dash flew level. a custom level t=22161: a dash ring turned 42 degrees taken at t=22159, GD's body climbs 1.169 a tick, the
        // seeded one stood at y 545.766 and died on a spike at t=22186.
        if (c.dashing) c.dashSlope = (float)P.f64(0x578);
        // The speed the body runs at: State::dx is read by the search (the group key, the cap's class, the pruning of a speed change) and the old step set it when it took a speed portal. The shadow takes the portal
        // itself and applies the new speed word at the start of the next update (SelfShadow::speedDirty / pendingSpeed), so the value the next tick runs with is the pending one when there is one.
        c.dx = dxForSpeedMul((double)(sh.speedDirty ? sh.pendingSpeed : P.f32(0x9fc)));
    };
    // --shadowdual: player 2's fields as writeBody writes player 1's, and the layer's dual (the old model's own tick may have started or ended it elsewhere)
    auto writeBody2 = [&](const Mem& P, const gdcol::RingTouches* rings = nullptr) {   // (rings: the second body's touched rings when P is the step's last image)
        if (!dualOn) return;
        c.dual = T.ds.active ? 1 : 0;
        if (!T.ds.active) return;
        c.y2 = P.f32(po::PosY);
        c.vy2 = (float)P.f64(po::Vy);
        c.mode2 = (uint8_t)shadowModeOf(P);
        c.flip2 = P.u8(po::Upside) ? 1 : 0;
        c.mini2 = P.f32(0x9f8) != 1.0f ? 1 : 0;
        c.grounded2 = P.u8(po::G) ? 1 : 0;
        c.rot2 = P.f32(0x38);
        // ...and the second body's ball stake and dash line (State::rotStep2 / dashSlope2), as writeBody writes player 1's. [R] The second body is seeded from the State with its halves swapped
        // (swapHalves: rotStep <-> rotStep2, dashSlope <-> dashSlope2) and the seed reads the first names; nothing wrote the second ones since the old tick went, so a second ball seeded from the
        // State (a block that is not exact) stood at its angle and a second body seeded inside a dash flew level -- what player 1's did before its words were written back.
        if (c.mode2 == 2) c.rotStep2 = shBallStakeOf(P);
        if (P.u8(0x9ec)) c.dashSlope2 = (float)P.f64(0x578);
        c.dashing2 = P.u8(0x9ec) ? 1 : 0;
        // ...and its velocity-limit exemption (+0x95a, State::boost2), as writeBody writes player 1's. [R] The second body is seeded from the State with its halves swapped (swapHalves: boost <-> boost2)
        // and the seed writes +0x95a from `boost`; the old tick wrote boost2 and nothing has since its removal, so a second flier seeded from the State (a block that is not exact) was clamped
        // to the terminal velocity at once where its own image held the exemption. The key reads it too (search_key.hpp: a dual's boost2).
        c.boost2 = P.u8(0x95a) ? 1 : 0;
        // ...except a bit the body's next update clears before anything reads it (upd.hpp exemptionClearedNext, and no ring for the buttons phase before it): written as the 0 it is about to
        // be. [R] The key splits a dual on this bit, and a flying body inside the plain band answers nothing with it: a custom level's dual stretches were split in two by it and their caps
        // shared between the halves.
        if (c.boost2 && rings && rings->arr.empty() && gdcol::upd::exemptionClearedNext(P)) c.boost2 = 0;
        if (cFrame == 0) {   // --shadowp2dir: player 2's direction and where it stands against player 1 (writeBody has already written player 1's x)
            c.rev2 = P.u8(po::Leftward) ? 1 : 0;
            c.dx2 = P.f32(po::PosX) - c.xAbs;
        }
    };
    // (a step that ends here without a slPack still leaves the `activated` flags it set on the thread's world: list them, so the thread's next unpack clears them)
    // --shadowframe: a death only the shadow dies in a turned frame is the old model's to confirm (a custom level t=9408 in the widecap_touch window: a mini cube riding a wall meets the id-468 plates, 1.5 x 30 turned -90, and the pass
    // kills it where the game does not -- the same plate customs a custom level t=13256 shows, whose rule is not read yet)
    if (outT.died) {
        g_shDeathNote = {(long long)K.t, outT.why, outT.whyUid};
        slTrackActivated(sh, T.W);
        ++g_shStats.deaths;
        ++g_shStats.shadowOnlyDeaths;
        writeBody(sh.atExit.b.size() >= 0xc80 ? sh.atExit : sh.img);
        writeBody2(T.ds.two.atExit.b.size() >= 0xc80 ? T.ds.two.atExit : T.ds.two.img);
        c.slRef = 0; c.slRef2 = 0;
        dead = true;
        // --skyprune: GD's ceiling kill (no killer object, y at the --maxplayy bound: the loop's skyKill) is the out-of-play bound's death, not the game's rules' (--partialphys reads the kind)
        if (g_skyPrune)
            g_deadKind = (g_maxPlayY < 1e17 && outT.whyUid < 0 && (double)(sh.atExit.b.size() >= 0xc80 ? sh.atExit : sh.img).f32(po::PosY) >= g_maxPlayY - 1.0) ? kDeathPrune : kDeathPhysics;
        g_shStats.ns += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0c).count();
        return c;
    }
    // ---- the head of step t+1: this tick's button against the last tick's (measured on s_cube_dn_111832753: the model's row t+1 carries the press of the
    // plan edge GD's step t+1 takes, so the input of the model's tick t+1 is the button of the game's step t+1) ----
    // The last button is the shadow's own (holding[1], what its last head left), not State::action: at an anchor the --start state's action is the
    // model's idea, while the seed file's holding bits are the game's -- a seed holding the button with action 0 never saw a release edge, so every
    // child kept the button down and jumped on every landing (the frontier of s_cube_dn_111832753 had no state left on the ground after the first landing).
    const SelfOut* headOut = nullptr;   // (the head's teleports, for --shadowsupply's tpBack)
    if (!lat2) {
        const bool last = sh.holding[1];
        SelfIn inH; inH.dt = t_shDt;
        inH.tick = K.t;
        inH.press1 = (btn && !last) ? 1 : 0;
        inH.release1 = (!btn && last) ? 1 : 0;
        if (ctrlAny) {
            // The touched Options state this head hands on: the parent's (State::ctrlT), or what THIS tick's pass touched. The game's pass calls the trigger where the body overlaps it, the trigger
            // switches the controls there (the button is let go at once), and the next update already runs without it. Read from the parent alone, the switch came a tick late: a custom level uid 198025,
            // the dump's ctrlOff comes up on row 22578 and the wave turns down on 22579; the model's turned on 22580. [M] worker 99, 15 flights (the y at t=22562 moved by dy): the wave turns down on
            // the row after the one ctrlOff comes up on, in 15 of 15. (A latency-2 mode's head runs before its pass and keeps the parent's.)
            bool touchOff = s.ctrlT != 0;
            if (ctrlOn && !g_ctrlTouchBoxes.empty() && s.frame == 0 && sh.atExit.b.size() >= 0xc80) {
                const gdcol::Mem& X = sh.atExit;
                const bool miX = X.f32(0x9f8) != 1.0f;
                const int moX = shadowModeOf(X);
                const double phX = (moX == 4 && miX) ? kWaveContactHalfMini : playerHalf((uint8_t)moX, miX);
                const double bxX = (double)X.f32(po::PosX), byX = sh.ptEntry.ok ? (double)sh.ptEntry.posY : (double)X.f32(po::PosY);
                for (const CtrlTouchBox& b : g_ctrlTouchBoxes) {
                    if (b.holdOff && btn) continue;
                    bool over;
                    if (b.turned) {
                        const float x4[4] = {b.g[0], b.g[2], b.g[4], b.g[6]}, y4[4] = {b.g[1], b.g[3], b.g[5], b.g[7]};
                        over = gdcol::obbOverlap(gdcol::makeObb((float)bxX, (float)byX, (float)(2.0 * phX), (float)(2.0 * phX), -s.rot * 0.017453292f), gdcol::obbFromCorners(x4, y4));
                    } else over = std::fabs(bxX - b.cx) <= b.hw + phX && std::fabs(byX - b.cy) <= b.hh + phX;
                    if (over) touchOff = b.on != 0;
                }
            }
            bool w = ctrlWant((double)sh.atExit.f32(po::PosX), ctrlBit, touchOff);
            // --shadowctrlahead: this head takes the controls the NEXT update runs with; the old tick's recorded window (--ctrlwin) is a flag of the tick before the one it names (the lag rules), so the head at the end of tick t
            // reads the window at t + 1. Only the rising edge looks ahead (the lowering one keeps the old gate: a press the old tick refuses must still be refused here).
            if (ctrlOld && !w && !dualCtrl && !s.dual && !cDual && s.mode != 4 && ctrlBodyInputOffAt(K.t + 1, false)) w = true;
            inH.ctrl = ctrlEdgeOf(ctrlBit, w); ctrlBit = w;
        }
        if (dualCtrl) { const bool w2 = t_ctrlGateP2; inH.ctrl2 = ctrlEdgeOf(ctrlBit2, w2); ctrlBit2 = w2; }
        T.ds.own2 = dualOn && in2e >= 0;
        if (T.ds.own2) { const bool b2 = in2e > 0, l2 = T.ds.two.holding[1]; T.ds.press2 = (b2 && !l2) ? 1 : 0; T.ds.release2 = (!b2 && l2) ? 1 : 0; }
        SelfOut& outH = T.out(2);
        if (dualOn) { SelfOut& o2 = T.out2(2); T.ds.head(inH, outH, o2); } else
        sh.stepHead(inH, outH);
        headOut = &outH;
    }
    pm(4);
    // (E9: the old tick no longer decides a pass that went left through an object it believed used only because of the seeding rule (outT.everGuess); the shadow's guess stands. An exact ever set is the next change.)
    // --shadowrev, history: the pass went left through an object it believed already used only because the seeding guesses "everything behind the player is used" -- on the run back the player meets things its way forward
    // never overlapped (lv22 t=20396, the mini portal uid 17034 at y 2445, passed at y 2292 on the way out): the old model decides that tick
    // (--shadowrev's check of a Teleport the old tick made on the run to the left is gone with the old tick: no old tick has run here, so there are no two bodies to compare)
    double cWX = c.xAbs, cWY = c.y;   // (the body of the State, in world coordinates: a turned frame keeps travel and lateral)
    toWorld((double)c.xAbs, (double)c.y, cWX, cWY);
    if (seededM) {
        const Mem& P = sh.img;
        // --shadowseedcomplete: the anchor's first step of a seed the payload completes is the shadow's, whatever the old tick says of position, velocity and gravity (mode and deaths still decline)
        const long long tSeed = K.t - 1;
        // a ring the player's box overlaps at the anchor is in GD's touched-rings list, which the seed does not carry: a press in this step fires it in the game, and the pass would not (a custom level t=6600)
        // and the pass does not touch it again (a custom level t=2200: the ring is still under the player in the first pass, and the shadow fires it there)
        bool ringAtAnchor = false;
        if (K.orbs && fr == 0)
            for (const Obj* o : *K.orbs) {
                if (!o) continue;
                const bool atA = std::fabs((double)s.xAbs - o->cx) < 15.0 + o->hw && std::fabs((double)s.y - o->cy) < 15.0 + o->hh;
                const bool atS = std::fabs(cWX - o->cx) < 15.0 + o->hw && std::fabs(cWY - o->cy) < 15.0 + o->hh;
                if (atA && !atS) { ringAtAnchor = true; break; }
            }
        // a force box / block (type 40, ids 2069 / 3645) the pass touches pushes through GD's m_stateForce counter, which the seed does not carry (a custom level t=8600: the box adds +0.279 a tick to vy; the old tick follows GD)
        bool forceFired = false;
        for (const auto& f : outT.fired) if (f.first % 1000 == 40) { forceFired = true; break; }
        const bool seedComplete = !forceFired && g_seedHistT0 >= 0 && g_seedSpinKnown && tSeed >= g_seedHistT0 && tSeed <= g_seedHistT0 + 1
            && !s.dual && !cDual
            && s.mode <= 2 && !s.onSlope && !s.dashing && !s.gravTouch && !ringAtAnchor && s.touchRing[0] < 0 && s.touchRing[1] < 0 && s.touchRing[2] < 0 && s.touchRing[3] < 0
            && (std::isnan(g_startGmod) || std::fabs(g_startGmod - 1.0) < 1e-6);   // (a cube / ship / ball that is not on a slope, not mid-dash, touching no ring, with the layer's gravity at 1 and no gravity trigger touched: the words the seed still leaves at the template -- the slope's angle and object, the dash words, the touched-rings list, +0xb84, the touched gravity trigger's factor -- are the holes; custom level A t=9000 rides a slope, custom level B t=3000 has gravityMod 0.2, custom level C t=4600 is mid-dash, custom level D t=6600 touched the ring a fresh press fires, custom level E t=8600 stands in a gravity trigger (id 2069), and the old tick is the one that follows GD in all five)
        if (shDebugAt(K.t)) std::fprintf(shDebugOut(), "shstep t=%lld seedcheck: complete=%d (flag %d mirror %d histT0 %lld spinKnown %d rev %d/%d dual %d/%d fr %d smode %d onSlope %d boost %d gmod %.3f dashing %d touchRing %d,%d,%d,%d ringAtAnchor %d gravTouch %d usedOrb %d/%d old %d spiderJumpT %d tpArrive %d tpFromX %.2f) mdead %d dx %.4f dy %.4f dvy %.4f mode sh %d old %d up sh %d old %d\n", K.t, (int)seedComplete, 1, 1, g_seedHistT0, (int)g_seedSpinKnown, (int)s.rev, (int)c.rev, (int)s.dual, (int)c.dual, (int)fr, (int)s.mode, (int)s.onSlope, (int)s.boost, g_startGmod, (int)s.dashing, (int)s.touchRing[0], (int)s.touchRing[1], (int)s.touchRing[2], (int)s.touchRing[3], (int)ringAtAnchor, (int)s.gravTouch, s.usedOrb ? (int)s.usedOrb->uid : -1, s.usedOrb ? (int)s.usedOrb->type : -1, (int)s.usedOrbOld, (int)s.spiderJumpT, (int)s.tpArrive, (double)s.tpFromX, 0, (double)P.f32(po::PosX) - cWX, (double)P.f32(po::PosY) - cWY, P.f64(po::Vy) - (double)c.vy, shadowModeOf(P), (int)c.mode, gdUp(P.u8(po::Upside) ? 1 : 0), (int)c.flip);
        static const char* const envSeedDbgLog = std::getenv("SEEDDBG_LOG");   // read once (see shDbgUid)
        if (const char* sdl = envSeedDbgLog) {   // diagnosis (env, off): one line per seed check -- which conditions of seedComplete fail, how far the shadow's body is from the old tick's
            unsigned fb = 0;
            if (forceFired) fb |= 1u << 1;
            if (g_seedHistT0 < 0) fb |= 1u << 3;
            if (!g_seedSpinKnown) fb |= 1u << 4;
            if (g_seedHistT0 >= 0 && tSeed < g_seedHistT0) fb |= 1u << 5;
            if (g_seedHistT0 >= 0 && tSeed > g_seedHistT0 + 1) fb |= 1u << 6;
            if (s.dual || cDual) fb |= 1u << 8;
            if (s.mode > 2) fb |= 1u << 9;
            if (s.onSlope) fb |= 1u << 10;
            if (s.dashing) fb |= 1u << 11;
            if (s.gravTouch) fb |= 1u << 12;
            if (ringAtAnchor) fb |= 1u << 13;
            if (s.touchRing[0] >= 0 || s.touchRing[1] >= 0 || s.touchRing[2] >= 0 || s.touchRing[3] >= 0) fb |= 1u << 14;
            if (!(std::isnan(g_startGmod) || std::fabs(g_startGmod - 1.0) < 1e-6)) fb |= 1u << 15;
            if (std::FILE* lf = std::fopen(sdl, "a")) {
                std::fprintf(lf, "t %lld dt %lld fb %u smode %d cmode %d shmode %d mdead %d dx %.4f dy %.4f dvy %.4f flip %d/%d grounded %d/%d onslope %d dash %d\n", (long long)K.t, g_seedHistT0 >= 0 ? (long long)(tSeed - g_seedHistT0) : -999LL, fb, (int)s.mode, (int)c.mode, shadowModeOf(P), 0,
                             (double)P.f32(po::PosX) - cWX, (double)P.f32(po::PosY) - cWY, P.f64(po::Vy) - (double)c.vy, (int)(gdUp(P.u8(po::Upside) ? 1 : 0)), (int)c.flip, (int)(P.u8(po::G) ? 1 : 0), (int)c.grounded, (int)s.onSlope, (int)s.dashing);
                std::fprintf(lf, "abs t %lld shx %.4f shy %.4f shvy %.4f ox %.4f oy %.4f ovy %.4f\n", (long long)K.t, (double)P.f32(po::PosX), (double)P.f32(po::PosY), P.f64(po::Vy), cWX, cWY, (double)c.vy);
                std::fprintf(lf, "st t %lld onSlope %d slopeT %d slopeM %.4f uid0 %d uidNow %d stick %d rideLanded %d seatT %d\n", (long long)K.t, (int)s.onSlope, (int)s.slopeT, (double)s.slopeM, (int)s.slopeUid0, (int)s.slopeUidNow, (int)s.stickSlope, (int)s.rideLanded, (int)s.seatT);
                if (s.mode == 5) std::fprintf(lf, "rb t %lld w838 %.5f b98d %d b98e %d b9a4 %d b985 %d b986 %d ba24 %d b99c %d sRH %d sAct %d cRH %d cAct %d\n", (long long)K.t, P.f64(0x838), (int)P.u8(0x98d), (int)P.u8(0x98e), (int)P.u8(0x9a4), (int)P.u8(0x985), (int)P.u8(0x986), (int)P.u8(0xa24), (int)P.u8(0x99c), (int)s.rHover, (int)s.action, (int)c.rHover, (int)c.action);
                std::fclose(lf);
            }
        }
        const bool sdPos = !seedComplete && (std::fabs((double)P.f32(po::PosX) - cWX) > g_shadowSeedTol || std::fabs((double)P.f32(po::PosY) - cWY) > g_shadowSeedTol);
        const bool sdMode = shadowModeOf(P) != (int)c.mode;
        const bool sdFlip = !seedComplete && gdUp(P.u8(po::Upside) ? 1 : 0) != c.flip;
        const bool sdVy = !seedComplete && fr == 0 && std::fabs(P.f64(po::Vy) - (double)c.vy) > g_shadowSeedTol;   // --shadowseedvy: ...and the velocity (a custom level t=6601: a ring jump the seed missed left x, y equal and vy -14.5 for the old tick's +11.18)
        if (g_shadowSDiff) { ++g_shSd2.seedChecks; if (sdPos) ++g_shSd2.seedPos; if (sdMode) ++g_shSd2.seedMode; if (sdFlip) ++g_shSd2.seedFlip; if (sdVy) ++g_shSd2.seedVy; if (sdPos || sdMode || sdFlip || sdVy) ++g_shSd2.seedDecl; }
        if (sdPos || sdMode || sdFlip || sdVy) ++g_shSd2.seedSkipped;   // the seed check no longer declines (was --shadowfromseed): the shadow's answer stands; counted (--shadowsdiff)
        ++g_shStats.seededModel;
    }
    // (a dual's player 2 seeded from the model (dualHow == 3) is not checked against the model's player 2: no old tick has run, so c.y2 / c.mode2 / c.flip2 are the State before the tick and no yardstick)
    if (dualOn && T.ds.active) {   // player 2's block (packed first: player 1's pack leaves the world's `activated` list as player 1's unpack wants it)
        if (!s.dual) sl2 = ShadowLite{};   // (born in this tick: shadowDualLoad left the block alone)
        swapPlayer(sh, T.ds.two);
        slPack(sh, T.W, sl2);
        {   // player 2's block names the rings player 2 fired, not the world's flags (player 1's)
            sl2.nAct = 0;
            for (int u : sh.act2) { if (sl2.nAct >= gdcol::g_slActCap) { sl2.flags |= kSlOverflow; break; } sl2.act[sl2.nAct++] = u; }
        }
        sl2.flags2 = (uint8_t)((sl2.flags2 & ~kSlCtrlOff) | (dualCtrl && ctrlBit2 ? kSlCtrlOff : 0));   // --shadowdualctrl: player 2's flag state carried
        swapPlayer(sh, T.ds.two);
        if (sl2.flags & kSlOverflow) ++g_shStats.overflow;
        c.slRef2 = g_slStore->put(sl2);
    } else c.slRef2 = 0;
    // --shadowsupply, the repair loop's fixups: a transition record the old tick matched (fixup.hpp applyFixup) is GD's own answer for this tick, so it is applied to the shadow's body before it is packed:
    // a delta sets y / vy (and the ground flag) from the PARENT's, a kill makes the tick fatal. Without this the model's override is overwritten by the shadow's body below and the loop's records
    // never take effect -- the same death at the same place iteration after iteration (lv22 t=3267, the Steam package's 'stuck at x=4125').
    const bool fxApply = g_shadowSupply;
    if (fxApply && t_fixDelta) {
        sh.img.f32(po::PosY, s.y + t_fixDy);
        sh.img.f64(po::Vy, (double)(s.vy + t_fixDvy));
        if (t_fixG != 255) sh.img.u8(po::G, t_fixG ? 1 : 0);
        ++g_shStats.supFixDelta;
    }
    // --shadowtporbbit (constants.hpp): State::tpSkip's 8 bit (a Spider Orb moved the body this tick) from the shadow's own tick: an orb (type 43) in the pass's fired list and the body's spider stamp (+0x828) written this tick (against the block the tick started from, still `sl` here).
    bool tpOrbMoved = false;
    {
        uint64_t pre = 0;
        { const int wi = slWordIndex(0x828); if (wi >= 0) { uint32_t w2[2] = {sl.w[wi], sl.w[wi + 1]}; std::memcpy(&pre, w2, 8); } }
        bool orb = false;
        for (const auto& f : outT.fired) if (f.first == 43) orb = true;
        tpOrbMoved = orb && sh.img.get<uint64_t>(0x828) != pre;
    }
    // --shadowspiderjump (constants.hpp): the phase of the body's spider stamp (+0x828), from this tick's own firing path -- the stamp seen at the three checkpoints of the tick (the block this tick started from, the image after the update = before the pass, after the pass,
    // after the final head). A head that ran before the tail (a latency-2 row) or the final head wrote it: a button (the pushButton ring scan). The collision pass wrote it: an orb or a pad touched in the pass. Nothing changed: the bit stands with the stamp.
    bool spiderPass = (sl.flags2 & kSlSpiderPass) != 0, spiderEarly = (sl.flags2 & kSlSpiderEarly) != 0;
    if (g_shadowSpiderJump || fcheck::g_on) {
        uint64_t pre = 0;
        { const int wi = slWordIndex(0x828); if (wi >= 0) { uint32_t w2[2] = {sl.w[wi], sl.w[wi + 1]}; std::memcpy(&pre, w2, 8); } }
        const bool okE = sh.ptEntry.ok, okX = sh.atExit.b.size() >= 0xc80;
        const uint64_t en = okE ? sh.ptEntry.flipStamp : pre, ex = okX ? sh.atExit.get<uint64_t>(0x828) : en, endStamp = sh.img.get<uint64_t>(0x828);
        if (en != pre) { const bool pend = exact ? (sl.flags2 & kSlHeadPending) != 0 : (seededM && lat2); spiderPass = !pend; spiderEarly = pend; }
        if (ex != en) { spiderPass = true; spiderEarly = false; }
        if (endStamp != ex) { spiderPass = false; spiderEarly = false; }
        if (!lat2) spiderEarly = false;   // (the head the row owed is run by the first latency-1 tick: the body's clock has caught up with the old tick's count)
    }
    bool teleFresh = false;   // the pass took a Teleport portal (collide.hpp case 0x1c, fresh): it moved the body inside the pass
    for (const auto& f : outT.fired) if (f.first == 1000 + 0x1c) teleFresh = true;
    slPack(sh, T.W, sl, teleFresh);
    sl.flags2 = (uint8_t)((sl.flags2 & ~(kSlLastBtn | kSlHeadPending | kSlCtrlOff)) | (btn ? kSlLastBtn : 0) | (lat2 ? kSlHeadPending : 0) | (ctrlAny && ctrlBit ? kSlCtrlOff : 0));
    if (g_shadowSpiderJump || fcheck::g_on) sl.flags2 = (uint8_t)((sl.flags2 & ~(kSlSpiderPass | kSlSpiderEarly)) | (spiderPass ? kSlSpiderPass : 0) | (spiderEarly ? kSlSpiderEarly : 0));
    slPackLayer(own, sl);
    if (sl.flags & kSlOverflow) ++g_shStats.overflow;
    if (sl.flags & kSlAmbiguous) ++g_shStats.ambiguous;
    pm(5);
    c.slRef = g_slStore->put(sl);
    pm(6);
    // --shadowsupply, the touch boxes: markTouched again, from the words the model's tick started with, at the shadow's body. The check mode runs it at the model's own body first (the plumbing alone: the
    // result must be the model's) and keeps the model's words.
    const bool supTouch = g_shadowSupply && t_trigBefore.valid && K.trigs && !K.trigs->empty();
    auto sameTouch = [](const State& a, const State& b) { return a.trig == b.trig && a.trigT == b.trigT && std::memcmp(a.fireB, b.fireB, sizeof(a.fireB)) == 0; };
    auto supplyTouch = [&](State& d, double preY) {
        d.trig = t_trigBefore.trig;
        d.trigT = t_trigBefore.trigT;
        std::memcpy(d.fireB, t_trigBefore.fireB, sizeof(d.fireB));
        // the toggle blocks the body fired in this pass (the entries the pass added to its fired-ring record, as eventsOfPass reads them): markTouched takes player 1's press of a block from these
        static thread_local std::vector<int> bodyFired;
        static thread_local const ShadowThread* knownT = nullptr;
        bodyFired.clear();
        { const std::vector<int>& F = sh.fireOrder; size_t added = 0; for (; added < F.size(); ++added) if (!t_evPreFire.empty() && F[added] == t_evPreFire[0]) break; bodyFired.assign(F.begin(), F.begin() + (std::ptrdiff_t)added); }
        knownT = &T;
        t_bodyRingFired = &bodyFired;
        t_bodyRingKnown = [](int uid) { return knownT && shadowIdxOf(*knownT, uid) >= 0; };
        markTouched(d, K, preY, s.action != 0, input != 0 && s.action == 0, &s, input != 0);
        t_bodyRingFired = nullptr; t_bodyRingKnown = nullptr;
        static const char* const tsmtDbg = std::getenv("TSMT_DBG");   /* diagnosis (env, off): the touch latch bits the shadow supply sets, by tick */ if (const char* e = tsmtDbg) { long long a = 0, b = 0; std::sscanf(e, "%lld,%lld", &a, &b); if (K.t >= a && K.t <= b) for (const auto& tp : *K.trigs) for (size_t bi = 0; bi < (size_t)kTouchBits; ++bi) if (tp.second.test(bi) && d.trig.test(bi) && !t_trigBefore.trig.test(bi)) std::fprintf(stderr, "supplyTouchSet t=%lld uid=%d bit=%zu\n", (long long)K.t, tp.first->uid, bi); }
        if (const char* e = tsmtDbg) { long long a = 0, b = 0; std::sscanf(e, "%lld,%lld", &a, &b); if (K.t >= a && K.t <= b) std::fprintf(stderr, "supplyTouch t=%lld x=%.4f y=%.4f preY=%.4f entry y=%.4f img y=%.4f exit y=%.4f bits %llx -> %llx\n", (long long)K.t, (double)d.xAbs, (double)d.y, preY, sh.ptEntry.ok ? (double)sh.ptEntry.posY : -1.0, sh.img.b.size() >= 0xc80 ? (double)sh.img.f32(po::PosY) : -1.0, sh.atExit.b.size() >= 0xc80 ? (double)sh.atExit.f32(po::PosY) : -1.0, (unsigned long long)t_trigBefore.trig.word(0), (unsigned long long)d.trig.word(0)); }
    };
    // the lock offset: the advance of the tick the lock window was open (the old tick's flag), the new model's own: lockOff = the parent's + (this x - the parent's x)
    if (g_shadowSupply) {
        if (c.tpBack != s.tpBack) ++g_shStats.seenTpBack;
        if (c.gravTouch != s.gravTouch) ++g_shStats.seenGravTouch;
        if (c.tpTouch != s.tpTouch) ++g_shStats.seenTpTouch;
        if (c.pcRel != s.pcRel) ++g_shStats.seenPcRel;
        if (c.pcTouch != s.pcTouch) ++g_shStats.seenPcTouch;
        if (c.ctrlT != s.ctrlT) ++g_shStats.seenCtrlT;
    }
    // --shadowsupply, tpBack: the farthest x a branch has left behind it when a teleport took it back (modifiers.hpp). The new model reads the teleports the shadow made -- the pass's portals, the
    // bridged Teleport triggers, the head's -- instead of the ones the old tick fired, from the parent's value as the old step does.
    uint8_t tpBackNew = c.tpBack;
    if (g_shadowSupply && g_tpBack) {
        tpBackNew = s.tpBack;
        for (const gdcol::TpEvt& e : outT.tps) if (e.xAfter < e.xBefore) tpBackNew = tpBackAfter(tpBackNew, shadowTpBackIdx(e.uid));
        if (headOut) for (const gdcol::TpEvt& e : headOut->tps) if (e.xAfter < e.xBefore) tpBackNew = tpBackAfter(tpBackNew, shadowTpBackIdx(e.uid));
        if (tpBackNew != s.tpBack || c.tpBack != s.tpBack) {
            ++g_shStats.supTpBackRan;
            if (tpBackNew != c.tpBack) ++g_shStats.supTpBackDiff;
        }
        c.tpBack = tpBackNew;
    }
    // --shadowsupply, gravTouch: the touch-triggered gravity trigger the pass overlapped (step.hpp's box test, State::gravTouch: the value in force from the next tick). The parent's value and the other ways
    // the old tick set it (a spawn ring) stand; the box test is run again at the shadow's body, from the trigger words the old test read.
    const bool supGrav = g_shadowSupply && t_gravLoopRan;
    auto gravBoxAt = [&](double bx, double byFree, int mode, bool mini, const TouchMask& trigIn) -> uint8_t {
        const double ph = (mode == 4 && mini) ? kWaveContactHalfMini : playerHalf(mode, mini);
        uint8_t hit = 0;
        for (size_t i = 0; i < g_gravTouchBoxes.size(); ++i) {
            const GravTouchBox& b = g_gravTouchBoxes[i];
            bool dead825 = false;
            for (const int ob : b.offBits) if (trigIn.test((size_t)ob)) { dead825 = true; break; }
            if (!dead825 && b.uid >= 0 && b.ringUid < 0 && std::fabs(bx - b.cx) <= b.hw + ph && std::fabs(byFree - b.cy) <= b.hh + ph) hit = (uint8_t)(i + 1);
        }
        return hit;
    };
    if (g_shadowSDiff && !t_trigBefore.valid) { ++g_shSd2.trigBaseInvalid; if (!(c.trig == s.trig)) ++g_shSd2.trigBaseDiff; }
    const TouchMask gravTrig = t_trigBefore.valid ? t_trigBefore.trig : s.trig;   // (invalid: the old tick kept no touch words -- the State before the tick's, not the old tick's output c.trig)
    // (the value without the box test: what the old tick had before it when the box test set the final value)
    const uint8_t gravBase = (t_gravBoxApplied && c.gravTouch == t_gravBoxApplied) ? t_gravPreBox : c.gravTouch;
    // --shadowsupply, ctrlT: the touch-triggered Options trigger the pass overlapped (step.hpp's box test; the controls it switches are refused from the next tick, the button above).
    const bool supCtrl = g_shadowSupply && t_ctrlLoopRan;
    auto ctrlBoxAt = [&](double bx, double byFree, int mode, bool mini, bool in) -> int {
        const double ph = (mode == 4 && mini) ? kWaveContactHalfMini : playerHalf(mode, mini);
        int hit = -1;
        for (const CtrlTouchBox& b : g_ctrlTouchBoxes) {
            if (b.holdOff && in) continue;
            if (b.turned) {
                // A trigger placed off a right angle: GD meets its oriented box with the body's (the body's rect turned by the body's own angle, as a turned portal is met). The bound as an upright
                // box took the body ten ticks early: a custom level uid 198025 (rot -45, scale y 3.87: a 30 x 116 strip, bound 103.3 x 103.3), the wave's controls went at t=22568, GD's at 22578.
                // [M] worker 99, the y at t=22562 moved by dy (-6 .. +11, 15 flights, the button held through): the row the dump's ctrlOff comes up on is the first row the wave's 10 x 10
                // box, turned by its angle, overlaps the corners in 15 of 15; an upright 10 x 10 misses the runs between dy +1.75 and +3.2.
                const float x4[4] = {b.g[0], b.g[2], b.g[4], b.g[6]}, y4[4] = {b.g[1], b.g[3], b.g[5], b.g[7]};
                const gdcol::Obb body = gdcol::makeObb((float)bx, (float)byFree, (float)(2.0 * ph), (float)(2.0 * ph), -s.rot * 0.017453292f);
                if (gdcol::obbOverlap(body, gdcol::obbFromCorners(x4, y4))) hit = (int)b.on;
                continue;
            }
            if (std::fabs(bx - b.cx) <= b.hw + ph && std::fabs(byFree - b.cy) <= b.hh + ph) hit = (int)b.on;
        }
        return hit;
    };
    const uint8_t ctrlBase = t_ctrlBoxApplied ? t_ctrlPreBox : c.ctrlT;
    // --tpexittouch (removed): whatever moved the body after the pass by more than a step (the bridged triggers above, a touch box, a Teleport the pass's own tail did) is a teleport that the pass's boxes did not see
    // (always on: the switch --tpexittouch is removed)
    // --tpexittouch (removed): the touch boxes meet the body the pass left, not the one a Teleport trigger / touch box
    // then moved. The shadow does GJBaseGameLayer::teleportPlayer at the END of the pass (the bridged crossing triggers and tpTouch boxes in
    // shadow_step.hpp), so GD's collision pass of that tick had the pre-teleport body, and a box the teleport lands in is entered by the next
    // tick's pass. supplyTouch ran markTouched at the post-teleport body and fired it one tick early. GD's own touchfire against the dump rows
    // (the 8 reference runs of the customs fidelity set): a box first overlapped on a row where the body jumped more than 60 px in y was
    // fired on the row's tick (15 of 15; none of them one tick before it), where without a jump 225 boxes were fired on the tick before the row and 9 on its tick.
    // A custom level t=21,658: a teleport to y 615 lands in Move boxes uid 1623 / 1625 / 1626 and the platform under it starts one tick
    // early (21,660 where GD's recording starts at 21,661), so the model's cube lands on it one tick before GD's.
    if (!tpAfterExit && sh.atExit.b.size() >= 0xc80
        && (std::fabs((double)sh.img.f32(po::PosX) - (double)sh.atExit.f32(po::PosX)) > 20.0
            || std::fabs((double)sh.img.f32(po::PosY) - (double)sh.atExit.f32(po::PosY)) > 20.0))
        tpAfterExit = true;    TrigBefore modelW;
    if (supTouch) { modelW.trig = c.trig; modelW.trigT = c.trigT; std::memcpy(modelW.fireB, c.fireB, sizeof(c.fireB)); }
    if (holdPending) {   // --shadowrotapply: the body of a turning tick is held for shadowRotFinish (the old tick's body stands until then)
        const Mem& P = sh.img;
        ShadowRotHold& H = t_shRotHold;
        H.valid = true; H.t = K.t; H.bits = holdBits;
        H.x = P.f32(po::PosX); H.y = P.f32(po::PosY); H.vy = P.f64(po::Vy);
        H.up = P.u8(po::Upside) ? 1 : 0; H.left = P.u8(po::Leftward) ? 1 : 0; H.side = P.u8(po::Sideways) ? 1 : 0;
        H.grounded = P.u8(po::G) ? 1 : 0; H.mini = P.f32(0x9f8) != 1.0f ? 1 : 0; H.boost = P.u8(0x95a) ? 1 : 0; H.dashing = P.u8(0x9ec) ? 1 : 0; H.dashSlope = (float)P.f64(0x578);
        H.mode = shadowModeOf(P); H.rot = P.f32(0x38); H.rotStep = shBallStakeOf(P);
        H.dx = (float)dxForSpeedMul((double)(sh.speedDirty ? sh.pendingSpeed : P.f32(0x9fc)));
    }
    {   // with no old tick the held tick's body is not the old tick's to stand: it is written (in the frame the tick started in) as any tick's, the caller's applyRotation turns it, and shadowRotFinish puts the held one over it
    if (g_shadowColDerive && fr == 0 && !s.dual && !c.dual) {   // observation only: nothing below writes the State
        const Mem& P = sh.img;
        ShadowColStats& Q = g_shCol;
        ++Q.n;
        const int64_t sn = P.get<int64_t>(po::SnappedTo);
        const int dUid = sn > 0 ? (int)(sn - 1) : -1;
        const int oUid = c.snapObj ? c.snapObj->uid : -1;
        bool bad = false;
        if (dUid != oUid) { ++Q.snapUidDiff; bad = true; }
        else if (oUid >= 0 && std::fabs(P.f64(po::SnapDist) - (double)c.snapDist) > 1e-3) { ++Q.snapDistDiff; bad = true; }
        const uint8_t pOn = P.u8(po::OnSlope) ? 1 : 0, pWas = P.u8(po::WasOnSlope) ? 1 : 0;
        if (pOn || c.onSlope) ++Q.onSlopeTicks;
        if (pOn != (c.onSlope ? 1 : 0)) ++Q.onSlopeVsOn;
        if (pWas != (c.onSlope ? 1 : 0)) ++Q.onSlopeVsWas;
        if (pOn && c.onSlope) {
            ++Q.slopeTRide;
            const double ticks = (P.f64(po::Time) - P.f64(0x598)) / t_shSec;
            int dT = (int)std::lround(std::min(24.0, ticks)) - (int)c.slopeT;
            if (dT != 0) ++Q.slopeTDiff;
            static const char* const envColSlopeT = std::getenv("COLDERIVE_SLOPET");   // read once (see shDbgUid)
            if (envColSlopeT && Q.printed.fetch_add(1) < 60) std::fprintf(stderr, "colslopet t=%lld raw %.4f old %d prevOld %d prevOn %d\n", (long long)K.t, ticks, (int)c.slopeT, (int)s.slopeT, (int)s.onSlope);
            ++Q.slopeTDelta[dT < -4 || dT > 4 ? 8 : dT + 4];   // (index 8 = beyond +-4; +4 itself is in [8] as well, which is fine for a histogram of small steps)
        }
        const int pg = (int)P.get<int32_t>(po::CollisionBottomUid);
        if (c.groundUid >= 0 && pg != c.groundUid) ++Q.groundDiff;
        const bool nulledNow = s.snapObj && !c.snapObj && dUid >= 0;   // the old tick let go of the block this tick while the body still holds it
        static const char* const envColFired = std::getenv("COLDERIVE_FIRED");   // read once (see shDbgUid)
        if (nulledNow && envColFired && Q.printed.fetch_add(1) < 40) {
            std::fprintf(stderr, "colnull t=%lld mode %d->%d grounded %d->%d flip %d->%d fired:", (long long)K.t, (int)s.mode, (int)c.mode, (int)s.grounded, (int)c.grounded, (int)s.flip, (int)c.flip);
            for (const auto& f : outT.fired) std::fprintf(stderr, " %d/%d", f.first, f.second);
            std::fprintf(stderr, "\n");
        }
        if (bad && !envColFired && Q.printed.fetch_add(1) < 12) std::fprintf(stderr, "colderive t=%lld snap uid derived %d old %d (dist %.4f / %.4f) onSlope P %d/%d old %d slopeT old %d\n", (long long)K.t, dUid, oUid, P.f64(po::SnapDist), (double)c.snapDist, (int)pOn, (int)pWas, (int)c.onSlope, (int)c.slopeT);
    }
    writeBody(sh.img);
    if (!g_shadowQuietDumpPath.empty() && sh.img.b.size() >= 0xc80) {   // --shadowquietdump (constants.hpp)
        static std::FILE* qf = [] {   // a "%p" in the path is this process's id (the sections of an arm each get a file); the field names go beside the first file as quiet.fields
            std::string p = g_shadowQuietDumpPath;
            const size_t pp = p.find("%p");
            if (pp != std::string::npos) p.replace(pp, 2, std::to_string((long long)
#ifdef _WIN32
                GetCurrentProcessId()
#else
                getpid()
#endif
            ));
            std::FILE* f = std::fopen(p.c_str(), "wb");
            const size_t sl = p.find_last_of("/\\");
            const std::string fp = pp != std::string::npos ? (sl == std::string::npos ? std::string("quiet.fields") : p.substr(0, sl + 1) + "quiet.fields") : g_shadowQuietDumpPath + ".fields";
            if (std::FILE* nf = std::fopen(fp.c_str(), "w")) { for (const auto& fl : stser::fields()) std::fprintf(nf, "%s\n", fl.name); std::fclose(nf); }
            return f; }();
        if (qf) {
            uint8_t rec[128] = {};
            const int64_t tt = K.t;
            std::memcpy(rec, &tt, 8);
            std::vector<uint8_t> sa, ca, za;
            stser::serializeState(s, sa); stser::serializeState(c, ca);
            static const State zero{};
            stser::serializeState(zero, za);
            std::vector<std::pair<uint32_t, uint32_t>> ss, cs, zs;
            stser::fieldSlices(sa, ss); stser::fieldSlices(ca, cs); stser::fieldSlices(za, zs);
            for (size_t k = 0; k < ss.size() && k < 192; ++k) {
                if (ss[k].second != cs[k].second || std::memcmp(sa.data() + ss[k].first, ca.data() + cs[k].first, ss[k].second) != 0) rec[8 + k / 8] |= (uint8_t)(1u << (k % 8));
                if (ss[k].second != zs[k].second || std::memcmp(sa.data() + ss[k].first, za.data() + zs[k].first, ss[k].second) != 0) rec[32 + k / 8] |= (uint8_t)(1u << (k % 8));
            }
            rec[56] = outT.died ? 1 : 0; rec[57] = 0; rec[58] = (s.dual || c.dual) ? 1 : 0; rec[59] = (s.frame != 0 || c.frame != 0) ? 1 : 0;
            rec[60] = input ? 1 : 0; rec[61] = s.held; rec[62] = s.mode; rec[63] = c.mode;
            rec[64] = (uint8_t)std::min<size_t>(outT.fired.size(), 255);
            for (size_t i = 0; i < 8 && i < outT.fired.size(); ++i) { const int16_t ty = (int16_t)outT.fired[i].first; std::memcpy(rec + 66 + 2 * i, &ty, 2); }
            const bool haveB = sh.atEntry.b.size() >= 0xc80;
            const Mem& A = sh.img; const Mem& B = haveB ? sh.atEntry : sh.img;
            auto sel = [&](const Mem& M, uint8_t* o) { o[0] = M.u8(po::G); o[1] = M.u8(po::OnSlope); o[2] = M.u8(po::WasOnSlope); o[3] = M.u8(po::Upside); o[4] = M.u8(0x9ec); o[5] = (uint8_t)shadowModeOf(M); o[6] = M.f32(0x9f8) != 1.0f ? 1 : 0; };
            sel(B, rec + 82); sel(A, rec + 89);
            const int32_t sb = (int32_t)B.get<int64_t>(po::SnappedTo), sa2 = (int32_t)A.get<int64_t>(po::SnappedTo), pb = B.get<int32_t>(0x690), pa = A.get<int32_t>(0x690);
            std::memcpy(rec + 96, &sb, 4); std::memcpy(rec + 100, &sa2, 4); std::memcpy(rec + 104, &pb, 4); std::memcpy(rec + 108, &pa, 4);
            const int32_t cu[4] = {A.get<int32_t>(0x5d0), A.get<int32_t>(0x5d4), A.get<int32_t>(0x5d8), A.get<int32_t>(0x5dc)};
            std::memcpy(rec + 112, cu, 16);
            std::fwrite(rec, 1, 128, qf);
            for (size_t k = 0; k < ss.size(); ++k) {   // a hash of each field of the state BEFORE the tick (so that a tool finds the field's idle value)
                uint32_t h = 2166136261u;
                for (uint32_t b = 0; b < ss[k].second; ++b) { h ^= sa[ss[k].first + b]; h *= 16777619u; }
                std::fwrite(&h, 4, 1, qf);
            }
        }
    }
    if (!g_shadowColDumpPath.empty() && fr == 0 && !s.dual && !c.dual && sh.img.b.size() >= 0xc80) {   // --shadowcoldump
        static std::FILE* df = std::fopen(g_shadowColDumpPath.c_str(), "wb");
        if (df) {
            const int64_t tt = K.t;
            std::fwrite(&tt, 8, 1, df);
            std::fwrite(&c.slopeM, 4, 1, df);
            const int32_t i4[4] = {c.slopeUid0, c.slopeUidNow, c.stickSlope, c.groundUid};
            std::fwrite(i4, 4, 4, df);
            const uint8_t u1[12] = {c.rideLanded, c.seatT, c.seatSpd, c.ceilT, c.ceilM4, c.ceilLatch, c.ceilMode, c.ceilPin, c.stickFb, c.hitG, c.freeHalf, c.grounded};
            std::fwrite(u1, 1, 12, df);
            std::fwrite(sh.img.b.data(), 1, 0xc80, df);
        }
    }
    if (fr == 0 && !s.dual && !c.dual) {   // --shadowcolown: the body the pass left says these fields (constants.hpp)
        const Mem& P = sh.img;
        const int64_t sn = P.get<int64_t>(po::SnappedTo);
        const int dUid = sn > 0 ? (int)(sn - 1) : -1;
        const int oUid = c.snapObj ? c.snapObj->uid : -1;
        static const char* const envSnapDbgLog = std::getenv("SNAPDBG_LOG");   // read once (see shDbgUid)
        if (const char* sl = envSnapDbgLog) if (std::FILE* lf = std::fopen(sl, "a")) { std::fprintf(lf, "%lld %d %d %.4f %.4f %d\n", K.t, oUid, dUid, (double)c.snapDist, P.f64(po::SnapDist), (int)(c.snapObj != nullptr)); std::fclose(lf); }   // (diagnosis, env: the old tick's snapObj / snapDist against the body's, per tick)
        if (dUid != oUid) {
            const Obj* hit = nullptr;
            if (dUid >= 0) {
                if (K.near) for (const Obj* o : *K.near) if (o->uid == dUid) { hit = o; break; }
                if (!hit && K.slopes) for (const Obj* o : *K.slopes) if (o->uid == dUid) { hit = o; break; }
                if (!hit) hit = shadowObjByUid(dUid);   // no old tick to have left it; the level's own object
            }
            if (dUid < 0) { c.snapObj = nullptr; c.snapDist = 0.f; ++g_shCol.ownSnap; }
            else if (hit) { c.snapObj = hit; c.snapDist = (float)P.f64(po::SnapDist); ++g_shCol.ownSnap; }
            else ++g_shCol.ownMiss;   // not in the old tick's window: left as the old tick had it
        } else if (oUid >= 0) c.snapDist = (float)P.f64(po::SnapDist);
        c.onSlope = P.u8(po::OnSlope) ? 1 : 0;
        if (c.onSlope) {
            const double raw = (P.f64(po::Time) - P.f64(0x598)) / t_shSec;
            c.slopeT = (uint8_t)std::min(24L, std::max(0L, std::lround(raw) - 1));
        }
    }
    }
    if ((fr != 0 || s.dual || c.dual)) {   // the snapped block of a turned frame / of a dual's bodies (colown above writes the upright single player's only); a uid is the same in every frame, the distance is the body's own word
        auto snapFrom = [&](const Mem& Pb, const Obj*& obj, float& dist) {
            const int64_t sn = Pb.get<int64_t>(po::SnappedTo);
            const int dUid = sn > 0 ? (int)(sn - 1) : -1;
            const int oUid = obj ? obj->uid : -1;
            if (dUid != oUid) {
                const Obj* hit = nullptr;
                if (dUid >= 0) {
                    if (K.near) for (const Obj* o : *K.near) if (o->uid == dUid) { hit = o; break; }
                    if (!hit && K.slopes) for (const Obj* o : *K.slopes) if (o->uid == dUid) { hit = o; break; }
                    if (!hit) hit = shadowObjByUid(dUid, fr);
                }
                if (dUid < 0) { obj = nullptr; dist = 0.f; } else if (hit) { obj = hit; dist = (float)Pb.f64(po::SnapDist); }
            } else if (oUid >= 0) dist = (float)Pb.f64(po::SnapDist);
        };
        snapFrom(sh.img, c.snapObj, c.snapDist);
        if (T.ds.active && T.ds.two.img.b.size() >= 0xc80) snapFrom(T.ds.two.img, c.snapObj2, c.snapDist2);
    }
    if (supTouch) {
        // the pass tested the body after the update (entry) and after the collisions (c.y, the head's image)
        double supPreY = sh.ptEntry.ok ? (double)sh.ptEntry.posY : (double)c.y;
        // (never on: the switch --shadowpreyframe is removed)
        // --shadowpreyframe (default off), P9: the preY the shadow's touch supply gives markTouched is the entry body's PosY, the world y. In a turned frame the State's (xAbs, y) are the frame's (u, v) = (-Y, X) for frame 1 and
        // markTouched tests y against the box's v, so the world y was the wrong axis (the travel coordinate as the lateral one). With the flag preY is the entry body's v, the same axis as the State's y.
        // (--shadowfreshwarp and --spidertptouch, removed: the shadow never set the spider's fresh-press teleport tick, so the supply meets the boxes at the body the pass left.)
        // --shadowtouchfinal (constants.hpp): a Spider Orb in the pass's fired list whose press (the head) moved the body. The State's touch words stay the supply's (the body the pass left, as with the old tick run:
        // the supply's answer replaces the old tick's), but the level-wide first-entry tick g_touchFireT (step.hpp markTouched) is the old tick's, who meets the boxes at the body it leaves: --slambox's dedup
        // (dynamics.hpp slamBoxRecorded) compares it with the recording's entry tick, and with no old tick it read the supply's, a tick later.
        bool orbFinal = false;
        if (fr == 0 && !c.dual && sh.img.b.size() >= 0xc80 && sh.atExit.b.size() >= 0xc80 && (std::fabs((double)sh.img.f32(po::PosY) - (double)sh.atExit.f32(po::PosY)) > 0.0005 || std::fabs((double)sh.img.f32(po::PosX) - (double)sh.atExit.f32(po::PosX)) > 0.0005))
            for (const auto& f : outT.fired) if (f.first == 43) { orbFinal = true; break; }
        if (orbFinal) {
            State b2 = c;
            b2.xAbs = sh.img.f32(po::PosX);
            b2.y = sh.img.f32(po::PosY);
            supplyTouch(b2, supPreY);   // (the State it fills is dropped: markTouched's level-wide g_touchFireT is what this is for)
        }
        if (tpAfterExit && sh.atExit.b.size() >= 0xc80) {
            // --tpexittouch (removed): the boxes meet the body the pass left (before the teleport); only the touch words come back
            State b = c;
            b.xAbs = sh.atExit.f32(po::PosX);
            b.y = sh.atExit.f32(po::PosY);
            supplyTouch(b, supPreY);
            c.trig = b.trig;
            c.trigT = b.trigT;
            std::memcpy(c.fireB, b.fireB, sizeof(c.fireB));
        } else
            supplyTouch(c, supPreY);        ++g_shStats.supTouchRan;
        if (!(c.trig == modelW.trig) || c.trigT != modelW.trigT || std::memcmp(c.fireB, modelW.fireB, sizeof(c.fireB)) != 0) ++g_shStats.supTouchDiff;
    }
    writeBody2(T.ds.two.img, &T.ds.two.rt);
    if (g_camy && dbPortal) ++g_camyObs.shPortalNew;   // --camy: which path handled this portal tick (cam_y_obs.hpp)
    if (dbPortal) c.bandBranch &= (uint8_t)~kBandAnchorA;   // a mode / dual portal of this tick writes its own band: the anchor's branch A reading is gone (bands.hpp kBandAnchorA)
    // The layer's Free Mode byte (+0x311) as the pass left it (a mode / dual portal's `free`, a Camera Mode trigger) goes to the State's band branch (kBandFreeKnown / kBandFree), which the old tick wrote
    // until it was removed. The byte lives in the block's layer, and a body seeded from the State (a block that is not exact) read the State's: unknown or stale, so a ship that entered by a Free Mode
    // portal was banded again by the band of the portal before it. a custom level t=12479: the mini ship (free from t=11911, the dump's freemode 1) landed on the floor of the ball's band, y 1207 over 1200,
    // where GD's flies on (1208.995, 1207.310 ...), and died on a spike at t=12486.
    if (own.freeMode != ((c.bandBranch & kBandFreeKnown) && (c.bandBranch & kBandFree)))
        c.bandBranch = (uint8_t)((c.bandBranch & ~kBandFree) | kBandFreeKnown | (own.freeMode ? kBandFree : 0));
    // The first Static Camera exit (Y axis) past an anchor taken under branch A: GameLayer 0x349410 releases the Y axis and getMinPortalY / getMaxPortalY read branch B from then on -- the portal's own band (the one
    // the anchor's replay carries, g_anchorBandB, zoom 1) moved by the camera Offset Y. [M] 62 flights' dump.csv, 80 exits with a band before and 20+ banded ticks after: at 63 the band changes at the exit, and at 49
    // of those F = (pmin + pmax) / 2 - camoffy and H = (pmax - pmin) * camscale are constant from the first tick after it (37 with offY / zoom moving; the other 14 have another portal / exit in the 40 ticks). The state
    // takes the portal's band here (until now only the shadow's read swapped it, bandOffY-blind, and the bit stayed), so the band follows the camera (--camyband branch B writes it) and the bit leaves the dedupe key.
    if ((c.bandBranch & kBandAnchorA) && g_anchorBandB.have && (double)c.xAbs >= g_anchorBandB.exitX) {
        c.bandFloor = g_anchorBandB.floorY; c.bandCeil = g_anchorBandB.ceilY; c.bandOffY = 0.f;
        c.bandBranch &= (uint8_t)~kBandAnchorA;
        ++g_camyObs.anchorExits;
    }
    // The band a mode portal writes (a dual portal too, and while the pair is a dual the larger height of the two bodies), for a tick the shadow handled. The shadow's layer does not port a dual's band
    // (ShadowLayer::heightKnown stays false, the second player's mode decides it) and nobody else writes the State's band on such a tick, so the band stayed the last one the old tick wrote -- past the recording's end
    // (--bandtrack) a ball portal inside a dual kept the UFO's [390, 690] where GD says [390, 660] (lv16 t=12,689). The band is the old tick's rule (step.hpp's portal pass): the reference y stays the dual portal's
    // start y, H the larger of the two bodies' finished modes: bandFor(the dual portal's start y, the larger of the two bodies' finished modes' heights).
    if (dbPortal && !dbFree && !fr && (c.dual || (!dbDualOn && bandHeightFor(dbTy) > 0.0))) {
        if (dbDualOn || !c.dual) c.bandRefY = dbY;
        const double H = c.dual ? std::max(bandHeightDual(c.mode), bandHeightDual(c.mode2)) : bandHeightFor(dbTy);
        const FlyBand nb = bandFor((double)c.bandRefY, H);
        c.bandFloor = (float)nb.floorY;
                    c.bandOffY = 0.f;   // (see step.hpp)
        c.bandCeil = (float)nb.ceilY;
        // (the per-level overrides g_shipCeilSet / g_ufoCeil / g_flyFloor were false / 0 / 0 with no setter left since their flags went, 2026-08-31, so the three band overrides that stood here never ran: removed, same band)
        if (g_bandTrackEnd >= 0 && K.t > g_bandTrackEnd) c.bandBranch |= kBandPastEnd;
        if (g_camy) { c.camPortal = 1; c.camTwStart = c.camY; ++g_camyObs.shEnter; g_camyEv = CamyPortalEv{true, false, c.dual != 0, false, (long long)K.t, (int)c.mode, (float)dbY}; }   // --camy: updateDualGround's enter on a tick the shadow handled alone (step.hpp's site is not run): the tween starts from the camera of this tick
    } else if (g_camy && dbPortal && !fr) {   // --camy: a mode portal that does not enter the ground-limited camera (height 270, a Free Mode portal): updateDualGround's else branch
        c.camPortal = 0; ++g_camyObs.shLeave; g_camyEv = CamyPortalEv{true, true, c.dual != 0, dbFree, (long long)K.t, (int)c.mode, (float)dbY};
    }
    if (supCtrl) {
        const int hit = ctrlBoxAt((double)c.xAbs, sh.ptEntry.ok ? (double)sh.ptEntry.posY : (double)c.y, c.mode, c.mini != 0, t_inputEffValid ? t_inputEff != 0 : input != 0);
        uint8_t nc = (uint8_t)(hit >= 0 ? hit : ctrlBase);
        if (nc)   // (--ctrltouchlower, step.hpp: the crossing queue's enabling Options trigger lowers it again; the old tick that did this is not run)
            for (const CtrlTrig& ct : g_ctrlTrigs)
                if (!ct.gated && ct.v == -1 && ct.cx > (double)s.xAbs && ct.cx <= (double)c.xAbs) { nc = 0; break; }
        ++g_shStats.supCtrlRan;
        if (nc != c.ctrlT) ++g_shStats.supCtrlDiff;
        c.ctrlT = nc;
    }
    // (frames.hpp ctrlChanCrossed) as step.hpp's -- the old tick that does this is not run (idempotent when it ran)
    ctrlCrossedApply(c, s, dead);
    if (c.frame != 0) c.ctrlT = 0;   // (step.hpp's: no raised flag past a turn into another frame)
    if (tpTouchIdx) c.tpTouch = (uint8_t)tpTouchIdx;
    if (supGrav) {
        const uint8_t hit = gravBoxAt((double)c.xAbs, sh.ptEntry.ok ? (double)sh.ptEntry.posY : (double)c.y, c.mode, c.mini != 0, gravTrig);
        // (always on; it fires only when the trigger shadow's supply is on) a Gravity trigger the trigger shadow's timeline fired at this tick (TrigLive::gravFiresAt: a delayed / ordered Spawn's or an Event's) that the model
        // places by no x crossing: the box cli.hpp kept for it (no overlap reaches it) becomes State::gravTouch, in force from the next tick as the touch boxes' are.
        uint8_t evFire = 0;
        if (g_gravEventsHook && g_tpEventsSupplyOn())
            for (const int guid : g_gravEventsHook((long long)K.t)) {
                ++g_shStats.gravEvSeen;
                bool crossing = false;
                for (const GravTrig& gt : g_gravTrigs) crossing = crossing || gt.uid == guid;
                if (crossing) continue;
                size_t bi = 0;
                for (; bi < g_gravTouchBoxes.size(); ++bi) if (g_gravTouchBoxes[bi].uid == guid) break;
                if (bi == g_gravTouchBoxes.size()) { ++g_shStats.gravEvNoBox; continue; }
                evFire = (uint8_t)(bi + 1);
                ++g_shStats.gravEvUnmodelled;
                const double bx = g_gravTouchBoxes[bi].cx, fx = (double)c.xAbs;   // the box stands at the trigger's own x; a crossing trigger between it and where the timeline fired it would order them the other way than the game
                for (const GravTrig& gt : g_gravTrigs) if (gt.cx > std::min(bx, fx) && gt.cx <= std::max(bx, fx)) { ++g_shStats.gravEvOrderRisk; break; }
            }
        const uint8_t ng = t_gravEvent ? t_gravEvent : (evFire ? evFire : (hit ? hit : gravBase));
        if (evFire && !t_gravEvent) ++g_shStats.gravEvApplied;
        ++g_shStats.supGravRan;
        if (ng != c.gravTouch) ++g_shStats.supGravDiff;
        c.gravTouch = ng;
    }
    // --reversetrig, touch-started (modifiers.hpp g_revTouchBoxes; step.hpp's box test): the box the body's rect overlaps in this tick's collision pass fires once and turns the body round for the next
    // tick (playerTouchedTrigger -> reverseDirection -> doReversePlayer, m_isGoingLeft toggled on the spot). The next tick seeds the body from the State (revMismatch below). Neither the shadow's pass nor
    // the trigger shadow knows a Reverse trigger, and no old tick runs here.
    if (g_reverseTrig && !g_revTouchBoxes.empty() && !outT.died && !dead && c.frame == 0 && !c.dual && !s.dual) {
        const int fired = revTouchApply(c, sh.ptEntry.ok ? (double)sh.ptEntry.posY : (double)c.y);
        if (fired) g_shRevTouchFires += fired;
    }
    // State::lockOff on a tick the shadow handled. The old tick's stepOne adds the tick's advance while the lock window its box opened is running ("locked-to-player" geometry, a Move that follows the
    // player's x), and the supply copied that sum only when stepOne said it had (t_lockAdvApplied); with no old tick nothing did, so lockOff stayed 0 and every object locked to the player stood still after its first step
    // (lv19 t=20,487..20,524: the platforms of group 1 moved 1.6 px where GD moves them with the player, 61 px by t=20,524, and the cube fell through the place GD lands on, t=20,529). the old tick's stepOne lock rule: the touch
    // word and fireB it reads are the parent's (the tick's own touch is marked after the lock rule, so the first advance is the tick after the touch), the advance is this tick's: the lock box's touch bit set and
    // t - fireB - 1 <= lockTicks.
    if (g_lockBox >= 0 && s.trig.test(g_lockBox)
        && (double)((long long)K.t - (long long)s.fireB[g_lockBox] - 1) <= g_lockTicks)
        c.lockOff = s.lockOff + (float)((double)c.xAbs - (double)s.xAbs);
    pm(7);
    dead = (fxApply && t_fixKill);   // (a kill record: GD died on this transition)
    if (dead) ++g_shStats.supFixKill;
    if (shDebugAt(K.t)) std::fprintf(shDebugOut(), "shstep t=%lld OUT c.x %.4f c.y %.4f c.vy %.4f c.mini %d c.flip %d c.rev %d c.rot %.3f sh9f8 %.2f decl-ok\n", K.t, (double)c.xAbs, (double)c.y, (double)c.vy, (int)c.mini, (int)c.flip, (int)c.rev, (double)c.rot, (double)sh.img.f32(0x9f8));
    if (shDebugAt(K.t) && (T.ds.active || T.ds.ended)) { std::fprintf(shDebugOut(), "shstep t=%lld P1 rect w %.3f h %.3f 9f8 %.3f pos %.4f %.4f | P2 rect w %.3f 9f8 %.3f pos %.4f %.4f\n", K.t, (double)sh.img.f32(0x360), (double)sh.img.f32(0x364), (double)sh.img.f32(0x9f8), (double)sh.img.f32(po::PosX), (double)sh.img.f32(po::PosY), (double)T.ds.two.img.f32(0x360), (double)T.ds.two.img.f32(0x9f8), (double)T.ds.two.img.f32(po::PosX), (double)T.ds.two.img.f32(po::PosY)); std::fprintf(shDebugOut(), "shstep t=%lld P2 active %d ended %d fired:", K.t, (int)T.ds.active, (int)T.ds.ended); for (const auto& f : T.outs[1].fired) std::fprintf(shDebugOut(), " %d:%d%s", f.first >= 1000 ? f.first - 1000 : f.first, f.second, f.first >= 1000 ? "*" : ""); std::fprintf(shDebugOut(), "\n"); }
    g_shStats.ns += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0c).count();
    struct AfterClock {   // SHSTEP_PROF part 9: from here to the return, whichever return it is
        bool on; std::chrono::steady_clock::time_point t0;
        explicit AfterClock(bool o) : on(o) { if (on) t0 = std::chrono::steady_clock::now(); }
        ~AfterClock() { if (on) shProfTl().ns[9] += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(); }
    } afterClock(g_shProf.on);
    if (!c.dual) c.a1cLatch = (uint8_t)((sh.img.u8(gdcol::po::Boost) ? 1 : 0) | (c.a1cLatch & 2));   // --shadowa1c (modifiers.hpp)
    if (c.dual && T.ds.active) {   // a dual: player 2's slots from its own record (PlayerSlot::fireOrder), as player 1's below
        const std::vector<int>& F2 = T.ds.two.fireOrder;
        const int v0 = F2.empty() ? -1 : F2[0];
        if (v0 < 0) c.usedOrb2 = nullptr;
        else if (!(c.usedOrb2 && c.usedOrb2->uid == v0)) {
            const Obj* f = nullptr;
            if (K.orbs) for (const Obj* ob : *K.orbs) if (ob && ob->uid == v0) { f = ob; break; }
            c.usedOrb2 = f;
        }
        c.usedOrbOld2 = F2.size() > 1 ? F2[1] : -1;
        for (int k = 0; k < 3; ++k) c.usedOrbHist2[k] = F2.size() > (size_t)(2 + k) ? F2[(size_t)(2 + k)] : -1;
    }
    {   // --shadowfireorder: the fired-ring slots of the State are the shadow's record, newest first (usedOrb, usedOrbOld, usedOrbHist[3]); the record starts from the State's slots (shadowAdvanceImpl's seed) and the shadow's fires move it on
        const std::vector<int>& F = sh.fireOrder;
        const int u0 = F.empty() ? -1 : F[0];
        if (u0 < 0) c.usedOrb = nullptr;
        else if (!(c.usedOrb && c.usedOrb->uid == u0)) {
            const Obj* f = nullptr;
            if (K.orbs) for (const Obj* ob : *K.orbs) if (ob && ob->uid == u0) { f = ob; break; }
            c.usedOrb = f;   // (a ring the old tick's window does not hold is not a pointer the State can name: the slot stays empty and the uid stays in the record)
        }
        c.usedOrbOld = F.size() > 1 ? F[1] : -1;
        for (int k = 0; k < 3; ++k) c.usedOrbHist[k] = F.size() > (size_t)(2 + k) ? F[(size_t)(2 + k)] : -1;
        // A spawn-only toggle ring (1594, TouchTrig::spawnRing) is called in the step the body took the press (GD: ringJump inside processCommands of that step queues spawnGroup, 0x261cb0; the next
        // step's updateSpawnTriggers, which comes before its collision pass, runs the group's Toggles). The old tick fires the box one step later, on the held press (markTouched: `held` for a spawn ring),
        // so the timeline was told a step late and the pads the ring switches on were off for the collision pass that GD sees them on in (a custom level t=4774: GD's vy 10.4 from the pad, the model's 11.42).
        // The body's own fire is dated here instead: the bit is set in this pass, with this pass's tick, and markTouched's later fire finds it set.
        if (K.trigs && u0 >= 0 && u0 != (s.usedOrb ? s.usedOrb->uid : -1) && u0 != s.usedOrbOld && u0 != s.usedOrbHist[0] && u0 != s.usedOrbHist[1] && u0 != s.usedOrbHist[2]) {
            for (const auto& tb : *K.trigs) {
                const TouchTrig* T = tb.first;
                if (!T->spawnRing || T->uid != u0 || (c.trig & tb.second)) continue;
                c.trig |= tb.second;
                c.trigT = (int32_t)K.t;
                const int b = touchBitIndex(tb.second);
                if (g_touchFireT[b] < 0) g_touchFireT[b] = (int)K.t;
                c.fireB[b] = (uint16_t)K.t;
            }
        }
    }    {   // a dual state too (player 1: the input is the plan's, H the body's)   // the input fields are what the plan and the body hold; the family "input" of --fieldcheck checks the same derivation
        c.held = input ? 1 : 0;
        c.jumpBuf = input ? 1 : 0;
        c.holdDead = (input && !sh.img.u8(0x98d)) ? 1 : 0;
    }
    {   // the rings this pass fired, for the world's tog update of this step (model.hpp togWorldStep)
        const std::vector<int>& F = sh.fireOrder; size_t added = 0;
        for (; added < F.size(); ++added) if (!t_evPreFire.empty() && F[added] == t_evPreFire[0]) break;
        t_bodyFiredOut.uids.assign(F.begin(), F.begin() + (std::ptrdiff_t)added);
        t_bodyFiredOut.valid = true;
    }
    if (folPlan) { if (!dead) followTouchStart(s, K, c, folNew, folOk); c.followF[0] = folNew[0]; c.followF[1] = folNew[1]; c.followOk = folOk; }   // --shadowfollow: the pass's State fields
    portalDeriveClocks(s, c, c.flipT, c.modeT);
    portalStateDerive(s, T, sh, c, K.t);
    if (!s.dual && !c.dual) c.revOff = (float)sh.img.f64(0x9d8);   // --shadowrevoff (constants.hpp): the reverse correction still owed, the body's own (a rotated frame's seed writes it only under --revframesign)
    if (!s.dual && !c.dual) {   // --shadowrhover and --shadowspiderjump (constants.hpp): one derivation for both (neither result is an input of the other: staleDeriveTo reads c.mode, c.rHover, c.spiderJumpT, c.slRef and the image, and writes only its output)
        StaleBooks dd;
        staleDeriveTo(s, c, sh, dd);
        c.rHover = dd.rHover;
        if (g_shadowSpiderJump) c.spiderJumpT = dd.spiderJumpT;
    }
    if (g_hsShadow) {   // the key's part of the press registers from the shadow body
        State dd = c; inputHsDerive(c, sh.img, false, dd); c.gdHS = dd.gdHS; c.gdAux = dd.gdAux;
        if (s.dual && c.dual && T.ds.active && T.ds.two.img.b.size() >= 0xc80) { inputHsDerive(c, T.ds.two.img, true, dd); c.gdHS2 = dd.gdHS2; c.gdAux = dd.gdAux; }
    }
    if (c.frame == 0) {   // --shadowcolbook (constants.hpp)
        State dd = c; collisionEventsDerive(s, c, sh.img, T, dd); c.rideLanded = dd.rideLanded; c.ceilT = dd.ceilT; c.ceilM4 = dd.ceilM4;
        if (s.dual && c.dual && T.ds.active && T.ds.two.img.b.size() >= 0xc80) { collisionEventsDerive2(s, c, T.ds.two.img, dd); c.rideLanded2 = dd.rideLanded2; }
    }
    c.tpSkip = (uint8_t)((c.tpSkip & ~8) | (tpOrbMoved ? 8 : 0));   // --shadowtporbbit (constants.hpp)
    c.bandAnim = bandTimerDerive(s, own);   // --shadowband: the band tween clock from the shadow layer's portal event   // --shadowportal2: armT / fgArm / portalLatch from the shadow body   // --shadowportal: the clocks from the shadow's flip / mode
    {   // env P9DUMPF=file (gd-04, 11-10): per tick the objects the shadow's pass fired (uid:type), for the check that a pad cannot fire twice in one contact
        static std::FILE* const ff = [] { const char* e = std::getenv("P9DUMPF"); return e && e[0] ? std::fopen(e, "w") : nullptr; }();
        if (ff) { std::fprintf(ff, "%lld", K.t); for (const auto& f : outT.fired) if (f.first >= 1000) std::fprintf(ff, ",%d:%d", f.second, f.first - 1000); std::fprintf(ff, "\n"); }
        }
    {   // env EVDUMP=file (gd-03, 11-19; observation only, nothing reads it): per tick the GJGameEvents the shadow's pass raised, the producer of the Event trigger / Toggle Orb design (notes event_orb_producer_design_gd03_1119.md).
        // A line "t,event:uid:type,..." (t = K.t, the shadow step). A pad that fresh-bumped raises gdEventOfType(type) and PadActivated (bumpPlayer); a ring that fired this tick (the new head of the body's fireOrder) raises OrbActivated and gdEventOfType(type) (ringJump).
        // The ring's events are the entries the pass added to the body's fired-ring record (t_evPreFire is the record as the pass started, after the State's slots seeded it): the seeded slots are not events.
        // The same list gives State::evgate below (listened events only).
        static std::FILE* const ef = [] {   // a "%p" in the name is this process's id (the 604 harness starts one leveldp a section; the --shadowquietdump convention)
            const char* e = std::getenv("EVDUMP");
            if (!e || !e[0]) return static_cast<std::FILE*>(nullptr);
            std::string nm = e;
            const size_t k = nm.find("%p");
            if (k != std::string::npos) nm.replace(k, 2, std::to_string(
#ifdef _WIN32
                (int)GetCurrentProcessId()
#else
                (int)getpid()
#endif
            ));
            return std::fopen(nm.c_str(), "w");
        }();
        static thread_local std::vector<gdev::EvTok> evToks;
        if (ef || gdev::evAnyListened()) eventsOfPass(sh, outT.fired, evToks);
        if ((gdev::evAnyListened() || gdev::g_tapFed) && !s.dual && !c.dual) {   // State::evgate: the events of this pass that an Event trigger of the level listens for, on top of the lineage's chain (nothing on a level with no listener)
            std::vector<int> evs;
            if (gdev::evAnyListened()) for (const gdev::EvTok& t : evToks) if (gdev::evHeardAt(t.ev, (double)c.xAbs)) evs.push_back(t.ev);   // only an event a registered listener can hear (game_event.hpp evFromX)
            const size_t nGameEv = evs.size();
            if (gdev::g_tapFed) {   // the Taps' button edges of this tick whose group the shadow runs a chain in: a press the Tap has not been armed for (the player's x before the tick below its own, or at / past the Stop that ends it) is no call
                const int edge = (input && !s.action) ? 1 : (!input && s.action) ? 2 : 0;
                c.tapWin = (s.tapWin << 1) & gdev::g_tapWinMask;   // the key's window slides every tick; an edge heard now is bit 0
                std::memcpy(c.tapN, s.tapN, sizeof c.tapN);
                std::memcpy(c.tapAge, s.tapAge, sizeof c.tapAge);
                const auto& taps = gdtrig::g_togTable.taps;
                for (const gdtrig::TogTap& tp : taps)   // a counts-only Tap whose count has reached what the Counts read: the age of that, saturating at what they start keeps running
                    if (tp.chain && !tp.perPress && tp.slot >= 0 && c.tapN[tp.slot] >= tp.cap && c.tapAge[tp.slot] < tp.sat) ++c.tapAge[tp.slot];
                if (edge) {
                    const double px = crossXOf((double)s.xAbs, s.tpBack);
                    for (size_t k = 0; k < taps.size(); ++k) {
                        const gdtrig::TogTap& tp = taps[k];
                        if (!tp.chain || !(px >= tp.cx) || px >= tp.stopCx || (edge == 2 && !tp.thold)) continue;
                        if (!tp.narrow) evs.push_back(gdev::kTapTok + 4 * (int)k + edge);   // (a narrow Tap has no chain in the timelines: its presses are only counted, and shadowApplyTapMoves applies them)
                        if (tp.perPress) c.tapWin |= 1u;   // (what a counts-only Tap's presses do is told by its count and age)
                        if (tp.slot >= 0 && c.tapN[tp.slot] < tp.cap) ++c.tapN[tp.slot];   // (a press / a release alike: one edge heard)
                    }
                }
            }
            if (!evs.empty()) c.evgate = gdev::evChainAdd(s.evgate, (long long)K.t, evs);   // (the lineage the timelines are told from: the events and the Taps' edges)
            if (nGameEv) c.evKey = gdev::evDigest(s.evKey, (long long)K.t, std::vector<int>(evs.begin(), evs.begin() + (long)nGameEv));   // (what the key reads of the events: the events alone)
        }
        if (ef) {   // "t,event:uid:type,...", then ";" and the State's digest when it is not 0
            std::string line;
            char buf[48];
            for (const gdev::EvTok& t : evToks) { std::snprintf(buf, sizeof buf, ",%d:%d:%d", t.ev, t.uid, t.type); line += buf; }
            if (c.evgate) { std::snprintf(buf, sizeof buf, ";%016llx", (unsigned long long)c.evgate); line += buf; }
            std::fprintf(ef, "%lld%s\n", K.t, line.c_str());
        }
    }
    if (!c.dual) { c.revCarry = (float)sh.img.f64(0x9e0); }   // the correction in flight (+0x9e0) is the shadow's own word; State::revCarry is that (the pending one, revOff, is --shadowrevoff's)
    fcheck::applyFamilies({s, c, K.t, &sh, &T, input, &own, nullptr}, c);   // (constants.hpp) the families' derivations over the State the shadow wrote
    // A shared gravity-portal bit changes hands where x crosses the point between its two portals (g_gpHandoff, level_loader.hpp;
    // empty on every level with kGravPortalBits or fewer). The old tick did this at the start of its step (a589807d step.hpp:2875) and E9 dropped it
    // with the tick: without it bit 0 stays spent past the hand-over, and a state carried into the next call seeds portal 128 as already fired
    // (--shadowportalseed above reads the latch); after the families, which derive the latch from the state before the tick. py/test_gravity_portal_bit_share.py pins it.
    for (const GpHandoff& h : g_gpHandoff)
        if (K.xPrev < h.x && K.x >= h.x) {
            c.portalLatch &= ~GravLatch::bit(h.bit);
            c.portalLatch2 &= ~GravLatch::bit(h.bit);
        }
    t_rotBookOn = true; t_rotBookBits = holdPending ? holdBits : 0;   // --shadowrotbook: the shadow stepped; applyRotation (the caller's, next) reads which slots its pass turned with
    return c;
}

// --shadowrotapply: called by the caller of shadowAdvance right after its applyRotation (the old tick's turn), with the rotation queue's spent word from BEFORE the tick. If the pass held a body
// (t_shRotHold) and the old tick spent exactly the slots the pass turned the body with, and the three direction bytes the pass left are the ones the State says for the frame the old tick turned to,
// the shadow's body (written in that frame's coordinates, as writeBody does) replaces the old tick's rebased one: the queue's bookkeeping is the old tick's alone, nothing is counted twice, and the
// two ways to turn (this and the old tick's alone) leave the same queue state. Otherwise the shadow's block is dropped, which is what a declined tick leaves.
inline void shadowRotFinish(State& c, uint64_t spentBefore) {
    if (t_shTrigBand.valid) {   // the band a Zoom / Free Mode trigger wrote this tick, over what the turn's reset left (see ShadowTrigBand)
        t_shTrigBand.valid = false;
        c.bandFloor = t_shTrigBand.f; c.bandCeil = t_shTrigBand.c; c.bandOffY = 0.f;
    }
    ShadowRotHold& H = t_shRotHold;
    if (!H.valid) return;
    H.valid = false;
    const int frC = (int)c.frame;
    const uint64_t newly = c.rotSpent & ~spentBefore;
    const bool ok = !c.dual && (frC == 0 || frC == 1 || frC == 3) && newly == H.bits && H.side == (frC ? 1 : 0) && H.up == shGdUp(frC, c.flip) && H.left == shGdLeft(frC, c.rev);
    if (g_shadowSDiff) {   // observation: the holds, and why a hold is not applied
        ++g_shSd2.rotHolds;
        if (ok) ++g_shSd2.rotOk;
        else if (c.dual) ++g_shSd2.rotFailDual;
        else if (!(frC == 0 || frC == 1 || frC == 3)) ++g_shSd2.rotFailFrame;
        else if (newly != H.bits) ++g_shSd2.rotFailSlots;
        else if (H.side != (frC ? 1 : 0)) ++g_shSd2.rotFailSide;
        else if (H.up != shGdUp(frC, c.flip)) ++g_shSd2.rotFailUp;
        else ++g_shSd2.rotFailLeft;
    }
    if (g_shadowRotProbe) std::fprintf(shDebugOut(), "shrotfinish t=%lld accept=%d frame %d spent %llx/%llx\n", H.t, ok ? 1 : 0, frC, (unsigned long long)newly, (unsigned long long)H.bits);
    if (!ok) { c.slRef = 0; c.slRef2 = 0; ++g_shStats.declinedFrame; return; }
    double u = (double)H.x, v = (double)H.y;
    if (frC) toFrame(frC, (double)H.x, (double)H.y, u, v);
    c.xAbs = (float)u; c.y = (float)v;
    c.vy = frC == 3 ? -(float)H.vy : (float)H.vy;
    if (H.mode != (int)c.mode) ++g_shStats.modeDiff;
    c.mode = (uint8_t)H.mode;
    c.mini = H.mini;
    c.grounded = H.grounded;
    c.rot = H.rot;
    if (H.mode == 2) c.rotStep = H.rotStep;
    c.boost = H.boost;
    c.dashing = H.dashing;
    if (H.dashing) c.dashSlope = H.dashSlope;
    c.dx = H.dx;
}

// SHSTEP_MEMOCHECK=1: a call that took the tail memo is run again without it, and the two answers are compared (the State without its store slots, and the block the slot holds).
// A memo is meant to be invisible; where it is not, the search depends on which thread steps a parent's second child.
// --fieldcheck, the collision family (tools/state_families.py): State::snapObj / snapDist / onSlope / slopeT as the body the pass left says them (GD's m_objectSnappedTo, m_snapDistance, m_isOnSlope, and the ride's clock
// +0x598). The old tick keeps slopeT as ticks since the ride's first touch less one, saturated at 24 (measured on lv19: raw 1 -> 0 ... raw 25 -> 24). A derived pointer is a scratch Obj carrying the uid (the record
// writes the uid only). Single player in the world frame only: anything else is left as the old tick wrote it (no difference counted).
// P9 (iii), restructure draft 7.24: rideLanded / ceilT / ceilM4 from the shadow body's words after the tick (P) and the state before it (s); c is the old tick's, read for the one thing the body does not say (a landing this tick).
// The press counter of a ceiling ramp runs while the body is on a ramp of the isTop kind that falls in the travel (the ground flag is not read: a ball or a ship rides a floor ramp with it down); a ride's landing is the ground flag at the tick the ride begins.
inline void collisionEventsDerive(const State& s, const State& c, const gdcol::Mem& P, const ShadowThread& T, State& d) {
    using namespace gdcol;
    d.rideLanded = c.rideLanded; d.ceilT = c.ceilT; d.ceilM4 = c.ceilM4;   // what this does not derive stays the old tick's
    const bool flippedNow = P.u8(po::Upside) != 0 || s.flip != 0 || c.flip != 0;   // a flipped body never presses here: the --lawunderflip (removed) press (a ship against a floor ramp's high corner) is one witness; the 43 flipped-ship floor-ramp ticks of the sample are rides
    const bool shOn = P.u8(po::OnSlope) != 0, shG = P.u8(po::G) != 0;
    bool press = false;
    double mc = 0.0;
    if (shOn) {
        const long long wi = shadowIdxOf(T, (int)P.get<int32_t>(0x690));
        if (wi >= 0) {
            const GObj& R = T.world[(size_t)wi];
            if (R.rw > 0.0f) {
                mc = (R.sy1 - R.sy0) / (double)R.rw;
                const bool isTop = R.slopeDir == 1 || R.slopeDir == 3 || R.slopeDir == 5 || R.slopeDir == 6;   // GD's own split of the ramps (slopes.hpp slopeSeatTarget): the solid side is below the line
                const double dirSign = (P.u8(po::Leftward) != 0) ? -1.0 : 1.0;
                // the old tick presses on the underside of a ramp that falls in the travel (upright) and, for the ship, the mirror of it against a floor ramp that rises (flipped)
                press = !flippedNow && isTop && dirSign * mc < 0.0;
            }
        }
    }
    // a ramp contact the old tick seats is a landing for the launch: rideLands (vy into the ramp <= 5) or, above it, rideOverSpeed (--rideoverspeed (removed) and --rideoverspeedunder (removed), both on by default); with either off the ground flag decides, as before
    // (always on: the switch --rideoverspeed is removed)
    // --rideoverspeed (removed): a ramp contact the 5.0 landing threshold refuses (the body arrives faster
    // than that on its gravity side) still is a ride: collidedWithSlopeInternal writes the slope velocity at its
    // tail whichever way the landing went, so the exit launch happens (see the rideLanded note in step.hpp).
    // (always on: the switch --rideoverspeedunder is removed)
    // --rideoverspeedunder (removed) (ON since 2026-10-24): --rideoverspeed (removed)'s exit launch also for a contact on the UNDERSIDE (m_isUpsideDown XOR
    // isTop, see slopeUnderside: a flipped body on a floor ramp's top, an upright one under a ceiling ramp's underside). The gate
    // excluded it, on the ground that the underside path has no velocity to put back; the launch is another write (the tail of
    // collidedWithSlopeInternal), and GD makes it whatever the entry speed. Measured by injecting a flipped ship onto a floor
    // ramp's top (m 1, speed 1.6, a custom level, t=29,122): the entry vy overwritten with -12 .. -6 (fast: away from its
    // gravity at 6.1 .. 8.0, the ceiling of the ship's fall) gives vy 0 on the contact tick (29,124, y 765.000) and +4.107 on
    // the next, which is slopeExitVy(1, ship) 10.27 x the ride factor 0.4; entering at -5 / -4 / -3 gives the +2.000 nudge on the
    // contact tick and the same +4.107 on the next. The model had the slow entries (they landed) and none of the fast ones.
    const uint8_t landed = (uint8_t)1;
    d.rideLanded = (shOn && !press) ? (s.onSlope ? s.rideLanded : landed) : s.rideLanded;
    if (press) {
        int v = std::min<int>((int)s.ceilT + 1, 200);
        if (s.onSlope && s.ceilT == 0) v = std::min<int>(std::max<int>(v, (int)s.slopeT + 1), 200);
        d.ceilT = (uint8_t)v;
        d.ceilM4 = (uint8_t)std::min<long>(255, std::lround(std::fabs(mc) * 4.0));
    } else if (s.ceilT > 0) {
        if (c.grounded && !s.grounded) { d.ceilT = s.ceilT; d.ceilM4 = s.ceilM4; }   // landed this tick: the release waits
        else { d.ceilT = 0; d.ceilM4 = 0; }
    } else { d.ceilT = s.ceilT; d.ceilM4 = s.ceilM4; }
}inline void collisionEventsDerive2(const State& s, const State& c, const gdcol::Mem& P, State& d) {
    using namespace gdcol;
    d.rideLanded2 = c.rideLanded2;
    const bool shOn = P.u8(po::OnSlope) != 0, shG = P.u8(po::G) != 0;
    if (shOn) d.rideLanded2 = s.onSlope2 ? s.rideLanded2 : (uint8_t)(shG ? 1 : 0);
    else d.rideLanded2 = s.rideLanded2;
}
// P9 (iii) press registers, restructure draft 7.25: the part of State::gdHS / gdAux the search key reads -- H and S (gdHS & 3) with the consumers the old tick has spent in the press step but applies one step later
// (gdAux bits 1 / 2: S / H "pend") -- is the shadow body's own H (+0x98d) and S (+0x98e) after the tick, the pend already applied. gdHSb (the scratch copy for the update part, "nothing reads it across steps") and the Hm / Sm / slot / RF bits
// (no reader outside the old tick) are not derived. A second body's (gdHS2, gdAux's high nibble) from player 2's image.
inline void inputHsDerive(const State& c, const gdcol::Mem& P, bool second, State& d) {
    using namespace gdcol;
    const uint8_t hs = (uint8_t)((P.u8(0x98d) ? kHsH : 0) | (P.u8(0x98e) ? kHsS : 0));
    if (!second) { d.gdHS = (uint8_t)((c.gdHS & ~3) | hs); d.gdAux = (uint8_t)(c.gdAux & ~6); }
    else { d.gdHS2 = (uint8_t)((c.gdHS2 & ~3) | hs); d.gdAux = (uint8_t)(c.gdAux & ~(6 << 4)); }
}
// P9 STALE fields with a reader outside the old tick (notes/p9_stale_readers_1111_gd03.md): revOff (the search key), spiderJumpT (the key, inside the grace window) and rHover (the key, the pressMoot prune, the robot seed),
// from the shadow body's own words: the reverse correction still owed (+0x9d8), the spider's flip stamp (+0x828) against the clock, and the robot's hover budget (+0x838, +0x9a4). Single body.
inline void staleDerive(const State& s, const State& c, const gdcol::SelfShadow& sh, State& d) { staleDeriveTo(s, c, sh, d); }
template <class Out> inline void staleDeriveTo(const State& s, const State& c, const gdcol::SelfShadow& sh, Out& d) {
    using namespace gdcol;
    const Mem& P = sh.img;
    d.revOff = (float)P.f64(0x9d8);
    const double stamp = P.f64(0x828);
    if (stamp != 0.0) {   // (the old tick counts 0 on the tick of the jump; the body's clock is a tick on in some cases and not in others -- a tap stamps in the button pass, a pad or orb in the collision pass -- see the note)
        const double age = (P.f64(po::Time) - stamp) / t_shSec;
        const ShadowLite* bl = (g_shadowSpiderJump || fcheck::g_on) ? slOf(c.slRef) : nullptr;   // --shadowspiderjump: the phase bit the firing path wrote (collision pass = the old tick's count is one less)
        const long ph = bl ? ((bl->flags2 & kSlSpiderPass) ? 1 : 0) - ((bl->flags2 & kSlSpiderEarly) ? 1 : 0) : 0;
        d.spiderJumpT = (uint8_t)std::min<long>(spiderJumpGraceTicks(), std::max<long>(0, std::lround(age) - ph));
    } else d.spiderJumpT = c.spiderJumpT;
    // the robot's hover: GD's release flag (+0x9a4) down with the button held and no unspent press, in the image after the update (atExit: a hover tick) or after the button pass (the press that starts one); the budget spent is +0x838
    // over the step it grows by (dt * 0.9 * 0.1)
    const bool hx = sh.atExit.b.size() >= 0xc80;
    const Mem& X = hx ? sh.atExit : P;
    const bool exitHover = X.u8(0x9a4) == 0 && X.u8(0x98d) && !X.u8(0x98e);
    const bool imgHover = P.u8(0x9a4) == 0 && P.u8(0x98d) && !P.u8(0x98e);
    if (c.mode == 5 && (exitHover || imgHover)) {
        const double tw = timeWarpAt((double)c.xAbs);
        const float dtS = (tw != 1.0) ? (float)(0.25 * (double)(float)tw) : 0.25f;
        const double inc = (double)((dtS * 0.9f) * 0.1f);
        const double w = exitHover ? X.f64(0x838) : P.f64(0x838);
        d.rHover = (uint8_t)std::max(0, (int)robotHoverTicks(tw) - (int)std::lround(w / inc));
    } else d.rHover = c.mode == 5 ? (uint8_t)0 : c.rHover;   // (a swing's hover budget is the old tick's: left)
    (void)s;
}inline void registerCollisionFamily() {
    static bool done = false;
    if (done) return;
    done = true;
    // the rest of the family has no body byte it equals (tools/col_map.py over lv3, 11, 16-20: 107,645 ticks, every offset): the hypothesis "the old tick leaves it as the tick before had it" is counted here, so the number of ticks the old tick
    // really changes each field in is known (collision_carry; single player, world frame)
    fcheck::registerFamily("collision_carry", {"slopeM", "slopeUid0", "slopeUidNow", "rideLanded", "seatT", "seatSpd", "ceilT", "ceilM4", "ceilLatch", "ceilMode", "ceilPin", "stickFb", "stickSlope", "hitG", "freeHalf", "groundUid"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || cx.c.frame != 0 || cx.c.dual || cx.s.dual) return;
        d.slopeM = cx.s.slopeM; d.slopeUid0 = cx.s.slopeUid0; d.slopeUidNow = cx.s.slopeUidNow; d.rideLanded = cx.s.rideLanded; d.seatT = cx.s.seatT; d.seatSpd = cx.s.seatSpd;
        d.ceilT = cx.s.ceilT; d.ceilM4 = cx.s.ceilM4; d.ceilLatch = cx.s.ceilLatch; d.ceilMode = cx.s.ceilMode; d.ceilPin = cx.s.ceilPin; d.stickFb = cx.s.stickFb; d.stickSlope = cx.s.stickSlope;
        d.hitG = cx.s.hitG; d.freeHalf = cx.s.freeHalf; d.groundUid = cx.s.groundUid;
    });
    fcheck::registerFamily("collision_events", {"rideLanded", "ceilT", "ceilM4"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || !cx.thread || cx.c.frame != 0) return;
        const gdcol::SelfShadow& sh = *static_cast<const gdcol::SelfShadow*>(cx.shadow);
        collisionEventsDerive(cx.s, cx.c, sh.img, *static_cast<const ShadowThread*>(cx.thread), d);
    });
    // the second body of a dual: rideLanded2 by the same rule from player 2's image (its press counter ceilT2 / ceilM42 has no reader outside the old tick: not derived)
    fcheck::registerFamily("collision_events2", {"rideLanded2"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || !cx.thread || cx.c.frame != 0 || !cx.c.dual || !cx.s.dual) return;
        const ShadowThread& T = *static_cast<const ShadowThread*>(cx.thread);
        if (!T.ds.active || T.ds.two.img.b.size() < 0xc80) return;
        collisionEventsDerive2(cx.s, cx.c, T.ds.two.img, d);
    });
    fcheck::registerFamily("input_hs", {"gdHS", "gdHS2"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || !cx.thread || !g_hsShadow) return;
        const gdcol::SelfShadow& sh = *static_cast<const gdcol::SelfShadow*>(cx.shadow);
        State dd = cx.c;
        inputHsDerive(cx.c, sh.img, false, dd);
        if (cx.s.dual && cx.c.dual) {
            const ShadowThread& T = *static_cast<const ShadowThread*>(cx.thread);
            if (!T.ds.active || T.ds.two.img.b.size() < 0xc80) return;
            inputHsDerive(dd, T.ds.two.img, true, dd);
        }
        // the compare is on the key's part: H, S of each body with the old tick's own pend bits put back (the flag applies them; the old tick's byte has the spent bit still up, so a difference here is a tick the body and the old tick disagree on the effective press)
        const uint8_t pend1 = (uint8_t)(((cx.c.gdAux & 2) ? kHsS : 0) | ((cx.c.gdAux & 4) ? kHsH : 0));
        const uint8_t pend2 = (uint8_t)((((cx.c.gdAux >> 4) & 2) ? kHsS : 0) | (((cx.c.gdAux >> 4) & 4) ? kHsH : 0));
        d.gdHS = (uint8_t)((cx.c.gdHS & ~3) | ((dd.gdHS & 3) | pend1));
        d.gdHS2 = (uint8_t)((cx.c.gdHS2 & ~3) | ((dd.gdHS2 & 3) | pend2));
    });
    fcheck::registerFamily("stale_books", {"revOff", "spiderJumpT", "rHover"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || cx.c.dual || cx.s.dual) return;
        staleDerive(cx.s, cx.c, *static_cast<const gdcol::SelfShadow*>(cx.shadow), d);
    });
    // the 11-11 audit (revOff): the raw comparison above has no tolerance (the old tick keeps the correction in a float it pays back in float steps, so a rounding apart is a difference there). What the search key reads is the 0.25 px bucket (revOffBucket):
    // this family counts a tick only when the two values fall in different buckets, so the number of ticks the key would have told apart is its own line, apart from the raw count.
    fcheck::Family* staleKey = fcheck::registerFamily("stale_revoff_key", {"revOff"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || cx.c.dual || cx.s.dual) return;
        staleDerive(cx.s, cx.c, *static_cast<const gdcol::SelfShadow*>(cx.shadow), d);
        if (revOffBucket(d.revOff) == revOffBucket(cx.c.revOff)) d.revOff = cx.c.revOff;
    });
    fcheck::declareExtraWrites(staleKey, {"spiderJumpT", "rHover"});   // staleDerive writes them too; this family keeps only revOff
    // the timers with an update law of their own (step.hpp: c.flipT / c.modeT from the gravity direction and the mode the tick leaves, c.bandAnim stepped every tick): derived from the state before and the fields the shadow wrote.
    // A difference is a tick the old tick restarted the clock in for a reason the shadow side must supply (a ground portal for bandAnim).
    fcheck::registerFamily("timer_laws", {"flipT", "modeT", "bandAnim"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || cx.c.dual || cx.s.dual) return;
        d.flipT = (gdUpOf(cx.c) != gdUpOf(cx.s)) ? 0 : (uint8_t)std::min<int>(kFlipGraceTicks, (int)cx.s.flipT + 1);
        d.modeT = (cx.c.mode != cx.s.mode) ? 0 : (uint8_t)std::min<int>(kFlipGraceTicks, (int)cx.s.modeT + 1);
        if (g_staticCamCol) d.bandAnim = bandAnimStep(cx.s.bandAnim);
    });
    fcheck::registerFamily("collision", {"snapObj", "snapDist", "onSlope", "slopeT"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || cx.c.frame != 0 || cx.c.dual || cx.s.dual) return;
        const gdcol::SelfShadow& sh = *static_cast<const gdcol::SelfShadow*>(cx.shadow);
        const gdcol::Mem& P = sh.img;
        using namespace gdcol;
        // (snapObj / snapDist: --shadowcolown has written the real Obj and its distance (a scratch pointer carrying only the uid would stand in the next tick's State), so the family leaves them as they are)
        d.onSlope = P.u8(po::OnSlope) ? 1 : 0;
        if (d.onSlope) {
            const double raw = (P.f64(po::Time) - P.f64(0x598)) / t_shSec;
            d.slopeT = (uint8_t)std::min(24L, std::max(0L, std::lround(raw) - 1));
        }   // (off a ride the old tick carries slopeT on: left as it is)
    });
}
// input2: the second body's own button (a plan's input bit 1) in a two-player level (g_twoPlayer); -1: not given (the ghost's plan in a search, secondLevelAt, else 0)
inline State shadowAdvance(const State& s, int input, const StepCtx& K, bool& dead, int input2 = -1) {
    static const bool chk = std::getenv("SHSTEP_MEMOCHECK") != nullptr;
    static const bool pure = std::getenv("SHSTEP_PURECHECK") != nullptr;
    if (pure) {   // the same call on the thread's second shadow, for one call in seven: the answers must agree whatever each shadow stepped before
        bool d1 = false;
        State c1 = shadowAdvanceImpl(s, input, K, d1, input2);
        dead = d1;
        if ((uint64_t)(K.t * 2654435761ull + (uint64_t)(s.xAbs * 16.0)) % 7 != 0) return c1;
        t_shAlt = true;
        bool d2 = false;
        State c2 = shadowAdvanceImpl(s, input, K, d2, input2);
        t_shAlt = false;
        static std::atomic<long long> nChecked{0}, nBad{0};
        ++nChecked;
        State a = c1, b = c2;
        a.slRef = b.slRef = 0; a.slRef2 = b.slRef2 = 0;
        const unsigned char* pa = reinterpret_cast<const unsigned char*>(&a);
        const unsigned char* pb = reinterpret_cast<const unsigned char*>(&b);
        std::string diff;
        for (size_t i = 0; i < sizeof(State); ++i) if (pa[i] != pb[i]) diff += " S+" + std::to_string(i);
        const gdcol::ShadowLite* l1 = gdcol::slOf(c1.slRef); const gdcol::ShadowLite* l2 = gdcol::slOf(c2.slRef);
        if (l1 && l2) {
            const unsigned char* qa = reinterpret_cast<const unsigned char*>(l1);
            const unsigned char* qb = reinterpret_cast<const unsigned char*>(l2);
            for (size_t i = 0; i < sizeof(gdcol::ShadowLite); ++i) if (qa[i] != qb[i]) diff += " L+" + std::to_string(i);
        } else if ((l1 != nullptr) != (l2 != nullptr)) diff += " block";
        if (d1 != d2) diff += " dead";
        if (!diff.empty() && l1 && l2) {
            auto lst = [](const char* nm, const int32_t* v, int n) { std::string r = std::string(" ") + nm + "["; for (int i = 0; i < n; ++i) r += (i ? "," : "") + std::to_string(v[i]); return r + "]"; };
            diff += " |" + lst("gate1", l1->gate, l1->nGate) + lst("gate2", l2->gate, l2->nGate) + lst("ever1", l1->ever, l1->nEver) + lst("ever2", l2->ever, l2->nEver) + lst("act1", l1->act, l1->nAct) + lst("act2", l2->act, l2->nAct);
            {   // the objects that are in one gate and not the other: where each thread's world has them
                std::set<int> gd; for (int i = 0; i < l1->nGate; ++i) gd.insert(l1->gate[i]); for (int i = 0; i < l2->nGate; ++i) gd.insert(l2->gate[i]);
                for (int u : gd) {
                    const auto it = g_shBase.idx.find(u);
                    if (it == g_shBase.idx.end()) continue;
                    for (int alt = 0; alt < 2; ++alt) {
                        t_shAlt = alt != 0;
                        const gdcol::GObj& G = shadowThread().world[it->second];
                        char b[300]; std::snprintf(b, sizeof b, " | obj %d %s: cx %.3f cy %.3f rx %.3f ry %.3f rw %.2f rh %.2f rot %.3f inact %d act %d mover %d type %d", u, alt ? "alt" : "main", (double)G.cx, (double)G.cy, (double)G.rx, (double)G.ry, (double)G.rw, (double)G.rh, (double)G.rot, (int)G.inactive, (int)G.activated, (int)g_shBase.mover[it->second], (int)G.type);
                        diff += b;
                    }
                    t_shAlt = false;
                    { char b[160]; std::snprintf(b, sizeof b, " | obj %d base rot %.3f oriented %d bw %.2f bh %.2f", u, (double)g_shBase.objs[it->second].rot, (int)g_shBase.objs[it->second].oriented, (double)g_shBase.objs[it->second].bw, (double)g_shBase.objs[it->second].bh); diff += b; }
                }
            }
            diff += " xAbs " + std::to_string(c1.xAbs) + "/" + std::to_string(c2.xAbs) + " mode " + std::to_string((int)c1.mode) + "/" + std::to_string((int)c2.mode) + " flags " + std::to_string((int)l1->flags) + "/" + std::to_string((int)l2->flags);
        }
        if (!diff.empty()) {
            const long long nb = ++nBad;
            if (nb <= 60) std::fprintf(stderr, "SHSTEP_PURECHECK: t=%lld x=%.4f y=%.4f vy=%.4f mode %d input %d slRef %u (%lld of %lld):%s | y %.5f/%.5f vy %.5f/%.5f\n", K.t, (double)s.xAbs, (double)s.y, (double)s.vy, (int)s.mode, input, s.slRef, nb, nChecked.load(), diff.c_str(), (double)c1.y, (double)c2.y, (double)c1.vy, (double)c2.vy);
        }
        return c1;
    }
    if (!chk) return shadowAdvanceImpl(s, input, K, dead, input2);
    bool d1 = false;
    State c1 = shadowAdvanceImpl(s, input, K, d1, input2);
    dead = d1;
    if (!t_shMemoHit) return c1;
    shadowThread().memo.valid = false;
    bool d2 = false;
    State c2 = shadowAdvanceImpl(s, input, K, d2, input2);
    static std::atomic<long long> nChecked{0}, nBad{0};
    ++nChecked;
    State a = c1, b = c2;
    a.slRef = b.slRef = 0; a.slRef2 = b.slRef2 = 0;
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(&a);
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(&b);
    std::string diff;
    for (size_t i = 0; i < sizeof(State); ++i) if (pa[i] != pb[i]) diff += " S+" + std::to_string(i);
    const gdcol::ShadowLite* l1 = gdcol::slOf(c1.slRef); const gdcol::ShadowLite* l2 = gdcol::slOf(c2.slRef);
    if (l1 && l2) {
        const unsigned char* qa = reinterpret_cast<const unsigned char*>(l1);
        const unsigned char* qb = reinterpret_cast<const unsigned char*>(l2);
        for (size_t i = 0; i < sizeof(gdcol::ShadowLite); ++i) if (qa[i] != qb[i]) diff += " L+" + std::to_string(i);
    }
    if (l1 && l2 && !diff.empty()) diff += " flags " + std::to_string((int)l1->flags) + "/" + std::to_string((int)l2->flags) + " flags2 " + std::to_string((int)l1->flags2) + "/" + std::to_string((int)l2->flags2) + " pruneW " + std::to_string(l1->pruneW) + "/" + std::to_string(l2->pruneW) + " (hit/fresh)";
    if (d1 != d2) diff += " dead";
    if (!diff.empty()) {
        const long long nb = ++nBad;
        if (nb <= 40) std::fprintf(stderr, "SHSTEP_MEMOCHECK: memo hit differs from a fresh step at t=%lld x=%.4f y=%.4f mode %d input %d (%lld of %lld):%s\n", K.t, (double)s.xAbs, (double)s.y, (int)s.mode, input, nb, nChecked.load(), diff.c_str());
    }
    return c2;
}

// printed by the replay and the search when the flag is on
// The counts shadowStepPrint's `shadowstep declined:` line prints, as one line for the mod to relay into result.txt (SearchOutcome::shstep). Nothing reads it.
inline std::string shadowStepSummary() {
    const ShadowStepStats& S = g_shStats;
    char b[320];
    std::snprintf(b, sizeof(b), "eligible %lld stepped %lld teleport %lld mini %lld inexact %lld warp %lld other %lld (dual %lld frame %lld reverse %lld platformer %lld) slope %lld seedcheck %lld",
                  S.eligible.load(), S.stepped.load(), S.declinedTeleport.load(), S.declinedMini.load(), S.declinedInexact.load(), S.declinedWarp.load(), S.declinedOther.load(),
                  S.declinedDual.load(), S.declinedFrame.load(), S.declinedRev.load(), S.declinedPlat.load(), S.declinedSlope.load(), S.declinedSeedCheck.load());
    return b;
}
inline void shadowStepPrint() {
    fcheck::print();
    if (!g_shModelDeaths.empty()) {
        std::vector<std::pair<std::string, ShModelDeath>> v(g_shModelDeaths.begin(), g_shModelDeaths.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
        for (size_t i = 0; i < v.size() && i < 40; ++i)
            std::printf("shadowstep model-only death: %s x%lld first t=%lld x=%.2f y=%.2f mode %d mini %d\n", v[i].first.c_str(), v[i].second.n, v[i].second.t, v[i].second.x, v[i].second.y, v[i].second.mode, v[i].second.mini);
        if (const char* fp = std::getenv("SHSTEP_MODELDEATHS"); fp && fp[0] && std::strcmp(fp, "1") != 0) {
            if (FILE* fo = std::fopen(fp, "a")) {
                for (const auto& e : v)
                    std::fprintf(fo, "%s\t%s\t%lld\t%lld\t%.4f\t%.4f\t%d\t%d\n", g_shObjPath.c_str(), e.first.c_str(), e.second.n, e.second.t, e.second.x, e.second.y, e.second.mode, e.second.mini);
                std::fclose(fo);
            }
        }
    }
    {
        if (!g_shadowStepModes) return;
        const ShadowStepStats& S = g_shStats;
        std::printf("shadowstep: eligible %lld stepped %lld (%.2f us each) seeded file %lld model %lld; declined teleport %lld mini %lld inexact %lld warp %lld other %lld slope %lld seedcheck %lld; "
                    "deaths %lld (shadow only %lld), model-only deaths overridden %lld; mode / gravity / size taken from the shadow where the model differed %lld / %lld / %lld; "
                    "gravity bridged %lld, teleport triggers bridged %lld; overflow %lld ambiguous %lld\n",
                    S.eligible.load(), S.stepped.load(), S.stepped.load() ? (double)S.ns.load() / 1000.0 / (double)S.stepped.load() : 0.0, S.seededFile.load(), S.seededModel.load(),
                    S.declinedTeleport.load(), S.declinedMini.load(), S.declinedInexact.load(), S.declinedWarp.load(), S.declinedOther.load(), S.declinedSlope.load(), S.declinedSeedCheck.load(), S.deaths.load(), S.shadowOnlyDeaths.load(),
                    S.modelOnlyDeaths.load(), S.modeDiff.load(), S.flipDiff.load(), S.sizeDiff.load(), S.gravBridged.load(), S.tpBridged.load(), S.overflow.load(), S.ambiguous.load());
        if (S.seedPrevGiven.load() + S.seedPrevNone.load() > 0) std::printf("shadowstep: seeds from the model took the previous position %lld, had none %lld; objects the previous position left fresh %lld\n", S.seedPrevGiven.load(), S.seedPrevNone.load(), gdcol::g_seedPrevFresh.load());
        if (gdcol::g_keyObs && g_prevUnsoundCmp.load() + g_prevUnsoundNone.load() > 0) std::printf("shadowstep: KEYOBS dedupe dropped states compared %lld, with no previous position %lld; the previous position would have changed the seed's fresh set (rect test, static objects) for %lld\n", g_prevUnsoundCmp.load(), g_prevUnsoundNone.load(), g_prevUnsoundDiff.load());
        {   // the old-tick fallbacks by site (print only); the first column of the file lines is the objrects path of the run
            long long nmo = 0, nsite = 0;
            for (int m = 0; m < 9; ++m) nmo += S.notModeOn[m].load();
            for (int l = 0; l < ShadowStepStats::kDeclineSites; ++l) nsite += S.declineSite[l].load();
            const long long ticks = S.eligible.load() + nmo;
            std::string line = "shadowstep: old-tick fallbacks: ticks " + std::to_string(ticks) + ", fallbacks " + std::to_string(nmo + nsite) + " (mode not on " + std::to_string(nmo) + ", declined " + std::to_string(nsite) + ")";
            const std::string stamp = "\t" + std::to_string((long long)std::time(nullptr)) + "\n";   // (the in-process run keeps one count per process: the time says which call a row came from)
            std::string rows = g_shObjPath + "\tticks\t" + std::to_string(ticks) + stamp + g_shObjPath + "\tstepped\t" + std::to_string(S.stepped.load()) + stamp;
            {   // the eligibility decline (the site of `not eligible`) by cause: the first that applies of dual / frame / reverse / platformer
                const long long cs[4] = {S.eligCause[0].load(), S.eligCause[1].load(), S.eligCause[2].load(), S.eligCause[3].load()};
                static const char* const cn[4] = {"dual", "frame", "rev", "plat"};
                for (int k = 0; k < 4; ++k) if (cs[k]) { line += std::string(" elig-") + cn[k] + "=" + std::to_string(cs[k]); rows += g_shObjPath + "\teligcause:" + cn[k] + "\t" + std::to_string(cs[k]) + stamp; }
            }            for (int m = 0; m < 9; ++m) if (S.notModeOn[m].load()) { line += " notmodeon[" + std::to_string(m) + "]=" + std::to_string(S.notModeOn[m].load()); rows += g_shObjPath + "\tnotmodeon:" + std::to_string(m) + "\t" + std::to_string(S.notModeOn[m].load()) + stamp; }
            for (int k = 0; k < 8; ++k) if (g_shUnsup[k].load()) { line += std::string(" unsupported-") + kShUnsupName[k] + "=" + std::to_string(g_shUnsup[k].load()); rows += g_shObjPath + "\tunsup:" + kShUnsupName[k] + "\t" + std::to_string(g_shUnsup[k].load()) + stamp; }
            if (g_shRevTouchFires.load()) { line += " revtouch-fires=" + std::to_string(g_shRevTouchFires.load()); rows += g_shObjPath + "\trevtouch:fires\t" + std::to_string(g_shRevTouchFires.load()) + stamp; }
            for (int l = 0; l < ShadowStepStats::kDeclineSites; ++l) if (S.declineSite[l].load()) { line += " L" + std::to_string(l) + "=" + std::to_string(S.declineSite[l].load()); rows += g_shObjPath + "\tsite:" + std::to_string(l) + "\t" + std::to_string(S.declineSite[l].load()) + stamp; }
            if (g_shProbe.probes) {   // the --sametick probe's trial ticks, apart from the run's own (ShProbeScope)
                const ShProbeCensus& P = g_shProbe;
                line += " | sametick probes " + std::to_string(P.probes) + " (eligible " + std::to_string(P.eligible) + " stepped " + std::to_string(P.stepped) + ")";
                rows += g_shObjPath + "\tprobe:probes\t" + std::to_string(P.probes) + stamp + g_shObjPath + "\tprobe:ticks\t" + std::to_string(P.eligible) + stamp + g_shObjPath + "\tprobe:stepped\t" + std::to_string(P.stepped) + stamp;
                for (int k = 0; k < 4; ++k) if (P.eligCause[k]) rows += g_shObjPath + "\tprobe:eligcause:" + std::string(k == 0 ? "dual" : k == 1 ? "frame" : k == 2 ? "rev" : "plat") + "\t" + std::to_string(P.eligCause[k]) + stamp;
                for (int m = 0; m < 9; ++m) if (P.notModeOn[m]) rows += g_shObjPath + "\tprobe:notmodeon:" + std::to_string(m) + "\t" + std::to_string(P.notModeOn[m]) + stamp;
                for (int l = 0; l < ShadowStepStats::kDeclineSites; ++l) if (P.declineSite[l]) rows += g_shObjPath + "\tprobe:site:" + std::to_string(l) + "\t" + std::to_string(P.declineSite[l]) + stamp;
            }            std::printf("%s\n", line.c_str());
            static const char* const declinesPath = std::getenv("SHSTEP_DECLINES");   // read once (tools/check_getenv.py)
            if (declinesPath && declinesPath[0])
                if (std::FILE* df = std::fopen(declinesPath, "ab")) { std::fwrite(rows.data(), 1, rows.size(), df); std::fclose(df); }
        }
        if (S.gravEvSeen.load()) std::printf("shadowstep: trigger-shadow Gravity fires %lld, placed by a box %lld (set gravTouch %lld, a crossing trigger between the box and the fire %lld), no box %lld\n", S.gravEvSeen.load(), S.gravEvUnmodelled.load(), S.gravEvApplied.load(), S.gravEvOrderRisk.load(), S.gravEvNoBox.load());
        if (S.tpEvFired.load() || S.tpEvMiss.load()) std::printf("shadowstep: trigger-shadow Teleport events applied %lld, not in the shadow's world %lld\n", S.tpEvFired.load(), S.tpEvMiss.load());
        if (g_shadowSDiff) {
            const ShadowSDiff& Q = g_shSd;
            std::printf("shadowsdiff: %lld eligible ticks; the old tick changed dual %lld rev %lld frame %lld rotChan %lld mode %lld flip %lld mini %lld, died %lld; the decline decision: from c %lld, from s %lld, differs %lld (c declines and s does not %lld, s declines and c does not %lld)\n",
                        Q.n.load(), Q.dual.load(), Q.rev.load(), Q.frame.load(), Q.rotChan.load(), Q.mode.load(), Q.flip.load(), Q.mini.load(), Q.mdead.load(), Q.eligC.load(), Q.eligS.load(), Q.eligDiff.load(), Q.eligCnotS.load(), Q.eligSnotC.load());
            if (const char* lp = std::getenv("GDSOLVER_SHSDIFF_LOG"))
                if (std::FILE* lf = std::fopen(lp, "a")) { std::fprintf(lf, "n %lld dual %lld rev %lld frame %lld rotchan %lld mode %lld flip %lld mini %lld mdead %lld eligC %lld eligS %lld diff %lld cnots %lld snotc %lld\n", Q.n.load(), Q.dual.load(), Q.rev.load(), Q.frame.load(), Q.rotChan.load(), Q.mode.load(), Q.flip.load(), Q.mini.load(), Q.mdead.load(), Q.eligC.load(), Q.eligS.load(), Q.eligDiff.load(), Q.eligCnotS.load(), Q.eligSnotC.load()); std::fclose(lf); }
        }
        if (g_shadowSDiff) {
            const ShadowSDiff2& R2 = g_shSd2;
            std::printf("shadowsdiff2: gravity-box touch base: old tick kept no words in %lld ticks, c.trig differs from s.trig in %lld; seed check ran %lld ticks, would decline %lld (old tick died %lld, position %lld, mode %lld, flip %lld, vy %lld; skipped by --shadowfromseed %lld); rot holds %lld, applied %lld (not applied: dual %lld frame %lld slots %lld side %lld up %lld left %lld)\n",
                        R2.trigBaseInvalid.load(), R2.trigBaseDiff.load(), R2.seedChecks.load(), R2.seedDecl.load(), R2.seedDead.load(), R2.seedPos.load(), R2.seedMode.load(), R2.seedFlip.load(), R2.seedVy.load(), R2.seedSkipped.load(), R2.rotHolds.load(), R2.rotOk.load(), R2.rotFailDual.load(), R2.rotFailFrame.load(), R2.rotFailSlots.load(), R2.rotFailSide.load(), R2.rotFailUp.load(), R2.rotFailLeft.load());
            if (const char* lp = std::getenv("GDSOLVER_SHSDIFF2_LOG"))
                if (std::FILE* lf = std::fopen(lp, "a")) { std::fprintf(lf, "tbinv %lld tbdiff %lld sc %lld sdecl %lld sdead %lld spos %lld smode %lld sflip %lld svy %lld sskip %lld rh %lld rok %lld rdual %lld rframe %lld rslots %lld rside %lld rup %lld rleft %lld\n", R2.trigBaseInvalid.load(), R2.trigBaseDiff.load(), R2.seedChecks.load(), R2.seedDecl.load(), R2.seedDead.load(), R2.seedPos.load(), R2.seedMode.load(), R2.seedFlip.load(), R2.seedVy.load(), R2.seedSkipped.load(), R2.rotHolds.load(), R2.rotOk.load(), R2.rotFailDual.load(), R2.rotFailFrame.load(), R2.rotFailSlots.load(), R2.rotFailSide.load(), R2.rotFailUp.load(), R2.rotFailLeft.load()); std::fclose(lf); }
        }
        {
            std::printf("shadowseedslope: ramp words taken from the scratch pass in %lld seeds, %lld seeds the ramp did not take\n", g_shCol.seedSlope.load(), g_shCol.seedSlopeMiss.load());
            if (const char* lp = std::getenv("GDSOLVER_SHSEEDSLOPE_LOG"))
                if (std::FILE* lf = std::fopen(lp, "a")) { std::fprintf(lf, "seedslope %lld miss %lld\n", g_shCol.seedSlope.load(), g_shCol.seedSlopeMiss.load()); std::fclose(lf); }
        }
        {
            std::printf("shadowcolown: snapObj taken from the pass in %lld ticks, %lld not in the old tick's window (left)\n", g_shCol.ownSnap.load(), g_shCol.ownMiss.load());
            if (const char* lp = std::getenv("GDSOLVER_SHCOWN_LOG"))   // one line per call, for a tool that discards the stdout
                if (std::FILE* lf = std::fopen(lp, "a")) { std::fprintf(lf, "ownsnap %lld ownmiss %lld\n", g_shCol.ownSnap.load(), g_shCol.ownMiss.load()); std::fclose(lf); }
        }
        if (g_shadowColDerive) {
            const ShadowColStats& Q = g_shCol;
            std::printf("shadowcolderive: %lld ticks compared; snapObj uid differs %lld, snapDist differs %lld (uid equal); on-slope ticks %lld: P.OnSlope vs State::onSlope differs %lld, P.WasOnSlope vs State::onSlope differs %lld; slopeT from the clock on %lld ride ticks: differs %lld (derived - old: -4..+3 %lld %lld %lld %lld %lld %lld %lld %lld, beyond %lld); groundUid (set ones) differs %lld\n",
                        Q.n.load(), Q.snapUidDiff.load(), Q.snapDistDiff.load(), Q.onSlopeTicks.load(), Q.onSlopeVsOn.load(), Q.onSlopeVsWas.load(), Q.slopeTRide.load(), Q.slopeTDiff.load(),
                        Q.slopeTDelta[0].load(), Q.slopeTDelta[1].load(), Q.slopeTDelta[2].load(), Q.slopeTDelta[3].load(), Q.slopeTDelta[4].load(), Q.slopeTDelta[5].load(), Q.slopeTDelta[6].load(), Q.slopeTDelta[7].load(), Q.slopeTDelta[8].load(), Q.groundDiff.load());
            if (const char* lp = std::getenv("GDSOLVER_SHCOL_LOG"))   // a tool that discards the stdout (quick_regress) still gets the counts: one line per call is appended
                if (std::FILE* lf = std::fopen(lp, "a")) {
                    std::fprintf(lf, "n %lld snapuid %lld snapdist %lld slopeticks %lld vsOn %lld vsWas %lld rideticks %lld slopeT %lld d %lld %lld %lld %lld %lld %lld %lld %lld %lld ground %lld\n", Q.n.load(), Q.snapUidDiff.load(), Q.snapDistDiff.load(), Q.onSlopeTicks.load(), Q.onSlopeVsOn.load(), Q.onSlopeVsWas.load(), Q.slopeTRide.load(), Q.slopeTDiff.load(),
                                 Q.slopeTDelta[0].load(), Q.slopeTDelta[1].load(), Q.slopeTDelta[2].load(), Q.slopeTDelta[3].load(), Q.slopeTDelta[4].load(), Q.slopeTDelta[5].load(), Q.slopeTDelta[6].load(), Q.slopeTDelta[7].load(), Q.slopeTDelta[8].load(), Q.groundDiff.load());
                    std::fclose(lf);
                }
        }
        std::printf("shadowstep overflow by list: gate %lld used %lld arr %lld now %lld ever %lld act %lld (blocks packed with a list that ran out of room; caps %d %d %d %d %d %d)\n", gdcol::g_slOverBy[0].load(), gdcol::g_slOverBy[1].load(), gdcol::g_slOverBy[2].load(), gdcol::g_slOverBy[3].load(), gdcol::g_slOverBy[4].load(), gdcol::g_slOverBy[5].load(), gdcol::kSlGate, gdcol::kSlUsed, gdcol::kSlArr, gdcol::kSlNow, gdcol::kSlEverMax, gdcol::g_slActCap);
        std::printf("shadowstep declined: eligible %lld stepped %lld teleport %lld mini %lld inexact %lld warp %lld other %lld (dual %lld frame %lld reverse %lld platformer %lld) slope %lld seedcheck %lld\n",
                    S.eligible.load(), S.stepped.load(), S.declinedTeleport.load(), S.declinedMini.load(), S.declinedInexact.load(), S.declinedWarp.load(), S.declinedOther.load(),
                    S.declinedDual.load(), S.declinedFrame.load(), S.declinedRev.load(), S.declinedPlat.load(), S.declinedSlope.load(), S.declinedSeedCheck.load());
        // a tool that discards the stdout (quick_regress) still gets them: GDSOLVER_SHDECL_LOG names a file one line per call is appended to
        if (const char* dp = std::getenv("GDSOLVER_SHDECL_LOG"))
            if (std::FILE* df = std::fopen(dp, "a")) {
                std::fprintf(df, "eligible %lld stepped %lld teleport %lld mini %lld inexact %lld warp %lld other %lld dual %lld frame %lld reverse %lld platformer %lld slope %lld seedcheck %lld\n",
                             S.eligible.load(), S.stepped.load(), S.declinedTeleport.load(), S.declinedMini.load(), S.declinedInexact.load(), S.declinedWarp.load(), S.declinedOther.load(),
                             S.declinedDual.load(), S.declinedFrame.load(), S.declinedRev.load(), S.declinedPlat.load(), S.declinedSlope.load(), S.declinedSeedCheck.load());
                std::fclose(df);
            }
        std::printf("shadowfollow: %lld ticks compared with the old tick's follow pass (%lld with a running command, %lld objects placed by one; %lld plan entries, %lld sent far), plan differs %lld, State fields differ %lld\n", g_shFollow.checked.load(), g_shFollow.active.load(), g_shFollow.placedObjs.load(), g_shFollow.entries.load(), g_shFollow.farObjs.load(), g_shFollow.planDiff.load(), g_shFollow.stateDiff.load());
        // (never on: the switch --shadowkeepprune is removed)
        // --shadowkeepprune (off): a death the old tick gives as the search's OUT-OF-PLAY bound (kDeathPrune "out-of-play", "wave/out-of-play", "fly/out-of-play"; step.hpp) stays a death under --shadowstep. The other prunes (escapee-prune, spider/no-target)
        // stay overridden: escapee-prune kills what GD flies on (a custom level t=5359, window w_126977670_5350: a false death with the whole kDeathPrune kind kept). shadowAdvance takes the shadow's verdict
        // and drops the old tick's, which is right for a kill the old tick over-reads (GD flies on) and wrong for a bound the search sets for itself: the shadow lets the sky-escape lane (a ball climbing past
        // the level's top, y > 2893 on lv22) and the lanes that fall out of the level live, and they fill the alive cap and evict the real lane (lv22 iter 3: 724,647 model-only deaths overridden in one
        // call, capHits 496, PARTIAL at x=4536 while the old model's search goes on)
        std::printf("shadowstep: model-only deaths by kind: physics %lld rule %lld prune %lld%s\n", S.modelOnlyByKind[0].load(), S.modelOnlyByKind[1].load(), S.modelOnlyByKind[2].load(),
                    "");
        if (g_shadowSupply) {
            std::printf("shadowsupply: touch boxes run again %lld ticks, differ from the model's %lld; follow objects placed %lld sent away %lld not movers %lld slopes %lld moved %lld; tpBack rerun %lld differ %lld; gravTouch rerun %lld differ %lld gate refused %lld ctrlT rerun %lld differ %lld touch teleports %lld fixup deltas %lld kills %lld; model changed tpBack %lld gravTouch %lld tpTouch %lld pcRel %lld pcTouch %lld ctrlT %lld\n", S.supTouchRan.load(), S.supTouchDiff.load(), S.supFolPlaced.load(), S.supFolFar.load(), S.supFolNonMover.load(), S.supFolSlope.load(), S.supFolMoved.load(), S.supTpBackRan.load(), S.supTpBackDiff.load(), S.supGravRan.load(), S.supGravDiff.load(), S.supGateRefused.load(), S.supCtrlRan.load(), S.supCtrlDiff.load(), S.supTpTouch.load(), S.supFixDelta.load(), S.supFixKill.load(), S.seenTpBack.load(), S.seenGravTouch.load(), S.seenTpTouch.load(), S.seenPcRel.load(), S.seenPcTouch.load(), S.seenCtrlT.load());
            // a tool that discards the stdout (quick_regress) still gets the counters: GDSOLVER_SHSUPPLY_LOG names a file they are appended to, one line per call
            if (const char* lp = std::getenv("GDSOLVER_SHSUPPLY_LOG"))
                if (std::FILE* lf = std::fopen(lp, "a")) {
                    std::fprintf(lf, "ran %lld diff %lld folplaced %lld folfar %lld folnonmover %lld folslope %lld folmoved %lld; tpBack rerun %lld differ %lld; gravTouch rerun %lld differ %lld gate refused %lld ctrlT rerun %lld differ %lld touch teleports %lld fixup deltas %lld kills %lld; model changed tpBack %lld gravTouch %lld tpTouch %lld pcRel %lld pcTouch %lld ctrlT %lld\n", S.supTouchRan.load(), S.supTouchDiff.load(), S.supFolPlaced.load(), S.supFolFar.load(), S.supFolNonMover.load(), S.supFolSlope.load(), S.supFolMoved.load(), S.supTpBackRan.load(), S.supTpBackDiff.load(), S.supGravRan.load(), S.supGravDiff.load(), S.supGateRefused.load(), S.supCtrlRan.load(), S.supCtrlDiff.load(), S.supTpTouch.load(), S.supFixDelta.load(), S.supFixKill.load(), S.seenTpBack.load(), S.seenGravTouch.load(), S.seenTpTouch.load(), S.seenPcRel.load(), S.seenPcTouch.load(), S.seenCtrlT.load());
                    std::fclose(lf);
                }
        }
        if (g_shadowDual) std::printf("shadowstep dual: stepped %lld (ticks with player 2), player 2 seeded from the model %lld\n", S.dualStepped.load(), S.dualSeededModel.load());
        if (g_shPlaceChecks.load()) std::printf("shplace check: %lld placements compared with the sweep, %lld movers it would have switched off left on\n", g_shPlaceChecks.load(), g_shPlaceCheckDiff.load());
        std::printf("shadowstep init: %lld calls (%.1f ms in all), %lld base builds (%.1f ms), %lld thread worlds built (%.1f ms)\n", S.initCalls.load(), (double)S.initNs.load() / 1e6, S.initRebuilds.load(), (double)S.buildNs.load() / 1e6, S.threadBuilds.load(), (double)S.threadBuildNs.load() / 1e6);
        if (const gdcol::SlStore* st = gdcol::g_slStore)
            std::printf("shadowstep store: gc %lld, peak %llu slots (%.1f MiB), held now %.1f MiB, lost %lld, refused %lld\n", st->gcs(),
                        (unsigned long long)st->peakSlots(), (double)st->peakSlots() * sizeof(gdcol::ShadowLite) / 1048576.0, (double)st->heldBytes() / 1048576.0,
                        st->lost(), st->overflow());
        if (const long long nb = gdcol::SlDiffRun::s_blocks().load())
            std::printf("shadowstep saves: %lld blocks kept by --ladderprefix, %.1f words (of %zu) a block differ from their layer's first\n", nb,
                        (double)gdcol::SlDiffRun::s_words().load() / (double)nb, gdcol::SlDiffRun::kW);
        if (gdcol::ssProf().on) {
            const double n = (double)std::max<long long>(1, S.stepped.load());
            const gdcol::SsTl Q = gdcol::ssSum();
            static const char* names[20] = {"h.clock", "h.nbr", "h.ctx", "h.edges", "h.wb+img", "t.ctx", "t.update", "t.img", "t.nbr", "t.ctx2", "t.pass", "t.sets", "t.wb+img",
                                            "t.after", "-", "u.words", "u.rest", "p.words", "p.act+rest", "p.lists"};
            std::printf("SSPROF us/stepped tick:");
            for (int k = 0; k < 20; ++k) if (k != 14) std::printf(" %s %.3f", names[k], Q.ns[k] / n / 1000.0);
            std::printf(" | nbLocal avg %.1f (%lld calls) nbPass avg %.1f (%lld calls)\n", Q.cnt[0] ? (double)Q.cnt[1] / Q.cnt[0] : 0.0, Q.cnt[0], Q.cnt[2] ? (double)Q.cnt[3] / Q.cnt[2] : 0.0, Q.cnt[2]);
            std::printf("SSPROF neighbourhoods: %lld taken from the last call's copies (%lld of them read `activated` again), %lld copied afresh (%lld: reuse not allowed with unannounced movers, %lld: another cell or nothing kept)\n",
                        Q.cnt[10], Q.cnt[14], Q.cnt[11], Q.cnt[12], Q.cnt[13]);
            std::printf("SSPROF press by mode: spider %lld (%.1f us each), ball %lld (%.1f us each), other %lld (%.1f us each)\n", Q.cnt[7], Q.cnt[7] ? (double)Q.ns[22] / Q.cnt[7] / 1000.0 : 0.0,
                        Q.cnt[8], Q.cnt[8] ? (double)Q.ns[23] / Q.cnt[8] / 1000.0 : 0.0, Q.cnt[9], Q.cnt[9] ? (double)Q.ns[20] / Q.cnt[9] / 1000.0 : 0.0);
            std::printf("SSPROF edges: press %lld (%.1f us each, %lld with a touched ring), release %lld (%.1f us each), no edge %.3f us a tick\n", Q.cnt[4], Q.cnt[4] ? (double)(Q.ns[20] + Q.ns[22] + Q.ns[23]) / Q.cnt[4] / 1000.0 : 0.0, Q.cnt[6],
                        Q.cnt[5], Q.cnt[5] ? (double)Q.ns[21] / Q.cnt[5] / 1000.0 : 0.0, Q.ns[3] / n / 1000.0);
        }
        if (g_shProf.on) {
            const double n = (double)std::max<long long>(1, S.stepped.load());
            const ShProfTl Z = shProfSum();
            std::printf("shadowstep prof (us a stepped tick): model %.2f movers %.2f unpack %.2f tail %.2f head %.2f pack %.2f put %.2f supply %.2f dual %.2f after-the-clock %.2f\n", Z.ns[0] / n / 1000.0,
                        Z.ns[1] / n / 1000.0, Z.ns[2] / n / 1000.0, Z.ns[3] / n / 1000.0, Z.ns[4] / n / 1000.0, Z.ns[5] / n / 1000.0,
                        Z.ns[6] / n / 1000.0, Z.ns[7] / n / 1000.0, Z.ns[8] / n / 1000.0, Z.ns[9] / n / 1000.0);
        }
        std::fflush(stdout);
    }
}

// P9 family "input" (gd-04, 11-07; --fieldcheck): the old tick's input / button fields against what the plan's input and the shadow body's GD bytes give (buttons.hpp H = +0x98d m_jumpBuffered, S = +0x98e m_stateRingJump).
// action is not registered: its convention (measured: it is not the input edge against the previous held level) is not read yet.
//   held      = the plan's input level            jumpBuf = the plan's input level (the old tick sets it from the input at its head)
//   holdDead  = the input is down and the body's H is 0 (a press the body does not hold)
//   pressSpent = H set and S clear (a press the body has spent)
// ringHold ("one ring per HOLD") has no GD byte: it is the old model's own latch, not derived here (not registered). Player 1 of a state that is not dual only.
inline const bool g_p9InputFamily = [] {
    fcheck::registerFamily("input", {"held", "jumpBuf", "pressSpent", "holdDead"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow) return;
        const gdcol::Mem& P = static_cast<const gdcol::SelfShadow*>(cx.shadow)->img;
        const uint8_t H = P.u8(0x98d), S = P.u8(0x98e);
        d.held = cx.input ? 1 : 0;
        d.jumpBuf = cx.input ? 1 : 0;
        d.holdDead = (cx.input && !H) ? 1 : 0;
        d.pressSpent = (H && !S) ? 1 : 0;
        // A SPIDER's press is spent by the teleport, which does not leave H set: spiderTestJumpInternal (0x4f25f0, spider.hpp: `P.u8(0x98d, 0)` after the flip and the +-1 velocity) clears H and S stays,
        // so the byte rule above reads 0 where the old tick's --spiderspend (step.hpp, the tap's teleport branch: pressSpent = 1, kept while the button is held) says 1. The press stays spent for as long as
        // the button is down and H is 0. [R] spider.hpp:474 (the port of 0x4f25f0); [M] run-both on the --sametick tap probe (cli.hpp): synthetic taps on grounded spiders of lv21 / lv22 (27 distinct
        // ticks): the old tick's pressSpent / groundJump agree on 26; the 27th (lv22 t=827, a state the earlier synthetic tap left falling) is a tick the old tick does not tap on at all (y 104.28 against the shadow's teleport to 123).
        if (cx.s.mode == 6 && cx.c.mode == 6 && cx.input && !H) d.pressSpent = 1;
        // env P9DUMP=file: one line a tick (a --replay walk is one thread, in order) for the join with the game's dump (gd-04): t, input, the old fields, the shadow's bytes
        static std::FILE* const df = [] { const char* e = std::getenv("P9DUMP"); return e && e[0] ? std::fopen(e, "w") : nullptr; }();
        if (df) std::fprintf(df, "%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", cx.t, cx.input, (int)cx.c.held, (int)cx.c.jumpBuf, (int)cx.c.holdDead, (int)cx.c.pressSpent, (int)cx.c.ringHold, (int)H, (int)S, (int)P.u8(0x991), (int)P.u8(0x992));
    });
    return true;
}();
// P9 family "ring_pad_orb" (gd-04, 11-07; --fieldcheck): the touching-rings list and the boost latch.
//   touchRing = the shadow's RingTouches::arr (m_touchingRings: uids in first-contact order, -1 padded to four)    a1cLatch = the body's +0xa24 (m_maybeIsBoosted, po::Boost), bit 0
// usedOrb / usedOrbOld / usedOrbHist (the order the rings were fired in) and usedPad have no shadow counterpart: the shadow keeps a SET of fired rings (usedRings, the objects' activated marks), not their order;
// touchRingT is the old model's own mask of which of its four slots the geometry touched. Player 1 of a state that is not dual only. env P9DUMPR=file: a line a tick for the join with the game's ctrace (BOOST).
inline const bool g_p9RingFamily = [] {
    fcheck::registerFamily("ring_pad_orb", {"touchRing", "a1cLatch", "usedOrb", "usedOrbOld", "usedOrbHist"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow) return;
        const gdcol::SelfShadow* sh = static_cast<const gdcol::SelfShadow*>(cx.shadow);
        {   // the State's fired-ring slots from the record, newest first (usedOrb, usedOrbOld, usedOrbHist[3])
            static thread_local Obj dummy;
            const std::vector<int>& F = sh->fireOrder;
            dummy.uid = F.empty() ? -1 : F[0];
            d.usedOrb = F.empty() ? nullptr : (cx.c.usedOrb && cx.c.usedOrb->uid == F[0] ? cx.c.usedOrb : shadowObjByUid(F[0]));   // the level's own Obj, not this thread's scratch dummy (a State pointer into a thread_local is a value of whichever thread stepped it: 8-thread searches differed, gd-05 11-12)
            d.usedOrbOld = F.size() > 1 ? F[1] : -1;
            for (int k = 0; k < 3; ++k) d.usedOrbHist[k] = F.size() > (size_t)(2 + k) ? F[(size_t)(2 + k)] : -1;
        }
        for (int i = 0; i < 4; ++i) d.touchRing[i] = i < (int)sh->rt.arr.size() ? sh->rt.arr[(size_t)i] : -1;
        d.a1cLatch = (uint8_t)((sh->img.u8(gdcol::po::Boost) ? 1 : 0) | (cx.c.a1cLatch & 2));
        {   // env P9DUMPU=file (gd-04, 11-10): the old fired-ring / used-pad slots against the shadow block's activated list (SlStore), a line a tick: t,usedOrb,usedOrbOld,hist0..2,usedPad0..3,nAct,act0..7
            static std::FILE* const uf = [] { const char* e = std::getenv("P9DUMPU"); return e && e[0] ? std::fopen(e, "w") : nullptr; }();
            if (uf) {
                const State& cs = cx.c;
                const gdcol::ShadowLite* blk = gdcol::slOf(cs.slRef);
                std::fprintf(uf, "%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", cx.t, cs.usedOrb ? cs.usedOrb->uid : -1, (int)cs.usedOrbOld, (int)cs.usedOrbHist[0], (int)cs.usedOrbHist[1], (int)cs.usedOrbHist[2],
                             cs.usedPad[0] ? cs.usedPad[0]->uid : -1, cs.usedPad[1] ? cs.usedPad[1]->uid : -1, cs.usedPad[2] ? cs.usedPad[2]->uid : -1, cs.usedPad[3] ? cs.usedPad[3]->uid : -1, blk ? (int)blk->nAct : -1);
                for (int i = 0; i < 8; ++i) std::fprintf(uf, ",%d", (blk && i < (int)blk->nAct) ? (int)blk->act[i] : -1);
                std::fprintf(uf, ",%d", blk ? (int)blk->nGate : -1);
                for (int i = 0; i < 12; ++i) std::fprintf(uf, ",%d", (blk && i < (int)blk->nGate) ? (int)blk->gate[i] : -1);
                { const gdcol::SelfShadow* shp = static_cast<const gdcol::SelfShadow*>(cx.shadow); for (int i = 0; i < 5; ++i) std::fprintf(uf, ",%d", (shp && i < (int)shp->fireOrder.size()) ? shp->fireOrder[(size_t)i] : -1); }
                std::fprintf(uf, "\n");
            }
        }        static std::FILE* const df = [] { const char* e = std::getenv("P9DUMPR"); return e && e[0] ? std::fopen(e, "w") : nullptr; }();
        if (df) std::fprintf(df, "%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", cx.t, (int)cx.c.a1cLatch, (int)d.a1cLatch, cx.c.touchRing[0], cx.c.touchRing[1], cx.c.touchRing[2], cx.c.touchRing[3], d.touchRing[0], d.touchRing[1], d.touchRing[2], d.touchRing[3], (int)cx.c.mode);
    });
    return true;
}();
// P9 family "portal" (gd-03, 11-08; --fieldcheck): the timers and the memory the old tick's portal handling leaves, against what the finished state says.
//   flipT = 0 on the tick gravity flipped (gdUpOf of the finished state against the state before), else the clock up one, saturated at kFlipGraceTicks (the removed step.hpp, end of the old tick's stepOne)
//   modeT = the same for the mode           halfPrev = the contact half the portal pass used: a mini wave's contact half, else playerHalf(mode, mini) of the state before the tick
// The finished state's flip / mode are the SHADOW's (writeBody), so a difference here is a tick in which the old tick's own flip / mode decision differed from the shadow's. One body only.
// Not registered, because nothing outside the old tick reads them (tools/ field readers, restructure draft 7.21): halfPrev2 / flipT2 / modeT2, spdFired(2) / spdMore(2) / spdSpill (printed by cli.hpp's seed line only).
inline const bool g_p9PortalFamily = [] {
    fcheck::registerFamily("portal", {"flipT", "modeT", "halfPrev"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || cx.c.dual || cx.s.dual) return;
        portalDeriveClocks(cx.s, cx.c, d.flipT, d.modeT);
        d.halfPrev = (float)((cx.s.mode == 4 && cx.s.mini) ? kWaveContactHalfMini : playerHalf(cx.s.mode, cx.s.mini != 0));
    });
    return true;
}();
// P9 family "portal_state" (gd-03, 11-08; --fieldcheck): the portal-side memory the search sees (search_key.hpp, shadow_step.hpp seeds: tools/field_readers), against the shadow body.
//   armT  = GD's ceiling-arm counter +0xb84 (id 1859; the seed writes it as kArmTicks - armT while armT < kArmTicks): armT = kArmTicks - counter while it is up, else 255
//   fgArm = the "Special" 2866's counter +0xb88 (collide.hpp didHitHead): kArmTicks while it is up, else 0
//   portalLatch = the gravity portals (g_shGpUid: bit -> uid) in the body's once-per-attempt `ever` set
//   ball728 = the ball's rotation word +0x728 (collide.hpp resets it to 0 on a flip / portal; runBallRotation sets it): 1 = zero, 2 = not, in State::ball728's low bits; other modes keep the old tick's value.  One body only.
inline const bool g_p9PortalStateFamily = [] {
    fcheck::registerFamily("portal_state", {"armT", "fgArm", "portalLatch", "ball728"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || cx.c.dual || cx.s.dual) return;
        const gdcol::SelfShadow& sh = *static_cast<const gdcol::SelfShadow*>(cx.shadow);
        const gdcol::Mem& P = sh.img;
        portalStateDerive(cx.s, *static_cast<const ShadowThread*>(cx.thread), sh, d);
        if (cx.c.mode == 2) d.ball728 = (uint8_t)((cx.c.ball728 & ~3u) | (P.get<uint32_t>(0x728) == 0u ? 1u : 2u));   // 1 = the rotation word was reset since, 2 = the ball rolled (runBallRotation / updateTimeMod)
    });
    return true;
}();
// P9 family "band_timer" (gd-03, 11-08; --fieldcheck): the ground band's tween clock State::bandAnim (bands.hpp: bit 7 = in, bits 0-6 = ticks elapsed, 127 = settled) made from events. The old tick steps it every tick when the level has
// a static camera (g_staticCamCol) and restarts it on a mode portal (in: bandAnimStart(true), out: bandAnimStart(false)); the shadow's layer says the same event: updateDualGround ran this tick (animEvent) and which way (animIn:
// the band is animated in, else retracted). One body only. (tight is written by the search itself, search_dp.hpp, not by the tick: not a family field; tpSkip has no reader outside the old tick; touchRingT / portSeen / ogLinger: see 7.23.)
inline const bool g_p9BandFamily = [] {
    fcheck::registerFamily("band_timer", {"bandAnim"}, [](const fcheck::Ctx& cx, State& d) {
        if (!cx.shadow || !cx.layer || cx.c.dual || cx.s.dual) return;
        d.bandAnim = bandTimerDerive(cx.s, *static_cast<const gdcol::ShadowLayer*>(cx.layer));
    });
    return true;
}();
}  // namespace dp
