// shadow_state_test: fixtures for dp/shadow_state.hpp (S2 stage 2), the shadow's state as a ShadowLite. No capture needed.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/shadow_state_test.cpp
// 1. leave and re-enter (AUD-670): an object in `ever` that leaves the neighbourhood and comes back is still in it after any number of round trips; a static
//    object far behind the player leaves the list, and a player that reaches back past it marks the state ambiguous instead of answering silently.
// 2. no reuse: an object that takes the place another left is not taken for it (the lists hold uids, not neighbourhood slots).
// 3. press reset: a press empties usedRings (+0x850), and the block packed after it says so.
// 4. a run of steps: a cube on a floor with a pad and a ring, stepped 400 times with presses, once on one SelfShadow and once packed / unpacked between every two
//    steps; the images, the sets and the world's activated flags must be the same at every step.
#include "dp/shadow_state.hpp"
#include "dp/shadow.hpp"

#include <cstdio>
#include <cstring>

using namespace dp::gdcol;

static int g_fail = 0;
#define CHECK(c, what) do { if (c) std::printf("ok   %s\n", what); else { std::printf("FAIL %s\n", what); ++g_fail; } } while (0)

static GObj obj(int uid, int type, float cx, float cy, float w, float h) {
    GObj o;
    o.uid = uid; o.type = type; o.cx = cx; o.cy = cy; o.prevCx = cx; o.prevCy = cy; o.rw = w; o.rh = h; o.rx = cx - w / 2; o.ry = cy - h / 2;
    return o;
}

static std::vector<uint8_t> templateImage(float x, float y) {
    ContactShadow cs;
    ShadowIn in;
    in.x = x; in.y = y; in.grounded = true;
    cs.seed(in);
    cs.applyBody(in);
    cs.img.f32(po::PosX, x); cs.img.f32(po::PosY, y);
    cs.img.f32(po::PrevX, x); cs.img.f32(po::PrevY, y);
    cs.img.f32(0xa98, x); cs.img.f32(0xa9c, y);
    cs.img.f32(0xa00, x); cs.img.f32(0xa04, y);
    cs.img.f64(0x7c0, 5.770002);   // the x speed factor (x advances speed * this * dt a step: 1.298 px at 0.9)
    return cs.img.b;
}

static Layer layer() {
    Layer L;
    L.speedA = tmpl::kSpeedA; L.speedB = tmpl::kSpeedB; L.w764 = tmpl::kW764; L.scaleX = tmpl::kScaleX; L.scaleY = tmpl::kScaleY; L.ceilWord = tmpl::kCeilWord;
    L.maxPortalY = tmpl::kMaxPortalY; L.minPortalY = tmpl::kMinPortalY;
    return L;
}

static void setup(SelfShadow& sh, std::vector<GObj>& world, SlWorld& W, const std::vector<int>& moverUids) {
    sh.world = &world;
    sh.index();
    sh.setMovers(moverUids, false);
    sh.L = layer();
    std::vector<uint8_t> mv(world.size(), 0);
    for (size_t i = 0; i < world.size(); ++i)
        for (int u : moverUids) if (world[i].uid == u) mv[i] = 1;
    W.build(&world, mv);
}

