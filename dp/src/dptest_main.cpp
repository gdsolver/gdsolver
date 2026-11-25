// dptest -- the header core's rules that can be decided without a level.
//
// The suite (py/quick_regress.py) compares whole sections against a corpus, which is the right
// instrument for a change that moves the model -- and the wrong one for a boundary that the
// corpus happens not to contain. The recording overlay is such a boundary: the corpus'
// recordings predate the `end` trailer, so quick_regress cannot see the rule at all.
//
//   dptest            runs every case, prints one line each, exit 1 on the first failure
//   dptest --groups-corpus <file>...
//                     reads each --groups recording with parseGroupTimeline and with the sscanf
//                     reader it replaced, and says whether they agree bit for bit
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "dp/trigevent_camera.hpp"
#include "dp/trigevent_orb.hpp"
#include "dp/trigevent_tap.hpp"
#include "dp/state_serial.hpp"
#include "dp/field_check.hpp"
#include "dp/trigevent_togload.hpp"
#include "dp/trigevent_world.hpp"
#include "dp/cli.hpp"
#include "dp/band_rows.hpp"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail = 1;
}

std::string writeTmp(const std::string& name, const std::string& body) {
    const std::string p =
        (std::filesystem::temp_directory_path() / name).string();
    std::ofstream f(p, std::ios::trunc);
    f << body;
    return p;
}

// Does the merged timeline hold a row at this tick with this cy?
bool has(const dp::GroupTimeline& g, int uid, int t, float cy) {
    const auto it = g.find(uid);
    if (it == g.end()) return false;
    for (const dp::DynSample& s : it->second)
        if (s.t == t && s.cy == cy) return true;
    return false;
}

// THE OVERLAY'S REACH. `over` is the newer recording: it wins for every tick its RUN covered,
// which is its `end` trailer when it has one and its last row when it does not. The rows here
// are far enough apart that the bootstrap re-timing finds no fit, so the base keeps its own
// ticks and the only thing under test is the reach.
void overlayReach() {
    const std::string over = writeTmp("dptest_over.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "1,7,0,100,30,3,1,0\n"
        "10,7,0,200,30,3,1,0\n"
        "end,50\n");
    const std::string base = writeTmp("dptest_base.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "5,7,0,300,30,3,1,0\n"
        "20,7,0,400,30,3,1,0\n"
        "50,7,0,500,30,3,1,0\n"
        "60,7,0,600,30,3,1,0\n");

    long long end = -1;
    dp::GroupTimeline o = dp::loadGroupTimeline(over, &end);
    check(end == 50, "the `end` trailer is read back as the run's end");
    check(o[7].size() == 2, "the trailer is not taken for a row");

    dp::GroupTimeline g = dp::loadGroupTimeline(base);
    dp::overlayGroupTimeline(g, o, false, end);
    check(has(g, 7, 10, 200.f), "the newer recording's own rows are kept");
    check(!has(g, 7, 20, 400.f),
          "a base row after the newer file's last row but inside its run is dropped");
    check(!has(g, 7, 50, 500.f), "a base row at t == end is dropped (the run covers it)");
    check(has(g, 7, 60, 600.f), "a base row past the end is kept");

    long long none = -1;
    dp::GroupTimeline o2 = dp::loadGroupTimeline(writeTmp("dptest_over2.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "1,7,0,100,30,3,1,0\n"
        "10,7,0,200,30,3,1,0\n"), &none);
    check(none == -1, "a recording without the trailer says so");
    dp::GroupTimeline g2 = dp::loadGroupTimeline(base);
    dp::overlayGroupTimeline(g2, o2, false, none);
    check(has(g2, 7, 20, 400.f) && has(g2, 7, 50, 500.f) && has(g2, 7, 60, 600.f),
          "without a trailer the reach is the last row, as before");
}

// THE t=0 SNAPSHOT. The mod writes `init,<uid>,<on>` right after the game's reset; the reader
// hands them out on request and otherwise skips them, and they are never taken for a row.
void initLines() {
    const std::string p = writeTmp("dptest_init.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "init,7,1\n"
        "init,8,0\n"
        "1,7,0,100,30,3,0,0\n"
        "5,8,0,200,30,3,1,0\n"
        "end,9\n");
    std::unordered_map<int, uint8_t> init;
    long long end = -1;
    const dp::GroupTimeline g = dp::loadGroupTimeline(p, &end, &init);
    check(init.size() == 2 && init.at(7) == 1 && init.at(8) == 0,
          "each init line gives its object's on at t=0");
    check(g.at(7).size() == 1 && g.at(8).size() == 1 && end == 9,
          "the init lines are not rows, and the trailer still reads");
    const dp::GroupTimeline g0 = dp::loadGroupTimeline(p);
    check(g0.at(7).size() == 1 && g0.at(8).size() == 1,
          "a caller that does not ask gets the same rows as before");
}

// The objrects header as the mod writes it (src/solver/solver.hpp). `nocol` is the 50th column,
// so a fixture that put it anywhere else would test a layout no dump has.
const char* const kObjHeader =
    "id,type,cx,cy,w,h,groups,uid,radius,rot,sy0,sy1,shz,sdir,sup,w0,h0,"
    "tpy,tpg,tpix,tpiy,tw,zoom,zdur,zease,zrate,mvdir,gnddir,"
    "optp1,optp2,flipx,flipy,nofx,notouch,tpex,tpey,dis,"
    "editvel,vmodx,vmody,ovrvel,force,free,touch,spawn,chan,axis,exstat,rev,"
    "nocol";

// One 30x30 static solid. Every column past `rot` is empty (what an older dump leaves there)
// except `nocol`, which is written when `withNocol` says the dump has that column at all.
std::string solidRow(int id, int uid, double cx, bool withNocol, const char* nocol) {
    std::string r = std::to_string(id) + ",0," + std::to_string(cx) + ",100,30,30,0,"
                    + std::to_string(uid) + ",0,0";
    if (withNocol) {
        for (int i = 10; i < 49; ++i) r += ",";
        r += ",";
        r += nocol;
    }
    return r + "\n";
}

bool keeps(const dp::Level& L, int uid) {
    for (const dp::Obj& o : L.objs)
        if (o.uid == uid) return true;
    return false;
}

// WHICH OBJECTS THE MODEL DOES NOT COLLIDE WITH. GD skips an object whose byte [obj+0x515] is
// set, whatever its id; the model used to stand in for that byte with id 1910. With the `nocol`
// column the byte decides, and without it the id rule stands. The corpus cannot test the new
// branch: every stored dump predates the column, so all 1,116 sections take the fallback.
void noCollideColumn() {
    // g_noColStatic (constants.hpp, default on) keeps a static solid whose byte is set as a one-way top (oneway = 2) instead of dropping it, so the checks below -- which are about what the
    // column and the id rule decide -- run with it off, and the default is checked on its own at the end.
    const bool savedNoColStatic = dp::g_noColStatic;
    dp::g_noColStatic = false;
    {
        std::istringstream in(std::string(kObjHeader) + "\n"
                              + solidRow(1910, 11, 100, true, "0")
                              + solidRow(1, 12, 200, true, "1")
                              + solidRow(1, 13, 300, true, "0")
                              + solidRow(1910, 14, 400, true, ""));
        const dp::Level L = dp::loadLevelFrom(in);
        check(keeps(L, 11), "with the column, an id-1910 object whose byte is 0 stays solid");
        check(!keeps(L, 12), "with the column, an id-1 object whose byte is 1 is dropped");
        check(keeps(L, 13), "with the column, an id-1 object whose byte is 0 stays solid");
        check(!keeps(L, 14), "an empty cell falls back to the id rule for that row");
    }
    {
        std::istringstream in(std::string(kObjHeader).substr(0, std::string(kObjHeader).size()
                                                                    - std::string(",nocol").size())
                              + "\n"
                              + solidRow(1910, 21, 100, false, "")
                              + solidRow(1, 22, 200, false, ""));
        const dp::Level L = dp::loadLevelFrom(in);
        check(!keeps(L, 21), "without the column, id 1910 is dropped as before");
        check(keeps(L, 22), "without the column, any other id stays solid");
    }
    dp::g_noColStatic = true;   // the default: a flagged static solid is kept, as a one-way top
    {
        std::istringstream in(std::string(kObjHeader) + "\n"
                              + solidRow(1, 32, 200, true, "1")
                              + solidRow(1, 33, 300, true, "0"));
        const dp::Level L = dp::loadLevelFrom(in);
        int oneway32 = -1, oneway33 = -1;
        for (const dp::Obj& o : L.objs) { if (o.uid == 32) oneway32 = o.oneway; if (o.uid == 33) oneway33 = o.oneway; }
        check(oneway32 == 2, "with --nocolstatic (the default), a static solid whose byte is 1 is kept as a one-way top (oneway = 2)");
        check(oneway33 == 0, "...and one whose byte is 0 is an ordinary solid");
    }
    dp::g_noColStatic = savedNoColStatic;
}

// THE LEVEL TEXT'S PASS, KEPT FROM CALL TO CALL (CsvRows, level_loader.hpp). A call used to read the whole text into
// rows, take each row's uid and place, drop GD's anti-cheat spike row and write the rest out again before it read a
// row; it now reads its rows from the text a first call has made that pass over. The same level through both must be
// the same level, and the rows a call is handed the rows the old pass would have handed it.
void keptTextPass() {
    // (1) The rows. getline over the text the old pass wrote (every kept row, '\n' ended) against nextCsvRow over the
    // kept pass's text: a last row without a newline, empty rows, a '\r' before the '\n', the empty text.
    const char* const texts[] = {"", "h", "h\n", "h\nr1", "h\nr1\n", "h\n\nr1\n\n", "h\r\nr1\r\n", "a\nb\n\nc", "\n", "\n\n"};
    for (const char* t : texts) {
        const std::string text = t;
        std::vector<std::string> rows;
        {
            std::istringstream in(text);
            std::string row;
            while (std::getline(in, row)) rows.push_back(row);
        }
        std::string all;
        for (const std::string& r : rows) { all += r; all.push_back('\n'); }
        std::vector<std::string> slow;
        {
            std::istringstream in(all);
            std::string row;
            while (std::getline(in, row)) slow.push_back(row);
        }
        const dp::CsvRows kept = dp::buildCsvRows(text);
        const std::string_view src = kept.noDrop ? std::string_view(text) : std::string_view(kept.filtered);
        std::vector<std::string> fast;
        size_t pos = 0;
        std::string row;
        while (dp::nextCsvRow(src, pos, row)) fast.push_back(row);
        check(fast == slow, "the kept pass hands a call the rows the old pass wrote, edge texts included");
    }
    // (2) A level with the anti-cheat spike (the highest uid, id 8 at (0,105), 6x12, 30x30 unscaled), a row with too
    // few fields for the uid pass, and solids: dropped, skipped, kept.
    auto joined = [](const std::vector<std::string>& f) {
        std::string s;
        for (size_t i = 0; i < f.size(); ++i) s += (i ? "," : "") + f[i];
        return s;
    };
    std::vector<std::string> spike(17, "");
    spike[0] = "8"; spike[1] = "0"; spike[2] = "0"; spike[3] = "105"; spike[4] = "6"; spike[5] = "12";
    spike[6] = "0"; spike[7] = "99"; spike[8] = "0"; spike[9] = "0"; spike[15] = "30"; spike[16] = "30";
    const std::string text = std::string(kObjHeader) + "\n" + solidRow(1, 11, 100, false, "") + "1,0,50,100\n"
                             + solidRow(1, 12, 200, false, "") + solidRow(1, 13, 300, false, "") + joined(spike) + "\n";
    const dp::CsvRows kept = dp::buildCsvRows(text);
    check(!kept.noDrop && kept.maxUid == 99, "the spike is the highest uid and is dropped");
    check(kept.places.size() == 4 && kept.places[0].uid == 11 && kept.places[0].cx == 100.0
              && kept.places[1].uid == 12 && kept.places[3].uid == 99,
          "the places are the rows with eight fields or more, the short one left out");
    check(kept.filtered.find(joined(spike)) == std::string::npos && kept.filtered.find("\n1,0,50,100\n") != std::string::npos,
          "the filtered text has no spike row and keeps the short one");
    std::istringstream in(text);
    const dp::Level slowL = dp::loadLevelFrom(in);
    std::istringstream none;
    const dp::Level fastL = dp::loadLevelFrom(none, nullptr, nullptr, nullptr, &kept, &text);
    bool same = slowL.objs.size() == fastL.objs.size();
    for (size_t i = 0; same && i < slowL.objs.size(); ++i) {
        const dp::Obj& a = slowL.objs[i];
        const dp::Obj& b = fastL.objs[i];
        same = a.uid == b.uid && a.cx == b.cx && a.cy == b.cy && a.hw == b.hw && a.hh == b.hh && a.id == b.id;
    }
    check(same && keeps(fastL, 11) && keeps(fastL, 13) && !keeps(fastL, 99),
          "a level read from the kept pass is the level the old pass gave, spike gone");
    // (3) No spike at all: the text itself is what the rows are read from.
    const std::string plain = std::string(kObjHeader) + "\n" + solidRow(1, 11, 100, false, "");
    check(dp::buildCsvRows(plain).noDrop, "nothing dropped: the kept pass keeps no copy of the text");
}

// AN OBJECT'S ON/OFF AS A SEQUENCE OF SWITCHES (five boundaries). A touch mask
// cannot carry any of these -- it only ever gains bits -- so each case is one the mask form gets
// wrong.
void onOffEvents() {
    uint16_t fireB[dp::kTouchBits] = {};
    auto on = [&](bool init, const std::vector<dp::OnEvent>& ev, dp::TouchMask mask, int t) {
        return dp::resolveOn(init, ev.data(), ev.size(), mask, fireB, t);
    };
    auto boxEv = [](int box, bool isOn, double delay) {
        dp::OnEvent e;
        e.box = box;
        e.on = isOn ? 1 : 0;
        e.delay = delay;
        return e;
    };
    const dp::TouchMask b0 = dp::touchBit(0), b1 = dp::touchBit(1);

    // (1) a spawn delay: the switch lands `delay` ticks after the box, not with it
    fireB[0] = 100;
    const std::vector<dp::OnEvent> delayed{boxEv(0, true, 10.0)};
    check(!on(false, delayed, b0, 109), "(1) one tick before the delay runs out it is still off");
    check(on(false, delayed, b0, 110), "(1) on the tick the delay runs out it is on");
    check(!on(false, delayed, dp::TouchMask{}, 500), "(1) a state that never entered the box: off");

    // (2) a Toggle that switches OFF (which a mask cannot say at all)
    const std::vector<dp::OnEvent> off{boxEv(0, false, 0.0)};
    check(on(true, off, b0, 99), "(2) before the box it is on");
    check(!on(true, off, b0, 100), "(2) from the box on it is off");

    // (3) two chains on one object: the later switch wins, whichever box it came from
    fireB[0] = 100;
    fireB[1] = 150;
    const std::vector<dp::OnEvent> onThenOff{boxEv(0, true, 0.0), boxEv(1, false, 0.0)};
    check(on(false, onThenOff, b0 | b1, 120), "(3) on at 100, off at 150: on at 120");
    check(!on(false, onThenOff, b0 | b1, 160), "(3) on at 100, off at 150: off at 160");
    const std::vector<dp::OnEvent> offThenOn{boxEv(0, false, 0.0), boxEv(1, true, 0.0)};
    check(on(true, offThenOn, b0 | b1, 160), "(3) off at 100, on at 150: on at 160");
    // ...and it is the EFFECTIVE tick that orders them: the box entered first can land last
    const std::vector<dp::OnEvent> slowFirst{boxEv(0, true, 100.0), boxEv(1, false, 0.0)};
    check(!on(false, slowFirst, b0 | b1, 190), "(3) off lands at 150, on at 200: off at 190");
    check(on(false, slowFirst, b0 | b1, 210), "(3) off lands at 150, on at 200: on at 210");
    // ...and a box the state did not enter takes no part
    check(on(false, onThenOff, b0, 160), "(3) without the second box it stays on");

    // (4) and (5): which roots give switches at all. A Tap and a Count set their bit on the
    // first press or item, so their chain's Toggle cannot be read off the bit; an Item Compare
    // (a Count root with cmode 3) has its own gate for coins in cli.hpp.
    std::vector<dp::TouchTrig> boxes(4);
    for (dp::TouchTrig& T : boxes) T.togEv.push_back({42, 1, 0.0});
    boxes[1].tap = true;
    boxes[2].count = 3;
    boxes[3].count = 6;
    boxes[3].cmode = 3;
    const auto ev = dp::boxOnEvents(boxes);
    const auto it = ev.find(42);
    check(it != ev.end() && it->second.size() == 1 && it->second[0].box == 0,
          "(4)(5) only the plain box gives a switch -- not the Tap, the Count or the Item Compare");
}

// A pickup row as the mod dumps it (SubZero 4002's key uid 6166, with uid / x / target / actgrp
// swapped in). actgrp is the 41st column.
std::string pickupRow(int uid, double cx, int target, int actgrp, int id = 1275,
                      int touch = 0) {
    return std::to_string(uid) + "," + std::to_string(id) + "," + std::to_string(cx)
           + ",985,25,20," + std::to_string(target) + ",0," + std::to_string(touch)
           + ",0,0.5,0,0,0,2,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,-,0,0,-1,0,"
           + std::to_string(actgrp) + ",0,0,0,-1,-1,-1,-1,0,0,-1,-1,-1,0,-1,-1,-1,-1\n";
}

const dp::TouchTrig* boxOf(const std::vector<dp::TouchTrig>& v, int uid) {
    for (const dp::TouchTrig& t : v)
        if (t.uid == uid) return &t;
    return nullptr;
}

// WHICH PICKUPS BECOME BOXES (--activators). Only one that switches its group ON, and only when
// everything in the group is something to stand on, collect or press: custom levels have pickups
// that bring hazards in or take coins away, and those are left out rather than guessed at.
void activatorRoots() {
    const std::string trig = writeTmp("dptest_act_trig.txt",
        "uid,id,cx,cy,w,h,target,center,touch,spawn,dur,ox,oy,ease,erate,lockx,locky,grav,"
        "gravmod,deg,ord,chan,sord,sordd,t360,lockrot,sdelay,mvtgt,mvaxis,tmodctr,dirsnap,"
        "dirdist,dynmode,silent,togon,remap,item,item2,count,subcount,actgrp,thold,ttog,tdual,"
        "cmode,i1mode,i2mode,tgtmode,mod1,mod2,res1,res2,res3,tol,rnd1,rnd2,sgn1,sgn2\n"
        + pickupRow(101, 1000, 10, 1)    // two solids
        + pickupRow(102, 2000, 20, 1)    // a solid and a hazard
        + pickupRow(103, 3000, 30, 1)    // an orb
        + pickupRow(104, 4000, 40, 0)    // switches OFF
        + pickupRow(105, 5000, 50, 1, 1594, 1));   // a toggle block (the dump says touch=1)
    const std::string grp = writeTmp("dptest_act_grp.txt",
        "uid,groups\n1001 10\n1002 10\n2001 20\n2002 20\n3001 30\n4001 40\n5001 50\n");
    const std::unordered_map<int, int> types{
        {101, 30}, {102, 30}, {103, 30}, {104, 30}, {105, 36},
        {1001, 0}, {1002, 0}, {2001, 0}, {2002, 2}, {3001, 35}, {4001, 0}, {5001, 35}};

    const auto on = dp::loadTouchTriggers(trig, grp, -1e18, &types);
    const dp::TouchTrig* k = boxOf(on, 101);
    bool both = k && k->activator && k->togEv.size() == 2;
    for (size_t i = 0; both && i < k->togEv.size(); ++i)
        both = k->togEv[i].on == 1 && k->togEv[i].delay == 0.0
               && (k->togEv[i].uid == 1001 || k->togEv[i].uid == 1002);
    check(both, "a pickup that switches two solids on is a box that switches both on");
    check(k && k->cx == 1000 && k->hw == 12.5 && k->hh == 10.0,
          "its box is the pickup's own rect");
    check(!boxOf(on, 102), "a pickup whose group holds a hazard is left out");
    check(boxOf(on, 103) != nullptr, "a pickup that switches an orb on is kept");
    check(!boxOf(on, 104), "a pickup that switches its group OFF is left out");
    const dp::TouchTrig* tb = boxOf(on, 105);
    check(tb && tb->activator && tb->press && !k->press,
          "a toggle block is an activator that needs the button; a pickup does not");
}

