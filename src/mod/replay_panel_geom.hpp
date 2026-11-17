#pragma once
// The replay panel's pure half: the level's boxes, GD's recorded path of the deepest verified attempt, and the
// primitives (clipped polygons, segments, dots) that draw them. No cocos, no Geode, no game -- only the standard
// library -- so tools/panel_geom_test.cpp builds and checks it on its own. replay_panel.hpp is the other half: it
// feeds this one from the mod's state and hands the result to a CCDrawNode.
//
// The picture is the one py/divergence_view.py draws (boxes, not artwork; a plain silhouette per mode), in the
// game's own coordinates: x right, y UP, which is also cocos' orientation, so nothing is flipped on the way out.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace p1 {
namespace panelgeom {

constexpr float kPi = 3.14159265358979f;

struct Color { float r = 0.f, g = 0.f, b = 0.f, a = 0.f; };
inline Color hex(unsigned v, float a = 1.f) {
    return Color{((v >> 16) & 255u) / 255.f, ((v >> 8) & 255u) / 255.f, (v & 255u) / 255.f, a};
}
inline Color withAlpha(Color c, float a) { c.a = a; return c; }

// ------------------------------------------------------------------ text readers

// A field as a number. strtof wants a NUL, and a line of a mmap-less string_view has none, so the field is copied
// out first (fields are short). 0 / the default for an empty or unparsable one.
inline float num(std::string_view f, float d = 0.f) {
    char b[48];
    const size_t n = std::min(f.size(), sizeof(b) - 1);
    std::memcpy(b, f.data(), n);
    b[n] = 0;
    char* end = nullptr;
    const float v = std::strtof(b, &end);
    return end == b ? d : v;
}
inline long long inum(std::string_view f, long long d = 0) {
    char b[32];
    const size_t n = std::min(f.size(), sizeof(b) - 1);
    std::memcpy(b, f.data(), n);
    b[n] = 0;
    char* end = nullptr;
    const long long v = std::strtoll(b, &end, 10);
    if (end == b) {                      // "12.0"-style cells: through the float reader
        const float x = num(f, (float)d);
        return (long long)x;
    }
    return v;
}

// Calls fn(line) for each line of `text`, without its '\n' / '\r'.
template <class F>
inline void forEachLine(std::string_view text, F&& fn) {
    size_t p = 0;
    while (p < text.size()) {
        size_t q = text.find('\n', p);
        if (q == std::string_view::npos) q = text.size();
        size_t e = q;
        if (e > p && text[e - 1] == '\r') --e;
        fn(text.substr(p, e - p));
        p = q + 1;
    }
}

// The cells of one line, in order. A fixed-capacity array: the widest file here has 70 columns.
struct Cells {
    static constexpr int kMax = 96;
    std::string_view c[kMax];
    int n = 0;
    void split(std::string_view line) {
        n = 0;
        size_t p = 0;
        while (n < kMax) {
            const size_t q = line.find(',', p);
            if (q == std::string_view::npos) { c[n++] = line.substr(p); break; }
            c[n++] = line.substr(p, q - p);
            p = q + 1;
        }
    }
    std::string_view at(int i) const { return (i >= 0 && i < n) ? c[i] : std::string_view(); }
};

// ------------------------------------------------------------------ the level

// A box the physics reads (one row of objrects.txt). `rot` is GD's clockwise degrees. For a hazard or block turned by
// something other than a quarter turn, (w, h) is not the box's own size, so `nw`/`nh` carry that when it is known:
// from GD's oriented box (obb.txt) exactly, else for an unturned one the size itself, else the bound undone. -1 = not
// known (the object is then drawn as its bound).
struct Obj {
    int id = 0, type = 0, uid = -1;
    float cx = 0.f, cy = 0.f, w = 30.f, h = 30.f, radius = 0.f, rot = 0.f, sy0 = 0.f, sy1 = 0.f;
    int sdir = 0;
    float nw = -1.f, nh = -1.f;
    bool grouped = false;           // belongs to a group, so a trigger could move it
};

inline bool isTurned(float rot) {
    const float m = std::fmod(std::fmod(rot, 90.f) + 90.f, 90.f);
    return m > 0.01f && m < 89.99f;
}

// The box's own size, from the pieces above. obb = GD's oriented box sides for this object (or null).
inline void setNominal(Obj& o, const float* obb) {
    if (obb) { o.nw = obb[0]; o.nh = obb[1]; return; }
    if (!isTurned(o.rot)) {
        const long long q = std::llround(o.rot / 90.f);
        const bool odd = (q % 2) != 0;
        o.nw = odd ? o.h : o.w;
        o.nh = odd ? o.w : o.h;
        return;
    }
    const float a = o.rot * kPi / 180.f, c = std::fabs(std::cos(a)), s = std::fabs(std::sin(a));
    const float det = c * c - s * s;
    if (std::fabs(det) > 0.2f) {                 // near 45 degrees the bound cannot be undone
        o.nw = (o.w * c - o.h * s) / det;
        o.nh = (o.h * c - o.w * s) / det;
    }
}

// Types the physics does not draw as a thing: 7 (decoration the exporter keeps) and 20 (triggers).
inline bool hiddenType(int t) { return t == 7 || t == 20; }

// obb.txt: uid,id,type,cx,cy,rot,x0,y0,x1,y1,x2,y2,x3,y3,oob -- the corners of GD's oriented box for every object
// turned by something other than a quarter turn. What is kept is the box's own two sides, uid -> (side 0-1, side 1-2).
using ObbMap = std::unordered_map<int, std::pair<float, float>>;
inline ObbMap parseObb(std::string_view text) {
    ObbMap out;
    int cUid = -1, cOob = -1, cX[6] = {-1, -1, -1, -1, -1, -1};
    bool first = true;
    Cells cs;
    forEachLine(text, [&](std::string_view line) {
        cs.split(line);
        if (first) {
            first = false;
            for (int i = 0; i < cs.n; ++i) {
                const std::string_view h = cs.c[i];
                if (h == "uid") cUid = i;
                else if (h == "oob") cOob = i;
                else if (h == "x0") cX[0] = i;
                else if (h == "y0") cX[1] = i;
                else if (h == "x1") cX[2] = i;
                else if (h == "y1") cX[3] = i;
                else if (h == "x2") cX[4] = i;
                else if (h == "y2") cX[5] = i;
            }
            return;
        }
        if (cUid < 0 || cOob < 0 || cX[5] < 0 || cs.n <= std::max(cOob, cX[5])) return;
        if (cs.c[cOob] != "1") return;
        float c[6];
        for (int k = 0; k < 6; ++k) c[k] = num(cs.c[cX[k]]);
        out[(int)inum(cs.c[cUid], -1)] = {std::hypot(c[2] - c[0], c[3] - c[1]), std::hypot(c[4] - c[2], c[5] - c[3])};
    });
    return out;
}

// The level, indexed by x so a frame asks only for the boxes under the camera. Built once and never changed after:
// the panel holds it as a shared_ptr<const Level> and the builder thread hands it over whole.
struct Level {
    std::vector<Obj> objs;
    std::unordered_map<int, int> byUid;     // uid -> index in objs
    static constexpr float kBucket = 240.f;
    long long gx0 = 0;                      // the bucket of cells[0]
    std::vector<std::vector<int>> cells;    // objects by bucket of their x span
    std::vector<int> wide;                  // spans too wide for the grid (a floor the length of the level)
    float minX = 0.f, maxX = 0.f;
    size_t hidden = 0;                      // rows read but not kept (types 7 and 20)

