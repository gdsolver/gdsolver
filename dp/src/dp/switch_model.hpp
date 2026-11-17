#pragma once
// A SWITCH TABLE: "a press of body B does this to these objects", read from what the game was seen to do (the mod's probe passes, repair.hpp), and the
// geometry it implies for any schedule of presses.
//
// A two-player level can give one body's button an effect on the world (a Touch trigger in toggle mode: every press of the second player flips a set
// of walls, and the first player's presses do nothing). The per-body search plans each body against a ghost of the other, so it cannot ask for such a
// press; and the moving geometry the loop keeps is one attempt's recording, which has the walls where THAT attempt's presses put them. The table is the
// press -> effect relation, kept as data and evaluated, so that no parity is written into the model:
//
//   switch 1                       the file's version
//   body <1|2>                     whose button it is
//   tend <tick>                    the last tick the schedule's geometry is written to
//   r <uid> <cx> <cy> <w> <h> <on> <rot>                    the object at tick 1 with no press
//   b <uid> <tick> <cx> <cy> <w> <h> <on> <rot>             its own timeline with no press (a row where a field changes, after tick 1): objects
//                                                           the level moves by itself keep moving, and a press moves them further
//   c <uid> <1|2> <dt> <dcx> <dcy> <dw> <dh> <drot> <flip>  what a press made of it `dt` ticks after the press, as the difference from the no-press
//                                                           timeline (flip: `on` is the other way round), for the 1st or the 2nd of two presses in
//                                                           a row that each moved it (the first row, dt 0, is the difference the press found)
//
// A press k (counting from 1, in tick order) plays curve 1 when k is odd and curve 2 when it is even -- a toggle; the table says what each did, and the
// k-th press's curve is the (k mod 2)-th. The differences add (a Move is relative: what a curve made from its first row, summed over the presses), onto the
// object's own timeline; `on` is the latest press's curve's flip. The groups the dp reads are the recording's rows, so the geometry for a schedule
// is written as such a file (writeSwitchGroups) and handed to the search as the last --groups layer, which wins over the recordings under it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace dp {

struct SwRow {
    int dt = 0;   // a curve's row: ticks after the press; a timeline row: the tick
    double cx = 0, cy = 0, w = 0, h = 0, rot = 0;   // a curve's row: the differences
    int on = 1;   // a curve's row: the flip (0/1)
};
struct SwObj {
    int uid = -1;
    SwRow rest;
    std::vector<SwRow> base;   // its own timeline after tick 1, by tick
    std::vector<SwRow> c[2];   // curve 1 (odd presses), curve 2 (even presses)
};
struct SwitchTable {
    int body = 2;
    long long tend = 0;
    std::vector<SwObj> objs;
    bool empty() const { return objs.empty(); }
};

inline bool loadSwitchTable(const std::string& path, SwitchTable& t) {
    std::ifstream f(path);
    if (!f) return false;
    t = SwitchTable{};
    std::string ln;
    auto find = [&](int uid) -> SwObj* {
        for (SwObj& o : t.objs)
            if (o.uid == uid) return &o;
        t.objs.push_back(SwObj{});
        t.objs.back().uid = uid;
        return &t.objs.back();
    };
    while (std::getline(f, ln)) {
        if (ln.empty()) continue;
        std::istringstream is(ln);
        std::string tag;
        is >> tag;
        if (tag == "body") is >> t.body;
        else if (tag == "tend") is >> t.tend;
        else if (tag == "r") {
            int uid;
            SwRow r;
            if (is >> uid >> r.cx >> r.cy >> r.w >> r.h >> r.on >> r.rot) find(uid)->rest = r;
        } else if (tag == "b") {
            int uid;
            SwRow r;
            if (is >> uid >> r.dt >> r.cx >> r.cy >> r.w >> r.h >> r.on >> r.rot) find(uid)->base.push_back(r);
        } else if (tag == "c") {
            int uid, k;
            SwRow r;
            if (is >> uid >> k >> r.dt >> r.cx >> r.cy >> r.w >> r.h >> r.rot >> r.on && (k == 1 || k == 2)) find(uid)->c[k - 1].push_back(r);
        }
    }
    for (SwObj& o : t.objs) {
        std::stable_sort(o.base.begin(), o.base.end(), [](const SwRow& a, const SwRow& b) { return a.dt < b.dt; });
        for (auto& c : o.c)
            std::stable_sort(c.begin(), c.end(), [](const SwRow& a, const SwRow& b) { return a.dt < b.dt; });
    }
    return !t.objs.empty();
}