// A trigger row for the chain walks: the columns up to togon matter here (sdelay is the 27th,
// togon the 35th).
std::string trigRow(int uid, int id, double cx, int target, int spawn, int togon, double sdelay,
                    double ox) {
    std::string r = std::to_string(uid) + "," + std::to_string(id) + "," + std::to_string(cx)
                    + ",315,30,30," + std::to_string(target) + ",0,0," + std::to_string(spawn)
                    + ",0.5," + std::to_string(ox) + ",0,0,2,0,0,1,0,0,0,0,0,0,0,0,"
                    + std::to_string(sdelay) + ",0,0,0,0,0,0,0," + std::to_string(togon)
                    + ",-,0,0,-1,0,-1,0,0,0,-1,-1,-1,-1,0,0,-1,-1,-1,0,-1,-1,-1,-1\n";
    return r;
}

// THE x-CROSSING SWITCHES, and the whole of SubZero 4003's hidden ring through the resolver: on
// at the start, off when the player crosses x=6,225 (Toggle uid 3027), on again when the box
// (id 1594) fires. The crossing's tick is a level property the search resolves as it goes, so the
// event must read it then, not when it was built.
void autoSwitches() {
    const std::string trig = writeTmp("dptest_auto_trig.txt",
        "uid,id,cx,cy,w,h,target,center,touch,spawn,dur,ox,oy,ease,erate,lockx,locky,grav,"
        "gravmod,deg,ord,chan,sord,sordd,t360,lockrot,sdelay,mvtgt,mvaxis,tmodctr,dirsnap,"
        "dirdist,dynmode,silent,togon,remap,item,item2,count,subcount,actgrp,thold,ttog,tdual,"
        "cmode,i1mode,i2mode,tgtmode,mod1,mod2,res1,res2,res3,tol,rnd1,rnd2,sgn1,sgn2\n"
        + trigRow(3027, 1049, 6225, 145, 0, 0, 0.0, 0.0)     // x-crossing Toggle, off
        + trigRow(900, 901, 9000, 50, 0, -1, 0.0, 30.0)      // x-crossing Move, whose group
        + trigRow(910, 1049, 9001, 60, 1, 1, 0.5, 0.0));     // holds a spawn-only Toggle
    const std::string grp = writeTmp("dptest_auto_grp.txt",
        "uid,groups\n4256 145\n500 50\n910 50\n600 60\n");
    std::vector<dp::AutoTrig> autos = dp::loadAutoTriggers(trig, grp);
    std::sort(autos.begin(), autos.end(),
              [](const dp::AutoTrig& a, const dp::AutoTrig& b) { return a.cx < b.cx; });
    const auto ev = dp::autoOnEvents(autos);
    const auto ring = ev.find(4256);
    check(ring != ev.end() && ring->second.size() == 1 && ring->second[0].on == 0
              && ring->second[0].autoIdx == 0 && ring->second[0].delay == 0.0,
          "an x-crossing Toggle switches its group off, on its own crossing");
    // A Move root spawns nothing: the spawn-triggered Toggle in its target group is carried as an
    // object and waits for a Spawn or a box (triggers.hpp, the Move-root rule in loadAutoTriggers,
    // measured on a custom level's group 171). This check used to expect the Toggle's switch 120
    // ticks after the crossing, the rule that measurement overturned.
    check(ev.find(600) == ev.end(),
          "a Move a crossing fires does not spawn the Toggle in its target group");

    // The ring: the crossing's switch plus the box's, resolved the way the model will.
    std::vector<dp::OnEvent> r = ring->second;
    dp::OnEvent box;
    box.box = 0;
    box.on = 1;
    r.push_back(box);
    uint16_t fireB[dp::kTouchBits] = {};
    fireB[0] = 7750;
    const dp::TouchMask b0 = dp::touchBit(0);
    dp::g_autoTrig = autos;
    auto on = [&](dp::TouchMask m, int t) {
        return dp::resolveOn(true, r.data(), r.size(), m, fireB, t);
    };
    check(on(b0, 7000), "before the crossing is resolved the ring is as the reset left it (on)");
    dp::g_autoTrig[0].fireT = 6995;
    check(!on(b0, 7000), "once the crossing is at 6,995 the ring is off after it");
    check(on(b0, 6994), "...and still on the tick before");
    check(!on(dp::TouchMask{}, 8000), "a state that never pressed in the box: off for good");
    check(on(b0, 7760) && !on(b0, 7749), "a state whose box fired at 7,750: on from then");
    dp::g_autoTrig.clear();
}

// THE KEY HOLDS A DELAYED SWITCH. Two states with the same mask that fired a
// box at different ticks are in different worlds until the later one's switch has landed, so
// the dedupe key must keep them apart for that long (and, since the world key is on for good, it keeps them apart afterwards too).
void keyHoldsDelayedSwitch() {
    const std::vector<dp::TouchTrig> saved = dp::g_touch;
    dp::g_touch.assign(1, dp::TouchTrig{});
    dp::g_touch[0].togEv.push_back({42, 1, 100.0});   // on, 100 ticks after the box
    dp::State a{};
    a.trig = dp::touchBit(0);
    a.fireB[0] = 100;
    dp::State b = a;
    b.fireB[0] = 150;
    // a's switch lands at 200, b's at 250: at 220 one world has it and the other does not.
    dp::buildTouchMoveTicks();
    check(dp::keyOf(a, 220) != dp::keyOf(b, 220),
          "while the later switch has not landed, the two states keep different keys");
    // since --worldkey-dedupe went on for good (rm-worldkey 941933ce, restructure draft 7.11 P5-2) the whole touch history (worldKeyOf: trig mask and fireB) stays in the key: two states that fired
    // the box at different ticks never merge again, even once both switches have landed (this check used to expect the merge; search_key.hpp modelKeyOf ends with `k ^= worldKeyOf(s, false, true)`)
    check(dp::keyOf(a, 400) != dp::keyOf(b, 400),
          "once both have landed they still differ: the key holds the whole touch history");
    dp::g_touch = saved;
    dp::buildTouchMoveTicks();
}


