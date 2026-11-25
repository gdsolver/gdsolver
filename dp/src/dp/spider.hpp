// The spider's teleport: PlayerObject::spiderTestJump 0x4f2540 and spiderTestJumpInternal 0x4f25f0, read from the 22091 decompile (kept outside this tree) and the
// disassembly (the decompile loses the float arguments and merges the stack slots; every constant and branch below was checked against the instructions).
// What the game asks of the world:
//   GJBaseGameLayer::staticObjectsInRect 0x318160   the solids (type 0), slopes (0x19) and type 0x15 whose rect meets a rectangle, found through the layer's section cells
//   GJBaseGameLayer::damagingObjectsInRect 0x318510 the hazards (type 2 / 0x2f) that touch a rectangle: a box test, the oriented-box test for a rotated one, a circle test for a saw
// Both are answered here from Ctx::objs by the exact test, with no section cell in between (the game's cells only drop objects whose rect misses anyway, when every object is filed in each cell
// it covers; that is an assumption the replay tool measures). An object the game would skip is told apart by the inactive byte (+0x28e) alone: the byte at +0x305 the game ORs in is not in GObj.
// Not ported, counted in Ctx::unsupportedBy: the sideways player's slope rotation (FUN_4ef700 / FUN_4ef9d0, key 2005). Not written, being sprite / camera / event work:
// playSpiderDashEffect 0x4f3370, checkCameraLimitAfterTeleport 0x3427b0, gameEventTriggered.
#pragma once
#include "dp/collide.hpp"
#include "dp/shadow_lite.hpp"   // g_spiderGrid

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dp {
namespace gdcol {
namespace spider {

// The objects the two world queries search. A caller whose Ctx::objs is only the neighbourhood of the pass (the spider pad is touched inside the pass) points this at the whole level.
inline const std::vector<GObj>*& worldOverride() { static thread_local const std::vector<GObj>* w = nullptr; return w; }   // (thread_local: leveldp's search runs shadows on several threads)
struct Rect { float x, y, w, h; };
inline float minX(const Rect& r) { return r.x; }
inline float maxX(const Rect& r) { return r.x + r.w; }
inline float minY(const Rect& r) { return r.y; }
inline float maxY(const Rect& r) { return r.y + r.h; }
inline Rect objRect(const GObj& o) { return Rect{o.rx, o.ry, o.rw, o.rh}; }
inline bool meets(const Rect& a, const Rect& b) { return rectsIntersect(a.x, a.y, a.w, a.h, b.x, b.y, b.w, b.h); }

// the CRT's qsort (the game calls it through the import table): shortsort up to 8 elements, else median-of-three quicksort. Equal keys keep the order the algorithm leaves them in.
template <class T, class Cmp> inline void crtShortsort(T* lo, T* hi, Cmp& cmp) {
    while (hi > lo) {
        T* mx = lo;
        for (T* p = lo + 1; p <= hi; ++p) if (cmp(*p, *mx) > 0) mx = p;
        std::swap(*mx, *hi);
        --hi;
    }
}
template <class T, class Cmp> inline void crtQsort(T* lo, T* hi, Cmp& cmp) {
    for (;;) {
        const long size = (long)(hi - lo) + 1;
        if (size <= 8) { crtShortsort(lo, hi, cmp); return; }
        T* mid = lo + size / 2;
        if (cmp(*lo, *mid) > 0) std::swap(*lo, *mid);
        if (cmp(*lo, *hi) > 0) std::swap(*lo, *hi);
        if (cmp(*mid, *hi) > 0) std::swap(*mid, *hi);
        T* loguy = lo; T* higuy = hi;
        for (;;) {
            if (mid > loguy) { do { ++loguy; } while (loguy < mid && cmp(*loguy, *mid) <= 0); }
            if (mid <= loguy) { do { ++loguy; } while (loguy <= hi && cmp(*loguy, *mid) <= 0); }
            do { --higuy; } while (higuy > mid && cmp(*higuy, *mid) > 0);
            if (higuy < loguy) break;
            std::swap(*loguy, *higuy);
            if (mid == higuy) mid = loguy;
        }
        ++higuy;
        if (mid < higuy) { do { --higuy; } while (higuy > mid && cmp(*higuy, *mid) == 0); }
        if (mid >= higuy) { do { --higuy; } while (higuy > lo && cmp(*higuy, *mid) == 0); }
        if (higuy - lo >= hi - loguy) {
            if (lo < higuy) crtQsort(lo, higuy, cmp);
            if (loguy < hi) { lo = loguy; continue; }
        } else {
            if (loguy < hi) crtQsort(loguy, hi, cmp);
            if (lo < higuy) { hi = higuy; continue; }
        }
        return;
    }
}

// the four comparators 0x4e4590 (min Y up) 0x4e45f0 (max Y down), 0x4e4650 (min X up) 0x4e46b0 (max X down)
inline int cmpMinY(const GObj* a, const GObj* b) { return (int)(minY(objRect(*a)) * 10.0f) - (int)(minY(objRect(*b)) * 10.0f); }
inline int cmpMaxY(const GObj* a, const GObj* b) { return -(int)(maxY(objRect(*b)) * -10.0f) - (int)(maxY(objRect(*a)) * 10.0f); }
inline int cmpMinX(const GObj* a, const GObj* b) { return (int)(minX(objRect(*a)) * 10.0f) - (int)(minX(objRect(*b)) * 10.0f); }
inline int cmpMaxX(const GObj* a, const GObj* b) { return -(int)(maxX(objRect(*b)) * -10.0f) - (int)(maxX(objRect(*a)) * 10.0f); }

// --spidergrid (on; --no-spidergrid is the scan): the two queries answered from an x-column index of the level instead of a scan of every object. Each query is a thin vertical strip (the spider's ray: 8 to 32 px wide, up to the layer's
// ceiling tall), and the scan tested every object of the level for it, three times a teleport (57 us a press on the 1.4 M-press lv22 run, against 0.3 us for any other mode's). The index files each object
// that a query can return (types 0, 0x15, 0x19, 2, 0x2f) in every 64 px column its reach covers (a hazard circle: its centre +- its largest radius; else the rect), once, for the objects that never move; the movers
// the caller names are tested at every query. The candidates of the columns a strip overlaps are put back in world order and run through the same tests, so the answer is the scan's (same objects, same order:
// the qsort after it keeps equal keys in the order it is given). A strip wider than kMaxCols columns, or a shadow that does not know its movers, scans.
// SPIDERGRID_CHECK=1: every indexed answer beside the scan's, abort on a difference.
// kMaxCols was 40 (2,560 px). In a turned frame the spider's ray lies along x, up to the layer's reach (about 3,000 px = 47 columns), so every query of a sideways spider was over the limit and scanned the whole
// level: on the 154,805 spider presses of an official lv22 call (6750 layers, 8 threads) a press cost 178-233 us (staticObjectsInRect-side 103-124 us, damaging 85-99 us a teleport) against 20 us at 160,
// the head of the shadow's tick 24-38 us against 4.3 us, the same plan (MD5) and no SPIDERGRID_CHECK difference. A strip of 160 columns (10,240 px) still reads about 170 filed entries; wider scans as before.
struct Grid {
    static constexpr float kCol = 64.0f;
    static constexpr int kMaxCols = 160, kMaxSpan = 48;
    // one filed object: its world index, which query wants it (1 the solids, 2 the hazards) and the y extent its test can reach (the candidates are cut by it before they are put in order)
    struct Ent { uint32_t i; uint32_t cls; float y0, y1; };
    const std::vector<GObj>* world = nullptr;
    size_t n = 0;
    bool ok = false;
    int base = 0;
    std::vector<std::vector<Ent>> cols;
    std::vector<Ent> wide;                   // objects spanning more than kMaxSpan columns (world order)
    std::vector<uint32_t> movers;            // world indices, ascending
    mutable std::vector<uint32_t> stamp, cand;
    mutable uint32_t gen = 0;
    // The movers' reaches, kept from query to query (speed only). liveGen (the owner's; null: every query reads every mover as before) is a counter the owner raises whenever a mover may have changed (its
    // place, box, radius, scale): while it stands, mv[0] (the solids) and mv[1] (the hazards) hold each mover's world index and reach as gridReach gave them, a mover whose reach is not a number with the
    // widest reach (it stays a candidate of every query, as in the live test). A query then reads 20 bytes a mover of its class, not the object: the live walk of every mover was 7% of lv22's tick.
    struct MEnt { uint32_t i; float x0, x1, y0, y1; };
    const uint64_t* liveGen = nullptr;
    mutable uint64_t mvGen = 0;
    mutable bool mvOk = false;
    mutable std::vector<MEnt> mv[2];
};
inline const Grid*& gridPtr() { static thread_local const Grid* p = nullptr; return p; }   // the calling shadow's grid, set for the length of a step (selfshadow.hpp SpiderGridScope)
inline bool gridRelevant(const GObj& o) { return o.type == 0 || o.type == 0x19 || o.type == 0x15 || o.type == 2 || o.type == 0x2f; }
inline uint32_t gridClass(const GObj& o) { return (o.type == 2 || o.type == 0x2f) ? 2u : 1u; }
// the extent a query's test can reach for the object (a circle hazard tests its circle only; every other, its rect)
inline void gridReach(const GObj& o, float& x0, float& x1, float& y0, float& y1) {
    if ((o.type == 2 || o.type == 0x2f) && o.radius > 0.0f) {
        float R = o.radius;
        if (!(o.scaleX == 1.0f && o.scaleY == 1.0f)) { float m = o.scaleX; if (m <= o.scaleY) m = o.scaleY; R = std::max(R, m * o.radius); }
        x0 = o.cx - R; x1 = o.cx + R; y0 = o.cy - R; y1 = o.cy + R;
    } else {
        x0 = o.rx; x1 = o.rx + o.rw; if (x1 < x0) std::swap(x0, x1);
        y0 = o.ry; y1 = o.ry + o.rh; if (y1 < y0) std::swap(y0, y1);
    }
}
inline int gridColOf(float x) { return (int)std::floor(x / Grid::kCol); }
inline void gridBuild(Grid& g, const std::vector<GObj>& W, const std::vector<char>& isMover) {
    g = Grid();
    g.world = &W; g.n = W.size();
    int lo = 0, hi = -1;
    bool any = false;
    struct Span { Grid::Ent e; int a, b; };
    std::vector<Span> spans;
    for (size_t i = 0; i < W.size(); ++i) {
        const GObj& o = W[i];
        if (!gridRelevant(o)) continue;
        if (isMover[i]) { g.movers.push_back((uint32_t)i); continue; }
        float x0, x1, y0, y1;
        gridReach(o, x0, x1, y0, y1);
        Grid::Ent e{(uint32_t)i, gridClass(o), y0, y1};
        if (!(y0 > -1e30f && y1 < 1e30f)) { e.y0 = -3.0e38f; e.y1 = 3.0e38f; }   // (also a NaN: never cut)
        if (!(x0 > -1e6f && x1 < 1e7f)) { g.wide.push_back(e); continue; }
        const int a = gridColOf(x0 - 1.0f), b = gridColOf(x1 + 1.0f);
        if (b - a > Grid::kMaxSpan) { g.wide.push_back(e); continue; }
        spans.push_back(Span{e, a, b});
        if (!any) { lo = a; hi = b; any = true; } else { lo = std::min(lo, a); hi = std::max(hi, b); }
    }
    g.base = lo;
    g.cols.assign(any ? (size_t)(hi - lo + 1) : 0, std::vector<Grid::Ent>());
    for (const Span& s : spans) for (int c = s.a; c <= s.b; ++c) g.cols[(size_t)(c - lo)].push_back(s.e);
    g.stamp.assign(W.size(), 0u);
    g.ok = true;
}
// The world indices, ascending, of every object of the classes in `mask` that a query over [x0, x1] x [y0, y1] can return (a superset: the test is the caller's); false: the strip is too wide or not a number (the caller scans).
inline bool gridCandidates(const Grid& g, const std::vector<GObj>& W, float x0, float x1, float y0, float y1, uint32_t mask, std::vector<uint32_t>& out) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    if (!(x0 > -1e6f && x1 < 1e7f) || !(y0 > -1e30f && y1 < 1e30f)) return false;
    const int a = gridColOf(x0 - 1.0f), b = gridColOf(x1 + 1.0f);
    if (b - a >= Grid::kMaxCols) return false;
    const float ylo = y0 - 1.0f, yhi = y1 + 1.0f;
    out.clear();
    if (++g.gen == 0) { std::fill(g.stamp.begin(), g.stamp.end(), 0u); g.gen = 1; }
    auto take = [&](const Grid::Ent& e) {
        if ((e.cls & mask) == 0 || e.y1 < ylo || e.y0 > yhi || g.stamp[e.i] == g.gen) return;
        g.stamp[e.i] = g.gen; out.push_back(e.i);
    };
    for (int c = std::max(a, g.base); c <= b && c - g.base < (int)g.cols.size(); ++c)
        for (const Grid::Ent& e : g.cols[(size_t)(c - g.base)]) take(e);
    for (const Grid::Ent& e : g.wide) take(e);
    if (g.liveGen) {
        if (!g.mvOk || g.mvGen != *g.liveGen) {
            g.mv[0].clear(); g.mv[1].clear();
            for (uint32_t i : g.movers) {
                const GObj& o = W[i];
                Grid::MEnt e{i, 0.0f, 0.0f, 0.0f, 0.0f};
                gridReach(o, e.x0, e.x1, e.y0, e.y1);
                if (!(e.x1 == e.x1 && e.x0 == e.x0 && e.y1 == e.y1 && e.y0 == e.y0)) { e.x0 = e.y0 = -3.0e38f; e.x1 = e.y1 = 3.0e38f; }
                g.mv[gridClass(o) == 2u ? 1 : 0].push_back(e);
            }
            g.mvGen = *g.liveGen; g.mvOk = true;
        }
        for (int k = 0; k < 2; ++k) {
            if ((mask & (k ? 2u : 1u)) == 0) continue;
            for (const Grid::MEnt& e : g.mv[k]) {
                if (e.x1 < x0 - 1.0f || e.x0 > x1 + 1.0f || e.y1 < ylo || e.y0 > yhi) continue;
                out.push_back(e.i);
            }
        }
    } else for (uint32_t i : g.movers) {   // (live: a mover stands where this tick put it)
        const GObj& o = W[i];
        float ox0, ox1, oy0, oy1;
        gridReach(o, ox0, ox1, oy0, oy1);
        if ((gridClass(o) & mask) == 0) continue;
        if (ox1 < x0 - 1.0f || ox0 > x1 + 1.0f || oy1 < ylo || oy0 > yhi) {   // (a NaN in the reach fails every comparison here: it stays a candidate)
            if (ox1 == ox1 && ox0 == ox0 && oy1 == oy1 && oy0 == oy0) continue;
        }
        out.push_back(i);   // (a mover is in no column: no duplicate)
    }
    std::sort(out.begin(), out.end());
    return true;
}inline const Grid* gridFor(const std::vector<GObj>* src) {
    const Grid* g = gridPtr();
    return (g && g->ok && g->world == src && g->n == src->size()) ? g : nullptr;
}

// staticObjectsInRect (0x318160) and damagingObjectsInRect (0x318510) walk the section grid: the cells x in [(int)(sx * x0 - 1) (0 below), (int)(sx * (x0 + w) + 1)], y likewise, and an object stands in ONE
// cell only -- addToSection (0x32f280) / updateObjectSection (0x330ba0) file it by its position, (int)(posX * sx), (int)(posY * sy) -- so an object whose rect meets the strip but whose position's
// cell is outside that range is not returned: a wide platform far from the strip (a 1,080 px block, 226 px from the centre) is invisible to the spider's ray. sx = 0.01; sy = 0.01 with
// dynamicLevelHeight, else 0 (every object in row 0). The position is the object's centre here (Obj::cx / cy), as everywhere else the model files objects.
inline int secCell(double v, double k) { return (v * k <= 0.0) ? 0 : (int)(v * k); }
inline bool cellInRange(const GObj& o, const Rect& r) {
    const double sx = 0.01, sy = g_dynamicLevelHeight ? 0.01 : 0.0;
    const float x0 = r.x, x1 = r.x + r.w;
    const double lox = (sx * x0 - 1.0 >= 0.0) ? (double)(int)(sx * x0 - 1.0) : 0.0, hix = (double)(int)(sx * x1 + 1.0);
    const double ox = (double)secCell(o.cx, sx);
    if (ox < lox || ox > hix) return false;
    if (sy > 0.0) {
        const float y0 = r.y, y1 = r.y + r.h;
        const double loy = (sy * y0 - 1.0 >= 0.0) ? (double)(int)(sy * y0 - 1.0) : 0.0, hiy = (double)(int)(sy * y1 + 1.0);
        const double oy = (double)secCell(o.cy, sy);
        if (oy < loy || oy > hiy) return false;
    }
    return true;
}
inline bool staticHit(const GObj& o, const Rect& r) {
    if (o.inactive && !o.farHold) return false;
    if (!(o.type == 0 || o.type == 0x19 || o.type == 0x15)) return false;
    if (g_spiderNoExtCol && o.extCol) return false;   // an Extended Collision object is filed in no section cell (addToSection puts it in the layer's flat list): the rect queries walk the cells and never return it (a custom level t=37011: id 3610 uid 142596, GD's dhit is the spike alone)
    return meets(r, objRect(o)) && cellInRange(o, r);
}

inline float ccpDist(float ax, float ay, float bx, float by) { const float dx = ax - bx, dy = ay - by; return std::sqrt(dx * dx + dy * dy); }

inline bool hazardHit(const GObj& o, const Rect& r) {
    if (o.inactive && !o.farHold) return false;
    if (!(o.type == 2 || o.type == 0x2f)) return false;
    if (g_spiderNoExtCol && o.extCol) return false;   // an Extended Collision object is filed in no section cell (addToSection puts it in the layer's flat list): the rect queries walk the cells and never return it (a custom level t=37011: id 3610 uid 142596, GD's dhit is the spike alone)
    if (!cellInRange(o, r)) return false;
    float R = o.radius;
    if (R <= 0.0f) {
        if (!meets(r, objRect(o))) return false;
        if (objRotatedBox(o)) {
            const Obb rb = makeObb(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w, r.h, 0.0f);
            if (!obbOverlap(objObb(o), rb)) return false;
        }
        return true;
    }
    if (!(o.scaleX == 1.0f && o.scaleY == 1.0f)) { float m = o.scaleX; if (m <= o.scaleY) m = o.scaleY; R = m * o.radius; }
    const float x1 = maxX(r), x0 = minX(r), y1 = maxY(r), y0 = minY(r);
    const bool inside = o.cx >= x0 && o.cx <= x1 && o.cy >= y0 && o.cy <= y1;
    bool add = inside;
    if (!add) {
        add = !(R <= std::fabs(ccpDist(x1, y1, o.cx, o.cy))) || !(R <= std::fabs(ccpDist(x1, y0, o.cx, o.cy)))
              || !(R <= std::fabs(ccpDist(x0, y0, o.cx, o.cy))) || !(R <= std::fabs(ccpDist(x0, y1, o.cx, o.cy)));
        if (!add) {
            float qx = o.cx, qy = o.cy;
            if (x1 < qx) qx = x1;
            if (qx < x0) qx = x0;
            if (qy < y0) qy = y0;
            if (y1 < qy) qy = y1;
            add = !(R <= ccpDist(qx, qy, o.cx, o.cy));
        }
    }
    return add;
}

template <class Hit>
inline std::vector<const GObj*> inRect(const std::vector<GObj>& src, const Rect& r, uint32_t mask, Hit hit) {
    std::vector<const GObj*> v;
    if (const Grid* g = gridFor(&src)) {
        if (gridCandidates(*g, src, r.x, r.x + r.w, r.y, r.y + r.h, mask, g->cand)) {
            for (uint32_t i : g->cand) if (hit(src[i], r)) v.push_back(&src[i]);
            static const bool check = std::getenv("SPIDERGRID_CHECK") != nullptr;
            if (check) {
                std::vector<const GObj*> ref;
                for (const GObj& o : src) if (hit(o, r)) ref.push_back(&o);
                if (ref != v) { std::fprintf(stderr, "SPIDERGRID_CHECK: the index answered %zu objects, the scan %zu, for the strip x %.3f w %.3f\n", v.size(), ref.size(), r.x, r.w); std::abort(); }
            }
            return v;
        }
    }
    for (const GObj& o : src) if (hit(o, r)) v.push_back(&o);
    return v;
}

inline std::vector<const GObj*> staticInRect(Ctx& c, const Rect& r) {
    const std::vector<GObj>* src = worldOverride() ? worldOverride() : c.objs;
    if (!src) return std::vector<const GObj*>();
    return inRect(*src, r, 1u, staticHit);
}

inline std::vector<const GObj*> hazardsInRect(Ctx& c, const Rect& r) {
    const std::vector<GObj>* src = worldOverride() ? worldOverride() : c.objs;
    if (!src) return std::vector<const GObj*>();
    return inRect(*src, r, 2u, hazardHit);
}

inline bool internal(Ctx& c, bool again);
inline int& callCount() { static thread_local int n = 0; return n; }   // for the replay tool: how many times the teleport ran
inline bool& debugOn() { static bool d = false; return d; } // for the replay tool: print what the queries found
// What the last teleport asked the world and got, for stage D: the uids the exact rect test returned, in the order the game's qsort leaves them, so a live run can count the objects
// the layer's section cells would have dropped (or added).
struct LastQuery { std::vector<int> staticUids, hazardUids; bool widened = false; };
inline LastQuery& lastQuery() { static thread_local LastQuery q; return q; }

// returns false when the game returns at once (dead, +0xa32, +0x7e9)
inline bool internal(Ctx& c, bool again) {
    Mem& P = c.P;
    if (P.u8(0x9c8) || P.u8(0xa32) || P.u8(0x7e9)) return false;
    const bool sideways = P.u8(po::Sideways) != 0;
    const bool upside = P.u8(po::Upside) != 0;
    const bool leftward = P.u8(po::Leftward) != 0;
    const bool plat = P.u8(po::Platformer) != 0;
    float scale8 = P.f32(0x9f8);
    scale8 = scale8 * 8.0f;
    const bool freeLike = sideways || c.L.freeMode ||
        !(P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeBall) || P.u8(po::ModeSpider));   // bVar3: the bounds come from the position, not the portals
    Rect R0;
    playerRect(P, R0.x, R0.y, R0.w, R0.h);
    Rect A = R0;
    const float posX = P.f32(po::PosX), posY = P.f32(po::PosY);
    float lo, hi;   // fVar19, fVar20
    if (!sideways) {
        if (!freeLike) { lo = c.L.minPortalY; hi = c.L.maxPortalY; }
        else {
            const float t = posY - 3000.0f;
            lo = (90.0f < t) ? t : 90.0f;                                  // maxss
            hi = posY + 3000.0f;
        }
    } else {
        hi = posX + 3000.0f;
        const float t = posX - 3000.0f;
        lo = (0.0f < t) ? t : 0.0f;
    }
    Rect B, C;
    if (!upside) {
        if (sideways) {
            A.x = A.x + (A.w - 2.0f);
            A.w = hi - A.x;
        } else {
            A.y = A.y + (A.h - 2.0f);
            A.h = hi - A.y;
        }
    } else {
        if (sideways) {
            const float d = A.x - lo;
            A.x = lo;
            A.w = d + 2.0f;
        } else {
            const float d = A.y - lo;
            A.y = lo;
            A.h = d + 2.0f;
        }
    }
    if (sideways) {   // 0x4f2861
        B = A;
        B.h = R0.h; B.y = R0.y;
        C = B;
        C.h = C.h + 4.0f;
        B.y = B.y + (A.h - scale8) * 0.5f;
        B.h = scale8;
    } else {          // 0x4f28fe
        B = A;
        B.w = R0.w; B.x = R0.x;
        C = B;
        C.w = C.w + 4.0f;
        B.x = B.x + (A.w - scale8) * 0.5f;
        B.w = scale8;
    }
    if (!plat) {
        if (!sideways) A.w = R0.w * 0.5f + 1.0f; else A.h = R0.h * 0.5f + 1.0f;
        if (!leftward) {
            if (!sideways) A.x = R0.w * 0.5f + R0.x; else A.y = R0.h * 0.5f + R0.y;
        } else {
            if (!sideways) A.x = (R0.x - R0.w * 0.5f) - 1.0f; else A.y = (R0.y - R0.h * 0.5f) - 1.0f;
        }
    } else {
        if (!sideways) { A.x = A.x + 1.0f; A.w = A.w + -2.0f; }
        else { A.h = A.h + -2.0f; A.y = A.y + 1.0f; }
    }
    std::vector<const GObj*> stat = staticInRect(c, A);
    std::vector<const GObj*> haz = hazardsInRect(c, B);
    int (*cmp)(const GObj*, const GObj*);
    if (!sideways) cmp = upside ? cmpMaxY : cmpMinY;
    else cmp = upside ? cmpMaxX : cmpMinX;
    bool widened = again;
    if ((stat.empty() && !haz.empty()) || again) {
        widened = true;
        stat = staticInRect(c, C);
    }
    auto cmpf = [&](const GObj* a, const GObj* b) { return cmp(a, b); };
    if (!stat.empty()) crtQsort(stat.data(), stat.data() + stat.size() - 1, cmpf);
    if (!haz.empty()) crtQsort(haz.data(), haz.data() + haz.size() - 1, cmpf);

    {
        LastQuery& q = lastQuery();
        q.staticUids.clear(); q.hazardUids.clear();
        for (const GObj* o : stat) q.staticUids.push_back(o->uid);
        for (const GObj* o : haz) q.hazardUids.push_back(o->uid);
        q.widened = widened;
    }
    if (debugOn()) {
        std::printf("  spider: pos (%.4f %.4f) R0 (%.4f %.4f %.4f %.4f) band lo %.4f hi %.4f upside %d sideways %d widened %d\n", posX, posY, R0.x, R0.y, R0.w, R0.h, lo, hi, upside, sideways, widened);
        std::printf("  A (%.4f %.4f %.4f %.4f) B (%.4f %.4f %.4f %.4f) C (%.4f %.4f %.4f %.4f)\n", A.x, A.y, A.w, A.h, B.x, B.y, B.w, B.h, C.x, C.y, C.w, C.h);
        for (const GObj* o : stat) std::printf("  static uid %d type %d rect (%.4f %.4f %.4f %.4f)\n", o->uid, o->type, o->rx, o->ry, o->rw, o->rh);
        for (const GObj* o : haz) std::printf("  hazard uid %d type %d rect (%.4f %.4f %.4f %.4f) radius %.2f\n", o->uid, o->type, o->rx, o->ry, o->rw, o->rh, o->radius);
    }
    float f24 = 0.0f, f23 = 0.0f;   // xmm9, xmm7
    bool sil = false;               // bVar18
    auto coordOf = [&](const GObj& o) { return sideways ? o.cx : o.cy; };
    if (stat.empty()) {
        if (!haz.empty()) { sil = true; f23 = coordOf(*haz[0]); }
    } else {
        const int n = (int)stat.size();
        const float dimHalf = (sideways ? R0.w : R0.h) * 0.5f;
        const float posCoord = sideways ? posX : posY;
        for (int i = 0; i < n; ++i) {
            const GObj& o = *stat[(size_t)i];
            const Rect orc = objRect(o);
            if (!upside) f23 = (sideways ? minX(orc) : minY(orc)) - dimHalf;
            else f23 = dimHalf + (sideways ? maxX(orc) : maxY(orc));
            sil = false;
            if (o.type == 0x19) {
                if (sideways) unsup(c, 2005);
                const bool top = slopeIsTop(o.slopeDir);
                if (top != upside) {
                    const float oc = sideways ? o.cx : o.cy;
                    const double sy = slopeYPos(o, posX);
                    const float diff = (float)(sy - (double)o.cy);
                    const float dirf = upside ? -1.0f : 1.0f;
                    f23 = (oc + diff) - (dimHalf * dirf);
                    sil = true;
                }
                if (sideways) unsup(c, 2005);
            }
            bool skip;
            if (!upside) skip = f23 < posCoord - 10.0f;
            else skip = (posCoord + 10.0f) < f23;
            bool check = false, endLoop = false;
            if (skip) { f23 = 0.0f; continue; }
            if (plat) {
                if (!sil) endLoop = true; else check = true;
            } else if (!leftward) {
                const float v = sideways ? minY(orc) : minX(orc);
                const float ref = sideways ? posY : posX;
                if (v > ref) check = true; else endLoop = true;
            } else {
                const float v = sideways ? maxY(orc) : maxX(orc);
                const float ref = sideways ? posY : posX;
                if (ref > v) check = true; else endLoop = true;
            }
            if (endLoop) break;
            (void)check;
            bool hit = false;
            for (int j = 0; j < n; ++j) {
                if (j == i) continue;
                Rect T = R0;
                if (!sideways) T.y = f23 - R0.h * 0.5f; else T.x = f23 - R0.w * 0.5f;
                Rect rj = objRect(*stat[(size_t)j]);
                if (!sil) {
                    if (!sideways) { rj.h = rj.h + -4.0f; rj.y = rj.y + 2.0f; }
                    else { rj.w = rj.w + -4.0f; rj.x = rj.x + 2.0f; }
                }
                if (meets(rj, T)) { hit = true; break; }
            }
            if (!hit) break;                       // this candidate stands
            if (sil) {
                if (f24 == 0.0f) f24 = f23;
                else if (!upside) { if (f24 > f23) f24 = f23; }
                else { if (f23 > f24) f24 = f23; }
            }
            f23 = 0.0f;
        }
        sil = false;                                // 0x4f2e44
    }
    if (!plat) f24 = 0.0f;
    else if (f24 != 0.0f) {
        if (!upside) { if (f23 > f24) f23 = f24; }
        else { if (f24 > f23) f23 = f24; }
    }
    if (f23 == 0.0f) {
        if (f24 != 0.0f) f23 = f24;
        else if (!upside) f23 = hi - (sideways ? R0.w : R0.h) * 0.5f;
        else f23 = (sideways ? R0.w : R0.h) * 0.5f + lo;
    }
    if (!sil && !haz.empty()) {
        const float h = coordOf(*haz[0]);
        const bool proceed = !upside ? (f23 > h) : (h > f23);
        if (proceed) {
            if (!widened) return internal(c, true);
            f23 = h;
        }
    }
    ++callCount();
    float nx = P.f32(po::PosX), ny = P.f32(po::PosY);
    if (sideways) nx = f23; else ny = f23;
    setPosition(P, nx, ny);
    P.u8(0x369, 1);                                                // the player's setPosition (vtable +0xb8) also marks +0x369, as in upd.hpp
    playerFlipGravity(c, upside ? 0 : 1, true);
    setYVelocity(P, P.u8(po::Upside) ? 1.0 : -1.0);
    P.u8(0x7f0, 0);
    P.u8(0x98d, 0);
    c.topDict.clear();
    c.botDict.clear();
    P.put<int64_t>(0x5e0, -1);
    P.put<int64_t>(0x5d0, -1);
    P.put<int64_t>(0x5d8, -1);
    if (sideways) P.f32(0xa98, nx); else P.f32(0xa9c, ny);
    P.put<uint64_t>(0x828, P.get<uint64_t>(0xaa8));
    if (P.u8(0x7ec) && !P.u8(0x733)) {                             // playSpiderDashEffect 0x4f3370: the sprite effects aside, it writes the second colour and the fade
        for (size_t k = 0x820; k < 0x826; ++k) P.u8(k, 0xff);
        P.put<uint64_t>(0x810, P.get<uint64_t>(0xaa8));
        P.put<uint32_t>(0x818, 0x3e99999au);
        P.put<uint32_t>(0x81c, 0x3d4ccccdu);
    }
    return true;
}

// PlayerObject::spiderTestJump 0x4f2540
inline bool spiderTestJump(Ctx& c) {
    Mem& P = c.P;
    const bool did = internal(c, false);
    if (!(P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeBall) || P.u8(po::ModeSpider))) {
        float a;
        if (P.u8(po::Sideways)) a = P.u8(po::Upside) ? 90.0f : 270.0f;
        else a = P.u8(po::Upside) ? 180.0f : 0.0f;
        if (P.f32(0x38) != a || P.f32(0x3c) != a) {                 // PlayerObject::setRotation 0x501e70
            P.put<uint16_t>(0x368, 0x101);
            P.u8(0x350, 1);
            P.f32(0x38, a); P.f32(0x3c, a);
            P.u8(0x390, (a == 90.0f || a == -90.0f || a == 270.0f || a == -270.0f) ? 1 : 0);
        }
    }
    return did;
}

}  // namespace spider
}  // namespace gdcol
}  // namespace dp