    void index() {
        byUid.clear();
        cells.clear();
        wide.clear();
        if (objs.empty()) return;
        minX = objs[0].cx; maxX = objs[0].cx;
        for (const Obj& o : objs) {
            minX = std::min(minX, o.cx - o.w / 2);
            maxX = std::max(maxX, o.cx + o.w / 2);
        }
        gx0 = (long long)std::floor(minX / kBucket);
        const long long g1 = (long long)std::floor(maxX / kBucket);
        cells.assign((size_t)(g1 - gx0 + 1), {});
        for (int i = 0; i < (int)objs.size(); ++i) {
            const Obj& o = objs[(size_t)i];
            if (o.uid >= 0) byUid[o.uid] = i;
            const long long a = (long long)std::floor((o.cx - o.w / 2) / kBucket);
            const long long b = (long long)std::floor((o.cx + o.w / 2) / kBucket);
            if (b - a > 40) { wide.push_back(i); continue; }
            for (long long g = a; g <= b; ++g) cells[(size_t)(g - gx0)].push_back(i);
        }
    }

    // fn(index) once per object whose x span may overlap [x0, x1]. (A caller still tests y; an object listed in two
    // buckets is called once.)
    mutable std::vector<unsigned> stamp;
    mutable unsigned curStamp = 0;
    template <class F>
    void query(float x0, float x1, F&& fn) const {
        if (objs.empty()) return;
        if (stamp.size() != objs.size()) stamp.assign(objs.size(), 0u);
        if (++curStamp == 0) { std::fill(stamp.begin(), stamp.end(), 0u); curStamp = 1; }
        const long long a = std::max<long long>(gx0, (long long)std::floor(x0 / kBucket));
        const long long b = std::min<long long>(gx0 + (long long)cells.size() - 1, (long long)std::floor(x1 / kBucket));
        for (long long g = a; g <= b; ++g)
            for (int i : cells[(size_t)(g - gx0)])
                if (stamp[(size_t)i] != curStamp) { stamp[(size_t)i] = curStamp; fn(i); }
        for (int i : wide) {
            const Obj& o = objs[(size_t)i];
            if (o.cx + o.w / 2 < x0 || o.cx - o.w / 2 > x1) continue;
            if (stamp[(size_t)i] != curStamp) { stamp[(size_t)i] = curStamp; fn(i); }
        }
    }
};

// objrects.txt (the level as src/solver writes it): a header line, then one row per object. Only the leading columns
// are read, by name. `obb` (may be null) gives the real sides of the objects turned by other than a quarter turn.
inline Level parseObjrects(std::string_view text, const ObbMap* obb) {
    Level L;
    int cId = -1, cType = -1, cCx = -1, cCy = -1, cW = -1, cH = -1, cGr = -1, cUid = -1, cRad = -1, cRot = -1,
        cSy0 = -1, cSy1 = -1, cSdir = -1;
    bool first = true;
    Cells cs;
    forEachLine(text, [&](std::string_view line) {
        cs.split(line);
        if (first) {
            first = false;
            for (int i = 0; i < cs.n; ++i) {
                const std::string_view h = cs.c[i];
                if (h == "id") cId = i;
                else if (h == "type") cType = i;
                else if (h == "cx") cCx = i;
                else if (h == "cy") cCy = i;
                else if (h == "w") cW = i;
                else if (h == "h") cH = i;
                else if (h == "groups") cGr = i;
                else if (h == "uid") cUid = i;
                else if (h == "radius") cRad = i;
                else if (h == "rot") cRot = i;
                else if (h == "sy0") cSy0 = i;
                else if (h == "sy1") cSy1 = i;
                else if (h == "sdir") cSdir = i;
            }
            return;
        }
        if (cType < 0 || cCx < 0 || cCy < 0 || cUid < 0 || cs.n <= cUid) return;
        Obj o;
        o.type = (int)inum(cs.at(cType), -1);
        if (hiddenType(o.type)) { ++L.hidden; return; }
        o.id = (int)inum(cs.at(cId));
        o.cx = num(cs.at(cCx));
        o.cy = num(cs.at(cCy));
        o.w = num(cs.at(cW), 30.f);
        o.h = num(cs.at(cH), 30.f);
        o.uid = (int)inum(cs.at(cUid), -1);
        o.radius = num(cs.at(cRad));
        o.rot = num(cs.at(cRot));
        o.sy0 = num(cs.at(cSy0));
        o.sy1 = num(cs.at(cSy1));
        o.sdir = (int)inum(cs.at(cSdir));
        o.grouped = cGr >= 0 && inum(cs.at(cGr)) > 0;
        float sides[2];
        const float* ob = nullptr;
        if (obb) {
            const auto it = obb->find(o.uid);
            if (it != obb->end()) { sides[0] = it->second.first; sides[1] = it->second.second; ob = sides; }
        }
        setNominal(o, ob);
        L.objs.push_back(o);
    });
    L.index();
    return L;
}

// ------------------------------------------------------------------ moving objects

// dp_groups.txt: `init,uid,on` lines, then `tick,uid,cx,cy,w,h,on,rot` rows, one per change, and an `end,tick`
// trailer. An object holds its last row until the next -- the rule the model reads the file by.
struct MoverRow { int t = 0; float cx = 0.f, cy = 0.f, w = 0.f, h = 0.f, rot = 0.f; unsigned char on = 1; };
struct Mover {
    int obj = -1;                           // index into Level::objs
    bool initOn = true;
    std::vector<MoverRow> rows;
    // Where it can ever be (its load pose and every row, grown for a turn), so a frame skips the ones that cannot be on
    // screen without a search through their rows: a heavy level has tens of thousands and the panel asks every frame.
    float bx0 = 0.f, bx1 = 0.f, by0 = 0.f, by1 = 0.f;
};
struct Movers {
    std::vector<Mover> m;
    long long cover = -1;                   // the last tick the recording speaks for (last row or its trailer)
    long long rowsRead = 0;
};

// Rows past tMax are skipped (the panel plays no further than the path it shows); rows for an object the level lacks
// are dropped.
inline Movers parseGroups(std::string_view text, const Level& L, long long tMax) {
    Movers out;
    std::unordered_map<int, size_t> slot;           // uid -> index in out.m
    std::unordered_map<int, bool> initOn;
    long long last = -1, end = -1;
    Cells cs;
    forEachLine(text, [&](std::string_view line) {
        if (line.empty()) return;
        cs.split(line);
        const std::string_view h = cs.c[0];
        if (h == "init") {
            if (cs.n >= 3) initOn[(int)inum(cs.c[1])] = (cs.c[2] != "0");
            return;
        }
        if (h == "end") {
            if (cs.n >= 2) end = std::max(end, inum(cs.c[1], -1));
            return;
        }
        if (cs.n < 8 || h.empty() || !(std::isdigit((unsigned char)h[0]) || h[0] == '-')) return;
        const long long t = inum(h);
        last = std::max(last, t);
        if (t > tMax) return;
        const int uid = (int)inum(cs.c[1], -1);
        const auto bu = L.byUid.find(uid);
        if (bu == L.byUid.end()) return;
        auto it = slot.find(uid);
        if (it == slot.end()) {
            it = slot.emplace(uid, out.m.size()).first;
            Mover mv;
            mv.obj = bu->second;
            out.m.push_back(std::move(mv));
        }
        MoverRow r;
        r.t = (int)t;
        r.cx = num(cs.c[2]); r.cy = num(cs.c[3]); r.w = num(cs.c[4]); r.h = num(cs.c[5]);
        r.on = (unsigned char)(inum(cs.c[6]) != 0);
        r.rot = num(cs.c[7]);
        out.m[it->second].rows.push_back(r);
        ++out.rowsRead;
    });
    for (Mover& mv : out.m) {
        const int uid = L.objs[(size_t)mv.obj].uid;
        const auto io = initOn.find(uid);
        mv.initOn = (io == initOn.end()) ? true : io->second;
        std::stable_sort(mv.rows.begin(), mv.rows.end(),
                         [](const MoverRow& a, const MoverRow& b) { return a.t < b.t; });
        const Obj& o = L.objs[(size_t)mv.obj];
        auto grow = [&](bool first, float cx, float cy, float w, float h) {
            const float hd = 0.71f * std::max(w, h) + 1.f;     // half the diagonal of the biggest box it can turn into
            if (first) { mv.bx0 = cx - hd; mv.bx1 = cx + hd; mv.by0 = cy - hd; mv.by1 = cy + hd; return; }
            mv.bx0 = std::min(mv.bx0, cx - hd); mv.bx1 = std::max(mv.bx1, cx + hd);
            mv.by0 = std::min(mv.by0, cy - hd); mv.by1 = std::max(mv.by1, cy + hd);
        };
        grow(true, o.cx, o.cy, o.w, o.h);
        for (const MoverRow& r : mv.rows) grow(false, r.cx, r.cy, r.w, r.h);
    }
    out.cover = std::max(last, end);
    return out;
}

struct Pose { float cx = 0.f, cy = 0.f, w = 0.f, h = 0.f, rot = 0.f; bool on = true; };

inline Pose staticPose(const Obj& o) { return Pose{o.cx, o.cy, o.w, o.h, o.rot, true}; }

// Where a recorded object stands at tick t: its last row at or before t, or its load position (and initial switch)
// before the first.
inline Pose moverPose(const Obj& o, const Mover& m, long long t) {
    const auto& r = m.rows;
    size_t lo = 0, hi = r.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (r[mid].t <= t) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return Pose{o.cx, o.cy, o.w, o.h, o.rot, m.initOn};
    const MoverRow& q = r[lo - 1];
    return Pose{q.cx, q.cy, q.w, q.h, q.rot, q.on != 0};
}

// ------------------------------------------------------------------ GD's path

// One tick of GD's own state (the deepest attempt's AnchorRow, the part a picture needs).
struct Frame {
    float x = 0.f, y = 0.f, vy = 0.f;
    float x2 = 0.f, y2 = 0.f;               // the second body when dual
    signed char mode = 0;                   // 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing
    signed char flip = 0, mini = 0, dual = 0, og = 0;
    signed char gframe = 0;                 // rotated gameplay: 0 +x, 1 -y, 2 -x, 3 +y
    bool valid = false;
    bool model = false;                     // the model's plan (a row of the DP's walk), not GD's own state
};

struct Track {
    std::vector<Frame> f;                   // indexed by tick
    long long first = -1, last = -1;        // the first and last valid tick
    long long serial = -2;                  // which attempt (anchors::g_deepestSerial) it is
    long long modelFrom = -1;               // the first tick that is the model's plan (-1: the whole path is GD's)
    void finish() {
        first = last = -1;
        for (size_t i = 0; i < f.size(); ++i)
            if (f[i].valid) { if (first < 0) first = (long long)i; last = (long long)i; }
    }
    bool has(long long t) const { return t >= 0 && (size_t)t < f.size() && f[(size_t)t].valid; }
    const Frame* at(long long t) const { return has(t) ? &f[(size_t)t] : nullptr; }
};

// ------------------------------------------------------------------ the model's plan

// The DP's walk of a plan, `<out>.trace.csv` (dp/src/dp/cli.hpp, "tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act,flip,
// frame,rotspent,rotchan,rotrev,dead,key"): what the model believes the plan does from its anchor on. It is not GD's state --
// the model may be wrong -- and the panel shows it as the model's: the frames are marked `model`. The columns are found by
// name, so a column added at the end does not move them. The walk runs on through its own kill (`dead` = 1 on the tick it
// fires); the rows after it are corpses, and are left out. A last line the solver is still writing is left out too.
inline Track parseModelTrace(std::string_view text) {
    Track tr;
    const size_t nl = text.rfind('\n');
    if (nl == std::string_view::npos) return tr;
    text = text.substr(0, nl + 1);
    int cT = -1, cX = -1, cY = -1, cVy = -1, cMode = -1, cGr = -1, cDual = -1, cY2 = -1, cFlip = -1, cFrame = -1, cDead = -1;
    bool head = true, dead = false;
    Cells cs;
    forEachLine(text, [&](std::string_view line) {
        if (line.empty() || dead) return;
        cs.split(line);
        if (head) {
            head = false;
            for (int i = 0; i < cs.n; ++i) {
                const std::string_view h = cs.c[i];
                if (h == "tick") cT = i; else if (h == "x") cX = i; else if (h == "y") cY = i;
                else if (h == "vy") cVy = i; else if (h == "mode") cMode = i; else if (h == "grounded") cGr = i;
                else if (h == "dual") cDual = i; else if (h == "y2") cY2 = i; else if (h == "flip") cFlip = i;
                else if (h == "frame") cFrame = i; else if (h == "dead") cDead = i;
            }
            return;
        }
        const int need = std::max({cT, cX, cY, cVy, cMode, cGr, cDual, cY2, cFlip, cFrame, cDead});
        if (cT < 0 || cX < 0 || cY < 0 || cs.n <= need) return;
        const long long t = inum(cs.c[cT], -1);
        if (t < 0 || t > 4000000) return;
        if ((size_t)t >= tr.f.size()) tr.f.resize((size_t)t + 1);
        Frame& f = tr.f[(size_t)t];
        f.x = num(cs.c[cX]); f.y = num(cs.c[cY]);
        f.vy = cVy >= 0 ? num(cs.c[cVy]) : 0.f;
        f.x2 = f.x;                                  // the second body runs beside the first (a two-player level's does not)
        f.y2 = cY2 >= 0 ? num(cs.c[cY2]) : 0.f;
        f.mode = cMode >= 0 ? (signed char)inum(cs.c[cMode]) : (signed char)0;
        f.dual = cDual >= 0 ? (signed char)(inum(cs.c[cDual]) != 0) : (signed char)0;
        f.flip = cFlip >= 0 ? (signed char)(inum(cs.c[cFlip]) != 0) : (signed char)0;
        f.og = cGr >= 0 ? (signed char)(inum(cs.c[cGr]) != 0) : (signed char)0;
        f.gframe = cFrame >= 0 ? (signed char)(inum(cs.c[cFrame]) & 3) : (signed char)0;
        f.valid = true;
        f.model = true;
        if (cDead >= 0 && inum(cs.c[cDead]) != 0) dead = true;
    });
    tr.finish();
    tr.modelFrom = tr.first;
    return tr;
}

// One plan of the ring, kept compact (a plan is a few thousand ticks long and starts a long way into the level): the model's
// frames from tick t0 on. The panel draws several at once, the deepest as the one it follows (see drawGhosts).
struct Ghost {
    long long t0 = 0;
    std::vector<Frame> f;
    long long first() const { return t0; }
    long long last() const { return t0 + (long long)f.size() - 1; }
    const Frame* at(long long t) const {
        if (t < t0 || t > last()) return nullptr;
        const Frame& r = f[(size_t)(t - t0)];
        return r.valid ? &r : nullptr;
    }
    float endX() const { return f.empty() ? 0.f : f.back().x; }
};
inline std::shared_ptr<const Ghost> ghostOf(const Track& model) {
    auto g = std::make_shared<Ghost>();
    if (model.first < 0) return g;
    g->t0 = model.first;
    g->f.assign(model.f.begin() + model.first, model.f.begin() + model.last + 1);
    return g;
}

// GD's own path up to where the plan's walk begins (its anchor), then the model's plan: the path as planned. `gd` may be
// null or stop short of the anchor (the frames between are then missing, and the path breaks there).
inline Track spliceGhost(const Track* gd, const Ghost& g) {
    Track out;
    if (g.f.empty()) {
        if (gd) out = *gd;
        return out;
    }
    out.f.resize((size_t)g.last() + 1);
    if (gd) {
        const size_t n = std::min(gd->f.size(), (size_t)g.first());
        for (size_t i = 0; i < n; ++i) out.f[i] = gd->f[i];
    }
    for (long long i = g.first(); i <= g.last(); ++i) out.f[(size_t)i] = g.f[(size_t)(i - g.first())];
    out.modelFrom = g.first();
    out.finish();
    return out;
}
inline Track spliceModel(const Track* gd, const Track& model) {
    if (model.first < 0) return spliceGhost(gd, Ghost{});
    return spliceGhost(gd, *ghostOf(model));
}
// The stretch of the path that plays: its last `span` ticks, ending at the frontier (the whole path when span <= 0).
inline void stretchOf(const Track& tr, long long span, long long& a, long long& z) {
    z = tr.last;
    a = tr.first;
    if (span > 0 && z - a > span) a = z - span;
}

// The playback clock. 240 ticks to the game's second, scaled by `speed`; it runs from a to z, stands at z for 1.5 s, and
// starts over at a (returning true on the frame it does, so the camera can snap).
struct Clock {
    double tick = 0.0;
    double hold = 0.0;                      // seconds left standing at the end
    bool advance(double dt, double speed, long long a, long long z) {
        if (hold > 0.0) {
            hold -= dt;
            if (hold > 0.0) return false;
            hold = 0.0;
            tick = (double)a;
            return true;
        }
        tick += dt * 240.0 * speed;
        if (tick < (double)a) tick = (double)a;
        if (tick >= (double)z) { tick = (double)z; hold = 1.5; }
        return false;
    }
};

// A point of the path where the model was wrong and GD's state was taken instead (a fixup of the run).
struct Mark { long long t = 0; float x = 0.f, y = 0.f; bool kill = false; };

// The player's axes in each gameplay frame, measured on a rotating level's reference run: `f` is the way it runs, `l`
// the way its vy points. 0 +x/+y, 1 -y/+x, 2 -x/+y, 3 +y/+x.
struct Axes { float f[2], l[2]; };
inline const Axes& axesOf(int frame) {
    static const Axes A[4] = {{{1, 0}, {0, 1}}, {{0, -1}, {1, 0}}, {{-1, 0}, {0, 1}}, {{0, 1}, {1, 0}}};
    return A[(frame >= 0 && frame < 4) ? frame : 0];
}

// Half the hitbox, in world units.
inline float halfOf(int mode, int mini) {
    if (mode == 4) return mini ? 2.f : 5.f;
    if (mode == 6) return mini ? 8.1f : 13.5f;
    return mini ? 9.f : 15.f;
}

// ------------------------------------------------------------------ the primitives

struct Box { float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f; };

struct Prim {
    enum Kind : unsigned char { Poly, Seg, Dot };
    Kind kind = Poly;
    int n = 0;                              // points (Poly: >= 3; Seg: 2; Dot: 1)
    size_t off = 0;                         // first coordinate in Scene::pts
    Color fill, line;
    float lw = 0.f;                         // Poly: border width; Seg: half width; Dot: radius
};

// Everything one frame draws, clipped to the panel's box so a CCDrawNode (which has no scissor) never paints outside
// it. Polygons are convex by construction (a CCDrawNode fills a polygon as a fan), and clipping a convex polygon with
// Sutherland-Hodgman keeps it convex.
struct Scene {
    Box clip;
    std::vector<float> pts;
    std::vector<Prim> prims;
    std::vector<float> tmpA, tmpB;