// --layerdigest v1 IS A PROJECTION OF THE STATE, NOT THE STATE (audit AUD-856). The dedupe key rounds the second body's y2 / vy2 to a bin, so two states that differ only in y2 inside
// one bin have the same key and the same fingerprint -- a layer made of them has the same layer digest. A matching layer digest means "the projections agree", not "the states are
// bit for bit the same". The fixture pins both: the pair that is NOT told apart (and says why), and the pairs that are.
void layerDigestIsAProjection() {
    dp::State a{};
    a.dual = 1;
    a.y = 150.f; a.vy = 2.f; a.xAbs = 500.f;
    a.y2 = 100.1f; a.vy2 = 1.f;
    dp::State inBin = a;  inBin.y2 = 100.1001f;             // y2 changed, same bin
    dp::State farBin = a; farBin.y2 = 150.f;                // y2 changed, another bin
    dp::State fineY = a;  fineY.y = 150.0001f;              // y changed at full float precision
    dp::State lineage = a; lineage.parent = 77;             // the lineage is not part of the projection
    const long long t = 1000;
    auto fp = [t](const dp::State& s) { return dp::layerStateFingerprint(s, dp::keyOf(s, t)); };
    check(dp::keyOf(a, t) == dp::keyOf(inBin, t), "precondition: y2 inside one bin does not change the dedupe key");
    check(dp::keyOf(a, t) != dp::keyOf(farBin, t), "precondition: y2 in another bin does");
    check(fp(a) == fp(inBin), "a pair that differs only in y2 inside one bin has the SAME v1 fingerprint (v1 is a projection, not a bit-for-bit match)");
    check(fp(a) != fp(farBin), "y2 in another bin changes the fingerprint");
    check(fp(a) != fp(fineY), "y at full float precision changes the fingerprint");
    check(fp(a) == fp(lineage), "the lineage (parent) is not in the fingerprint");
    check(std::memcmp(&a, &inBin, sizeof a) != 0, "the two states of the first pair are different states");
}
// --worldkey (audit AUD-857): the camera summary keeps WHEN the speed changed and where x jumped, and the instrument counts the children BEFORE the dedupe.
void cameraSummaryFixtures() {
    using namespace dp::gdtrig;
    // A -> B at tick 100 is not A -> B at tick 200 (the same dx values, different change ticks)
    const uint64_t at100 = dxHistStep(0, 1.3f, 1.6f, 5000.0, 5001.6, 100);
    const uint64_t at200 = dxHistStep(0, 1.3f, 1.6f, 5000.0, 5001.6, 200);
    check(at100 != at200 && at100 != 0, "the same speed change at tick 100 and at tick 200 are different histories");
    check(dxHistStep(at100, 1.6f, 1.6f, 5001.6, 5003.2, 101) == at100, "a tick with no change leaves the history alone");
    check(dxHistStep(7, 1.3f, 1.3f, 1000.0, 1001.3, 50) == 7, "...and so does a plain advance of one tick");
    // two histories that differ only in a jump of x (a teleport): same dx throughout
    const uint64_t j1 = dxHistStep(0, 1.3f, 1.3f, 1000.0, 1500.0, 100);
    const uint64_t j2 = dxHistStep(0, 1.3f, 1.3f, 1000.0, 1600.0, 100);
    const uint64_t j3 = dxHistStep(0, 1.3f, 1.3f, 1000.0, 1500.0, 200);
    check(j1 != 0 && j1 != j2 && j1 != j3, "a jump of x: where it lands and when are both kept");
    check(dxHistStep(0, 1.3f, 1.3f, 1000.0, 1006.0, 100) == 0, "a step of six pixels is an advance, not a jump");
    check(dxHistStep(0, 1.3f, 1.3f, 1000.0, 980.0, 100) != 0, "a step of 20 pixels backwards (beyond 8 dx) is a jump too");
    // the instrument counts the children before the dedupe: two children with the SAME key and different histories
    std::vector<std::pair<uint64_t, uint64_t>> kids{{42, 1}, {42, 2}, {43, 1}};
    KeyCount c = countKeys(kids);
    check(c.k == 2 && c.kd == 3, "before the dedupe: 2 distinct keys, 3 with the history (the split is seen)");
    std::vector<std::pair<uint64_t, uint64_t>> survivors{{42, 1}, {43, 1}};   // the old dedupe kept the first child of key 42
    KeyCount s = countKeys(survivors);
    check(s.k == 2 && s.kd == 2, "after the old dedupe: 2 and 2 (the survivors cannot show the split)");
    check(c.kd - c.k == 1 && s.kd - s.k == 0, "the pair count before and after the dedupe differs: 1 against 0");
}
// P4 / P5 PURE PARTS (restructure draft section 7.5 / 7.6): the Tap producer, the group on / off bits and the x-crossing gate digest. Nothing is wired into the search yet.
void worldPartsFixtures() {
    using namespace dp::gdtrig;
    // ---- the Tap producer: a custom level's Tap (hold mode, ttog 2: a press switches the group off, the release on)
    dp::gdtouch::TapDecider d;
    d.taps[30207] = {300, 1, 2};
    d.startAttempt();
    d.crossed(30207, 100);
    check(tapEvents(d, 90, 1).empty(), "an edge before the crossing does nothing");
    check(tapEvents(d, 100, 1).empty(), "an edge at the crossing's own tick does not act (the crossing arms it)");
    std::vector<Event> pr = tapEvents(d, 150, 1);
    check(pr.size() == 1 && pr[0].kind == kEvTap && pr[0].step == 151 && pr[0].uid == 30207 && pr[0].group == 300 && !pr[0].on && !pr[0].late,
          "a press after the crossing: one Tap event, group 300 off, applied at step tick + 1");
    std::vector<Event> rl = tapEvents(d, 155, 2);
    check(rl.size() == 1 && rl[0].step == 156 && rl[0].on, "the release in hold mode puts the group back on");
    check(tapEvents(d, 160, 0).empty(), "no edge, no event");
    d.stopAt(30207, 200);
    check(!tapEvents(d, 200, 1).empty() && tapEvents(d, 201, 1).empty(), "a Stop ends the Tap: an edge after the Stop's tick does nothing");
    dp::gdtouch::TapDecider nh;
    nh.taps[7] = {500, 0, 1};
    nh.startAttempt(); nh.crossed(7, 10);
    check(tapEvents(nh, 20, 1).size() == 1 && tapEvents(nh, 20, 1)[0].on && tapEvents(nh, 25, 2).empty(),
          "without hold mode a press applies the toggle mode (ttog 1: on) and a release does nothing");
    check(buttonEdge(0, 1) == 1 && buttonEdge(1, 0) == 2 && buttonEdge(1, 1) == 0 && buttonEdge(0, 0) == 0, "the edge of two button levels");
    check(eventHash(pr[0]) != eventHash(rl[0]), "a press and a release are different events");
    Event late = pr[0]; late.late = true;
    check(eventHash(pr[0]) != eventHash(late), "late is part of an event's identity");
    // ---- TogBits: last write wins, the value is the current state and not the history
    TogBits a, b;
    a.set(3, false, true); a.set(70, false, true);
    b.set(70, false, true); b.set(3, false, true);
    check(a == b && a.digest() == b.digest(), "the order of two switches does not matter");
    TogBits c = a;
    c.set(3, true, true);
    check(!(c == a) && c.digest() != a.digest(), "switching a group back changes the value");
    c.set(70, true, true);
    check(c == TogBits{} && c.digest() == TogBits{}.digest(), "every group back where it started is the value of a level nothing switched");
    TogBits e1, e2;
    e1.set(5, false, true); e1.set(5, false, true);
    e2.set(5, false, true);
    check(e1 == e2, "writing the same value twice is writing it once");
    TogBits startsOff;
    startsOff.set(9, false, false);
    check(startsOff == TogBits{}, "a group that starts off and is written off is unchanged");
    check(!(TogBits{}.digest() == a.digest()) && a.differs(3) && a.differs(70) && !a.differs(4), "the bits and the digest see the switched groups");
    // ---- xgate: a SUM, so the same trigger crossed twice does not cancel
    const uint64_t once = xgateAdd(0, 11, true);
    const uint64_t twice = xgateAdd(once, 11, true);
    check(once != 0 && twice != 0 && once != twice, "a trigger crossed once, twice and never: three different values (twice does not return to 0)");
    const uint64_t fireSkip = xgateAdd(xgateAdd(0, 11, true), 11, false);
    const uint64_t fireFire = xgateAdd(xgateAdd(0, 11, true), 11, true);
    const uint64_t skipSkip = xgateAdd(xgateAdd(0, 11, false), 11, false);
    check(fireSkip != fireFire && fireFire != skipSkip && fireSkip != skipSkip, "(fired, skipped), (fired, fired) and (skipped, skipped) are three different histories");
    check(xgateAdd(xgateAdd(0, 11, true), 12, false) == xgateAdd(xgateAdd(0, 12, false), 11, true), "the order of the crossings does not matter");
    check(xgateAdd(0, 11, true) != xgateAdd(0, 12, true), "different triggers differ");
}
// P4: THE TOGGLE ORB PRODUCER (trigevent_orb.hpp), a pure transcription of tools/trig_replay.cpp's --pickup rule. Nothing is wired into the search yet.
void orbProducerFixtures() {
    using namespace dp::gdtrig;
    OrbRect orb; orb.cx = 1000; orb.cy = 500; orb.w = 30; orb.h = 30; orb.w0 = 30; orb.h0 = 30;
    OrbPlayer on1; on1.x = 1000; on1.y = 500;                 // on the orb
    OrbPlayer near1; near1.x = 1000 + 29.9; near1.y = 500;    // 15 + 15 = 30 apart in x is the edge: 29.9 is inside
    OrbPlayer off1; off1.x = 1000 + 30.5; off1.y = 500;       // just outside
    check(orbOverlap(orb, on1) && orbOverlap(orb, near1) && !orbOverlap(orb, off1), "the player's 30 x 30 box against the orb's 30 x 30 rect: inside, at the edge, outside");
    OrbPlayer sp = off1; sp.spider = true; sp.x = 1000 + 28.4;   // spider box is 27 x 27: 13.5 + 15 = 28.5
    check(orbOverlap(orb, sp), "the spider's box is 27 x 27 (half 13.5): 28.4 away is a hit");
    sp.x = 1000 + 28.6;
    check(!orbOverlap(orb, sp), "...and 28.6 away is not");
    OrbPlayer big = off1; big.vsize = 2.0; big.x = 1000 + 44;
    check(orbOverlap(orb, big), "the box is times the size (half 30 for size 2: 44 away is a hit)");
    // a rotated orb: the bounding rect of a 45-degree 30 x 30 square is 42.4 wide, the oriented box is the square
    OrbRect rot = orb; rot.rot = 45; rot.w = rot.h = 42.43;
    OrbPlayer corner; corner.x = 1000 + 34; corner.y = 500 + 34;   // |34| <= 15 + 21.2: inside the bounding rect's reach; the diamond |x| + |y| <= 21.2 is 19 + 19 = 38 away from the player's nearest corner
    check(orbOverlap(rot, corner, false), "without the oriented box the 45-degree orb is its bounding rect: the corner hits");
    check(!orbOverlap(rot, corner, true), "with the oriented box (SAT) the same corner misses");
    // the decision
    OrbInput push; push.pushNow = true;
    OrbCall c = orbDecide(orb, on1, off1, push);
    check(c.fire && c.push, "a click while the previous row is on the orb: the push path fires, late");
    c = orbDecide(orb, off1, on1, push);
    check(!c.fire, "a click while the previous row is off the orb does not fire by the push path (the call row's box is not tested)");
    OrbInput held; held.armed = true;
    c = orbDecide(orb, off1, on1, held);
    check(c.fire && !c.push, "an armed push reaches the orb at the call row: the contact path fires, not late");
    held.consumed = true;
    check(!orbDecide(orb, off1, on1, held).fire, "one Toggle Orb per push: consumed, no second");
    held.consumed = false; held.noContact = true;
    check(!orbDecide(orb, off1, on1, held).fire, "in a ship / ufo / wave / swing the contact path is closed");
    held.noContact = false; held.contactPath = false;
    check(!orbDecide(orb, off1, on1, held).fire, "without the contact path (the plain rule) a held button does not fire");
    OrbInput again = push; again.done = true;
    check(!orbDecide(orb, on1, on1, again).fire, "an orb that fired before does not fire again...");
    again.multi = true;
    check(orbDecide(orb, on1, on1, again).fire, "...unless it is Multi Activate");
    OrbPlayer gone = on1; gone.valid = false;
    check(!orbDecide(orb, gone, gone, push).fire, "a row without a player place never fires");
    const Event ev = orbEvent(2000, 77, true);
    check(ev.kind == kEvOrb && ev.step == 2000 && ev.uid == 77 && ev.late && !orbEvent(2000, 77, false).late, "the orb Event: step, uid, late = the push path");
}
// An Event trigger (3604, GJBaseGameLayer::activateEventTrigger 0x33ada0 / gameEventTriggered 0x33ac80): crossed, it registers a listener; a game event raised with its key spawns its target group, whose spawn-triggered
// Toggle switches a group on. The level is a custom level's chain at a small scale: Toggle 412 (group 101 off at the start), Event 414 (events 42, target 103), Toggle 413 in group 103 (spawn-triggered, activates 101), a pad 402 in 101.
void eventTriggerFixtures() {
    using namespace dp::gdtrig;
    auto build = [](bool fed, bool extra) {
        TrigShadow z;
        z.addObject(402, 6000, 100, 0, 0, {101});     // the pad
        z.addObject(412, 10, 0, 0, 0, {});            // the start Toggle
        z.addObject(414, 100, 0, 0, 0, {});           // the Event trigger
        z.addObject(413, 200, 0, 0, 0, {103});        // the Toggle the event's group holds
        TrigDef off; off.uid = 412; off.id = 1049; off.target = 101; off.togon = 0; off.cx = 10; z.addTrigger(off);
        TrigDef ev; ev.uid = 414; ev.id = 3604; ev.target = 103; ev.cx = 100; ev.evList = {42, 39}; ev.evOk = true; ev.evKey = extra ? 10001 : 0; z.addTrigger(ev);
        TrigDef on; on.uid = 413; on.id = 1049; on.target = 101; on.togon = 1; on.spawn = 1; on.cx = 200; z.addTrigger(on);
        if (fed) { z.setEventsFed(); z.addGameEvent(6, 42); z.addGameEvent(6, 9); z.addGameEvent(9, 39); }
        z.finish();
        return z;
    };
    auto padOn = [](const TrigShadow& z) { return z.objs[z.byUid.find(402)->second].on(); };
    {   // fed: the crossing registers, the event at step 6 spawns group 103, Toggle 413 switches 101 on
        TrigShadow z = build(true, false);
        check(z.modelled(414) && z.modelled(413), "event: the Event trigger is run when events are fed, and the spawn-triggered Toggle in its group with it");
        z.step(1, 50); check(!padOn(z), "event: the start Toggle switched group 101 off");
        z.step(2, 150); z.step(3, 250); z.step(4, 300); z.step(5, 350);
        check(!padOn(z) && z.eventsRaised == 0, "event: before the event nothing spawned group 103");
        z.step(6, 400); check(padOn(z) && z.eventsRaised == 1, "event: event 42 raised at step 6 spawned group 103, Toggle 413 switched 101 on");
        z.step(7, 450);
        z.step(8, 500); z.step(9, 550); check(z.eventsRaised == 2, "event: a second event of the list (39) raised later calls the listener again");
    }
    {   // not fed: an Event trigger is not run (its groups are not vouched for), a raised event does nothing
        TrigShadow z = build(false, false);
        check(!z.modelled(414) && !z.modelled(413), "event: without setEventsFed the Event trigger and the Toggle it spawns are not run");
        z.raiseEvent(42); check(z.eventsRaised == 0, "event: raiseEvent does nothing when the events are not fed");
    }
    {   // a listener with extra IDs answers only that key; a player other than 0 also raises the key with player 0
        TrigShadow z = build(true, true);
        z.step(1, 50); z.step(2, 150); z.step(3, 250);
        z.raiseEvent(42, 0, 0); check(z.eventsRaised == 0, "event: a listener under key 10001 does not answer key 0");
        z.raiseEvent(42, 1, 1); check(z.eventsRaised == 1, "event: a call (event, material 1, player 1) answers the listener of key 10001");
    }
    {   // one call a step for each (event, key): a second raise in the same step is dropped; the next step takes it again
        TrigShadow z = build(true, false);
        z.step(1, 50); z.step(2, 150);
        z.raiseEvent(42); z.raiseEvent(42); check(z.eventsRaised == 1, "event: the same (event, key) twice in a step spawns once");
        z.raiseEvent(42, 0, 2); check(z.eventsRaised == 1, "event: player 2 raises key 2 (no listener), and its call with player 0 is the (event, key 0) already raised this step: dropped");
        z.step(3, 250); z.raiseEvent(42, 0, 2); check(z.eventsRaised == 2, "event: in a new step, player 2's call reaches the listener of key 0 through the call with player 0");
        z.step(4, 300); z.raiseEvent(42); check(z.eventsRaised == 3, "event: the next step takes the event again");
    }
}
// State::evgate's chain (game_event.hpp): each link names the digest before it, the step and the events; walking back from a digest gives the lineage's events oldest-first; the reset empties it.
void eventChainFixtures() {
    using namespace dp::gdev;
    evChainReset();
    const uint64_t a = evChainAdd(0, 100, {42});
    const uint64_t b = evChainAdd(a, 140, {8, 42});
    const uint64_t c = evChainAdd(0, 140, {8, 42});
    check(a != 0 && b != 0 && a != b && b != c, "evchain: a digest is not 0, and the same events at the same step on another history are another digest");
    check(g_evChain.size() == 3 && g_evChain.at(b).prev == a && g_evChain.at(b).step == 140 && g_evChain.at(b).events == std::vector<int>({8, 42}), "evchain: a link names the digest before it, its step and its events");
    std::vector<long long> steps; for (uint64_t d = b; d != 0; d = g_evChain.at(d).prev) steps.push_back(g_evChain.at(d).step);
    check(steps == std::vector<long long>({140, 100}), "evchain: walking back from a digest gives the steps newest first");
    check(evChainAdd(0, 100, {42}) == a && g_evChain.size() == 3, "evchain: the same link twice is one digest");
    g_evListened[0] = 1ull << 9; check(evListened(9) && !evListened(10) && evAnyListened(), "evchain: the listened events are a bit set");
    evChainReset(); check(g_evChain.empty() && !evAnyListened(), "evchain: the reset empties the chain and the listened set");
    {   // the key: a state that raised the events is another world, and another group
        dp::State s0{}, s1{}; s1.evKey = 5;
        check(dp::evSig(s0) == 0 && dp::evSig(s1) != 0, "evchain: the group signature of a state with no events is 0, with events not");
        check(dp::keyOf(s0, 100) != dp::keyOf(s1, 100), "evchain: two states that differ only in State::evKey (the events alone) have other dedupe keys");
        dp::State s2{}; s2.evgate = 9;   // the chain's digest also carries the Taps' presses: it is the timelines', not the key's
        check(dp::evSig(s2) == 0 && dp::keyOf(s0, 100) == dp::keyOf(s2, 100), "evchain: State::evgate alone is not in the key or the signature (State::evKey is)");
    }
    {   // a Tap chain's presses in the key: the count per chain Tap and the edges of the last effect window; two states with the same count and window are one world whatever the older presses' ticks were
        dp::State s0{}, a{}, b{}, c{};
        a.tapN[0] = 3; a.tapWin = 0x5; a.evgate = 11;   // (other lineages: only the key's share counts)
        b.tapN[0] = 3; b.tapWin = 0x5; b.evgate = 22;
        c.tapN[0] = 4; c.tapWin = 0x5;
        check(dp::tapKeyBits(s0) == 0 && dp::tapKeyBits(a) != 0, "tapkey: no Tap press heard is 0");
        check(dp::keyOf(a, 100) == dp::keyOf(b, 100) && dp::evSig(a) == dp::evSig(b), "tapkey: the same count and window with other older ticks (other State::evgate) are one key and one signature");
        check(dp::keyOf(a, 100) != dp::keyOf(c, 100) && dp::evSig(a) != dp::evSig(c), "tapkey: another count is another world");
        c.tapN[0] = 3; c.tapWin = 0x9;
        // the window enters the key only while the player is near what the chains move (game_event.hpp g_tapNear): far from everything, the edges' exact ticks are one world (the count says the rest)
        const auto nearKeep = dp::gdev::g_tapNear; const double pxKeep = dp::gdev::g_tapNearPx, pyKeep = dp::gdev::g_tapNearPy;
        dp::gdev::g_tapNear.clear(); dp::gdev::g_tapNearPx = dp::gdev::g_tapNearPy = 0;
        check(dp::keyOf(a, 100) == dp::keyOf(c, 100), "tapkey: the same count with another edge in the window is one world while the player is far from what the chains move");
        dp::gdev::g_tapNear = {{-1e9, 1e9, -1e9, 1e9}};   // a box around the fixture's states (xAbs 0, y 0)
        check(dp::keyOf(a, 100) != dp::keyOf(c, 100), "tapkey: the same count with another edge in the window is another world");
        dp::gdev::g_tapNear = nearKeep; dp::gdev::g_tapNearPx = pxKeep; dp::gdev::g_tapNearPy = pyKeep;
        // a Tap's count leaves the key once the player is past the farthest x the objects it moves can reach (game_event.hpp g_tapPastX)
        const double pastKeep = dp::gdev::g_tapPastX[0];
        dp::State p3{}, p4{}; p3.tapN[0] = 3; p4.tapN[0] = 4; p3.xAbs = p4.xAbs = 50.0f;
        dp::gdev::g_tapPastX[0] = 100.0;
        check(dp::keyOf(p3, 100) != dp::keyOf(p4, 100), "tapkey: another count is another world while the player is short of the Tap's past x");
        p3.xAbs = p4.xAbs = 150.0f;
        check(dp::keyOf(p3, 100) == dp::keyOf(p4, 100), "tapkey: past the Tap's past x the count is no part of the key");
        dp::gdev::g_tapPastX[0] = pastKeep;
    }
}
// The world's tog (restructure draft section 7.7): the per-tick update of State::tog / xgate against trigtouch.hpp's TapDecider, and the table builder
// --trigsupply-group: a group that a camera-reading trigger acts on is not supplied (AUD-871). groupReadsCamera is what ensure() asks per supplied group.
void supplyCameraFixtures() {
    using namespace dp::gdtrig;
    auto build = [](bool screenArea, bool lockCam, bool plainArea) {
        TrigShadow z;
        z.addObject(1, 100, 100, 0, 0, {636});         // plain member of 636 only
        z.addObject(2, 100, 130, 0, 0, {636, 240});    // member of 636 that is also in the group 240 the camera trigger acts on
        z.addObject(3, 100, 160, 0, 0, {1708});        // another group, untouched
        if (screenArea || plainArea) { AreaDef a; a.uid = 900; a.target = 240; a.special = screenArea ? -3 : -1; z.addArea(a); }
        if (lockCam) { TrigDef t; t.uid = 901; t.id = 901; t.target = 240; t.lockCam = true; z.addTrigger(t); }
        return z;
    };
    int via = 0;
    { const TrigShadow z = build(true, false, false); check(z.groupReadsCamera(636, &via) && via == 240, "supply: a screen Area (special -3) on group 240 makes group 636 camera-dependent"); check(!z.groupReadsCamera(1708), "supply: a group none of whose objects is in 240 is not camera-dependent"); }
    { const TrigShadow z = build(false, true, false); via = 0; check(z.groupReadsCamera(636, &via) && via == 240, "supply: a camera-locked Move on group 240 makes group 636 camera-dependent"); }
    { const TrigShadow z = build(false, false, true); check(!z.groupReadsCamera(636), "supply: an Area centred on the player (special -1) does not read the camera"); }
    { const TrigShadow z = build(false, false, false); check(!z.groupReadsCamera(636) && !z.groupReadsCamera(240) && !z.groupReadsCamera(999), "supply: no camera trigger, and an unknown group, are not camera-dependent"); }
    {   // a Move locked to the player (lockx / locky) on group 240 makes group 636 player-lock-dependent; a plain Move and a camera lock do not
        TrigShadow z = build(false, false, false);
        TrigDef t; t.uid = 902; t.id = 901; t.target = 240; t.lockPlayer = true; t.lockPX = true; z.addTrigger(t);
        via = 0; check(z.groupReadsPlayerLock(636, &via) && via == 240, "supply: a Move locked to the player (x) on group 240 makes group 636 player-lock-dependent");
        check(!z.groupReadsPlayerLock(1708) && !z.groupReadsPlayerLock(999), "supply: a group none of whose objects is in 240, and an unknown group, are not player-lock-dependent");
        check(!z.groupReadsCamera(636), "supply: a player lock is not a camera reader");
        TrigShadow z2 = build(false, false, false);
        TrigDef p; p.uid = 903; p.id = 901; p.target = 240; z2.addTrigger(p);
        check(!z2.groupReadsPlayerLock(636), "supply: a plain Move on group 240 is not player-lock-dependent");
        TrigShadow z3 = build(false, true, false);
        check(!z3.groupReadsPlayerLock(636), "supply: a camera-locked Move is not a player lock");
        int bv = 0;
        check(dp::TrigLive::supplyBoundary(z, 636, false, false, &bv) == 3 && bv == 240, "supply: a player-lock group is supplied by default (outcome 3, noted in the log)");
        check(dp::TrigLive::supplyBoundary(z, 636, true, false, &bv) == 2, "supply: with --trigsupplynolock a player-lock group is dropped");
        check(dp::TrigLive::supplyBoundary(z3, 636, false, false, &bv) == 1 && dp::TrigLive::supplyBoundary(z3, 636, true, false, &bv) == 1, "supply: a camera reader is dropped either way");
        check(dp::TrigLive::supplyBoundary(z2, 636, false, false, &bv) == 0, "supply: a group with a plain Move is supplied without a note");
        {   // --trigsupplycam: a Move locked to the camera (camLockOk), X or Y, is taken over (a step whose camera is not known on a locked axis blinds its target); a screen Area along y, or a lock the loader did not run, stays dropped
            auto camMove = [&](bool cx, bool cy, bool ok) { TrigShadow w = build(false, false, false); TrigDef c; c.uid = 904; c.id = 901; c.target = 240; c.lockCam = true; c.lockCX = cx; c.lockCY = cy; c.camLockOk = ok; w.addTrigger(c); return w; };
            const TrigShadow cx = camMove(true, false, true), cxy = camMove(true, true, true), cxOff = camMove(true, false, false);
            check(cx.groupCameraReader(636, &bv) == 2 && bv == 240, "supply: a camera-X Move (camLockOk) on group 240 is a soft camera reader of group 636");
            check(cxy.groupCameraReader(636, &bv) == 2 && bv == 240, "supply: a Move locked to the camera Y too (camLockOk) is a soft camera reader");
            check(cxOff.groupCameraReader(636) == 1, "supply: a camera lock the loader did not run is a hard camera reader");
            // T80 (05be757e): a screen Area Move along x (dirType 1, AreaDef's default) reads the camera's x alone: a soft reader; along y or radial it reads the camera's bottom / height, which the camera model has not: hard
            check(build(true, false, false).groupCameraReader(636) == 2 && build(false, false, false).groupCameraReader(636) == 0 && cx.groupCameraReader(1708) == 0, "supply: a screen Area along x is a soft camera reader; no camera trigger, and a group out of reach, read nothing");
            for (int dt : {0, 2}) { TrigShadow sa = build(false, false, false); AreaDef a; a.uid = 905; a.target = 240; a.special = -3; a.dirType = dt; sa.addArea(a); check(sa.groupCameraReader(636) == 1, "supply: a screen Area along y (or radial) is a hard camera reader"); }
            check(dp::TrigLive::supplyBoundary(cx, 636, false, false, &bv) == 1, "supply: a camera-X group is dropped by default");
            check(dp::TrigLive::supplyBoundary(cx, 636, false, true, &bv) == 4, "supply: with --trigsupplycam a camera-X group is supplied (outcome 4)");
            check(dp::TrigLive::supplyBoundary(cxy, 636, false, false, &bv) == 1 && dp::TrigLive::supplyBoundary(cxy, 636, false, true, &bv) == 4, "supply: a camera-Y group is dropped by default and supplied with --trigsupplycam (outcome 4)");
            check(dp::TrigLive::supplyBoundary(cxOff, 636, false, true, &bv) == 1, "supply: --trigsupplycam does not take an unrun lock");
            check(TrigShadow::ignorableTrigger(2913) && TrigShadow::ignorableTrigger(2919) && TrigShadow::ignorableTrigger(2904) && TrigShadow::ignorableTrigger(2924) && !TrigShadow::ignorableTrigger(2906) && !TrigShadow::ignorableTrigger(2918) && !TrigShadow::ignorableTrigger(901) && !TrigShadow::ignorableTrigger(2925),
                  "supply: the shader triggers (2904 .. 2924 but 2906 / 2908 / 2918: triggerShaderCommand) move nothing, so a group they act on is not uncovered by them");
        }
        {   // a touch Toggle Block (1594) the caller keeps as a box is run, so its target group is a supply candidate; one it does not keep stays out (nothing would ever give it an Event)
            auto blocks = [&](bool told) {
                TrigShadow w; w.addObject(1, 3000, 100, 0, 0, {22}); w.addObject(2, 3100, 100, 0, 0, {23});
                TrigDef a; a.uid = 50; a.id = 1594; a.touch = 1; a.target = 22; w.addTrigger(a);
                TrigDef b; b.uid = 51; b.id = 1594; b.touch = 1; b.target = 23; w.addTrigger(b);
                w.setTouchEvents({}); w.setRecordComplete(true);
                if (told) w.setToggleBoxes({{50, 0}});
                w.finish();
                return w;
            };
            const TrigShadow t1 = blocks(true), t0 = blocks(false);
            check(t1.modelled(50) && !t1.modelled(51) && t1.moverTargetGroups().count(22) == 1 && t1.moverTargetGroups().count(23) == 0, "supply: a touch Toggle Block the caller keeps as a box is run and its group is a candidate; the one it does not keep is not");
            check(!t0.modelled(50) && t0.moverTargetGroups().empty(), "supply: with no box told, a touch 1594 is not run (the loader's pickup option is off)");
        }
        {   // the touch triggers behind the anchor that name a group (both shapes: a touch Move on the group; a touch trigger on a group the object of a Move on the group is in), and a box that rides a mover's group
            TrigShadow w;
            w.addObject(1, 3000, 100, 0, 0, {22});        // member of 22
            w.addObject(61, 5000, 100, 0, 0, {30});       // the object of the Move (uid 61) on group 22 sits in group 30
            w.addObject(60, 1000, 100, 0, 0, {40});       // the touch Move's own object
            TrigDef a; a.uid = 60; a.id = 901; a.touch = 1; a.target = 22; a.cx = 1000; w.addTrigger(a);            // acts on 22, behind x 2000
            TrigDef m; m.uid = 61; m.id = 901; m.target = 22; m.cx = 5000; w.addTrigger(m);                         // a plain Move on 22 whose object is in group 30
            TrigDef g; g.uid = 62; g.id = 1049; g.touch = 1; g.target = 30; g.cx = 1500; w.addTrigger(g);           // gates it
            TrigDef f; f.uid = 63; f.id = 901; f.touch = 1; f.target = 22; f.cx = 2500; w.addTrigger(f);            // acts on 22 but ahead of x 2000
            TrigDef r; r.uid = 64; r.id = 901; r.target = 41; r.cx = 9000; w.addTrigger(r);                         // a Move on group 41
            w.addObject(5, 1200, 100, 0, 0, {41}); TrigDef rb; rb.uid = 5; rb.id = 1049; rb.touch = 1; rb.target = 99; w.addTrigger(rb);
            const std::vector<int> us = w.touchBehindUids(22, 2000.0);
            check(us.size() == 2 && std::find(us.begin(), us.end(), 60) != us.end() && std::find(us.begin(), us.end(), 62) != us.end(), "supply: touchBehindUids names every touch trigger behind the anchor that acts on or gates the group, and none ahead of it");
            check(w.touchBoxRidesMover(5) && !w.touchBoxRidesMover(60), "supply: a touch box whose object is in a group a Move acts on rides it; one that is in no such group does not");
        }
    }
}

// A Move-To (target mode) whose target group has no main object: GD's tryGetObject takes a RANDOM member, and the dump's mva says -1. When the members stand at the same x (the axis read), any member gives the same
// distance, so the shadow takes the first one (TrigShadow::agreeMember); members that differ, or the switch off, leave the old reading (no distance). a custom level g2000 (three uids, Move 3826 -> object 909) differed by 0.0958 px.
void moveToAnyMemberFixtures() {
    using namespace dp::gdtrig;
    auto run = [](bool agree, bool flag, double* x1, double* x3) {
        TrigShadow z;
        z.moveToAnyMember = flag;
        z.addObject(1, 4900.0, 100, 0, 0, {2000});
        z.addObject(2, 4900.0, 200, 0, 0, {2000});
        z.addObject(3, agree ? 4900.0 : 4901.0, 300, 0, 0, {2000});
        z.addObject(909, 5000.0, 50, 0, 0, {56});
        TrigDef t; t.uid = 3826; t.id = 901; t.cx = 1000; t.cy = 100; t.target = 2000; t.center = 56; t.mvtgt = true; t.mvaxis = 1;
        t.mvaCol = true; t.mvaObj = -1; t.ctrCol = true; t.ctrObj = 909; t.dur = 0; t.erate = 2.0;
        z.addTrigger(t);
        z.finish();
        for (int s = 1; s <= 8; ++s) z.step(s, s < 3 ? 900.0 : 1100.0 + s, 0.0);
        *x1 = z.objs[z.byUid.at(1)].x; *x3 = z.objs[z.byUid.at(3)].x;
    };
    double x1 = 0, x3 = 0;
    run(true, true, &x1, &x3);
    check(x1 == 5000.0 && x3 == 5000.0, "Move-To: a target group with no main object and members at one x moves its members to the destination's x");
    run(true, false, &x1, &x3);
    check(x1 == 4900.0 && x3 == 4900.0, "Move-To: with the member rule off the old reading stays (no distance)");
    run(false, true, &x1, &x3);
    check(x1 == 4900.0 && x3 == 4901.0, "Move-To: members that disagree on x (a random pick would decide) are left alone");
}

