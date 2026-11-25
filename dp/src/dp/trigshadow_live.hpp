#pragma once
// --trigshadowlive DIR[,EXTDIR] (default off, observation only): the trigger shadow (trigshadow.hpp) run beside the search, one timeline per group.
// T0 of the trigger merge: nothing here is read by the search yet. It answers what the design costs on the real search -- how many timelines a layer
// holds, how often a group has to be given a copy of a timeline (a split), what a step and a copy cost, and how big a snapshot is.
//
// A timeline is a whole TrigShadow stepped with the group's K.x (the x the group's moving geometry is placed for, search_dp.hpp). A layer's group takes the timeline of
// the previous layer's group with the same key (dx, touch mask, frame, rev, fire signature, xc: model.hpp GroupKey) when no other group has taken it; otherwise a copy
// of it (a split) -- and a group whose key the previous layer did not have takes the timeline its first member was stepped in (GroupIn::world: the search knows it from
// the arena node of the state), or, where the search cannot name one (the first layer of a search), a copy of the previous group with its touch history nearest in x.
// The key a timeline is stored under also says how many x-crossing triggers its group has crossed (GroupIn below).
#include "dp/cam_model.hpp"
#include "dp/trigevent.hpp"
#include "dp/trigshadow_load.hpp"

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdio>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "dp/cam_y_obs.hpp"   // --camy S2.1 (CamYGroup)
#include "dp/game_event.hpp"   // State::evgate's chain (feedEvents)
#include "dp/trigevent_togstate.hpp"   // the Taps (g_togTable.taps): a press is a token of that chain

namespace dp {

// The world a second body's pass asked (cli.hpp twoBodyFromBirth, GDSOLVER_B2CHECK=1, observation only): the group keys given to the shadow while the pass ran, one value per
// (tick, key) summed and also mixed in order, with how many there were.
inline bool g_b2WorldOn = false;
inline std::atomic<uint64_t> g_b2WorldSum{0};   // the sum of the mixed (tick, key): the same key given twice does not cancel, the order of the groups of a layer does not matter
inline std::atomic<uint64_t> g_b2WorldSeq{0};   // the same values mixed in the order given (the search's thread that calls group(); a check, not a proof of order)
inline std::atomic<long long> g_b2WorldGroups{0};

struct TrigLive {
    using Clock = std::chrono::steady_clock;
    struct Line { std::shared_ptr<gdtrig::TrigShadow> sh; double x = 0; bool taken = false; bool shared = false; uint64_t told = 0; uint64_t evtold = 0; dp::gdcam::CamState cam; uint16_t fb[kTouchBits] = {}; };   // cam: --trigshadowopts cammodel (the camera model's state along this timeline); told: the touch bits (State::trig) whose Events this timeline has been given (--trigshadowevents)
    bool on = false;
    // --trigshadowevents (default off, observation only; docs/ notes/p4_touch_event_wiring_design_1105_gd04.md): a timeline is told the touch calls of its group. A call is a bit set in the group's latch (State::trig, the
    // group key holds it) and not in the parent timeline's `told`; its Event is {kEvTouch, step = fireB[bit] + evShift, uid of the box} (trigevent_touch.hpp touchEventsFromLatch). The base shadow is loaded with
    // touchFed_ and recordComplete_ set, so the Touch-Triggered triggers (and their Spawn / Stop chains) are modelled at all.
    // --trigshadowreplay (default off, observation only): a timeline that has no parent (the first layer, an anchor in the middle of the level) is not a copy of the level's start stepped once at the anchor's tick
    // but is stepped through the ticks before it along the run's x track (--xtrack: GD's x of this run, the anchor's own input), so the movers the crossings started earlier are where they are at the anchor. With
    // --trigshadowevents the owed touch Events (the anchor's latch and fireB) are given to that replay.
    bool events = false, replay = false;
    int evShift = 0;   // the step a call is filed under in the REPLAY of a parentless timeline, relative to the model's K.t of the call (fireB): 0 = the step fireB itself (trigshadow.hpp setTouchEvents: the game's tick + 1)
    bool built = false;
    std::vector<int> boxUid;   // bit b -> the touch trigger's uid (g_touch), built at the first group
    long long evGiven = 0, evReplays = 0, evReplaySteps = 0, evNoTrack = 0, evOwedFresh = 0;
    double evReplayMs = 0;
    std::string dir, ext;
    std::string optList;   // --trigshadowopts (ensure())
    std::shared_ptr<gdtrig::TrigShadow> base;
    std::unordered_map<uint64_t, Line> prev, cur;
    long long layerJumps = 0;   // layers that did not follow the one before (a new search): the parents were dropped
    long long lastT = -1, layers = 0, groupsSeen = 0, steps = 0, moves = 0, copies = 0, fresh = 0, maxLines = 0, sumLines = 0, snapSamples = 0;
    double stepMs = 0, copyMs = 0, snapSumKB = 0, snapMaxKB = 0;
    size_t objCount = 0, covered = 0, trigCount = 0, trigRun = 0;
    gdtrig::CowPages<gdtrig::ObjState> init;   // the objects at the load (a copy of the base's: it shares the base's pages for good)