    void reset(const Box& b) { clip = b; pts.clear(); prims.clear(); }
    bool inside(float x, float y) const { return x >= clip.x0 && x <= clip.x1 && y >= clip.y0 && y <= clip.y1; }

    // One Sutherland-Hodgman pass against the half-plane `side * (coord - edge) >= 0` on axis `ax` (0 = x, 1 = y).
    static void clipEdge(const std::vector<float>& in, std::vector<float>& out, int ax, float edge, float side) {
        out.clear();
        const size_t n = in.size() / 2;
        if (!n) return;
        auto dist = [&](size_t i) { return side * (in[2 * i + (size_t)ax] - edge); };
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const float di = dist(i), dj = dist(j);
            const bool ii = di >= 0.f, jj = dj >= 0.f;
            if (ii) { out.push_back(in[2 * i]); out.push_back(in[2 * i + 1]); }
            if (ii != jj) {
                const float k = di / (di - dj);
                out.push_back(in[2 * i] + (in[2 * j] - in[2 * i]) * k);
                out.push_back(in[2 * i + 1] + (in[2 * j + 1] - in[2 * i + 1]) * k);
            }
        }
    }

    // A convex polygon. With a border, the border is dropped when the clip cut the shape (it would trace the panel's
    // edge). Returns whether anything was emitted.
    bool poly(const float* xy, int n, Color fill, float bw = 0.f, Color border = Color{}) {
        if (n < 3) return false;
        bool allIn = true;
        float mnx = xy[0], mxx = xy[0], mny = xy[1], mxy = xy[1];
        for (int i = 0; i < n; ++i) {
            mnx = std::min(mnx, xy[2 * i]); mxx = std::max(mxx, xy[2 * i]);
            mny = std::min(mny, xy[2 * i + 1]); mxy = std::max(mxy, xy[2 * i + 1]);
            if (!inside(xy[2 * i], xy[2 * i + 1])) allIn = false;
        }
        if (mxx < clip.x0 || mnx > clip.x1 || mxy < clip.y0 || mny > clip.y1) return false;
        Prim p;
        p.kind = Prim::Poly;
        p.fill = fill; p.line = border; p.lw = bw;
        p.off = pts.size();
        if (allIn) {
            pts.insert(pts.end(), xy, xy + 2 * n);
            p.n = n;
        } else {
            tmpA.assign(xy, xy + 2 * n);
            clipEdge(tmpA, tmpB, 0, clip.x0, 1.f);  tmpA.swap(tmpB);
            clipEdge(tmpA, tmpB, 0, clip.x1, -1.f); tmpA.swap(tmpB);
            clipEdge(tmpA, tmpB, 1, clip.y0, 1.f);  tmpA.swap(tmpB);
            clipEdge(tmpA, tmpB, 1, clip.y1, -1.f); tmpA.swap(tmpB);
            if (tmpA.size() < 6) return false;
            pts.insert(pts.end(), tmpA.begin(), tmpA.end());
            p.n = (int)(tmpA.size() / 2);
            p.lw = 0.f;
        }
        prims.push_back(p);
        return true;
    }