// A Tap's press is the game's toggleGroupTriggered (GJEffectManager::playerButton 0x36c910 -> the layer's delegate): the group is switched and, switched on, SPAWNED. A group that holds a spawn-triggered Move starts
// it at every press, on or already on; a group of plain objects only changes its bit.
void tapToggleSpawnFixtures() {
    using namespace dp::gdtrig;
    auto run = [](bool withMove, std::initializer_list<long long> pressSteps, double* y) {
        TrigShadow z;
        z.addObject(1, 4900.0, 100, 0, 0, {200});         // the object the Move moves
        z.addObject(2, 4000.0, 50, 0, 0, {74});           // a plain member of the Tap's group
        if (withMove) z.addObject(3, 4000.0, 50, 0, 0, {74});   // the spawn-triggered Move, in the Tap's group
        if (withMove) { TrigDef mv; mv.uid = 3; mv.id = 901; mv.cx = 4000; mv.cy = 50; mv.target = 200; mv.dur = 0; mv.oy = 10; mv.spawn = 1; z.addTrigger(mv); }
        for (long long s : pressSteps) z.addGroupToggleEvent(s, 74, true, 99);
        z.finish();
        for (int s = 1; s <= 12; ++s) z.step(s, 100.0, 0.0);
        *y = z.objs[z.byUid.at(1)].y;
    };
    double y0 = 0, y1 = 0, y2 = 0;
    run(true, {}, &y0);
    run(true, {5}, &y1);
    run(true, {5, 8}, &y2);
    check(y1 > y0 && y2 > y1, "Tap: a press on a group with a spawn-triggered Move starts the Move, and every press starts it again");
    double yp = 0;
    run(false, {5, 8}, &yp);
    check(yp == y0, "Tap: a press on a group of plain objects moves nothing");
    {   // the live search learns a press one layer late: toggleGroupLate between step 5 and step 6 is addGroupToggleEvent(6) (the head of step 6), position for position
        auto build = [](TrigShadow& z) {
            z.addObject(1, 4900.0, 100, 0, 0, {200});
            z.addObject(3, 4000.0, 50, 0, 0, {74});
            TrigDef mv; mv.uid = 3; mv.id = 901; mv.cx = 4000; mv.cy = 50; mv.target = 200; mv.dur = 0.1; mv.oy = 10; mv.erate = 2.0; mv.spawn = 1; z.addTrigger(mv);
        };
        TrigShadow a; build(a); a.addGroupToggleEvent(6, 74, true, 99); a.finish();
        TrigShadow b; build(b); b.setGroupToggleFed(); b.finish();
        bool same = true;
        for (int s = 1; s <= 40; ++s) {
            a.step(s, 100.0, 0.0);
            if (s == 6) b.toggleGroupLate(74, true, 99);   // (after step 5, before step 6: made once the pass of step 5 has been seen)
            b.step(s, 100.0, 0.0);
            same = same && a.objs[a.byUid.at(1)].y == b.objs[b.byUid.at(1)].y;
        }
        check(same, "Tap: a press handed over late (toggleGroupLate, between two steps) moves the group exactly as the call at the head of the next step does");
    }
}

// A Move locked to the camera (lockcamx / lockcamy): the group moves by the displacement of the camera CENTRE (left edge + half the visible width = 569 / zoom, bottom + half of 320 / zoom) of the step before, times the
// Move's modifier. Measured against wtrace on custom level A g77 (X, modX 1, a zoom change) and custom level B uid 21495 (Y, modY -0.15): the step-before centre fits to 0.0005; the same step, or the left edge, does not.
// P9: the field-check registry (field_check.hpp). A family that derives a field differently is counted per field and per tick; one that derives it the same is not; fields outside the family are not read.
void fieldCheckFixtures() {
    using namespace dp;
    fcheck::Family* diffFam = fcheck::registerFamily("test-differs", {"items", "taps"}, [](const fcheck::Ctx&, State& d) { d.items = (uint16_t)(d.items + 1); });
    fcheck::Family* sameFam = fcheck::registerFamily("test-same", {"items", "taps"}, [](const fcheck::Ctx&, State& d) { d.coins = (uint8_t)(d.coins + 1); });   // changes a field it does not name
    State s{}, c{}; c.items = 5; c.taps = 2;
    fcheck::run({s, c, 100, nullptr, nullptr});
    c.items = 6;
    fcheck::run({s, c, 101, nullptr, nullptr});
    check(diffFam->ticks == 2 && diffFam->anyDiff == 2 && diffFam->diff[0] == 2 && diffFam->diff[1] == 0, "fieldcheck: a family that derives items differently is counted in items, in every tick, and not in taps");
    check(sameFam->ticks == 2 && sameFam->anyDiff == 0, "fieldcheck: a change outside the family's fields is not read");
    check(diffFam->ex.size() == 2 && diffFam->ex[0].field == "items" && diffFam->ex[0].t == 100, "fieldcheck: the first differences are kept with their tick");
}
// P7-0: the canonical serializer (state_serial.hpp). A change in any one field of a State is seen, a change in the order of the states in a layer is not, and neither the lineage nor padding nor the address of a pointer enter.
void stateSerialFixtures() {
    using namespace dp;
    using namespace dp::stser;
    struct FO { const char* name; char kind; size_t off, size; };
    State base{};
    std::vector<FO> fo;
#define X(k, n) fo.push_back({#n, k, (size_t)((const char*)&base.n - (const char*)&base), sizeof(base.n)});
    DP_STATE_FIELDS(X)
#undef X
    auto rec = [](const State& s) { std::vector<uint8_t> o; serializeState(s, o); return o; };
    const std::vector<uint8_t> r0 = rec(base);
    check(fo.size() == fields().size() && fo.size() == 182, "serializer: the field table has 182 rows (State's members)");
    // 1. every byte-valued and float member: flip one bit of its first byte, the record must differ
    int missed = 0; std::string missedNames;
    for (const FO& f : fo) {
        if (f.kind != 'i' && f.kind != 'f') continue;
        State s = base;
        ((unsigned char*)&s)[f.off] ^= 1;
        if (rec(s) == r0) { ++missed; missedNames += std::string(f.name) + " "; }
    }
    check(missed == 0, ("serializer: a one-bit change in any plain member is seen (missed: " + missedNames + ")").c_str());
    { State s = base; s.parent += 7; check(rec(s) == r0, "serializer: State::parent (an arena index) is not in the record"); }
    { State s = base; s.evgate = 0x1234; check(rec(s) == r0, "serializer: State::evgate is not in the record yet (a level with no Event trigger serializes as before)"); }
    { State s = base; s.evKey = 0x1234; s.tapN[2] = 7; s.tapAge[1] = 9; s.tapWin = 0xf0; check(rec(s) == r0, "serializer: State::evKey / tapN / tapAge / tapWin are not in the record yet (a level with no Tap chain serializes as before)"); }
    // 2. pointers: the uid is written, not the address
    {
        Obj oa; oa.uid = 11; Obj ob; ob.uid = 12; Obj oc; oc.uid = 11;
        State a = base, b = base, c = base, n = base;
        a.snapObj = &oa; b.snapObj = &ob; c.snapObj = &oc;
        check(rec(a) != rec(b) && rec(a) != rec(n) && rec(a) == rec(c), "serializer: a pointer to an Obj is its uid (other uid and null differ, another object with the same uid does not)");
        State d = base; d.usedPad[3] = &oa; State e = base; e.usedPad[2] = &oa;
        check(rec(d) != rec(e) && rec(d) != r0, "serializer: the slot of a pointer in an array matters");
        SpdSpillNode n1{5, nullptr}, n2{6, nullptr}, n3{7, &n1};
        State s1 = base, s2 = base, s3 = base;
        s1.spdSpill = &n1; s2.spdSpill = &n2; s3.spdSpill = &n3;
        check(rec(s1) != rec(s2) && rec(s1) != rec(s3) && rec(s1) != r0, "serializer: the spill chain is written as the uids along it");
    }
    // 3. padding: the bytes no member covers are not read
    {
        std::vector<char> covered(sizeof(State), 0);
        for (const FO& f : fo) for (size_t i = 0; i < f.size; ++i) covered[f.off + i] = 1;
        size_t pad = 0; for (char c : covered) if (!c) ++pad;
        check(pad == kStatePadBytes, "serializer: the bytes outside every member are exactly kStatePadBytes");
        State s = base;
        for (size_t i = 0; i < sizeof(State); ++i) if (!covered[i]) ((unsigned char*)&s)[i] = 0xFF;
        check(rec(s) == r0, "serializer: padding bytes do not enter the record");
    }
    // 4. a layer is a SET: the order does not matter; one field of one state, one state more, one state fewer, a duplicate do
    {
        std::vector<State> layer;
        for (int i = 0; i < 40; ++i) { State s = base; s.y = 100.f + (float)i * 0.25f; s.vy = (float)(i % 7); s.mode = (uint8_t)(i % 3); layer.push_back(s); }
        std::vector<std::vector<uint8_t>> recs;
        layerRecords(layer, recs); const Dig128 d0 = layerDigest(recs);
        std::vector<State> shuf = layer; std::reverse(shuf.begin(), shuf.end()); std::swap(shuf[3], shuf[17]); std::swap(shuf[0], shuf[39]);
        layerRecords(shuf, recs); const Dig128 d1 = layerDigest(recs);
        check(d0.a == d1.a && d0.b == d1.b, "layer digest v2: the order of the states in a layer does not change it");
        std::vector<State> chg = layer; chg[21].dx2 += 0.5f;
        layerRecords(chg, recs); const Dig128 d2 = layerDigest(recs);
        check(d2.a != d0.a && d2.b != d0.b, "layer digest v2: one field (dx2, a member the v1 projection leaves out) of one state changes it");
        std::vector<State> more = layer; more.push_back(layer[5]);
        layerRecords(more, recs); const Dig128 d3 = layerDigest(recs);
        std::vector<State> fewer = layer; fewer.pop_back();
        layerRecords(fewer, recs); const Dig128 d4 = layerDigest(recs);
        check(d3.a != d0.a && d4.a != d0.a && d3.a != d4.a, "layer digest v2: a duplicate state and a missing state change it");
        std::vector<State> swapped = layer; std::swap(swapped[4].y, swapped[9].y);
        layerRecords(swapped, recs); const Dig128 d5 = layerDigest(recs);
        check(d5.a != d0.a, "layer digest v2: exchanging a field between two states changes it (a per-field sum would not see it)");
    }
    // 5. a NaN is counted
    { const long long before = g_serialStats.nanFields; State s = base; s.y = std::nanf(""); (void)rec(s); check(g_serialStats.nanFields == before + 1, "serializer: a NaN float field is counted"); }
    // 6. +0.0 and -0.0 are different bits
    { State s = base; s.rot = 0.f; State t = base; t.rot = -0.f; check(rec(s) != rec(t), "serializer: +0.0 and -0.0 differ (raw bits)"); }
}
// --trigsupply-group, two loads a touch trigger can fail (gd-04): its box is moved by a Move (groupTouchBoxMoved), or it lies behind the anchor's x (groupTouchBehind)
void supplyTouchFixtures() {
    using namespace dp::gdtrig;
    auto build = [](bool moveBox, double touchCx, int touchId, bool touch) {
        TrigShadow z;
        z.addObject(1, 100.0, 100.0, 0, 0, {593});             // the supplied group's member
        z.addObject(2, touchCx, 300.0, 0, 0, {409});           // the touch trigger's own object, in group 409
        z.addObject(3, 50.0, 300.0, 0, 0, {7});                // the Move's own object
        TrigDef tt; tt.uid = 2; tt.id = touchId; tt.target = 593; tt.touch = touch ? 1 : 0; tt.cx = touchCx; z.addTrigger(tt);
        TrigDef mv; mv.uid = 3; mv.id = 901; mv.target = moveBox ? 409 : 555; mv.cx = 50.0; z.addTrigger(mv);
        z.finish();
        return z;
    };
    int tu = 0, mu = 0;
    { const TrigShadow z = build(true, 500.0, 901, true); check(z.groupTouchBoxMoved(593, &tu, &mu) && tu == 2 && mu == 3, "supply: a touch trigger on group 593 whose group 409 a Move targets has a moved box"); }
    { const TrigShadow z = build(false, 500.0, 901, true); check(!z.groupTouchBoxMoved(593), "supply: no Move targets the touch trigger's groups: the box stays"); }
    { const TrigShadow z = build(true, 500.0, 901, false); check(!z.groupTouchBoxMoved(593), "supply: a trigger that is not a touch one is not a box"); }
    { const TrigShadow z = build(false, 500.0, 901, true); check(z.groupTouchBehind(593, 800.0, &tu) && tu == 2, "supply: a touch Move whose box is behind the anchor's x is flagged");
      check(!z.groupTouchBehind(593, 400.0), "supply: a touch Move whose box is ahead of the anchor is not flagged");
      check(!z.groupTouchBehind(593, 514.0) && z.groupTouchBehind(593, 516.0), "supply: the box edge is cx + 15 (a 30 px trigger)"); }
    { const TrigShadow z = build(false, 500.0, 1595, true); check(!z.groupTouchBehind(593, 800.0), "supply: only a touch Move / Rotate / Toggle counts (not a Touch trigger 1595)"); }
}

// The Settings object 3662 (22091: no case in EffectGameObject::triggerObject, not in the sections) moves nothing: with settingsInert the group it names is covered, without it the group is not.
void settingsInertFixtures() {
    using namespace dp::gdtrig;
    auto cov = [](bool inert, int trigId) {
        TrigShadow z;
        z.settingsInert = inert;
        z.addObject(1, 100.0, 100.0, 0, 0, {9999, 77});
        z.addObject(2, 200.0, 100.0, 0, 0, {5});
        TrigDef t; t.uid = 900; t.id = trigId; t.target = 9999; z.addTrigger(t);
        z.finish();
        return std::make_pair(z.coveredObject(z.byUid.at(1)), z.coveredObject(z.byUid.at(2)));
    };
    check(!cov(false, 3662).first && cov(false, 3662).second, "settings object: without the option a group 3662 names is not covered (the old reading), another group is");
    check(cov(true, 3662).first && cov(true, 3662).second, "settings object: with the option the group 3662 names is covered");
    check(!cov(true, 3663).first, "settings object: the option is for 3662 only (another unrun id still uncovers its group)");
}

void camLockFixtures() {
    using namespace dp::gdtrig;
    auto cam = [](int s) { Cam c; const double z = 1.0 - 0.01 * s; c.l = 1000.0 + 10.0 * s; c.b = 200.0 + 3.0 * s; c.w = 569.0 / z; c.h = 320.0 / z; c.ok = true; c.okY = true; return c; };   // okY: the camera Y is the game's (a recorded camera, or the model under a follow Static)
    auto run = [&](bool ok, double modX, double modY, double* ex, double* ey, bool yKnown = true, bool* blind = nullptr) {
        TrigShadow z;
        z.addObject(1, 100.0, 100.0, 0, 0, {5});
        TrigDef t; t.uid = 900; t.id = 901; t.cx = 50; t.target = 5; t.dur = 30; t.lockCam = true; t.lockCX = true; t.lockCY = true; t.camLockOk = ok; t.modX = modX; t.modY = modY;
        z.addTrigger(t);
        z.finish();
        double sx = 0, sy = 0;
        for (int s = 1; s <= 8; ++s) {
            z.cam = cam(s); z.cam.okY = yKnown;
            z.step(s, s < 2 ? 40.0 : 60.0 + s, 0.0);
            if (s >= 3) {   // the Move fired in step 2 runs from step 3 on, with the camera of steps 2 and 1
                const Cam a = cam(s - 1), b = cam(s - 2);
                sx += (double)(float)((a.l + a.w * 0.5) - (b.l + b.w * 0.5)) * modX;
                sy += (double)(float)((a.b + a.h * 0.5) - (b.b + b.h * 0.5)) * modY;
            }
        }
        *ex = 100.0 + sx - z.objs[z.byUid.at(1)].x; *ey = 100.0 + sy - z.objs[z.byUid.at(1)].y;
        if (blind) *blind = z.camLockBlind && z.objs[z.byUid.at(1)].blind;
    };
    double ex = 1, ey = 1;
    run(true, 1.0, -0.5, &ex, &ey);
    check(std::fabs(ex) < 1e-3 && std::fabs(ey) < 1e-3, "camera lock: the group follows the camera centre's displacement of the step before, X and Y, times the modifier");
    {   // the camera model has no Y outside a follow Static: the Y lock reads nothing (the group keeps its y), and the target is blind
        bool bl = false;
        run(true, 1.0, -0.5, &ex, &ey, false, &bl);
        check(std::fabs(ex) < 1e-3 && std::fabs(ey) > 1.0 && bl, "camera lock: with the camera Y not known the X lock still follows, the Y lock does not move the group, and the target is blind");
    }
    run(false, 1.0, -0.5, &ex, &ey);
    check(std::fabs(ex) > 1.0, "camera lock: with the option off the Move is not run (the old reading)");
    {   // camLockBlind (--trigsupplycam): set once a camera-locked Move steps with a camera that is not certain, and only then
        auto blind = [&](int notOkFrom) {
            TrigShadow z;
            z.addObject(1, 100.0, 100.0, 0, 0, {5});
            TrigDef t; t.uid = 900; t.id = 901; t.cx = 50; t.target = 5; t.dur = 30; t.lockCam = true; t.lockCX = true; t.camLockOk = true;
            z.addTrigger(t);
            z.finish();
            for (int s = 1; s <= 8; ++s) { z.cam = cam(s); z.cam.ok = notOkFrom == 0 || s < notOkFrom; z.step(s, s < 2 ? 40.0 : 60.0 + s, 0.0); }
            return z.camLockBlind;
        };
        check(!blind(0), "camera lock: a certain camera all the way does not set camLockBlind");
        check(blind(5), "camera lock: a Move that steps with an uncertain camera sets camLockBlind");
    }
}

// An Edit Area (3011..3015) that finds its Area by EFFECT id (areaedit.txt useeffectid = 1): its `target` is an effect id, not a group. When the shadow does not run the edit (a field it does not keep), what it
// leaves unvouched for is the Areas with that effect id (their target group), not the objects of the group that happens to have the same number (a custom level: effect id 610, 262 members of group 610 moved only by Moves).
void editByEffectFixtures() {
    using namespace dp::gdtrig;
    auto build = [](bool useEffect, int editedTarget) {
        TrigShadow z;
        z.addObject(1, 100.0, 100.0, 0, 0, {610});   // a mover of group 610 (a plain Move acts on it)
        z.addObject(2, 200.0, 100.0, 0, 0, {5});     // an object of the Area's target group
        TrigDef mv; mv.uid = 800; mv.id = 901; mv.cx = 50; mv.target = 610; mv.dur = 1; mv.oy = 10;
        TrigDef ar; ar.uid = 801; ar.id = 3006; ar.cx = 60; ar.target = 5; ar.areaOk = true;
        TrigDef ed; ed.uid = 802; ed.id = 3011; ed.cx = 70; ed.target = editedTarget; ed.editOk = false;
        z.addTrigger(mv); z.addTrigger(ar); z.addTrigger(ed);
        AreaDef a; a.uid = 801; a.target = 5; a.special = -1; z.addArea(a);
        z.setAreaEffectIds({{801, 610}});
        z.addAreaEditInfo(802, editedTarget, useEffect);
        z.finish();
        return z;
    };
    {
        const TrigShadow z = build(true, 610);
        check(z.coveredObject(z.byUid.at(1)), "Edit Area by effect id 610 does not uncover the objects of GROUP 610");
        check(!z.coveredObject(z.byUid.at(2)), "...it uncovers the target group of the Area with effect id 610");
    }
    {
        const TrigShadow z = build(false, 610);
        check(!z.coveredObject(z.byUid.at(1)), "an Edit Area by group (the old reading) still uncovers the group it names");
    }
}

