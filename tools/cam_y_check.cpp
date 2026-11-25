// cam_y_check <dump.csv> ...: dp/src/dp/cam_y.hpp against the game's own camera Y (+0x414, dumpcamedge dump.csv): per tick, with the previous row as the carried state.
// Counts ticks where |camYStep - c414| < 1e-3 (the same test as t2\shadowy.py).  Static ticks read the static point c1e4 and rate from the row (the dump's, not self-produced).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "dp/cam_y.hpp"
#include "dp/cam_y_static.hpp"   // gdtrig::eased
#include "dp/cam_model.hpp"
using namespace dp::gdcam;
#ifdef CAM_OLD   // the model before the trigger channels (trunk dc9b3947): no crossing coordinates, no channel state
#define CAMSTEP(M, S, X, Z, Y, PY) (M).step(S, X, Z, Y)
#define CAMACTIVE(S) 0
#define CAMVERT(S) 0
#else
#define CAMSTEP(M, S, X, Z, Y, PY) (M).step(S, X, Z, Y, X, PY)
#define CAMACTIVE(S) (S).active
#define CAMVERT(S) ((int)(S).vertical)
#endif

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> o; std::string t; std::stringstream ss(s);
    while (std::getline(ss, t, ',')) o.push_back(t);
    if (!s.empty() && s.back() == ',') o.push_back("");
    return o;
}
int main(int argc, char** argv) {
    long long gok = 0, gn = 0;
    for (int a = 1; a < argc; ++a) {
        std::ifstream f(argv[a], std::ios::binary);
        std::string line;
        if (!std::getline(f, line)) { std::printf("%s: unreadable\n", argv[a]); continue; }
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::map<std::string, int> col; { auto h = split(line); for (size_t i = 0; i < h.size(); ++i) col[h[i]] = (int)i; }
        struct Row { std::vector<double> v; };
        std::vector<std::vector<std::string>> rows;
        while (std::getline(f, line)) { while (!line.empty() && (line.back() == '\r')) line.pop_back(); auto r = split(line); if (r.size() > (size_t)col["attempt"] && r[col["attempt"]] == "1") rows.push_back(std::move(r)); }
        auto num = [&](const std::vector<std::string>& r, const char* k, double d = 0.0) { auto it = col.find(k); if (it == col.end() || (size_t)it->second >= r.size()) return d; char* e; double v = std::strtod(r[it->second].c_str(), &e); return e == r[it->second].c_str() ? d : v; };
        // ORBLAG=1 ([M], 363 cube flip ticks): a gravity flip that came from an ORB (blue 84, green 1022, dash 1751 at the player's x) is read by updateCamera as the flag BEFORE the flip;
        // a portal's or a pad's flip is read as the flag after it. The orbs' x come from the level file named in the autorun.cfg beside the dump (none for the official levels: they keep "after").
        std::vector<double> orbX;
        if (std::getenv("ORBLAG")) {
            std::string path = argv[a], dir = path.substr(0, path.rfind('\\')), lvl;
            { std::ifstream cf(dir + "\\autorun.cfg"); std::string l; while (std::getline(cf, l)) { while (!l.empty() && l.back() == '\r') l.pop_back(); if (l.rfind("levelfile=", 0) == 0) lvl = l.substr(10); } }
            std::ifstream lf(lvl, std::ios::binary); std::string all((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
            size_t pos = all.find(';');
            while (pos != std::string::npos) {
                size_t e = all.find(';', pos + 1); std::string seg = all.substr(pos + 1, e == std::string::npos ? std::string::npos : e - pos - 1); pos = e;
                auto kv = split(seg); int id = 0; double ox = 0; bool hasId = false;
                for (size_t q = 0; q + 1 < kv.size(); q += 2) { if (kv[q] == "1") { id = std::atoi(kv[q + 1].c_str()); hasId = true; } else if (kv[q] == "2") ox = std::atof(kv[q + 1].c_str()); }
                if (hasId && (id == 84 || id == 1022 || id == 1751)) orbX.push_back(ox);
            }
        }
        // PRESSLAG=1 ([R] order + [M] 204 / 206 flips): the flip that a press at tick T-1 causes (pushButton in processCommands, after the previous camera, before this row was written) is in row T but not yet in the
        // camera of row T. Presses are the input=tick,1 lines of the autorun.cfg beside the dump.
        std::set<long long> pressT;
        if (std::getenv("PRESSLAG")) {
            std::string path = argv[a], dir = path.substr(0, path.rfind('\\')); std::ifstream cf(dir + "\\autorun.cfg"); std::string l;
            while (std::getline(cf, l)) { while (!l.empty() && l.back() == '\r') l.pop_back(); if (l.rfind("input=", 0) == 0) { const size_t cm = l.find(','); if (cm != std::string::npos && l.substr(cm + 1) == "1") pressT.insert(std::atoll(l.substr(6, cm - 6).c_str())); } }
        }
        long long ok = 0, n = 0;
        // STATIC1914=1: the Camera Static (1914) point from the level's own keys (cam_model.hpp + cam_y_static.hpp camYStaticPointAt) instead of the row's c1e4: start = the previous tick's centre (the previous ROW's c414 here), bezier tangent
        // = (c414[t-1] - c414[t-2]) * 240 (rows), target = the target group's first object + 90. Counted on the ticks it applies to (a 1914 static camera, its target group static, no time warp), against the same ticks with the row's point.
        CamModel camM; CamState camS; bool haveCam = false, fireSeen = false; float fireStart = 0.0f, fireTan = 0.0f; long long sN = 0, sRow[3] = {}, sSelf[3] = {}; double sMax = 0.0, sRowMax = 0.0;
        long long tN = 0, tRs[7] = {}, tWhy[5] = {}, tTrust = 0, tOk05 = 0, tOk1e3 = 0; std::map<int, std::pair<long long, long long>> tEase; std::map<int, std::pair<long long, long long>> tKind; long long zN = 0, zOk = 0, zUnk = 0; unsigned long long mh = 1469598103934665603ULL; auto mix = [&](unsigned long long v) { mh ^= v + 0x9e3779b97f4a7c15ULL + (mh << 6) + (mh >> 2); mh *= 0xff51afd7ed558ccdULL; }; auto fb = [](float f) { unsigned u; std::memcpy(&u, &f, 4); return (unsigned long long)u; };   // ZOOMCHK: the model's zoom (the Y side's copy) against the row's c1a8, tick by tick, along the game's own x
        if (std::getenv("STATIC1914") || std::getenv("ZOOMCHK") || std::getenv("MODELHASH") || std::getenv("TRUST")) {
            std::string path = argv[a], dir = path.substr(0, path.rfind('\\')), lvl;
            { std::ifstream cf(dir + "\\autorun.cfg"); std::string l; while (std::getline(cf, l)) { while (!l.empty() && l.back() == '\r') l.pop_back(); if (l.rfind("levelfile=", 0) == 0) lvl = l.substr(10); } }
            if (lvl.empty() || !std::ifstream(lvl).good()) { const char* ld = std::getenv("CAMY_LVLDIR"); if (ld) lvl = std::string(ld) + "\\lv" + dir.substr(dir.rfind('\\') + 1) + ".lvl"; }
            haveCam = camM.load(lvl);
            if (!haveCam) std::printf("STATIC1914 %s: no level file (%s)\n", argv[a], lvl.c_str());
            if (haveCam && !rows.empty()) { CamYTick y0; float z0; CAMSTEP(camM, camS, num(rows[0], "x"), &z0, &y0, num(rows[0], "y")); }
        }
        // FREERUN=1: the camera Y carries its own output from tick to tick (every other input still read from the row), instead of the dump's c414 of the previous row; counts how far it
        // drifts (|err| < 1e-3 / 0.01 / 0.5) and its error on the ticks where a portal starts the static camera (c23c 0 -> on with a mode change within 1 tick).
        const bool freerun = std::getenv("FREERUN") != nullptr;
        float own = rows.empty() ? 0.0f : (float)num(rows[0], "c414"); float ownP500 = 0.0f; float offStart = 0.0f, offD = 0.0f, offRate = 2.0f, offPrevVal = 0.0f, ownOffPrev = 0.0f; long long offK = 0; double offMaxErr = 0.0; int offStartTick = 0; int offKind = 0; bool offActive = false, offInTween = false, offBez = false; float offTan = 0.0f, offBzK = 0.0f; long long offFails = 0; float ownCand = rows.empty() ? 0.0f : (float)num(rows[0], "c1c4");
        long long rotN = 0, rotBad = 0; long long fr[3] = {}, pe[3] = {}, pn = 0, se[3] = {}, sn = 0; double smax = 0.0; double pmax = 0.0; bool firstDone = false;
        for (size_t i = 1; i < rows.size(); ++i) {
            const auto& p = rows[i - 1]; const auto& c = rows[i];
            CamYState s; s.camY = freerun ? own : (float)num(p, "c414");
            CamYIn in;
            in.y = (float)num(c, "y"); in.pointY = (float)num(c, "pa38");
            in.upsideDown = (int)num(std::getenv("UPLAG") ? p : c, "p9c7") != 0;
            if (!pressT.empty() && (int)num(p, "p9c7") != (int)num(c, "p9c7") && pressT.count((long long)num(c, "tick") - 1)) in.upsideDown = (int)num(p, "p9c7") != 0;
            if (!orbX.empty() && (int)num(p, "p9c7") != (int)num(c, "p9c7")) { const double px = num(c, "x"); for (double ox : orbX) if (std::fabs(ox - px) < 60.0) { in.upsideDown = (int)num(p, "p9c7") != 0; break; } }
            in.rotated = (int)num(c, "p9cb") != 0;
            in.flier = false; for (const char* k : {"p9c1", "p9c2", "p9c4", "p9cc", "p9c3", "p9c6"}) if ((int)num(c, k) != 0) in.flier = true;
            in.flierNo9c3_9c6 = true; for (const char* k : {"p9c1", "p9c2", "p9c4", "p9cc"}) if ((int)num(c, k) != 0) in.flierNo9c3_9c6 = false;
            in.zoom = (float)num(c, "c1a8"); in.zoomPrev = (float)num(p, "c1a8");
            in.offY = (float)num(c, "c1b4"); in.offYPrev = (float)num(p, "c1b4");
            in.offApplied = (float)num(c, "c1f4"); in.offAppliedPrev = (float)num(p, "c1fc");
            in.tw = (float)num(c, "tw", 1.0); in.k2cc = (float)num(c, "k2cc"); in.rate2d0 = (float)num(c, "r2d0");
            in.snap = (int)num(c, "b331d") != 0 || (int)num(c, "b761") != 0 || num(c, "c244") != 0;
            in.staticOn = num(c, "c23c") != 0;
            // TELESNAP=m ([R] checkCameraLimitAfterTeleport 0x3427b0, called by teleportPlayer): after a teleport, with the static camera off and the player not rotated, when the player is beyond the camera window by the margin m
            // the game sets +0x761 and runs updateCamera once more (snap). A teleport is read off the dump as a jump of the player's y by more than 20 in one tick.
            if (const char* tm = std::getenv("TELESNAP")) {
                const double m = std::atof(tm), dy = std::fabs(num(c, "y") - num(p, "y"));
                if (dy > 20.0 && !in.staticOn && !in.rotated) { const double cy = s.camY, py = num(c, "y"); if (320.0 + cy + m < py || cy - m > py) in.snap = true; }
            }
            if (std::getenv("EXITSNAP") && num(p, "c23c") != 0 && !in.staticOn) {   // the tick a static camera ends: +0x244 (the exit timer) is set by the exit trigger and cleared by updateCamera in the same tick, so the dump never shows it
                bool portal = false; for (int d = -1; d <= 1; ++d) { const long long j = (long long)i + d; if (j >= 1 && j < (long long)rows.size() && rows[j][col["mode"]] != rows[j - 1][col["mode"]]) portal = true; }
                const bool wantPortalSnap = std::getenv("EXITSNAP")[0] == '2';
                if (!portal || wantPortalSnap) in.snap = true;
            }
            in.staticY = (float)num(c, "c1e4"); in.staticStart = (float)num(c, "c1dc"); in.staticTarget = (float)num(c, "c1ec"); in.mode2ec = (float)num(c, "c2ec");
            { float f310 = (float)num(c, "c310"); unsigned char b; std::memcpy(&b, &f310, 1); in.b310 = b != 0; }
            { float ry = (float)num(c, "rateY"); in.staticRate = ry > 1.0f ? ry : 1.0f; }
            in.rotTween = (float)num(c, "c284");
            in.edgeT = (int)num(c, "edgeT"); in.edgeB = (int)num(c, "edgeB");
            in.xgate = (int)num(c, "edgeR") > 0 || (int)num(c, "b3320") != 0;
            in.edgeTy = (float)num(c, "eTy", -99999.0); in.edgeBy = (float)num(c, "eBy", -99999.0);
            in.defY = (float)num(c, "defY"); in.top37a8 = (float)num(c, "c37a8");
            in.post500Prev = (float)num(p, "c500"); in.prevCand = (float)num(p, "c1c4");
            if (std::getenv("SELFCAND")) in.prevCand = ownCand;   // [R] 1087: +0x1c4 is the previous tick's own cand
            // SELF500 ([R] teleportPlayer 0x316599 sets +0x500 = 0.5f; updateCamera 1088-1097: rate 30 while it is > 0, it falls by dt / 60 a tick, a snap clears it; [M] 0.4958 = 0.5 - 1/240 on the tick, 120 ticks): the timer self-carried
            // EXITOFF ([R] updateCamera 1166-1348, [M] a custom level tick 3542): the tick a 1914 exit ends a static camera, the camera keeps where it was (offApplied = the jump, -86 there) and the offset tweens back to 0 over
            // the exit trigger's duration (c24c) with its easing (c25c, rate c254: 15 = sine out, 2): offApplied(k) = start * (1 - ease(k / (dur * 240))). The three trigger keys are read from the row (the dp has the trigger).
            if (std::getenv("EXITOFF")) {
                bool portalx = false; for (int d = -1; d <= 1; ++d) { const long long j = (long long)i + d; if (j >= 1 && j < (long long)rows.size() && rows[j][col["mode"]] != rows[j - 1][col["mode"]]) portalx = true; }
                if (num(p, "c23c") != 0 && !in.staticOn && !portalx) {
                    CamYIn z = in; z.offApplied = 0.0f; z.snap = true;
                    const float out0 = camYStep(s, z);
                    offStart = s.camY - out0; offK = 0; offD = (float)num(c, "c24c") * 240.0f; offKind = (int)num(c, "c25c"); offRate = (float)num(c, "c254"); offActive = true; offPrevVal = ownOffPrev; offBez = num(c, "c264") != 0; offTan = (float)num(c, "c204"); offBzK = (float)num(c, "c268");
                    in.snap = true;
                }
                offInTween = offActive;
                if (offActive) {
                    const float tt = offD > 0.0f ? std::min(1.0f, (float)offK / offD) : 1.0f;
                    const float e = offKind > 0 ? dp::gdtrig::eased(tt, offKind, offRate) : tt;   // GJValueTween::step: the ease only when kind > 0
                    float val;
                    if (offBez) {   // [R] updateCamera 1286-1344, the bezier exit (+0x264): P0 = start, P3 = 0, P1 = P0 + dur * tangent / 3, P2 = (P1 - P3) * +0x268 + P3; t = the eased time
                        const float P0 = offStart, P1 = offStart + (offD / 240.0f) * offTan / 3.0f, P2 = P1 * offBzK, u = 1.0f - e;
                        val = P1 * 3.0f * e * u * u + u * P0 * u * u + P2 * 3.0f * e * e * u;
                    } else val = offStart * (1.0f - e);
                    if (std::getenv("EXITOFFDBG")) { const double dd = std::fabs((double)val - num(c, "c1f4")); if (dd > offMaxErr) offMaxErr = dd; if (offK == 0) { offStartTick = (int)num(c, "tick"); offMaxErr = dd; } if (tt >= 1.0f || (i + 1 < rows.size() && num(rows[i + 1], "c23c") != 0)) std::printf("EXITOFF %s tick %d kind %d rate %g dur %g start %.3f maxErr %.4f k %lld\n", argv[a], offStartTick, offKind, (double)offRate, (double)offD / 240.0, (double)offStart, offMaxErr, offK); }
                    in.offApplied = val; in.offAppliedPrev = ownOffPrev;
                    ++offK; if (tt >= 1.0f) offActive = false;
                    ownOffPrev = val;
                } else { ownOffPrev = in.offApplied; }   // not in a self-run exit tween: the row's offApplied (other sources: the frame-flip snap, portal exits) stays
            }
            CamYIn inSelf = in; bool selfOn = false; bool unkCamUse = false; long long dK = 0; int dBez = 0, dFol = 0; float dTgt = 0.0f, dTan = 0.0f, dDur = 0.0f; int dEase = 0;
            if (haveCam) {
                CamYTick yt; float zx = 1.0f; const float pxv = CAMSTEP(camM, camS, num(c, "x"), &zx, &yt, num(c, "y"));
                mix(fb(pxv)); mix(fb(zx)); mix(fb(yt.zoom)); mix(fb(yt.offY)); mix(fb(yt.tw)); mix((unsigned long long)yt.staticY | ((unsigned long long)yt.yev << 1) | ((unsigned long long)camS.uncertain << 2)); mix((unsigned long long)(yt.edgeTGrp * 131 + yt.edgeBGrp)); mix(fb(yt.st.tgtY)); mix((unsigned long long)yt.k);
                if (std::getenv("ZOOMCHK")) { ++zN; if (yt.unkCam) ++zUnk; if (std::fabs((double)yt.zoom - num(c, "c1a8")) < 1e-4) ++zOk; else if (std::getenv("ZOOMDBG") && zN - zOk < 400 && ((zN - zOk) % 50 == 1)) std::printf("ZDBG %s tick %d x %.1f model %.4f game %.4f active %d vertical %d\n", argv[a], (int)num(c, "tick"), num(c, "x"), (double)yt.zoom, num(c, "c1a8"), CAMACTIVE(camS), CAMVERT(camS)); }
                const bool gameStatic = num(c, "c23c") != 0; float f310 = (float)num(c, "c310"); unsigned char b310; std::memcpy(&b310, &f310, 1);
                unkCamUse = std::getenv("TRUSTRAW") ? (camM.unknownStaticMinX <= camS.runMax || camS.vertUnk) : yt.unkCam;   // TRUSTRAW: leave the Toggle / order predicate out (what it costs, what it saves)
                if (gameStatic && yt.staticY && !b310 && yt.st.tgtKnown && yt.tw == 1.0f && !unkCamUse && i >= 2) {
                    const float centrePrev = (float)num(p, "c414") + 160.0f / (float)num(p, "c1a8") - (float)num(p, "c1b4");
                    // a tween that fired on an earlier tick: its own start (the previous centre of the fire tick) and tangent, kept
                    if (yt.yev) { fireSeen = true; fireStart = centrePrev; fireTan = (float)((num(p, "c414") - num(rows[i - 2], "c414")) * 240.0); }
                    CamYTween tw; tw.follow = yt.st.follow; tw.bezier = yt.st.bez; tw.dur = yt.st.dur; tw.speed = yt.st.speed; tw.tangent = fireTan; tw.bz = yt.st.bz; tw.target = yt.st.tgtY; tw.ease = yt.st.ease; tw.easeRate = yt.st.easeRate;
                    float r = 1.0f; inSelf.staticY = camYStaticPointAt(tw, fireStart, yt.k, yt.st.tgtY, &r); inSelf.staticStart = fireStart; inSelf.staticTarget = yt.st.tgtY; inSelf.staticRate = r > 1.0f ? r : 1.0f; selfOn = fireSeen && yt.k >= 1; dK = yt.k; dBez = yt.st.bez; dFol = yt.st.follow; dTgt = yt.st.tgtY; dTan = fireTan; dDur = yt.st.dur; dEase = yt.st.ease;
                }
            }
            if (haveCam && std::getenv("TRUST")) {   // S3-0 (b): a static (non-portal) camera tick of the game, and why the model's point is or is not trusted (the first failing predicate)
                float f310b = (float)num(c, "c310"); unsigned char b310b; std::memcpy(&b310b, &f310b, 1);
                if (num(c, "c23c") != 0 && !b310b) {
                    ++tN;
                    if (selfOn) { ++tTrust; }
                    else if (!camS.staticY) ++tRs[0];            // the model has no static camera here (a trigger it did not fire, or fired late)
                    else if (camS.yUnk && !std::getenv("TRUSTRAW")) ++tRs[1];                // a Toggle names the trigger's group, or it has an order
                    else if (camS.vertUnk) ++tRs[2];             // a vertical channel with no y
                    else if (!camM.groupYKnownNow(camS)) { ++tRs[3]; ++tWhy[camM.groupWhy(camS.yev.tgt)]; } // the target group is not a static object
                    else if (camS.tw != 1.0f) ++tRs[4];
                    else if (unkCamUse) ++tRs[5];                // timing unknown (a spawned or touched 1914, > 32 channels)
                    else ++tRs[6];                               // no start seen in this run
                }
            }
            float p500in = ownP500;
            if (std::getenv("SELF500")) {
                const bool tele = std::fabs(num(c, "y") - num(p, "y")) > 20.0;
                if (tele) p500in = 0.5f;
                if (in.snap) p500in = 0.0f;
                in.post500Prev = p500in;
            }
            float candNow = 0.0f; const float out = camYStep(s, in, &candNow); ownCand = candNow;
            if (selfOn && std::getenv("TRUST")) { const double eS0 = std::fabs((double)camYStep(s, inSelf) - num(c, "c414")); if (eS0 < 0.05) ++tOk05; if (eS0 < 1e-3) ++tOk1e3; auto& e = tEase[dEase * 4 + (dFol ? 2 : 0) + (dBez ? 1 : 0)]; ++e.first; if (eS0 < 0.05) ++e.second; auto& k = tKind[(dFol ? 2 : 0) + (dBez ? 1 : 0)]; ++k.first; if (eS0 < 0.05) ++k.second; }
            if (selfOn) { const double eS = std::fabs((double)camYStep(s, inSelf) - num(c, "c414")), eR = std::fabs((double)out - num(c, "c414")); ++sN; if (eS < 1e-3) ++sSelf[0]; if (eS < 0.01) ++sSelf[1]; if (eS < 0.5) ++sSelf[2]; if (eR < 1e-3) ++sRow[0]; if (eR < 0.01) ++sRow[1]; if (eR < 0.5) ++sRow[2]; sMax = std::max(sMax, eS); sRowMax = std::max(sRowMax, eR); if (std::getenv("S1914DBG") && eS >= 1e-3) std::printf("S1914BAD %s tick %d errSelf %.4f errRow %.4f self %.4f row %.4f k %lld bez %d follow %d tgtY %.2f tan %.3f dur %g ease %d\n", argv[a], (int)num(c, "tick"), eS, eR, (double)inSelf.staticY, (double)in.staticY, dK, dBez, dFol, (double)dTgt, (double)dTan, (double)dDur, dEase); }
            { const float dt = 0.25f * std::min(in.tw, 1.0f); ownP500 = in.snap ? 0.0f : (p500in > 0.0f ? p500in - dt / 60.0f : p500in); if (ownP500 < 0.0f) ownP500 = 0.0f; }
            if (std::getenv("EXITSTAT") && num(p, "c23c") != 0 && !in.staticOn) {
                bool portal = false; for (int d = -1; d <= 1; ++d) { const long long j = (long long)i + d; if (j >= 1 && j < (long long)rows.size() && rows[j][col["mode"]] != rows[j - 1][col["mode"]]) portal = true; }
                CamYIn a1 = in, a2 = in; a1.snap = false; a2.snap = true; const double want = num(c, "c414");
                std::printf("EXIT %s tick %d portal:%d nosnap:%d snap:%d offApplied %.3f offPrev %.3f zoomchg:%d offchg:%d err0 %.3f err1 %.3f\n", argv[a], (int)num(c, "tick"), (int)portal, (int)(std::fabs((double)camYStep(s, a1) - want) < 1e-3), (int)(std::fabs((double)camYStep(s, a2) - want) < 1e-3), in.offApplied, in.offAppliedPrev, (int)(in.zoom != in.zoomPrev), (int)(in.offY != in.offYPrev), std::fabs((double)camYStep(s, a1) - want), std::fabs((double)camYStep(s, a2) - want));
            }
            if (std::getenv("FLIPSTAT") && (int)num(p, "p9c7") != (int)num(c, "p9c7")) {
                // a gravity flip tick: which of the flag now / the flag before the game's camera used (teacher-forced)
                CamYState st; st.camY = (float)num(p, "c414"); CamYIn in2 = in; in2.upsideDown = (int)num(p, "p9c7") != 0;
                const bool nowOk = std::fabs((double)out - num(c, "c414")) < 1e-3, lagOk = std::fabs((double)camYStep(st, in2) - num(c, "c414")) < 1e-3;
                std::printf("FLIP %s tick %d to %d now:%d lag:%d mode %s yvel %.3f onG %g pyvel %.3f ponG %g dy %.3f x %.3f\n", argv[a], (int)num(c, "tick"), (int)num(c, "p9c7"), (int)nowOk, (int)lagOk, rows[i][col["mode"]].c_str(), num(c, "yvel"), num(c, "onGround"), num(p, "yvel"), num(p, "onGround"), num(c, "y") - num(p, "y"), num(c, "x"));
            }
            own = out;
            if (freerun && std::getenv("FIRST") && !firstDone && std::fabs((double)out - num(c, "c414")) >= 1e-3) {
                // the first tick the self-carried camera leaves the game's by 1e-3: what the tick is (one line per level; the same tick forced from the dump's previous c414 shows whether the rule or the carried state is off)
                firstDone = true;
                CamYState st; st.camY = (float)num(p, "c414"); const double errT = std::fabs((double)camYStep(st, in) - num(c, "c414"));
                const bool prevStatic = num(p, "c23c") != 0, modeChg = rows[i][col["mode"]] != rows[i - 1][col["mode"]];
                std::string f;
                auto add = [&](bool b, const char* s) { if (b) { f += s; f += ' '; } };
                add(in.staticOn, "static"); add(in.staticOn != prevStatic, "static-edge"); add(modeChg, "modechg"); add(in.rotated, "rotated"); add(in.upsideDown, "upsidedown");
                add(in.edgeT > 0, "edgeT"); add(in.edgeB > 0, "edgeB"); add(in.zoom != in.zoomPrev, "zoomchg"); add(in.offY != in.offYPrev, "offchg"); add(in.offApplied != 0.0f || in.offAppliedPrev != 0.0f, "offApplied");
                add(in.snap, "snap"); add(in.post500Prev > 0.0f, "post500"); add(in.flier, "flier"); add(in.tw != 1.0f, "tw"); add(in.xgate, "xgate"); add(in.staticRate > 1.0f, "staticRate");
                add(in.pointY == 105.0f && !in.staticOn, "pointY105"); add((int)num(p, "p9c7") != (int)num(c, "p9c7"), "FLIPTICK"); add(!orbX.empty(), "hasOrbList");
                std::printf("FIRST %s tick %d row %zu errFree %.4f errForced %.6f : %s| model %.4f dump %.4f prev %.4f y %.3f c500p %.3f c244 %g b761 %g b331d %g c1c4p %.3f c1c4 %.3f zoom %.4f x %.3f\n", argv[a], (int)num(c, "tick"), i, std::fabs((double)out - num(c, "c414")), errT, f.c_str(),
                            (double)out, num(c, "c414"), num(p, "c414"), num(c, "y"), num(p, "c500"), num(c, "c244"), num(c, "b761"), num(c, "b331d"), num(p, "c1c4"), num(c, "c1c4"), num(c, "c1a8"), num(c, "x"));
            }
            ++n; const double err = std::fabs((double)out - num(c, "c414")); if (err < 1e-3) ++ok;
            if (std::getenv("ROTSTAT") && in.rotated) { ++rotN; if (err >= 1e-3) ++rotBad; }
            if (std::getenv("EXITOFFDBG") && offInTween) { if (offK == 1) offFails = 0; if (err >= 1e-3) ++offFails; if (!offActive) std::printf("EXITEND %s tick %d fails %lld of k %lld kind %d dur %g\n", argv[a], offStartTick, offFails, offK, offKind, (double)offD / 240.0); }
            if (err < 1e-3) ++fr[0]; if (err < 0.01) ++fr[1]; if (err < 0.5) ++fr[2];
            if (freerun && num(p, "c23c") == 0 && num(c, "c23c") != 0) {
                // the mode column is a name, not a number: compare the strings
                bool mc2 = false; for (int d = -1; d <= 1; ++d) { const long long j = (long long)i + d; if (j >= 1 && j < (long long)rows.size() && rows[j][col["mode"]] != rows[j - 1][col["mode"]]) mc2 = true; }
                if (mc2) { ++pn; if (err < 1e-3) ++pe[0]; if (err < 0.01) ++pe[1]; if (err < 0.5) ++pe[2]; if (err > pmax) pmax = err;
                    // the START POINT of the tween the portal begins is the camera of the PREVIOUS tick (c1dc): the carried camera against the game's c414 of the previous row (not the tick's own output, which follows the static point the row gives)
                    const double e0 = std::fabs((double)s.camY - num(p, "c414")); if (e0 >= 1e-3 && std::getenv("STARTMISS")) std::printf("STARTMISS %s tick %d err %.4f\n", argv[a], (int)num(c, "tick"), e0); ++sn; if (e0 < 1e-3) ++se[0]; if (e0 < 0.01) ++se[1]; if (e0 < 0.5) ++se[2]; if (e0 > smax) smax = e0; }
            }
        }
        if (haveCam && std::getenv("TRUST")) {
            std::printf("TRUST %s: static %lld | model-not-static %lld toggle/order %lld vertical %lld target-unknown %lld tw %lld timing %lld no-start %lld | trusted %lld  <0.05 %lld  <1e-3 %lld\n", argv[a], tN, tRs[0], tRs[1], tRs[2], tRs[3], tRs[4], tRs[5], tRs[6], tTrust, tOk05, tOk1e3);
            std::printf("TRUSTWHY %s: target-unknown: static %lld no-object %lld moved-by-trigger %lld parent %lld no-target %lld\n", argv[a], tWhy[0], tWhy[1], tWhy[2], tWhy[3], tWhy[4]);
            for (const auto& e : tEase) std::printf("TRUSTEASE %s: ease %d kind %d ticks %lld within0.05 %lld\n", argv[a], e.first / 4, e.first % 4, e.second.first, e.second.second);
            for (const auto& k : tKind) std::printf("TRUSTKIND %s: kind %s%s ticks %lld within0.05 %lld\n", argv[a], (k.first & 2) ? "follow" : "plain", (k.first & 1) ? "+bezier" : "", k.second.first, k.second.second);
        }
        if (haveCam && std::getenv("MODELHASH")) std::printf("MODELHASH %s %016llx\n", argv[a], mh);
        if (haveCam && std::getenv("ZOOMCHK")) std::printf("ZOOMCHK %s: ticks %lld zoom within 1e-4 %lld unkCam %lld\n", argv[a], zN, zOk, zUnk);
        if (haveCam && std::getenv("STATIC1914")) std::printf("STATIC1914 %s: ticks %lld  row point <1e-3 %lld <0.01 %lld <0.5 %lld max %.3f  |  self point <1e-3 %lld <0.01 %lld <0.5 %lld max %.3f\n", argv[a], sN, sRow[0], sRow[1], sRow[2], sRowMax, sSelf[0], sSelf[1], sSelf[2], sMax);
        if (std::getenv("ROTSTAT")) std::printf("ROTSTAT %s rotated ticks %lld wrong %lld\n", argv[a], rotN, rotBad);
        std::printf("%s: %lld / %lld = %.4f", argv[a], ok, n, n ? (double)ok / (double)n : 0.0);
        if (freerun) std::printf(" START-POINT sn %lld <1e-3 %lld <0.01 %lld <0.5 %lld max %.3f ", sn, se[0], se[1], se[2], smax);
        if (freerun) std::printf("  freerun: <1e-3 %lld  <0.01 %lld  <0.5 %lld of %lld; portal starts %lld: <1e-3 %lld <0.01 %lld <0.5 %lld max err %.3f", fr[0], fr[1], fr[2], n, pn, pe[0], pe[1], pe[2], pmax);
        std::printf("\n");
        gok += ok; gn += n;
    }
    std::printf("TOTAL %lld / %lld = %.4f\n", gok, gn, gn ? (double)gok / (double)gn : 0.0);
    return 0;
}