int main() {
    const float x0 = 300.0f, y0 = 105.0f;
    // ---- 1 / 2: the lists, without stepping ----
    {
        std::vector<GObj> world;
        world.push_back(obj(10, 8, 320.0f, 92.0f, 30.0f, 4.0f));     // a pad (static) just ahead
        world.push_back(obj(11, 8, 360.0f, 92.0f, 30.0f, 4.0f));     // a pad that moves (a mover)
        world.push_back(obj(12, 0xb, 3000.0f, 200.0f, 36.0f, 36.0f)); // a ring far away (takes 11's place below)
        SelfShadow sh;
        SlWorld W;
        setup(sh, world, W, {11});
        sh.img.b = templateImage(x0, y0);
        ShadowLite sl;
        sh.ever = {10, 11};
        slPack(sh, W, sl);
        CHECK(sl.nEver == 2 && !(sl.flags & kSlOverflow), "both objects the pass saw are in the list");
        // the mover leaves the neighbourhood (far away), the player moves on; repeat pack / unpack while it is away
        world[1].cx = 9000.0f; world[1].rx = 9000.0f - 15.0f; sh.moverMoved(1);
        for (int k = 0; k < 50; ++k) {
            slUnpack(sl, sh.img.b, W, sh, -3.0e38f);
            sh.img.f32(po::PosX, x0 + 10.0f * (float)k);
            sh.atButtons = sh.img; sh.atEntry = sh.img; sh.atExit = sh.img; sh.ptButtons = sh.ptEntry = sh.pointOf(sh.img);
            slPack(sh, W, sl);
        }
        // it comes back
        world[1].cx = 820.0f; world[1].rx = 805.0f; sh.moverMoved(1);
        slUnpack(sl, sh.img.b, W, sh, -3.0e38f);
        CHECK(sh.ever.count(11) == 1, "leave / re-enter: the mover is still in ever after 50 round trips away from the neighbourhood");
        CHECK(sh.ever.count(10) == 0 && (sl.flags2 & kSlPruneW) && sl.pruneW == 335.0f, "the static pad 445 px behind the player left the list, and pruneW is its right edge");
        CHECK(!(sl.flags & kSlAmbiguous), "no step reached back past it: not ambiguous");
        // 2: another object takes the place the mover left: it is not in ever
        world[2].cx = 360.0f; world[2].cy = 92.0f; world[2].rx = 342.0f; world[2].ry = 74.0f;
        world[1].cx = 9000.0f; world[1].rx = 8985.0f;
        slUnpack(sl, sh.img.b, W, sh, -3.0e38f);
        CHECK(sh.ever.count(12) == 0 && sh.ever.count(11) == 1, "no reuse: the object now where the mover was is not taken for it");
        // the player reaches back past the dropped pad (a teleport): ambiguous
        slUnpack(sl, sh.img.b, W, sh, -3.0e38f);
        sh.img.f32(po::PosX, 320.0f);
        sh.atButtons = sh.img; sh.atEntry = sh.img; sh.atExit = sh.img; sh.ptButtons = sh.ptEntry = sh.pointOf(sh.img);
        slPack(sh, W, sl);
        CHECK((sl.flags & kSlAmbiguous) != 0, "a player rect that reaches back past pruneW marks the state ambiguous");
        // the seed rule: a static pad behind the seed's player is in ever by everW, not by the list
        ShadowLite s2;
        sh.img.f32(po::PosX, 500.0f);
        sh.ever = {10};
        slSeedRule(sh, W, s2);
        slPack(sh, W, s2);
        slUnpack(s2, sh.img.b, W, sh, -3.0e38f);
        CHECK(s2.nEver == 0 && (s2.flags2 & kSlEverW) && sh.ever.count(10) == 1, "seed rule: the static pad behind the seed is in ever by everW, the list stays empty");
    }
    // ---- 3: press reset ----
    {
        std::vector<GObj> world;
        world.push_back(obj(20, 0xb, x0 + 5.0f, y0, 36.0f, 36.0f));   // a yellow ring on the player
        SelfShadow sh;
        SlWorld W;
        setup(sh, world, W, {});
        sh.img.b = templateImage(x0, y0);
        sh.usedRings = {20};
        ShadowLite sl;
        slPack(sh, W, sl);
        CHECK(sl.nUsed == 1 && sl.used[0] == 20, "usedRings is carried");
        slUnpack(sl, sh.img.b, W, sh, -3.0e38f);
        Ctx c(sh.img);
        c.L = sh.L; c.objs = &world; c.usedRings = &sh.usedRings;
        ButtonsBody b; b.c = &c; b.touch = &sh.rt; b.usedRings = &sh.usedRings;
        ButtonsHooks hk;
        hk.updateJump = [](Ctx&, float) {};
        hk.spiderTestJump = [](Ctx&, const GObj*) { return false; };
        hk.startDashing = [](Ctx&, const GObj*) {};
        hk.stopDashing = [](Ctx&) {};
        pushButton(b, 1, hk);
        slPack(sh, W, sl);
        CHECK(sl.nUsed == 0, "press reset: the press emptied usedRings and the packed block has none");
    }
    // ---- 4: a run of steps, continuous against packed / unpacked ----
    {
        auto makeWorld = [&] {
            std::vector<GObj> w;
            w.push_back(obj(1, 8, 420.0f, 92.0f, 30.0f, 4.0f));          // a yellow pad
            w.push_back(obj(2, 0xb, 560.0f, 200.0f, 36.0f, 36.0f));      // a yellow ring
            w.push_back(obj(3, 0, 800.0f, 105.0f, 30.0f, 30.0f));        // a block
            w.push_back(obj(4, 0xc, 700.0f, 160.0f, 36.0f, 36.0f));      // a pink ring
            w.push_back(obj(5, 0x14, 1000.0f, 150.0f, 34.0f, 90.0f));    // a speed portal
            return w;
        };
        std::vector<GObj> wa = makeWorld(), wb = makeWorld();
        SelfShadow a, b;
        SlWorld Wa, Wb;
        setup(a, wa, Wa, {});
        setup(b, wb, Wb, {});
        a.img.b = templateImage(x0, y0);
        b.img.b = a.img.b;
        a.layerTime = b.layerTime = a.img.f64(po::Time);
        const std::vector<uint8_t> tmplImg = b.img.b;
        ShadowLite sl;
        slPack(b, Wb, sl);
        int bad = -1;
        long long pruned = 0;
        std::set<size_t> uncarried;   // words outside the carried set where the two differ (a word the step writes and never reads before writing)
        for (long long t = 1; t <= 400 && bad < 0; ++t) {
            SelfIn in;
            in.tick = t;
            in.press1 = (t % 37 == 3 || t == 210) ? 1 : 0;
            in.release1 = (t % 37 == 20 || t == 215) ? 1 : 0;
            a.step(in);
            slUnpack(sl, tmplImg, Wb, b, -3.0e38f);
            b.step(in);
            slPack(b, Wb, sl);
            bool wordsSame = true;
            for (size_t o = 0; o < a.img.b.size(); o += 4) {
                if (std::memcmp(a.img.b.data() + o, b.img.b.data() + o, 4) == 0) continue;
                if (slCarried(o)) wordsSame = false; else uncarried.insert(o);
            }
            // the two sets less what b drops on purpose (static, right edge at or behind pruneW: the player cannot meet it again unless it reaches back, which marks b ambiguous)
            auto live = [&](const std::set<int>& e, bool count) {
                std::set<int> r;
                for (int u : e) {
                    const long long wi = a.worldIndexOf(u);
                    if ((sl.flags2 & kSlPruneW) && wi >= 0 && !Wb.mover[(size_t)wi] && Wb.right((size_t)wi) <= sl.pruneW) { if (count) ++pruned; continue; }
                    r.insert(u);
                }
                return r;
            };
            bool same = wordsSame && !(sl.flags & (kSlAmbiguous | kSlOverflow)) && a.gate == b.gate && live(a.ever, true) == live(b.ever, false) && a.usedRings == b.usedRings && a.rt.arr == b.rt.arr && a.rt.now == b.rt.now;
            for (size_t i = 0; i < wa.size(); ++i) same = same && (wa[i].activated == wb[i].activated || ((sl.flags2 & kSlPruneW) && !Wb.mover[i] && Wb.right(i) <= sl.pruneW));   // (an activated ring b dropped behind the player)
            if (!same) {
                bad = (int)t;
                for (size_t o = 0; o < a.img.b.size(); o += 4) if (slCarried(o) && std::memcmp(a.img.b.data() + o, b.img.b.data() + o, 4)) std::printf("     +0x%zx a %08x b %08x carried %d\n", o, a.img.get<uint32_t>(o), b.img.get<uint32_t>(o), (int)slCarried(o));
                std::printf("     flags %02x flags2 %02x pruneW %.3f x %.3f / %.3f\n", sl.flags, sl.flags2, (double)sl.pruneW, (double)a.img.f32(po::PosX), (double)b.img.f32(po::PosX));
                for (size_t i = 0; i < wa.size(); ++i) std::printf("     obj %d activated a %d b %d\n", wa[i].uid, (int)wa[i].activated, (int)wb[i].activated);
                std::printf("     sets: gate %d ever %d used %d arr %d now %d\n", (int)(a.gate == b.gate), (int)(a.ever == b.ever), (int)(a.usedRings == b.usedRings), (int)(a.rt.arr == b.rt.arr), (int)(a.rt.now == b.rt.now));
            }
        }
        std::printf("info run: x %.3f -> the cube ended at x %.3f y %.3f, ever %zu, activated %d %d %d %d %d\n", (double)x0, (double)a.img.f32(po::PosX), (double)a.img.f32(po::PosY), a.ever.size(), (int)wa[0].activated, (int)wa[1].activated, (int)wa[2].activated, (int)wa[3].activated, (int)wa[4].activated);
        std::printf("info steps where a held an entry b had dropped behind the player: %lld\n", pruned);
        std::printf("info words outside the carried set that differ between the two (written, never read before written):");
        for (size_t o : uncarried) std::printf(" %zx", o);
        std::printf("\n");
        CHECK(bad < 0, "400 steps: one SelfShadow and a packed / unpacked one agree at every step (carried words, sets, activated)");
        if (bad >= 0) std::printf("     first difference at step %d\n", bad);
    }
    std::printf(g_fail ? "FAILED %d\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