void togStateFixtures() {
    using namespace dp::gdtrig;
    TogTable T;
    T.groups = {5, 9}; T.initialOn = {1, 1};
    TogTap tp; tp.uid = 11; tp.idx = 0; tp.target = 5; tp.thold = 1; tp.ttog = 2; tp.cx = 100.0; tp.stopCx = 400.0;
    T.taps.push_back(tp);
    TogGate g1; g1.uid = 21; g1.cx = 200.0; g1.idx = {0};          // sits in the Tap's group
    TogGate g2; g2.uid = 22; g2.cx = 250.0; g2.idx = {0, 1};
    T.gates.push_back(g1); T.gates.push_back(g2);
    // parity with the decider the offline check used: a random-looking button over a walk of 6 px per tick, tick by tick, the same on / off of the Tap's group
    dp::gdtouch::TapDecider d;
    dp::gdtouch::TapDef def; def.target = 5; def.thold = 1; def.ttog = 2; d.taps[11] = def;
    TogBits tog; uint64_t xg = 0;
    bool decOn = true;
    int held = 0, mism = 0, nChange = 0;
    uint32_t rng = 12345;
    double x = 70.0;
    for (long long tick = 0; tick < 120; ++tick) {
        rng = rng * 1664525u + 1013904223u;
        const int input = ((rng >> 24) % 7) < 3 ? 1 : 0;
        const double kx = x + 6.0;
        const int edge = (input && !held) ? 1 : (!input && held) ? 2 : 0;
        for (const dp::gdtouch::TapEvent& e : d.step(tick, edge)) { decOn = e.on; ++nChange; }
        if (kx >= 100.0) d.crossed(11, tick);
        if (kx >= 400.0) d.stopAt(11, tick);
        uint64_t xg0 = xg;
        togStep(T, tog, xg, x, kx, held, input);
        (void)xg0;
        if (togGroupOn(T, tog, 0) != decOn) ++mism;
        held = input; x = kx;
    }
    check(nChange > 5 && mism == 0, "the Tap's group on / off follows trigtouch.hpp's TapDecider tick by tick (armed by x, a press, the hold release, the Stop)");
    // the crossing's own tick does not act; an edge a tick later does
    TogBits a; uint64_t xa = 0;
    togStep(T, a, xa, 97.0, 103.0, 0, 1);
    check(!a.differs(0), "a press at the crossing's own tick does not act");
    togStep(T, a, xa, 103.0, 109.0, 1, 1);
    check(!a.differs(0), "no edge, no change");
    togStep(T, a, xa, 109.0, 115.0, 1, 0);   // release: hold mode, the group goes back on (it was never off)
    check(!a.differs(0), "a release before any press switches nothing off");
    togStep(T, a, xa, 115.0, 121.0, 0, 1);
    check(a.differs(0), "the press after the crossing switches the group off (ttog 2)");
    // "x goes back, armed stays": the caller hands over the farthest x, so a parent x of 300 after a teleport back to 50 still reads as armed
    TogBits b; uint64_t xb = 0;
    togStep(T, b, xb, 300.0, 300.0, 0, 1);
    check(b.differs(0), "with the farthest x handed over, a Tap crossed before a teleport back is still armed");
    TogBits c; uint64_t xc = 0;
    togStep(T, c, xc, 50.0, 50.0, 0, 1);
    check(!c.differs(0), "...and with the plain x (--tpback off) it is not: the load-time warning is for this");
    // a Stop crossed before the tick ends the Tap
    TogBits s; uint64_t xs = 0;
    togStep(T, s, xs, 405.0, 411.0, 0, 1);
    check(!s.differs(0), "an edge after the Stop's crossing does nothing");
    // x gate: the crossing adds H(T, fired); fired follows the group's state at the tick; crossing twice does not cancel
    TogBits on; uint64_t xon = 0;
    togStep(T, on, xon, 195.0, 205.0, 0, 0);
    check(xon == xgateAdd(0, 21, true), "a gated trigger crossed while its group is on is added as fired");
    TogBits off; off.set(0, false, true); uint64_t xoff = 0;
    togStep(T, off, xoff, 195.0, 205.0, 0, 0);
    check(xoff == xgateAdd(0, 21, false) && xoff != xon, "...and as not fired while the group is off: the digests differ");
    uint64_t xtw = 0;
    togStep(T, on, xtw, 195.0, 205.0, 0, 0);
    togStep(T, on, xtw, 195.0, 205.0, 0, 0);
    check(xtw == xgateAdd(xgateAdd(0, 21, true), 21, true) && xtw != 0, "a second crossing of the same trigger does not cancel the first");
    uint64_t xs2 = 0;
    togStep(T, on, xs2, 245.0, 255.0, 0, 0);
    check(xs2 == xgateAdd(0, 22, true), "a trigger in two groups fires only when both are on");
    TogBits one; one.set(1, false, true); uint64_t xo = 0;
    togStep(T, one, xo, 245.0, 255.0, 0, 0);
    check(xo == xgateAdd(0, 22, false), "...and not when one of them is off");
    uint64_t xin = 0;
    togStep(T, on, xin, 205.0, 215.0, 0, 0);
    check(xin == 0, "a tick that crosses nothing leaves the digest alone");
    // the table builder
    std::unordered_map<int, dp::TrigRow> rows;
    auto mk = [](int uid, int id, int target, double cx, int touch, int spawn) { dp::TrigRow r{}; r.uid = uid; r.id = id; r.target = target; r.cx = cx; r.touch = touch; r.spawn = spawn; return r; };
    dp::TrigRow tapRow = mk(11, 1595, 5, 100.0, 0, 0); tapRow.thold = 1; tapRow.ttog = 2;
    rows[11] = tapRow;
    rows[12] = mk(12, 1616, 7, 400.0, 0, 0);                    // a Stop on group 7
    rows[21] = mk(21, 1268, 3, 200.0, 0, 0);                    // a Spawn that sits in group 5
    rows[22] = mk(22, 1268, 3, 300.0, 1, 0);                    // touch-started: not an x gate
    rows[23] = mk(23, 1594, 9, 150.0, 0, 0);                    // a Toggle Orb on group 9
    rows[24] = mk(24, 1268, 3, 260.0, 0, 0);                    // sits only in the orb's group
    std::unordered_map<int, std::vector<int>> mem;
    mem[5] = {21, 22, 31, 32}; mem[7] = {11}; mem[9] = {24, 33};   // 31, 32: level objects in the Tap's group; 33: one in the orb's
    TogLoadReport rep;
    const TogTable B = buildTogTable(rows, mem, &rep);
    check(B.objs.size() == 2 && B.objs[0].uid == 31 && B.objs[1].uid == 32 && B.objs[0].idx == std::vector<int>{0} && rep.gatedObjs == 2,
          "the level's own objects in the Tap's group are listed (not the triggers, not the orb's group)");
    {
        std::unordered_map<int, dp::TrigRow> rows2 = rows;
        rows2[40] = mk(40, 1049, 5, 120.0, 0, 0);   // a Toggle trigger on the same group: the bit is not the group's state any more
        const TogTable B2 = buildTogTable(rows2, mem, nullptr);
        check(B2.objs.empty(), "a group a Toggle trigger also switches is not read for its objects");
        check(B2.tapMembers == std::vector<int>({31, 32}) && B.tapMembers == std::vector<int>({31, 32}), "...but its objects still count as the Tap's members (the recording's on / off of them is the Tap's too)");
    }
    check(B.taps.size() == 1 && B.taps[0].uid == 11 && B.taps[0].stopCx == 400.0 && B.taps[0].cx == 100.0, "the table holds the Tap with the crossing x of the Stop whose group holds the Tap's own object");
    check(B.groups.size() == 2 && B.groups[0] == 5 && B.groups[1] == 9 && rep.orbGroups == 1, "the toggleable groups are the Tap's target and the orb's, ascending");
    check(B.gates.size() == 2 && B.gates[0].uid == 21 && B.gates[1].uid == 24 && rep.gatesOnlyOrb == 1, "x gates: the crossing-started triggers in those groups, by x; the one only an orb switches is counted");
    // Toggle Orbs by the push path: the box of the parent row against the orb's rect, only on a press edge; a hit sets the orb's group (actgrp)
    {
        TogTable O; O.groups = {70, 827}; O.initialOn = {1, 1};
        TogOrb a; a.uid = 2399; a.idx = 0; a.target = 70; a.on = true; a.r.cx = 4110; a.r.cy = 390; a.r.w = 36; a.r.h = 36;   // c133224341's first orb
        TogOrb b; b.uid = 43426; b.idx = 1; b.target = 827; b.on = false; b.r.cx = 38025; b.r.cy = 585; b.r.w = 36; b.r.h = 36;
        O.orbs = {a, b};
        std::vector<int> hits;
        OrbPlayer pl; pl.x = 4085.5; pl.y = 417.5;   // the game's row 3010: dx -24.5, dy 27.5 -- inside the 15 + 18 reach
        togOrbPushHits(O, pl, true, hits);
        check(hits.size() == 1 && hits[0] == 2399, "a press with the player's box over the orb hits it");
        togOrbPushHits(O, pl, false, hits);
        check(hits.empty(), "...and no press, no hit (the contact path is not modelled)");
        pl.y = 430.0;   // dy 40 > 33
        togOrbPushHits(O, pl, true, hits);
        check(hits.empty(), "a press out of reach hits nothing");
        pl.x = 38036.7; pl.y = 615.5;
        togOrbPushHits(O, pl, true, hits);
        check(hits.size() == 1 && hits[0] == 43426, "the second orb, from the game's row 22153");
        TogBits tg; uint64_t xg2 = 0;
        togStep(O, tg, xg2, 38036.7, 38039.1, 0, 1, nullptr, &hits);
        check(tg.differs(1) && !tg.differs(0), "its actgrp 0 switches group 827 off");
    }    // the State's fields are the zero value and a default State is the same as one nothing touched
    dp::State z{};
    check(z.xgate == 0 && z.tog.w[0] == 0 && z.tog.w[1] == 0, "a default State has State::tog and xgate zero");
}

// --capaudit (cap_audit.hpp): the world accounting of a capped layer, on states built by hand: two worlds (a different fired-touch history), the cap keeps both states of the first and none of the second.
void capAuditWorlds() {
    dp::g_capAuditStats = dp::CapAuditStats{};
    dp::g_capAuditEvery = 1;
    std::vector<dp::State> layer(5);
    for (auto& s : layer) { s.y = 100.f; s.vy = 1.f; s.xAbs = 700.f; }
    for (int i = 3; i < 5; ++i) layer[(size_t)i].coins = 1;   // world B: 2 states (a different item count)
    check(dp::worldKeyOf(layer[0]) != dp::worldKeyOf(layer[3]) && dp::worldKeyOf(layer[3]) == dp::worldKeyOf(layer[4]), "precondition: three states in one world, two in another");
    dp::capAuditLayer(4100, layer, std::vector<uint32_t>{0, 1});   // keeps two of world A, none of world B
    const auto& b = dp::g_capAuditStats.bk.at(4000);
    check(b.layers == 1 && b.states == 5 && b.kept == 2 && b.worlds == 2, "5 states, 2 kept, 2 worlds");
    check(b.wiped == 1 && b.wipedStates == 2 && b.layersWithWiped == 1, "the world the cap kept nothing of is counted as wiped, with its 2 states");
    check(b.ratio[0] == 1 && b.ratio[3] == 1, "kept ratio: one world 0, the other 2 of 3 (between 50 and 100 %)");
    // AUD-861: the same two states, differing only in the camera summary (dxHist); the cap keeps the first. In the COARSE world (no camera) they are one world and nothing is wiped; with the camera they are
    // two worlds and the second is wiped. (A run without --worldkey leaves dxHist 0 in every state, which is the coarse case: its "no world wiped" does not carry over to the full key.)
    dp::g_capAuditStats = dp::CapAuditStats{};
    std::vector<dp::State> cam(2);
    for (auto& s : cam) { s.y = 100.f; s.vy = 1.f; s.xAbs = 700.f; }
    cam[1].dxHist = 12345;
    dp::capAuditLayer(4100, cam, std::vector<uint32_t>{0}, false);
    check(dp::g_capAuditStats.bk.at(4000).worlds == 1 && dp::g_capAuditStats.bk.at(4000).wiped == 0, "coarse world (no camera): the two states are one world, nothing wiped");
    dp::g_capAuditStats = dp::CapAuditStats{};
    dp::capAuditLayer(4100, cam, std::vector<uint32_t>{0}, true);
    check(dp::g_capAuditStats.bk.at(4000).worlds == 2 && dp::g_capAuditStats.bk.at(4000).wiped == 1 && dp::g_capAuditStats.bk.at(4000).wipedStates == 1, "with the camera summary: two worlds, the one the cap kept nothing of is wiped");
    // tog / xgate separate worlds too
    dp::g_capAuditStats = dp::CapAuditStats{};
    std::vector<dp::State> tg(2);
    for (auto& s : tg) { s.y = 100.f; s.vy = 1.f; s.xAbs = 700.f; }
    tg[1].xgate = 99;
    dp::capAuditLayer(4100, tg, std::vector<uint32_t>{0}, true);
    check(dp::g_capAuditStats.bk.at(4000).worlds == 2 && dp::g_capAuditStats.bk.at(4000).wiped == 1, "xgate separates worlds in capaudit (worldKeyOf alone does not read it)");
    dp::g_capAuditStats = dp::CapAuditStats{};
    dp::g_capAuditEvery = 10;
}
// --portaudit (port_audit.hpp): the pair finder and the next-tick comparison, on candidates built by hand (audit AUD-857: the count is taken on the candidates BEFORE the dedupe, where the dropped child is still visible).
struct AuditKid { dp::State s{}; uint64_t key = 0; uint8_t dead = 0, valid = 1; };
void portAuditPairsBeforeDedupe() {
    dp::State a{};
    a.y = 150.f; a.vy = 2.f; a.xAbs = 500.f;
    dp::State b = a;
    b.portSeen[0] = 4711;                       // the same body, a portal overlapped on this tick: only portSeen differs
    const long long t = 1000;
    check(dp::keyOf(a, t) == dp::keyOf(b, t), "precondition: portSeen is not in the dedupe key (the old key is the same)");
    check(dp::portSeenDiffers(a, b), "the two states differ in portSeen");
    dp::State c = a; c.portSeen2[1] = 9;
    check(dp::portSeenDiffers(a, c), "...and portSeen2 counts too");
    check(!dp::portSeenDiffers(a, a), "a state does not differ from itself");
    check(!dp::portSeenReal(a) && dp::portSeenReal(b) && dp::portSeenReal(c), "a real portal is an entry >= 0 in either list; the -1 / -2 markers are not");
    std::vector<AuditKid> kids(5);
    kids[0].s = a; kids[0].key = dp::keyOf(a, t);
    kids[1].s = b; kids[1].key = dp::keyOf(b, t);                  // collides with kid 0, portSeen differs -> a pair
    kids[2].s = a; kids[2].key = dp::keyOf(a, t);                  // collides with kid 0, portSeen the same -> not a pair
    kids[3].s = c; kids[3].key = dp::keyOf(c, t); kids[3].dead = 1; // dead: not looked at
    kids[4].s = c; kids[4].key = dp::keyOf(c, t); kids[4].valid = 0; // never stepped: not looked at
    long long alive = 0, shared = 0;
    const auto prs = dp::portAuditFind(kids, &alive, &shared);
    check(prs.size() == 1 && prs[0].first == 0 && prs[0].second == 1, "one pair: the first candidate with the key and the colliding one with another portSeen");
    check(alive == 3 && shared == 2, "3 alive candidates, 2 of them with a key an earlier one had");
    // the comparison after one more tick
    dp::State y1 = a, y2 = a; y2.y = 151.f;
    check(dp::portAuditClassify(false, false, 5, 5, y1, a) == dp::PortOutcome::Same, "same key, same body -> same");
    check(dp::portAuditClassify(false, false, 5, 6, y1, a) == dp::PortOutcome::KeyDiffers, "keys differ -> key-differs");
    check(dp::portAuditClassify(false, false, 5, 5, y1, y2) == dp::PortOutcome::BodyDiffers, "same key, y differs -> body-differs");
    check(dp::portAuditClassify(true, false, 5, 5, y1, y1) == dp::PortOutcome::DeadDiffers, "one died -> dead-differs");
    check(dp::portAuditClassify(true, true, 5, 6, y1, y2) == dp::PortOutcome::BothDead, "both died -> both-dead");
}}  // namespace

// splitCommaFields against the stream loop it replaced in loadLevelFrom, field by field, on the
// shapes a row can take: empty, leading/trailing/repeated commas, a CR left by the line read, and
// rows at and past the loader's column bound.
static void commaFields() {
    std::vector<std::string> rows{"", ",", "a", "a,", ",a", "a,,b", "a,b,", ",,,", "a\r",
                                  "a,b\r", " a , b ", "1,2,3"};
    std::string wide;
    for (int i = 0; i < dp::kObjFields + 6; ++i) wide += std::to_string(i) + ",";
    rows.push_back(wide);
    rows.push_back(wide.substr(0, wide.size() - 1));
    std::string exact;
    for (int i = 0; i < dp::kObjFields; ++i) exact += (i ? "," : "") + std::to_string(i);
    rows.push_back(exact);
    rows.push_back(exact + ",");
    bool same = true;
    for (const std::string& line : rows) {
        std::string a[dp::kObjFields], b[dp::kObjFields];
        std::stringstream ss(line);
        for (int i = 0; i < dp::kObjFields && std::getline(ss, a[i], ','); ++i) {}
        dp::splitCommaFields(line, b, dp::kObjFields);
        for (int i = 0; i < dp::kObjFields; ++i) same = same && a[i] == b[i];
    }
    check(same, "splitCommaFields fills the same fields as the getline loop it replaced");
}

// The reader loadGroupTimeline was until 2026-09-30 -- getline on a text-mode stream, sscanf per
// line -- kept as the reference parseGroupTimeline is held to.
static dp::GroupTimeline groupsByScanf(const std::string& path, long long* endOut,
                                       std::unordered_map<int, uint8_t>* initOut) {
    *endOut = -1;
    initOut->clear();
    dp::GroupTimeline g;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        int t = 0, uid = 0, on = 1, env = 0;
        float cx = 0, cy = 0, w = 0, h = 0, rot = 0;
        if (line.rfind("end,", 0) == 0) {
            *endOut = std::atoll(line.c_str() + 4);
            continue;
        }
        if (line.rfind("init,", 0) == 0) {
            int u = 0, o = 1;
            if (std::sscanf(line.c_str() + 5, "%d,%d", &u, &o) == 2) (*initOut)[u] = o ? 1 : 0;
            continue;
        }
        const int n = std::sscanf(line.c_str(), "%d,%d,%f,%f,%f,%f,%d,%f,%d", &t, &uid, &cx, &cy,
                                  &w, &h, &on, &rot, &env);
        if (n < 6) continue;
        g[uid].push_back({t, cx, cy, w * 0.5f, h * 0.5f, (uint8_t)(on ? 1 : 0), rot,
                          (uint8_t)(env ? 1 : 0)});
    }
    for (auto& kv : g)
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const dp::DynSample& a, const dp::DynSample& b) { return a.t < b.t; });
    return g;
}

static bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// Empty when the two hold the same samples, bit for bit, AND iterate in the same order: the loader
// walks the timeline, so the order is part of what a reader hands on.
static std::string timelineDiff(const dp::GroupTimeline& a, const dp::GroupTimeline& b) {
    if (a.size() != b.size())
        return "objects " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
        if (ia->first != ib->first) return "order differs at uid " + std::to_string(ia->first);
        const auto& va = ia->second;
        const auto& vb = ib->second;
        if (va.size() != vb.size()) return "uid " + std::to_string(ia->first) + " row count";
        for (size_t k = 0; k < va.size(); ++k) {
            const dp::DynSample& x = va[k];
            const dp::DynSample& y = vb[k];
            if (x.t != y.t || x.on != y.on || x.env != y.env || !sameBits(x.cx, y.cx)
                || !sameBits(x.cy, y.cy) || !sameBits(x.hw, y.hw) || !sameBits(x.hh, y.hh)
                || !sameBits(x.rot, y.rot))
                return "uid " + std::to_string(ia->first) + " t=" + std::to_string(x.t);
        }
    }
    return "";
}