    bool rect(float x0, float y0, float x1, float y1, Color fill, float bw = 0.f, Color border = Color{}) {
        const float v[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        return poly(v, 4, fill, bw, border);
    }

    // A segment of half-width `hw`, clipped to the box (Liang-Barsky).
    bool seg(float ax, float ay, float bx, float by, float hw, Color c) {
        float t0 = 0.f, t1 = 1.f;
        const float dx = bx - ax, dy = by - ay;
        const float p[4] = {-dx, dx, -dy, dy};
        const float q[4] = {ax - clip.x0, clip.x1 - ax, ay - clip.y0, clip.y1 - ay};
        for (int i = 0; i < 4; ++i) {
            if (p[i] == 0.f) { if (q[i] < 0.f) return false; continue; }
            const float r = q[i] / p[i];
            if (p[i] < 0.f) { if (r > t1) return false; t0 = std::max(t0, r); }
            else            { if (r < t0) return false; t1 = std::min(t1, r); }
        }
        Prim s;
        s.kind = Prim::Seg; s.n = 2; s.line = c; s.lw = hw; s.off = pts.size();
        pts.push_back(ax + dx * t0); pts.push_back(ay + dy * t0);
        pts.push_back(ax + dx * t1); pts.push_back(ay + dy * t1);
        prims.push_back(s);
        return true;
    }

    bool dot(float x, float y, float r, Color c) {
        if (!inside(x, y)) return false;
        Prim d;
        d.kind = Prim::Dot; d.n = 1; d.line = c; d.lw = r; d.off = pts.size();
        pts.push_back(x); pts.push_back(y);
        prims.push_back(d);
        return true;
    }

    // A regular polygon standing for a circle; filled and/or bordered.
    bool circle(float x, float y, float r, Color fill, float bw = 0.f, Color border = Color{}, int sides = 18) {
        float v[2 * 40];
        sides = std::max(6, std::min(sides, 40));
        for (int i = 0; i < sides; ++i) {
            const float a = 2.f * kPi * (float)i / (float)sides;
            v[2 * i] = x + std::cos(a) * r;
            v[2 * i + 1] = y + std::sin(a) * r;
        }
        return poly(v, sides, fill, bw, border);
    }
};

// ------------------------------------------------------------------ the view

// world -> panel. (px, py) is where the camera's centre sits in the panel; scale is panel units per world unit.
struct View {
    float cx = 0.f, cy = 0.f, scale = 1.f, px = 0.f, py = 0.f;
    float X(float x) const { return px + (x - cx) * scale; }
    float Y(float y) const { return py + (y - cy) * scale; }
    float worldX0(const Box& b) const { return cx + (b.x0 - px) / scale; }
    float worldX1(const Box& b) const { return cx + (b.x1 - px) / scale; }
    float worldY0(const Box& b) const { return cy + (b.y0 - py) / scale; }
    float worldY1(const Box& b) const { return cy + (b.y1 - py) / scale; }
};

// ------------------------------------------------------------------ the palette (the divergence view's)

namespace pal {
inline Color solid(float a = 1.f) { return hex(0x96a0af, a); }
inline Color hazard(float a = 1.f) { return hex(0xe14646, a); }
inline Color portal(float a = 1.f) { return hex(0x509af5, a); }
inline Color mover(float a = 1.f) { return hex(0x78c882, a); }
inline Color other(float a = 1.f) { return hex(0x777788, a); }
inline Color gd(float a = 1.f) { return hex(0xffffff, a); }
inline Color mark(float a = 1.f) { return hex(0xffa63d, a); }
inline Color killMark(float a = 1.f) { return hex(0xff5b4b, a); }
inline Color model(float a = 1.f) { return hex(0x5ab4ff, a); }       // the model's plan, as against GD's own path
}  // namespace pal

inline bool isPortalType(int t) {
    switch (t) {
        case 3: case 4: case 5: case 6: case 14: case 15: case 16: case 17: case 18: case 19: case 23: case 24:
        case 26: case 27: case 28: case 33: case 41: case 42: return true;
        default: return false;
    }
}
inline bool padColor(int t, Color& c) {
    switch (t) {
        case 8: c = hex(0xf2d03b); return true;
        case 9: c = hex(0xee82c8); return true;
        case 10: c = hex(0x3fc9ee); return true;
        case 34: c = hex(0xe8493f); return true;
        case 44: c = hex(0xb07cf0); return true;
        default: return false;
    }
}
inline bool ringColor(int t, Color& c) {
    switch (t) {
        case 11: c = hex(0xf2d03b); return true;
        case 12: c = hex(0xee82c8); return true;
        case 13: c = hex(0x3fc9ee); return true;
        case 29: c = hex(0x5ad06a); return true;
        case 32: c = hex(0x303030); return true;       // the viewer's #101010 is invisible on a dark panel
        case 35: c = hex(0xe8493f); return true;
        case 36: c = hex(0xbbbbbb); return true;
        case 37: case 38: c = hex(0xc88af0); return true;
        case 43: c = hex(0xb07cf0); return true;
        case 46: c = hex(0x5fe0d0); return true;
        default: return false;
    }
}

// The corners of a w x h box turned clockwise by `deg` about (cx, cy), in the order the y-up plane draws them. (The
// divergence view's rotRect in a y-flipped canvas: clockwise on screen is clockwise here too.)
inline void rotRectCorners(float cx, float cy, float w, float h, float deg, float out[8]) {
    const float a = deg * kPi / 180.f, c = std::cos(a), s = std::sin(a), hw = w / 2, hh = h / 2;
    const float lx[4] = {-1, 1, 1, -1}, ly[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        // clockwise by a on a y-up plane: (x, y) -> (x cos a + y sin a, -x sin a + y cos a)
        out[2 * i] = cx + lx[i] * hw * c + ly[i] * hh * s;
        out[2 * i + 1] = cy - lx[i] * hw * s + ly[i] * hh * c;
    }
}

// One object at a pose.
inline void drawObject(Scene& s, const View& v, const Obj& o, const Pose& p, bool isMover) {
    const int t = o.type;
    const float scale = v.scale;
    const float x0 = v.X(p.cx - p.w / 2), x1 = v.X(p.cx + p.w / 2), y0 = v.Y(p.cy - p.h / 2), y1 = v.Y(p.cy + p.h / 2);
    if (x1 < s.clip.x0 || x0 > s.clip.x1 || y1 < s.clip.y0 || y0 > s.clip.y1) return;
    const float thin = 0.35f;
    if (!p.on) {                                  // switched off: only an outline, faint
        const Color c = hex(0x55556a, 0.9f);
        s.seg(x0, y0, x1, y0, thin * 0.5f, c); s.seg(x1, y0, x1, y1, thin * 0.5f, c);
        s.seg(x1, y1, x0, y1, thin * 0.5f, c); s.seg(x0, y1, x0, y0, thin * 0.5f, c);
        return;
    }
    Color col = isMover ? pal::mover() : pal::other();
    Color pc;
    if (!isMover) {
        if (t == 0 || t == 25) col = pal::solid();
        else if (t == 2) col = pal::hazard();
        else if (isPortalType(t)) col = pal::portal();
        else if (padColor(t, pc) || ringColor(t, pc)) col = pc;
    } else if (t == 2) {
        col = pal::hazard();
    }
    if (t == 25) {                                // slope: the surface line GD sampled, the solid side filled
        const float yl = v.Y(o.sy0), yr = v.Y(o.sy1);
        if (o.sdir == 0 || o.sdir == 2) {
            const float q[8] = {x0, yl, x1, yr, x1, y0, x0, y0};
            s.poly(q, 4, withAlpha(col, 0.55f));
        } else if (o.sdir == 1 || o.sdir == 3) {
            const float q[8] = {x0, yl, x1, yr, x1, y1, x0, y1};
            s.poly(q, 4, withAlpha(col, 0.55f));
        }
        s.seg(x0, yl, x1, yr, thin, col);
        return;
    }
    Color padc;
    if (padColor(t, padc)) {
        s.rect(x0, y0, x1, std::max(y1, y0 + 1.0f), withAlpha(padc, 0.85f));
        return;
    }
    Color ringc;
    if (ringColor(t, ringc) || (t == 2 && o.radius > 0.f)) {      // rings and saws are round
        const float rad = std::max(1.0f, (o.radius > 0.f ? o.radius : p.w / 2) * scale);
        if (t == 2) s.circle(v.X(p.cx), v.Y(p.cy), rad, withAlpha(col, 0.7f));
        else        s.circle(v.X(p.cx), v.Y(p.cy), rad, Color{}, thin * 1.4f, col);
        return;
    }
    if ((t == 2 || t == 0) && isTurned(p.rot) && o.nw > 0.f && o.nh > 0.f) {
        float q[8];
        rotRectCorners(p.cx, p.cy, o.nw, o.nh, p.rot, q);
        for (int i = 0; i < 4; ++i) { q[2 * i] = v.X(q[2 * i]); q[2 * i + 1] = v.Y(q[2 * i + 1]); }
        s.poly(q, 4, withAlpha(col, t == 0 ? 0.8f : 0.9f));
        return;
    }
    if (t == 2 || t == 0) {
        s.rect(x0, y0, std::max(x1, x0 + 0.6f), std::max(y1, y0 + 0.6f), withAlpha(col, t == 0 ? 0.8f : 0.9f));
        return;
    }
    if (isPortalType(t)) {
        s.rect(x0, y0, x1, y1, withAlpha(col, 0.35f), thin, col);
        return;
    }
    s.rect(v.X(p.cx) - 0.7f, v.Y(p.cy) - 0.7f, v.X(p.cx) + 0.7f, v.Y(p.cy) + 0.7f, col);
}

// A plain shape per mode, enough to tell them apart (not the game's art). Local axes: +x ahead, +y away from the
// floor (flipped when gravity is upside down); both turn with the gameplay rotation. h is the half size in panel
// units; (px, py) the centre. `fwd` is how far the player ran along its forward axis this tick (for the ship's and
// the wave's tilt), `runDist` how far it has run (the ball's spoke).
inline void drawSilhouette(Scene& s, const Frame& f, float px, float py, float h, float fwd, float runDist,
                           float halfWorld, Color col, bool bold) {
    const int mode = f.mode;
    const float flip = f.flip ? -1.f : 1.f;
    const Axes& ax = axesOf(f.gframe);
    if (!(fwd > 0.05f)) fwd = 1.6f;
    const float tilt = (mode == 1 || mode == 4) ? std::atan2(f.vy * 0.225f, fwd) : 0.f;   // 0.225 = GD's step per unit of vy
    const float c = std::cos(tilt), sn = std::sin(tilt);
    auto P = [&](float lx, float ly, float& ox, float& oy) {
        ly *= flip;
        const float rx = lx * c - ly * sn, ry = lx * sn + ly * c;
        ox = px + (rx * ax.f[0] + ry * ax.l[0]) * h;
        oy = py + (rx * ax.f[1] + ry * ax.l[1]) * h;
    };
    // col.a is how strongly the whole figure is drawn (1 for the path's own player, less for a ghost's)
    const float fillA = (bold ? 0.35f : 0.25f) * col.a;
    const float lw = bold ? 0.85f : 0.65f;
    auto poly = [&](std::initializer_list<float> pts, bool border) {
        float v[24];
        int n = 0;
        auto it = pts.begin();
        while (it != pts.end() && n < 12) {
            const float lx = *it++, ly = *it++;
            P(lx, ly, v[2 * n], v[2 * n + 1]);
            ++n;
        }
        s.poly(v, n, withAlpha(col, fillA), border ? lw : 0.f, col);
    };
    auto line = [&](float x0, float y0, float x1, float y1, float w) {
        float a[2], b[2];
        P(x0, y0, a[0], a[1]); P(x1, y1, b[0], b[1]);
        s.seg(a[0], a[1], b[0], b[1], w, col);
    };
    auto arc = [&](float cx, float cy, float r, float a0, float a1) {
        float px0 = 0, py0 = 0;
        for (int k = 0; k <= 10; ++k) {
            const float a = a0 + (a1 - a0) * (float)k / 10.f;
            const float lx = cx + std::cos(a) * r, ly = cy + std::sin(a) * r;
            float qx, qy;
            P(lx, ly, qx, qy);
            if (k) s.seg(px0, py0, qx, qy, lw * 0.5f, col);
            px0 = qx; py0 = qy;
        }
    };
    switch (mode) {
        case 1:                                    // ship: a pointed hull with a dome on top
            poly({-1, -.45f, .55f, -.45f, 1, 0, .55f, .45f, -1, .45f}, true);
            arc(-.05f, .45f, .45f, 0.f, kPi);
            break;
        case 2: {                                  // ball: a circle with a spoke that turns as it rolls
            s.circle(px, py, h, withAlpha(col, fillA), lw, col);
            const float a = -runDist / std::max(1.f, halfWorld);
            s.seg(px, py, px + std::cos(a) * h, py + std::sin(a) * h, lw * 0.5f, col);
            break;
        }
        case 3:                                    // ufo: a saucer with a dome
            poly({-1, -.1f, -.55f, -.55f, .55f, -.55f, 1, -.1f, .55f, .1f, -.55f, .1f}, true);
            arc(0.f, .1f, .5f, 0.f, kPi);
            break;
        case 4:                                    // wave: an arrowhead (two triangles, then its outline)
            poly({-1, -.8f, 1, 0, -.45f, 0}, false);
            poly({1, 0, -1, .8f, -.45f, 0}, false);
            line(-1, -.8f, 1, 0, lw * 0.5f); line(1, 0, -1, .8f, lw * 0.5f);
            line(-1, .8f, -.45f, 0, lw * 0.5f); line(-.45f, 0, -1, -.8f, lw * 0.5f);
            break;
        case 5:                                    // robot: a body with a visor and two feet
            poly({-.8f, -.55f, .8f, -.55f, .8f, .9f, -.8f, .9f}, true);
            poly({-.8f, -1, -.25f, -1, -.25f, -.55f, -.8f, -.55f}, false);
            poly({.25f, -1, .8f, -1, .8f, -.55f, .25f, -.55f}, false);
            poly({.05f, .25f, .8f, .25f, .8f, .6f, .05f, .6f}, false);
            break;
        case 6:                                    // spider: a small body and four legs
            poly({-.5f, -.4f, .5f, -.4f, .5f, .5f, -.5f, .5f}, true);
            line(-.5f, -.2f, -1, -.9f, lw * 0.5f); line(.5f, -.2f, 1, -.9f, lw * 0.5f);
            line(-.5f, .3f, -1, -.1f, lw * 0.5f); line(.5f, .3f, 1, -.1f, lw * 0.5f);
            break;
        case 7: {                                  // swing: a round body with a wing bar across the top
            s.circle(px, py, h * .55f, withAlpha(col, fillA), lw, col);
            line(-1, .7f, 1, .7f, lw * 0.9f);
            break;
        }
        default:                                   // cube (0) and anything unknown: a square with a face
            poly({-.85f, -.85f, .85f, -.85f, .85f, .85f, -.85f, .85f}, true);
            poly({.15f, .1f, .55f, .1f, .55f, .5f, .15f, .5f}, false);
    }
}

// One frame of GD's player: its hitbox, the silhouette, the ground bar and the vy line. prev/next are the
// neighbouring ticks (may be null) for the tilt and the roll.
inline void drawPlayer(Scene& s, const View& v, const Frame& f, const Frame* prev, const Frame* next, Color col,
                       bool bold) {
    const float halfW = halfOf(f.mode, f.mini);
    const Axes& ax = axesOf(f.gframe);
    const float px = v.X(f.x), py = v.Y(f.y);
    const float half = std::max(1.0f, halfW * v.scale);
    const float h = std::max(half, 7.f * v.scale);
    // the hitbox the physics reads, thin
    {
        const Color c = withAlpha(col, 0.7f * col.a);
        const float t = 0.25f;
        s.seg(px - half, py - half, px + half, py - half, t, c); s.seg(px + half, py - half, px + half, py + half, t, c);
        s.seg(px + half, py + half, px - half, py + half, t, c); s.seg(px - half, py + half, px - half, py - half, t, c);
    }
    float fwd = 1.6f;
    auto along = [&](const Frame& a, const Frame& b) {
        return std::fabs((b.x - a.x) * ax.f[0] + (b.y - a.y) * ax.f[1]);
    };
    if (next && next->valid) fwd = along(f, *next);
    else if (prev && prev->valid) fwd = along(*prev, f);
    drawSilhouette(s, f, px, py, h, fwd, f.x * ax.f[0] + f.y * ax.f[1], halfW, col, bold);
    // on the ground: a bar along the hitbox edge that faces the floor
    const float floorSide = f.flip ? 1.f : -1.f;
    if (f.og) {
        const float ex = px + ax.l[0] * floorSide * half, ey = py + ax.l[1] * floorSide * half;
        const float fx = ax.f[0] * half, fy = ax.f[1] * half;
        s.seg(ex - fx, ey - fy, ex + fx, ey + fy, 0.7f, col);
    }
    // vy, along the axis it is measured on
    s.seg(px, py, px + ax.l[0] * f.vy * 2.5f * v.scale, py + ax.l[1] * f.vy * 2.5f * v.scale, 0.3f, col);
    if (f.dual && f.gframe == 0) {                // the partner (for an unrotated run only)
        const float qx = v.X(f.x2 != 0.f ? f.x2 : f.x), qy = v.Y(f.y2);
        const Color c = withAlpha(col, 0.5f * col.a);
        const float t = 0.25f;
        s.seg(qx - half, qy - half, qx + half, qy - half, t, c); s.seg(qx + half, qy - half, qx + half, qy + half, t, c);
        s.seg(qx + half, qy + half, qx - half, qy + half, t, c); s.seg(qx - half, qy + half, qx - half, qy - half, t, c);
    }
}

// The path around tick t: a faint stretch behind, a bright one just behind, and a dim stretch ahead.
inline void drawPath(Scene& s, const View& v, const Track& tr, long long t) {
    auto stroke = [&](long long from, long long to, int step, Color c, Color mc, float hw) {
        if (from < tr.first) from = tr.first;
        if (to > tr.last) to = tr.last;
        const Frame* a = nullptr;
        for (long long i = from; i <= to; i += step) {
            const Frame* b = tr.at(i);
            if (!b) { a = nullptr; continue; }
            if (a) s.seg(v.X(a->x), v.Y(a->y), v.X(b->x), v.Y(b->y), hw, b->model ? mc : c);
            a = b;
        }
    };
    stroke(t - 1500, t - 120, 4, pal::gd(0.22f), pal::model(0.22f), 0.25f);
    stroke(t - 120, t, 1, pal::gd(0.8f), pal::model(0.8f), 0.4f);
    stroke(t, t + 600, 3, hex(0x6fd08c, 0.35f), pal::model(0.35f), 0.25f);
}

// The ring of plans, beside the one the picture follows. Each is its own walk of the plan the solver found at some anchor,
// and they share the game's clock: at tick t a ghost is where its plan has the player at tick t (before its anchor it is
// on GD's path, which the path already draws, so it is not drawn there). A ghost is a faint line over its whole walk, a
// figure of the player where the plan has it now (the shape of its mode, as the path's own player is drawn), and a cross where the walk ends (the model's death, or the end of its
// look-ahead). The newest is the brightest: `i` is its place in the ring, 0 the newest.
inline void drawGhosts(Scene& s, const View& v, const std::vector<std::shared_ptr<const Ghost>>& ring, const Ghost* focus,
                       long long t) {
    const size_t n = ring.size();
    for (size_t i = 0; i < n; ++i) {
        const Ghost* g = ring[i].get();
        if (!g || g == focus || g->f.empty()) continue;
        const float age = n > 1 ? (float)i / (float)(n - 1) : 0.f;
        const float a = 0.55f - 0.35f * age;
        const long long from = g->first(), to = g->last();
        const int step = (to - from) > 2400 ? 6 : 3;
        const Frame* prev = nullptr;
        for (long long k = from; k <= to; k += step) {
            const Frame* b = g->at(k);
            if (!b) { prev = nullptr; continue; }
            if (prev) s.seg(v.X(prev->x), v.Y(prev->y), v.X(b->x), v.Y(b->y), 0.22f, pal::model(a));
            prev = b;
        }
        const Frame& e = g->f.back();
        const float ex = v.X(e.x), ey = v.Y(e.y), c = 1.6f * v.scale + 0.8f;
        s.seg(ex - c, ey - c, ex + c, ey + c, 0.25f, pal::model(a + 0.2f));
        s.seg(ex - c, ey + c, ex + c, ey - c, 0.25f, pal::model(a + 0.2f));
        // where the plan has the player now: its figure, the shape of its mode, as faint as the line (the deepest's is the path's own player)
        if (const Frame* at = g->at(t)) drawPlayer(s, v, *at, g->at(t - 1), g->at(t + 1), pal::model(std::min(1.f, a + 0.15f)), false);
    }
}

// ------------------------------------------------------------------ one whole frame

// What a frame is built from. Everything is borrowed; the caller owns it.
struct FrameIn {
    const Level* level = nullptr;
    const Movers* movers = nullptr;               // the run's recording, or null (the objects then stand where they load)
    const std::vector<int>* moverOf = nullptr;    // object index -> index in movers->m, -1 for a static one
    bool moversOfThisAttempt = false;             // the recording is the one the path's attempt made
    const Track* track = nullptr;
    const std::vector<Mark>* marks = nullptr;
    // the ring of the model's plans, newest first, drawn beside the path (null: none); `focus` is the one the path was built
    // from (the deepest), which is drawn as the path and followed by the camera, not as a ghost
    const std::vector<std::shared_ptr<const Ghost>>* ghosts = nullptr;
    const Ghost* focus = nullptr;
    long long t = 0;                              // the tick on screen
    long long a = 0, z = 0;                       // the stretch the bar spans
    float W = 0.f, H = 0.f;                       // the panel's size
};

struct Camera {
    float y = 0.f;                                // eased toward the player's y
    bool on = false;
};

// The panel for one tick: backdrop, level, moving parts, GD's path, the corrections, the player, the bar. Returns the
// status line. The clock and the camera's easing are the caller's; this is a function of (state, tick).
inline std::string buildFrame(Scene& S, const FrameIn& in, Camera& cam) {
    const float W = in.W, H = in.H;
    S.reset(Box{0.f, 0.f, W, H});
    S.rect(0.f, 0.f, W, H, Color{0.f, 0.f, 0.f, 0.72f});
    {
        const Color fr{1.f, 1.f, 1.f, 0.25f};
        S.seg(0.f, 0.3f, W, 0.3f, 0.3f, fr); S.seg(0.f, H - 0.3f, W, H - 0.3f, 0.3f, fr);
        S.seg(0.3f, 0.f, 0.3f, H, 0.3f, fr);  S.seg(W - 0.3f, 0.f, W - 0.3f, H, 0.3f, fr);
    }
    const Frame* f = in.track ? in.track->at(in.t) : nullptr;
    if (!f && in.track) f = in.track->at(in.track->first);
    if (!f || !in.level) return "replay panel: nothing to show yet";
    const Level& L = *in.level;
    const Track& tr = *in.track;
    const Box clip{0.f, 0.f, W, H};

    // the camera: x on the player (a third of the way in, so the road ahead shows), y eased to it
    const float scale = W / 640.f;
    View v;
    v.scale = scale;
    v.px = W * 0.34f;
    v.py = H * 0.5f;
    v.cx = f->x;
    if (!cam.on) { cam.y = f->y; cam.on = true; }
    cam.y += (f->y - cam.y) * 0.12f;
    {
        const float lim = (H * 0.5f - 22.f) / scale;      // keep the player (and a little floor) on the panel
        cam.y = std::max(f->y - lim, std::min(f->y + lim, cam.y));
    }
    v.cy = cam.y;

    // the level, then the moving parts
    const bool haveMovers = in.movers && in.moverOf && in.moverOf->size() == L.objs.size();
    L.query(v.worldX0(clip) - 40.f, v.worldX1(clip) + 40.f, [&](int i) {
        if (haveMovers && (*in.moverOf)[(size_t)i] >= 0) return;
        const Obj& o = L.objs[(size_t)i];
        drawObject(S, v, o, staticPose(o), false);
    });
    if (haveMovers) {
        const float vx0 = v.worldX0(clip) - 40.f, vx1 = v.worldX1(clip) + 40.f;
        const float vy0 = v.worldY0(clip) - 40.f, vy1 = v.worldY1(clip) + 40.f;
        for (const Mover& m : in.movers->m) {
            if (m.bx1 < vx0 || m.bx0 > vx1 || m.by1 < vy0 || m.by0 > vy1) continue;   // never on screen
            const Obj& o = L.objs[(size_t)m.obj];
            drawObject(S, v, o, moverPose(o, m, in.t), true);
        }
    }

    // GD's path and, ringed as it passes, the points where the model was wrong
    drawPath(S, v, tr, in.t);
    if (in.ghosts) drawGhosts(S, v, *in.ghosts, in.focus, in.t);
    if (const Frame* an = tr.modelFrom >= 0 ? tr.at(tr.modelFrom) : nullptr)       // where the plan begins: the anchor
        S.circle(v.X(an->x), v.Y(an->y), 5.f * scale + 1.f, Color{}, 0.5f, pal::model(0.7f));
    int inStretch = 0;
    if (in.marks)
        for (const Mark& k : *in.marks) {
            if (k.t >= in.a && k.t <= in.z) ++inStretch;
            const long long d = k.t - in.t;
            if (d < -120 || d > 240) continue;
            const float a01 = d >= 0 ? 0.9f : 0.9f * (1.f + (float)d / 120.f);
            S.circle(v.X(k.x), v.Y(k.y), 14.f * scale + 2.f, Color{}, 0.5f, k.kill ? pal::killMark(a01) : pal::mark(a01));
        }
    drawPlayer(S, v, *f, tr.at(in.t - 1), tr.at(in.t + 1), f->model ? pal::model() : pal::gd(), true);

    // the bar along the bottom: where in the stretch, and where the corrections are
    {
        const float by0 = 1.2f, by1 = 4.2f, bx0 = 3.f, bx1 = W - 3.f;
        const float span = (float)std::max<long long>(1, in.z - in.a);
        S.rect(bx0, by0, bx1, by1, Color{1.f, 1.f, 1.f, 0.12f});
        const float px = bx0 + (bx1 - bx0) * (float)(in.t - in.a) / span;
        S.rect(bx0, by0, std::max(bx0 + 0.5f, px), by1, Color{0.43f, 0.82f, 0.55f, 0.55f});
        if (tr.modelFrom > in.a && tr.modelFrom <= in.z)           // the planned part of the stretch
            S.rect(bx0 + (bx1 - bx0) * (float)(tr.modelFrom - in.a) / span, by0, bx1, by1, pal::model(0.3f));
        if (in.marks)
            for (const Mark& k : *in.marks) {
                if (k.t < in.a || k.t > in.z) continue;
                const float mx = bx0 + (bx1 - bx0) * (float)(k.t - in.a) / span;
                S.seg(mx, by0 - 0.6f, mx, by1 + 0.6f, 0.35f, k.kill ? pal::killMark() : pal::mark());
            }
        S.seg(px, by0 - 1.f, px, by1 + 1.f, 0.4f, Color{1.f, 1.f, 1.f, 0.9f});
    }
    // the moving parts: this attempt's own recording, another attempt's (then they are only close), or none
    const char* mv = !in.movers ? "as loaded" : (in.moversOfThisAttempt ? "recorded" : "another attempt's");
    char b[200];
    std::snprintf(b, sizeof(b), "%s  t %lld/%lld  x %.0f  %d correction%s  moving parts: %s",
                  f->model ? "planned path (the model)" : "GD path", in.t, in.z, (double)f->x, inStretch,
                  inStretch == 1 ? "" : "s", mv);
    return b;
}

}  // namespace panelgeom
}  // namespace p1