// The row of a curve `dt` ticks after its press: the last row at or before dt (the first row, when dt precedes them all)
inline const SwRow& swCurveAt(const std::vector<SwRow>& c, long long dt) {
    size_t lo = 0, hi = c.size();
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        if (c[m].dt <= dt) lo = m + 1;
        else hi = m;
    }
    return lo == 0 ? c.front() : c[lo - 1];
}

// The object's own state at tick t: its timeline's last row at or before t (the tick-1 row before any)
inline SwRow swBaseAt(const SwObj& o, long long t) {
    SwRow s = o.rest;
    size_t lo = 0, hi = o.base.size();
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        if (o.base[m].dt <= t) lo = m + 1;
        else hi = m;
    }
    if (lo > 0) s = o.base[lo - 1];
    s.dt = 0;
    return s;
}

// The object's state at tick t after the presses `S` (edge ticks, ascending)
inline SwRow switchState(const SwObj& o, const std::vector<long long>& S, long long t) {
    SwRow s = swBaseAt(o, t);
    double dcx = 0, dcy = 0, dw = 0, dh = 0, drot = 0;
    int flip = 0;
    for (size_t k = 0; k < S.size(); ++k) {
        const long long dt = t - S[k];
        if (dt < 0) break;
        const std::vector<SwRow>& c = o.c[k & 1];
        if (c.empty()) continue;
        const SwRow& r = swCurveAt(c, dt);
        dcx += r.cx - c.front().cx;
        dcy += r.cy - c.front().cy;
        dw += r.w - c.front().w;
        dh += r.h - c.front().h;
        drot += r.rot - c.front().rot;
        flip = r.on;
    }
    s.cx += dcx;
    s.cy += dcy;
    s.w += dw;
    s.h += dh;
    s.rot += drot;
    if (flip) s.on = s.on ? 0 : 1;
    return s;
}

// The recording-format rows (`tick,uid,cx,cy,w,h,on,rot`; `init,uid,on`) of every object in the table over ticks 1..tend, one row where a field changes
inline bool writeSwitchGroups(const SwitchTable& tab, const std::vector<long long>& S, const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "tick,uid,cx,cy,w,h,on,rot\n";
    for (const SwObj& o : tab.objs) f << "init," << o.uid << "," << o.rest.on << "\n";
    struct Row { long long t; int uid; SwRow s; };
    std::vector<Row> rows;
    for (const SwObj& o : tab.objs) {
        SwRow last;
        bool have = false;
        for (long long t = 1; t <= tab.tend; ++t) {
            const SwRow s = switchState(o, S, t);
            const bool same = have && std::fabs(s.cx - last.cx) < 5e-4 && std::fabs(s.cy - last.cy) < 5e-4 && std::fabs(s.w - last.w) < 5e-4
                              && std::fabs(s.h - last.h) < 5e-4 && std::fabs(s.rot - last.rot) < 5e-4 && s.on == last.on;
            if (!same) {
                rows.push_back({t, o.uid, s});
                last = s;
                have = true;
            }
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t != b.t ? a.t < b.t : a.uid < b.uid; });
    char b[256];
    for (const Row& r : rows) {
        std::snprintf(b, sizeof b, "%lld,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f\n", r.t, r.uid, r.s.cx, r.s.cy, r.s.w, r.s.h, r.s.on, r.s.rot);
        f << b;
    }
    return true;
}
}  // namespace dp