// One file through both readers: the rows, the trailer and the init lines. Empty when they agree.
static std::string groupsReadersDiff(const std::string& path, long long* rows = nullptr) {
    long long endA = -1, endB = -1;
    std::unordered_map<int, uint8_t> initA, initB;
    const dp::GroupTimeline a = groupsByScanf(path, &endA, &initA);
    const dp::GroupTimeline b = dp::loadGroupTimeline(path, &endB, &initB);
    if (rows) {
        *rows = 0;
        for (const auto& kv : a) *rows += (long long)kv.second.size();
    }
    const std::string d = timelineDiff(a, b);
    if (!d.empty()) return d;
    if (endA != endB) return "end " + std::to_string(endA) + " vs " + std::to_string(endB);
    if (initA != initB) return "init lines";
    return "";
}

// parseGroupTimeline against the sscanf reader it replaced, on the shapes a line can take: a
// line cut off mid-write, blanks, signs, a CR, an exponent, inf/nan, hex, a trailing comma, more
// columns than the format, more digits than the plain path takes, float rounding midpoints -- and
// 20,000 rows of %.3f the way the recorder writes them, from small to 1e7. Once through a text
// stream (CRLF, as the mod writes) and once written as bytes (LF only).
static void groupsParser() {
    std::string body =
        "tick,uid,cx,cy,w,h,on,rot\n"
        "1,7,0,100,30,3,1,0\n"
        "2,7,0.125,100.5,30.000,3.000,1,45.500\n"
        "3,8,-0.000,-12.345,1.5,2.5,0,-90.000,1\n"
        "4,8,1e3,2E-2,3,4\n"
        "5,9, 7.5, 8.25,9,10,1,0\n"
        "6,9,.5,1.,+2.5,-.25,0,0\n"
        "7,10,1.5,2.5,3.5\n"
        "8,10,1.5,2.5,3.5,4.5,\n"
        "9,10,1.5,2.5,3.5,4.\n"
        "10,11,nan,inf,-inf,1,1,0\n"
        "11,11,123456.789,0.001,99999.999,0.0005,1,359.999\n"
        "12,11,1.2.3,4,5,6\n"
        "\n"
        "13,12,1,2,3,4,5,6,7,8\n"
        "14,12,0x1p3,2,3,4\n"
        "init,12,0\n"
        "init,13,1\n"
        "15,13,3.14159265358979,2.718281828,1,1\n"
        "16,13,16777217.000,33554433.5,1,1\n"
        "17,13,2097152.125,4194303.875,1,1\n"
        "18,13,0.1,0.2,0.3,0.7,1,0.9\n"
        "19,14,-5,6,7,8,1,0\r\n"
        "20,14,  -7.5  ,1,1,1\n"
        "21 ,15,1,1,1,1\n"
        "-22,15,1,1,1,1\n"
        "23,16,1,1,1,1,1,1,1\n"
        "24,16,1,1,1,1,0,0,0\n"
        "25,16,-,1,1,1\n"
        "26,16,1,.,1,1\n"
        "end,77\n";
    unsigned long long s = 0x9E3779B97F4A7C15ULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (double)(s >> 11) / 9007199254740992.0;
    };
    const double scales[] = {1.0, 30.0, 360.0, 5000.0, 40000.0, 2.1e6, 1.0e7};
    for (int i = 0; i < 20000; ++i) {
        const double sc = scales[i % 7];
        char b[256];
        std::snprintf(b, sizeof b, "%d,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f%s\n", 30 + i, 100 + i % 97,
                      (float)((next() - 0.5) * sc), (float)(next() * sc), (float)(next() * 60.0),
                      (float)(next() * 60.0), i % 2, (float)((next() - 0.5) * 720.0),
                      i % 5 == 0 ? ",1" : "");
        body += b;
    }
    body += "99999,17,1,2,3,4";   // the last line with no newline
    const std::string text = writeTmp("dptest_groups_text.txt", body);
    const std::string bin = (std::filesystem::temp_directory_path() / "dptest_groups_lf.txt").string();
    {
        std::ofstream f(bin, std::ios::binary | std::ios::trunc);
        f << body;
    }
    const std::string d1 = groupsReadersDiff(text);
    const std::string d2 = groupsReadersDiff(bin);
    if (!d1.empty()) std::printf("  text stream: %s\n", d1.c_str());
    if (!d2.empty()) std::printf("  bytes: %s\n", d2.c_str());
    check(d1.empty() && d2.empty(),
          "parseGroupTimeline reads every line the way the sscanf reader it replaced did");

    // groupLayersFor's first layer is a COPY of a file's parse (the parse stays cached for the
    // next call), where it used to be the parse itself: the copy has to iterate the same way, and
    // an overlay laid over it has to come out as it did over the original.
    long long end = -1;
    std::unordered_map<int, uint8_t> init;
    dp::GroupTimeline base = dp::loadGroupTimeline(bin, &end, &init);
    const dp::GroupTimeline over = dp::loadGroupTimeline(writeTmp("dptest_groups_over.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "5,7,1,2,3,4,1,0\n"
        "6,50000,1,2,3,4,1,0\n"
        "7,50001,1,2,3,4,1,0\n"
        "8,150,1,2,3,4,1,0\n"
        "end,20\n"), &end, &init);
    dp::GroupTimeline copy = base;
    const std::string dc = timelineDiff(base, copy);
    dp::overlayGroupTimeline(copy, over, false, end);
    dp::overlayGroupTimeline(base, over, false, end);
    const std::string doverlay = timelineDiff(base, copy);
    if (!dc.empty()) std::printf("  copy: %s\n", dc.c_str());
    if (!doverlay.empty()) std::printf("  overlay: %s\n", doverlay.c_str());
    check(dc.empty() && doverlay.empty(),
          "a copied parse iterates as the parse does, and so does an overlay laid over it");
}

// The differential parse (groups.hpp parseGroupTimelineFileMeta, cli.hpp groupLayersFor): the loop's recording of the next round is the last one with rows
// added, and a signed file whose signature is new is read as an addition to the cached parse when its prefix hashes the same. Every answer is compared
// with the whole parse of the same file (rows bit for bit, the table's iteration order, the trailer, the init lines), and the cases that must NOT be read
// as an addition -- a changed prefix, a row older than the old ones, two rows on one tick, a shorter file -- must say so and still come out equal.
static void groupsDifferential() {
    const std::string path = (std::filesystem::temp_directory_path() / "dptest_groups_gen.txt").string();
    auto rowText = [](int t, int uid) {
        char b[160];
        std::snprintf(b, sizeof b, "%d,%d,%.3f,%.3f,30.000,30.000,%d,%.3f\n", t, uid, 100.0 + t * 0.25 + uid, 50.0 + uid * 3 + (t % 7), (t / 40) % 2,
                      (t * 3) % 360 * 1.0);
        return std::string(b);
    };
    // ticks [t0, t1): every tick one or two uids move (a different set each tick), `first` uids first appear at tick t0 + 5 (new in this generation)
    auto rows = [&](int t0, int t1, int newUid0, int newUids) {
        std::string s;
        for (int t = t0; t < t1; ++t) {
            for (int u = 1; u <= 40; ++u)
                if ((u + t) % 5 == 0) s += rowText(t, u);
            if (t == t0 + 5)
                for (int k = 0; k < newUids; ++k) s += rowText(t, newUid0 + k);
        }
        return s;
    };
    const std::string head = "tick,uid,cx,cy,w,h,on,rot\ninit,3,1\ninit,4,0\n";
    auto setHint = [&](const char* sig) { dp::g_fileSigHint.clear(); dp::g_fileSigHint[path] = sig; };
    auto writeFile = [&](const std::string& body) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << body;
    };
    auto asked = [&](const char* sig) -> const dp::GroupTimeline& {
        setHint(sig);
        return dp::groupLayersFor({path});   // the cache's own table (a copy would not iterate as it does); valid until the next call
    };
    auto equalsWhole = [&](const dp::GroupTimeline& got, long long wantEnd) {
        long long e = -1;
        std::unordered_map<int, uint8_t> in;
        const dp::GroupTimeline ref = dp::loadGroupTimeline(path, &e, &in);
        const std::string d = timelineDiff(ref, got);
        if (!d.empty()) std::printf("  differs from the whole parse: %s\n", d.c_str());
        if (e != wantEnd || dp::g_groupsEndT != wantEnd) std::printf("  end %lld / %lld, wanted %lld\n", e, dp::g_groupsEndT, wantEnd);
        if (in != dp::g_groupInit) std::printf("  init lines differ\n");
        return d.empty() && e == wantEnd && dp::g_groupsEndT == wantEnd && in == dp::g_groupInit;
    };
    dp::g_groupLayersCache = dp::GroupLayersCache{};
    const std::string a = head + rows(1, 100, 100, 0);
    const std::string b = a + rows(100, 180, 100, 3);   // three uids new in this generation
    const std::string c = b + rows(180, 260, 103, 2);
    writeFile(a + "end,99\n");
    const dp::GroupTimeline& ga = asked("sigA");
    check(equalsWhole(ga, 99), "the first generation is parsed whole and is the whole parse");
    const unsigned long long h0 = dp::g_groupIncHits;
    writeFile(b + "end,179\n");
    const dp::GroupTimeline& gb = asked("sigB");
    check(dp::g_groupIncHits == h0 + 1, "the next generation (the old rows, the old trailer replaced by rows, a new trailer) is read as an addition");
    check(equalsWhole(gb, 179), "...and comes out as the whole parse of its file: rows, table order, trailer, init lines");
    writeFile(c + "end,259\n");
    const dp::GroupTimeline& gc = asked("sigC");
    check(dp::g_groupIncHits == h0 + 2 && equalsWhole(gc, 259), "and the one after it, on the addition's own meta");
    // a prefix that changed: one value in an old row
    {
        std::string bad = c;
        const size_t at = bad.find('\n', bad.size() / 3);
        const size_t dot = bad.find('.', at);
        bad[dot + 1] = bad[dot + 1] == '9' ? '8' : '9';
        writeFile(bad + rows(260, 300, 105, 0) + "end,299\n");
        const unsigned long long h = dp::g_groupIncHits;
        const dp::GroupTimeline& g = asked("sigD");
        check(dp::g_groupIncHits == h + 1 && equalsWhole(g, 299), "a changed value in the old file parts the two there: the rows before its tick stand, the rest is parsed");
    }
    // the rows of the last generation again, then a row on a tick older than its uid's last
    {
        writeFile(c + "end,259\n");
        asked("sigE");
        const unsigned long long h = dp::g_groupIncHits;
        writeFile(c + rowText(10, 5) + rows(260, 270, 105, 0) + "end,269\n");   // uid 5 at t=10, long past its last row
        const dp::GroupTimeline& g = asked("sigF");
        check(dp::g_groupIncHits == h && equalsWhole(g, 269), "a row older than the old ones is not an addition: parsed whole");
    }
    // two rows of one uid on one tick in the added part
    {
        writeFile(c + "end,259\n");
        asked("sigG");
        const unsigned long long h = dp::g_groupIncHits;
        writeFile(c + rowText(260, 5) + rowText(260, 5) + "end,260\n");
        const dp::GroupTimeline& g = asked("sigH");
        check(dp::g_groupIncHits == h && equalsWhole(g, 260), "two rows of one uid on one tick are not appended: parsed whole");
    }
    // a file shorter than the old prefix
    {
        writeFile(c + "end,259\n");
        asked("sigI");
        const unsigned long long h = dp::g_groupIncHits;
        writeFile(a + "end,99\n");
        const dp::GroupTimeline& g = asked("sigJ");
        check(dp::g_groupIncHits == h + 1 && equalsWhole(g, 99), "a file that is the start of the old one is read as that start (the old rows after it are dropped)");
    }
    // the same bytes again under a new signature: an addition of nothing
    {
        writeFile(c + "end,259\n");
        asked("sigK");
        const unsigned long long h = dp::g_groupIncHits;
        const dp::GroupTimeline& g = asked("sigL");
        check(dp::g_groupIncHits == h + 1 && equalsWhole(g, 259), "the same file under a new signature is an addition of no rows");
    }
    // the next generation parts from the old one at a tick (the loop's anchor is a few ticks before the last one's death): the old rows before that tick
    // stand, the rest are the new file's; a uid whose rows were all after it is dropped and comes back in the order a whole parse gives it
    {
        auto rowsAlt = [&](int t0, int t1, int newUid0, int newUids) {   // the same shape as rows(), other values
            std::string r;
            for (int t = t0; t < t1; ++t) {
                for (int u = 1; u <= 40; ++u)
                    if ((u + t) % 5 == 0) { char b[160]; std::snprintf(b, sizeof b, "%d,%d,%.3f,%.3f,30.000,30.000,%d,%.3f\n", t, u, 7.0 + t * 0.5 + u, 9.0 + u, 1, 0.0); r += b; }
                if (t == t0 + 3)
                    for (int k = 0; k < newUids; ++k) { char b[160]; std::snprintf(b, sizeof b, "%d,%d,5.000,6.000,30.000,30.000,1,0.000\n", t, newUid0 + k); r += b; }
            }
            return r;
        };
        dp::g_groupLayersCache = dp::GroupLayersCache{};
        const std::string base0 = head + rows(1, 100, 100, 2) + "end,99\n";   // uids 100, 101 first appear at tick 6 of this call... and in 80..99 below
        writeFile(head + rows(1, 80, 200, 0) + rows(80, 100, 100, 2) + "end,99\n");   // the old file: uids 100, 101 first move at tick 85
        asked("p1");
        const unsigned long long h = dp::g_groupIncHits;
        writeFile(head + rows(1, 80, 200, 0) + rowsAlt(80, 130, 300, 1) + rowsAlt(130, 150, 100, 2) + "end,149\n");   // parts at tick 80; uid 300 new at 83, 100 / 101 at 133
        const dp::GroupTimeline& g1 = asked("p2");
        check(dp::g_groupIncHits == h + 1 && equalsWhole(g1, 149), "a generation that parts from the old one at a tick keeps the rows before it and parses the rest (a uid whose rows were all after it comes back in a whole parse's order)");
        // parting inside the last tick's lines, and at the very last tick
        writeFile(head + rows(1, 80, 200, 0) + rowsAlt(80, 130, 300, 1) + rowsAlt(130, 150, 100, 2) + rowText(150, 5) + "end,150\n");
        const dp::GroupTimeline& g2 = asked("p3");
        check(dp::g_groupIncHits == h + 2 && equalsWhole(g2, 150), "the same file with a row added after the last tick is an addition after it");
        std::string last = head + rows(1, 80, 200, 0) + rowsAlt(80, 130, 300, 1) + rowsAlt(130, 150, 100, 2);
        writeFile(last + rowText(150, 6) + "end,150\n");   // the last tick's row differs from the old file's last tick's
        const dp::GroupTimeline& g3 = asked("p4");
        check(dp::g_groupIncHits == h + 3 && equalsWhole(g3, 150), "a generation that parts in the old file's last tick keeps everything before that tick");
        // parting at the first tick: nothing of the old rows stands, the file is parsed whole
        writeFile(head + rowsAlt(1, 60, 100, 2) + "end,59\n");
        const dp::GroupTimeline& g4 = asked("p5");
        check(dp::g_groupIncHits == h + 4 && equalsWhole(g4, 59), "a generation that parts at the first tick keeps no row and is the whole parse");
        // a mid-file end line in the old file: not continued from a cut
        writeFile(head + rows(1, 40, 100, 1) + "end,39\n" + rows(40, 70, 100, 1) + "end,69\n");
        asked("p6");
        const unsigned long long h6 = dp::g_groupIncHits;
        writeFile(head + rows(1, 40, 100, 1) + "end,39\n" + rowsAlt(40, 90, 100, 1) + "end,89\n");
        const dp::GroupTimeline& g5 = asked("p7");
        check(dp::g_groupIncHits == h6 && equalsWhole(g5, 89), "an old file with a mid-file end line is parsed whole when the new one parts before its end");
        (void)base0;
    }    // the mod's shape: one --groups file and a --groupsauto recording beside it (both signed)
    {
        const std::string apath = (std::filesystem::temp_directory_path() / "dptest_groups_auto.txt").string();
        { std::ofstream f(apath, std::ios::binary | std::ios::trunc); f << head << rows(1, 60, 100, 2) << "end,59\n"; }
        dp::g_bootAuto = true;
        dp::g_groupAutoPaths = {apath};
        dp::g_groupLayersCache = dp::GroupLayersCache{};
        auto askedAuto = [&](const char* sig) -> const dp::GroupTimeline& { dp::g_fileSigHint.clear(); dp::g_fileSigHint[path] = sig; dp::g_fileSigHint[apath] = "autosig"; return dp::groupLayersFor({path}); };
        writeFile(a + "end,99\n");
        askedAuto("m1");
        const unsigned long long h = dp::g_groupIncHits, t = dp::g_groupIncCalls;
        writeFile(b + "end,179\n");
        const dp::GroupTimeline& gm = askedAuto("m2");
        check(dp::g_groupIncHits == h + 1 && dp::g_groupIncCalls == t + 1 && equalsWhole(gm, 179), "with a --groupsauto recording beside it (the mod's shape) the next generation is still an addition");
        dp::g_groupAutoPaths.clear();
        std::error_code ec2;
        std::filesystem::remove(apath, ec2);
    }
    dp::g_fileSigHint.clear();
    dp::g_groupLayersCache = dp::GroupLayersCache{};
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// dptest --groups-inc <older> <newer>: two real generations of one recording, read through groupLayersFor in turn under the file's own path (the
// older copied to it, then the newer), the second as an addition; the result is compared with the whole parse of the newer file, and the times are printed.
static int groupsIncReal(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: dptest --groups-inc <older> <newer>\n"); return 2; }
    const std::string path = (std::filesystem::temp_directory_path() / "dptest_groups_inc_real.txt").string();
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("  [%s] %.1f s\n", what, std::chrono::duration<double>(t1 - t0).count());
        t0 = t1;
    };
    dp::g_groupLayersCache = dp::GroupLayersCache{};
    std::error_code ec;
    std::filesystem::copy_file(argv[2], path, std::filesystem::copy_options::overwrite_existing, ec);
    dp::g_fileSigHint.clear();
    dp::g_fileSigHint[path] = "older";
    lap("copy older");
    dp::groupLayersFor({path});
    lap("older through groupLayersFor (whole parse)");
    std::filesystem::copy_file(argv[3], path, std::filesystem::copy_options::overwrite_existing, ec);
    dp::g_fileSigHint[path] = "newer";
    lap("copy newer");
    const unsigned long long h0 = dp::g_groupIncHits;
    const dp::GroupTimeline& got = dp::groupLayersFor({path});
    lap("newer through groupLayersFor");
    const bool inc = dp::g_groupIncHits == h0 + 1;
    long long e = -1;
    std::unordered_map<int, uint8_t> in;
    const dp::GroupTimeline ref = dp::loadGroupTimeline(path, &e, &in);
    lap("newer, whole parse for the comparison");
    const std::string d = timelineDiff(ref, got);
    const bool same = d.empty() && e == dp::g_groupsEndT && in == dp::g_groupInit;
    std::printf("%s: addition used %d, rows kept %llu bytes parsed %llu bytes; %s%s\n", same ? "ok" : "FAIL", (int)inc,
                dp::g_groupIncKeptBytes, dp::g_groupIncParsedBytes, d.c_str(), same ? "" : " (or end / init)");
    std::filesystem::remove(path, ec);
    return same && inc ? 0 : 1;
}

// dptest --groups-corpus <file>...: the same comparison on real recordings.
static int groupsCorpus(int argc, char** argv) {
    int bad = 0;
    for (int i = 2; i < argc; ++i) {
        long long rows = 0;
        const std::string d = groupsReadersDiff(argv[i], &rows);
        std::printf("%s: %lld rows %s%s\n", d.empty() ? "ok" : "FAIL", rows, argv[i],
                    d.empty() ? "" : (" -- " + d).c_str());
        bad |= !d.empty();
    }
    return bad;
}

