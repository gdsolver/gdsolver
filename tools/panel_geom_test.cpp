// Checks the replay panel's pure half (src/mod/replay_panel_geom.hpp) without the game.
//
//   cmake --build build-dp --config Release --target panelgeomtest
//   build-dp/Release/panelgeomtest.exe [objrects.txt [obb.txt]]
//
// With no argument it runs hand-built cases. With an objrects.txt (and an obb.txt) it also parses them and reports how long
// that took and how many boxes a camera-sized query returns -- the numbers the panel's frame budget depends on.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "mod/replay_panel_geom.hpp"

using namespace p1::panelgeom;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define NEAR(a, b, eps) do { const double A_ = (a), B_ = (b); if (!(std::fabs(A_ - B_) <= (eps))) { \
    std::printf("FAIL %s:%d  %s = %g, expected %g (+-%g)\n", __FILE__, __LINE__, #a, A_, B_, (double)(eps)); ++g_fail; } } while (0)

static const char* kObjrects =
    "id,type,cx,cy,w,h,groups,uid,radius,rot,sy0,sy1,shz,sdir,sup,w0,h0\n"
    "1,0,15,15,30,30,0,10,0,0,0,0,0,0,0,30,30\n"            // a floor block
    "8,2,45,15,30,30,0,11,0,0,0,0,0,0,0,30,30\n"            // a spike
    "1007,20,75,15,30,30,0,12,0,0,0,0,0,0,0,30,30\n"        // a trigger: not drawn
    "392,2,375,135,26,25.5,0,84,0,-30,0,0,0,0,0,13,12\n"    // a spike turned 30 degrees (with an obb row below)
    "1,0,900,15,30,30,3,13,0,0,0,0,0,0,0,30,30\n"           // a block that belongs to a group
    "1,0,5000,15,10000,30,0,14,0,0,0,0,0,0,0,10000,30\n";   // a floor the length of the level

static const char* kObb =
    "uid,id,type,cx,cy,rot,x0,y0,x1,y1,x2,y2,x3,y3,oob\n"
    "84,999,7,375,135,-30,370.117,117.458,392.633,130.458,379.883,152.542,357.367,139.542,1\n"
    "85,999,7,375,135,-30,0,0,1,0,1,1,0,1,0\n";             // oob = 0: not an oriented box

static const char* kGroups =
    "tick,uid,cx,cy,w,h,on,rot\n"
    "init,13,0\n"
    "10,13,900,15,30,30,1,0\n"
    "50,13,900,45,30,30,1,0\n"
    "80,13,900,45,30,30,0,0\n"
    "99,99999,0,0,1,1,1,0\n"                                // an object the level lacks
    "end,120\n";

static void testParsing() {
    CHECK(num("12.5") == 12.5f);
    CHECK(num("", 7.f) == 7.f);
    CHECK(num("abc", 3.f) == 3.f);
    CHECK(inum("-12") == -12);
    CHECK(inum("5.0") == 5);

    const ObbMap obb = parseObb(kObb);
    CHECK(obb.size() == 1);
    CHECK(obb.count(84) == 1);
    NEAR(obb.at(84).first, 26.0, 0.01);
    NEAR(obb.at(84).second, 25.5, 0.01);

    const Level L = parseObjrects(kObjrects, &obb);
    CHECK(L.objs.size() == 5);
    CHECK(L.hidden == 1);
    CHECK(L.byUid.count(12) == 0);                      // the trigger is not kept
    CHECK(L.byUid.count(84) == 1);
    const Obj& turned = L.objs[(size_t)L.byUid.at(84)];
    NEAR(turned.nw, 26.0, 0.01);
    NEAR(turned.nh, 25.5, 0.01);
    CHECK(L.objs[(size_t)L.byUid.at(13)].grouped);
    CHECK(!L.objs[(size_t)L.byUid.at(10)].grouped);

    // the same turned spike without the oriented box: the bound undone is not that spike's size (the bound there is
    // not the turned box's), but it is a finite size -- and an unturned box is itself
    const Level L2 = parseObjrects(kObjrects, nullptr);
    NEAR(L2.objs[(size_t)L2.byUid.at(10)].nw, 30.0, 1e-4);

    // a query returns the boxes near x once each, and the long floor wherever it is asked
    std::vector<int> got;
    L.query(0.f, 60.f, [&](int i) { got.push_back(L.objs[(size_t)i].uid); });
    std::sort(got.begin(), got.end());
    CHECK((got == std::vector<int>{10, 11, 14}));
    got.clear();
    L.query(880.f, 920.f, [&](int i) { got.push_back(L.objs[(size_t)i].uid); });
    std::sort(got.begin(), got.end());
    CHECK((got == std::vector<int>{13, 14}));
    got.clear();
    L.query(-5000.f, -4000.f, [&](int i) { got.push_back(L.objs[(size_t)i].uid); });
    CHECK(got.empty());
    got.clear();
    L.query(4990.f, 5010.f, [&](int i) { got.push_back(L.objs[(size_t)i].uid); });
    CHECK(got.size() == 1 && got[0] == 14);

    // the moving block
    const Movers mv = parseGroups(kGroups, L, 1000);
    CHECK(mv.m.size() == 1);
    CHECK(mv.cover == 120);
    CHECK(mv.rowsRead == 3);
    const Mover& m = mv.m[0];
    const Obj& o = L.objs[(size_t)m.obj];
    CHECK(o.uid == 13);
    CHECK(!m.initOn);
    Pose p = moverPose(o, m, 5);
    CHECK(!p.on && p.cy == 15.f);                       // before its first row: where it loads, switched as `init` says
    p = moverPose(o, m, 10);
    CHECK(p.on && p.cy == 15.f);
    p = moverPose(o, m, 49);
    CHECK(p.on && p.cy == 15.f);
    p = moverPose(o, m, 50);
    CHECK(p.on && p.cy == 45.f);
    p = moverPose(o, m, 79);
    CHECK(p.on && p.cy == 45.f);
    p = moverPose(o, m, 5000);
    CHECK(!p.on);                                       // holds its last row
    CHECK(m.bx0 <= 900.f - 15.f && m.bx1 >= 900.f + 15.f);        // where it can ever be: its load pose and its rows
    CHECK(m.by0 <= 15.f - 15.f && m.by1 >= 45.f + 15.f);
    const Movers cut = parseGroups(kGroups, L, 60);
    CHECK(cut.m[0].rows.size() == 2);
    CHECK(cut.cover == 120);
}

// The model's plan (the DP's walk, `<out>.trace.csv`): columns by name, the rows after the walk's own kill left out, a last
// line that is not finished left out, and the splice onto GD's own path.
static const char* kTrace =
    "tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act,flip,frame,rotspent,rotchan,rotrev,dead,key\n"
    "100,500.5,105,0,0,1,0,0,0,0,0,0,0,7,0,0,0,11\n"
    "101,502.0,105,0,0,1,0,0,0,0,0,0,0,7,0,0,0,12\n"
    "102,503.5,112.5,5.5,1,0,1,300,-5.5,0,0,1,2,7,0,0,0,13\n"
    "103,505.0,119,6,1,0,1,290,-6,0,0,1,2,7,0,0,1,14\n"      // the walk's own kill: the last row that counts
    "104,506.5,125,6,1,0,1,280,-6,0,0,1,2,7,0,0,1,15\n"      // a corpse row: left out
    "105,508.0,1";                                           // a line the solver is still writing: left out

static void testModelTrace() {
    const Track m = parseModelTrace(kTrace);
    CHECK(m.first == 100 && m.last == 103);
    CHECK(m.modelFrom == 100);
    CHECK(m.at(99) == nullptr && m.at(104) == nullptr);
    const Frame* a = m.at(100);
    CHECK(a && a->model && a->valid);
    NEAR(a->x, 500.5, 1e-4); NEAR(a->y, 105.0, 1e-4);
    CHECK(a->mode == 0 && a->og == 1 && a->dual == 0 && a->flip == 0);
    const Frame* c = m.at(102);
    CHECK(c && c->mode == 1 && c->dual == 1 && c->flip == 1 && c->og == 0 && c->gframe == 2);
    NEAR(c->y2, 300.0, 1e-4); NEAR(c->vy, 5.5, 1e-4); NEAR(c->x2, 503.5, 1e-4);
    CHECK(parseModelTrace("").first == -1);                         // nothing
    CHECK(parseModelTrace("tick,x,y\n100,1,2").first == -1);        // no finished row (the last line is still being written)
    CHECK(parseModelTrace("tick,x\n100,1\n").first == -1);          // no y column: nothing to draw
    // columns found by name: a column in front moves nothing
    const Track m2 = parseModelTrace("a,tick,vy,x,y,mode,grounded,dual,y2,flip,frame,dead\n9,7,1.5,20,30,4,0,0,0,0,0,0\n");
    CHECK(m2.first == 7 && m2.at(7) && m2.at(7)->mode == 4);
    NEAR(m2.at(7)->x, 20.0, 1e-4); NEAR(m2.at(7)->y, 30.0, 1e-4);

    // the splice: GD's own frames up to the anchor, the model's from there on
    Track gd;
    gd.f.resize(150);
    for (size_t i = 0; i < gd.f.size(); ++i) { gd.f[i].x = (float)i; gd.f[i].y = 50.f; gd.f[i].valid = true; }
    gd.finish();
    const Track s = spliceModel(&gd, m);
    CHECK(s.first == 0 && s.last == 103);                            // the model's last row ends it, GD's later ones are not used
    CHECK(s.modelFrom == 100);
    CHECK(s.at(99) && !s.at(99)->model);
    NEAR(s.at(99)->x, 99.0, 1e-4);
    CHECK(s.at(100) && s.at(100)->model);
    NEAR(s.at(100)->x, 500.5, 1e-4);
    CHECK(s.at(104) == nullptr);
    const Track s0 = spliceModel(nullptr, m);                        // no path of GD's: only the plan
    CHECK(s0.first == 100 && s0.last == 103 && s0.modelFrom == 100);
    const Track none = spliceModel(&gd, Track{});                    // no plan: GD's path as it is
    CHECK(none.first == 0 && none.last == 149 && none.modelFrom == -1);

    // drawn: the plan's part of the path and the player take the model's colour, the status line says so
    Level L = parseObjrects(kObjrects, nullptr);
    Camera cam;
    Scene S;
    FrameIn in;
    in.level = &L; in.track = &s; in.W = 320.f; in.H = 180.f;
    in.a = s.first; in.z = s.last;
    in.t = 101;
    const std::string planned = buildFrame(S, in, cam);
    CHECK(planned.find("planned path") == 0);
    in.t = 50;
    Camera cam2;
    const std::string gdPath = buildFrame(S, in, cam2);
    CHECK(gdPath.find("GD path") == 0);

    // the ring: a plan is kept compact (frames from its first tick), and the ones that are not the focus are drawn beside it
    const auto g = ghostOf(m);
    CHECK(g->first() == 100 && g->last() == 103 && g->f.size() == 4);
    CHECK(g->at(99) == nullptr && g->at(104) == nullptr && g->at(102) && g->at(102)->mode == 1);
    NEAR(g->endX(), 505.0, 1e-4);
    const Track s2 = spliceGhost(&gd, *g);
    CHECK(s2.first == s.first && s2.last == s.last && s2.modelFrom == s.modelFrom);
    CHECK(ghostOf(Track{})->f.empty());
    Track m3 = parseModelTrace("tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act,flip,frame,rotspent,rotchan,rotrev,dead,key\n"
                               "100,500,105,0,0,1,0,0,0,0,0,0,0,7,0,0,0,1\n101,501,106,0,0,1,0,0,0,0,0,0,0,7,0,0,0,1\n"
                               "102,502,107,0,0,1,0,0,0,0,0,0,0,7,0,0,0,1\n");
    std::vector<std::shared_ptr<const Ghost>> ring{ghostOf(m3), g};
    View vw;
    vw.cx = 500.f; vw.cy = 110.f; vw.scale = 1.f; vw.px = 160.f; vw.py = 90.f;
    Scene gs;
    gs.reset(Box{0.f, 0.f, 320.f, 180.f});
    drawGhosts(gs, vw, ring, g.get(), 101);                          // g is the focus: only m3 is drawn
    const size_t one = gs.prims.size();
    CHECK(one > 0);
    Scene gs2;
    gs2.reset(Box{0.f, 0.f, 320.f, 180.f});
    drawGhosts(gs2, vw, ring, nullptr, 101);                         // no focus: both
    CHECK(gs2.prims.size() > one);
    Scene gs3;
    gs3.reset(Box{0.f, 0.f, 320.f, 180.f});
    drawGhosts(gs3, vw, ring, nullptr, 5000);                        // a tick neither plan reaches: the lines and the crosses stay
    CHECK(gs3.prims.size() > 0 && gs3.prims.size() < gs2.prims.size());
    // a ghost's player is a figure of its mode (the path's own player's shape), drawn as faintly as its line: nothing of it is brighter than the line
    for (const Prim& pr : gs2.prims) CHECK(pr.line.a <= 0.76f && pr.fill.a <= 0.76f);
    in.ghosts = &ring; in.focus = g.get(); in.t = 101;
    Camera cam3;
    CHECK(buildFrame(S, in, cam3).find("planned path") == 0);        // and a whole frame with a ring still builds
}
static void testRotation() {
    float q[8];
    rotRectCorners(375.f, 135.f, 26.f, 25.5f, -30.f, q);   // GD's own corners for this spike (the obb row above)
    NEAR(q[0], 370.117, 0.01); NEAR(q[1], 117.458, 0.01);
    NEAR(q[2], 392.633, 0.01); NEAR(q[3], 130.458, 0.01);
    NEAR(q[4], 379.883, 0.01); NEAR(q[5], 152.542, 0.01);
    NEAR(q[6], 357.367, 0.01); NEAR(q[7], 139.542, 0.01);
    CHECK(isTurned(-30.f) && !isTurned(90.f) && !isTurned(-180.f) && isTurned(44.f));
    // a quarter turn swaps the sides
    Obj o; o.w = 30; o.h = 10; o.rot = 90;
    setNominal(o, nullptr);
    NEAR(o.nw, 10.0, 1e-4); NEAR(o.nh, 30.0, 1e-4);
}

static void testClock() {
    Track tr;
    tr.f.resize(1001);
    for (size_t i = 100; i <= 1000; ++i) tr.f[i].valid = true;
    tr.finish();
    long long a = 0, z = 0;
    stretchOf(tr, 0, a, z);
    CHECK(a == 100 && z == 1000);                      // span 0: the whole path
    stretchOf(tr, 300, a, z);
    CHECK(a == 700 && z == 1000);                      // the last 300 ticks, ending at the frontier
    stretchOf(tr, 5000, a, z);
    CHECK(a == 100 && z == 1000);                      // a span longer than the path is the path

    Clock c;
    c.tick = 700.0;
    CHECK(!c.advance(0.5, 1.0, 700, 1000));
    NEAR(c.tick, 820.0, 1e-9);                         // 240 ticks to the second
    CHECK(!c.advance(0.25, 2.0, 700, 1000));
    NEAR(c.tick, 940.0, 1e-9);                         // twice as fast
    CHECK(!c.advance(0.5, 1.0, 700, 1000));
    NEAR(c.tick, 1000.0, 1e-9);                        // stops at the frontier...
    CHECK(c.hold > 1.4);
    CHECK(!c.advance(1.0, 1.0, 700, 1000));
    NEAR(c.tick, 1000.0, 1e-9);                        // ...and stands there
    CHECK(c.advance(0.6, 1.0, 700, 1000));             // 1.5 s later it starts over, and says so
    NEAR(c.tick, 700.0, 1e-9);
    CHECK(c.hold == 0.0);
    // a clock left before the stretch (the stretch moved on) is brought to its start
    Clock d;
    d.tick = 10.0;
    d.advance(0.001, 1.0, 700, 1000);
    CHECK(d.tick >= 700.0);
}

static bool allInside(const Scene& s) {
    for (const Prim& p : s.prims) {
        const size_t n = p.kind == Prim::Poly ? (size_t)p.n : p.kind == Prim::Seg ? 2u : 1u;
        for (size_t i = 0; i < n; ++i) {
            const float x = s.pts[p.off + 2 * i], y = s.pts[p.off + 2 * i + 1];
            if (x < s.clip.x0 - 1e-3f || x > s.clip.x1 + 1e-3f || y < s.clip.y0 - 1e-3f || y > s.clip.y1 + 1e-3f)
                return false;
        }
    }
    return true;
}

static void testClip() {
    Scene s;
    s.reset(Box{0.f, 0.f, 100.f, 50.f});
    CHECK(s.rect(10, 10, 20, 20, Color{1, 1, 1, 1}));                     // inside: as given
    CHECK(s.prims.back().n == 4);
    CHECK(!s.rect(200, 10, 220, 20, Color{1, 1, 1, 1}));                   // outside: dropped
    CHECK(!s.rect(-30, 10, -10, 20, Color{1, 1, 1, 1}));
    const size_t before = s.prims.size();
    CHECK(s.rect(-500, -500, 500, 500, Color{1, 1, 1, 1}, 1.f, Color{1, 0, 0, 1}));   // covers the box: the box itself
    CHECK(s.prims.size() == before + 1);
    CHECK(s.prims.back().n == 4);
    CHECK(s.prims.back().lw == 0.f);                                        // a cut shape keeps no border
    CHECK(allInside(s));
    // a triangle poking out of the right edge: clipped to a quadrilateral with a vertex on the edge
    const float tri[6] = {90, 10, 130, 25, 90, 40};
    CHECK(s.poly(tri, 3, Color{1, 1, 1, 1}));
    const Prim& tp = s.prims.back();
    CHECK(tp.n == 4);
    bool onEdge = false;
    for (int i = 0; i < tp.n; ++i) if (std::fabs(s.pts[tp.off + 2 * (size_t)i] - 100.f) < 1e-3f) onEdge = true;
    CHECK(onEdge);
    CHECK(allInside(s));
    // segments
    Scene g;
    g.reset(Box{0.f, 0.f, 100.f, 50.f});
    CHECK(g.seg(-50, 25, 50, 25, 1.f, Color{1, 1, 1, 1}));
    NEAR(g.pts[0], 0.0, 1e-3); NEAR(g.pts[2], 50.0, 1e-3);
    CHECK(!g.seg(-50, 60, 150, 60, 1.f, Color{1, 1, 1, 1}));                // parallel and outside
    CHECK(!g.seg(110, 0, 200, 40, 1.f, Color{1, 1, 1, 1}));
    CHECK(g.seg(-10, -10, 110, 60, 1.f, Color{1, 1, 1, 1}));                // crosses a corner region
    CHECK(allInside(g));
    CHECK(!g.dot(200, 10, 2.f, Color{1, 1, 1, 1}));
    CHECK(g.dot(10, 10, 2.f, Color{1, 1, 1, 1}));
}

static void testDraw() {
    Scene s;
    s.reset(Box{0.f, 0.f, 320.f, 146.f});
    View v;
    v.cx = 100.f; v.cy = 60.f; v.scale = 0.5f; v.px = 160.f; v.py = 73.f;

    // every mode, every gameplay frame, both gravities: something is drawn and nothing leaves the panel
    for (int mode = 0; mode < 8; ++mode)
        for (int gframe = 0; gframe < 4; ++gframe)
            for (int flip = 0; flip < 2; ++flip) {
                Frame f;
                f.x = 100.f; f.y = 60.f; f.vy = 4.f; f.mode = (signed char)mode; f.gframe = (signed char)gframe;
                f.flip = (signed char)flip; f.og = 1; f.valid = true;
                Frame nx = f; nx.x = 101.5f;
                const size_t before = s.prims.size();
                drawPlayer(s, v, f, nullptr, &nx, pal::gd(), true);
                CHECK(s.prims.size() > before + 4);
            }
    CHECK(allInside(s));

    // the cube's face sits ahead and above its floor: in frame 0 to the right and up, in frame 1 (running -y)
    // below and to the right of the centre
    for (int gframe = 0; gframe < 2; ++gframe) {
        Scene c;
        c.reset(Box{0.f, 0.f, 320.f, 146.f});
        Frame f;
        f.x = 100.f; f.y = 60.f; f.mode = 0; f.gframe = (signed char)gframe; f.valid = true;
        drawPlayer(c, v, f, nullptr, nullptr, pal::gd(), true);
        // the last Poly is the face
        const Prim* face = nullptr;
        for (const Prim& p : c.prims) if (p.kind == Prim::Poly) face = &p;
        CHECK(face && face->n == 4);
        float cx = 0, cy = 0;
        for (int i = 0; i < 4; ++i) { cx += c.pts[face->off + 2 * (size_t)i] / 4; cy += c.pts[face->off + 2 * (size_t)i + 1] / 4; }
        if (gframe == 0) { CHECK(cx > v.px && cy > v.py); }
        else             { CHECK(cx > v.px && cy < v.py); }
    }

    // a spike turned off the axes is drawn as a turned box (four corners, no horizontal top edge)
    const ObbMap obbMap = parseObb(kObb);
    Level L = parseObjrects(kObjrects, &obbMap);
    Scene t;
    t.reset(Box{0.f, 0.f, 320.f, 146.f});
    View w;
    w.cx = 375.f; w.cy = 135.f; w.scale = 1.f; w.px = 160.f; w.py = 73.f;
    const Obj& o = L.objs[(size_t)L.byUid.at(84)];
    drawObject(t, w, o, staticPose(o), false);
    CHECK(t.prims.size() == 1 && t.prims[0].kind == Prim::Poly && t.prims[0].n == 4);
    bool level = false;
    for (int i = 0; i < 4; ++i) {
        const float y0 = t.pts[t.prims[0].off + 2 * (size_t)i + 1], y1 = t.pts[t.prims[0].off + 2 * (size_t)((i + 1) % 4) + 1];
        if (std::fabs(y0 - y1) < 1e-3f) level = true;
    }
    CHECK(!level);
    // a switched-off mover is an outline only
    Scene off;
    off.reset(Box{0.f, 0.f, 320.f, 146.f});
    Pose p = staticPose(o);
    p.on = false;
    drawObject(off, w, o, p, true);
    for (const Prim& pr : off.prims) CHECK(pr.kind == Prim::Seg);
    CHECK(!off.prims.empty());

    // a path across the panel
    Track tr;
    tr.f.resize(400);
    for (size_t i = 0; i < tr.f.size(); ++i) { tr.f[i].x = (float)i * 1.5f; tr.f[i].y = 60.f; tr.f[i].valid = i >= 10; }
    tr.finish();
    CHECK(tr.first == 10 && tr.last == 399);
    CHECK(tr.has(10) && !tr.has(9) && !tr.has(400));
    Scene ps;
    ps.reset(Box{0.f, 0.f, 320.f, 146.f});
    View pv;
    pv.cx = 300.f; pv.cy = 60.f; pv.scale = 0.5f; pv.px = 160.f; pv.py = 73.f;
    drawPath(ps, pv, tr, 200);
    CHECK(!ps.prims.empty());
    CHECK(allInside(ps));
}

// The moving-parts recording of a real run (dp_groups.txt), parsed against the same level, and every mover asked for its pose
// across the run: counts, and the time it took. (Under AddressSanitizer this is also the heap check of the reader.)
static void testRealGroups(const char* objrects, const char* obbPath, const char* groups) {
    auto slurp = [](const char* path) {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string obbText = obbPath ? slurp(obbPath) : std::string();
    const ObbMap obb = parseObb(obbText);
    const Level L = parseObjrects(slurp(objrects), obbPath ? &obb : nullptr);
    const std::string text = slurp(groups);
    const auto t0 = std::chrono::steady_clock::now();
    const Movers mv = parseGroups(text, L, 1000000000LL);
    const auto t1 = std::chrono::steady_clock::now();
    size_t on = 0;
    for (long long t = 0; t < 40000; t += 97)
        for (const Mover& m : mv.m) on += moverPose(L.objs[(size_t)m.obj], m, t).on ? 1u : 0u;
    std::printf("real groups: %zu bytes -> %zu movers, %lld rows, cover %lld, parsed in %.1f ms (poses on: %zu)\n",
                text.size(), mv.m.size(), mv.rowsRead, mv.cover,
                std::chrono::duration<double, std::milli>(t1 - t0).count(), on);
    CHECK(!mv.m.empty());
}

static void testReal(const char* objrects, const char* obbPath) {
    auto slurp = [](const char* path) {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string text = slurp(objrects);
    const std::string obbText = obbPath ? slurp(obbPath) : std::string();
    const auto t0 = std::chrono::steady_clock::now();
    const ObbMap obb = parseObb(obbText);
    const Level L = parseObjrects(text, obbPath ? &obb : nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("real: %zu bytes -> %zu objects (%zu hidden), %zu oriented boxes, x %.0f..%.0f, parsed in %.1f ms\n",
                text.size(), L.objs.size(), L.hidden, obb.size(), L.minX, L.maxX,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    CHECK(!L.objs.empty());
    Scene s;
    s.reset(Box{0.f, 0.f, 320.f, 146.f});
    View v;
    v.scale = 0.5f; v.px = 160.f; v.py = 73.f; v.cy = 300.f;
    size_t maxN = 0, maxPrims = 0;
    const auto t2 = std::chrono::steady_clock::now();
    int frames = 0;
    for (float x = L.minX; x < L.maxX; x += 40.f, ++frames) {
        v.cx = x;
        s.reset(Box{0.f, 0.f, 320.f, 146.f});
        size_t n = 0;
        L.query(v.worldX0(s.clip), v.worldX1(s.clip), [&](int i) { ++n; drawObject(s, v, L.objs[(size_t)i], staticPose(L.objs[(size_t)i]), false); });
        maxN = std::max(maxN, n);
        maxPrims = std::max(maxPrims, s.prims.size());
        CHECK(allInside(s));
    }
    const auto t3 = std::chrono::steady_clock::now();
    std::printf("real: %d camera positions, most boxes in one query %zu, most primitives in one frame %zu, "
                "%.1f us a frame\n", frames, maxN, maxPrims,
                std::chrono::duration<double, std::micro>(t3 - t2).count() / std::max(1, frames));
}

// ---- --render: the panel for one tick of a real run, as an SVG, with no game in sight
//
//   panelgeomtest --render objrects.txt obb.txt|- groups.txt|- gd_dump.csv tick out.svg [width height [span]]
//
// What the game would hand a CCDrawNode, drawn instead as vector shapes (y flipped for SVG). The dump is a GD dump.csv
// (the attempt with the most live rows is used); the corrections are not known here, so no marks. The status line is
// printed. This is the path the picture is checked by when no game may be started.
static std::string slurpFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static Track trackFromDump(const std::string& text) {
    static const char* kModes[] = {"cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"};
    std::unordered_map<long long, std::vector<std::pair<long long, Frame>>> byAttempt;
    std::unordered_map<std::string, int> col;
    bool first = true;
    Cells cs;
    forEachLine(text, [&](std::string_view line) {
        cs.split(line);
        if (first) {
            first = false;
            for (int i = 0; i < cs.n; ++i) col[std::string(cs.c[i])] = i;
            return;
        }
        auto at = [&](const char* k) { const auto it = col.find(k); return it == col.end() ? std::string_view() : cs.at(it->second); };
        if (inum(at("dead")) != 0) return;
        Frame f;
        f.x = num(at("x")); f.y = num(at("y")); f.vy = num(at("yvel"));
        const std::string_view m = at("mode");
        for (int i = 0; i < 8; ++i) if (m == kModes[i]) f.mode = (signed char)i;
        f.flip = (signed char)(inum(at("upsideDown")) != 0);
        f.og = (signed char)(inum(at("onGround")) != 0);
        f.mini = (signed char)(num(at("vsize"), 1.f) < 0.9f);
        f.dual = (signed char)(inum(at("dual")) != 0);
        f.y2 = num(at("p2y")); f.x2 = f.x;
        f.gframe = (signed char)(inum(at("gframe")) & 3);
        f.valid = true;
        byAttempt[inum(at("attempt"))].push_back({inum(at("tick")), f});
    });
    long long best = -1; size_t most = 0;
    for (auto& kv : byAttempt) if (kv.second.size() >= most) { most = kv.second.size(); best = kv.first; }
    Track tr;
    if (best < 0) return tr;
    long long mx = 0;
    for (auto& p : byAttempt[best]) mx = std::max(mx, p.first);
    tr.f.resize((size_t)mx + 1);
    for (auto& p : byAttempt[best]) tr.f[(size_t)p.first] = p.second;
    tr.finish();
    return tr;
}

static int renderSvg(int argc, char** argv) {
    if (argc < 8) { std::printf("usage: --render objrects obb|- groups|- dump.csv tick out.svg [W H [span]]\n"); return 2; }
    const std::string obbText = std::strcmp(argv[3], "-") ? slurpFile(argv[3]) : std::string();
    const ObbMap obb = parseObb(obbText);
    const Level L = parseObjrects(slurpFile(argv[2]), &obb);
    const Track tr = trackFromDump(slurpFile(argv[5]));
    const long long tick = std::atoll(argv[6]);
    const float W = argc > 8 ? (float)std::atof(argv[8]) : 322.f, H = argc > 9 ? (float)std::atof(argv[9]) : 146.f;
    Movers mv;
    std::vector<int> moverOf;
    const bool haveGroups = std::strcmp(argv[4], "-") != 0;
    if (haveGroups) {
        mv = parseGroups(slurpFile(argv[4]), L, tr.last + 600);
        moverOf.assign(L.objs.size(), -1);
        for (size_t k = 0; k < mv.m.size(); ++k) moverOf[(size_t)mv.m[k].obj] = (int)k;
        std::printf("movers: %zu objects, %lld rows, cover %lld\n", mv.m.size(), mv.rowsRead, mv.cover);
    }
    if (tr.last < 0) { std::printf("no live rows in the dump\n"); return 1; }
    FrameIn in;
    in.level = &L; in.track = &tr; in.t = tick; in.W = W; in.H = H;
    in.a = tr.first; in.z = tr.last;
    if (haveGroups) { in.movers = &mv; in.moverOf = &moverOf; in.moversOfThisAttempt = true; }
    Scene S;
    Camera cam;
    std::string text;
    // let the camera ease in over the ticks before: a frame at 60 Hz is 4 ticks, so walk the last 40 frames up to tick
    for (long long t = std::max(tr.first, tick - 160); t <= tick; t += 4) { in.t = t; text = buildFrame(S, in, cam); }
    in.t = tick;
    text = buildFrame(S, in, cam);
    std::printf("%s\n%zu primitives\n", text.c_str(), S.prims.size());
    CHECK(allInside(S));
    const float k = 4.f;
    std::ofstream o(argv[7]);
    o << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W * k << "' height='" << H * k << "' viewBox='0 0 " << W << " " << H
      << "'><rect width='100%' height='100%' fill='#222'/>\n";
    auto rgba = [](Color c, char* b) { std::snprintf(b, 64, "rgba(%d,%d,%d,%.3f)", (int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), c.a); };
    char a[64], b[64];
    for (const Prim& p : S.prims) {
        const float* q = S.pts.data() + p.off;
        if (p.kind == Prim::Poly) {
            rgba(p.fill, a); rgba(p.line, b);
            o << "<polygon points='";
            for (int i = 0; i < p.n; ++i) o << q[2 * i] << "," << H - q[2 * i + 1] << " ";
            o << "' fill='" << a << "'";
            if (p.lw > 0.f) o << " stroke='" << b << "' stroke-width='" << p.lw << "'";
            o << "/>\n";
        } else if (p.kind == Prim::Seg) {
            rgba(p.line, b);
            o << "<line x1='" << q[0] << "' y1='" << H - q[1] << "' x2='" << q[2] << "' y2='" << H - q[3] << "' stroke='" << b
              << "' stroke-width='" << p.lw * 2 << "' stroke-linecap='round'/>\n";
        } else {
            rgba(p.line, b);
            o << "<circle cx='" << q[0] << "' cy='" << H - q[1] << "' r='" << p.lw << "' fill='" << b << "'/>\n";
        }
    }
    o << "<text x='3' y='7' font-size='5' fill='#ddd' font-family='monospace'>" << text << "</text></svg>\n";
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--render")) return renderSvg(argc, argv);
    testParsing();
    testRotation();
    testModelTrace();
    testClock();
    testClip();
    testDraw();
    if (argc > 1) testReal(argv[1], argc > 2 ? argv[2] : nullptr);
    if (argc > 3) testRealGroups(argv[1], argv[2], argv[3]);
    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("panelgeomtest: all checks passed\n");
    return 0;
}
