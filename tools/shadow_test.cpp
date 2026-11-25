// shadow_test: dp/src/dp/shadow.hpp (the contact shadow, which carries its own image from tick to tick) against the game's entry / exit bytes of the collision pass.
//   shadow_test <ctraw.csv> <objrects.txt> [--from T] [--to T] [--verbose] [--tail N]
// Per row the shadow is given only what the update phase produced (x, y, vy, mode, size, gravity, speed) -- never the game's contact bytes -- and what it
// returns is compared with the game's exit: y, vy, G, G2, BOOST, on-slope, gravity and the uids of the floor, the ceiling and the ramp.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/shadow_test.cpp
#include "dp/shadow.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace dp::gdcol;

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

static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> b(s.size() / 2);
    auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)(v(s[2 * i]) * 16 + v(s[2 * i + 1]));
    return b;
}

template <class T> static T at(const std::vector<uint8_t>& b, size_t o) { T v; std::memcpy(&v, b.data() + o, sizeof(T)); return v; }

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: shadow_test ctraw.csv objrects.txt [--from T] [--to T] [--verbose]\n"); return 2; }
    long long from = 0, to = 1LL << 60;
    bool verbose = false, resync = false, autoJump = false;
    int who = 1;   // which player's passes: 1, or 2 on a capture taken with ctrawp2=1
    std::vector<size_t> forceOffs;   // words of the game's entry image the shadow is given anyway (to find which carried word matters)
    int show = 10;
    std::string dumpPath;       // --dump <dump.csv of the same run>: the game's own death, to compare with the shadow's (the pass's return value is not the death: it is 0 on the pass that kills)
    bool fixRadius = false;     // --fixradius: the level keeps fixRadiusCollision (levelsettings.txt)
    std::map<long long, int> gdDead;   // tick -> the dump's dead column (attempt 1 only)
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from") from = std::atoll(argv[++i]);
        else if (a == "--to") to = std::atoll(argv[++i]);
        else if (a == "--verbose") verbose = true;
        else if (a == "--show") show = std::atoi(argv[++i]);
        else if (a == "--resync") resync = true;
        else if (a == "--autojump") autoJump = true;
        else if (a == "--who") who = std::atoi(argv[++i]);
        else if (a == "--dump") dumpPath = argv[++i];
        else if (a == "--fixradius") fixRadius = true;
        else if (a == "--force") { for (const auto& tok : split(argv[++i], ',')) forceOffs.push_back(std::stoul(tok, nullptr, 16)); }
    }
    std::vector<GObj> objs;
    {
        std::ifstream f(argv[2]);
        std::string line;
        std::getline(f, line);
        const auto hdr = split(line, ',');
        std::map<std::string, size_t> col;
        for (size_t i = 0; i < hdr.size(); ++i) col[hdr[i]] = i;
        auto num = [&](const std::vector<std::string>& v, const char* n, double d = 0) { auto it = col.find(n); return it == col.end() || it->second >= v.size() || v[it->second].empty() ? d : std::stod(v[it->second]); };
        while (std::getline(f, line)) {
            const auto v = split(line, ',');
            if (v.size() < 8) continue;
            GObj o;
            o.objectId = (int)num(v, "id");
            o.type = (int)num(v, "type");
            const double cx = num(v, "cx"), cy = num(v, "cy"), w = num(v, "w"), h = num(v, "h");
            o.uid = (int)num(v, "uid", -1);
            o.cx = (float)cx; o.cy = (float)cy; o.prevCx = o.cx; o.prevCy = o.cy;
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
            o.touchTrig = num(v, "touch") != 0;
            o.rot = (float)num(v, "rot"); o.w0 = (float)num(v, "w0"); o.h0 = (float)num(v, "h0");
            o.sy0 = num(v, "sy0"); o.sy1 = num(v, "sy1");
            objs.push_back(o);
        }
        if (const char* obbPath = std::getenv("ST_OBB")) {   // obb.txt (uid,id,type,cx,cy,rot,x0,y0..x3,y3,oob): GD's own oriented-box corners, instead of a size solved from sibling objects
            std::ifstream ob(obbPath);
            std::string ol;
            std::getline(ob, ol);
            std::map<int, std::vector<float>> corners;
            while (std::getline(ob, ol)) {
                const auto v = split(ol, ',');
                if (v.size() < 14) continue;
                std::vector<float> c;
                for (size_t i = 6; i < 14; ++i) c.push_back((float)std::atof(v[i].c_str()));
                corners[std::atoi(v[0].c_str())] = c;
            }
            for (auto& o : objs) {
                auto it = corners.find(o.uid);
                if (it == corners.end()) continue;
                o.hasGc = true;
                for (int q = 0; q < 8; ++q) o.gc[q] = it->second[q];
                o.gcx = o.cx; o.gcy = o.cy;
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
            auto it = ratio.find(o.objectId);
            if (it != ratio.end()) { o.bw = it->second.first * o.w0 * o.scaleX; o.bh = it->second.second * o.h0 * o.scaleY; }
            if (const char* du = std::getenv("ST_DEBUG_UID")) if (o.uid == std::atoi(du)) std::printf("object %d: type %d rot %.2f rect %.4f x %.4f w0 %.3f h0 %.3f bw %.4f bh %.4f (ratio %s)\n", o.uid, o.type, o.rot, o.rw, o.rh, o.w0, o.h0, o.bw, o.bh, it != ratio.end() ? "from a siblings" : "none");
        }
    }
    if (!dumpPath.empty()) {
        std::ifstream d(dumpPath);
        std::string dl;
        std::getline(d, dl);
        const auto dh = split(dl, ',');
        size_t cAtt = 0, cTick = 0, cDead = 0;
        for (size_t i = 0; i < dh.size(); ++i) { if (dh[i] == "attempt") cAtt = i; else if (dh[i] == "tick") cTick = i; else if (dh[i] == "dead") cDead = i; }
        std::string firstAtt;
        while (std::getline(d, dl)) {
            const auto v = split(dl, ',');
            if (v.size() <= cDead) continue;
            if (firstAtt.empty()) firstAtt = v[cAtt];
            if (v[cAtt] != firstAtt) break;
            gdDead[std::atoll(v[cTick].c_str())] = std::atoi(v[cDead].c_str());
        }
    }
    long long deathsGd = 0, deathsShadow = 0, deathsBothSameRow = 0;
    std::ifstream f(argv[1]);
    std::string line;
    std::getline(f, line);
    ContactShadow sh;
    long long rows = 0, compared = 0, seededRows = 0;
    std::map<std::string, long long> bad;
    std::map<std::string, std::vector<long long>> badAt;
    long long rowsBad = 0;
    long long lastTick = -100;
    while (std::getline(f, line)) {
        const auto v = split(line, ',');
        if (v.size() < 8) continue;
        const long long tick = std::atoll(v[1].c_str());
        if (tick < from || tick > to) continue;
        if (tick <= 13) continue;   // the level is still starting: the game returns 1 from the pass
        if (std::atoi(v[2].c_str()) != who) continue;
        const auto pre = unhex(split(v[6], ';')[0]);
        const auto post = unhex(split(v[7], ';')[0]);
        ShadowIn in;
        in.t = tick;
        in.x = at<float>(pre, po::PosX);
        in.y = at<float>(pre, po::PosY);
        in.vy = at<double>(pre, po::Vy);
        in.mini = at<float>(pre, 0x9f8) != 1.0f;
        in.flip = pre[po::Upside] != 0;
        in.speed = at<float>(pre, 0x9fc);
        in.grounded = pre[po::G2] != 0;
        in.mode = 0;
        if (pre[0x9c1]) in.mode = 1; else if (pre[0x9c3]) in.mode = 2; else if (pre[0x9c2]) in.mode = 3; else if (pre[0x9c4]) in.mode = 4;
        else if (pre[0x9c5]) in.mode = 5; else if (pre[0x9c6]) in.mode = 6; else if (pre[0x9cc]) in.mode = 7;
        in.held = pre[0x98d] != 0;
        in.noAutoJump = !autoJump;
        in.rot = at<float>(pre, 0x38);   // the body's sprite rotation: the player's oriented box turns by it (a rotated hazard is tested against that box)
        in.useRot = true;
        in.fixRadius = fixRadius;
        in.jumped = !autoJump && pre[po::Boost] != 0 && sh.img.u8(po::Boost) == 0;   // (what the press side would tell the shadow)   // (what the press side would tell the shadow)
        {
            const auto parts = split(v[6], ';');
            const auto pq = split(parts[7], ':');
            in.maxPortalY = std::stof(pq[0]);
            in.minPortalY = std::stof(pq[1]);
            in.freeMode = unhex(parts[3])[0x311 - 0x300] != 0;
            in.dual = unhex(parts[3])[0x422 - 0x300] != 0;
            const auto A = unhex(parts[2]), C = unhex(parts[4]), E = unhex(parts[6]);
            in.speedA = at<float>(A, 8); in.speedB = at<float>(A, 12); in.w764 = at<float>(C, 4); in.ceilWord = at<float>(E, 8);
        }
        std::vector<GObj> cands;
        const float px = in.x, py = in.y;
        for (const GObj& o : objs)
            if (o.rx <= px + 95 && px - 95 <= o.rx + o.rw && o.ry <= py + 95 && py - 95 <= o.ry + o.rh) cands.push_back(o);
        if (!forceOffs.empty()) sh.tweak = [&](Mem& m) { for (size_t o : forceOffs) std::memcpy(m.b.data() + o, pre.data() + o, 4); };
        const ShadowOut out = sh.step(in, cands);
        ++rows;
        if (out.seeded) { ++seededRows; lastTick = tick; }
        if (tick < lastTick + 3) continue;   // the first ticks after a seed carry the template's words
        ++compared;
        std::map<size_t, int> puid;
        {
            std::istringstream is(split(v[7], ';')[1]);
            std::string tok;
            while (is >> tok) { const auto c = tok.find(':'); puid[std::stoul(tok.substr(0, c), nullptr, 16)] = std::stoi(tok.substr(c + 1)); }
        }
        auto gpost = [&](size_t o) { auto it = puid.find(o); return (long long)(it == puid.end() ? -1 : it->second); };
        bool any = false;
        auto cmp = [&](const char* name, bool same) {
            if (same) return;
            ++bad[name];
            if (badAt[name].size() < 8) badAt[name].push_back(tick);
            any = true;
        };
        cmp("y", out.y == at<float>(post, po::PosY));
        cmp("vy", out.vy == at<double>(post, po::Vy));
        cmp("G", out.g == post[po::G]);
        cmp("G2", out.g2 == post[po::G2]);
        cmp("boost", out.boost == post[po::Boost]);
        cmp("onSlope", out.onSlope == post[po::OnSlope]);
        cmp("upside", out.upside == post[po::Upside]);
        cmp("floorUid", out.fl == at<int32_t>(post, po::CollisionBottomUid));
        cmp("ceilUid", out.ce == at<int32_t>(post, po::CollisionTopUid));
        cmp("slopeObj", out.slope == (post[po::OnSlope] ? (int)gpost(po::SlopeObj) : -1));
        cmp("snap", out.snap == (int)gpost(po::SnappedTo));
        cmp("ret", (out.ret != 0) == (std::atoi(v[5].c_str()) != 0));
        if (!gdDead.empty() && gdDead.count(tick + 1) && gdDead.count(tick)) {
            // the dump row of tick+1 is the state after this pass (see the ccl probe: pass t <-> dump t+1): dead there and not before = the pass called destroyPlayer
            const bool gd = gdDead[tick + 1] != 0 && gdDead[tick] == 0;
            deathsGd += gd;
            deathsShadow += out.died;
            deathsBothSameRow += (gd && out.died);
            cmp("died", out.died == gd);
            if (out.died != gd) std::printf("DEATH MISMATCH row %lld: game %s, shadow %s (line %d, uid %d)\n", tick, gd ? "died" : "alive", out.died ? "died" : "alive", out.why, out.whyUid);
        }
        if (any) ++rowsBad;
        if (any && resync) {   // one wrong row must not colour the rest: carry the game's own exit image on
            Mem g(post.size());
            g.b = post;
            for (size_t off : {(size_t)0x5e8, (size_t)0x5f0, (size_t)0x600, (size_t)0x608, (size_t)0x610, (size_t)0x618, (size_t)0x678, (size_t)0x680, (size_t)0x890}) {
                auto it = puid.find(off);
                g.put<int64_t>(off, it == puid.end() || it->second < 0 ? 0 : (int64_t)it->second + 1);
            }
            sh.img = g;
        }
        if (verbose && any) {
            std::printf("  entry words that differ from the game's:");
            for (size_t o = 0; o + 4 <= pre.size(); o += 4) {
                if (o >= 0x358 && o < 0x370) continue;
                if (std::memcmp(sh.entry.b.data() + o, pre.data() + o, 4) != 0) { uint32_t a1, b1; std::memcpy(&a1, sh.entry.b.data() + o, 4); std::memcpy(&b1, pre.data() + o, 4); std::printf(" [%zx sh %08x gd %08x]", o, a1, b1); }
            }
            std::printf("\n");
        }
        if (verbose && any)
            std::printf("t=%lld y %.4f/%.4f vy %.4f/%.4f G %d/%d G2 %d/%d fl %d/%d ce %d/%d slope %d/%d snap %d/%d\n", tick, out.y, at<float>(post, po::PosY), out.vy,
                        at<double>(post, po::Vy), out.g, post[po::G], out.g2, post[po::G2], out.fl, at<int32_t>(post, po::CollisionBottomUid), out.ce,
                        at<int32_t>(post, po::CollisionTopUid), out.slope, (int)gpost(po::SlopeObj), out.snap, (int)gpost(po::SnappedTo));
        if (verbose && !out.fired.empty()) {
            std::printf("  non-solid objects the pass met (type, uid; +1000 = the gate let it act):");
            for (const auto& f : out.fired) std::printf(" %d/%d", f.first, f.second);
            std::printf("\n");
        }
    }
    std::printf("rows %lld, compared %lld (seeded %lld times), rows with any differing field %lld\n", rows, compared, seededRows, rowsBad);
    if (!gdDead.empty()) std::printf("deaths: game %lld, shadow %lld, the same row %lld\n", deathsGd, deathsShadow, deathsBothSameRow);
    for (const auto& kv : bad) {
        std::printf("  %-9s differs on %lld rows, e.g.", kv.first.c_str(), kv.second);
        for (long long t : badAt[kv.first]) std::printf(" %lld", t);
        std::printf("\n");
    }
    (void)show;
    return 0;
}