    // the directories are read here; the shadow itself is built at the first group (ensure()), after every flag is parsed (--trigshadowevents changes how it is loaded)
    bool load(const std::string& arg) {
        const size_t c = arg.find(',');
        dir = arg.substr(0, c);
        ext = c == std::string::npos ? std::string() : arg.substr(c + 1);
        on = true;
        // per call: a --replay walk sets singlePath (one group a tick on one path), and nothing cleared it, so every search after the first replay of the process ran with it: its second group of a layer then
        // "took the parent over twice", the supply was switched off for good (supplyGroups.clear()) and the shadow ran as an observer only. A search call starts with the supply it was built with.
        singlePath = false; layerMoved = false;
        replayMemo = ReplayMemo{};   // (the replay kept for this call's later parentless timelines: another call has another x track and anchor)
        if (!supplySaved.empty()) { supplyGroups = supplySaved; supplySaved.clear(); }
        if (built && dataSig() != builtSig) { built = false; levelChanged = true; }   // a new level's dump: supplyOn() asks !built, so the first group() of this call comes and ensure() builds the shadow again
        // the anchor check of group() (a supplied group a touch trigger behind the anchor acts on is dropped unless the anchor's history decides it) belongs to the CALL: anchorSeen was reset only with the level and the
        // groups the first call dropped stayed dropped, so a later call (a farther anchor: lv22 5309, 84 of 96 calls after the first) kept supplying groups whose touch had fired before its anchor, with no Event for it.
        if (built && !supplyGroups0.empty()) { supplyGroups = supplyGroups0; supplySaved.clear(); anchorSeen = false; }
        return true;
    }
    // dp runs inside the game and a session solves level after level in ONE process, with the same data directory whose files are rewritten for each level: the shadow built for the first level (objects 0 for a level
    // whose dump was not written yet) kept running for every later one. The shadow is built again when the dump files it reads are not the ones it was built from (size and last-write time).
    std::string builtSig;
    bool levelChanged = false;   // load() saw other dump files than the shadow was built from (ensure() starts from nothing)
    std::set<int> supplySaved;   // the supplied groups of a call whose supply was switched off (singlePath violation): the next call has them again
    std::set<int> namedGroups;   // --trigsupply-group G of this process (the groups the supply-all and the load tests add and drop are not)
    std::string dataSig() const {   // (sys/stat, not <filesystem>: the mod builds dp with `std` renamed, which <filesystem> does not survive)
        std::string s;
        for (const std::string& d : {dir, ext}) {
            if (d.empty()) continue;
            for (const char* f : {"objrects.txt", "objgroups.txt", "triggers.txt", "trigext.txt", "areas.txt", "rotgameplay.txt", "areaeffect.txt", "areaedit.txt", "areatargets.txt", "multi.txt", "stopdump.txt", "levelflags.txt"}) {
                const std::string p = d + "/" + f;
#ifdef _WIN32
                struct _stat64 st;
                if (_stat64(p.c_str(), &st) != 0) { s += "-;"; continue; }
#else
                struct stat st;
                if (stat(p.c_str(), &st) != 0) { s += "-;"; continue; }
#endif
                s += std::to_string((long long)st.st_size) + ":" + std::to_string((long long)st.st_mtime) + ";";
            }
        }
        return s;
    }
    // everything the previous level left in the object (the flags and the named groups stay)
    void resetLevel() {
        base.reset(); init.clear(); boxUid.clear(); camTrace.clear();
        stepMemo.clear(); shareSteps = false; replayMemo = ReplayMemo{}; osx0Have_ = false;
        prev.clear(); cur.clear(); pend.clear(); supCur.reset(); supPre.clear(); supObj.clear(); supSlot.clear(); supFlat.clear(); supAny = false;
        supplyGroups = namedGroups; supplySaved.clear(); supplyUids.clear(); supplyGroups0.clear(); partialGroups.clear();
        anchorSeen = false; lastT = -1; layerJumps = 0; layerMoved = false; layerTake = false; anchorRngReported = false;
        objCount = covered = trigCount = trigRun = 0;
        crossBuilt = false; crossPm.clear(); crossRun.clear(); gin = GroupIn{}; annIn.clear();
    }
    // bit b of the touch latch (State::trig) is the b-th box of THIS call's g_touch -- loadTouchTriggers keeps a window of boxes from the anchor's x, so the bit -> trigger map is a property of the call, not of the
    // level. boxUid was made once, in ensure(), from the first call's window and kept while the dump files did not change: a later call (another anchor) read its latch bits against the first call's boxes, and
    // feed() gave the timeline the Events of the wrong triggers (lv22 5309: Spawn 17747 is bit 13 there, bit 27 in the 1986 call, so the platform it starts never moved in the walks: 73 of 83 walks of one cold read a stale map).
    void refreshBoxes() {
        if (!events || !built) return;
        bool same = boxUid.size() == std::min(g_touch.size(), (size_t)kTouchBits);
        for (size_t b = 0; same && b < boxUid.size(); ++b) if (boxUid[b] != g_touch[b].uid) same = false;
        if (same) return;
        boxUid.clear();
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) boxUid.push_back(g_touch[b].uid);
        gdEntry.clear();   // (the diagnostic reads the same bits)
        ++boxRemaps;
    }
    long long boxRemaps = 0;   // calls whose touch window differed from the map boxUid held
    // The Tap chains the build found (game_event.hpp g_tapFed / g_tapWinTicks / g_tapWinMask, and each chain Tap's chain / perPress / slot / cap / sat in gdtrig::g_togTable) are not kept by the globals: gdev::evChainReset() clears the
    // flags and cli.hpp builds g_togTable again at the start of every call, while the shadow is built once and kept for the calls after it (ensure() returns at `built`). A second attempt of a ladder, or a later call of the same
    // process, therefore ran with the Tap chains off. The build's verdict is kept here, by Tap uid, and put back after the table is built again (restoreTapFlags, cli.hpp).
    bool tapFedKeep = false; int tapWinTicksKeep = 0; uint32_t tapWinMaskKeep = 0;
    struct TapKeep { int uid; bool chain, perPress; int slot, cap, sat; bool narrow; };
    std::vector<TapKeep> tapKeep;
    std::vector<gdev::TapNearBox> tapNearKeep;
    double tapNearPxKeep = 0, tapNearPyKeep = 0;
    bool levelMayGoBackKeep = true;   // gdev::g_levelMayGoBack as the build decided it (evChainReset sets it back to true every call)
    std::vector<gdtrig::TapMove> tapMovesKeep;
    double tapPastKeep[8] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    // Only the NARROW Taps (a plain Move spawned at every press: tp.narrow) are put back for the calls after the build: their presses are counted by the state and the Move is applied to the movers in the pass, with no chain in the
    // timelines. The Taps whose chain the timeline runs (State::evgate) keep the behaviour they had: on in the build's own call, off in the calls after it.
    void restoreTapFlags() {
        if (built) gdev::g_levelMayGoBack = levelMayGoBackKeep;   // (every call: evChainReset sets it back to true; also on a level with no Tap)
        if (!built || !tapFedKeep) return;
        bool any = false;
        for (gdtrig::TogTap& tp : gdtrig::g_togTable.taps)
            for (const TapKeep& k : tapKeep) if (k.uid == tp.uid && k.narrow) { tp.chain = k.chain; tp.perPress = k.perPress; tp.slot = k.slot; tp.cap = k.cap; tp.sat = k.sat; tp.narrow = true; any = true; break; }
        if (!any) return;
        gdev::g_tapNear = tapNearKeep; gdev::g_tapNearPx = tapNearPxKeep; gdev::g_tapNearPy = tapNearPyKeep;
        gdev::g_tapFed = true; gdev::g_tapWinTicks = tapWinTicksKeep; gdev::g_tapWinMask = tapWinMaskKeep;
        gdtrig::g_tapMoves = tapMovesKeep;
        for (int i = 0; i < 8; ++i) gdev::g_tapPastX[i] = tapPastKeep[i];
    }
    void ensure() {
        if (built) return;
        tapFedKeep = false;
        if (levelChanged) { std::printf("trigshadowlive: the level data changed (%s): the shadow is built again\n", dir.c_str()); resetLevel(); levelChanged = false; }
        built = true;
        builtSig = dataSig();
        gdtrig::TrigLoadOpts lo;
        lo.ref = dir;
        lo.extDir = ext;
        lo.collision = true;   // (observation only: the shadow runs everything it can -- a collblocks.txt in the directory, the Areas with a separate ease-out)
        lo.areaEOut = true;
        { std::ifstream f(dir + "/areatargets.txt"); if (f) lo.areaTargetsFile = dir + "/areatargets.txt"; else if (!ext.empty()) { std::ifstream g(ext + "/areatargets.txt"); if (g) lo.areaTargetsFile = ext + "/areatargets.txt"; } }   // (the flight's own directory when the reference's has none)
        { const std::string sd = (ext.empty() ? dir : ext) + "/stopdump.txt"; std::ifstream f(sd); if (f) lo.stopDumpFile = sd; }
        // --trigshadowopts: the loader's options of tools/trig_replay.cpp that the layer B check has not had (observation only; coverage measured on a custom level with trig_replay --coveronly, the options one by one on a base
        // with --selftouch: --arearadial +61 objects, --scale with the level string +116, --pickup +36, --screenareas +2450 -- and the last one needs the camera, see Cam in trigshadow.hpp)
        bool multiOpt = false;
        {
            size_t p = 0;
            while (p <= optList.size()) {
                size_t q = optList.find(',', p); if (q == std::string::npos) q = optList.size();
                const std::string o = optList.substr(p, q - p); p = q + 1;
                if (o == "remap") lo.remapChain = true;
                else if (o == "editwide") lo.areaEditWide = true;
                else if (o == "angle") lo.areaAngle = true;
                else if (o == "radial") lo.areaRadial = true;
                else if (o == "areascale") lo.areaScale = true;
                else if (o == "items") lo.items = true;
                else if (o == "settings") lo.settingsInert = true;   // the Settings object 3662 moves nothing (trigshadow.hpp TrigShadow::ign)
                else if (o == "camlock") lo.camLock = true;   // the Moves locked to the camera (needs the camera: cam / camtrace.csv)
                else if (o == "screen") lo.screenAreas = true;
                else if (o.compare(0, 9, "cammodel=") == 0) { camModelOn = camModel.load(o.substr(9)); if (!camModelOn) std::printf("trigshadowlive: cammodel: cannot read %s\n", o.substr(9).c_str()); }   // the camera X MODEL (cam_model.hpp), from the level string, in place of a recording. It only READS the model: the screen-type Areas run on it with `screen` as well (cammodel=LEVEL,screen: what cammodel=LEVEL alone did before 11-15)
                else if (o == "cam") { lo.screenAreas = true; loadCam(); }   // the flight's camtrace.csv as the camera (a measurement)
                else if (o == "multi") multiOpt = true;
                else if (o == "random") lo.random = true;   // Random / Advanced Random run in the shadow (the trigger LCG; the stream from 0)
                else if (o == "randompin") { lo.random = true; lo.randomPin = true; }   // ...with the first step's draws on the in-reset seed (the mod's rngearly)
                else if (o.compare(0, 6, "scale=") == 0) { lo.scaleTrig = true; lo.levelString = o.substr(6); }
                else if (o == "all") { lo.remapChain = lo.areaEditWide = lo.areaAngle = lo.areaRadial = lo.areaScale = lo.items = true; multiOpt = true; }
            }
        }
        auto S = std::make_shared<gdtrig::TrigShadow>();
        const std::vector<gdtrig::TrigTracked> none;
        gdtrig::loadTrigShadow(*S, lo, none);
        {   // osx0 / osy0: the scale each object's row in this call's objrects holds, over the level string's (scx / sscx), by the shadow's object index (see osx0_)
            osx0Have_ = false; osx0_.clear(); osy0_.clear(); osx0Scaled_ = 0; gdev::g_objrectsStartScale = false;
            std::ifstream of(dir + "/objrects.txt"); std::string ol;
            if (of && std::getline(of, ol)) {
                std::vector<std::string> hd; { size_t p = 0; while (p <= ol.size()) { size_t q = ol.find(',', p); if (q == std::string::npos) q = ol.size(); hd.push_back(ol.substr(p, q - p)); p = q + 1; } }
                auto col = [&](const char* n) { for (size_t i = 0; i < hd.size(); ++i) if (hd[i] == n) return (int)i; return -1; };
                const int cU = col("uid"), cSx = col("scx"), cSy = col("scy"), cS0x = col("sscx"), cS0y = col("sscy");
                if (cU >= 0 && cSx >= 0 && cSy >= 0 && cS0x >= 0 && cS0y >= 0) {
                    const size_t n = S->objs.size();
                    osx0_.assign(n, 1.f); osy0_.assign(n, 1.f);
                    const int need = std::max({cU, cSx, cSy, cS0x, cS0y});
                    while (std::getline(of, ol)) {
                        std::vector<std::string> f; size_t p = 0; while (p <= ol.size()) { size_t q = ol.find(',', p); if (q == std::string::npos) q = ol.size(); f.push_back(ol.substr(p, q - p)); p = q + 1; }
                        if ((int)f.size() <= need) continue;
                        const auto it = S->byUid.find(std::atoi(f[(size_t)cU].c_str()));
                        if (it == S->byUid.end() || (size_t)it->second >= n) continue;
                        const double a = std::atof(f[(size_t)cSx].c_str()), a0 = std::atof(f[(size_t)cS0x].c_str()), b = std::atof(f[(size_t)cSy].c_str()), b0 = std::atof(f[(size_t)cS0y].c_str());
                        const double rx = a0 != 0.0 ? a / a0 : 1.0, ry = b0 != 0.0 ? b / b0 : 1.0;   // (a flip is a sign of both, which the ratio drops)
                        if (rx > 0.0 && std::isfinite(rx)) osx0_[(size_t)it->second] = (float)rx;
                        if (ry > 0.0 && std::isfinite(ry)) osy0_[(size_t)it->second] = (float)ry;
                        if (std::fabs(rx - 1.0) > 1e-6 || std::fabs(ry - 1.0) > 1e-6) ++osx0Scaled_;
                    }
                    osx0Have_ = true;
                    gdev::g_objrectsStartScale = true;
                    if (osx0Scaled_) std::printf("osx0: the call's objrects holds %lld of %zu objects at a scale other than the level string's\n", osx0Scaled_, n);
                }
            }
        }
        if (multiOpt) {   // multi.txt (cfg stopdump=1): every trigger object's m_isMultiTriggered
            std::ifstream mf((ext.empty() ? dir : ext) + "/multi.txt");
            std::string ml; std::getline(mf, ml);
            std::unordered_map<int, bool> mm;
            while (std::getline(mf, ml)) { const size_t c = ml.find(','); if (c != std::string::npos && c + 1 < ml.size()) mm[std::atoi(ml.substr(0, c).c_str())] = ml[c + 1] != '0'; }
            if (!mm.empty()) S->setMultiMap(mm);
        }
        if (events) { S->setTouchEvents({}); S->setRecordComplete(true); }   // before finish(): modelled_ is decided there
        if (gdev::evAnyListened()) S->setEventsFed();   // the Event triggers (3604) with the extra IDs 0 / 0 run: the groups hand the timeline the game events their members raised (State::evgate, feedEvents)
        {   // the Taps (Touch trigger 1595) of the level whose group holds a trigger: the group's chain starts at every press (GJEffectManager::playerButton -> toggleGroupTriggered), so the shadow runs it from the presses the
            // states raise (State::evgate's chain, game_event.hpp kTapTok). A Tap over level objects only is State::tog's (shadowApplyTapGates) and changes nothing here.
            std::map<int, const gdtrig::TrigDef*> tdOf;
            for (const gdtrig::TrigDef& d : S->triggers()) tdOf[d.uid] = &d;
            std::set<int> chainTaps, narrowTaps;
            std::vector<gdtrig::TapMove> narrowMoves;
            std::vector<gdev::TapNearBox> nearIv;
            double pastBase[8]; for (double& p : pastBase) p = std::numeric_limits<double>::infinity();
            struct ObjSz { int type; double w, h, radius; };
            std::unordered_map<int, ObjSz> objSize;   // objrects.txt by uid: how big a member is, and whether it can be touched at all
            bool hasTeleport = false;   // a teleport portal whose exit lies behind it (exit x < the portal's x): the player's x can go back, so "past x" is not an x the player stays beyond (an id-747 portal keeps x; an exit ahead only skips forward)
            {
                std::ifstream of(dir + "/objrects.txt"); std::string ol;
                if (of && std::getline(of, ol)) {
                    std::vector<std::string> hd; { size_t p = 0; while (p <= ol.size()) { size_t q = ol.find(',', p); if (q == std::string::npos) q = ol.size(); hd.push_back(ol.substr(p, q - p)); p = q + 1; } }
                    auto col = [&](const char* n) { for (size_t i = 0; i < hd.size(); ++i) if (hd[i] == n) return (int)i; return -1; };
                    const int cT = col("type"), cW = col("w"), cH = col("h"), cU = col("uid"), cR = col("radius"), cX = col("cx"), cTx = col("tpex"), cI = col("id");
                    if (cT >= 0 && cW >= 0 && cH >= 0 && cU >= 0) while (std::getline(of, ol)) {
                        std::vector<std::string> f; size_t p = 0; while (p <= ol.size()) { size_t q = ol.find(',', p); if (q == std::string::npos) q = ol.size(); f.push_back(ol.substr(p, q - p)); p = q + 1; }
                        if ((int)f.size() <= std::max({cT, cW, cH, cU})) continue;
                        if (cX >= 0 && cTx >= 0 && cI >= 0 && (int)f.size() > std::max({cX, cTx, cI}) && std::atoi(f[cI].c_str()) != 747 && std::atof(f[cTx].c_str()) != 0.0 && std::atof(f[cTx].c_str()) < std::atof(f[cX].c_str()) - std::atof(f[cW].c_str())) hasTeleport = true;   // (the exit lies behind the portal's own box; an id-747 portal puts the body at its own x, collide.hpp teleportPlayer, and its tpex is only a nominal exit beside it)
                        objSize[std::atoi(f[cU].c_str())] = {std::atoi(f[cT].c_str()), std::atof(f[cW].c_str()), std::atof(f[cH].c_str()), cR >= 0 && (int)f.size() > cR ? std::atof(f[cR].c_str()) : 0.0};
                    }
                }
            }
            gdev::g_levelMayGoBack = hasTeleport || objSize.empty();   // (objrects.txt unread: not known; plus the triggers below: Reverse / Rotate Gameplay)
            for (const gdtrig::TrigDef& t : S->triggers()) if (t.id == 1917 || t.id == 2900) { gdev::g_levelMayGoBack = true; break; }
            levelMayGoBackKeep = gdev::g_levelMayGoBack;
            long long spanMax = 0;
            std::string spanNote;
            bool tooLong = false;
            int slots = 0;
            for (gdtrig::TogTap& tp : gdtrig::g_togTable.taps) {
                tp.chain = false; tp.slot = -1; tp.cap = 255;
                const auto m = S->members.find(tp.target);
                if (m == S->members.end()) continue;
                for (size_t i : m->second) if (tdOf.count(S->objs[i].uid)) { tp.chain = true; break; }
                if (!tp.chain) continue;
                // what a press of this Tap sets going, from the level's triggers: A = the triggers its group's Spawn chain reaches at every press (a Move / anything that acts on objects makes the NUMBER of presses part of the world;
                // a Pickup feeds an item), B = what the Counts reading those items do at their thresholds (they fire once: the number of presses matters up to the highest threshold, and the effects they start run on). The span is the
                // longest (Move duration + Spawn delay) along either, in the shadow's steps: how long a press can still be changing the world.
                std::set<int> seenG, items;
                std::vector<std::pair<int, long long>> work{{tp.target, 0}};
                bool perPress = false;
                std::vector<gdtrig::TapMove> mvs; bool narrowBad = false;
                std::vector<std::pair<size_t, double>> reachJ;   // the members a press of this Tap can move or switch, with their reach (for the x beyond which the count cannot matter)
                std::set<int> ownMoves;   // the Moves this Tap spawns at every press (their offset repeats with the count)
                long long spanA = 0, spanB = 0;   // A: what the press itself sets going; B: what the Counts it feeds start
                int capCount = 1;
                auto walk = [&](bool counts) {
                    while (!work.empty()) {
                        const auto [g, acc] = work.back();
                        work.pop_back();
                        if (!seenG.insert(g).second) continue;
                        const auto mm = S->members.find(g);
                        if (mm == S->members.end()) continue;
                        for (size_t oi : mm->second) {
                            const auto it = tdOf.find(S->objs[oi].uid);
                            if (it == tdOf.end()) continue;
                            const gdtrig::TrigDef& d = *it->second;
                            // NARROW (a plain Move spawned at every press, nothing else that acts on objects): read from the trigger's columns. What the Counts it feeds start may only switch groups (Toggle) or act on no object.
                            if (d.id == 901) {
                                if (!counts && d.spawn && !d.touch && !d.lockPlayer && !d.lockCam && d.center == 0 && d.chan == 0 && !d.mvtgt && !d.dynmode && !d.lockrot) {
                                    gdtrig::TapMove m; m.tapUid = tp.uid; m.dx = d.ox; m.dy = d.oy; m.dur = d.dur; m.kind = d.ease; m.rate = d.erate;
                                    const auto tm2 = S->members.find(d.target);
                                    if (tm2 != S->members.end()) for (size_t j : tm2->second) m.uids.push_back(S->objs[j].uid);
                                    ownMoves.insert(d.uid);
                                    mvs.push_back(std::move(m));
                                } else narrowBad = true;
                            } else if (d.id != 1006 && d.id != 1007 && d.id != 899 && d.id != 1817 && d.id != 1611 && d.id != 1811 && !(counts && d.id == 1049)) narrowBad = true;
                            { long long& sp = counts ? spanB : spanA; sp = std::max(sp, acc + (long long)std::ceil((d.dur + d.sdelay) * 240.0)); }
                            if (d.id != 1268 && d.id != 1817 && d.id != 1611 && d.id != 1811 && d.id != 1616 && d.id != 899 && d.id != 1006 && d.id != 1007) {   // what the chain can change about the world the player meets: a trigger that moves, toggles or transforms its target group's members (Color / Pulse / Alpha, the Spawn / Stop / Pickup / Count triggers do not)
                                const auto tm = S->members.find(d.target);
                                if (tm != S->members.end()) for (size_t j : tm->second) {
                                    // the box the player must be in to be touched by this member while the press's effect runs: the member's position widened by the trigger's own offset and by the member's reach (its half size, or its radius, + the cube); a decoration or a trigger (objrects type 7 / 20) is never touched
                                    double reach = 30.0;   // (no objrects row for it: one cube and one grid cell)
                                    const auto sz = objSize.find(S->objs[j].uid);
                                    if (sz != objSize.end()) { if (sz->second.type == 7 || sz->second.type == 20) continue; reach = (sz->second.radius > 0 ? sz->second.radius : 0.5 * std::max(sz->second.w, sz->second.h)) + kCubeHalf; }
                                    const double x = S->objs[j].x, y = S->objs[j].y, wx = std::fabs(d.ox), wy = std::fabs(d.oy);
                                    nearIv.push_back({x - wx - reach, x + wx + reach, y - wy - reach, y + wy + reach});
                                    reachJ.emplace_back(j, reach);
                                }
                            }
                            if (d.id == 1268) work.push_back({d.target, acc + (long long)std::ceil(d.sdelay * 240.0)});
                            else if (d.id == 1817) items.insert(d.pkItem);
                            else if (d.id == 1611 || d.id == 1811 || d.id == 1049 || d.id == 1616) {}
                            else if (!counts) perPress = true;
                        }
                    }
                };
                walk(false);
                for (bool grew = true; grew;) {   // the Counts reading the items this chain feeds, and the groups they activate (a Count that is itself reached feeds on)
                    grew = false;
                    for (const auto& kv : tdOf) {
                        const gdtrig::TrigDef& c = *kv.second;
                        if ((c.id != 1611 && c.id != 1811) || !items.count(c.cntItem)) continue;
                        capCount = std::max(capCount, c.cntCount);
                        const size_t before = seenG.size();
                        work.push_back({c.target, 0});
                        walk(true);
                        grew = grew || seenG.size() != before;
                    }
                }
                tp.perPress = perPress;
                tp.cap = perPress ? 255 : std::max(1, std::min(255, capCount));
                tp.sat = (int)std::min<long long>(255, (perPress ? 0 : spanB) + gdev::kTapLag);   // (a counts-only Tap: how long after its count reached the cap what the Counts start keeps running)
                if (perPress) { spanMax = std::max(spanMax, std::max(spanA, spanB)); tooLong = tooLong || spanB + gdev::kTapLag > 31 || spanA + gdev::kTapLag > 31; }
                else tooLong = tooLong || spanB + gdev::kTapLag > 255;
                spanNote += " [Tap " + std::to_string(tp.uid) + ": " + (perPress ? "per-press effects, span " + std::to_string(spanA) + " steps, then Counts " + std::to_string(spanB) : "counts only, what the Counts start runs " + std::to_string(spanB) + " steps") + ", cap " + std::to_string(tp.cap) + "]";
                if (slots < 8) tp.slot = slots;
                ++slots;
                tp.narrow = perPress && !narrowBad && !mvs.empty();
                if (tp.narrow) {
                    for (gdtrig::TapMove& m : mvs) narrowMoves.push_back(std::move(m)); narrowTaps.insert(tp.uid);
                    // the farthest x the objects this Tap moves can reach, from the level's triggers: the member's x + reach + what every Move on a group of it can add toward +x (a Move of the level fires once; this Tap's own Move
                    // repeats with the count, up to its cap). A trigger that moves it by a rule this does not follow (a follow, a rotate, a scale, a Move that is spawned or touched by anything else) leaves the bound open.
                    std::unordered_set<size_t> js; for (const auto& r : reachJ) js.insert(r.first);
                    std::set<int> gs;
                    for (const auto& kv : S->members) for (size_t j : kv.second) if (js.count(j)) { gs.insert(kv.first); break; }
                    double up = 0.0; bool open = hasTeleport; std::string openWhy = hasTeleport ? "a teleport portal whose exit is behind it" : "";   // (a teleport can put the player back: open)
                    for (const gdtrig::TrigDef& t : S->triggers()) if (t.id == 1917 || t.id == 2900) { open = true; openWhy = "trigger " + std::to_string(t.uid) + " (id " + std::to_string(t.id) + ") can turn the player round"; break; }   // Reverse / Rotate Gameplay: x is not monotone (a Move locked to the player moves its target, not the player)
                    for (const gdtrig::TrigDef& t : S->triggers()) {
                        if (!gs.count(t.target)) continue;
                        if (t.id == 1346 || t.id == 1347 || t.id == 2067 || t.id == 1814 || t.id == 1935 || t.id == 2066) { open = true; openWhy = "trigger " + std::to_string(t.uid) + " (id " + std::to_string(t.id) + ") moves a member by a rule this does not follow"; break; }   // Rotate, Follow, Scale, Follow Player Y, Advanced Follow, Keyframe: a way to move it this does not follow
                        if (t.id != 901) continue;
                        const bool plain = !t.touch && !t.lockPlayer && !t.lockCam && t.center == 0 && t.chan == 0 && !t.mvtgt && !t.dynmode && !t.lockrot;
                        if (!plain) { open = true; openWhy = "Move " + std::to_string(t.uid) + " on a member group is not plain"; break; }
                        // how far toward +x a Move of ox can take it: its eased share over [0, 1] can run past 1 or below 0 (elastic / back), so the extremes of the ease are taken, not the end
                        double lo = 0.0, hi = 0.0;
                        for (int k = 0; k <= 256; ++k) { const double e = (double)gdtrig::eased((float)k / 256.0f, t.ease, (float)t.erate); lo = std::min(lo, e); hi = std::max(hi, e); }
                        const double toward = std::max({0.0, (double)t.ox * lo, (double)t.ox * hi});
                        if (ownMoves.count(t.uid)) up += toward * (double)tp.cap;
                        else if (t.spawn) { open = true; openWhy = "Move " + std::to_string(t.uid) + " on a member group is spawned by something else"; break; }
                        else up += toward;
                    }
                    if (open) std::printf("trigshadowlive:   Tap slot %d: its count stays in the key (the bound is open: %s)\n", tp.slot, openWhy.c_str());
                    if (!open && !reachJ.empty() && tp.slot >= 0) {
                        double hi = -1e300; for (const auto& r : reachJ) hi = std::max(hi, (double)S->objs[r.first].x + r.second);
                        pastBase[tp.slot] = hi + up;
                    }
                }
                else chainTaps.insert(tp.uid);
            }
            const long long win = spanMax + gdev::kTapLag;
            // THE LIMITS BELOW ARE THE CAPACITY OF THE FIELDS, NOT A RULE ABOUT THE GAME: 8 chain Taps (State::tapN / tapAge are 8 bytes each), 31 steps of window per press (State::tapWin is 32 bits, one of them the edge heard now),
            // 255 steps of age after a count (State::tapAge is a byte). Nothing about how a Tap behaves is decided by them: a level over a limit is simply not run by the shadow, and every Tap in it is handled as it was before
            // the chains were run (the recording supplies the group, State::tog gates the plain objects). They read from the level's triggers; a wider field lifts them.
            if (!chainTaps.empty() && (slots > 8 || tooLong)) {   // more chain Taps than State::tapN holds, or an effect that runs longer than the window the key's fields hold: the chain is not run (the recording supplies it, as before)
                std::printf("trigshadowlive: %zu Tap(s) (1595) hold a trigger in their group but the shadow does not run them: %s;%s\n", chainTaps.size(), slots > 8 ? "more than 8 chain Taps (State::tapN)" : "an effect runs longer than the key's window (31 steps per press, 255 after a count: State::tapWin / tapAge)", spanNote.c_str());
                for (gdtrig::TogTap& tp : gdtrig::g_togTable.taps) { tp.chain = false; tp.slot = -1; tp.narrow = false; }
                chainTaps.clear(); narrowTaps.clear(); narrowMoves.clear();
            }
            if (!chainTaps.empty() || !narrowTaps.empty()) {
                {   // a narrow Tap is modelled too (the groups its Move reaches are supplied by the shadow: without it they fall back to the recording), but hands over no calls: its presses are counted by the state, not run by a chain
                    std::set<int> fedTaps = chainTaps; fedTaps.insert(narrowTaps.begin(), narrowTaps.end());
                    if (!fedTaps.empty()) S->setGroupToggleFed(fedTaps);
                }
                gdev::g_tapFed = true;
                tapMovesKeep = narrowMoves; gdtrig::g_tapMoves = narrowMoves;
                gdev::g_tapWinTicks = (int)win; gdev::g_tapWinMask = win >= 32 ? 0xffffffffu : ((1u << win) - 1u);
                gdev::g_tapNear = nearIv; tapNearKeep = nearIv;
                for (int i = 0; i < 8; ++i) { gdev::g_tapPastX[i] = pastBase[i] + (std::isinf(pastBase[i]) ? 0.0 : (double)win * gdev::kMaxStepPx); tapPastKeep[i] = gdev::g_tapPastX[i]; }
                for (int i = 0; i < 8; ++i) if (!std::isinf(gdev::g_tapPastX[i])) std::printf("trigshadowlive:   Tap slot %d: its count leaves the key past x %.1f (the farthest an object it moves can reach, + the player's travel in the effect window)\n", i, gdev::g_tapPastX[i]);
                gdev::g_tapNearPx = tapNearPxKeep = (double)win * gdev::kMaxStepPx; gdev::g_tapNearPy = tapNearPyKeep = (double)win * gdev::kMaxVyStepPx;
                tapFedKeep = true; tapWinTicksKeep = gdev::g_tapWinTicks; tapWinMaskKeep = gdev::g_tapWinMask;
                tapKeep.clear(); for (const gdtrig::TogTap& tp : gdtrig::g_togTable.taps) tapKeep.push_back({tp.uid, tp.chain, tp.perPress, tp.slot, tp.cap, tp.sat, tp.narrow});
                std::printf("trigshadowlive: %zu of the level's %zu Tap(s) (1595) hold a trigger in their group: the shadow runs those chains from the presses the states raise (State::evgate); the key sees them as a count per Tap and the last %lld steps (State::tapN / tapWin)\n", chainTaps.size(), gdtrig::g_togTable.taps.size(), win);
                for (const gdtrig::TogTap& tp : gdtrig::g_togTable.taps) if (tp.chain) std::printf("trigshadowlive:   Tap %d (cx %.0f, stop %s) group %d: slot %d, counted up to %d%s\n", tp.uid, tp.cx, tp.stopCx < 1e299 ? std::to_string((long long)tp.stopCx).c_str() : "none", tp.target, tp.slot, tp.cap, tp.narrow ? " (narrow: its plain Move is applied in the pass from the presses counted, no chain in the timeline)" : "");
            }
        }
        if (events) {   // the touch Toggle Blocks (1594) this build keeps as boxes get their Events from the latch, so the shadow runs them (a switch-off box: --boxoff (removed); a switch-on one: --activators)
            std::map<int, int> tb;
            for (const TouchTrig& b : g_touch) if (b.id == 1594 && !b.spawnRing) tb[b.uid] = b.offSwitch ? 0 : (b.activator ? 1 : 2);   // (2: the loader left it out, its group holds a chain it does not follow; the shadow follows it from the trigger's Activate Group column)
            // the shadow is built once per level, from the first call's window; a later call's window can hold a touch Toggle Block this one did not (lv22: uid 11642 at x 13335 is in no early window). Every 1594 of the level that the
            // window did not name is kept as a box too, as the loader-left-out kind (2: the shadow reads its Activate Group column): its Event comes from the latch of the call whose window holds it, as for the others.
            for (const gdtrig::TrigDef& d : S->triggers()) if (d.id == 1594 && d.touch && !tb.count(d.uid)) tb[d.uid] = 2;
            if (!tb.empty()) { S->setToggleBoxes(tb); std::printf("trigshadowlive: %zu touch Toggle Block(s) (1594) run in the shadow\n", tb.size()); }
        }
        S->finish();
        if (S->eventsFed()) {   // a report: the Event triggers of the level and how many the shadow runs (their groups' Toggles with them)
            size_t nEv = 0, nRun = 0;
            for (const gdtrig::TrigDef& t : S->triggers()) if (t.id == 3604) { ++nEv; if (S->modelled(t.uid)) ++nRun; }
            std::printf("trigshadowlive: %zu Event trigger(s) (3604) in the dump, %zu run (the game events are handed over: State::evgate)\n", nRun ? nEv : nEv, nRun);
        }
        if (!tableFile.empty()) writeTable(*S);
        if (events) {
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) boxUid.push_back(g_touch[b].uid);
        }
        objCount = S->objs.size();
        for (size_t i = 0; i < S->objs.size(); ++i) if (S->coveredObject(i)) ++covered;
        trigCount = S->triggers().size();
        for (const gdtrig::TrigDef& d : S->triggers()) if (S->modelled(d.uid)) ++trigRun;
        init = S->objs;
        base = S;
        crossBuilt = false;   // (the x-crossing split's table is this shadow's)
        on = true;
        if (supplyDeco) buildDecoInert(*S, dir);
        if (supplyAll) {   // --trigsupply-all: every group a trigger the shadow runs moves is a candidate; the tests below decide, as for a group named by hand
            size_t cand = 0;
            for (int g : S->moverTargetGroups()) if (S->members.count(g) && supplyGroups.insert(g).second) ++cand;
            std::printf("trigsupply: --trigsupply-all: %zu candidate group(s) (targets of the Moves / Rotates / Scales the shadow runs), %zu with the ones named\n", cand, supplyGroups.size());
        }
        // the unit is the GROUP, and a group some of whose members the shadow covers is supplied for those members only (Orch 11-21, replacing AUD-864's whole-or-nothing; TrigShadow::partialSupplyOk keeps out a group a trigger reads as a whole); the others stay on the recording
        for (auto gi = supplyGroups.begin(); gi != supplyGroups.end();) {
            const auto mi = S->members.find(*gi);
            size_t nCov = 0;
            size_t nDeco = 0;
            if (mi != S->members.end()) for (size_t i : mi->second) { if (S->coveredObject(i)) ++nCov; else if (supplyDeco && decoInert.count(S->objs[i].uid)) { ++nCov; ++nDeco; } }
            if (nDeco) decoCounted += (long long)nDeco;
            if (mi != S->members.end() && nCov != mi->second.size()) {   // which members are not covered (the first few: uid, last trigger that acted, the groups they are in)
                int shown = 0;
                for (size_t i : mi->second) if (!S->coveredObject(i) && !(supplyDeco && decoInert.count(S->objs[i].uid)) && shown++ < 6) { std::printf("trigsupply:   group %d member uid %d is not covered (its groups:", *gi, S->objs[i].uid); for (int g2 : S->objs[i].groups) std::printf(" %d", g2); std::printf("; why: %s)\n", S->whyUncovered(i).c_str()); }
            }
            if (mi != S->members.end() && nCov > 0 && nCov != mi->second.size()) {   // --trigsupply-partial: the covered members only (the uncovered ones stay on the recording); see partialSupplyOk
                int via = 0;
                if (S->partialSupplyOk(*gi, &via)) {
                    std::printf("trigsupply: group %d is supplied for its %zu covered members of %zu ; the others stay on the recording\n", *gi, nCov, mi->second.size());
                    partialGroups.insert(*gi);
                } else std::printf("trigsupply: group %d is not supplied partly: its group %d is read as a whole by a trigger the shadow runs\n", *gi, via);
                if (!partialGroups.count(*gi)) { gi = supplyGroups.erase(gi); continue; }
            } else
            if (mi == S->members.end() || mi->second.empty() || nCov != mi->second.size()) {
                std::printf("trigsupply: group %d is NOT supplied: %zu of %zu members covered by the shadow (a group is supplied whole or not at all)\n", *gi, nCov, mi == S->members.end() ? (size_t)0 : mi->second.size());
                gi = supplyGroups.erase(gi);
                continue;
            }
            { int tu = 0, mu = 0;   // gd-04: a touch trigger that acts on this group has its box moved by a Move (the latch would read the box where the level string put it)
              if (S->groupTouchBoxMoved(*gi, &tu, &mu)) {
                  std::printf("trigsupply: group %d is NOT supplied: a touch trigger %d acts on it and its box is moved by Move %d\n", *gi, tu, mu);
                  gi = supplyGroups.erase(gi);
                  continue;
              } }
            int bVia = 0;
            if (supplyAll && S->groupReadsPlayerLockY(*gi, &bVia)) {   // --trigsupply-all: a Y lock is not supplied (custom level A:29000 g4001: 30 px off); an X lock is (custom level B g216 improves)
                std::printf("trigsupply: group %d is NOT supplied: a Move locked to the player's Y acts on its group %d (--trigsupply-all)\n", *gi, bVia);
                gi = supplyGroups.erase(gi);
                continue;
            }
            const int bnd = supplyBoundary(*S, *gi, supplyNoLock, supplyCam, &bVia);
            if (bnd == 4 && !(camModelOn || !camTrace.empty())) {   // --trigsupplycam needs the camera as an input: the camera model (cammodel=LEVEL) or a recording (cam)
                std::printf("trigsupply: group %d is NOT supplied: a Move locked to the camera X acts on its group %d and there is no camera (--trigshadowopts cammodel=LEVEL)\n", *gi, bVia);
                gi = supplyGroups.erase(gi);
                continue;
            }
            if (bnd == 4) std::printf("trigsupply: group %d is supplied although a camera X reader (a Move locked to the camera X, a screen Area Move along x) acts on its group %d (--trigsupplycam): its value follows the camera model; a timeline whose camera was uncertain keeps the model's placement\n", *gi, bVia);
            if (bnd == 1) {   // AUD-871: the camera is not an input in every tick; the owner of a group is fixed at load, so a group that reads it stays on the old path
                std::printf("trigsupply: group %d is NOT supplied: a trigger that reads the camera (a screen Area / a camera-locked Move) acts on its group %d\n", *gi, bVia);
                gi = supplyGroups.erase(gi);
                continue;
            }
            if (bnd == 2) {   // --trigsupplynolock
                std::printf("trigsupply: group %d is NOT supplied: a Move locked to the player (lockx / locky) acts on its group %d (--trigsupplynolock)\n", *gi, bVia);
                gi = supplyGroups.erase(gi);
                continue;
            }
            if (bnd == 3) std::printf("trigsupply: group %d is supplied although a Move locked to the player (lockx / locky) acts on its group %d: its value follows the group's own x curve (a world-shared value would need the curve in its key)\n", *gi, bVia);
            for (size_t i : mi->second) if (!partialGroups.count(*gi) || S->coveredObject(i) || (supplyDeco && decoInert.count(S->objs[i].uid))) supplyUids.push_back(S->objs[i].uid);
            ++gi;
        }
        supplyGroups0 = supplyGroups;
        finishSupply();
        if (!supplyGroups.empty()) std::printf("trigsupply: %zu group(s), %zu object(s) supplied by the shadow\n", supplyGroups.size(), supplyUids.size());
        if (supplyAll) { std::printf("trigsupply: --trigsupply-all groups:"); for (int g : supplyGroups) std::printf(" %d", g); std::printf("\n"); }
        std::printf("trigshadowlive: %s: objects %zu, covered %zu, triggers %zu, run %zu%s%s (touch boxes %zu)\n", dir.c_str(), objCount, covered, trigCount, trigRun, events ? ", events" : "", replay ? ", replay" : "", boxUid.size());
        // A shadow with no objects is not a shadow: every group falls back to the model and the run measures the model alone, and nothing else says so. It happened twice in a row on
        // 2026-11-15 (an offline repro whose directory had no objrects.txt: capHits that differed from the game's by a few percent, and a "cold vs exe" difference that was only this),
        // so it is said loudly, on stdout and stderr, and the run goes on (dp runs inside the game: stopping here would end the session).
        if (objCount == 0) {
            const char* msg = "trigshadowlive: ERROR: the shadow holds 0 objects (no objrects.txt, or an empty one, in %s): every supplied group falls back to the model and this run measures nothing of the trigger shadow\n";
            std::printf(msg, dir.c_str());
            std::fprintf(stderr, msg, dir.c_str());
        }
    }

    static uint64_t keyOf(float dx, uint64_t trigWord, int frame, int rev, uint64_t fsig, int xc) {
        auto mix = [](uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; };
        uint32_t d; std::memcpy(&d, &dx, 4);
        uint64_t h = mix(0x1234567ULL, d);
        h = mix(h, trigWord); h = mix(h, (uint64_t)(uint32_t)frame); h = mix(h, (uint64_t)(uint32_t)rev); h = mix(h, fsig); h = mix(h, (uint64_t)(uint32_t)xc);
        return h;
    }

    // --trigshadowcheck N (W0 of the wiring, notes/trigshadow-wiring-plan-1104.md): every Nth layer, the moving geometry the model placed for a group (its Obj, after model.prepare) is held against the
    // same group's shadow timeline -- the model's position of each mover the shadow's triggers move, against the shadow's before this layer's step (tick t-1) and after it (tick t). Observation only:
    // nothing the search reads is written. The cost of the snapshot and of the comparison is timed per compared layer.
    struct SnapObj { int uid; double x, y; };
    struct Pending { uint64_t key; double x; Line* parent; float dx = 0.0f; int frame = 0; bool snap = false; std::vector<SnapObj> objs; uint64_t wk = 0, fs = 0; long long parents = 0; uint64_t parentKey = 0; bool hasParentKey = false; bool parentNamed = false;   // parentNamed: the parent is the one the group names (its look key or its world), not a guess
                     uint64_t bits = 0; uint16_t fb[kTouchBits] = {};
                     uint64_t evg = 0;   // the group's State::evgate (the game events its members raised that an Event trigger listens for): the timeline is told the ones its parent was not (feedEvents)
                     bool cross = false; uint32_t cross0 = 0, cross1 = 0;   // the group's GroupIn counts (the x-crossing split), for the check after its step
                     double wy = std::nan("");   // the world y of the walk's single state (frame 0), NaN where a group has many states (the search): then a vertical channel's crossing waits
                     double wx = std::nan("");   // the world x of the walk's single state in a turned frame (1 / 3: world x = +-the lateral coordinate), NaN where a group has many states (the search): then the crossing reads 0 as before (a live group's lateral part is not known; making the search wait for it stalled lv22, T89)
                     std::shared_ptr<gdtrig::TrigShadow> sh; bool stepped = false; CamYGroup camY; dp::gdcam::CamLive live; };   // camY: --camy S2.1, this group's camera timeline input at tick t (camYTickFor)   // sh / stepped: --trigsupply-group stepped this group's timeline at group() already (stepNow)   // bits / fb: the group's touch latch and the latest entry tick of each box (--trigshadowevents)
    std::vector<Pending> pend;
    // --trigshadowlive also counts the worlds of the restructure draft (notes/restructure_design_draft_1105.md section 2): per layer, the number of distinct (touch mask, fire signature, xc) -- the group key without dx / frame / rev --
    // and of distinct fire signatures alone, against the groups (full keys) and the parent states the search steps; in buckets of 2000 layers to see how they grow. Observation only.
    struct Bucket { long long layers = 0, keys = 0, worlds = 0, sigs = 0, parents = 0, maxKeys = 0, maxWorlds = 0, maxSigs = 0; };
    std::map<long long, Bucket> buckets;
    long long wLayers = 0, wKeys = 0, wWorlds = 0, wSigs = 0, wParents = 0, wMaxWorlds = 0, wMaxSigs = 0;
    std::map<long long, long long> worldHist;   // worlds a layer -> number of layers
    // dupbatch (search_dp.hpp): the search hands ALL the groups of a layer to group() first and keeps what each call left as the current group (view()); it then makes one of them current again
    // (activate) before it places that group's geometry. actPend names that group in `pend` for the calls that used to mean "the group just given" (-1: the last one given to group(), as before).
    struct View { std::shared_ptr<gdtrig::TrigShadow> cur; gdtrig::CowPages<gdtrig::ObjState> pre; std::vector<int> tp, grav; long long pendIdx = -1; };
    long long actPend = -1;
    Pending& pendCur() { return (actPend >= 0 && (size_t)actPend < pend.size()) ? pend[(size_t)actPend] : pend.back(); }
    const Pending& pendCur() const { return (actPend >= 0 && (size_t)actPend < pend.size()) ? pend[(size_t)actPend] : pend.back(); }
    View view() const { View v; v.cur = supCur; v.pre = supPre; v.tp = tpFires_; v.grav = gravFires_; v.pendIdx = (long long)pend.size() - 1; return v; }
    void activate(const View& v, long long t) { supCur = v.cur; supPre = v.pre; tpFires_ = v.tp; tpFiresTick_ = t; gravFires_ = v.grav; gravFiresTick_ = t; actPend = v.pendIdx; }
    void parents(long long n) { if (on && !pend.empty()) pendCur().parents = n; }
    // --trigsupply-group G (P6-0 of the wiring, default off; notes/p6_supply_order_1105_gd04.md, notes/p6_prereq_valueat_dcx_argv_1105_gd03.md): the movers of group G take the SHADOW's place instead of the model's Dynamics
    // placement. The shadow of a layer's group is normally stepped in endLayer (after every group of the layer is known, so that a parent timeline is taken over by its last claimant), which is AFTER model.prepare has
    // placed the group's geometry. With a supplied group the timeline is stepped in group() -- always a copy of the parent's, so no claim count is needed -- and valueAt(uid) then gives the place after the step (tick t,
    // what the model's placement is, 96.4% of 20,854 movers in the W0 check) and the place before it (tick t-1) for dcx / dcy. The movers are then overwritten in search_dp.hpp after model.prepare.
    std::set<int> supplyGroups;
    bool xcont = false;                                          // --trigshadowxcont: a parentless timeline's replay goes on past the end of the x track, along the x of the earlier layers of this call (seenX)
    std::map<long long, std::vector<std::pair<float, double>>> seenX;   // per tick, the (dx, x) of the groups the call has had at that layer (--trigshadowxcont)
    long long xcontSteps = 0, xcontReplays = 0;
    long long evEnteredGiven = 0;   // replayInto: the touch Events given for triggers the attempt had entered before the anchor and the model keeps no box for
    long long gapSteps = 0, gapReplays = 0;   // replayInto: the ticks between the end of the x track and a layer, walked in a straight line (a gap before this change was one jump)
    long long supGround = 0;     // supplied movers left on the model's placement because GD's rect of them lies wholly below y = 0 (the rect's top is then raised to 0: its centre is not the position)
    // --trigsupplytake (the search side of singlePath): the search announces the keys of the layer's groups (announce) before it calls group(). A key is unique in a layer, and a group takes the parent of its own key, so
    // when every announced key already exists in the previous layer each parent has exactly one claimant and the supplied timeline can take it over instead of copying it (2.4 MB a copy on a big level). A layer with a key
    // the previous layer lacks may give that group any parent (the nearest by x): that layer copies.
    // (With GroupIn below: a group names its parent by its look key, or else by its world; the layer takes over when every group names one and no parent is named twice.)
    bool take = false;
    std::vector<uint64_t> annKeys; long long annT = -1;
    bool layerTake = false;
    long long takeLayers = 0, takeCopyLayers = 0;
    long long parentOwnHistory = 0, parentFallback = 0;   // new keys whose parent was picked among the timelines with this group's own touch history / had none (the old pick: the nearest in x, ties by the map's order)
    // ---- the x-crossing split (search_dp.hpp) -----------------------------------------------------------------------------------------------------
    // A timeline is stepped with ONE x, the front of its group, and fires an x-crossing trigger when that x reaches it: every member of the group then has the trigger dated by the front's crossing, where the
    // game (and the walk, one state stepped with its own x) dates it by each player's own. Measured on a heavy custom call: a state 7 ticks behind the front of its group was stepped with a teleport portal
    // 2.41 px further along its Move than the walk had it, touched it in the search and missed it in the walk. The search therefore keeps apart, in a layer of the plain frame, the states that have not
    // crossed the same triggers before this tick's step (cross0) and after it (cross1): a group's members then cross what its timeline fires, on the tick it fires it. The count is of the triggers the
    // shadow RUNS in channel 0's queue (a trigger it does not run fires nothing), read in the queue's order (a trigger waits for the ones before it: checkSpawnObjects).
    //   store key = the group's key with cross1: what the group's members have crossed when the step is done;
    //   look key  = the group's key with cross0: the timeline of the previous layer that has fired exactly what they had crossed before it.
    // A group that crosses on this tick (cross0 < cross1) and a group that crossed before (cross1 on both sides) would store under one key; the search salts the first (GroupIn::salt), and the layer after
    // finds its timeline by `world`: the store key of the group that stepped the group's first member, which the search knows from the arena (search_dp.hpp WorldRange). `world` is also how any group whose
    // key the previous layer lacks finds its parent, before the guess by touch history and x below is tried.
    struct GroupIn { bool cross = false; uint32_t cross0 = 0, cross1 = 0; bool salt = false; bool hasWorld = false; uint64_t world = 0; uint64_t evg = 0; };   // evg: the group's State::evgate (its members share it: the signature of the group's key holds it)
    GroupIn gin;   // the next group()'s (group() takes it and clears it: every other caller gives none)
    static uint64_t crossMix(uint64_t k, uint64_t v) { k ^= v + 0x9e3779b97f4a7c15ULL + (k << 6) + (k >> 2); return k * 0xff51afd7ed558ccdULL; }
    static uint64_t storeKeyOf(uint64_t k, const GroupIn& in) { if (!in.cross) return k; uint64_t s = crossMix(k, 0x78630000ULL + in.cross1); return in.salt ? crossMix(s, 0x73616c74ULL + in.cross0) : s; }
    static uint64_t lookKeyOf(uint64_t k, const GroupIn& in) { return in.cross ? crossMix(k, 0x78630000ULL + in.cross0) : k; }
    std::vector<GroupIn> annIn;   // announce: each announced key's GroupIn (empty: none)
    void announce(long long t, const std::vector<uint64_t>& keys, const std::vector<GroupIn>& ins) { if (!on || !take) return; annT = t; annKeys = keys; annIn = ins; }
    long long parentByWorld = 0;   // groups whose key the previous layer lacked and whose parent is the timeline their first member was stepped in
    long long crossSteps = 0, crossShort = 0, crossOver = 0;   // timelines stepped for a group with counts; those that had passed fewer / more running triggers than the group has crossed (stepNow)
    // channel 0's queue: the prefix maximum of its x, and the running triggers among its first n. Of the level the shadow is built for: this object outlives a level in the mod (one game, many levels), so
    // the table is dropped with the level (resetLevel) and wherever `base` is made -- not told by base's address, which the next level's shadow can be given again.
    std::vector<double> crossPm; std::vector<uint32_t> crossRun; bool crossBuilt = false;
    void crossBuild() {
        crossBuilt = true; crossPm.clear(); crossRun.assign(1, 0u);
        const std::vector<size_t>* q = base ? base->chanQueue(0) : nullptr;
        if (!q) return;
        double m = -1e300;
        for (size_t n = 0; n < q->size(); ++n) {
            const gdtrig::TrigDef& T = base->triggers()[(*q)[n]];
            m = std::max(m, (double)T.cx); crossPm.push_back(m);
            crossRun.push_back(crossRun.back() + (base->kindRunsNow(T) ? 1u : 0u));
        }
    }
    bool crossOn() { if (!on || !built || !base) return false; if (!crossBuilt) crossBuild(); return crossRun.back() > 0; }   // (not before this call's shadow is built: the first layer of a new level's first call is not split)
    uint32_t crossCount(double x) const { return crossRun[(size_t)(std::upper_bound(crossPm.begin(), crossPm.end(), x) - crossPm.begin())]; }   // (after crossOn())
    // ---- --ladderprefix (search_dp.hpp LadderPrefixSnap): a layer's timelines, kept by a save and given back to the attempt that resumes from it ----
    // A resumed attempt used to start with no timeline: its first layer does not follow the last one this object was given, so the parents were dropped and each group's timeline was REPLAYED (replayInto) --
    // along GD's x track up to the anchor and, past it, along the x of whichever group the call had at each earlier tick (seenX: the first with the group's present dx, else the first), with no world y.
    // That is not the history the group's own states came by. The save now holds the layer's timelines as they are, and the resumed attempt's first layer finds its parents there (by key, or by world:
    // the save keeps the search's ranges of arena nodes too), exactly as a layer of an attempt that was never interrupted does.
    struct SavedLayer { bool have = false; long long t = -1; std::unordered_map<uint64_t, Line> lines; };
    long long savedLayers = 0, restoredLayers = 0;
    void saveLayer(SavedLayer& S, long long t) {
        S = SavedLayer{};
        if (!on || !supplyOn() || lastT != t) return;   // (without the supply a layer's timelines are only made when the next layer starts: there is nothing to keep yet, and the resume replays as before)
        for (auto& kv : cur) kv.second.shared = true;    // the save holds these timelines from now on: the next layer copies them instead of stepping them in place
        S.have = true; S.t = t; S.lines = cur;
        ++savedLayers;
    }
    void restoreLayer(const SavedLayer& S) {
        if (!on || !S.have) return;
        endLayer();   // (what the last search or walk left pending)
        prev.clear(); cur = S.lines; lastT = S.t;
        stepMemo.clear(); layerMoved = false; layerTake = false; actPend = -1;
        ++restoredLayers;
    }
    size_t savedBytes(const SavedLayer& S) const {   // (every line counted whole: the pages a save shares with the live timelines or with another save are counted again, on the safe side of the cap)
        return S.lines.size() * (sizeof(Line) + sizeof(gdtrig::TrigShadow) + objCount * sizeof(gdtrig::ObjState));
    }
    // V1 of the behind-the-anchor design (notes/touch_behind_anchor_design_gd04_1117.md): can the anchor's history decide a touch trigger whose box lies behind the anchor? 0 = decided, it was entered (a tick > 0 in
    // --touchentered: replayInto gives the shadow its Event); 1 = decided, it was not entered; else why not: 2 no payload, 3 no x track, 4 the shadow does not run it, 5 a toggle block (1594: pressed, not touched),
    // 6 multi-trigger, 7 its box rides a mover's group (the rows' overlap test reads the box where the level string put it), 8 unknown trigger, 9 a touch Spawn (1268)
    // how the flight's rows entered a touch box at tick e (the --xtrack rows with the world columns): 1 = a jump (the position moved 30 px or more between row e - 1 and row e: a spider's jump, a portal, a teleport),
    // 0 = continuous, -1 = the rows to say are not there. V2 (64 customs, 905 touch triggers GD fired and the rows entered): GD's fire tick is the overlap tick - 1 in 728 of 773 continuous entries (95%), and the
    // overlap tick itself in 114 of 118 jumps (97%); no step is between 6 and 30 px. The shadow's step is the game's tick + 1, so a continuous entry is filed at the overlap tick and a jump at the overlap tick + 1.
    int entryJump(long long e) const {
        if (g_xTrack.empty() || g_xTrackW.size() != g_xTrack.size()) return -1;
        const auto it = std::lower_bound(g_xTrack.begin(), g_xTrack.end(), e, [](const std::pair<int, double>& a, long long v) { return (long long)a.first < v; });
        if (it == g_xTrack.end() || (long long)it->first != e || it == g_xTrack.begin()) return -1;
        const size_t k = (size_t)(it - g_xTrack.begin());
        if ((long long)g_xTrack[k - 1].first != e - 1) return -1;
        return (std::fabs(g_xTrackW[k][0] - g_xTrackW[k - 1][0]) >= 30.0 || std::fabs(g_xTrackW[k][1] - g_xTrackW[k - 1][1]) >= 30.0) ? 1 : 0;
    }
    int touchBehindDecision(const gdtrig::TrigShadow& S, int uid) const {
        const gdtrig::TrigDef* t = S.triggerByUid(uid);
        if (!t) return 8;
        if (!g_touchEnteredGiven) return 2;
        if (g_xTrack.empty()) return 3;
        if (!S.modelled(uid)) return 4;
        if (t->id == 1594) return 5;
        if (t->id == 1268) return 9;   // a touch Spawn: 14 of 96 in the 64 customs were entered and never fired (V2), so the rows' overlap does not say whether its chain started
        if (t->multi) return 6;
        if (S.touchBoxRidesMover(uid)) return 7;
        const auto et = g_touchEnteredT.find(uid);
        if (et == g_touchEnteredT.end() || et->second <= 0) return 1;
        return entryJump(et->second) < 0 ? 3 : 0;   // an entry the shadow files by how the rows entered needs those rows (with the world columns)
    }
    long long behindGroups = 0, behindDecidable = 0, behindMovers = 0, behindDecidableMovers = 0, behindTrigs[10] = {};
    // true when the anchor's history decides every touch trigger behind the anchor that acts on / gates group g: the refusal is lifted (the replay gives the shadow the Events, entered ones filed by entryJump)
    bool noteBehind(const gdtrig::TrigShadow& S, int g, double x0) {
        ++behindGroups;
        const auto mi = S.members.find(g);
        const long long nm = mi == S.members.end() ? 0 : (long long)mi->second.size();
        behindMovers += nm;
        const std::vector<int> us = S.touchBehindUids(g, x0);
        bool all = !us.empty();
        for (int u : us) { const int d = touchBehindDecision(S, u); ++behindTrigs[d]; if (d > 1) all = false; }
        if (all) { ++behindDecidable; behindDecidableMovers += nm; }
        return all;
    }
    std::set<int> supplyGroups0;   // the supplied groups as ensure() left them: what every call starts from, before its own anchor check (a call's check drops the groups whose touch trigger lies behind THAT call's anchor)
    bool anchorSeen = false;     // the first group() of the run has been seen (the anchor's x is known: touch movers behind it are checked)
    bool singlePath = false;    // set by the --replay walk: one group a tick on one path, so a supplied group's timeline takes its parent over instead of copying it (the copy was 1.1 ms a layer on a big level)
    bool layerMoved = false;     // this layer's group took its parent over (singlePath): a second group of the same layer would find the parent already stepped
    long long singleViolations = 0;
    bool supplyNoLock = false;   // --trigsupplynolock: a group a Move locked to the player acts on is not supplied (default: it is, the timeline follows the group's own x curve)
    // the load-time boundary of one supplied group (after the coverage test): 0 supplied; 1 dropped, a camera reader acts on it; 2 dropped, a player lock acts on it and --trigsupplynolock is on; 3 supplied although a player lock acts on it (noted in the log)
    // 4 (--trigsupplycam): supplied although a Move locked to the camera X acts on it -- the only camera reader the supply takes over, and only when no player lock acts on it as well (then 2 / 3)
    static int supplyBoundary(const gdtrig::TrigShadow& S, int g, bool noLock, bool allowCam, int* via) {
        const int cr = S.groupCameraReader(g, via);
        if (cr == 1 || (cr == 2 && !allowCam)) return 1;
        if (S.groupReadsPlayerLock(g, via)) return noLock ? 2 : 3;
        return cr == 2 ? 4 : 0;
    }
    long long supFrame = 0;     // --trigsupply-all: supplied group-ticks left on the model's placement because the group is in a rotated gameplay frame
    // --trigshadowworld (default off): the shadow's timelines step with the PLAYER'S WORLD position, not (x, 0). A crossing trigger of a vertical channel (a turned gameplay frame: lv22's maze) is compared on the
    // player's world y, which the replay's x-only track and the live layers' (x, 0) never gave: the shadow entered a vertical channel at its first Rotate Gameplay and never left it, ran no x-crossing trigger of the
    // channel the game was in, and the supply put its still platforms over the right recording (lv22 5617: Move 5103 at x=7305 never fired, the platform of group 197 stayed at y 885 while GD's sank 12 px). The
    // replay reads --xtrack's y and gframe columns (g_xTrackW: the mod writes them, py/dump world y) when the file has them; a live layer's group in a turned frame makes its world (x, y) from the progress axis
    // (State::xAbs; the lateral part is not known per group and stays 0: a crossing reads the travel axis only). Frame 0 is the old call (x, 0).
    bool supplyWorld = false;
    void stepCoords(int frame, double u, double& X, double& Y) const {
        X = u; Y = 0.0;
        if (!supplyWorld) return;
        switch (frame & 3) { case 0: break; case 1: X = 0.0; Y = -u; break; case 2: X = -u; break; default: X = 0.0; Y = u; break; }
    }
    // --trigsupplydeco (default off): a Decoration (objrects type 7: no hitbox) member the shadow does not cover does not keep its group from being supplied, unless something reads its place -- it is the object
    // a trigger took for its centre / Move-To measure (ctrobj, mva), or it is in a group some trigger names as its centre, or an Area reaches it through its parent / child arrays. Such a member can still be
    // moved by a trigger the shadow does not run through ANOTHER of its groups; that moves the decoration, not the group's timeline, and no hitbox. (604 sections: 5,662 groups were left out for such members, 95.8% of them hold one.)
    bool supplyArea = false;   // --trigsupplyarea (default off; with --trigsupply-all): the objects an Area Move / Scale acts on are movers the supply places (triggers.hpp loadAreaTargetMovers)
    bool supplyDeco = false;
    std::unordered_set<int> decoInert;   // the uids that are not counted (built with the shadow)
    long long decoCounted = 0, decoReferenced = 0, decoAreaReached = 0;
    void buildDecoInert(const gdtrig::TrigShadow& S, const std::string& refDir) {
        decoInert.clear(); decoCounted = decoReferenced = decoAreaReached = 0;
        std::unordered_set<int> deco;
        std::ifstream f(refDir + "/objrects.txt");
        std::string line;
        if (!std::getline(f, line)) { std::printf("trigsupply: --trigsupplydeco: no %s/objrects.txt: nothing is left out\n", refDir.c_str()); return; }
        int cu = -1, ct = -1;
        { std::vector<std::string> h; size_t p = 0; while (true) { const size_t q = line.find(',', p); h.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
          for (size_t k = 0; k < h.size(); ++k) { std::string n = h[k]; while (!n.empty() && (n.back() == '\r' || n.back() == ' ')) n.pop_back(); if (n == "uid") cu = (int)k; if (n == "type") ct = (int)k; } }
        if (cu < 0 || ct < 0) { std::printf("trigsupply: --trigsupplydeco: objrects.txt has no uid / type column: nothing is left out\n"); return; }
        while (std::getline(f, line)) {
            std::vector<std::string> c; size_t p = 0;
            while (true) { const size_t q = line.find(',', p); c.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
            if ((int)c.size() <= std::max(cu, ct)) continue;
            if (std::atoi(c[(size_t)ct].c_str()) == 7) deco.insert(std::atoi(c[(size_t)cu].c_str()));
        }
        std::unordered_set<int> referenced;
        for (const gdtrig::TrigDef& d : S.triggers()) {
            if (d.ctrObj > 0) referenced.insert(d.ctrObj);
            if (d.mvaObj > 0) referenced.insert(d.mvaObj);
            if (d.center > 0) { const auto mi = S.members.find(d.center); if (mi != S.members.end()) for (size_t i : mi->second) referenced.insert(S.objs[i].uid); }
        }
        for (int u : deco) {
            if (referenced.count(u)) { ++decoReferenced; continue; }
            if (S.uncoveredBy(u) != 0) { ++decoAreaReached; continue; }
            decoInert.insert(u);
        }
        std::printf("trigsupply: --trigsupplydeco: %zu decoration object(s), %zu are not counted when a group is tested (%lld are a trigger's centre / measure object or in a centre group, %lld are reached by an Area's parent / child arrays)\n", deco.size(), decoInert.size(), decoReferenced, decoAreaReached);
    }
    bool supplyAll = false;     // --trigsupply-all (default off): every group the shadow's Moves / Rotates / Scales target that passes the load tests is supplied, as if named with --trigsupply-group
    bool supplyCam = false;     // --trigsupplycam: a group a Move locked to the camera X acts on can be supplied (default off: it stays on the model's placement, AUD-871)
    long long supCamBlind = 0;  // supplied group-layers left on the model's placement because the timeline's camera was not certain
    int dbgFrame = 0, dbgRev = 0;   // the group being supplied (diagnosis print only)
    long long dbgParents = 0;
    std::set<int> partialGroups;                  // the supplied groups that are supplied for their covered members only
    std::vector<int> supplyUids;                // the uids of those groups' objects, built with the shadow
    struct SupVal { double x = 0, y = 0, px = 0, py = 0, rot = 0; bool pon = true; float sx = 1.f, sy = 1.f, psx = 1.f, psy = 1.f; };   // psx / psy: the own scale BEFORE this tick's step   // pon: the object's disable counter >= 0 BEFORE this tick's step (T29: the collision-time on/off)
    // the current group's supplied movers: place after its step (x, y) and before it (px, py). A flat table over supplyUids (slot = index in the sorted list), refilled in place for each timeline: a hash map of ~4,000
    // entries built and torn down for every group step was a heap allocation per mover.
    // Since E8 W2 the values are not copied into a table per group step (two walks of every supplied uid, some 4,000, for each of the 350,000 group steps of a call): they are read where they are --
    // `supCur` is the group's timeline after its step, `supPre` its objects as they stood before it (a CowPages copy: it shares the pages, and the step clones the few it writes). supVal() is the
    // one reader; a slot is "filled" when its object is in the shadow and the step did not leave it blind, as supOk had it.
    std::vector<int> supObj;                 // per slot: the object's index in the shadow (-1: not in it)
    std::unordered_map<int, int> supSlot;    // uid -> slot
    // The same answer without the hash (speed only): supFlat[uid] is the slot or -1, built with supSlot when the supplied uids are non-negative and their largest is small enough for a table (else it stays
    // empty and the map answers). The placement asks the on/off of every recording-less object of every group step (onAt): the map's find was 16% of applyTriggersImpl on Grief (2026-11-20, 1 thread).
    std::vector<int> supFlat;
    int slotOf(int uid) const {
        if (!supFlat.empty()) return uid >= 0 && (size_t)uid < supFlat.size() ? supFlat[(size_t)uid] : -1;
        const auto it = supSlot.find(uid); return it == supSlot.end() ? -1 : it->second;
    }
    std::shared_ptr<gdtrig::TrigShadow> supCur;
    gdtrig::CowPages<gdtrig::ObjState> supPre;
    bool supAny = false;
    // The objects' own scale AS THE CALL'S OBJRECTS HOLDS IT, as a ratio to the level string's (objrects scx / sscx, sscx = GameObject::m_startScaleX): ObjState::osx / osy is the ratio to the level-string
    // scale (the timeline replays every Scale from tick 0), while the model's objects -- the objrects and obb a call reads -- stand as the reset that wrote them left them, with every Scale an earlier
    // attempt had run (to its death) already in their size, and the size the readers multiplied by osx was counted twice. What a reader wants is the ratio since the dump, osx / osx0. Not read (no sscx
    // column in an older objrects) reads as 1: the timeline's ratio as it was.
    // a custom level uid 16076: scx 0.019068 in the call's objrects against 0.227 at the level's start (osx0 0.0840); osx(14802) = 0.3527, GD's box 0.3527 / 0.0840 = 4.198 times the dumped one.
    std::vector<float> osx0_, osy0_;
    bool osx0Have_ = false;
    long long osx0Scaled_ = 0;
    float osx0Of(size_t ix) const { return osx0Have_ && ix < osx0_.size() && osx0_[ix] != 0.f ? osx0_[ix] : 1.f; }
    float osy0Of(size_t ix) const { return osx0Have_ && ix < osy0_.size() && osy0_[ix] != 0.f ? osy0_[ix] : 1.f; }
    bool supVal(size_t slot, SupVal& v) const {
        const int ix = supObj[slot];
        if (ix < 0 || !supCur) return false;
        const gdtrig::ObjState& o = supCur->objs[(size_t)ix];
        if (o.blind) return false;   // a camera-locked Move stepped it with an uncertain camera (or it turned about such an object): its place is not the game's, the model's placement stays for it
        const gdtrig::ObjState& q = supPre[(size_t)ix];
        const float d0x = osx0Of((size_t)ix), d0y = osy0Of((size_t)ix);
        v.x = o.x; v.y = o.y; v.rot = o.rot; v.sx = o.osx / d0x; v.sy = o.osy / d0y; v.psx = q.osx / d0x; v.psy = q.osy / d0y; v.px = q.x; v.py = q.y; v.pon = q.on();
        return true;
    }
    // the own scale since the objrects were written (osx / osx0) of ANY object of the shadow, supplied or not: a mover with a recording is in no slot (supVal), but the timeline carries every member of a
    // scaled group along. Read only; false while the objrects has no sscx column (the dump's scale is then not known).
    bool ownScaleAt(int uid, float& sx, float& sy, float& psx, float& psy) const {
        if (!base || !supCur || !osx0Have_) return false;
        const auto bi = base->byUid.find(uid);
        if (bi == base->byUid.end()) return false;
        const size_t ix = (size_t)bi->second;
        if (ix >= supPre.size() || ix >= supCur->objs.size()) return false;
        const gdtrig::ObjState& o = supCur->objs[ix];
        if (o.blind) return false;
        const gdtrig::ObjState& q = supPre[ix];
        const float d0x = osx0Of(ix), d0y = osy0Of(ix);
        sx = o.osx / d0x; sy = o.osy / d0y; psx = q.osx / d0x; psy = q.osy / d0y;
        return true;
    }
    // dupbatch: how many of the model's movers have no slot in the supply, or a slot with no object in the shadow. Those stay on the model's own placement in every frame, and that placement reads the
    // group's touch fire ticks, which the shadow's content does not hold (search_dp.hpp: a leader's build then notes the ticks it reads for those movers). Counted once for a set of slots.
    // hasSlot is the same test for one uid; supGen counts the sets of slots (finishSupply), so a table built from hasSlot knows when it is stale.
    long long supGen = 0;
    bool hasSlot(int uid) const { const int sl = slotOf(uid); return sl >= 0 && supObj[(size_t)sl] >= 0; }
    long long unsupMovers_ = -1;
    template <class V> long long unsupMovers(const V& dynObjs) {
        if (unsupMovers_ < 0) {
            long long n = 0;
            std::string first;
            for (const auto& o : dynObjs) { const auto it = supSlot.find(o.uid); if (it == supSlot.end() || supObj[(size_t)it->second] < 0) { if (n < 40) first += " " + std::to_string(o.uid); ++n; } }
            unsupMovers_ = n;
            std::printf("dupbatch: %lld of the model's %zu movers have no slot in the supply (%zu slots):%s\n", n, (size_t)dynObjs.size(), supSlot.size(), first.c_str());
        }
        return unsupMovers_;
    }
    void finishSupply() {   // supplyUids is final: sort, slots, object indices
        std::sort(supplyUids.begin(), supplyUids.end()); supplyUids.erase(std::unique(supplyUids.begin(), supplyUids.end()), supplyUids.end());
        supSlot.clear(); supObj.assign(supplyUids.size(), -1);
        for (size_t i = 0; i < supplyUids.size(); ++i) {
            supSlot[supplyUids[i]] = (int)i;
            if (base) { const auto bi = base->byUid.find(supplyUids[i]); if (bi != base->byUid.end()) supObj[i] = (int)bi->second; }
        }
        supFlat.clear();
        if (!supplyUids.empty() && supplyUids.front() >= 0 && supplyUids.back() < (1 << 24)) {   // (sorted: front the smallest, back the largest; 16M uids = 64 MB is the limit of the table)
            supFlat.assign((size_t)supplyUids.back() + 1, -1);
            for (size_t i = 0; i < supplyUids.size(); ++i) supFlat[(size_t)supplyUids[i]] = (int)i;
        }
        supCur.reset(); supPre.clear(); supAny = false; unsupMovers_ = -1; ++supGen;
    }
    bool supplyOn() const { return on && (!supplyGroups.empty() || (supplyAll && !built)); }   // --trigsupply-all: the groups are chosen when the shadow is built (ensure(), at the first group)
    // The player-effect Teleports (id 3022) the current group's timeline fired in ITS step at tick t (stepNow: trigger uids in firing order, the timeline's own crossing / spawn-chain / touch decision). Filled with
    // supplyOn() (every group is stepped in group() then) right after the step, before model.prepare and the parallel step of the group's states, and read-only until the next group(): the same discipline as valueAt.
    // Nobody read TrigShadow::events before (written, never read). tpFiresAt(t) is the list for tick t of the group last stepped, empty for any other tick. The trigger's own parameters (tpx / tpy / tpg ...) are the
    // reader's own table by uid (the old model's Teleport registry), TrigShadow::Event::raw is the objrects columns as text and needs no parsing for that.
    std::vector<int> tpFires_; long long tpFiresTick_ = -1;
    const std::vector<int>& tpFiresAt(long long t) const { static const std::vector<int> none; return t == tpFiresTick_ ? tpFires_ : none; }
    // The Gravity triggers (id 2066) the same step fired (TrigShadow::gravChanges: the timeline's Spawn chain / Event decision, not the x crossing or a touch of the model), uids in firing order. Filled and read as tpFiresAt.
    std::vector<int> gravFires_; long long gravFiresTick_ = -1;
    const std::vector<int>& gravFiresAt(long long t) const { static const std::vector<int> none; return t == gravFiresTick_ ? gravFires_ : none; }
    bool valueAt(int uid, double& x, double& y, double& px, double& py) const {
        const int sl = slotOf(uid);
        SupVal v;
        if (sl < 0 || !supVal((size_t)sl, v)) return false;
        x = v.x; y = v.y; px = v.px; py = v.py;
        return true;
    }
    // the current group's supplied movers whose place after its step is in [x0, x1] (uid, x): what Dynamics::collect adds to the group's lists when the model's own place left them out (dynamics.hpp SupHits).
    // Read-only like valueAt, by slot in ascending order.
    //
    // It was a scan of every supplied object in the timeline (supplyHitsScan below): a few thousand on the official levels, but 25,921 objects of ~130 bytes each on a custom level with that many movers
    // (a custom level), for every group of every layer and every tick of a walk -- 0.7 to 1.0 ms a call, 85% of prepareGroup's `near` bucket there (2026-11-18). A step writes a few dozen objects, so the
    // x of every slot (NaN: no object, or blind) is kept in a flat array and only the pages of the timeline's objects that are not the ones read last are read again (CowPages::Watch: a page it holds
    // is never written, so the same page is the same content). Timelines that went apart long ago differ in many pages, so there are up to kSupXsMax arrays: the call takes the most recently used one
    // within kSupXsNear pages of the timeline, else starts another from a copy of the nearest (or takes the least recently used one over with that copy). Which array is taken never changes the
    // answer -- each is brought to the timeline's content before it is read -- only how many pages are read. The scan then is the array's: the same slots, in the same order, with the same x.
    // A copy is the whole array (one double a slot) and its page list: as much as reading some hundreds of pages. On a level whose step writes more than kSupXsNear pages (a walk of one plan on the
    // level above: 117 pages a tick) no array was ever near, and every call copied the nearest one and then read the 117 pages. So the nearest array within kSupXsStep pages is brought forward
    // in place, with no copy; only a timeline farther than that from every array starts (or takes over) another.
    // The scan of the array was every slot for every call: 25,921 doubles for some 200 hits, 62 of the call's 110 microseconds on the level above (the pages read again 42, the choice of the array 6;
    // measured over the 12,055 calls of one search and its walk). Each array now keeps, for every page that holds a supplied object, the least and the greatest x of that page's slots (NaN slots
    // left out; none: a range no window meets), set where the page is read. A call looks at the ranges and opens only the pages whose range meets the window. The hits come out by page then, so they are
    // put back in ascending slot order, which is the order the scan gave them in.
    // SUPHITS_CHECK=1 (env, read once): every answer is held against the scan of the objects themselves, and a difference stops the process.
    struct SupXs { gdtrig::CowPages<gdtrig::ObjState>::Watch watch; std::vector<double> x, lo, hi; long long used = 0; };   // lo / hi: per page of SupXIndex::page, the range of its slots' x
    static constexpr size_t kSupXsMax = 6, kSupXsNear = 24, kSupXsStep = 256;
    struct SupXIndex {
        long long gen = -1; size_t slots = 0, pages = 0;   // the supply (supGen), its slots and the timeline's pages this index was built for
        std::vector<uint32_t> page, start;                 // the pages that hold a supplied object, ascending; start[k] .. start[k + 1] = that page's slots in `slot`
        std::vector<int> slot;
        std::vector<SupXs> xs; long long clock = 0;
        long long calls = 0, pagesRead = 0, arrays = 0, checked = 0, pagesOpened = 0;   // (a report)
        std::vector<std::pair<int, double>> hit;   // (a call's hits by slot, before they are put in order)
    };
    mutable SupXIndex supIx_;
    void supplyHitsScan(double x0, double x1, std::vector<std::pair<int, double>>& out) const {
        for (size_t i = 0; i < supObj.size(); ++i) {
            if (supObj[i] < 0) continue;
            const gdtrig::ObjState& o = supCur->objs[(size_t)supObj[i]];
            if (!o.blind && o.x >= x0 && o.x <= x1) out.emplace_back(supplyUids[i], o.x);
        }
    }
    void supplyHits(double x0, double x1, std::vector<std::pair<int, double>>& out) const {
        if (!supCur) return;
        static std::mutex mu;   // the arrays are the one thing this reader writes: one caller at a time
        std::lock_guard<std::mutex> lk(mu);
        const gdtrig::CowPages<gdtrig::ObjState>& objs = supCur->objs;
        SupXIndex& ix = supIx_;
        if (ix.gen != supGen || ix.slots != supObj.size() || ix.pages != objs.pageCount()) {
            ix.gen = supGen; ix.slots = supObj.size(); ix.pages = objs.pageCount();
            ix.xs.clear(); ix.xs.reserve(kSupXsMax);   // (reserved: the pointers into it below stay good across a push_back)
            std::vector<std::pair<uint32_t, int>> ps;
            for (size_t i = 0; i < supObj.size(); ++i) if (supObj[i] >= 0) ps.emplace_back((uint32_t)objs.pageOf((size_t)supObj[i]), (int)i);
            std::sort(ps.begin(), ps.end());
            ix.page.clear(); ix.start.clear(); ix.slot.clear();
            for (const auto& e : ps) {
                if (ix.page.empty() || ix.page.back() != e.first) { ix.page.push_back(e.first); ix.start.push_back((uint32_t)ix.slot.size()); }
                ix.slot.push_back(e.second);
            }
            ix.start.push_back((uint32_t)ix.slot.size());
        }
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        SupXs* c = nullptr; SupXs* best = nullptr; size_t bestD = (size_t)-1;
        std::array<SupXs*, kSupXsMax> ord{}; size_t n = 0;
        for (SupXs& s : ix.xs) ord[n++] = &s;
        std::sort(ord.begin(), ord.begin() + n, [](const SupXs* a, const SupXs* b) { return a->used > b->used; });
        for (size_t k = 0; k < n && !c; ++k) {
            const size_t d = ord[k]->watch.differing(objs, ix.page);
            if (d < bestD) { bestD = d; best = ord[k]; }
            if (d <= kSupXsNear) c = ord[k];
        }
        if (!c && best && bestD <= kSupXsStep) c = best;
        if (!c) {
            if (ix.xs.size() < kSupXsMax) {
                if (best) ix.xs.push_back(*best); else { ix.xs.emplace_back(); ix.xs.back().x.assign(supObj.size(), nan); ix.xs.back().lo.assign(ix.page.size(), inf); ix.xs.back().hi.assign(ix.page.size(), -inf); }
                c = &ix.xs.back(); ++ix.arrays;
            } else {
                c = ord[n - 1];
                if (c != best) *c = *best;
            }
        }
        c->watch.sync(objs, ix.page, [&](size_t k) {
            double lo = inf, hi = -inf;   // (no slot of the page has an x: a range no window meets)
            for (uint32_t j = ix.start[k]; j < ix.start[k + 1]; ++j) {
                const size_t s = (size_t)ix.slot[j];
                const gdtrig::ObjState& o = objs[(size_t)supObj[s]];
                c->x[s] = o.blind ? nan : o.x;
                if (o.blind || o.x != o.x) continue;
                if (o.x < lo) lo = o.x;
                if (o.x > hi) hi = o.x;
            }
            c->lo[k] = lo; c->hi[k] = hi;
            ++ix.pagesRead;
        });
        c->used = ++ix.clock; ++ix.calls;
        const size_t out0 = out.size();
        const double* const xs = c->x.data();
        const double* const plo = c->lo.data(); const double* const phi = c->hi.data();
        ix.hit.clear();
        for (size_t k = 0, m = ix.page.size(); k < m; ++k) {
            if (!(phi[k] >= x0 && plo[k] <= x1)) continue;
            ++ix.pagesOpened;
            for (uint32_t j = ix.start[k]; j < ix.start[k + 1]; ++j) { const int s = ix.slot[j]; const double v = xs[(size_t)s]; if (v >= x0 && v <= x1) ix.hit.emplace_back(s, v); }
        }
        std::sort(ix.hit.begin(), ix.hit.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.first < b.first; });
        for (const auto& h : ix.hit) out.emplace_back(supplyUids[(size_t)h.first], h.second);
        static const bool check = std::getenv("SUPHITS_CHECK") != nullptr;
        if (check) {
            std::vector<std::pair<int, double>> ref;
            supplyHitsScan(x0, x1, ref);
            bool same = ref.size() == out.size() - out0;
            for (size_t i = 0; same && i < ref.size(); ++i) same = ref[i].first == out[out0 + i].first && std::memcmp(&ref[i].second, &out[out0 + i].second, sizeof(double)) == 0;
            ++ix.checked;
            if (!same) {
                std::printf("suphits: MISMATCH at call %lld (layer %lld, window %.3f..%.3f): the arrays give %zu hits, the scan of the objects %zu\n", ix.calls, lastT, x0, x1, out.size() - out0, ref.size());
                std::fflush(stdout);
                std::abort();
            }
        }
    }
    // the supplied object's own scale after the group's step, as a ratio to its load scale (ObjState::osx / osy): a Transform that scales takes it along; the saw's radius reads it (collide.hpp circleHit)
    bool scaleAt(int uid, float& sx, float& sy) const {
        const int sl = slotOf(uid);
        SupVal v;
        if (sl < 0 || !supVal((size_t)sl, v)) return false;
        sx = v.sx; sy = v.sy;
        return true;
    }
    // ...and the same before the group's step (the step before's scale): what GD sizes the object's hitbox by on this tick (shadow_step.hpp shadowPlaceMovers, the plain rect's scale)
    bool scaleAt(int uid, float& sx, float& sy, float& psx, float& psy) const {
        const int sl = slotOf(uid);
        SupVal v;
        if (sl < 0 || !supVal((size_t)sl, v)) return false;
        sx = v.sx; sy = v.sy; psx = v.psx; psy = v.psy;
        return true;
    }
    // the supplied object's own angle after the group's step (ObjState::rot, degrees as the game holds them: the level's angle plus what the Rotates have added)
    bool rotAt(int uid, double& rot) const {
        const int sl = slotOf(uid);
        SupVal v;
        if (sl < 0 || !supVal((size_t)sl, v)) return false;
        rot = v.rot;
        return true;
    }
    // T29: the supplied object's on/off for the tick being placed = its disable counter before the group's step (a Toggle fired in step c changes it for step c + 1's collisions)
    bool onAt(int uid, bool& on) const {
        const int sl = slotOf(uid);
        SupVal v;
        if (sl < 0 || !supVal((size_t)sl, v)) return false;
        on = v.pon;
        return true;
    }
    // what was supplied against what the model had placed (IDENTICAL = bit equal), and both against the game's own rows when --trigshadowgd names them
    struct SupStat { long long n = 0, posF = 0, posExact = 0, dExact = 0, gdN = 0, oldT = 0, shT = 0, oldP = 0, shP = 0, shPOnly = 0; double maxPos = 0, maxD = 0; };
    std::map<int, SupStat> supStat;
    long long supN = 0, supExact = 0, supDExact = 0, supGroupsDone = 0, supF = 0;
    int checkEvery = 0;   // 0 = off
    struct UidStat { long long n = 0, atT = 0, atPrev = 0, none = 0; double maxD = 0, sumD = 0; };
    std::map<int, UidStat> uidStat;
    // --trigshadowuid U[,U...] (default off, observation only; with --trigshadowcheck): the compared uids named here are reported one by one at the end whatever their rank -- the printed worst-uid list shows only
    // the 14 worst. For each: in how many compared layers the model placed it as a mover (`snaps`), how many times the shadow did not know it (`notIn`) or did not cover / move it (`uncov`), the layers it was
    // compared in (UidStat above), the first layer where the model matched neither of the shadow's ticks, and, with --trigshadowgd, how often the shadow was off the game's own row.
    struct WatchStat { long long snaps = 0, notIn = 0, uncov = 0, firstNoneT = -1, firstNoneModel = 0; double firstNoneD = 0; };
    std::map<int, WatchStat> watch;
    std::set<int> coverGroups;   // --trigshadowcover G[,G...] (default off, observation only; with --trigshadowlive): at the end of the call, how many members of each group the shadow covers and moves
    long long chkLayers = 0, chkGroups = 0, chkObjs = 0, chkAtT = 0, chkAtPrev = 0, chkNone = 0, chkUncovered = 0, chkNotInShadow = 0, chkIdle = 0;
    long long chkHist[5] = {0, 0, 0, 0, 0};   // |shadow(t) - model| of the compared movers: <= 0.01, <= 0.5, <= 2, <= 20, more
    double chkMs = 0, snapMs = 0, modelSnapMs = 0;   // modelSnapMs: the caller's walk of the model's placed movers (search_dp.hpp)
    bool snapping() const { return !pend.empty() && pendCur().snap; }
    void snapObj(int uid, double x, double y) { if (snapping()) pendCur().objs.push_back({uid, x, y}); }
    // ---- the world table, observed (restructure draft section 2 / P3; --trigshadowlive, observation only) ----
    // A state's own arena node is its `parent` field, and arena[node].parent() is the node of the state it was stepped from. members() records, for every state a group steps, which group (key) it was stepped in; the next
    // layer's groups then read their EXACT parent groups off the arena instead of the approximation above (same key, else nearest x). It also counts the worlds: the distinct group keys a layer holds, and the
    // distinct (key, history of the dx the state's ancestors were stepped with) -- the camera summary of the draft's choice (a) -- and keeps a table of every key seen with its first / last layer and its parent keys.
    struct WEntry { uint64_t key = 0, hist = 0, lhist = 0; float dx = 0.f; };   // lhist: the dx history restricted to the ticks a player-lock Move was running in the parent's timeline
    std::unordered_map<uint32_t, WEntry> wPrev, wCur;
    long long wLayerT = -2;
    size_t wLastArena = 0;
    long long wGcResets = 0;
    struct WInfo { long long first = 0, last = 0, layers = 0, lastLayerSeen = -1; std::unordered_set<uint64_t> parentKeys; };
    std::unordered_map<uint64_t, WInfo> wTable;
    std::unordered_set<uint64_t> lyKeys, lyKeysC, lyKeysL;
    long long wSumKeysL = 0, wMaxKeysL = 0;
    long long wGroups = 0, wUnknown = 0, wSingle = 0, wMulti = 0, wApproxHit = 0, wApproxMiss = 0, wEdges = 0, wLayers2 = 0;
    long long wSumKeys = 0, wSumKeysC = 0, wMaxKeysC = 0, wMaxKeys = 0;
    long long wStates = 0, wHistSplit = 0;   // states measured; groups whose states carry more than one dx history
    static uint64_t wmix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; }
    void wFlushLayer() {
        if (lyKeys.empty()) return;
        ++wLayers2; wSumKeys += (long long)lyKeys.size(); wSumKeysC += (long long)lyKeysC.size(); wSumKeysL += (long long)lyKeysL.size(); wMaxKeysL = std::max(wMaxKeysL, (long long)lyKeysL.size());
        wMaxKeys = std::max(wMaxKeys, (long long)lyKeys.size()); wMaxKeysC = std::max(wMaxKeysC, (long long)lyKeysC.size());
        lyKeys.clear(); lyKeysC.clear(); lyKeysL.clear();
    }
    template <class Arena, class States>
    void members(const Arena& arena, const States& cur, const std::vector<size_t>& gidx, float gdx) {
        if (!on || pend.empty()) return;
        if (arena.size() < wLastArena) { wPrev.clear(); wCur.clear(); ++wGcResets; }   // the arena was compacted: the node numbers changed
        wLastArena = arena.size();
        if (lastT != wLayerT) { wFlushLayer(); wPrev.swap(wCur); wCur.clear(); wLayerT = lastT; }
        const Pending& pd = pendCur();
        std::unordered_set<uint64_t> parentSet, hists, lhists;
        const bool lockAct = pd.parent && pd.parent->sh && pd.parent->sh->lockActive();
        for (size_t i : gidx) {
            const uint32_t node = cur[i].parent;
            WEntry mine; mine.key = pd.key; mine.dx = gdx;
            uint64_t ph = 0, lh = 0;
            if (node < arena.size()) {
                const auto it = wPrev.find(arena[node].parent());
                if (it != wPrev.end()) { parentSet.insert(it->second.key); const uint64_t dxb = (uint64_t)*reinterpret_cast<const uint32_t*>(&it->second.dx); ph = wmix(it->second.hist, dxb); lh = lockAct ? wmix(it->second.lhist, dxb) : it->second.lhist; }
            }
            mine.hist = ph; mine.lhist = lh;
            hists.insert(ph); lhists.insert(lh);
            wCur[node] = mine;
            ++wStates;
        }
        ++wGroups;
        if (hists.size() > 1) ++wHistSplit;
        if (parentSet.empty()) ++wUnknown;
        else {
            if (parentSet.size() == 1) ++wSingle; else ++wMulti;
            if (pd.hasParentKey) { if (parentSet.count(pd.parentKey)) ++wApproxHit; else ++wApproxMiss; }
        }
        lyKeys.insert(pd.key);
        for (uint64_t h : hists) lyKeysC.insert(wmix(pd.key, h));
        for (uint64_t h : lhists) lyKeysL.insert(wmix(pd.key, h));
        WInfo& w = wTable[pd.key];
        if (w.layers == 0) { w.first = lastT; }
        w.last = lastT;
        if (w.lastLayerSeen != lastT) { ++w.layers; w.lastLayerSeen = lastT; }
        for (uint64_t pk : parentSet) if (w.parentKeys.insert(pk).second) ++wEdges;
    }
    void wPrint() const {
        if (!wGroups) return;
        long long dead = 0, lifeSum = 0, lifeMax = 0, multiParent = 0;
        for (const auto& kv : wTable) {
            lifeSum += kv.second.last - kv.second.first + 1; lifeMax = std::max(lifeMax, kv.second.last - kv.second.first + 1);
            if (kv.second.last < lastT - 50) ++dead;
            if (kv.second.parentKeys.size() > 1) ++multiParent;
        }
        std::printf("trigshadowlive: world table: %zu worlds seen (alive at the end %zu, finished earlier %lld; life in layers mean %.1f max %lld), %lld distinct parent edges, %lld worlds with more than one parent; arena compactions %lld\n",
                    wTable.size(), wTable.size() - (size_t)dead, dead, wTable.empty() ? 0.0 : (double)lifeSum / (double)wTable.size(), lifeMax, wEdges, multiParent, wGcResets);
        std::printf("trigshadowlive: world table: %lld groups measured (%lld states): exact parent unknown %lld, one parent group %lld, states from two or more parent groups %lld; the approximate parent is among the exact ones in %lld of %lld (%.1f%%)\n",
                    wGroups, wStates, wUnknown, wSingle, wMulti, wApproxHit, wApproxHit + wApproxMiss, (wApproxHit + wApproxMiss) ? 100.0 * (double)wApproxHit / (double)(wApproxHit + wApproxMiss) : 0.0);
        std::printf("trigshadowlive: world table: a layer holds %.2f worlds (max %lld); with the dx history of the states' ancestors %.2f (max %lld, x%.2f); %lld groups hold states with different dx histories\n",
                    wLayers2 ? (double)wSumKeys / (double)wLayers2 : 0.0, wMaxKeys, wLayers2 ? (double)wSumKeysC / (double)wLayers2 : 0.0, wMaxKeysC, wSumKeys ? (double)wSumKeysC / (double)wSumKeys : 0.0, wHistSplit);
        std::printf("trigshadowlive: world table: with the dx history restricted to the ticks a player-lock Move is running (the P7 key for lock groups): %.2f worlds a layer (max %lld, x%.2f of the plain worlds)\n",
                    wLayers2 ? (double)wSumKeysL / (double)wLayers2 : 0.0, wMaxKeysL, wSumKeys ? (double)wSumKeysL / (double)wSumKeys : 0.0);
    }
    // called by the caller right after group(): is this layer one of the compared ones
    void markSnap(long long t) { if (checkEvery > 0 && !pend.empty() && t % checkEvery == 0) pendCur().snap = true; }

    // --trigshadowgd DIR (default off, observation only; with --trigshadowcheck): the game's own truth for the compared movers. DIR holds dump.csv (tick, x: where the game's player was) and grouptrace_last.txt
    // (tick, uid, cx, cy: the game's moving objects, a row when one changed). A group whose x is within 0.5 of the game's x at that tick is on the game's path: its movers are held against the game's rows
    // as well, the model's placement and the shadow's. That is the comparison the model-against-shadow table cannot make (which of the two is right).
    std::string tableFile;   // --trigshadowtable FILE (observation only): after the load, one row per trigger the shadow holds (and per activator object the dump leaves out): what finish() decided
    void writeTable(const gdtrig::TrigShadow& S) {
        std::ofstream o(tableFile, std::ios::trunc);
        if (!o) { std::printf("trigshadowtable: cannot write %s\n", tableFile.c_str()); return; }
        o << "# unknownActivators=" << (S.unknownActivators ? 1 : 0) << " unlisted=" << S.unlistedActivators.size() << " touchFed=" << (S.touchWasFed() ? 1 : 0) << "\n";
        o << "uid,id,start,cx,modelled,fed,kindRuns,reason,blockers\n";
        for (const gdtrig::TrigDef& t : S.triggers()) {
            const bool mod = S.modelled(t.uid), fed = S.isFedSpawn(t.uid), kr = S.kindRunsNow(t);
            const std::set<int> bl = S.blockersOf(t.uid);
            std::string reason;
            if (mod && !fed) reason = "run";
            else if (mod && fed) reason = "fed-from-record";
            else if (t.touch && !S.touchWasFed()) reason = "touch-not-fed";
            else if (t.spawn && S.unknownActivators) reason = "unknownActivators";
            else if (t.spawn && !bl.empty()) reason = "spawn-blocked";
            else if (!kr) reason = "kind-not-run";
            else reason = "other";
            o << t.uid << ',' << t.id << ',' << (t.touch ? "touch" : (t.spawn ? "spawn" : "auto")) << ',' << t.cx << ',' << (mod ? 1 : 0) << ',' << (fed ? 1 : 0) << ',' << (kr ? 1 : 0) << ',' << reason << ',';
            bool first = true; for (int b : bl) { if (!first) o << '|'; o << b; first = false; }
            o << '\n';
        }
        for (const auto& u : S.unlistedActivators) o << u.first << ',' << u.second << ",unlisted,0,0,0,0,unlisted-activator,\n";
    }    std::string gdDir;
    std::map<long long, double> gdX;
    std::unordered_map<int, std::vector<std::array<double, 3>>> gdTr;   // uid -> (tick, cx, cy), ascending
    bool gdLoaded = false;
    long long gdN = 0, gdModelOk = 0, gdShadowOk = 0, gdBoth = 0, gdOff = 0;
    // TSGD_GROUP=G (env, diagnosis): the same comparison for the members of group G alone (objgroups.txt of the gd directory), per layer, so that the ticks the shadow (and the model) agree with the game on can be listed as ranges
    std::map<long long, std::array<long long, 3>> gdLay;   // layer -> (movers compared, shadow ok, model ok)
    std::unordered_set<int> gdGrpMembers; bool gdGrpRead = false; int gdGrpWanted = -1;
    void gdReadMembers() {
        gdGrpRead = true;
        const char* g = std::getenv("TSGD_GROUP"); if (!g) return;
        gdGrpWanted = std::atoi(g);
        std::ifstream f(gdDir + "/objgroups.txt"); std::string l; std::getline(f, l);
        while (std::getline(f, l)) {
            std::vector<long long> v; size_t p = 0;
            while (p < l.size()) { while (p < l.size() && !std::isdigit((unsigned char)l[p]) && l[p] != '-') ++p; if (p >= l.size()) break; char* e = nullptr; v.push_back(std::strtoll(l.c_str() + p, &e, 10)); p = (size_t)(e - l.c_str()); }
            if (v.size() >= 2) for (size_t i = 1; i < v.size(); ++i) if (v[i] == gdGrpWanted) { gdGrpMembers.insert((int)v[0]); break; }
        }
    }
    std::map<int, std::pair<long long, long long>> gdUidBad;   // uid -> (shadow wrong, compared)
    long long tyCat[5] = {0, 0, 0, 0, 0};                      // the row-type census: A, B, C, S, N (gdCompare)
    std::map<int, std::array<long long, 17>> tyUid;            // per uid: A B C S N, then for the N ones the band counts of d0 (|model - row t|: 0.01-0.05, 0.05-1, > 1) and of dm (the smaller of the distances to rows t and t+1), and the max d0 x 1000 and its sum x 1000 (n1 n2 n3 m1 m2 m3 max sum)
    long long tyBand[6] = {0, 0, 0, 0, 0, 0};
    size_t dbgI = 0, dbgN = 0; uint64_t dbgFsig = 0;   // the pending entry being compared (gdCompare's TSGD_RECUID line)
    long long tyCatC[2][5] = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}};   // [shadow covers and moves the mover][A B C S N]
    long long tyBandC[2][5] = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}}, tyShOk01[5] = {0, 0, 0, 0, 0}, tyShOk05[5] = {0, 0, 0, 0, 0};   // the movers off row t by class (B, C, N <= 0.05, 0.05-1, > 1), and for the covered ones how many have the shadow within 0.01 / 0.05 of row t
    void gdLoad() {
        if (gdLoaded || gdDir.empty()) return;
        gdLoaded = true;
        { std::ifstream f(gdDir + "/dump.csv"); std::string l; bool head = true; int ct = -1, cx = -1; std::vector<std::string> col;
          while (std::getline(f, l)) { std::stringstream ss(l); std::string c; std::vector<std::string> v; while (std::getline(ss, c, ',')) v.push_back(c);
            if (head) { head = false; for (size_t i = 0; i < v.size(); ++i) { if (v[i] == "tick") ct = (int)i; if (v[i] == "x") cx = (int)i; } continue; }
            if (ct >= 0 && cx >= 0 && (int)v.size() > std::max(ct, cx)) gdX[std::atoll(v[ct].c_str())] = std::atof(v[cx].c_str()); } }
        { std::ifstream f(gdDir + "/grouptrace_last.txt"); std::string l;
          while (std::getline(f, l)) { if (l.empty() || l[0] == 'i' || l[0] == 't') continue; std::stringstream ss(l); std::string c; std::vector<std::string> v; while (std::getline(ss, c, ',')) v.push_back(c);
            if (v.size() < 4) continue; gdTr[std::atoi(v[1].c_str())].push_back({(double)std::atoll(v[0].c_str()), std::atof(v[2].c_str()), std::atof(v[3].c_str())}); } }
    }
    // the game's own touch latch at a tick, from where its player was (dump.csv x, y; a 30x30 box against each touch box of the window): bit b is set from the first tick the boxes overlap. A group is held
    // against the game's rows only when its latch IS that one -- a group of the same x that has touched a box the game's player did not (or not yet) is another world.
    std::vector<long long> gdEntry;   // per bit: the first dump tick the game's player overlapped box b (1<<60: never)
    long long gdMaskSkip = 0, gdClampSkip = 0;   // gdClampSkip: rows whose game centre is below y = 0 (getObjectRect's clamp), left out
    void gdEntries() {
        if (!gdEntry.empty()) return;
        std::vector<std::array<double, 3>> path;   // tick, x, y
        { std::ifstream f(gdDir + "/dump.csv"); std::string l; bool head = true; int ct = -1, cx = -1, cy = -1;
          while (std::getline(f, l)) { std::stringstream ss(l); std::string c; std::vector<std::string> v; while (std::getline(ss, c, ',')) v.push_back(c);
            if (head) { head = false; for (size_t i = 0; i < v.size(); ++i) { if (v[i] == "tick") ct = (int)i; if (v[i] == "x") cx = (int)i; if (v[i] == "y") cy = (int)i; } continue; }
            if (ct >= 0 && cx >= 0 && cy >= 0 && (int)v.size() > std::max(ct, std::max(cx, cy))) path.push_back({(double)std::atoll(v[ct].c_str()), std::atof(v[cx].c_str()), std::atof(v[cy].c_str())}); } }
        std::map<size_t, long long> fileTick;   // TSGD_LATCHFILE: "bit tick" lines -- the ticks the MODEL's own replay of the game's inputs set each bit (supplyTouch, the shadow's pre-collision body), taken instead of the plain box rule
        if (const char* lf = std::getenv("TSGD_LATCHFILE")) { std::ifstream f(lf); long long bb, tt; while (f >> bb >> tt) fileTick[(size_t)bb] = tt; }
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            long long first = (long long)1 << 60;
            if (fileTick.count(b)) { gdEntry.push_back(fileTick[b]); continue; }
            for (const auto& r : path)
                if (std::fabs(r[1] - g_touch[b].cx) <= g_touch[b].hw + 15.0 && std::fabs(r[2] - g_touch[b].cy) <= g_touch[b].hh + 15.0) { first = (long long)r[0]; break; }
            gdEntry.push_back(first);
        }
        if (gdEntry.empty()) gdEntry.push_back((long long)1 << 60);
    }
    uint64_t gdMaskAt(long long t) {
        gdEntries();
        uint64_t m = 0;
        for (size_t b = 0; b < gdEntry.size() && b < 64; ++b) if (gdEntry[b] <= t) m |= 1ull << b;
        return m;
    }
    // what the groups on the game's path are, counted once per group (not per mover): of the groups whose x is the game's, how many have the game's touch latch and entry ticks, and where the others differ --
    // `extra` (a box the game's player had not touched yet: the search's own other worlds), `missing` (a box the game's player HAD overlapped that the group's latch lacks: the model's latch is later than the game's
    // overlap), both, or the same bits entered on other ticks.
    long long gdGrpPath = 0, gdGrpSame = 0, gdGrpExtraOnly = 0, gdGrpMissingOnly = 0, gdGrpBoth = 0, gdGrpTick = 0;
    void gdGroup(const Pending& p, double x) {
        if (gdDir.empty()) return;
        gdLoad();
        static const long long shift = [] { const char* e = std::getenv("TSGD_SHIFT"); return e ? std::atoll(e) : 0LL; }();
        const auto xi = gdX.find(lastT + shift);
        if (xi == gdX.end() || std::fabs(xi->second - x) > 0.5) return;
        ++gdGrpPath;
        const uint64_t gm = gdMaskAt(lastT);
        const uint64_t extra = p.bits & ~gm, missing = gm & ~p.bits;
        if (extra && missing) { ++gdGrpBoth; return; }
        if (extra) { ++gdGrpExtraOnly; return; }
        if (missing) { ++gdGrpMissingOnly; return; }
        for (size_t b = 0; b < gdEntry.size() && b < (size_t)kTouchBits; ++b)
            if (((p.bits >> b) & 1ull) && (long long)p.fb[b] != gdEntry[b]) { ++gdGrpTick; return; }
        ++gdGrpSame;
    }
    void gdCompare(double groupX, const SnapObj& o, const gdtrig::ObjState* Sp, uint64_t bits, const uint16_t* fb) {   // Sp = nullptr: the shadow does not cover / move this mover (the census still counts it, with no shadow value)
        if (gdDir.empty()) return;
        gdLoad();
        static const bool noSkip = std::getenv("TSGD_NOSKIP") != nullptr;   // diagnosis: the mask / entry-tick condition off (every group on the game's path is compared, whatever its touch history)
        if (!noSkip && bits != gdMaskAt(lastT)) { ++gdMaskSkip; return; }
        // ...and the boxes were entered on the ticks the game entered them (a group of the game's mask that got there earlier or later has a chain started at another step)
        static const long long fbTol = [] { const char* e = std::getenv("TSGD_FBTOL"); return e ? std::atoll(e) : 0LL; }();   // calibration: the latch may be up to this many ticks EARLIER than the box rule on the game's path (the pass tests the body before the solids push it back)
        for (size_t b = 0; b < gdEntry.size() && b < (size_t)kTouchBits; ++b)
            if (!noSkip && ((bits >> b) & 1ull) && ((long long)fb[b] > gdEntry[b] || (long long)fb[b] < gdEntry[b] - fbTol)) { ++gdMaskSkip; return; }
        static const long long shift = [] { const char* e = std::getenv("TSGD_SHIFT"); return e ? std::atoll(e) : 0LL; }();   // calibration: the game's row for layer t is row t + shift
        const auto xi = gdX.find(lastT + shift);
        { static int dbg = 0; if (dbg < 5 && std::getenv("TSGD_DBG")) { ++dbg; std::printf("trigshadowgd: debug layer %lld groupX %.4f gd x %s %.4f (rows %zu, traces %zu)\n", lastT, groupX, xi == gdX.end() ? "none" : "row", xi == gdX.end() ? 0.0 : xi->second, gdX.size(), gdTr.size()); } }
        if (xi == gdX.end() || std::fabs(xi->second - groupX) > 0.5) return;
        const auto bi = base->byUid.find(o.uid);
        if (bi == base->byUid.end()) return;
        double gx = init[bi->second].x, gy = init[bi->second].y;
        const auto it = gdTr.find(o.uid);
        if (it != gdTr.end()) {
            const auto& v = it->second;
            size_t lo = 0, hi = v.size();
            while (lo < hi) { const size_t m = (lo + hi) / 2; if (v[m][0] <= (double)lastT) lo = m + 1; else hi = m; }
            if (lo > 0) { gx = v[lo - 1][1]; gy = v[lo - 1][2]; }
        }
        // grouptrace's cx, cy are the centre of getObjectRect, and GD pulls the top edge of a rect that lies below y = 0 up to 0 (gd-03, 11-06: all 1,827 rows with a difference had cy + h/2 = 0): a row whose centre
        // is below 0 is not the object's place, so it is left out of the comparison (and of the row-type census below) and counted (TSGD_KEEPNEG=1 keeps them, the old reading). The wtrace is the place itself.
        static const bool keepNeg = std::getenv("TSGD_KEEPNEG") != nullptr;
        if (!keepNeg && gy < 0.0) { ++gdClampSkip; return; }
        {   // the row-type census (observation only): the model's placed position against the game's rows of tick t and tick t+1. A = (x, y) of row t, B = x of row t and y of row t+1 (a model that moves y one tick early),
            // C = both of row t+1, S = y does not change between the two rows (A and B cannot be told apart), N = none of them
            double gx1 = gx, gy1 = gy;
            if (it != gdTr.end()) { const auto& v = it->second; size_t lo = 0, hi = v.size(); while (lo < hi) { const size_t m = (lo + hi) / 2; if (v[m][0] <= (double)(lastT + 1)) lo = m + 1; else hi = m; } if (lo > 0) { gx1 = v[lo - 1][1]; gy1 = v[lo - 1][2]; } }
            const auto eq = [](double a, double b) { return std::fabs(a - b) <= 0.01; };
            const bool xT = eq(o.x, gx), yT = eq(o.y, gy), xT1 = eq(o.x, gx1), yT1 = eq(o.y, gy1);
            const bool moves = !eq(gy, gy1) || !eq(gx, gx1);
            int cat = 4;   // N
            if (xT && yT) cat = moves ? 0 : 3;   // A (or S when nothing moves between the rows)
            else if (xT && yT1) cat = 1;         // B
            else if (xT1 && yT1) cat = 2;        // C
            ++tyCat[cat];
            auto& tu = tyUid[o.uid]; ++tu[cat];
            tu[13] = Sp ? 1 : 0;
            const int ci = Sp ? 1 : 0;
            ++tyCatC[ci][cat];
            if (const char* ra = std::getenv("TSGD_RECALL")) {   // TSGD_RECALL=uid:fromLayer:count -- every layer of that uid from fromLayer, whatever its class (diagnosis)
                int ru = 0, rf = 0, rc = 0; std::sscanf(ra, "%d:%d:%d", &ru, &rf, &rc);
                static int shownAll = 0;
                if (o.uid == ru && lastT >= rf && shownAll < rc) { ++shownAll; std::printf("trigshadowall: [entry %zu of %zu, bits %llx] layer %lld cat %d model (%.4f, %.4f) row t (%.4f, %.4f) shadow %s (%.4f, %.4f) group x %.4f\n", dbgI, dbgN, (unsigned long long)bits, lastT, cat, o.x, o.y, gx, gy, Sp ? "covered" : "none", Sp ? Sp->x : 0.0, Sp ? Sp->y : 0.0, groupX); }
            }
            if (cat == 1 || cat == 2 || cat == 4) {   // the band class of a mover that does not match row t: 0 = B, 1 = C, 2 = N <= 0.05, 3 = N 0.05-1, 4 = N > 1
                const double d0c = std::max(std::fabs(o.x - gx), std::fabs(o.y - gy));
                const int bc = cat == 1 ? 0 : cat == 2 ? 1 : d0c <= 0.05 ? 2 : d0c <= 1.0 ? 3 : 4;
                ++tyBandC[ci][bc];
                if (Sp) { const double sd = std::max(std::fabs(Sp->x - gx), std::fabs(Sp->y - gy)); if (sd <= 0.01) ++tyShOk01[bc]; if (sd <= 0.05) ++tyShOk05[bc]; }
                if (const char* ru = std::getenv("TSGD_RECUID")) {
                    static int shown = 0;
                    static const int recN = [] { const char* e = std::getenv("TSGD_RECN"); return e ? std::atoi(e) : 14; }();
                    if (o.uid == std::atoi(ru) && shown < recN && (cat == 4 ? d0c > 0.05 : true)) {
                        ++shown;
                        double px = 0, py = 0, qx = 0, qy = 0; long long pt = -1, qt = -1;
                        if (it != gdTr.end()) { const auto& v = it->second; size_t lo = 0, hi = v.size(); while (lo < hi) { const size_t m = (lo + hi) / 2; if (v[m][0] <= (double)lastT) lo = m + 1; else hi = m; }
                            if (lo > 0) { pt = (long long)v[lo - 1][0]; px = v[lo - 1][1]; py = v[lo - 1][2]; } if (lo < v.size()) { qt = (long long)v[lo][0]; qx = v[lo][1]; qy = v[lo][2]; } }
                        std::printf("trigshadowrec: [entry %zu of %zu, bits %llx] uid %d layer %lld model (%.3f, %.3f) | row before: tick %lld (%.3f, %.3f) after: tick %lld (%.3f, %.3f) | row t (%.3f, %.3f) row t+1 (%.3f, %.3f) | shadow %s (%.3f, %.3f) | d0 %.3f | group x %.3f game player x %.3f\n", dbgI, dbgN, (unsigned long long)bits, o.uid, lastT, o.x, o.y, pt, px, py, qt, qx, qy, gx, gy, gx1, gy1, Sp ? "covered" : "none", Sp ? Sp->x : 0.0, Sp ? Sp->y : 0.0, d0c, groupX, xi->second);
                    }
                }
            }
            if (cat == 4) {
                const double d0 = std::max(std::fabs(o.x - gx), std::fabs(o.y - gy)), d1 = std::max(std::fabs(o.x - gx1), std::fabs(o.y - gy1)), dm = std::min(d0, d1);
                const int b0 = d0 <= 0.05 ? 0 : d0 <= 1.0 ? 1 : 2, bm = dm <= 0.05 ? 0 : dm <= 1.0 ? 1 : 2;
                ++tu[5 + b0]; ++tu[8 + bm]; ++tyBand[b0]; ++tyBand[3 + bm];
                tu[11] = std::max(tu[11], (long long)(d0 * 1000.0)); tu[12] += (long long)(d0 * 1000.0);
            }
        }
        if (!Sp) return;
        const gdtrig::ObjState& S = *Sp;
        { const double sdr = std::max(std::fabs(S.x - gx), std::fabs(S.y - gy));
          if (const char* rf = std::getenv("TSGD_RECFAR")) {   // TSGD_RECFAR=uid[:count]: the first layers of that uid where the shadow is more than 0.05 from row t (diagnosis)
              int fu = 0, fc = 40; std::sscanf(rf, "%d:%d", &fu, &fc);
              static int shownFar = 0;
              if (o.uid == fu && sdr > 0.05 && shownFar < fc) { ++shownFar; std::printf("trigshadowfar: [entry %zu of %zu, bits %llx] uid %d layer %lld model (%.4f, %.4f) row t (%.4f, %.4f) shadow (%.4f, %.4f) group x %.4f\n", dbgI, dbgN, (unsigned long long)bits, o.uid, lastT, o.x, o.y, gx, gy, S.x, S.y, groupX); }
          } auto& tu2 = tyUid[o.uid]; if (sdr > 0.05) { ++tu2[14]; tu2[15] = std::max(tu2[15], (long long)(sdr * 1000.0)); if (std::max(std::fabs(o.x - gx), std::fabs(o.y - gy)) > 0.05) ++tu2[16]; } }   // [16]: the model is off row t by more than 0.05 as well (both far)   // the shadow itself more than 0.05 from row t (the csv: shadow_far, shadow_far_max_milli)
        const bool mOk = std::max(std::fabs(o.x - gx), std::fabs(o.y - gy)) <= 0.01;
        const bool sOk = std::max(std::fabs(S.x - gx), std::fabs(S.y - gy)) <= 0.01;
        ++gdN; if (mOk) ++gdModelOk; if (sOk) ++gdShadowOk; if (mOk && sOk) ++gdBoth;
        if (!gdGrpRead) gdReadMembers();
        if (gdGrpWanted >= 0 && gdGrpMembers.count(o.uid)) { auto& L = gdLay[lastT]; ++L[0]; if (sOk) ++L[1]; if (mOk) ++L[2]; }
        auto& u = gdUidBad[o.uid]; ++u.second; if (!sOk) { ++u.first; ++gdOff; }
        static const char* const envAllUid = std::getenv("TSGD_ALLUID");   // read once: getenv takes the CRT's environment lock (shadow_step.hpp shDbgUid)
        if (const char* da = envAllUid) { static long long shownA = 0; static const long long lo = [] { const char* e = std::getenv("TSGD_ALLLO"); return e ? std::atoll(e) : 0LL; }(), hi = [] { const char* e = std::getenv("TSGD_ALLHI"); return e ? std::atoll(e) : (1LL << 60); }(); if (o.uid == std::atoi(da) && lastT >= lo && lastT <= hi && shownA < 4000) { ++shownA; std::printf("tsgdall: uid %d layer %lld shadow (%.3f, %.3f) game (%.3f, %.3f)\n", o.uid, lastT, S.x, S.y, gx, gy); } }
        static const char* const envGdUid = std::getenv("TSGD_UID");   // read once
        if (const char* du = envGdUid) { static int shown = 0; if (o.uid == std::atoi(du) && !sOk && shown < 12) { ++shown; std::printf("trigshadowgd: uid %d layer %lld group bits %llx game bits %llx: shadow (%.3f, %.3f) model (%.3f, %.3f) game (%.3f, %.3f)\n", o.uid, lastT, (unsigned long long)bits, (unsigned long long)gdMaskAt(lastT), S.x, S.y, o.x, o.y, gx, gy); } }
    }

    // --trigsupply-group + --trigshadowgd: a supplied mover against the game's own rows, at the layer's tick t and the one before. The same "is this group on the game's path" test as gdCompare.
    long long supSkipNoDir = 0, supSkipBits = 0, supSkipFb = 0, supSkipX = 0;
    long long fbTol = [] { const char* e = std::getenv("TSGD_FBTOL"); return e ? std::atoll(e) : 0LL; }();   // the box-entry ticks may differ by this much (calibration)   // why a supplied mover was not held against the game's rows
    bool gdPathOk(double groupX, uint64_t bits, const uint16_t* fb) {
        if (gdDir.empty()) { ++supSkipNoDir; return false; }
        gdLoad();
        if (bits != gdMaskAt(lastT)) { ++supSkipBits; return false; }
        for (size_t b = 0; b < gdEntry.size() && b < (size_t)kTouchBits; ++b)
            if (((bits >> b) & 1ull) && std::llabs((long long)fb[b] - gdEntry[b]) > fbTol) { ++supSkipFb; static const bool fbDbg = std::getenv("TSGD_FBDBG") != nullptr; if (fbDbg && supSkipFb <= 12) std::printf("trigsupply: layer %lld box bit %zu: the group's entry tick %d, the game's %lld\n", lastT, b, (int)fb[b], gdEntry[b]); return false; }
        static const long long shift = [] { const char* e = std::getenv("TSGD_SHIFT"); return e ? std::atoll(e) : 0LL; }();
        const auto xi = gdX.find(lastT + shift);
        if (xi == gdX.end() || std::fabs(xi->second - groupX) > 0.5) {
            ++supSkipX;
            static const bool xDbg = std::getenv("TSGD_XDBG") != nullptr;   // read once
            if (xDbg && supSkipX <= 8) std::printf("trigsupply: layer %lld group x %.4f, the game's x at row %lld: %s %.4f\n", lastT, groupX, lastT + shift, xi == gdX.end() ? "none" : "row", xi == gdX.end() ? 0.0 : xi->second);
            return false;
        }
        return true;
    }
    bool gdPlace(int uid, long long tick, double& gx, double& gy) {
        const auto bi = base->byUid.find(uid);
        if (bi == base->byUid.end()) return false;
        gx = init[bi->second].x; gy = init[bi->second].y;
        const auto it = gdTr.find(uid);
        if (it != gdTr.end()) {
            const auto& v = it->second;
            size_t lo = 0, hi = v.size();
            while (lo < hi) { const size_t m = (lo + hi) / 2; if (v[m][0] <= (double)tick) lo = m + 1; else hi = m; }
            if (lo > 0) { gx = v[lo - 1][1]; gy = v[lo - 1][2]; }
        }
        return true;
    }
    // one supplied mover. `o*` is what the model had placed (in the group's frame), `n*` what the shadow supplies (same frame); `l*` are level coordinates for the game's rows: old, shadow at t, shadow at t-1.
    void supplyNote(int uid, double ocx, double ocy, double odcx, double odcy, double ncx, double ncy, double ndcx, double ndcy,
                    double olx, double oly, double slx, double sly) {
        SupStat& s = supStat[uid];
        ++s.n; ++supN;
        static const char* const tsupUid = std::getenv("TSUP_UID");   // read once
        if (const char* du = tsupUid) {   // diagnosis: the group's x and the place of one mover, supplied against placed, every layer it is supplied in (first 400 lines)
            static int shown = 0;
            if (uid == std::atoi(du) && shown < 400 && !pend.empty()) { ++shown; std::printf("tsup: layer %lld frame %d rev %d parent %s groupX %.4f shadow(%.4f, %.4f) model(%.4f, %.4f) supplied dc(%.4f, %.4f) model dc(%.4f, %.4f)\n", lastT, dbgFrame, dbgRev, pend.back().parent ? "yes" : "NONE", pend.back().x, slx, sly, olx, oly, ndcx, ndcy, odcx, odcy); }
        }
        if (ocx == ncx && ocy == ncy) { ++s.posExact; ++supExact; }
        if ((float)ocx == (float)ncx && (float)ocy == (float)ncy) { ++s.posF; ++supF; }   // equal at the float the game keeps
        if (odcx == ndcx && odcy == ndcy) { ++s.dExact; ++supDExact; }
        s.maxPos = std::max(s.maxPos, std::max(std::fabs(ocx - ncx), std::fabs(ocy - ncy)));
        s.maxD = std::max(s.maxD, std::max(std::fabs(odcx - ndcx), std::fabs(odcy - ndcy)));
        if (!pend.empty() && gdPathOk(pendCur().x, pendCur().bits, pendCur().fb)) {
            double gtx, gty, gpx, gpy;
            if (gdPlace(uid, lastT, gtx, gty) && gdPlace(uid, lastT - 1, gpx, gpy)) {
                auto near = [](double ax, double ay, double bx, double by) { return std::max(std::fabs(ax - bx), std::fabs(ay - by)) <= 0.01; };
                ++s.gdN;
                if (near(olx, oly, gtx, gty)) ++s.oldT;
                if (near(slx, sly, gtx, gty)) ++s.shT;
                if (near(olx, oly, gpx, gpy)) ++s.oldP;
                if (near(slx, sly, gpx, gpy)) { ++s.shP; if (!near(slx, sly, gtx, gty)) ++s.shPOnly; }
            }
        }
    }
    // The report's long tables -- one line per supplied uid (supplyPrint) and one per 2000-tick bucket of the worlds (print) -- are printed by the LAST report only (the destructor's), or by every
    // report with --trigshadowreportfull. The report repeats every 2000 layers of the process, and inside the game a printed line costs about 90 us (its stdout is the pipe the loader logs from;
    // 1 us to a file): on the 22 official levels these two tables were 59-91% of everything the dp printed (lv20: 141,000 lines a cold, about 13 s). A repeated report gives one line in their place.
    bool reportFull = false;
    void supplyPrint(bool full) const {
        std::printf("trigsupply: parents of new keys: %lld picked among the timelines told only this group's own touches (entered at the same ticks), %lld fell back to the nearest in x (the old pick)\n", parentOwnHistory, parentFallback);
        std::printf("trigsupply: parents of new keys: %lld took the timeline their first member was stepped in (before the pick above)\n", parentByWorld);
        if (savedLayers || restoredLayers) std::printf("trigsupply: --ladderprefix: %lld layers' timelines kept by a save, %lld given back to a resumed attempt\n", savedLayers, restoredLayers);
        std::printf("trigsupply: x-crossing split: %lld timelines stepped for a group with crossing counts; %lld had passed fewer running triggers than the group has crossed, %lld more\n", crossSteps, crossShort, crossOver);
        std::printf("trigsupply: vertical-channel crossings that waited because the step had no world y (frame 0, a group of many states): %lld\n", (long long)gdtrig::g_vertWaitsY.load());
        if (take) std::printf("trigsupply: take-over: %lld layers took their parents over, %lld layers copied (a key the previous layer lacks)\n", takeLayers, takeCopyLayers);
        std::printf("trigsupply: movers collected at the shadow's place (the model's own place was outside the window): %lld\n", (long long)g_supHitAdded.load());
        if (supIx_.calls) std::printf("trigsupply: the windows' movers by x: %lld calls read %lld of the timeline's pages again (%.2f a call; %zu pages hold a supplied object), %lld arrays; %lld pages opened for their range (%.2f a call); %lld calls checked against the scan\n", supIx_.calls, supIx_.pagesRead, (double)supIx_.pagesRead / (double)supIx_.calls, supIx_.page.size(), supIx_.arrays, supIx_.pagesOpened, (double)supIx_.pagesOpened / (double)supIx_.calls, supIx_.checked);
        if (supCamBlind && !supN) std::printf("trigsupply: no mover-tick supplied; %lld group steps left on the model's placement: a Move locked to the camera stepped with a camera the model was not certain of\n", supCamBlind);
        if (!supN) return;
        std::printf("trigsupply: %lld supplied mover-ticks in %lld group steps: position bit-equal to the model's placement for %lld (%.3f%%), equal as float for %lld (%.3f%%), dcx / dcy bit-equal for %lld (%.3f%%)\n",
                    supN, supGroupsDone, supExact, 100.0 * (double)supExact / (double)supN, supF, 100.0 * (double)supF / (double)supN, supDExact, 100.0 * (double)supDExact / (double)supN);
        if (supCamBlind) std::printf("trigsupply: %lld group steps left on the model's placement: a Move locked to the camera stepped with a camera the model was not certain of\n", supCamBlind);
        if (supGround) std::printf("trigsupply: %lld mover-ticks left on the model's placement: GD's rect of the mover lies wholly below y = 0 (its top is raised to 0, so the rect's centre is not the position)\n", supGround);
        std::printf("trigsupply: not held against the game's rows: no --trigshadowgd %lld, touch latch is not the game's %lld, box entry ticks differ %lld, the group's x is not the game's %lld\n", supSkipNoDir, supSkipBits, supSkipFb, supSkipX);
        if (!full) {   // a repeated report: the uids in one line (how many, how many of them differ from the model's placement somewhere, and the largest differences with their uids)
            long long differ = 0; double mp = 0.0, md = 0.0; int mpU = -1, mdU = -1;
            for (const auto& kv : supStat) {
                const SupStat& s = kv.second;
                if (s.posExact != s.n || s.dExact != s.n) ++differ;
                if (s.maxPos > mp) { mp = s.maxPos; mpU = kv.first; }
                if (s.maxD > md) { md = s.maxD; mdU = kv.first; }
            }
            std::printf("trigsupply:   %zu uids, %lld of them not bit-equal to the model's placement on every tick; max |pos diff| %.6f (uid %d), max |dc diff| %.6f (uid %d); one line a uid in the last report or with --trigshadowreportfull\n",
                        supStat.size(), differ, mp, mpU, md, mdU);
            return;
        }
        for (const auto& kv : supStat) {
            const SupStat& s = kv.second;
            std::printf("trigsupply:   uid %d: %lld ticks, position equal %lld (as float %lld), dc equal %lld, max |pos diff| %.6f, max |dc diff| %.6f", kv.first, s.n, s.posExact, s.posF, s.dExact, s.maxPos, s.maxD);
            if (s.gdN) std::printf(" | vs the game's rows on its path (%lld): model = row t %lld, row t-1 %lld; shadow = row t %lld, row t-1 %lld (only t-1: %lld)", s.gdN, s.oldT, s.oldP, s.shT, s.shP, s.shPOnly);
            std::printf("\n");
        }
    }
    // --trigshadowevents: the touch calls a timeline is owed -- the bits of the group's latch that its parent was not told -- as Events {kEvTouch, fireB + evShift, uid} (trigevent_touch.hpp).
    // The timeline applies one at the head of the step of that number; an Event whose step is already behind it is lost, which is why a parentless timeline is replayed (replayInto).
    // The key of a call is the model's K.t of the pass that made it (fireB) = the shadow's step (trigfire's game tick + 1, trigshadow.hpp setTouchEvents): fireTouch(t) fires it inside step t. A group of layer L
    // holds states the tick L-1 produced, and its timeline has already run step L-1 (the parent's): the call is made at once, after that step (fireTouchLate); `inOrder` (the replay of a parentless timeline, not
    // yet stepped) files it under its step instead, for the replay's own step to take.
    // The game events (State::evgate): the chain of (step, events) behind the group's digest, from the one its parent timeline was told (Line::evtold) to the group's own, oldest first. A timeline with no parent (the replay of
    // the first layer) is told them by step (they follow the anchor, whose digest is 0); a timeline that has run the step of the pass that raised them is told at once, after it (raiseEvent: the spawn the game made inside that step).
    // A told digest that is not on the group's chain (the parent's world raised events this group's did not) cannot be undone: it is counted (evNotAncestor) and the group's events are given all the same.
    long long evFedNodes = 0, evNotAncestor = 0, evListenerCalls = 0;
    void feedEvents(gdtrig::TrigShadow& S, const Pending& p, uint64_t toldDig, bool inOrder) {
        if (p.evg == 0 || p.evg == toldDig || !(S.eventsFed() || S.groupTogglesFed())) return;
        std::vector<gdev::EvNode> chain;
        bool reached = toldDig == 0;
        {
            std::lock_guard<std::mutex> lk(gdev::g_evChainMu);
            for (uint64_t d = p.evg; d != 0 && chain.size() < 4096; ) {
                if (d == toldDig) { reached = true; break; }
                const auto it = gdev::g_evChain.find(d);
                if (it == gdev::g_evChain.end()) break;
                chain.push_back(it->second);
                d = it->second.prev;
            }
        }
        if (!reached) ++evNotAncestor;
        long long lastStep = std::numeric_limits<long long>::min();
        const long long raisedBefore = S.eventsRaised;
        struct Tally { long long& n; const gdtrig::TrigShadow& z; long long b; ~Tally() { n += z.eventsRaised - b; } } tally{evListenerCalls, S, raisedBefore};
        for (size_t i = chain.size(); i-- > 0;) {
            const gdev::EvNode& n = chain[i];
            if (!inOrder && n.step != lastStep) { S.endEventStep(); lastStep = n.step; }
            for (const int e : n.events) {
                if (gdev::isTapTok(e)) {   // a Tap's press / release (game_event.hpp kTapTok): the game's toggleGroupTriggered on the Tap's group (trigtouch.hpp TapDecider's rule: a press applies the Tap's toggle mode, a release the opposite)
                    const size_t ti = (size_t)((e - gdev::kTapTok) >> 2); const int kind = (e - gdev::kTapTok) & 3;
                    if (ti >= gdtrig::g_togTable.taps.size() || !S.groupTogglesFed()) continue;
                    const gdtrig::TogTap& tp = gdtrig::g_togTable.taps[ti];
                    const bool on = kind == 1 ? tp.ttog == 1 : tp.ttog != 1;
                    if (inOrder) S.addGroupToggleEvent(n.step + 1, tp.target, on, tp.uid); else S.toggleGroupLate(tp.target, on, tp.uid);   // (the head of the step after the pass that saw the edge, as toggleGroupLate)
                    ++evFedNodes;
                    continue;
                }
                if (inOrder) S.addGameEvent(n.step, e); else S.raiseEventLate(e);
                ++evFedNodes;
            }
        }
    }
    void feed(gdtrig::TrigShadow& S, const Pending& p, uint64_t told, bool inOrder = false) {
        feedTouch(S, p, told, inOrder);
        feedEvents(S, p, inOrder ? 0 : (p.parent ? p.parent->evtold : 0), inOrder);
    }
    void feedTouch(gdtrig::TrigShadow& S, const Pending& p, uint64_t told, bool inOrder) {
        if (!events) return;
        const uint64_t owed = p.bits & ~told;
        if (!owed) return;
        for (size_t b = 0; b < boxUid.size() && b < (size_t)kTouchBits; ++b) {
            if (!((owed >> b) & 1ull) || boxUid[b] < 0) continue;
            if (inOrder) S.addTouchEvent((long long)p.fb[b] + evShift, boxUid[b]);
            else S.fireTouchLate(boxUid[b]);
            ++evGiven;
            static const bool evDbg = std::getenv("TSEV_DBG") != nullptr;   // read once
            if (evDbg && evGiven <= 40) {   // diagnosis: the Event given, and the tick the game's player first overlapped that box (--trigshadowgd)
                long long ge = -1; if (!gdDir.empty()) { gdEntries(); if (b < gdEntry.size()) ge = gdEntry[b] > ((long long)1 << 59) ? -2 : gdEntry[b]; }
                std::printf("trigshadowev: layer %lld bit %zu uid %d fireB %d step %lld; game's first overlap tick %lld\n", lastT, b, boxUid[b], (int)p.fb[b], (long long)p.fb[b] + evShift, ge);
            }
        }
    }
    // --trigshadowreplay / --trigshadowevents, a timeline with no parent (the first layer of a search that starts at an anchor): stepped through the ticks before this layer along the run's x track
    // (g_xTrack: tick, x of --xtrack -- the same run's GD path, an input the anchor already has), with the anchor's owed touch Events given first. The cost is one step per tick of the track, once per
    // group of the first layer.
    // --trigshadowopts cam (a measurement: the game's own camera of the flight, camtrace.csv of the directory -- cfg areadump=1; the loop has no such record, so a camera MODEL is the end state): the camera the screen
    // Areas read at step t, set by the caller before each step exactly as tools/trig_replay.cpp does (the window 569 wide, 320 high, over the zoom)
    dp::gdcam::CamModel camModel; bool camModelOn = false;
    long long camCertain = 0, camUncertain = 0;   // layers (per timeline) whose model camera X was / was not the follow formula (a Static Camera that moves X or a left / right Camera Edge is active)
    // the model camera of a timeline at step t: cs advanced with the player x of the group; Y is not modelled (screen-type Areas read the camera X only: notes camera_model_1105_gd04.md), the window follows the zoom
    void camModelStep(gdtrig::TrigShadow& Z, dp::gdcam::CamState& cs, double x, int frame = 0, const dp::gdcam::CamLive* live = nullptr) {
        float zoom = 1.0f;
        double wx = x, wy = 0.0; stepCoords(frame, x, wx, wy);   // the crossing coordinates, as the shadow's own step takes them
        const float px = camModel.step(cs, x, &zoom, nullptr, wx, supplyWorld ? wy : std::nan(""), live);
        Z.cam.l = px; Z.cam.b = 0.0; Z.cam.w = (double)(569.0f / zoom); Z.cam.h = (double)(320.0f / zoom);
        Z.cam.ok = !cs.uncertain; Z.cam.okY = false;
        if (cs.uncertain) ++camUncertain; else ++camCertain;
        static const bool camWhy = std::getenv("TSCAM_WHY") != nullptr;   // read once
        if (cs.uncertain && camWhy) {   // diagnosis: why the model camera is not certain (the first 40 timeline-steps)
            static int shown = 0;
            if (shown++ < 40) std::printf("tscamwhy: step %lld x %.3f runMax %.3f static %d edge %d%d exit %d behind %d tp %d unknown %d (unknownMinX %.1f)\n", (long long)Z.stepNo_, x, cs.runMax, cs.staticX ? 1 : 0, cs.edgeX[1] ? 1 : 0, cs.edgeX[2] ? 1 : 0, cs.n <= cs.exitUntil ? 1 : 0, x < cs.runMax - 1.0 ? 1 : 0, cs.n <= cs.tpUntil ? 1 : 0, camModel.useUnknown && camModel.unknownMinX <= cs.runMax ? 1 : 0, camModel.unknownMinX);
        }
    }
    // The second half of the model camera's tick, after the shadow's own step: under a follow Static Camera whose target a trigger moves the game's camera reads the target as this tick left it
    // (CamModel::resolveLive). The camera handed before the step was uncertain; the one placed here is what the next step's camera-locked Moves read.
    long long camLiveSteps = 0, camLivePlaced = 0, camLiveYPlaced = 0;
    void camModelAfter(gdtrig::TrigShadow& Z, dp::gdcam::CamState& cs) {
        if (!cs.liveNeed && !cs.liveYNeed) return;
        ++camLiveSteps;
        dp::gdcam::CamLive lv;
        const int g = cs.liveNeed ? cs.xev.tgt : cs.yev.tgt;
        float tx = 0.0f, ty = 0.0f;
        if (!dp::gdcam::g_camNoLive && Z.firstMemberPos(g, &tx, &ty)) { lv.okPos = true; lv.pg = g; lv.px = tx; lv.py = ty; }
        float left = (float)Z.cam.l;
        const bool placed = camModel.resolveLive(cs, lv, &left);
        gdtrig::Cam c = Z.cam;
        if (placed) { c.l = (double)left; c.ok = true; ++camLivePlaced; --camUncertain; ++camCertain; }
        if (cs.cenYOk) { c.b = (double)cs.cenY - c.h * 0.5; c.okY = true; ++camLiveYPlaced; }
        if (placed || cs.cenYOk) Z.setStepCam(c);
    }
    struct CamT { double x, y, zoom; };
    std::map<long long, CamT> camTrace;
    void loadCam() {
        for (const std::string& d : {ext, dir}) {
            if (d.empty()) continue;
            std::ifstream f(d + "/camtrace.csv");
            if (!f) continue;
            std::string line; std::getline(f, line);
            long long att = -1;
            std::map<long long, CamT> cur;
            while (std::getline(f, line)) {
                std::vector<std::string> c; size_t p = 0;
                while (p <= line.size()) { size_t q = line.find(',', p); if (q == std::string::npos) q = line.size(); c.push_back(line.substr(p, q - p)); p = q + 1; }
                if (c.size() < 7) continue;
                const long long a = std::atoll(c[0].c_str());
                if (a != att) { att = a; cur.clear(); }
                cur[std::atoll(c[1].c_str())] = {std::atof(c[2].c_str()), std::atof(c[3].c_str()), std::atof(c[4].c_str())};
            }
            camTrace = cur;
            return;
        }
    }
    void setCamAt(gdtrig::TrigShadow& Z, long long t) const {
        if (camTrace.empty()) return;
        const auto it = camTrace.find(t);
        if (it == camTrace.end() || it->second.zoom <= 0) return;
        const float z = (float)it->second.zoom;
        Z.cam.l = it->second.x; Z.cam.b = it->second.y; Z.cam.w = (double)(569.0f / z); Z.cam.h = (double)(320.0f / z); Z.cam.ok = true; Z.cam.okY = true;
    }
    // hist version 13 (frames.hpp g_anchorRng): after the first parentless replay of the call -- the steps up to the anchor -- the shadow's own LCG against the game's at the anchor tick. Printed once per call (observation); with
    // --trigshadowopts random* the game's value is taken in when they differ (the stream the shadow went on with is then the game's).
    long long anchorRngMatch = 0, anchorRngDiff = 0;
    bool anchorRngReported = false;
    void anchorRngCheck(gdtrig::TrigShadow& S) {
        if (!g_anchorRngKnown || anchorRngReported) return;
        anchorRngReported = true;
        const uint32_t mine = (uint32_t)(S.rng_ & 0xffffffffULL);
        const bool same = mine == g_anchorRng;
        (same ? anchorRngMatch : anchorRngDiff) += 1;
        const bool taken = !same && (S.drawOnFollow || S.rngPinFirst_);
        std::printf("trigshadowlive: anchor rng: the game %u (0x%08x), the shadow %u (0x%08x) after %lld draws: %s%s\n", g_anchorRng, g_anchorRng, mine, mine, S.randDraws_, same ? "SAME" : "DIFFERENT", taken ? " (the game's value is taken in)" : "");
        if (std::FILE* af = std::fopen((dir + "/anchor_rng.txt").c_str(), "a")) { std::fprintf(af, "%lld,%u,%u,%lld,%d\n", lastT, g_anchorRng, mine, S.randDraws_, same ? 1 : 0); std::fclose(af); }   // the data directory's own file, so the loop's runs (whose stdout is not kept) leave the comparison: step, game, shadow, draws, same
        if (taken) S.rng_ = g_anchorRng;
    }
    void replayInto(gdtrig::TrigShadow& S, const Pending& p, dp::gdcam::CamState* cs = nullptr) {
        if (!replay && !events) return;
        // The touch triggers the attempt had entered before the anchor (--touchentered uid:tick) that the model keeps no box for -- a trigger whose chain moves only things with a recording, which the model reads
        // from the record -- are not in boxUid, so no latch bit tells the shadow of them and the replay never fired them: an Area Move entered at tick 2620 (c139771614 uid 19878, the Area under the block 12693)
        // stood unfired, and the block the game moves 1 px by tick 4936 stayed where the level has it (the spider pad's teleport then landed 1.09 px low). Each of them is told to the replay at its entry tick,
        // as a box's call is (fireB + evShift), when the shadow models it.
        if (events && g_touchEnteredGiven) {
            std::set<int> boxed(boxUid.begin(), boxUid.end());
            for (const auto& kv : g_touchEnteredT) {
                if (kv.second <= 0 || (long long)kv.second >= lastT || boxed.count(kv.first) || !S.modelled(kv.first)) continue;
                S.addTouchEvent((long long)kv.second + evShift + (entryJump(kv.second) == 1 ? 1 : 0), kv.first);   // a jump into the box is called one tick later than the rows overlap (entryJump)
                ++evEnteredGiven;
            }
        }
        feed(S, p, 0, true);
        if (g_xTrack.empty()) { ++evNoTrack; return; }
        const auto r0 = Clock::now();
        const char* dbgUid = std::getenv("TSEV_UID");   // diagnosis: the first tick the replay moves this uid
        const auto dbgIt = dbgUid ? S.byUid.find(std::atoi(dbgUid)) : S.byUid.end();
        bool dbgDone = false;
        long long lastDone = -1; size_t lastK = (size_t)-1;
        const bool worldRows = supplyWorld && g_xTrackW.size() == g_xTrack.size();   // --trigshadowworld: the track's rows have the world x, y and frame
        size_t xi = 0;
        struct RandLogScope { RandLogScope() { gdtrig::g_randLogReplay = true; } ~RandLogScope() { gdtrig::g_randLogReplay = false; } } randLogScope;   // --trigshadowrandlog: the draws of this replay
        for (const auto& kv : g_xTrack) {
            const size_t k = xi++;
            if (kv.first >= lastT) break;
            if (cs && camModelOn) camModelStep(S, *cs, kv.second); else setCamAt(S, kv.first);
            if (worldRows) S.step(kv.first, g_xTrackW[k][0], g_xTrackW[k][1]); else
            S.step(kv.first, kv.second, 0.0);
            if (cs && camModelOn) camModelAfter(S, *cs);
            lastDone = kv.first; lastK = k;
            ++evReplaySteps;
            if (dbgUid && !dbgDone && dbgIt != S.byUid.end()) {
                const gdtrig::ObjState& o = S.objs[dbgIt->second];
                const gdtrig::ObjState& o0 = init[dbgIt->second];
                if (o.x != o0.x || o.y != o0.y) { dbgDone = true; std::printf("trigshadowev: replay moved uid %s at step %lld to (%.3f, %.3f) from (%.3f, %.3f)\n", dbgUid, (long long)kv.first, o.x, o.y, o0.x, o0.y); }
            }
        }
        if (xcont && lastDone >= 0) {   // --trigshadowxcont: past the end of the x track the timeline does not jump to this layer: it goes on one tick at a time along the x of the layers this call had before (seenX)
            bool any = false;
            for (auto it = seenX.upper_bound(lastDone); it != seenX.end() && it->first < lastT; ++it) {
                double x = it->second.front().second;
                for (const auto& e : it->second) if (e.first == p.dx) { x = e.second; break; }
                if (cs && camModelOn) camModelStep(S, *cs, x); else setCamAt(S, it->first);
                S.step(it->first, x, 0.0);
                if (cs && camModelOn) camModelAfter(S, *cs);
                ++evReplaySteps; ++xcontSteps; any = true;
                lastDone = it->first;
            }
            if (any) ++xcontReplays;
        }
        // A track that ends before this layer (a call anchored past the end of its --xtrack: a mid-plan re-anchor after the flight that wrote the track died earlier) leaves the ticks in between without a row.
        // Stepping the timeline from the last row straight to the layer lets a position trigger the gap crosses fire inside ONE step of the whole gap's width, and a Move locked to the player's X (lockX) then
        // adds the whole gap's travel instead of the part after its own crossing (lv20 t=17104, the Move 13310 at x 24285: the track ends at 23997, the layer's x is 24832, so the spike under it stood 288 px,
        // the part of the gap before the crossing, ahead of the game's). The gap is walked a tick at a time, the x running in a straight line from the last row to this layer's x (the speed is the
        // anchor's over a stretch of a few hundred ticks; where it changes the dating is off by the ticks the change moves the crossing by, not by the whole gap). Frame 0 only: a turned frame's
        // world x / y are not one straight line, and a gap there keeps the jump.
        if (lastDone >= 0 && lastDone + 1 < lastT && p.frame == 0 && lastK != (size_t)-1 && (!worldRows || (int)g_xTrackW[lastK][2] == 0)) {
            const double xl = g_xTrack[lastK].second, yl = worldRows ? g_xTrackW[lastK][1] : 0.0;
            const double xl0 = worldRows ? g_xTrackW[lastK][0] : xl;
            if (g_xTrack[lastK].first == lastDone) {   // (xcont's steps moved lastDone past the row: its own x is the line's start then, which it does not keep; leave that gap alone)
                for (long long t = lastDone + 1; t < lastT; ++t) {
                    const double f = (double)(t - lastDone) / (double)(lastT - lastDone);
                    const double x = xl + (p.x - xl) * f, wx = xl0 + (p.x - xl0) * f;
                    if (cs && camModelOn) camModelStep(S, *cs, x); else setCamAt(S, t);
                    if (worldRows) S.step(t, wx, yl); else S.step(t, x, 0.0);
                    if (cs && camModelOn) camModelAfter(S, *cs);
                    ++evReplaySteps; ++gapSteps;
                }
                ++gapReplays;
            }
        }
        ++evReplays;
        evReplayMs += std::chrono::duration<double, std::milli>(Clock::now() - r0).count();
    }

    // A parentless timeline is the level's shadow replayed from tick 0 to its layer along the run's x track, one step a tick, and a call makes it more than once: for the search's first layer, and again for
    // the first tick of the walk of the plan the search found (the walk starts where the search did and keeps none of its timelines). On a call anchored at tick 33,983 of a level with 26,000 moving objects
    // one replay is 240 ms, as much as every other tick of that walk's shadow together (measured). The replay is a function of the call's inputs (the level's shadow, the x track, the touch triggers entered
    // before the anchor) and of what the group hands it: the layer, the touch latch with its entry ticks, the event digest, and the group's x and frame (the gap past the track's end). The call's first one
    // is kept as it stood after the replay, before the anchor's rng check and the layer's own step, and a later group that hands the same starts from a copy of it. Not with --trigshadowxcont (that replay
    // goes on along the layers the call has had, which are not the same later), --trigshadowrandlog (every replay writes its draws) or the replay's diagnosis (TSEV_UID / TSEV_DBG print from inside it).
    // The group's x is part of what it hands only where the replay read it, in the gap past the track's end (the search's group and the walk's state of the same anchor do not hand the same x: 48849.6617 and
    // 48849.6602 on the call above, the walk's being the state's float). load() and resetLevel() drop it: a call's inputs are not the next call's.
    // It also holds the level's shadow it was replayed from (of): a call that came without --trigshadowlive does not pass load(), and this object lives across the levels of a session, so a kept replay is
    // only taken while ase is that same object. Holding it keeps the object alive, so another level's shadow cannot stand at its address.
    struct ReplayMemo { bool have = false; long long t = -1; uint64_t bits = 0, evg = 0; uint16_t fb[kTouchBits] = {}; double x = 0; int frame = 0; bool gap = false; std::shared_ptr<const gdtrig::TrigShadow> sh; std::shared_ptr<gdtrig::TrigShadow> of; dp::gdcam::CamState cam; };   // gap: the replay walked the gap, so it read x
    ReplayMemo replayMemo;
    long long replayReused = 0;
    std::shared_ptr<gdtrig::TrigShadow> freshTimeline(const Pending& p, dp::gdcam::CamState& cam) {
        static const bool diag = std::getenv("TSEV_UID") != nullptr || std::getenv("TSEV_DBG") != nullptr;   // read once
        const bool keep = !xcont && !diag && gdtrig::g_randLogFile.empty();
        ReplayMemo& M = replayMemo;
        if (keep && M.have && M.of == base && M.t == lastT && M.bits == p.bits && M.evg == p.evg && M.frame == p.frame && (!M.gap || std::memcmp(&M.x, &p.x, sizeof(double)) == 0) && std::memcmp(M.fb, p.fb, sizeof M.fb) == 0) {
            ++replayReused; cam = M.cam;
            return std::make_shared<gdtrig::TrigShadow>(*M.sh);
        }
        std::shared_ptr<gdtrig::TrigShadow> m = std::make_shared<gdtrig::TrigShadow>(*base);
        const long long gap0 = gapReplays;
        replayInto(*m, p, &cam);
        if (keep && !M.have) {
            M.have = true; M.gap = gapReplays != gap0; M.t = lastT; M.bits = p.bits; M.evg = p.evg; M.frame = p.frame; M.x = p.x; std::memcpy(M.fb, p.fb, sizeof M.fb);
            M.sh = std::make_shared<const gdtrig::TrigShadow>(*m); M.of = base; M.cam = cam;
        }
        return m;
    }

    // --trigsupply-group: this group's timeline, stepped NOW (at group()): a copy of the parent's (or a fresh one, replayed), the touch Events it is owed, its place before the step for the supplied uids, the step, its place
    // after. endLayer then leaves it alone. Always a copy: the claim count that lets a last claimant take the parent over is not known yet, and the supplied-group arm is a measurement.
    double nowCopyMs = 0, nowFillMs = 0, nowStepMs = 0, nowTailMs = 0;   // stepNow's parts (a report): the timeline copy / take-over + feed, the supply table, the step, the rest
    // SUPHASH_OUT=FILE (env, read once; observation only): one line per supplied group step -- "layer, the number of supplied values, a hash of every value the supply hands the search for this group (the
    // place after and before the step, the on/off, the scale, and the Teleports / Gravity triggers fired)", and on every 64th layer the timeline's own dynHash as well. Two builds that store the timeline
    // differently are held against each other with it: the layer digest only sees the supply through what the states did with it. (The line's form is gd-04's, 2026-11-17.)
    void supHashLine(const gdtrig::TrigShadow& m) {
        static std::FILE* const f = [] { const char* e = std::getenv("SUPHASH_OUT"); return (e && *e) ? std::fopen(e, "a") : nullptr; }();
        if (!f) return;
        uint64_t h = 1469598103934665603ULL; size_t nOk = 0;
        auto mixb = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
        auto bitsOf = [](double d) { uint64_t u; std::memcpy(&u, &d, sizeof u); return u; };
        for (size_t i = 0; i < supObj.size(); ++i) {
            SupVal v; const bool ok = supVal(i, v);
            mixb((uint64_t)(ok ? 1 : 0));
            if (!ok) continue;
            ++nOk;
            mixb(bitsOf(v.x)); mixb(bitsOf(v.y)); mixb(bitsOf(v.px)); mixb(bitsOf(v.py)); mixb(bitsOf((double)v.sx)); mixb(bitsOf((double)v.sy)); mixb(v.pon ? 1u : 0u);
        }
        mixb(0x7470ULL); for (int u : tpFires_) mixb((uint64_t)(uint32_t)u);
        mixb(0x6772ULL); for (int u : gravFires_) mixb((uint64_t)(uint32_t)u);
        if (lastT % 64 == 0) std::fprintf(f, "%lld %zu %016llx %016llx\n", (long long)lastT, nOk, (unsigned long long)h, (unsigned long long)m.dynHash());
        else std::fprintf(f, "%lld %zu %016llx\n", (long long)lastT, nOk, (unsigned long long)h);
        static long long lines = 0;
        if ((++lines & 4095) == 0) std::fflush(f);
    }
    // WORLDHASH_OUT=FILE (env, read once; observation only): the worlds of each layer by the shadow's content hash (TrigShadow::contentHash / trackHash). One line per layer of supplied group steps:
    //   layer, groups stepped, distinct parent contents, distinct (parent content, parent track), distinct child contents, distinct (child content, child track), groups whose child reads the
    //   player's place (an Area in force / a player lock running), purity pairs, purity violations, pages hashed anew
    // A purity pair is two groups of the layer whose timelines had the same content AND track before the step and were given the same inputs (the step's x / y and camera): their children must have
    // the same content and track, or the hash leaves out something a step reads (a violation: printed, the first 20 with the layer).
    struct WsRow { gdtrig::Hash2 pc, pt, in, cc, ct; bool reads = false; };
    std::vector<WsRow> wsRows; long long wsT = -1, wsViol = 0, wsPairs = 0, wsSteps = 0; double wsHashMs = 0;
    static std::FILE* worldStatFile() { static std::FILE* const f = [] { const char* e = std::getenv("WORLDHASH_OUT"); return (e && *e) ? std::fopen(e, "a") : nullptr; }(); return f; }
    static bool worldStatOn() { return worldStatFile() != nullptr; }
    void worldStatBefore(WsRow& r, const gdtrig::TrigShadow& m, double wx, double wy) {
        const auto t0 = Clock::now();
        if (lastT != wsT) { worldStatFlush(); wsT = lastT; }
        r.pc = m.contentHash(); r.pt = m.trackHash();
        r.in.addD(wx); r.in.addD(wy); r.in.addD(m.cam.l); r.in.addD(m.cam.b); r.in.addD(m.cam.w); r.in.addD(m.cam.h); r.in.add((m.cam.ok ? 1 : 2) + (m.cam.okY ? 4 : 0));
        wsHashMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
    void worldStatAfter(WsRow& r, const gdtrig::TrigShadow& m) {
        const auto t0 = Clock::now();
        r.cc = m.contentHash(); r.ct = m.trackHash(); r.reads = m.readsPlayerPlace();
        wsRows.push_back(r); ++wsSteps;
        wsHashMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
    void worldStatFlush() {
        if (wsRows.empty() || !worldStatOn()) { wsRows.clear(); return; }
        auto k1 = [](const gdtrig::Hash2& a) { return a.a ^ (a.b * 0x9e3779b97f4a7c15ULL); };
        auto k2 = [&](const gdtrig::Hash2& a, const gdtrig::Hash2& b) { return gdtrig::Hash2::mixA(k1(a), k1(b)); };
        std::unordered_set<uint64_t> sPc, sPct, sCc, sCct;
        std::unordered_map<uint64_t, uint64_t> pure;   // (parent content, parent track, inputs) -> (child content, child track)
        long long reads = 0, pairs = 0, viol = 0;
        for (const WsRow& r : wsRows) {
            sPc.insert(k1(r.pc)); sPct.insert(k2(r.pc, r.pt)); sCc.insert(k1(r.cc)); sCct.insert(k2(r.cc, r.ct));
            if (r.reads) ++reads;
            const uint64_t key = gdtrig::Hash2::mixA(k2(r.pc, r.pt), k1(r.in)), val = k2(r.cc, r.ct);
            const auto ins = pure.emplace(key, val);
            if (!ins.second) { ++pairs; if (ins.first->second != val) { ++viol; if (wsViol + viol <= 20) std::printf("worldhash: PURITY VIOLATION at layer %lld: two timelines with one content, track and input have different children\n", wsT); } }
        }
        wsPairs += pairs; wsViol += viol;
        long long hashed = 0;
        for (const auto& kv : prev) if (kv.second.sh) hashed += kv.second.sh->objs.hashedPages();   // (called from the next layer's first step: the layer's timelines are in `prev` by then)
        std::fprintf(worldStatFile(), "%lld %zu %zu %zu %zu %zu %lld %lld %lld %lld\n", wsT, wsRows.size(), sPc.size(), sPct.size(), sCc.size(), sCct.size(), reads, pairs, viol, hashed);
        wsRows.clear();
    }
    // dupbatch (shareSteps, set by the search per layer): a group whose parent timeline (the same object), owed touch calls and step inputs (x, world y, frame) are an earlier group's of this layer takes
    // that group's stepped timeline -- the object itself -- instead of copying the parent and stepping the copy: the step is a function of exactly those (the camera model's state rides with the Line and is
    // the first group's by the same argument). Two Lines then hold one timeline, so neither may be stepped in place by a group of the next layer (Line::shared: the take-overs below copy instead).
    // Groups come to share a parent object when a key splits (the new key starts from a copy of an existing Line) and stay together for as long as their inputs agree.
    bool shareSteps = false;
    struct StepMemo { Line* line = nullptr; const gdtrig::TrigShadow* parentSh = nullptr; uint64_t owed = 0, evg = 0, evtold = 0; double x = 0, wy = 0, wx = 0; int frame = 0; gdtrig::CowPages<gdtrig::ObjState> pre; std::vector<int> tp, grav; bool anyBlind = false; };
    std::unordered_map<uint64_t, StepMemo> stepMemo;   // this layer's steps by (parent timeline, owed calls, inputs)
    long long sharedSteps = 0;
    void stepNow(Pending& p) {
        const auto tA = Clock::now();
        double wx0 = p.x, wy0 = 0.0; stepCoords(p.frame, p.x, wx0, wy0);
        if (supplyWorld && p.frame == 0 && std::isfinite(p.wy)) wy0 = p.wy;   // a live group has no world y (many states): the crossing reads 0 as before (T63 had it WAIT: lv22 search stalled); only the walk's one state hands its y
        if (supplyWorld && (p.frame & 1) && std::isfinite(p.wx)) wx0 = p.wx;   // a turned frame (1 / 3): the walk's one state hands its world x; a live group's is not known and reads 0 as before (T89)
        const bool mayShare = shareSteps && p.parent && p.parent->sh && !singlePath;
        const uint64_t owedNow = (mayShare && events) ? (p.bits & ~p.parent->told) : 0;
        uint64_t mkey = 0;
        if (mayShare) {
            gdtrig::Hash2 h; h.add((uint64_t)(uintptr_t)p.parent->sh.get()); h.add(owedNow); h.add(p.evg); h.add(p.parent->evtold); h.addD(p.x); h.addD(wy0); h.addD(wx0); h.add((uint64_t)(uint32_t)p.frame);
            mkey = h.a ^ (h.b * 0x9e3779b97f4a7c15ULL);
            const auto it = stepMemo.find(mkey);
            if (it != stepMemo.end() && it->second.parentSh == p.parent->sh.get() && it->second.owed == owedNow && it->second.evg == p.evg && it->second.evtold == p.parent->evtold && it->second.frame == p.frame
                && std::memcmp(&it->second.x, &p.x, sizeof(double)) == 0 && std::memcmp(&it->second.wy, &wy0, sizeof(double)) == 0 && std::memcmp(&it->second.wx, &wx0, sizeof(double)) == 0) {
                StepMemo& M = it->second;
                Line L; L.sh = M.line->sh; L.x = p.x; L.told = p.bits; L.evtold = p.evg; for (int b = 0; b < kTouchBits; ++b) L.fb[b] = p.fb[b];
                L.cam = M.line->cam; L.shared = true; M.line->shared = true;
                supCur = L.sh; supPre = M.pre;
                tpFires_ = M.tp; tpFiresTick_ = lastT; gravFires_ = M.grav; gravFiresTick_ = lastT;
                if (M.anyBlind) ++supCamBlind;
                supHashLine(*L.sh);
                p.sh = L.sh; p.stepped = true;
                cur[p.key] = std::move(L);
                ++supGroupsDone; ++sharedSteps;
                nowCopyMs += std::chrono::duration<double, std::milli>(Clock::now() - tA).count();
                return;
            }
        }
        std::shared_ptr<gdtrig::TrigShadow> m;
        dp::gdcam::CamState freshCam;   // --trigshadowopts cammodel: a parentless timeline's camera after its replay
        const gdtrig::TrigShadow* const parentShPtr = p.parent ? p.parent->sh.get() : nullptr;
        if (!p.parent) { m = freshTimeline(p, freshCam); ++fresh; anchorRngCheck(*m); }
        else if (layerTake && p.parentNamed && !p.parent->taken && !p.parent->shared) { m = p.parent->sh; p.parent->taken = true; ++moves; feed(*m, p, p.parent->told); }   // --trigsupplytake: this key's parent has no other claimant this layer
        else if (singlePath && pend.size() == 1 && !p.parent->taken && !p.parent->shared) { m = p.parent->sh; p.parent->taken = true; layerMoved = true; ++moves; feed(*m, p, p.parent->told); }   // --replay: one group a tick on one path, nobody else wants the parent
        else { m = std::make_shared<gdtrig::TrigShadow>(*p.parent->sh); ++copies; feed(*m, p, p.parent->told); }
        const auto tB = Clock::now();
        supPre.assignNear(m->objs);   // the objects before this group's step (shares the pages; the step clones the ones it writes)
        supCur = m;
        const auto tC = Clock::now();
        Line L; L.sh = m; L.x = p.x; L.told = p.bits; L.evtold = p.evg; for (int b = 0; b < kTouchBits; ++b) L.fb[b] = p.fb[b];   // the order of endLayer's step loop: camera at the tick, camera model, then the step (after feed)
        setCamAt(*m, lastT);
        if (camModelOn) { L.cam = p.parent ? p.parent->cam : freshCam; camModelStep(*m, L.cam, p.x, p.frame, &p.live); }
        { const double wx = wx0, wy = wy0;
          WsRow wr; const bool ws = worldStatOn(); if (ws) worldStatBefore(wr, *m, wx, wy);
          m->step(lastT, wx, wy);
          if (camModelOn) camModelAfter(*m, L.cam);
          if (ws) worldStatAfter(wr, *m); }
        ++steps;
        if (p.cross) {   // the x-crossing split's premise: the timeline has passed exactly the running triggers the group's members have crossed
            ++crossSteps;
            const size_t cq = std::min(m->chanCursor(0), crossRun.size() - 1);
            const uint32_t has = crossRun[cq];
            if (has != p.cross1) {
                long long& n = has < p.cross1 ? crossShort : crossOver;
                if (++n <= 6) std::printf("trigsupply: crosscheck layer %lld x %.4f: the group has crossed %u running trigger(s) (%u before the step), its timeline has passed %u (queue cursor %zu, active channel %d, vertical %d, parent %s)\n",
                                          lastT, p.x, p.cross1, p.cross0, has, m->chanCursor(0), m->activeChan(), (int)m->travelsVertical(), !p.parent ? "none (replayed)" : p.parentNamed ? "named" : "guessed");
            }
        }
        const auto tD = Clock::now();
        static const char* const tsupAll = std::getenv("TSUP_ALL");   // read once
        if (const char* da = tsupAll) {   // diagnosis: where this uid is in every group's timeline at every layer, supplied or not (first 40000 lines)
            static int shownAll = 0;
            const auto bi = m->byUid.find(std::atoi(da));
            if (bi != m->byUid.end() && shownAll < 40000) { ++shownAll; std::printf("tsall: layer %lld parent %s groupX %.4f uid y %.4f x %.4f\n", lastT, p.parent ? "yes" : "NONE", p.x, m->objs[bi->second].y, m->objs[bi->second].x); }
        }
        bool anyBlind = false;
        if (m->anyBlind())   // (only a timeline that has a blind object at all is walked for a supplied one)
            for (size_t i = 0; i < supObj.size() && !anyBlind; ++i) if (supObj[i] >= 0 && m->objs[(size_t)supObj[i]].blind) anyBlind = true;
        tpFires_.clear(); tpFiresTick_ = lastT;
        for (auto ei = m->events.rbegin(); ei != m->events.rend() && ei->step == lastT; ++ei) tpFires_.push_back(ei->uid);   // newest first -> reverse to the firing order
        std::reverse(tpFires_.begin(), tpFires_.end());
        gravFires_.clear(); gravFiresTick_ = lastT;
        for (auto gi = m->gravChanges.rbegin(); gi != m->gravChanges.rend() && gi->step == lastT; ++gi) gravFires_.push_back(gi->uid);
        std::reverse(gravFires_.begin(), gravFires_.end());
        if (anyBlind) ++supCamBlind;   // (a timeline with a blind object: the objects it stepped with an uncertain camera are not supplied, the rest are)
        supHashLine(*m);
        Line& stored = (cur[p.key] = std::move(L));
        if (mayShare) {   // (this layer's later groups with the same parent object, owed calls and inputs take `stored`'s timeline)
            StepMemo& M = stepMemo[mkey];
            M.line = &stored; M.parentSh = parentShPtr; M.owed = owedNow; M.evg = p.evg; M.evtold = p.parent ? p.parent->evtold : 0; M.x = p.x; M.wy = wy0; M.wx = wx0; M.frame = p.frame; M.pre = supPre; M.tp = tpFires_; M.grav = gravFires_; M.anyBlind = anyBlind;
        }
        p.sh = m; p.stepped = true;
        ++supGroupsDone;
        const auto tE = Clock::now();
        nowCopyMs += std::chrono::duration<double, std::milli>(tB - tA).count(); nowFillMs += std::chrono::duration<double, std::milli>(tC - tB).count();
        nowStepMs += std::chrono::duration<double, std::milli>(tD - tC).count(); nowTailMs += std::chrono::duration<double, std::milli>(tE - tD).count();
    }

    // the layer's groups are stepped together once all of them are known: a parent timeline that two groups want is copied BEFORE either of them steps it
    void endLayer() {
        if (pend.empty()) return;
        std::unordered_map<Line*, int> claims;
        for (const Pending& p : pend) if (p.parent && !p.stepped) ++claims[p.parent];
        std::vector<std::shared_ptr<gdtrig::TrigShadow>> mine(pend.size());
        std::vector<dp::gdcam::CamState> freshCam(pend.size());   // --trigshadowopts cammodel: the state of a parentless timeline after its replay
        {   // the worlds of this layer (observation)
            std::unordered_set<uint64_t> wk, fs;
            long long par = 0;
            for (const Pending& p : pend) { wk.insert(p.wk); fs.insert(p.fs); par += p.parents; }
            Bucket& b = buckets[lastT / 2000];
            ++b.layers; b.keys += (long long)pend.size(); b.worlds += (long long)wk.size(); b.sigs += (long long)fs.size(); b.parents += par;
            b.maxKeys = std::max(b.maxKeys, (long long)pend.size()); b.maxWorlds = std::max(b.maxWorlds, (long long)wk.size()); b.maxSigs = std::max(b.maxSigs, (long long)fs.size());
            ++wLayers; wKeys += (long long)pend.size(); wWorlds += (long long)wk.size(); wSigs += (long long)fs.size(); wParents += par;
            wMaxWorlds = std::max(wMaxWorlds, (long long)wk.size()); wMaxSigs = std::max(wMaxSigs, (long long)fs.size());
            ++worldHist[(long long)wk.size()];
        }
        const auto t0 = Clock::now();
        for (size_t i = 0; i < pend.size(); ++i) {
            Line* par = pend[i].parent;
            if (pend[i].stepped) { mine[i] = pend[i].sh; continue; }   // --trigsupply-group: stepped at group()
            if (!par) { mine[i] = freshTimeline(pend[i], freshCam[i]); ++fresh; anchorRngCheck(*mine[i]); }
            else if (--claims[par] == 0 && !par->taken && !par->shared) { par->taken = true; mine[i] = par->sh; ++moves; feed(*mine[i], pend[i], par->told); }   // the last claimant takes the parent's timeline over (not one another Line holds too: shareSteps)
            else { mine[i] = std::make_shared<gdtrig::TrigShadow>(*par->sh); ++copies; feed(*mine[i], pend[i], par->told); }
        }
        const auto t1 = Clock::now();
        std::vector<std::vector<std::pair<double, double>>> pre(pend.size());   // the shadow's place before this layer's step (tick t-1), for the snapshot's uids
        bool anySnap = false;
        for (size_t i = 0; i < pend.size(); ++i) {
            if (!pend[i].snap) continue;
            anySnap = true;
            const auto s0 = Clock::now();
            pre[i].reserve(pend[i].objs.size());
            if (pend[i].stepped) { for (size_t k = 0; k < pend[i].objs.size(); ++k) pre[i].push_back({1e18, 1e18}); continue; }   // (already stepped: the place before the step is not kept for the snapshot)
            for (const SnapObj& o : pend[i].objs) {
                const auto bi = mine[i]->byUid.find(o.uid);
                pre[i].push_back(bi == mine[i]->byUid.end() ? std::make_pair(1e18, 1e18) : std::make_pair(mine[i]->objs[bi->second].x, mine[i]->objs[bi->second].y));
            }
            snapMs += std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
        }
        for (size_t i = 0; i < pend.size(); ++i) {
            if (pend[i].stepped) continue;
            setCamAt(*mine[i], lastT);
            Line L; L.sh = mine[i]; L.x = pend[i].x; L.told = pend[i].bits; L.evtold = pend[i].evg; for (int b = 0; b < kTouchBits; ++b) L.fb[b] = pend[i].fb[b];
            if (camModelOn) {
                if (pend[i].parent) L.cam = pend[i].parent->cam;
                else L.cam = freshCam[i];
                camModelStep(*mine[i], L.cam, pend[i].x, pend[i].frame, &pend[i].live);
            }
            { double wx = pend[i].x, wy = 0.0; stepCoords(pend[i].frame, pend[i].x, wx, wy); const bool wyOk = supplyWorld && pend[i].frame == 0 && std::isfinite(pend[i].wy); if (wyOk) wy = pend[i].wy; const bool wxOk = supplyWorld && (pend[i].frame & 1) && std::isfinite(pend[i].wx); if (wxOk) wx = pend[i].wx; mine[i]->step(lastT, wx, wy); if (camModelOn) camModelAfter(*mine[i], L.cam); }
            cur[pend[i].key] = std::move(L);
            ++steps;
        }
        const auto t2 = Clock::now();
        if (anySnap) {
            const auto c0 = Clock::now();
            ++chkLayers;
            for (size_t i = 0; i < pend.size(); ++i) {
                if (!pend[i].snap) continue;
                ++chkGroups;
                gdGroup(pend[i], pend[i].x);
                const gdtrig::TrigShadow& Z = *mine[i];
                for (size_t k = 0; k < pend[i].objs.size(); ++k) {
                    const SnapObj& o = pend[i].objs[k];
                    const auto bi = Z.byUid.find(o.uid);
                    const auto wi = watch.empty() ? watch.end() : watch.find(o.uid);
                    if (wi != watch.end()) ++wi->second.snaps;
                    if (bi == Z.byUid.end()) { ++chkNotInShadow; if (wi != watch.end()) ++wi->second.notIn; dbgI = i; dbgN = pend.size(); gdCompare(pend[i].x, o, nullptr, pend[i].bits, pend[i].fb); continue; }
                    if (!Z.coveredObject(bi->second) || !Z.actedOnByModelled(bi->second)) { ++chkUncovered; if (wi != watch.end()) ++wi->second.uncov; dbgI = i; dbgN = pend.size(); gdCompare(pend[i].x, o, nullptr, pend[i].bits, pend[i].fb); continue; }
                    const gdtrig::ObjState& S = Z.objs[bi->second];
                    const double dT = std::max(std::fabs(S.x - o.x), std::fabs(S.y - o.y));
                    const double dP = pre[i][k].first > 1e17 ? 1e18 : std::max(std::fabs(pre[i][k].first - o.x), std::fabs(pre[i][k].second - o.y));
                    dbgI = i; dbgN = pend.size(); gdCompare(pend[i].x, o, &S, pend[i].bits, pend[i].fb);   // --trigshadowgd: the same mover against the game's own record, for the groups that are on the game's path
                    UidStat& us = uidStat[o.uid];
                    ++chkObjs; ++us.n;
                    const bool okT = dT <= 0.01, okP = dP <= 0.01;
                    if (okT) { ++chkAtT; ++us.atT; }
                    else if (okP) { ++chkAtPrev; ++us.atPrev; }
                    else { ++chkNone; ++us.none; if (wi != watch.end() && wi->second.firstNoneT < 0) { wi->second.firstNoneT = lastT; wi->second.firstNoneD = dT; } }
                    us.maxD = std::max(us.maxD, dT); us.sumD += dT;
                    ++chkHist[dT <= 0.01 ? 0 : dT <= 0.5 ? 1 : dT <= 2.0 ? 2 : dT <= 20.0 ? 3 : 4];
                }
                pend[i].objs.clear();
            }
            chkMs += std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
        }
        copyMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        stepMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
        pend.clear(); actPend = -1;
        ++layers;
        maxLines = std::max(maxLines, (long long)cur.size());
        sumLines += (long long)cur.size();
        if (layers % 256 == 0) {   // a snapshot of every timeline of this layer: the objects that differ from the load, 48 bytes each, and the rest
            for (const auto& kv : cur) {
                const gdtrig::TrigShadow& Z = *kv.second.sh;
                long long n = 0;
                for (size_t i = 0; i < Z.objs.size(); ++i) {
                    const gdtrig::ObjState& a = Z.objs[i]; const gdtrig::ObjState& b = init[i];
                    if (a.x != b.x || a.y != b.y || a.rot != b.rot || a.counter != b.counter || a.ax != b.ax || a.ay != b.ay) ++n;
                }
                const double kb = ((double)n * 48.0 + (double)Z.dynPrivBytes()) / 1024.0;
                snapSumKB += kb; ++snapSamples; snapMaxKB = std::max(snapMaxKB, kb);
            }
        }
        if (layers % 2000 == 0) print(reportFull);
    }

    // one group of layer t: x = its K.x
    double grpTotalMs = 0, grpEndLayerMs = 0, grpClearMs = 0, grpPre = 0, grpFind = 0, grpPush = 0, grpStepNow = 0;   // group()'s parts (a report)
    // --camy S2.1: the camera Y's inputs at tick t for the group just pushed: the model's CamYTick of a COPY of the parent timeline's camera state stepped at the group's x (what endLayer will do to the group's own Line.cam), stamped
    // with (t, key). A group with no parent timeline (the first layer of a search: endLayer replays it from the x track) or a run without --trigshadowopts cammodel has none: valid stays false and the search counts it.
    long long camYNoParent = 0, camYNoModel = 0;
    void camYTickFor(Pending& p, long long t) {
        if (!camModelOn) { ++camYNoModel; return; }
        if (!p.parent) { ++camYNoParent; return; }
        dp::gdcam::CamState cs = p.parent->cam;
        // --camy (a): a moving static target the model could not place when the trigger fired is read now, once, from the parent's timeline -- the shadow as the parent's last step left it
        p.live = dp::gdcam::CamLive{};
        if (cs.yev.liveWait && p.parent->sh && !dp::gdcam::g_camNoLive) { float ly = 0.0f; ++dp::gdcam::g_camLiveAsked; int lw = 0; if (!p.parent->sh->firstMemberY(cs.yev.tgt, &ly, &lw)) ++dp::gdcam::g_camLiveWhy[(lw < 0 || lw > 4) ? 0 : lw]; else { p.live.ok = true; p.live.g = cs.yev.tgt; p.live.y = ly; ++dp::gdcam::g_camLiveGot; } }
        dp::gdcam::CamYTick yt;
        p.camY.zoomPrev = cs.zoomY.v; p.camY.offYPrev = cs.offY.v;   // the Y side's values after the parent's step (this tick's previous ones)
        { double wx = p.x, wy = 0.0; stepCoords(p.frame, p.x, wx, wy); camModel.step(cs, p.x, nullptr, &yt, wx, supplyWorld ? wy : std::nan(""), &p.live); }
        p.camY.valid = true; p.camY.t = t; p.camY.key = p.key; p.camY.tick = yt;
    }
    CamYGroup camYOfLast() const { return (on && !pend.empty()) ? pendCur().camY : CamYGroup{}; }   // (pendCur: the group just given, or under dupbatch the one activate() made current)
    void group(long long t, float dx, uint64_t trigWord, int frame, int rev, uint64_t fsig, int xc, double x, const uint16_t* fireB = nullptr, double wy = std::nan(""), double wxw = std::nan("")) {
        if (!on) return;
        const auto gA = Clock::now();
        ensure();
        refreshBoxes();
        if (!anchorSeen) {   // the first group of the call: its x is the anchor's (plus one step). A supplied group a touch mover behind that x acts on is dropped (gd-04, a custom level:46200 g625)
            anchorSeen = true;
            if (!supplyGroups.empty() && base) {
                for (auto gi = supplyGroups.begin(); gi != supplyGroups.end();) {
                    int tu = 0;
                    int mu = 0;
                    if (base->groupTouchBehind(*gi, x, &tu)) {
                        if (noteBehind(*base, *gi, x)) { std::printf("trigsupply: group %d stays supplied: its touch trigger(s) behind the anchor (x %.1f) are all decided by the anchor's history\n", *gi, x); ++gi; continue; }
                        std::printf("trigsupply: group %d is NOT supplied: a touch trigger %d that acts on it has its box behind the anchor (x %.1f): the touch fired before the anchor and the shadow has no Event for it\n", *gi, tu, x);
                        gi = supplyGroups.erase(gi);
                    } else if (supplyAll && base->groupGateTouchBehind(*gi, x, &tu, &mu)) {   // --trigsupply-all only: hand-chosen groups are as measured
                        if (noteBehind(*base, *gi, x)) { std::printf("trigsupply: group %d stays supplied: its touch trigger(s) behind the anchor (x %.1f) are all decided by the anchor's history\n", *gi, x); ++gi; continue; }
                        std::printf("trigsupply: group %d is NOT supplied: a touch trigger %d behind the anchor (x %.1f) gates the trigger %d that moves it\n", *gi, tu, x, mu);
                        gi = supplyGroups.erase(gi);
                    } else ++gi;
                }
                if (behindGroups) std::printf("trigsupply: behind the anchor (x %.1f): %lld group(s) with a touch box behind it, %lld of them decided by the anchor's history and kept (%lld of %lld members); the touch triggers: %lld entered, %lld not entered, undecided: no payload %lld, no x track %lld, not run %lld, toggle block %lld, multi %lld, box rides a mover %lld, unknown %lld, touch Spawn %lld\n",
                                              x, behindGroups, behindDecidable, behindDecidableMovers, behindMovers, behindTrigs[0], behindTrigs[1], behindTrigs[2], behindTrigs[3], behindTrigs[4], behindTrigs[5], behindTrigs[6], behindTrigs[7], behindTrigs[8], behindTrigs[9]);
                supplyUids.clear();
                for (int g : supplyGroups) { const auto mi = base->members.find(g); if (mi != base->members.end()) for (size_t i : mi->second) if (!partialGroups.count(g) || base->coveredObject(i) || (supplyDeco && decoInert.count(base->objs[i].uid))) supplyUids.push_back(base->objs[i].uid); }
                finishSupply();
                if (supplyAll) { std::printf("trigsupply: --trigsupply-all groups after the anchor check:"); for (int g : supplyGroups) std::printf(" %d", g); std::printf("\n"); }
            }
        }
        if (t != lastT) {
            const auto eA = Clock::now();
            endLayer();
            grpEndLayerMs += std::chrono::duration<double, std::milli>(Clock::now() - eA).count();
            const auto cA = Clock::now();
            prev.swap(cur);
            cur.clear();
            grpClearMs += std::chrono::duration<double, std::milli>(Clock::now() - cA).count();
            if (lastT >= 0 && t != lastT + 1) {   // a new search (a rung that starts earlier, or a gap): the last layer's timelines belong to ANOTHER tick -- no parent, the timeline is replayed fresh
                if (++layerJumps <= 20) std::printf("trigshadowlive: layer %lld follows layer %lld: the %zu parent timeline(s) of the earlier search are dropped\n", t, lastT, prev.size());
                prev.clear();
            }
            lastT = t;
            stepMemo.clear();   // (its Lines were `cur`'s, which is `prev` now)
            layerMoved = false;
            layerTake = false;
            if (take && supplyOn() && annT == t && !annKeys.empty()) {   // every announced group has a parent it names (its look key, or else its world), and no parent is named twice
                std::vector<uint64_t> ks; ks.reserve(annKeys.size());
                bool ok = true;
                for (size_t i = 0; i < annKeys.size() && ok; ++i) {
                    const GroupIn in = i < annIn.size() ? annIn[i] : GroupIn{};
                    const uint64_t lk = lookKeyOf(annKeys[i], in);
                    if (prev.find(lk) != prev.end()) ks.push_back(lk);
                    else if (in.hasWorld && prev.find(in.world) != prev.end()) ks.push_back(in.world);
                    else ok = false;
                }
                std::sort(ks.begin(), ks.end());
                if (ok) ok = std::adjacent_find(ks.begin(), ks.end()) == ks.end();
                layerTake = ok;
                if (ok) ++takeLayers; else ++takeCopyLayers;
            }
        }
        else if (singlePath && layerMoved) {   // a second group in a layer whose first took the parent over: the parent is stepped already. The supply is switched off (the model's placement stays) and the log says so
            if (++singleViolations == 1) std::printf("trigsupply: ERROR a second group at layer %lld after the first took its parent timeline over (singlePath): the supply is switched off\n", t);
            supplySaved = supplyGroups;
            supplyGroups.clear();
        }
        const auto g1 = Clock::now();
        ++groupsSeen;
        const GroupIn in = gin; gin = GroupIn{};
        const uint64_t k0 = keyOf(dx, trigWord, frame, rev, fsig, xc);
        if (g_b2WorldOn) { const uint64_t e = gdtrig::wkMix(k0, (uint64_t)t + 0x9e3779b97f4a7c15ull); g_b2WorldSum.fetch_add(e); g_b2WorldSeq.store(gdtrig::wkMix(g_b2WorldSeq.load(), e)); g_b2WorldGroups.fetch_add(1); }
        const uint64_t k = storeKeyOf(k0, in), lk = lookKeyOf(k0, in);
        Line* par = nullptr;
        auto pi = prev.find(lk);
        if (pi == prev.end() && in.hasWorld) { pi = prev.find(in.world); if (pi != prev.end()) ++parentByWorld; }   // the timeline the group's first member was stepped in
        if (pi != prev.end()) par = &pi->second;
        else {
            // A new key whose timeline the search could not name (no GroupIn::world) starts from a copy of a previous layer's timeline. The nearest in x is NOT enough: the states of a layer share the group's x, so the pick was the first in the map's order, and a timeline
            // whose touch history is not this group's (a box entered at another tick, one this group has not entered) carries that history's Moves into the new world (lv22 anchor 5309 t=5928: the group
            // that entered the box on this tick took a parent that had entered it 13 ticks before, and its block was already moving). A parent may only have been told touches this group has, entered at the
            // ticks this group entered them; the nearest in x among those, and the old pick when there is none.
            Line* ok = nullptr;
            for (auto& kv : prev) {
                const Line& L = kv.second;
                bool same = (L.told & ~trigWord) == 0;
                // (the entry tick matters while the box's Moves still run -- the states of one group share those, groupFireSig; a box whose Moves are done may have been entered at another tick by another member)
                if (same && fireB) for (int b = 0; b < kTouchBits && same; ++b) {
                    if (!((L.told >> b) & 1ull) || L.fb[b] == fireB[b]) continue;
                    const long long moving = (size_t)b < g_touchMoveTicks.size() ? (long long)g_touchMoveTicks[(size_t)b] : 0;
                    if (moving > 0 && (t - (long long)fireB[b] < moving || t - (long long)L.fb[b] < moving)) same = false;
                }
                if (same && (!ok || std::fabs(L.x - x) < std::fabs(ok->x - x))) ok = &kv.second;
            }
            if (ok) { par = ok; ++parentOwnHistory; }
            else if (!prev.empty()) {
                ++parentFallback;
                static int shownFb = 0;
                if (shownFb++ < 12) std::printf("trigshadowlive: parent fallback at layer %lld (x %.4f, touch latch %llx): no timeline of layer %lld was told only the touches this group has, entered at the same ticks; the nearest in x is taken (the old pick)\n", t, x, (unsigned long long)trigWord, lastT - 1);
                for (auto& kv : prev) if (!par || std::fabs(kv.second.x - x) < std::fabs(par->x - x)) par = &kv.second;
            }
        }
        const auto g2 = Clock::now();
        actPend = -1;   // (the group being given is the current one again)
        pend.push_back({k, x, par});
        pend.back().dx = dx;
        pend.back().frame = frame;
        pend.back().wy = wy;
        pend.back().wx = wxw;
        if (xcont) { auto& v = seenX[t]; bool has = false; for (const auto& e : v) if (e.first == dx) { has = true; break; } if (!has && v.size() < 8) v.push_back({dx, x}); }
        pend.back().bits = trigWord;
        pend.back().evg = in.evg;
        pend.back().cross = in.cross; pend.back().cross0 = in.cross0; pend.back().cross1 = in.cross1;
        if (fireB) { for (int b = 0; b < kTouchBits; ++b) pend.back().fb[b] = fireB[b]; }
        if (par) { pend.back().hasParentKey = true; pend.back().parentNamed = pi != prev.end(); pend.back().parentKey = (pi != prev.end()) ? pi->first : [&] { for (auto& kv : prev) if (&kv.second == par) return kv.first; return (uint64_t)0; }(); }
        { auto mix = [](uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; };
          pend.back().wk = mix(mix(mix(0x7ULL, trigWord), fsig), (uint64_t)(uint32_t)xc); pend.back().fs = fsig; }
        const auto g3 = Clock::now();
        if (g_camy) camYTickFor(pend.back(), t);   // --camy S2.1: the camera Y's timeline input of this group (a pure function of a copy of the parent's camera state)
        if (supplyOn()) stepNow(pend.back());   // --trigsupply-group: the timeline is needed before model.prepare places this group's movers
        const auto g4 = Clock::now();
        grpTotalMs += std::chrono::duration<double, std::milli>(g4 - gA).count();
        grpPre += std::chrono::duration<double, std::milli>(g1 - gA).count(); grpFind += std::chrono::duration<double, std::milli>(g2 - g1).count();
        grpPush += std::chrono::duration<double, std::milli>(g3 - g2).count(); grpStepNow += std::chrono::duration<double, std::milli>(g4 - g3).count();
    }

    void print(bool full = true) const {   // full: with the long tables (see reportFull)
        if (!on || !layers) return;
        std::printf("trigshadowlive: layers %lld groups %lld (timelines a layer: mean %.2f max %lld) steps %lld, took over %lld, copied %lld (splits and new keys), fresh %lld; "
                    "step %.4f ms each, copy %.3f ms each; snapshot of a timeline (sampled %lld): mean %.1f KB max %.1f KB\n",
                    layers, groupsSeen, (double)sumLines / (double)layers, maxLines, steps, moves, copies, fresh,
                    steps ? stepMs / (double)steps : 0.0, copies + fresh ? copyMs / (double)(copies + fresh) : 0.0,
                    snapSamples, snapSamples ? snapSumKB / (double)snapSamples : 0.0, snapMaxKB);
        if (evFedNodes || evNotAncestor) std::printf("trigshadowlive: game events handed to the timelines: %lld, listener calls %lld; groups whose parent's digest was not on their chain: %lld\n", evFedNodes, evListenerCalls, evNotAncestor);
        if (grpTotalMs > 0) std::printf("trigshadowlive: group() %.0f ms in all (endLayer %.0f ms, dropping the older layer %.0f ms); before the key %.0f, find parent %.0f, push %.0f, stepNow %.0f\n", grpTotalMs, grpEndLayerMs, grpClearMs, grpPre, grpFind, grpPush, grpStepNow);
        if (nowCopyMs + nowStepMs > 0) std::printf("trigshadowlive: stepNow %lld calls: copy+feed %.0f ms, supply table %.0f ms, step %.0f ms, rest %.0f ms; shared-table clones %lld\n", supGroupsDone, nowCopyMs, nowFillMs, nowStepMs, nowTailMs, (long long)gdtrig::g_sharedConstClones.load());
        { static const bool cowStat = std::getenv("COWPAGES_STAT") != nullptr;   // read once (a report only under the env: the lines above are compared between builds as they are)
          if (cowStat) std::printf("trigshadowlive: object pages: %zu a timeline; %lld page-table copies, %lld pages cloned by a write\n", base ? base->objs.pageCount() : (size_t)0, (long long)gdtrig::g_cowPageCopies.load(), (long long)gdtrig::g_cowPageClones.load());
          if (cowStat && !cur.empty() && cur.begin()->second.sh) std::printf("trigshadowlive: a timeline's other copied members (elements): %s\n", cur.begin()->second.sh->copiedSizes().c_str());
          if (cowStat && worldStatOn()) std::printf("trigshadowlive: world hash: %lld steps hashed (before and after), %.0f ms; purity pairs %lld, violations %lld\n", wsSteps, wsHashMs, wsPairs, wsViol); }
        if (events || replay)
            std::printf("trigshadowlive: events %s (step shift %d), replay %s: %lld touch Events given, %lld parentless timelines replayed (%lld steps, %.2f ms), %lld without an x track\n",
                        events ? "on" : "off", evShift, (replay || events) ? "on" : "off", evGiven, evReplays, evReplaySteps, evReplayMs, evNoTrack);
        if (replayReused) std::printf("trigshadowlive: %lld parentless timeline(s) started from the call's first replay instead of replaying again\n", replayReused);
        if (gapReplays) std::printf("trigshadowlive: x-track gap: %lld parentless timeline(s) had ticks between the end of the x track and their layer, walked in a straight line, %lld steps\n", gapReplays, gapSteps);
        if (xcont) std::printf("trigshadowlive: xcont:%lld parentless timeline(s) went on past the x track along the call's own layers, %lld steps\n", xcontReplays, xcontSteps);
        if (camModelOn) std::printf("trigshadowlive: cammodel: %lld timeline-layers with the model camera X certain (the follow formula), %lld uncertain (a Static Camera that moves X or a left / right Camera Edge active: screen Areas not run)\n", camCertain, camUncertain);
        if (camModelOn && camLiveSteps) std::printf("trigshadowlive: cammodel: %lld timeline-steps under a follow Static Camera with a moving target: the camera X placed from the live target on %lld, the camera Y on %lld\n", camLiveSteps, camLivePlaced, camLiveYPlaced);
        if (gdGrpPath)
            std::printf("trigshadowgd: groups on the game's path (x): %lld; with the game's touch latch and entry ticks %lld (%.2f%%); a box the game had not touched (extra) only %lld, a box the game had overlapped that the latch lacks (missing) only %lld, both %lld; the same boxes entered on other ticks %lld\n",
                        gdGrpPath, gdGrpSame, 100.0 * (double)gdGrpSame / (double)gdGrpPath, gdGrpExtraOnly, gdGrpMissingOnly, gdGrpBoth, gdGrpTick);
        if (!coverGroups.empty() && base) {   // --trigshadowcover: every member of each named group, covered or not, whether or not the model placed it in this call
            for (int g : coverGroups) {
                const auto mi = base->members.find(g);
                if (mi == base->members.end()) { std::printf("trigshadowcover: group %d has no members in the shadow\n", g); continue; }
                long long cov = 0, notActed = 0, unc = 0; std::string bad;
                for (size_t i : mi->second) {
                    const bool c = base->coveredObject(i), a = base->actedOnByModelled(i);
                    if (c && a) { ++cov; continue; }
                    if (!c) ++unc; else ++notActed;
                    if (bad.size() < 400) { bad += " " + std::to_string(base->objs[i].uid) + (c ? "(not acted on)" : "(uncovered: " + base->whyUncovered(i) + ")"); }
                }
                std::printf("trigshadowcover: group %d: %zu members, %lld covered and moved, %lld uncovered, %lld covered but not acted on by a modelled trigger%s%s\n", g, mi->second.size(), cov, unc, notActed, bad.empty() ? "" : ":", bad.c_str());
            }
        }
        if (gdN) {
            std::printf("trigshadowgd: row-type census of the model's placed movers against the game's rows: A (x, y of row t) %lld, B (x of row t, y of row t+1) %lld, C (x, y of row t+1) %lld, S (nothing moves between the rows) %lld, N (none) %lld\n", tyCat[0], tyCat[1], tyCat[2], tyCat[3], tyCat[4]);
            std::printf("trigshadowgd: census by shadow cover: covered and moved A %lld B %lld C %lld S %lld N %lld | not covered A %lld B %lld C %lld S %lld N %lld\n", tyCatC[1][0], tyCatC[1][1], tyCatC[1][2], tyCatC[1][3], tyCatC[1][4], tyCatC[0][0], tyCatC[0][1], tyCatC[0][2], tyCatC[0][3], tyCatC[0][4]);
            std::printf("trigshadowgd: off row t by class (B / C / N<=0.05 / N 0.05-1 / N>1): covered %lld %lld %lld %lld %lld, of which the shadow is within 0.01 of row t: %lld %lld %lld %lld %lld, within 0.05: %lld %lld %lld %lld %lld; not covered %lld %lld %lld %lld %lld\n",
                        tyBandC[1][0], tyBandC[1][1], tyBandC[1][2], tyBandC[1][3], tyBandC[1][4], tyShOk01[0], tyShOk01[1], tyShOk01[2], tyShOk01[3], tyShOk01[4], tyShOk05[0], tyShOk05[1], tyShOk05[2], tyShOk05[3], tyShOk05[4], tyBandC[0][0], tyBandC[0][1], tyBandC[0][2], tyBandC[0][3], tyBandC[0][4]);
            std::printf("trigshadowgd: N bands (|model - row t| 0.01-0.05 / 0.05-1 / > 1 px): %lld / %lld / %lld; against the nearer of rows t and t+1: %lld / %lld / %lld\n", tyBand[0], tyBand[1], tyBand[2], tyBand[3], tyBand[4], tyBand[5]);
            if (const char* tp = std::getenv("TSGD_TYPEOUT")) { std::ofstream tf(tp); tf << "uid,A,B,C,S,N,n1,n2,n3,m1,m2,m3,max_d0_milli,sum_d0_milli,covered,shadow_far,shadow_far_max_milli,both_far\n"; for (const auto& kv : tyUid) { tf << kv.first; for (int k = 0; k < 17; ++k) tf << ',' << kv.second[(size_t)k]; tf << '\n'; } }
            std::printf("trigshadowgd: %lld movers on the game's path held against the game's own rows: the model within 0.01 for %lld (%.2f%%), the shadow within 0.01 for %lld (%.2f%%), both %lld; shadow off: %lld (%lld skipped: the group's touch latch is not the game's)\n",
                        gdN, gdModelOk, 100.0 * (double)gdModelOk / (double)gdN, gdShadowOk, 100.0 * (double)gdShadowOk / (double)gdN, gdBoth, gdOff, gdMaskSkip);
            std::printf("trigshadowgd: %lld further mover-layers left out: the game's row has its centre below y = 0 (getObjectRect pulls such a rect's top edge to 0, so the row is not the place; TSGD_KEEPNEG=1 keeps them)\n", gdClampSkip);
            std::vector<std::pair<int, std::pair<long long, long long>>> v(gdUidBad.begin(), gdUidBad.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first != b.second.first ? a.second.first > b.second.first : a.first < b.first; });
            for (size_t i = 0; i < v.size() && i < 8 && v[i].second.first > 0; ++i)
                std::printf("trigshadowgd:   uid %d: the shadow is off the game's row %lld of %lld times\n", v[i].first, v[i].second.first, v[i].second.second);
        }
        if (gdGrpWanted >= 0) {   // TSGD_GROUP: the layers where every compared mover of the group agrees with the game, as ranges (a gap of up to 3 layers without a compared mover does not break a range)
            long long ln = 0, ls = 0, lm = 0;
            for (const auto& kv : gdLay) { ln += kv.second[0]; ls += kv.second[1]; lm += kv.second[2]; }
            std::printf("trigshadowgd: group %d (%zu members): %zu layers with a compared member, %lld member-layers: the shadow within 0.01 of the game for %lld (%.2f%%), the model for %lld (%.2f%%); %lld group-layers skipped (the group's touch latch is not the game's)\n",
                        gdGrpWanted, gdGrpMembers.size(), gdLay.size(), ln, ls, ln ? 100.0 * (double)ls / (double)ln : 0.0, lm, ln ? 100.0 * (double)lm / (double)ln : 0.0, gdMaskSkip);
            for (int who = 1; who <= 2; ++who) {
                std::printf("trigshadowgd:   layers where every compared member agrees with the game, %s:", who == 1 ? "shadow" : "model");
                long long s0 = -1, e0 = -1; int shown = 0;
                for (const auto& kv : gdLay) {
                    const bool ok = kv.second[(size_t)who] == kv.second[0];
                    if (ok) { if (s0 < 0) s0 = kv.first; else if (kv.first > e0 + 3) { if (shown++ < 14) std::printf(" %lld-%lld", s0, e0); s0 = kv.first; } e0 = kv.first; }
                    else if (s0 >= 0) { if (shown++ < 14) std::printf(" %lld-%lld", s0, e0); s0 = -1; }
                }
                if (s0 >= 0 && shown++ < 14) std::printf(" %lld-%lld", s0, e0);
                std::printf("%s\n", shown >= 14 ? " ..." : "");
            }
        }
        if (checkEvery > 0 && chkLayers) {
            std::printf("trigshadowcheck: every %d layer(s): %lld layers compared, %lld group snapshots, %lld movers compared (%lld not in the shadow, %lld not covered or not moved by it); "
                        "model = shadow at tick t for %lld (%.2f%%), at tick t-1 for %lld (%.2f%%), neither for %lld (%.2f%%); |diff at t| <=0.01 %lld, <=0.5 %lld, <=2 %lld, <=20 %lld, more %lld; "
                        "cost per compared layer: the model's movers walked %.4f ms + the shadow's before-place %.4f ms + compare %.4f ms\n",
                        checkEvery, chkLayers, chkGroups, chkObjs, chkNotInShadow, chkUncovered,
                        chkAtT, chkObjs ? 100.0 * (double)chkAtT / (double)chkObjs : 0.0, chkAtPrev, chkObjs ? 100.0 * (double)chkAtPrev / (double)chkObjs : 0.0,
                        chkNone, chkObjs ? 100.0 * (double)chkNone / (double)chkObjs : 0.0,
                        chkHist[0], chkHist[1], chkHist[2], chkHist[3], chkHist[4], modelSnapMs / (double)chkLayers, snapMs / (double)chkLayers, (chkMs - snapMs) / (double)chkLayers);
            for (const auto& wkv : watch) {   // --trigshadowuid
                const WatchStat& w = wkv.second;
                const auto us = uidStat.find(wkv.first);
                const auto gb = gdUidBad.find(wkv.first);
                std::printf("trigshadowcheck: watch uid %d: model placed it as a mover in %lld compared layers; shadow does not know it %lld, does not cover / move it %lld; compared %lld (at t %lld, at t-1 %lld, neither %lld, max |diff| %.3f)%s",
                            wkv.first, w.snaps, w.notIn, w.uncov, us == uidStat.end() ? 0LL : us->second.n, us == uidStat.end() ? 0LL : us->second.atT, us == uidStat.end() ? 0LL : us->second.atPrev,
                            us == uidStat.end() ? 0LL : us->second.none, us == uidStat.end() ? 0.0 : us->second.maxD, w.firstNoneT >= 0 ? "" : "; ");
                if (w.firstNoneT >= 0) std::printf("; first layer matching neither shadow tick: t=%lld (|diff| %.3f); ", w.firstNoneT, w.firstNoneD);
                if (gb != gdUidBad.end()) std::printf("against the game's rows: the shadow is off %lld of %lld times\n", gb->second.first, gb->second.second);
                else std::printf("against the game's rows: never compared (%s)\n", gdDir.empty() ? "no --trigshadowgd" : "no layer of the group on the game's path");
            }
            std::vector<std::pair<int, UidStat>> v(uidStat.begin(), uidStat.end());
            std::sort(v.begin(), v.end(), [](const std::pair<int, UidStat>& a, const std::pair<int, UidStat>& b) { return a.second.none != b.second.none ? a.second.none > b.second.none : a.first < b.first; });
            for (size_t i = 0; i < v.size() && i < 14; ++i)
                std::printf("trigshadowcheck:   uid %d: %lld compared, %lld at t, %lld at t-1, %lld neither (max |diff| %.3f, mean %.4f)\n", v[i].first, v[i].second.n, v[i].second.atT, v[i].second.atPrev,
                            v[i].second.none, v[i].second.maxD, v[i].second.n ? v[i].second.sumD / (double)v[i].second.n : 0.0);
        }
        if (wLayers) {
            std::printf("trigshadowlive: worlds: %lld layers; a layer holds (mean / max) %.2f / %lld groups (full keys), %.2f / %lld worlds (touch mask + fire signature + xc), %.2f / %lld fire signatures; %.1f parent states stepped (summed over the groups)\n",
                        wLayers, (double)wKeys / (double)wLayers, maxLines, (double)wWorlds / (double)wLayers, wMaxWorlds, (double)wSigs / (double)wLayers, wMaxSigs, (double)wParents / (double)wLayers);
            if (!full) std::printf("trigshadowlive: worlds: %zu buckets of 2000 ticks (one line a bucket in the last report or with --trigshadowreportfull)\n", buckets.size());
            for (const auto& kv : buckets) {
                if (!full) break;
                const Bucket& b = kv.second;
                std::printf("trigshadowlive: worlds bucket t=%lld..%lld: %lld layers, groups %.2f (max %lld), worlds %.2f (max %lld), signatures %.2f (max %lld), parents %.1f\n",
                            kv.first * 2000, kv.first * 2000 + 1999, b.layers, (double)b.keys / (double)b.layers, b.maxKeys, (double)b.worlds / (double)b.layers, b.maxWorlds,
                            (double)b.sigs / (double)b.layers, b.maxSigs, (double)b.parents / (double)b.layers);
            }
            std::printf("trigshadowlive: worlds a layer, histogram:");
            for (const auto& kv : worldHist) std::printf(" %lld:%lld", kv.first, kv.second);
            std::printf("\n");
        }
        wPrint();
        supplyPrint(full);
        std::fflush(stdout);
    }
    ~TrigLive() { if (on) { endLayer(); print(); } }
};
inline TrigLive g_trigLive;
// --trigsupply-all (T29): dynamics.hpp's g_supplyOnHook. True (and `on` set) only for an object of a supplied group in the current group's table.
inline bool supplyOnHookFn(int uid, int& on) {
    bool b = true;
    if (!g_trigLive.supplyOn() || !g_trigLive.onAt(uid, b)) return false;
    on = b ? 1 : 0;
    return true;
}

}  // namespace dp