// THE RUN (search_dp.hpp SearchRun). A run is one level's solve: begin refuses while one is open --
// the carry-over a one-session cold run exists to catch -- and end closes it. A verification says
// whether the plan flown was the one this core emitted last (its edges from that plan's anchor on).
void runLifecycle() {
    dp::g_run = dp::SearchRun{};
    std::string why;
    check(dp::runBegin(4, 7, why), "a run begins when none is open");
    check(dp::g_run.open && dp::g_run.level == 4 && dp::g_run.seed == 7,
          "begin records the level and the seed");
    std::string why2;
    check(!dp::runBegin(5, 0, why2), "begin refuses while a run is open");
    check(why2.find("level 4") != std::string::npos && why2.find("level 5") != std::string::npos,
          "the refusal names the open run's level and the level that asked");
    check(dp::g_run.level == 4, "a refused begin leaves the open run as it was");
    dp::g_run.lastT0 = 100;
    dp::g_run.lastEdges = {{101, 1}, {110, 0}};
    const std::vector<std::pair<long long, int>> spliced = {{20, 1}, {30, 0}, {101, 1}, {110, 0}};
    const std::vector<std::pair<long long, int>> other = {{20, 1}, {30, 0}, {102, 1}, {110, 0}};
    check(dp::runOwnPlan(spliced), "a flight of the emitted tail on another prefix is the run's own");
    check(!dp::runOwnPlan(other), "a flight whose tail differs is not");
    // the seam: an emitted edge a tick before the anchor that changes nothing, which the splice drops
    dp::g_run.lastEdges = {{99, 1}, {101, 1}, {110, 0}};
    check(dp::runOwnPlan({{20, 1}, {101, 1}, {110, 0}}),
          "an emitted edge that changes nothing need not be flown");
    check(!dp::runOwnPlan({{20, 1}, {30, 0}, {101, 1}, {110, 0}}),
          "...but a level the emitted plan holds through the seam must be held there");
    dp::g_run.lastEdges = {{101, 1}, {110, 0}};
    const std::string line = dp::runVerified(spliced, 500, 1234.5, false, 480);
    check(line.find("own=1") != std::string::npos && dp::g_run.verified == 1 && dp::g_run.own == 1,
          "runVerified counts the flight and says it was the run's own");
    check(line.find("via=") == std::string::npos, "...and a flight of the run's own plan carries no via");
    // the section search splices another tail from t=200: a flight of it is counted apart
    const std::vector<std::pair<long long, int>> sec = {{20, 1}, {30, 0}, {205, 1}, {230, 0}};
    dp::runNoteSplice(sec, 200);
    const std::string ls = dp::runVerified(sec, 260, 2000.0, false, -1);
    check(ls.find("own=0") != std::string::npos && ls.find(" via=secsolve") != std::string::npos
              && dp::g_run.viaSplice == 1,
          "a flight of the spliced plan is the section search's (via=secsolve)");
    const std::string lo = dp::runVerified(other, 300, 2100.0, false, -1);
    check(lo.find("via=") == std::string::npos && dp::g_run.viaSplice == 1,
          "a flight of neither plan carries no via");
    const std::string e = dp::runEnd();
    check(e.find("level=4") != std::string::npos && e.find(" via-secsolve=1") != std::string::npos
              && !dp::g_run.open,
          "end closes the run and counts the section search's flights");
    const std::string pf = writeTmp("dptest_plan.txt", "input=101,1\ninput=105,0\n");
    check(dp::runNotePlanFile(pf) && dp::g_run.lastEdges.size() == 2
              && dp::g_run.lastEdges[1] == std::make_pair(105LL, 0),
          "the plan a call answered with is read back from its file");
    check(!dp::runNotePlanFile(pf + ".missing") && dp::g_run.lastEdges.size() == 2,
          "a plan file that cannot be read leaves the record as it was");
    check(dp::runBegin(5, 0, why), "the next level's run begins once the last has ended");
    dp::runEnd();
    check(dp::runEnd().find("no run open") != std::string::npos, "an end with no run open says so");
}


// --rotboost: a 2900 that swaps the axes stores the velocity-limit exemption (+0x952) as 1, then as
// 0 when the trigger has Dont Slide (rotateGameplay 0x399faa / 0x39a00d). Both are stores, so the
// byte the body arrived with does not survive the swap either way; with the flag off the model
// leaves it alone. One trigger at x=100, a ship crossing it in frame 0.
void rotBoostDontSlide() {
    const auto savedTrig = dp::g_rotTrig;
    const bool savedFlag = dp::g_rotBoost;
    auto run = [](bool flag, int boostIn, int dontSlide) {
        dp::g_rotTrig.clear();
        dp::RotTrig rt{100.0, 0.0, 3};
        rt.setRev = 0;
        rt.dontSlide = (uint8_t)dontSlide;
        dp::g_rotTrig.push_back(rt);
        dp::g_rotBoost = flag;
        dp::State c{};
        c.mode = 1;
        c.xAbs = 101.f;
        c.boost = (uint8_t)boostIn;
        const int nf = dp::applyRotation(c, 99.0, 1.95, 1);
        return nf == 3 && c.frame == 3 ? (int)c.boost : -1;
    };
    for (int b = 0; b <= 1; ++b) {
        check(run(true, b, 0) == 1, b ? "--rotboost: latched in, Dont Slide 0 -> latched"
                                       : "--rotboost: unlatched in, Dont Slide 0 -> latched");
        check(run(true, b, 1) == 0, b ? "--rotboost: latched in, Dont Slide 1 -> unlatched"
                                       : "--rotboost: unlatched in, Dont Slide 1 -> unlatched");
        check(run(false, b, 0) == b && run(false, b, 1) == b,
              b ? "flag off: a latched body keeps its byte through the swap"
                : "flag off: an unlatched body keeps its byte through the swap");
    }
    dp::g_rotTrig = savedTrig;
    dp::g_rotBoost = savedFlag;
}


// --moverextend's data form needs the Move's start: a run of constant-velocity rows that goes back to
// the object's OWN first row has nothing before it that dates the Move. Object A stands still at t=0 (a row), then
// moves from t=90 (the gap dates the start): extended for the Move's remaining 50 - 12 ticks (12 steps since t=90). Object B is first
// recorded at t=100 with 3 rows: its first row is not shown to be the Move's start, so it is NOT extended, although
// another object's rows begin at t=0 (a recording-wide minimum would have called B's start known). Object C is B
// with one earlier row at another velocity: the change dates it, so it is extended (2 steps, cap 48).
void moverExtendStart() {
    dp::Dynamics d;
    d.objs.resize(3);
    d.samples.assign(3, {});
    auto row = [](int t, float cx) { dp::DynSample s{}; s.t = t; s.cx = cx; s.cy = 0.f; s.hw = s.hh = 5.f; s.on = 1; return s; };
    d.samples[0].mut().push_back(row(0, 0.f));
    for (int t = 90; t <= 102; ++t) d.samples[0].mut().push_back(row(t, 2.f * (float)(t - 90)));
    for (int t = 100; t <= 102; ++t) d.samples[1].mut().push_back(row(t, 500.f + 2.f * (float)(t - 100)));
    d.samples[2].mut().push_back(row(99, 800.f));
    for (int t = 100; t <= 102; ++t) d.samples[2].mut().push_back(row(t, 805.f + 2.f * (float)(t - 100)));
    dp::Dynamics::AutoPart mv{};
    mv.dx = 100.f; mv.dy = 0.f; mv.dur = 50.0; mv.ease = 0;
    d.partsAny.assign(3, std::vector<dp::Dynamics::AutoPart>{mv});
    d.prepareExtend();
    check(d.extOk[0] == 1 && d.extCap[0] == 38, "--moverextend: stood still, then moved: start dated by the gap -> extended (cap 38)");
    check(d.extOk[1] == 0, "--moverextend: first recorded mid-Move at t=100 while another object has rows from t=0 -> not extended");
    check(d.extOk[2] == 1 && d.extCap[2] == 48, "--moverextend: a velocity change dates the start -> extended (cap 48)");
}
// --slambox: the toward-the-player steps an object takes from the boxes the state touched,
// each part decided the way dynamics.hpp's slam loop decides it -- skipped iff slamBoxRecorded says the
// recorded attempt fired that same box on the state's own entry tick. `fire[b]` is the state's entry into box b
// (-1 = not touched), `rec` the recorded attempt's (--rectouch). Returns the summed dy the state adds.
struct SlamPart { int box; float dy; };
float slamSum(const std::vector<SlamPart>& parts, const std::array<int, dp::kTouchBits>& fire,
              const std::array<int, dp::kTouchBits>& rec) {
    float sum = 0.f;
    for (const SlamPart& p : parts) {
        if (p.dy >= 0.f) continue;
        const int F = fire[(size_t)p.box];
        if (F < 0) continue;
        if (dp::slamBoxRecorded(rec, p.box, F)) continue;
        sum += p.dy;
    }
    return sum;
}
void slamBoxIdentity() {
    auto none = [] { std::array<int, dp::kTouchBits> a{}; a.fill(-1); return a; };
    const int A = 1, B = 2;
    {   // two candidate boxes; the recording holds A; the state enters A on its tick and B later
        auto fire = none(), rec = none();
        rec[A] = 100; fire[A] = 100; fire[B] = 120;
        check(slamSum({{A, -9.f}, {B, -9.f}}, fire, rec) == -9.f,
              "--slambox: A recorded and entered on its tick -> skipped; B, not recorded -> kept");
    }
    {   // the old rule's hole: the recording holds A, the state touches only B, on A's tick
        auto fire = none(), rec = none();
        rec[A] = 100; fire[B] = 100;
        check(slamSum({{A, -9.f}, {B, -9.f}}, fire, rec) == -9.f,
              "--slambox: only B touched (the recording's touch is A) -> B keeps its step");
    }
    {   // two boxes hit on the same tick, only A in the recording
        auto fire = none(), rec = none();
        rec[A] = 100; fire[A] = 100; fire[B] = 100;
        check(slamSum({{A, -9.f}, {B, -4.f}}, fire, rec) == -4.f,
              "--slambox: A and B on the same tick, only A recorded -> A skipped, B kept");
    }
    {   // one box, two slam parts
        auto fire = none(), rec = none();
        rec[A] = 100; fire[A] = 100;
        check(slamSum({{A, -9.f}, {A, -3.f}}, fire, rec) == 0.f,
              "--slambox: one recorded box with two parts -> both skipped");
    }
    {   // the same box entered on another tick is another activation
        auto fire = none(), rec = none();
        rec[A] = 100; fire[A] = 101;
        check(slamSum({{A, -9.f}}, fire, rec) == -9.f,
              "--slambox: the recorded box entered a tick later -> kept (delta 0 only)");
    }
    {   // the witness's shape: -9 / 0 / +300 boxes, the recording fired -9 and +300, the state likewise
        auto fire = none(), rec = none();
        const int b19 = 19, b20 = 20, b23 = 23;
        rec[b19] = 5119; rec[b23] = 5132; fire[b19] = 5119; fire[b23] = 5132;
        check(slamSum({{b19, -9.f}, {b20, 0.f}, {b23, 300.f}}, fire, rec) == 0.f,
              "--slambox: the plate's three boxes, the -9 recorded on its tick -> no second -9");
    }
}
// The recorded band's source through the writer and the reader (band_rows.hpp, bands.hpp loadBandTrack).
// A band whose source goes camera -> portal -> camera with the same numbers used to write one row, and the
// lookup gave the first row's source for all three ticks. The writer now adds an S row for a change of
// source alone; the reader keeps S rows out of the track, so every reader of the rows sees the rows a
// file without them gives.
namespace {
void clearBandTrack() {
    dp::g_bandTrack.clear();
    dp::g_bandTrackIntervals.clear();
    dp::g_bandSrcRows.clear();
    dp::g_bandTrackCam = -1;
    dp::g_bandTrackEnd = -1;
}
}  // namespace
// --formularec reads the recording only when it is the anchor's own attempt, up to the tick it reached
// (--recprov). The same tick and the same box from another attempt is not that attempt's world.
void formulaRecProvenance() {
    auto with = [](bool given, long long rec, long long anc, long long reach, long long t) {
        dp::g_recProvGiven = given;
        dp::g_recProvRec = rec;
        dp::g_recProvAnchor = anc;
        dp::g_recProvReach = reach;
        return dp::formulaRecProvOk(t);
    };
    check(with(true, 41, 41, 3000, 2500), "--recprov: one attempt, before its reach -> read");
    check(!with(true, 47, 41, 3000, 2500), "--recprov: the recording is another attempt's -> not read");
    check(!with(true, 41, 41, 3000, 3001), "--recprov: past the tick the recording reached -> not read");
    check(!with(false, -1, -1, -1, 2500), "--recprov: not given (an older mod) -> not read");
    check(!with(true, -1, -1, 3000, 2500), "--recprov: unknown attempts on both sides -> not read");
    with(false, -1, -1, -1, 0);
}
// --tog-corpus GD_DIR PLAN OUT: the world's tog table and togStep walked over a game flight's own x (GD_DIR/dump.csv) and a plan's button, tick by tick -- no model, so the x is exact. Writes `tick,group,on` for every
// Tap call (the form of py tap_events.py's output, to be compared as sets) and `gate,tick,uid,fired` lines for every x-gate crossing after them, and prints the xgate digest at the end.
static int togCorpus(int argc, char** argv) {
    if (argc < 5) { std::printf("usage: dptest --tog-corpus GD_DIR PLAN OUT [ORBFIRE.csv [DUMP.csv]]\n"); return 2; }
    const std::string gd = argv[2];
    const auto* rows = dp::trigRowsCached(gd + "/triggers.txt");
    const auto* mem = dp::groupMembersCached(gd + "/objgroups.txt");
    if (!rows || !mem) { std::printf("togcorpus: cannot read triggers.txt / objgroups.txt\n"); return 2; }
    dp::gdtrig::TogLoadReport rep;
    const dp::gdtrig::TogTable T = dp::gdtrig::buildTogTable(*rows, *mem, &rep);
    std::printf("togcorpus: %d Tap(s), %d gated trigger(s), %d group(s), %d orb group(s), %d gated only by orbs\n", rep.taps, rep.gates, rep.groups, rep.orbGroups, rep.gatesOnlyOrb);
    std::vector<std::pair<long long, int>> ins;
    {
        std::ifstream pf(argv[3]);
        std::string ln;
        while (std::getline(pf, ln)) {
            if (ln.compare(0, 6, "input=") != 0) continue;
            long long t = 0; int b = 0;
            if (std::sscanf(ln.c_str() + 6, "%lld,%d", &t, &b) == 2) ins.push_back({t, b});
        }
        std::sort(ins.begin(), ins.end());
    }
    bool pushMode = false;
    std::map<long long, std::vector<int>> orbAt;   // optional 5th argument: the game's orbfire.csv (tick,uid,player,via) -- the Toggle Orbs it hit, applied at that tick
    if (argc > 5) {
        pushMode = !std::strncmp(argv[5], "push:", 5);   // "push:FILE": the orbfire.csv is only the reference, and the Toggle Orbs hit are computed by the push path (togOrbPushHits)
        std::ifstream of(pushMode ? argv[5] + 5 : argv[5]);
        std::string ln;
        std::getline(of, ln);
        while (std::getline(of, ln)) {
            long long t = 0; int u = 0;
            if (std::sscanf(ln.c_str(), "%lld,%d", &t, &u) == 2) orbAt[t].push_back(u);
        }
        for (auto& kv : orbAt) std::sort(kv.second.begin(), kv.second.end());
    }
    std::vector<double> xs, ys, vzs;   // per dump row (row tick 1 is the first)
    std::vector<uint8_t> sps;
    {
        std::ifstream df(argc > 6 ? std::string(argv[6]) : gd + "/dump.csv");   // optional 6th argument: the flight's own dump.csv
        std::string ln;
        std::getline(df, ln);
        std::vector<std::string> hd;
        { std::stringstream hs(ln); std::string f; while (std::getline(hs, f, ',')) hd.push_back(f); }
        auto col = [&](const char* n) { for (size_t k = 0; k < hd.size(); ++k) if (hd[k] == n) return (int)k; return -1; };
        const int cT = col("tick"), cX = col("x"), cY = col("y"), cV = col("vsize"), cM = col("mode");
        long long last = 0;
        while (std::getline(df, ln)) {
            std::vector<std::string> f;
            { std::stringstream ls(ln); std::string s; while (std::getline(ls, s, ',')) f.push_back(s); }
            if (cT < 0 || cX < 0 || (int)f.size() <= std::max(cT, cX)) continue;
            const long long tk = std::atoll(f[(size_t)cT].c_str());
            if (tk <= last) break;   // the next attempt
            last = tk;
            if ((long long)xs.size() <= tk) { xs.resize((size_t)tk + 1, 0.0); ys.resize((size_t)tk + 1, 0.0); vzs.resize((size_t)tk + 1, 1.0); sps.resize((size_t)tk + 1, 0); }
            xs[(size_t)tk] = std::atof(f[(size_t)cX].c_str());
            if (cY >= 0 && (int)f.size() > cY) ys[(size_t)tk] = std::atof(f[(size_t)cY].c_str());
            if (cV >= 0 && (int)f.size() > cV) vzs[(size_t)tk] = std::atof(f[(size_t)cV].c_str());
            if (cM >= 0 && (int)f.size() > cM) sps[(size_t)tk] = f[(size_t)cM] == "spider";
        }
    }    dp::gdtrig::TogBits tog; uint64_t xg = 0;
    std::ofstream out(argv[4]);
    size_t ii = 0; int held = 0, nTap = 0, nGate = 0;
    std::vector<int> pushHits;
    std::vector<std::pair<long long, int>> predicted;
    for (long long t = 0; t + 1 < (long long)xs.size(); ++t) {
        int input = held;
        while (ii < ins.size() && ins[ii].first <= t) { input = ins[ii].second; ++ii; }
        std::vector<dp::gdtrig::TogChange> ch;
        const std::vector<int>* hitsP = nullptr;
        if (pushMode) {
            dp::gdtrig::OrbPlayer pl; pl.x = xs[(size_t)t]; pl.y = ys[(size_t)t]; pl.vsize = vzs[(size_t)t]; pl.spider = sps[(size_t)t] != 0;
            dp::gdtrig::togOrbPushHits(T, pl, input && !held, pushHits);
            for (int u : pushHits) predicted.push_back({t, u});
            hitsP = &pushHits;
        } else {
            const auto oa = orbAt.find(t);
            if (oa != orbAt.end()) hitsP = &oa->second;
        }
        dp::gdtrig::togStep(T, tog, xg, xs[(size_t)t], xs[(size_t)t + 1], held, input, &ch, hitsP);
        for (const auto& e : ch) {
            if (e.gate) { out << "gate," << t << "," << e.uid << "," << (e.fired ? 1 : 0) << "\n"; ++nGate; }
            else { out << t << "," << e.group << "," << (e.on ? 1 : 0) << "\n"; ++nTap; }
        }
        held = input;
    }
    std::printf("togcorpus: %zu ticks, %zu plan edges, %d Tap call(s), %d gate crossing(s), tog %016llx%016llx, xgate %016llx\n", xs.size(), ins.size(), nTap, nGate,
                (unsigned long long)tog.w[1], (unsigned long long)tog.w[0], (unsigned long long)xg);
    if (pushMode) {
        // the Toggle Orbs the push path predicts against the game's own orbfire.csv rows for the same orbs
        int match = 0, extra = 0, missed = 0;
        for (const auto& p : predicted) {
            const auto it = orbAt.find(p.first);
            if (it != orbAt.end() && std::find(it->second.begin(), it->second.end(), p.second) != it->second.end()) ++match;
            else { ++extra; std::printf("togcorpus: predicted orb %d at tick %lld, the game did not\n", p.second, p.first); }
        }
        for (const auto& kv : orbAt)
            for (int u : kv.second) {
                if (!T.findOrb(u)) continue;
                if (std::find(predicted.begin(), predicted.end(), std::make_pair(kv.first, u)) == predicted.end()) { ++missed; std::printf("togcorpus: the game hit orb %d at tick %lld, not predicted\n", u, kv.first); }
            }
        std::printf("togcorpus: push path, Toggle Orb hits: %d matched, %d predicted but not in the game's file, %d in the game's file but not predicted\n", match, extra, missed);
    }
    return 0;
}

// P9 (gd-04): the shadow's ring fire-order record (SelfShadow::fireOrder, ShadowLite::fired) is the block's, not the thread's. A state A packed, a state B unpacked, stepped (a fire) and packed, then A unpacked
// again: A's five are what they were, and the next step from A gives what a thread that never saw B gives. The dual's player-2 unpack must not wipe player 1's record (slUnpackKeepFire).
void fireOrderFixtures() {
    using namespace dp::gdcol;
    auto mk = [](int uid, int type, float cx, float cy) { GObj o; o.uid = uid; o.type = type; o.cx = cx; o.cy = cy; o.prevCx = cx; o.prevCy = cy; o.rw = 36; o.rh = 36; o.rx = cx - 18; o.ry = cy - 18; return o; };
    std::vector<GObj> world = {mk(1, 0xb, 400, 200), mk(2, 0xb, 450, 200), mk(3, 0xb, 500, 200)};
    ContactShadow cs; ShadowIn in; in.x = 300.f; in.y = 105.f; in.grounded = true; cs.seed(in); cs.applyBody(in);
    const std::vector<uint8_t> tmplImg = cs.img.b;
    auto setup = [&](SelfShadow& sh, SlWorld& W) {
        sh.world = &world; sh.index(); sh.setMovers({}, false);
        Layer L; L.speedA = tmpl::kSpeedA; L.speedB = tmpl::kSpeedB; L.w764 = tmpl::kW764; L.scaleX = tmpl::kScaleX; L.scaleY = tmpl::kScaleY; L.ceilWord = tmpl::kCeilWord; L.maxPortalY = tmpl::kMaxPortalY; L.minPortalY = tmpl::kMinPortalY;
        sh.L = L;
        W.build(&world, std::vector<uint8_t>(world.size(), 0));
        sh.img.b = tmplImg;
    };
    auto five = [](const SelfShadow& sh) { std::vector<int> v = sh.fireOrder; return v; };
    // the thread: A and B alternate on one SelfShadow
    SelfShadow sh; SlWorld W; setup(sh, W);
    ShadowLite slA, slB;
    sh.fireOrder = {101, 102, 103, 104, 105}; slPack(sh, W, slA);
    sh.fireOrder = {7, 8}; slPack(sh, W, slB);
    // B steps: unpack B, a fire (noteFired), pack
    slUnpack(slB, tmplImg, W, sh, -3.0e38f);
    check(five(sh) == std::vector<int>({7, 8}), "fire order: unpacking B gives B's record, not what the thread held before");
    noteFired(&sh.fireOrder, 9);
    check(five(sh) == std::vector<int>({9, 7, 8}), "fire order: a fire goes to the front");
    ShadowLite slB2; slPack(sh, W, slB2);
    // A is unpacked again: its five are what they were
    slUnpack(slA, tmplImg, W, sh, -3.0e38f);
    check(five(sh) == std::vector<int>({101, 102, 103, 104, 105}), "fire order: A unpacked after B's step has A's five, in A's order");
    // the next step from A against a thread that never saw B
    SelfShadow shC; SlWorld WC; setup(shC, WC);
    slUnpack(slA, tmplImg, WC, shC, -3.0e38f);
    noteFired(&sh.fireOrder, 106); noteFired(&shC.fireOrder, 106);
    ShadowLite nA, nC; slPack(sh, W, nA); slPack(shC, WC, nC);
    check(nA.nFired == nC.nFired && std::memcmp(nA.fired, nC.fired, sizeof nA.fired) == 0 && nA.nFired == 5 && nA.fired[0] == 106 && nA.fired[4] == 104, "fire order: the next step from A packs the same record as on a thread that never saw B (newest first, the oldest of six drops)");
    // a ring that fires twice in a row is one entry
    noteFired(&sh.fireOrder, 106);
    check(sh.fireOrder.size() == 5 && sh.fireOrder[0] == 106 && sh.fireOrder[1] == 101, "fire order: the same ring twice in a row is one entry");
    // the dual's player-2 unpack keeps player 1's record
    sh.fireOrder = {55, 56, 57}; ShadowLite slEmpty;
    slUnpackKeepFire(slEmpty, tmplImg, W, sh, -3.0e38f, false);
    check(five(sh) == std::vector<int>({55, 56, 57}), "fire order: the player-2 unpack (slUnpackKeepFire) keeps player 1's record");
    slUnpack(slEmpty, tmplImg, W, sh, -3.0e38f, false);
    check(sh.fireOrder.empty(), "fire order: a plain unpack of a block with no record empties it (why the dual's unpack needs the guard)");
}
// the 11-11 audit: the search key reads gdHS & 3 and gdAux & 6 as separate bits, so a state with a pending spend (the press step's consumer has spent S / H, applied one step later: gdAux bit 1 / 2) and a state whose
// body already carries the spent byte (what --shadowhs writes: the body's H, S, pend 0) have the same EFFECTIVE press and not the same key. A pair of each (single body and a dual's second body), the keys before and after the
// derivation, and the next tick's answers: hsTop's registers for a hold, a press and a release, the ring gate and a ring fired on the Old register.
void pressPendFixtures() {
    using namespace dp;
    const bool savedSlots = g_ringSlots, savedKey = g_ringSlotsKey, savedHs = g_hsShadow;
    g_ringSlots = true; g_ringSlotsKey = true; g_hsShadow = true;
    const long long t = 5000;
    // single body: effective H = 1, S = 0
    State A{}, B{};
    A.gdHS = (uint8_t)(kHsH | kHsS | kHsHm | kHsSm); A.gdAux = (uint8_t)(8 | 2);   // S spent in the press step, still up in the byte
    B.gdHS = (uint8_t)(kHsH | kHsHm | kHsSm);        B.gdAux = 8;
    gdcol::Mem body;  body.u8(0x98d, 1); body.u8(0x98e, 0);                        // the shadow body: H up, S spent
    State A1 = A; inputHsDerive(A, body, false, A1);
    check(keyOf(A, t) != keyOf(B, t), "pend fixture: the pending state and the settled one are not the same key now");
    check(keyOf(A1, t) == keyOf(B, t) && A1.gdHS == B.gdHS && A1.gdAux == B.gdAux, "pend fixture: after the derivation the pair is the same key and the same bytes");
    // a dual's second body: the high nibble of gdAux and gdHS2
    State A2{}, B2{};
    A2.dual = B2.dual = 1;
    A2.gdHS = B2.gdHS = (uint8_t)(kHsH | kHsHm | kHsSm); A2.gdAux = (uint8_t)(0x88 | 0x20); B2.gdAux = 0x88;
    A2.gdHS2 = (uint8_t)(kHsH | kHsS | kHsHm | kHsSm); B2.gdHS2 = (uint8_t)(kHsH | kHsHm | kHsSm);
    State A3 = A2; inputHsDerive(A2, body, true, A3);
    check(keyOf(A2, t) != keyOf(B2, t), "pend fixture (dual): the pending second body and the settled one are not the same key now");
    check(keyOf(A3, t) == keyOf(B2, t) && A3.gdHS2 == B2.gdHS2 && A3.gdAux == B2.gdAux, "pend fixture (dual): after the derivation the pair is the same key and the same bytes");
    g_ringSlots = savedSlots; g_ringSlotsKey = savedKey; g_hsShadow = savedHs;
}
// the 11-11 audit (revOff): the search key reads State::revOff in 0.25 px buckets (lround(r * 4), half away from zero), so two values a rounding apart that sit on either side of a bucket boundary are two keys, positive and negative alike. The fieldcheck
// family stale_books used to give the old tick's value back when the two were within 1e-3; the pairs below are within 1e-3 and are different keys, which is why the comparison is now raw and the key's own comparison is the stale_revoff_key family.
void revOffKeyFixtures() {
    using namespace dp;
    const long long t = 5000;
    auto keyWith = [t](float r) { State s{}; s.revOff = r; return keyOf(s, t); };
    struct P { float a, b; bool same; const char* what; };
    const P pairs[] = {
        {0.1249f, 0.1251f, false, "+0.1249 / +0.1251 (buckets 0 and 1)"},
        {-0.1249f, -0.1251f, false, "-0.1249 / -0.1251 (buckets 0 and -1)"},
        {0.3749f, 0.3751f, false, "+0.3749 / +0.3751 (buckets 1 and 2)"},
        {-0.3749f, -0.3751f, false, "-0.3749 / -0.3751 (buckets -1 and -2)"},
        {0.125f, -0.125f, false, "+0.125 / -0.125 (half away from zero: buckets 1 and -1)"},
        {0.5f, -0.5f, false, "+0.5 / -0.5"},
        {0.2f, 0.3f, true, "+0.2 / +0.3 (both bucket 1)"},
        {-0.2f, -0.3f, true, "-0.2 / -0.3 (both bucket -1)"},
        {0.0f, 0.00001f, true, "0 / +1e-5 (a nonzero revOff in bucket 0 is the key of none)"},
        {0.0f, -0.00001f, true, "0 / -1e-5"},
    };
    for (const P& p : pairs) {
        const bool keySame = keyWith(p.a) == keyWith(p.b), bucketSame = revOffBucket(p.a) == revOffBucket(p.b);
        check(keySame == p.same, (std::string("revOff key: ") + p.what + (p.same ? " is one key" : " is two keys")).c_str());
        check(keySame == bucketSame, (std::string("revOff key: the key and revOffBucket agree for ") + p.what).c_str());
    }
    check(std::fabs(0.1249f - 0.1251f) < 1e-3f && keyWith(0.1249f) != keyWith(0.1251f), "revOff key: a pair inside the old 1e-3 tolerance is two keys");
}

// FireReadSet / FireTicks / FireHi (dynamics.hpp): a group's touch fire ticks note which of them a placement read, when a set is in; dupbatch asks a follower for the leader's value of those only.
static void fireReadFixtures() {
    using namespace dp;
    uint16_t fb[kTouchBits] = {};
    fb[1] = 100; fb[2] = 200; fb[3] = 300;
    {
        const FireTicks plain(fb);
        check((bool)plain && plain[3] == 300 && !(bool)FireTicks(nullptr), "fireread: without a set a read is the plain load, and a null array reads as none");
    }
    FireReadSet s;
    const FireTicks rd(fb, &s);
    check(s.count() == 0 && !s.scalar.load() && !s.all.load(), "fireread: a new set has nothing read");
    check(rd[3] == 300 && s.test(3) && !s.test(2) && s.count() == 1 && !s.scalar.load(), "fireread: a read of one box notes that box and nothing else");
    check((bool)rd && s.count() == 1, "fireread: asking whether there is an array is not a read");
    {
        const FireHi hi{4242, &s};
        const int v = hi;
        check(v == 4242 && s.scalar.load() && s.count() == 1, "fireread: the scalar's read is noted apart from the boxes");
        const FireHi quiet{7, nullptr};
        check((int)quiet == 7, "fireread: the scalar without a set is its value");
    }
    {   // a helper that takes the ticks by value notes into the caller's set: the Stop of a mover asks only the entered boxes that stop THAT mover
        const std::vector<std::vector<int>> keep = g_touchStops;
        g_touchStops.assign(4, {});
        g_touchStops[1] = {77}; g_touchStops[2] = {88}; g_touchStops[3] = {77};
        FireReadSet q;
        TouchMask m{}; m |= touchBit(1); m |= touchBit(2);   // box 3 stops the mover too, but the group has not entered it
        check(touchStopTick(m, FireTicks(fb, &q), 77) == 100 && q.test(1) && !q.test(2) && !q.test(3) && q.count() == 1, "fireread: touchStopTick notes the entered box that stops the mover, not the others");
        g_touchStops = keep;
    }
    s.clear();
    check(s.count() == 0 && !s.scalar.load() && !s.all.load(), "fireread: clear empties the set");
}

// TrigShadow::objsHash: the hash dupbatch takes of the objects as they stood BEFORE a group's step (the supply's previous place and on / off come from there). Two timelines can step to one
// content from two different ones; the hash of the objects before the step tells them apart, and a copy that shares its pages hashes the same.
static void preStepHashFixtures() {
    using dp::gdtrig::CowPages;
    using dp::gdtrig::ObjState;
    using dp::gdtrig::TrigShadow;
    CowPages<ObjState> landed;   // a mover that reached y = 100 a tick ago
    for (int i = 0; i < 40; ++i) { ObjState o; o.uid = i + 1; o.x = 10.0 * i; o.y = 100.0; landed.push_back(o); }
    CowPages<ObjState> moving = landed;   // ...and one still on its last step: it stands at 99.992 before the step
    moving.mut(7).y = 99.992;
    CowPages<ObjState> afterA = landed, afterB = moving;
    afterB.mut(7).y = 100.0;   // the step lands it
    check(TrigShadow::objsHash(afterA) == TrigShadow::objsHash(afterB), "prestep: the two timelines hold the same objects after the step");
    check(TrigShadow::objsHash(landed) != TrigShadow::objsHash(moving), "prestep: the objects before the step tell the two timelines apart");
    const CowPages<ObjState> copy = moving;
    check(TrigShadow::objsHash(copy) == TrigShadow::objsHash(moving), "prestep: a copy that shares the pages has the same hash");
    CowPages<ObjState> off = landed;
    off.mut(3).counter = -1;   // switched off before the step: the supply's on / off (pon) is read from there
    check(TrigShadow::objsHash(off) != TrigShadow::objsHash(landed), "prestep: an object switched off before the step is another hash");
}

// CowPages (shared_const.hpp): the trigger shadow's objects copy by sharing pages; a write clones the one page it writes and never reaches a copy made earlier.
void cowPagesFixtures() {
    using dp::gdtrig::CowPages;
    CowPages<int, 2> a;   // 4 to a page
    for (int i = 0; i < 10; ++i) a.push_back(i * 10);
    check(a.size() == 10 && a.pageCount() == 3 && a[0] == 0 && a[9] == 90 && a.front() == 0 && a.back() == 90, "cowpages: push_back fills pages in order");
    const long long c0 = dp::gdtrig::g_cowPageClones.load();
    CowPages<int, 2> b = a;
    check(dp::gdtrig::g_cowPageClones.load() == c0 && b.ownedPages() == 0 && a.ownedPages() == 0, "cowpages: a copy clones no page and shares every one");
    b.mut(5) = 555;
    check(a[5] == 50 && b[5] == 555, "cowpages: a write to the copy does not reach the one it was copied from");
    check(dp::gdtrig::g_cowPageClones.load() == c0 + 1 && b.ownedPages() == 1, "cowpages: the write cloned exactly the page it wrote");
    b.mut(6) = 666; b.mut(4) = 444;
    check(dp::gdtrig::g_cowPageClones.load() == c0 + 1 && a[4] == 40 && a[6] == 60 && b[4] == 444 && b[6] == 666, "cowpages: more writes to an owned page clone nothing");
    a.mut(5) = 5;   // the page b left: a is its only holder now
    check(dp::gdtrig::g_cowPageClones.load() == c0 + 1 && a[5] == 5 && b[5] == 555, "cowpages: the page the copy left is written in place by its last holder");
    a.mut(0) = 1;   // a page both still share
    check(dp::gdtrig::g_cowPageClones.load() == c0 + 2 && a[0] == 1 && b[0] == 0, "cowpages: a write to the original does not reach the copy either");
    {
        const int& before = b[9];   // a reference taken before the page is cloned
        CowPages<int, 2> c = b;
        c.mut(8) = 8;
        check(before == 90 && &before == &b[9] && c[9] == 90 && c[8] == 8 && b[8] == 80, "cowpages: a reference into a page stays valid when a copy clones that page");
    }
    long long sum = 0; size_t n = 0;
    for (const int& v : b) { sum += v; ++n; }
    check(n == 10 && sum == 0 + 10 + 20 + 30 + 444 + 555 + 666 + 70 + 80 + 90, "cowpages: range-for reads every element once, in order");
    CowPages<int, 2> s;
    s.assign(10, 7);
    bool all7 = s.size() == 10; for (const int& v : s) all7 = all7 && v == 7;
    s.mut(9) = 1;
    check(all7 && s[9] == 1 && s[8] == 7 && s[0] == 7 && s.ownedPages() == 1, "cowpages: assign fills with one value; the first write to a page takes it out of the shared fill");
    // the shadow itself: a copy that writes an object leaves the parent's object alone
    dp::gdtrig::TrigShadow P;
    for (int u = 1; u <= 40; ++u) P.addObject(u, 30.0 * u, 15.0, 0.0, 0, {7});
    dp::gdtrig::TrigShadow K = P;
    K.setObjectType(33, 0);   // writes ObjState::noOwnRot of the object with uid 33
    check(!P.objs[32].noOwnRot && K.objs[32].noOwnRot && K.objs[31].x == P.objs[31].x && K.objs.ownedPages() == 1, "cowpages: a copy of the shadow that writes one object clones one page and leaves the parent's object as it was");
    // the content hash (TrigShadow::contentHash): the same for a copy, changed by any state a step reads, and after a write only the written page is hashed anew
    const dp::gdtrig::Hash2 hP = P.contentHash();
    check(hP == K.contentHash(), "content hash: a copy that differs only in what the load fixed (noOwnRot) has the parent's hash");
    dp::gdtrig::TrigShadow Q = P;
    const long long n0 = Q.objs.hashedPages();
    check(Q.contentHash() == hP && Q.objs.hashedPages() == n0, "content hash: a copy's pages carry their hashes, nothing is hashed anew");
    Q.objs.mut(5).x = std::nextafter(Q.objs[5].x, 1e9);
    check(Q.contentHash() != hP && Q.objs.hashedPages() == n0 + 1, "content hash: one bit of one object's x changes it, and one page is hashed anew");
    Q.objs.mut(5).x = P.objs[5].x;
    check(Q.contentHash() == hP, "content hash: written back, it is the parent's again");
    Q.objs.mut(5).lastTrig = 99;
    check(Q.contentHash() == hP, "content hash: the last trigger that acted on an object (a report) is not in it");
    Q.setGroupBit(7, false);
    check(Q.contentHash() != hP && Q.trackHash() == P.trackHash(), "content hash: a group's switch is in it, and not in the track hash");
    // the item state (T57: Pickup / Item Edit / Item Persist write it)
    dp::gdtrig::TrigShadow R = P;
    R.setItemForTest(3, 5);
    check(R.contentHash() != hP, "content hash: an item's number is in it");
    dp::gdtrig::TrigShadow R2 = P;
    R2.setItemForTest(3, 6);
    check(R2.contentHash() != R.contentHash(), "content hash: two numbers of one item are two worlds");
    R.setItemForTest(3, 0);
    check(R.contentHash() == hP, "content hash: an item written back to 0 is the world that never had it (itemCountOf reads 0 for none)");
    R.setPersistForTest(3, true);
    check(R.contentHash() != hP, "content hash: an item in the persistent set is in it");
    R.setPersistForTest(3, false);
    check(R.contentHash() == hP, "content hash: taken out of the set, it is the parent's again");
}
int main(int argc, char** argv) {    if (argc > 2 && !std::strcmp(argv[1], "--tog-corpus")) return togCorpus(argc, argv);
    if (argc > 2 && !std::strcmp(argv[1], "--groups-corpus")) return groupsCorpus(argc, argv);
    if (argc > 3 && !std::strcmp(argv[1], "--groups-inc")) return groupsIncReal(argc, argv);
    formulaRecProvenance();
    slamBoxIdentity();
    rotBoostDontSlide();
    runLifecycle();
    groupsParser();
    groupsDifferential();
    commaFields();
    overlayReach();
    initLines();
    noCollideColumn();
    keptTextPass();
    onOffEvents();
    activatorRoots();
    autoSwitches();
    keyHoldsDelayedSwitch();
    layerDigestIsAProjection();
    cameraSummaryFixtures();
    worldPartsFixtures();
    orbProducerFixtures();
    togStateFixtures();
    supplyCameraFixtures();
    eventTriggerFixtures();
    eventChainFixtures();
    moveToAnyMemberFixtures();
    tapToggleSpawnFixtures();
    camLockFixtures();
    editByEffectFixtures();
    settingsInertFixtures();
    stateSerialFixtures();
    fieldCheckFixtures();
    supplyTouchFixtures();
    portAuditPairsBeforeDedupe();
    capAuditWorlds();
    moverExtendStart();
    fireOrderFixtures();
    pressPendFixtures();
    revOffKeyFixtures();
    cowPagesFixtures();
    fireReadFixtures();
    preStepHashFixtures();
    std::printf(g_fail ? "FAILED\n" : "all ok\n");
    return g_fail;
}
