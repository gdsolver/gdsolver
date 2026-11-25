#pragma once
#include <random>
// The search: the layer DP's loop (searchLayers), its dedupe tables, its stepped children, and the
// coin rules it enforces. cliMainOnce (cli.hpp) sets the search up and hands it a SearchEnv; the
// model it steps through is model.hpp.
#include "dp/reset.hpp"
#include "dp/clearance.hpp"
#include "dp/refwatch.hpp"
#include "dp/search_census.hpp"
#include "dp/port_audit.hpp"
#include "dp/cap_audit.hpp"
#include "dp/model.hpp"
#include "dp/trigevent_camera.hpp"
#include "dp/worldkey.hpp"
#include "dp/state_serial.hpp"
#include "dp/place_table.hpp"
#include "dp/trigshadow_live.hpp"
#include "dp/cam_y_search.hpp"   // --camy S2.1
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dp {

// --trigsupply-group (trigshadow_live.hpp, P6-0): the movers of the supplied groups take the shadow's place for the tick, and dcx / dcy the step from tick t-1 to t (both read in the same phase). The search runs
// it after model.prepare; a walk (model.hpp rollout, the --replay path) through g_rolloutPlaceHook after prepareGroup -- the same block, so the two cannot drift.
inline void supplyPlaceMovers(const GroupEnv& env, int gframe, int grev) {
    // --trigsupply-all (gd-04, a custom level:15800 g276): in a rotated gameplay frame the group's x is the frame's, not the world x the position triggers cross (the Move never fires in the shadow's timeline): the model's placement stays
    if (g_trigLive.supplyAll && gframe != 0) { ++g_trigLive.supFrame; return; }
    const std::vector<const Obj*>& near = env.near;
    const std::vector<const Obj*>& ports = env.ports;
    const std::vector<const Obj*>& pads = env.pads;
    const std::vector<const Obj*>& orbs = env.orbs;
    const std::vector<const Obj*>& slps = env.slps;
    std::vector<Obj*> done;
    for (const std::vector<const Obj*>* lp : {&near, &ports, &pads, &orbs, &slps})
        for (const Obj* oc : *lp) {
            if (!oc || !oc->dynObj || std::find(done.begin(), done.end(), const_cast<Obj*>(oc)) != done.end()) continue;
            double sx, sy, spx, spy;
            if (!g_trigLive.valueAt(oc->uid, sx, sy, spx, spy)) {
                // A turned hazard that has a recording is not supplied (no slot: its place is the rows'), but its box is not in the rows -- they hold the bound -- and what grows it is the timeline's scale since the
                // objrects were written. Only the scale is written here (the place, the previous place and the on/off stay the model's). ownScaleAt answers only for an objrects with the sscx column.
                if (!gframe && oc->oriented && oc->type == 2 && oc->radius <= 0.0) {
                    float qx = 1.f, qy = 1.f, px = 1.f, py = 1.f;
                    if (g_trigLive.ownScaleAt(oc->uid, qx, qy, px, py)) { Obj* oo = const_cast<Obj*>(oc); oo->sclX = qx; oo->sclY = qy; oo->sclPX = px; oo->sclPY = py; oo->sclT = g_trigLive.lastT; }
                }
                continue;
            }
            Obj* o = const_cast<Obj*>(oc);
            done.push_back(o);
            if (!(t_sqDyn && t_sqDyn->sqTouch(o))) g_sqForeign.store(true, std::memory_order_relaxed);   // the supply writes the object from here on (marked also where the ground clamp below leaves it): dynamics.hpp sqHead
            // GD's getObjectRect (what the recording and so the model's cx / cy / hh hold) raises the top of a rect that lies wholly below y = 0 up to 0: the rect's centre is then not the object's position
            // (measured on a custom level, w104 flight, wtrace against grouptrace: 1,827 of 1,827 differing rows have cy + h / 2 = 0 exactly, and no row of a rect wholly below 0 agrees). The model's own placement is that rect: kept.
            if (!gframe && sy < 0.0 && std::fabs(o->cy + o->hh) < 0.02) { ++g_trigLive.supGround; continue; }
            double fx = sx, fy = sy, fpx = spx, fpy = spy, olx = o->cx, oly = o->cy;
            if (gframe) { toFrame(gframe, sx, sy, fx, fy); toFrame(gframe, spx, spy, fpx, fpy); fromFrame(gframe, o->cx, o->cy, olx, oly); }
            g_trigLive.dbgFrame = gframe; g_trigLive.dbgRev = grev;
            g_trigLive.supplyNote(o->uid, o->cx, o->cy, o->dcx, o->dcy, fx, fy, fx - fpx, fy - fpy, olx, oly, sx, sy);
            o->cx = fx; o->cy = fy; o->dcx = fx - fpx; o->dcy = fy - fpy;
            // The previous place is the timeline's too. Obj::pcy (read by shadowPlaceMovers before cy - dcy) came from the model's own formula (Dynamics::groupPrevApply, or the last place the thread's
            // Dynamics made for another group): with cy and dcy the supply's it would be another world's step, and the pass would ride a mover whose previous and current place have two sources.
            o->pcy = (float)fpy;
            // The object's own scale is the timeline's too (a Transform that scales takes it along): the shadow's world reads it for a saw's radius (shadowPlaceMovers), where the load scale stood.
            { float qx = 1.f, qy = 1.f, px = 1.f, py = 1.f; if (g_trigLive.scaleAt(o->uid, qx, qy, px, py)) { if (gframe & 1) { std::swap(qx, qy); std::swap(px, py); } o->sclX = qx; o->sclY = qy; o->sclPX = px; o->sclPY = py; o->sclT = g_trigLive.lastT; } }
            // The object's own angle is the timeline's too (a Rotate turns a portal about its own centre: the place does not change, the box does). The placement turns a box by the recording's angle
            // (Dynamics::turnedBox), and a supplied group has no recording to read: the box stood at the angle it was loaded with. a custom level t=18056: a cube portal and a size portal (uid 152886 / 152885,
            // loaded at 45 degrees, eased round by a Rotate; the flight's row says 52.09) were met at t=18058 both, GD's at 18056 and 18057 -- the tick each box at the row's angle first overlaps the ship.
            // Only a box that is oriented as loaded (its own halves are known and a turn does not change them: turnedBox's `only the angle is redrawn`), in the level's own frame.
            if (!gframe && o->oriented && (lp == &ports || lp == &pads || lp == &orbs)) {
                double rdeg = 0.0;
                if (g_trigLive.rotAt(o->uid, rdeg)) { o->rotSup = rdeg; o->rotT = g_trigLive.lastT; }   // (stamped with its tick, as the scale: object.hpp)
            }
        }
}
// K1 (dynamics.hpp sqGateAnchored): the supply's on/off of the model's anchored movers as g_supplyOnHook gives it for the group being placed, kept per mover from call to call (`m.v`: 0 off, 1 on,
// 2 no answer, 3 not read yet). Read-only on the supply, in model code: the timeline's objects are read where supVal reads them (the object's disable counter before the group's step, `blind` after it), and
// only the pages that are not the ones this reader read last are read again (CowPages::Watch: a page it holds is shared, so no copy writes it again and its address is not given to another page).
struct SupOnSt {
    long long gen = -1; bool supOn = false; size_t n = 0, pages = 0;
    std::vector<int> slot;                          // per mover: its supply slot, -1 = none
    std::vector<uint32_t> page, start, list;        // the pages of the timeline that hold some mover (ascending); CSR page -> places in the movers' list
    gdtrig::CowPages<gdtrig::ObjState>::Watch wCur, wPre;
    std::vector<uint8_t> dirty;
};
inline void supplyOnSync(SupOnMirror& m, const std::vector<int>& uids, std::vector<uint32_t>& changed) {
    TrigLive& T = g_trigLive;
    changed.clear();
    if (!m.st) m.st = std::make_shared<SupOnSt>();
    SupOnSt& st = *static_cast<SupOnSt*>(m.st.get());
    const bool supOn = T.supplyOn();
    const size_t n = uids.size();
    const size_t pages = T.supCur ? T.supCur->objs.pageCount() : 0;
    if (m.v.size() != n || st.gen != T.supGen || st.n != n || st.supOn != supOn || st.pages != pages) {   // a new supply, another set of movers, another timeline's size: all of it is read again
        st.gen = T.supGen; st.n = n; st.supOn = supOn; st.pages = pages;
        m.v.assign(n, 3);
        st.wCur.clear(); st.wPre.clear();
        st.slot.assign(n, -1);
        std::vector<std::pair<uint32_t, uint32_t>> pj;
        for (size_t j = 0; j < n; ++j) {
            const auto it = T.supSlot.find(uids[j]);
            if (it == T.supSlot.end()) continue;
            st.slot[j] = it->second;
            const int ix = T.supObj[(size_t)it->second];
            if (ix >= 0 && T.supCur) pj.emplace_back((uint32_t)T.supCur->objs.pageOf((size_t)ix), (uint32_t)j);
        }
        std::sort(pj.begin(), pj.end());
        st.page.clear(); st.start.clear(); st.list.clear();
        for (const auto& e : pj) {
            if (st.page.empty() || st.page.back() != e.first) { st.page.push_back(e.first); st.start.push_back((uint32_t)st.list.size()); }
            st.list.push_back(e.second);
        }
        st.start.push_back((uint32_t)st.list.size());
    }
    auto tri = [&](size_t j) -> uint8_t {
        if (!supOn) return 2;
        const int sl = st.slot[j];
        if (sl < 0) return 2;
        TrigLive::SupVal v;
        return T.supVal((size_t)sl, v) ? (uint8_t)(v.pon ? 1 : 0) : (uint8_t)2;
    };
    auto setj = [&](size_t j) { const uint8_t nv = tri(j); if (m.v[j] != nv) { m.v[j] = nv; changed.push_back((uint32_t)j); } };
    if (!supOn || !T.supCur || T.supPre.pageCount() != pages) {   // no answer at all, or a timeline this reader cannot watch: every mover is read
        for (size_t j = 0; j < n; ++j) setj(j);
        st.wCur.clear(); st.wPre.clear();
        return;
    }
    st.dirty.assign(st.page.size(), 0);
    st.wCur.sync(T.supCur->objs, st.page, [&](size_t k) { st.dirty[k] = 1; });
    st.wPre.sync(T.supPre, st.page, [&](size_t k) { st.dirty[k] = 1; });
    for (size_t k = 0; k < st.page.size(); ++k)
        if (st.dirty[k]) for (uint32_t q = st.start[k]; q < st.start[k + 1]; ++q) setj((size_t)st.list[q]);
    static const bool check = std::getenv("K1CHECK") != nullptr;
    if (check) {   // every mover against the hook itself: a difference stops the process
        for (size_t j = 0; j < n; ++j) {
            int so = 0; const bool h = supplyOnHookFn(uids[j], so);
            const uint8_t want = h ? (uint8_t)(so ? 1 : 0) : (uint8_t)2;
            if (m.v[j] == 3 ? want != 2 && st.slot[j] >= 0 && false : m.v[j] != want) {
                std::printf("k1check: supplyOnSync MISMATCH uid=%d mirror=%d hook=%d\n", uids[j], (int)m.v[j], (int)want);
                std::fflush(stdout);
                std::abort();
            }
        }
    }
}// --supobs (dynamics.hpp): after the supply placed a frame-0 group's movers. `dyn` is the frame-0 Dynamics (the lists point into its objs), `wLo` / `wHi` the window the group's lists were
// collected for (model.hpp prepareGroup: near and pads +-40, portals and slopes +-60, orbs +-50). Reads only.
inline void supObsGroup(const GroupEnv& env, const Dynamics& dyn, double wLo, double wHi) {
    const std::vector<const Obj*>* lists[5] = {&env.near, &env.ports, &env.pads, &env.orbs, &env.slps};
    const double margin[5] = {40.0, 60.0, 40.0, 50.0, 60.0};
    SupObsStat& S = g_supObsStat;
    std::vector<const Obj*> done;
    for (int li = 0; li < 5; ++li)
        for (const Obj* oc : *lists[li]) {
            if (!oc || !oc->dynObj || std::find(done.begin(), done.end(), oc) != done.end()) continue;
            double sx, sy, spx, spy;
            if (!g_trigLive.valueAt(oc->uid, sx, sy, spx, spy)) continue;
            if (sy < 0.0 && std::fabs(oc->cy + oc->hh) < 0.02) continue;   // (the ground clamp: left on the model's placement, not a placed mover)
            done.push_back(oc);
            ++S.placed;
            if (sx < wLo - margin[li] || sx > wHi + margin[li]) { ++S.outWin; ++S.outWinBy[li]; }
            const std::ptrdiff_t i = dyn.objs.empty() ? -1 : (oc - &dyn.objs[0]);
            if (i < 0 || (size_t)i >= dyn.objs.size() || (size_t)i >= dyn.samples.size() || dyn.samples[(size_t)i].empty()) { ++S.noRow; continue; }
            const DynSample& r0 = dyn.samples[(size_t)i][0];
            const double dw = std::fabs(oc->hw - (double)r0.hw), dh = std::fabs(oc->hh - (double)r0.hh);
            float qx = 1.f, qy = 1.f;
            const bool scaled = g_trigLive.scaleAt(oc->uid, qx, qy) && (std::fabs(qx - 1.f) > 1e-6f || std::fabs(qy - 1.f) > 1e-6f);
            if (dw <= 1e-3 && dh <= 1e-3) { ++S.sizeLoad; if (scaled) ++S.scaledButLoad; continue; }
            ++S.sizeDiff;
            if (std::max(dw, dh) > S.maxDiff) { S.maxDiff = std::max(dw, dh); S.maxDiffUid = oc->uid; }
            if (!scaled) { ++S.sizeDiffUnscaled; continue; }
            ++S.sizeDiffScaled;
            if (std::fabs(oc->hw - (double)r0.hw * qx) <= 1e-3 && std::fabs(oc->hh - (double)r0.hh * qy) <= 1e-3) ++S.sizeScaledMatch;
        }
}
// --layerprof (progress.hpp): the running totals a row is the difference of, and their names. Counts first, then milliseconds: --phaseprof's phases of the layer loop, the parts of the group
// setup (g_ppSub), the step (thread time inside the children's steps, wall time of the step blocks), the supply's placement, and the trigger shadow's own clocks (group() in all, its part
// before the key, stepNow in all and its four parts, endLayer, dropping the older layer). The shadow's times lie INSIDE the phases (group() is called from the setup), so the columns do not add up.
inline const char* layerProfHeader() {
    return "groups\tparents\tdup_leaders\tdup_followers\tsetup\tstep\tdedupe\tcap\tgc\tmem\trest\ts_autotrig\ts_groupkeys\ts_rottrig\ts_preprest\tp_seek\tp_apply\tp_near\tp_windows\tp_touch\tp_veto"
           "\tkid_thread\tstep_wall\tsupply\tsh_group\tsh_prekey\tsh_stepnow\tsh_copyfeed\tsh_table\tsh_step\tsh_rest\tsh_endlayer\tsh_drop";
}
inline int layerProfTotals(double* o) {
    int n = 0;
    o[n++] = (double)g_ppGroups; o[n++] = (double)g_ppParents; o[n++] = (double)g_dupBatchLeaders; o[n++] = (double)g_dupBatchFollowers;
    for (int i = 0; i < 7; ++i) o[n++] = g_ppMain[i] * 1e3;
    for (int i = 0; i < 10; ++i) o[n++] = g_ppSub[i] * 1e3;
    o[n++] = (double)g_ppKidNs.load(std::memory_order_relaxed) * 1e-6; o[n++] = (double)g_ppStepNs * 1e-6; o[n++] = (double)g_ppSupplyNs * 1e-6;
    o[n++] = g_trigLive.grpTotalMs; o[n++] = g_trigLive.grpPre; o[n++] = g_trigLive.grpStepNow;
    o[n++] = g_trigLive.nowCopyMs; o[n++] = g_trigLive.nowFillMs; o[n++] = g_trigLive.nowStepMs; o[n++] = g_trigLive.nowTailMs;
    o[n++] = g_trigLive.grpEndLayerMs; o[n++] = g_trigLive.grpClearMs;
    return n;
}
inline void rolloutGroupShadow(long long t, float dx, const State& s, double x) {
    if (g_trigLive.supplyOn()) {
        // the walk's one state: its world y (frame 0); a turned frame's travel axis is the progress axis, and its lateral part (s.y in the frame's coordinates) is the world x of an odd frame (frames.hpp fromFrame). NOTE: s is
        // the state before this tick's step (x is the tick's), so y is one tick old: a crossing can fire one tick late
        double wxw = std::nan("");
        if ((int)s.frame & 1) { double X = 0.0, Y = 0.0; fromFrame((int)s.frame, 0.0, (double)s.y, X, Y); wxw = X; }
        g_trigLive.gin = TrigLive::GroupIn{}; g_trigLive.gin.evg = s.evgate;   // (the events the walk's one state raised: the timeline is told them, feedEvents)
        g_trigLive.group(t, dx, s.trig.word(0), (int)s.frame, (int)s.rev, 0, 0, x, s.fireB, s.frame == 0 ? (double)s.y : std::nan(""), wxw);
    }
}
inline void rolloutPlaceShadow(const GroupEnv& env, int gframe, int grev) {
    if (g_trigLive.supplyOn()) supplyPlaceMovers(env, gframe, grev);
}
inline const std::vector<int>& tpEventsAt(long long t) { return g_trigLive.tpFiresAt(t); }
inline bool tpEventsSupplyOn() { return g_trigLive.supplyOn(); }
inline bool tpEventsModelled(int uid) { return g_trigLive.base && g_trigLive.base->modelled(uid); }
inline bool supBoxPlace(int uid, double& x, double& y, bool& on) {   // shadow_step.hpp placeBox: a special box a trigger moves, with no recording rows, stands where the supply's timeline has it
    if (!g_trigLive.supplyOn()) return false;
    double px, py;
    if (!g_trigLive.valueAt(uid, x, y, px, py)) return false;
    bool o = true;
    on = g_trigLive.onAt(uid, o) ? o : true;
    return true;
}
inline const bool g_tpEventsHooksInstalled = [] { g_tpEventsHook = &tpEventsAt; g_tpEventsSupplyOnHook = &tpEventsSupplyOn; g_tpEventsModelledHook = &tpEventsModelled; g_supBoxPlaceHook = &supBoxPlace; return true; }();
inline const std::vector<int>& gravEventsAt(long long t) { return g_trigLive.gravFiresAt(t); }
inline void supplyHitsOf(double x0, double x1, SupHits& out) {
    if (g_trigLive.supplyOn()) g_trigLive.supplyHits(x0, x1, out);
}
inline const bool g_rolloutSupplyHooksInstalled = [] { g_rolloutGroupHook = &rolloutGroupShadow; g_rolloutPlaceHook = &rolloutPlaceShadow; g_supplyHitsHook = &supplyHitsOf; g_gravEventsHook = &gravEventsAt; return true; }();

// DUPOBS_OUT=FILE (env, read once; observation only; E8 W3): do the groups of a layer that share one shadow world also share one tick of the model's world?
// For each group, after its geometry is placed (model.prepare, then the supply), two hashes:
//   dup   what the trigger shadow's side of the group is: the content and track hash of its timeline after the step (TrigShadow::contentHash / trackHash), the group's x, its window, its speed, frame and direction
//   ctx   what the group's step reads beside its states: x / xPrev / dx, the objects in its windows (which objects, in which order, and for a moving one where the tick put it: place, size, turn, per-tick
//         motion, on / off, scale), the touch boxes in reach, the autonomous triggers' fire ticks as this group has them, and the Teleports / Gravity triggers its shadow step fired
// One line per layer: layer, groups, distinct dup, distinct (dup, ctx), groups whose (dup, ctx) an earlier group of the layer had (a REPEAT: its prepare made what was already there) and their parents,
// groups whose dup an earlier group had with ANOTHER ctx (the model's side tells them apart although the shadow's does not). Totals at the end of the process.
// Not in ctx: the coins' and the Follow objects' per-group lists (their sizes are), the camera Y's per-group input.
struct DupObs {
    enum { kParts = 7 };   // ctx by part: 0 x / xPrev / dx, 1 which objects in which order, 2 the moving objects' placement, 3 the touch boxes in reach, 4 the coin / follow list sizes, 5 the autonomous triggers' fire ticks, 6 the shadow step's Teleports / Gravity
    enum { kKeyParts = 6 };   // what tells the two groups of a REPEAT apart in the search's own group key: 0 the touch mask, 1 the boxes' fire ticks (gFire, gFireB), 2 the lock offset, 3 the own-crossing records' signature, 4 the fire signature, 5 the x part
    struct Row { uint64_t dup, ctx; long long parents; uint64_t part[kParts]; uint64_t kp[kKeyParts]; };
    std::vector<Row> rows;
    long long keyDiff[kKeyParts] = {}, repeatsNoOldInput = 0;   // repeats whose parts 0..2 (the old model's inputs) are the first group's too
    long long t = -1, groups = 0, dupKeys = 0, pairKeys = 0, repeats = 0, repeatParents = 0, differ = 0, parentsAll = 0, noShadow = 0;
    long long partDiff[kParts] = {}, differShown = 0;
    static std::FILE* file() { static std::FILE* const f = [] { const char* e = std::getenv("DUPOBS_OUT"); return (e && *e) ? std::fopen(e, "a") : nullptr; }(); return f; }
    static bool on() { return file() != nullptr; }
    void flush() {
        if (rows.empty()) return;
        std::unordered_map<uint64_t, size_t> sp;   // (dup, ctx) -> the first row that had it
        std::unordered_map<uint64_t, size_t> sd;   // dup -> the first row that had it
        long long rep = 0, repPar = 0, dif = 0, par = 0;
        for (size_t ri = 0; ri < rows.size(); ++ri) {
            const Row& r = rows[ri];
            par += r.parents;
            const uint64_t pk = gdtrig::Hash2::mixA(r.dup, r.ctx);
            const auto di = sd.find(r.dup);
            const auto pi = sp.find(pk);
            if (pi != sp.end()) {
                ++rep; repPar += r.parents;
                bool oldSame = true;
                for (int k = 0; k < kKeyParts; ++k) if (r.kp[k] != rows[pi->second].kp[k]) { ++keyDiff[k]; if (k <= 2) oldSame = false; }
                if (oldSame) ++repeatsNoOldInput;
            }
            else if (di != sd.end()) {   // a new pair of a dup seen before: which parts of the ctx differ from the first group's
                ++dif;
                unsigned mask = 0;
                for (int k = 0; k < kParts; ++k) if (r.part[k] != rows[di->second].part[k]) { ++partDiff[k]; mask |= 1u << k; }
                if (differShown++ < 40) std::fprintf(file(), "# differ t=%lld parts=%02x parents=%lld (first group's %lld)\n", t, mask, r.parents, rows[di->second].parents);
            }
            if (pi == sp.end()) sp.emplace(pk, ri);
            if (di == sd.end()) sd.emplace(r.dup, ri);
        }
        std::fprintf(file(), "%lld %zu %zu %zu %lld %lld %lld\n", t, rows.size(), sd.size(), sp.size(), rep, repPar, dif);
        groups += (long long)rows.size(); dupKeys += (long long)sd.size(); pairKeys += (long long)sp.size(); repeats += rep; repeatParents += repPar; differ += dif; parentsAll += par;
        rows.clear();
    }
    uint64_t kpNow[kKeyParts] = {};   // set by the caller before group() (keyParts)
    void keyParts(const TouchMask& gtrig, int gFire, const uint16_t* gFireB, float lockAt, uint64_t xcS, uint64_t fireS, int xc) {
        gdtrig::Hash2 a, b;
        for (int k = 0; k < (kTouchBits + 63) / 64; ++k) a.add(gtrig.word(k));
        b.add((uint64_t)(uint32_t)gFire); for (int i = 0; i < kTouchBits; ++i) if (gtrig.test(i)) b.add((uint64_t)gFireB[i]);
        uint32_t lb; std::memcpy(&lb, &lockAt, 4);
        kpNow[0] = a.a; kpNow[1] = b.a; kpNow[2] = lb; kpNow[3] = xcS; kpNow[4] = fireS; kpNow[5] = (uint64_t)(uint32_t)xc;
    }
    void group(const GroupEnv& env, int gframe, int grev, float gdx, double wLo, double wHi, long long tt, size_t parents) {
        if (tt != t) { flush(); t = tt; }
        if (!g_trigLive.supplyOn() || !g_trigLive.supCur) { ++noShadow; return; }
        gdtrig::Hash2 d;
        d.addH(g_trigLive.supCur->contentHash()); d.addH(g_trigLive.supCur->trackHash());
        d.addD(env.x); d.addD(wLo); d.addD(wHi); d.addF(gdx); d.add((uint64_t)(uint32_t)gframe | ((uint64_t)(uint32_t)grev << 32));
        gdtrig::Hash2 pc[kParts];
        pc[0].addD(env.x); pc[0].addD(env.xPrev); pc[0].addF(env.dxF);
        const std::vector<const Obj*>* lists[7] = {&env.near, &env.ports, &env.pads, &env.orbs, &env.slps, &env.spds, &env.nearSolid};
        for (const auto* lp : lists) {
            pc[1].add(0x4c00 + (uint64_t)lp->size());
            for (const Obj* o : *lp) {
                pc[1].add((uint64_t)(uintptr_t)o);
                if (!o || !o->dynObj) continue;
                gdtrig::Hash2& c = pc[2];
                c.addD(o->cx); c.addD(o->cy); c.addD(o->hw); c.addD(o->hh); c.addD(o->radius); c.addD(o->rot); c.addD(o->sy0); c.addD(o->sy1); c.addD(o->ohw); c.addD(o->ohh); c.addD(o->rc); c.addD(o->rs);
                c.addD(o->gcx); c.addD(o->gcy); c.addD(o->bhw); c.addD(o->bhh); c.addD(o->bc); c.addD(o->bs); c.addD(o->dcx); c.addD(o->dcy); c.addD(o->dhh); c.addF(o->pcy); c.addF2(o->sclX, o->sclY); c.addF2(o->sclPX, o->sclPY);
                c.add((uint64_t)o->oriented | ((uint64_t)o->obbOk << 8) | ((uint64_t)o->hasGc << 16) | ((uint64_t)o->rotTurned << 24) | ((uint64_t)o->recOn << 32) | ((uint64_t)o->onPrev << 40) | ((uint64_t)o->onNew << 48) | ((uint64_t)o->slope << 56));
            }
        }
        pc[3].add(0x4d00 + (uint64_t)env.trigs.size());
        for (const auto& tb : env.trigs) { pc[3].add((uint64_t)(uintptr_t)tb.first); for (int k = 0; k < (kTouchBits + 63) / 64; ++k) pc[3].add(tb.second.word(k)); }
        pc[4].add(0x4e00 + (uint64_t)env.coinLive.size()); pc[4].add(0x4f00 + (uint64_t)env.fol.size());
        pc[5].add(0x5000 + (uint64_t)g_autoTrig.size()); for (const AutoTrig& a : g_autoTrig) pc[5].add((uint64_t)(uint32_t)a.fireT);
        pc[6].add(0x5100); for (int u : g_trigLive.tpFiresAt(tt)) pc[6].add((uint64_t)(uint32_t)u);
        pc[6].add(0x5200); for (int u : g_trigLive.gravFiresAt(tt)) pc[6].add((uint64_t)(uint32_t)u);
        Row row{};
        gdtrig::Hash2 c;
        for (int k = 0; k < kParts; ++k) { row.part[k] = pc[k].a ^ (pc[k].b * 0x9e3779b97f4a7c15ULL); c.add(row.part[k]); }
        row.dup = d.a ^ (d.b * 0x9e3779b97f4a7c15ULL); row.ctx = c.a ^ (c.b * 0x9e3779b97f4a7c15ULL); row.parents = (long long)parents;
        for (int k = 0; k < kKeyParts; ++k) row.kp[k] = kpNow[k];
        rows.push_back(row);
    }
    ~DupObs() {
        if (!on()) return;
        flush();
        std::fprintf(file(), "# total: groups %lld (parents %lld), distinct dup %lld, distinct (dup, ctx) %lld, repeats %lld (parents %lld), same dup with another ctx %lld, groups without a shadow timeline %lld\n",
                     groups, parentsAll, dupKeys, pairKeys, repeats, repeatParents, differ, noShadow);
        std::fprintf(file(), "# the parts that differed in those: x %lld, objects %lld, placement %lld, touch boxes %lld, coin / follow sizes %lld, auto fire ticks %lld, shadow events %lld\n",
                     partDiff[0], partDiff[1], partDiff[2], partDiff[3], partDiff[4], partDiff[5], partDiff[6]);
        std::fprintf(file(), "# what told a repeat from the first group with its (dup, ctx): touch mask %lld, fire ticks %lld, lock offset %lld, own-crossing signature %lld, fire signature %lld, x part %lld; repeats with the first group's mask, fire ticks and lock offset %lld\n",
                     keyDiff[0], keyDiff[1], keyDiff[2], keyDiff[3], keyDiff[4], keyDiff[5], repeatsNoOldInput);
        std::fflush(file());
    }
};
inline DupObs g_dupObs;

// --coins: a counter a coin needs (see where coinReq is filled in cliMainOnce).
struct CoinCountReq {
    int item;      // which counter
    int need;      // ...has to reach this
    int coinIdx;   // the coin it decides (skip once that one is collected)
};

// --coins: the touch bits that, once fired, put a coin out of reach (see coinGate in cliMainOnce).
struct CoinOnGate {
    int coinIdx;
    // THE MASK'S OWN TYPE. This was uint32_t while s.trig, which it is
    // tested against, is a TouchMask: at a width of 64 the `1u << b` that
    // filled it is undefined for b >= 32 (and wraps mod 32 on x86), the
    // store truncates, and the test can only ever match the low half. The
    // gate sets dead = true, so a wrong bit KILLS BRANCHES -- it is the
    // expensive direction of that mistake, not the harmless one.
    std::vector<std::pair<TouchMask, double>> ons;  // bit, x past which it is gone
};

// cell -> hi/lo representatives (indices into nxt). Keep BOTH vy extremes
// per cell: first-wins kept the slowest lineage and strangled climbs
// (measured: ship climb rate collapsed to ~0.02 vy/tick); a single
// extreme-|vy| slot instead discarded dive-recovery lineages.
struct Slots { int hi = -1; int lo = -1; };
// Open addressing, linear probing, cleared in O(used) via `touched`.
// std::unordered_map was the whole serial phase once stepping went parallel
// (a node allocation and a pointer chase per child, ~80k children a layer).
// Semantics are identical -- key -> Slots, lookup and insert only, never
// iterated -- so `nxt` still fills in the same order and the plan is
// bit-identical. Key 0 doubles as "empty": keyOf is a scrambled XOR, so a
// real key of exactly 0 is a 2^-64 event, and if it ever happened it would
// merge two cells, which is what a hash collision already does.
struct CellMap {
    std::vector<uint64_t> keys;
    std::vector<Slots> vals;
    std::vector<uint32_t> touched;
    size_t mask = 0;
    void ensure(size_t want) {
        size_t n = 1024;
        while (n < want * 4) n <<= 1;   // keep the load factor under 1/4
        if (n <= keys.size()) return;
        keys.assign(n, 0);
        vals.assign(n, Slots{});
        mask = n - 1;
    }
    void clear() {
        for (uint32_t i : touched) keys[i] = 0;
        touched.clear();
    }
    // same names the memory report used when this was an unordered_map
    size_t size() const { return touched.size(); }
    size_t bucket_count() const { return keys.size(); }
    Slots& at(uint64_t k) {
        size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull) & mask;
        for (;;) {
            if (keys[i] == k) return vals[i];
            if (keys[i] == 0) {
                keys[i] = k;
                vals[i] = Slots{};
                touched.push_back((uint32_t)i);
                return vals[i];
            }
            i = (i + 1) & mask;
        }
    }
};

// ---- parallel dedupe (phase 2p) -------------------------------------
// The serial dedupe was the measured ceiling of the whole DP (1 thread
// 22.5 s -> 8 threads 8.0 s -> 16 threads 8.9 s on lv16's cap-40,000
// layers: the stepping scales, the dedupe does not). It parallelises by
// KEY: children of one key all land in one shard (keyed by a hash of the
// dedupe key), each shard replays emit()'s exact hi/lo hysteresis over
// its keys in ordinal order, and a serial merge then materialises `nxt`
// in the SAME slot order the serial loop would have produced -- the
// emitted plan stays bit-identical (checked against --old-slope-era runs
// and asserted by the regression). Only the arena's node COUNT differs:
// the serial loop pushed a node per accepted improvement, this pushes one
// per final representative, so the arena is strictly smaller and parent
// CHAINS (the only thing read back) are unchanged.
struct KeyRec {
    uint64_t key;
    uint32_t aOrd;          // ordinal that allocated the key's first slot
    uint32_t bOrd;          // ordinal of the first hi/lo split (kNone: none)
    uint32_t hiIdx, loIdx;  // child index of the current hi / lo rep
    float hiVy, loVy;
    uint8_t bWasHi;
};
struct ShardMap {           // CellMap's open addressing, key -> rec index
    std::vector<uint64_t> keys;
    std::vector<uint32_t> vals;
    std::vector<uint32_t> touched;
    std::vector<KeyRec> recs;
    uint32_t goalOrd = kNone;
    uint16_t goalTight = 0xffff;   // route score; see the goal pick below
    size_t mask = 0;
    void ensure(size_t want) {
        size_t n = 1024;
        while (n < want * 4) n <<= 1;
        if (n <= keys.size()) return;
        keys.assign(n, 0);
        vals.assign(n, kNone);
        mask = n - 1;
    }
    void clear() {
        for (uint32_t i : touched) keys[i] = 0;
        touched.clear();
        recs.clear();
        goalOrd = kNone;
        goalTight = 0xffff;
    }
    // ensure() sizes for the EXPECTED keys per shard; a lopsided hash
    // could still overfill one shard's table, and a full open-addressing
    // table is an infinite probe loop, not a slowdown. Grow keeps the
    // guarantee unconditional.
    void grow() {
        const size_t n = keys.size() ? keys.size() * 2 : 1024;
        std::vector<uint64_t> ok;
        ok.swap(keys);
        std::vector<uint32_t> ov;
        ov.swap(vals);
        keys.assign(n, 0);
        vals.assign(n, (uint32_t)kNone);
        mask = n - 1;
        touched.clear();
        for (size_t i = 0; i < ok.size(); ++i) {
            if (!ok[i]) continue;
            size_t j = (size_t)(ok[i] * 0x9E3779B97F4A7C15ull) & mask;
            while (keys[j]) j = (j + 1) & mask;
            keys[j] = ok[i];
            vals[j] = ov[i];
            touched.push_back((uint32_t)j);
        }
    }
    uint32_t& at(uint64_t k) {
        if ((touched.size() + 1) * 4 > keys.size()) grow();
        size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull) & mask;
        for (;;) {
            if (keys[i] == k) return vals[i];
            if (keys[i] == 0) {
                keys[i] = k;
                vals[i] = kNone;
                touched.push_back((uint32_t)i);
                return vals[i];
            }
            i = (i + 1) & mask;
        }
    }
};

// One stepped child. Slot 2i is state i's no-input child, 2i+1 its pressed
// one (empty when that state does not expand both -- see stepKid).
struct Child {
    State s{};
    uint64_t key = 0;         // modelKeyOf(s) ^ rules.keyBits(s), computed on the worker thread
    uint8_t dead = 0;
    uint8_t deadKind = 0;     // g_deadKind of a dead child (kDeathPhysics / kDeathRule / kDeathPrune): --partialphys
    uint8_t valid = 0;
    const char* why = "";     // --dbg only
    const Obj* obj = nullptr; // --dbg only
    int rotUid = -1;          // --rotwatch only: the 2900 stepKid applied
    int8_t rotF0 = -1, rotNf = -1;
};


// ---- per-tick input levels -> the plan's `input=press,level` edges --------------------------
//
// ONE copy of this conversion, because two callers now depend on it agreeing with itself: the
// plan writer at the end of cliMain, and the common-prefix publisher inside the layer loop
// (dp/progress.hpp). The prefix is only usable if the inputs the caller flies are the ones the
// emitted plan WOULD have carried for those ticks -- an edge placed one tick out is 2 ticks of
// dy at the seam (the measured lv20 case in the writer's own note below), and the caller would
// be testing a plan the search never proposed.
//
// It is a left-to-right scan carrying `prev`, so running it over a PREFIX of `lvl` gives
// exactly the leading edges it gives over the whole of `lvl`: every index it reads
// (`i - 1`, `i - lat0`) is smaller than `i`, so nothing behind the cut is consulted.
struct PlanEdge {
    long long press;   // the tick the button changes state on
    int level;         // ...and to what
};

// --start-state (statefile.hpp): the button state the plan starts from, by PRESS tick, as --start's
// hold field names it (the caller's heldBefore over the prefix it splices onto). The state's own
// `held` is the physics' (a cube's is never written), so the edges are counted from this instead,
// which is what a call seeded from the game's anchor counts them from. -1 = not a --start-state call.
inline int g_planPrevHeld = -1;

// --needsize <tick>:<0|1>: player 1 must be normal (0) or mini (1) on that tick; the search drops
// every state that is not (a rule, not the game's physics). The loop asks it of a search that begins
// before an optional size change when the game has shown that only the other size gets over the
// wall ahead (src/mod/repair.hpp, cfg dpsecstate): "be that size at the wall" lets the route take an
// unavoidable portal and the one that undoes it, which "never change the size" cannot. -1 = off.
inline long long g_needSizeT = -1;
inline int g_needSizeMini = 0;

// --needspeed <x>:<multiplier> / <x>:!<multiplier>: the state that crosses x must be at that speed (0.7 / 0.9 / 1.1 /
// 1.3 / 1.6) as it does, or must not be (the "!" form); the search drops every one that breaks it (a rule, not the
// game's physics). The same question as --needsize, for the speed: a route that took an optional speed portal can reach a
// wall only another speed gets over. It is asked by x and not by tick because the speeds do not share a clock: at
// the same tick a faster state is already further on, and "be at another speed when you reach the wall" is
// the same for every one of them. A state that starts at or past x is not asked. Negative x = off.
inline double g_needSpeedX = -1.0;
inline int g_needSpeedCode = 0;
inline bool g_needSpeedNot = false;
inline int planPrevHeld(const State& init) {
    return g_planPrevHeld >= 0 ? g_planPrevHeld : (int)init.held;
}

// `modeAt[i]` is the mode at the END of tick t0+i+1, `prevHeld` the button state the plan
// starts from and `initMode` the anchor's mode (used when the lookback reaches before tick 0).
inline std::vector<PlanEdge> planEdges(const std::vector<uint8_t>& lvl,
                                       const std::vector<uint8_t>& modeAt, long long t0,
                                       int prevHeld, uint8_t initMode) {
    std::vector<PlanEdge> out;
    int prev = prevHeld;
    for (size_t i = 0; i < lvl.size(); ++i) {
        if (lvl[i] == prev) continue;
        const long long effect = t0 + (long long)i + 1;
        auto latOf = [](uint8_t m) { return (m == 1 || m == 3) ? 2 : 1; };
        auto modeAtTick = [&](long long j) -> uint8_t {
            if (j < 0) return initMode;
            if ((size_t)j >= modeAt.size()) return modeAt.empty() ? initMode : modeAt.back();
            return modeAt[(size_t)j];
        };
        // The latency is read at the tick the button last changed under: lat0 from the tick before this one,
        // then the mode at (i - lat0). (A mode-at-i form, --old-latency, existed only to bisect lv18.)
        const int lat0 = latOf(modeAtTick((long long)i - 1));
        const int lat = latOf(modeAtTick((long long)i - lat0));
        out.push_back(PlanEdge{effect - lat, (int)lvl[i]});
        prev = lvl[i];
    }
    return out;
}

// The gamble (PlainElsewhere::ready): the deepest tick an attempt of the ladder in progress has
// died on, for the plain search beside it to compare itself with, and the flag that stops the
// attempt in progress once that search has been taken. Both belong to one ladder: cliMain sets
// them at its start and clears them on the way out. The CLI never sets the flag.
inline std::atomic<long long> g_ladderBestT{-1};
inline std::atomic<bool> g_ladderStop{false};

// ---- where the search is going ----------------------------------------------------------------
// The goal and the route the layer loop is asked to reach, kept apart from how it searches
// (search-plugin cut 5). searchLayers takes any type with these members; CurrentRules is today's:
// the level's end in frame 0, with every coin when --coins is on. `C` is cliMainOnce's CoinTables:
// references to the coin tables built at load and to the two lambdas the coin rules call.
// heldRingInReach's moving rings (search below): per Dynamics, the box every recorded place of each ORB-bucket mover and its place at the build lie in. Built
// once per level object and kept while its object count and rows are the ones it was built from, for one call (g_shCallGen: a new call empties the list, so
// no entry outlives the level it names); under a mutex, since the groups that ask may run at once.
struct MovRingBox { double x0, x1, y0, y1; bool anywhere; };   // anywhere: a mover with no rows (no bound from them, in x or y)
struct MovRingBoxes { const void* dyn = nullptr; size_t n = 0; const void* rows0 = nullptr; uint64_t gen = 0; std::vector<MovRingBox> box; size_t anywhere = 0; };
inline std::mutex g_movRingMu;
inline std::vector<std::unique_ptr<MovRingBoxes>> g_movRings;
inline const MovRingBoxes& heldRingBoxesOf(const Dynamics& D) {
    std::lock_guard<std::mutex> lk(g_movRingMu);
    const void* rows0 = D.samples.empty() ? nullptr : (const void*)D.samples[0].rows().data();
    if (!g_movRings.empty() && g_movRings.front()->gen != g_shCallGen) g_movRings.clear();
    for (const auto& p : g_movRings) if (p->dyn == &D && p->n == D.objs.size() && p->rows0 == rows0) return *p;
    auto m = std::make_unique<MovRingBoxes>();
    m->dyn = &D; m->n = D.objs.size(); m->rows0 = rows0; m->gen = g_shCallGen;
    for (size_t i = 0; i < D.objs.size() && i < D.bucket.size(); ++i) {
        if (D.bucket[i] != Dynamics::ORB) continue;
        const Obj& o = D.objs[i];
        MovRingBox b{(double)o.cx - o.hw, (double)o.cx + o.hw, (double)o.cy - o.hh, (double)o.cy + o.hh, false};
        const bool rows = i < D.samples.size() && !D.samples[i].rows().empty();
        if (rows)
            for (const DynSample& r : D.samples[i].rows()) {
                b.x0 = std::min(b.x0, (double)r.cx - r.hw); b.x1 = std::max(b.x1, (double)r.cx + r.hw);
                b.y0 = std::min(b.y0, (double)r.cy - r.hh); b.y1 = std::max(b.y1, (double)r.cy + r.hh);
            }
        else { b.anywhere = true; ++m->anywhere; }
        m->box.push_back(b);
    }
    std::printf("heldring: %zu moving ring(s) (%zu without rows) for the held-jump test\n", m->box.size(), m->anywhere);
    g_movRings.push_back(std::move(m));
    return *g_movRings.back();
}

template <class C>
struct CurrentRules {
    double goalX;      // the level's end (cliMainOnce: L.maxX + 60)
    bool coinOn;       // --coins, on a level whose coins fit the mask
    uint32_t coinAll;  // ...and the mask of all of them
    const C& c;

    // Has `s` reached the goal? Only in frame 0: goalX is a world x, and in a turned frame xAbs is a
    // different axis. Under --coins, only with the full set. Both dedupes ask this.
    bool reached(const State& s) const {
        return s.frame == 0 && (double)s.xAbs >= goalX
               && (!coinOn || s.coins == (uint8_t)coinAll);
    }
    // Of two states that reached it, is the one with route score `cand` to replace `held`?
    bool prefer(uint16_t cand, uint16_t held) const { return roomierRoute(cand, held); }
    // How good `s` is, for a search that ranks states (the beam): more coins first under --coins,
    // then how far it has got.
    double score(const State& s) const {
        return (coinOn ? 1e7 * popCount32((uint32_t)s.coins) : 0.0) + progressOf(s).x;
    }
    // --coinpick: the state a cut or a PARTIAL emits -- the first one holding the most coins.
    // Without --coins it is front().
    const State& pick(const std::vector<State>& v) const {
        size_t best = 0;
        if (coinOn) {
            int most = -1;
            for (size_t i = 0; i < v.size(); ++i) {
                const int c = popCount32((uint32_t)v[i].coins);
                if (c > most) { most = c; best = i; }
            }
        }
        return v[best];
    }

    // --coins only. Measured 2026-08-31: with the band class on for
    // every run, 20 of the 21 levels the cold suite reached kept their
    // baseline iteration count exactly -- and lv20 went 37 -> 50. That
    // is the level where the band really does branch, and +13 cold
    // iterations is not a price the PLAIN solver has any reason to pay:
    // it needs the deepest route, which is the majority the water-fill
    // already keeps. Preserving a minority branch is a coin-routing
    // problem, so the class is a coin-routing class.
    bool bandClass() const { return coinOn; }
    // --coinwin (thread_pool.hpp): the (y, vy) cell of a state inside the approach window
    // of a coin it still lacks, and 0 everywhere else -- so with the flag off, or outside
    // every window, the cap's partition is the one it was.
    uint64_t winCell(const State& s) const {
        auto& L = c.L;
        auto& coinPruneOk = c.coinPruneOk;
        if (!coinOn || g_coinWinY <= 0.0 || s.frame != 0) return 0;
        for (size_t ci = 0; ci < L.coins.size() && ci < 8; ++ci) {
            if (!coinPruneOk[ci] || ((s.coins >> ci) & 1u)) continue;
            const double cx = L.coins[ci].cx;
            if ((double)s.xAbs < cx - g_coinWinLen || (double)s.xAbs > cx + L.coins[ci].hw)
                continue;
            const int64_t yb = (int64_t)std::floor((double)s.y / g_coinWinY);
            const int64_t vb = (int64_t)std::floor((double)s.vy / g_coinWinVy);
            return (((uint64_t)(ci + 1) << 40) ^ ((uint64_t)(yb & 0xfffff) << 16)
                    ^ (uint64_t)(vb & 0xffff)) * 0xBF58476D1CE4E5B9ull;
        }
        return 0;
    }
    // May the search join an old plan at `rjS` (null: no join found)? With --coins, not from
    // INSIDE an open counting-tap window whose gate has not fired. The old plan's tail is flown as
    // it is, and a join is a SOLVED: joined there, the presses still owed are never planned.
    // Anywhere else a join stays what it was -- a tail that then passes the window short is ended
    // by the mod at the shut point (Outcome::coinGates), and the loop plans the window again from
    // an anchor inside it. (Refusing every join short of all coins was tried first: 0 joins
    // against 34, and the run stalled at x=3.6-4.9k, 1,000 px before any coin.)
    // ...nor after the gate has fired while a coin it opens is still to be taken: the old tail was
    // flown with the coin off, and on lv22 it glides under it.
    bool rejoinOk(const State* rjS) const {
        auto& tapGive = c.tapGive;
        auto& tapGateBits = c.tapGateBits;
        auto& gatedMask = c.gatedMask;
        const bool rjWindow = rjS && (rjS->taps & 0x80) && !(rjS->taps & 0x40)
                              && !(rjS->trig & tapGateBits);
        const bool rjOwed = rjS && gatedMask && (rjS->trig & tapGateBits)
                            && (rjS->coins & gatedMask) != gatedMask;
        return !coinOn || !tapGive.bit || !(rjWindow || rjOwed);
    }

    // The route's own part of a state's dedupe key: the coins collected. A search keys a state by
    // the model's key (search_key.hpp modelKeyOf) xor this. 0 with no coin taken, so a run without
    // --coins keys by the model alone.
    uint64_t keyBits(const State& s) const { return coinKeyBits(s); }
    // ...and of its class under the cap's water-fill (searchLayers classOf): the collected set is a
    // class of its own, so the fill -- which serves the SMALLEST class first -- protects the few
    // lineages that took a coin instead of thinning them at the stride of the many that are merely
    // inside its window. 0 without coins, so every partition without --coins is unchanged.
    uint64_t classBits(const State& s) const { return (uint64_t)s.coins << 40; }
    // The route's part of the root state. --coinmask: the coins GD had already credited at the
    // anchor (the repair loop's re-anchor, repair.hpp). An anchored tail otherwise starts with an
    // empty set, so every coin behind x0 reads as missed and the miss prune kills the seed on its
    // first step. Bits for coins the level does not have drop. --coinskip: every coin test skips a
    // collected coin, so seeding the bit takes that coin out of the goal and out of both miss prunes.
    void init(State& s) const {
        if (!coinOn) return;
        s.coins = (uint8_t)((uint32_t)g_coinMaskSeed & coinAll);
        if (s.coins)
            std::printf("coins: starting with 0x%x already collected\n", (unsigned)s.coins);
        const uint8_t skip = (uint8_t)((uint32_t)g_coinSkip & coinAll);
        if (skip) {
            s.coins |= skip;
            std::printf("coins: 0x%x not a goal this run (--coinskip)\n", (unsigned)skip);
        }
    }
    // Does the route need afterStep on every step of every state? A search that steps without it
    // (montecarlo's rollouts) cannot follow such a route, and declines the call.
    bool needsAfterStep() const { return coinOn; }

    // ---- --coins: collect, then kill whoever left one behind --------
    // A child `kid` of `s` (pressed or not: `input`) has just stepped through the group's tick (`K`,
    // frame `gframe`; the coins where they are now are K.coins, the group's list when it was asked
    // for -- GroupOpts::coins). Credit what it touched, and set `dead` if it can no longer collect
    // what --coins asks for. Runs on the worker threads.
    void afterStep(const State& s, Child& kid, int input, const StepCtx& K, int gframe, long long t,
                   bool& dead) const {
        static const std::vector<CoinLive> kNoCoins;
        const std::vector<CoinLive>& coinLive = K.coins ? *K.coins : kNoCoins;
        auto& L = c.L;
        auto& passedOn = c.passedOn;
        auto& tapGive = c.tapGive;
        auto& tapGateBits = c.tapGateBits;
        auto& gatedMask = c.gatedMask;
        auto& tapMissX = c.tapMissX;
        auto& tapMissY = c.tapMissY;
        auto& coinGate = c.coinGate;
        auto& tapSole = c.tapSole;
        auto& coinEnable = c.coinEnable;
        auto& coinReq = c.coinReq;
        auto& coinPruneOk = c.coinPruneOk;
        // After the step and after any rotation, so the position tested is
        // the one the tick ended at, in the frame the coin's world
        // coordinates mean something. In the frame coinLive was built in
        // (the group's): frame 0 for every coin, a turned frame only for
        // the coins that never move (coinAnyFrame), whose rows are turned
        // into it. A tick that turns the player skips the test, as before.
        if (coinOn && !dead && kid.s.frame == gframe) {
            const double ph =
                hazardHalfFor(kid.s.mode, kid.s.mini != 0);
            const double px = (double)kid.s.xAbs, py = (double)kid.s.y;
            // COLLECTING reads the live rows (coinLive): a coin its group
            // has moved is tested where it is now, and one the group has
            // switched off is not in the list at all -- GD credits neither
            // a moved coin at its old place nor a disabled one.
            for (const CoinLive& lc : coinLive) {
                const uint8_t bit = (uint8_t)(1u << lc.bit);
                if (kid.s.coins & bit) continue;
                // --coindbg: how close anything got. Recorded BEFORE the
                // collect test and only inside the x bound, so a coin that
                // is never collected still says which of the three reasons
                // it was (see g_coinNearDy).
                // (frame 0 only: the probe's x is a world x)
                if (g_coinDbgT >= 0 && gframe == 0 && px > g_coinProbeMaxX)
                    g_coinProbeMaxX = px;
                if (g_coinDbgT >= 0 && gframe == 0
                    && std::fabs(px - lc.cx) <= lc.hw + ph) {
                    ++g_coinSeen[lc.bit];
                    const double dy = std::fabs(py - lc.cy);
                    double& best = g_coinNearDy[lc.bit];
                    if (best < 0.0 || dy < best) {
                        best = dy;
                        g_coinNearX[lc.bit] = px;
                        g_coinNearY[lc.bit] = py;
                        g_coinNearT[lc.bit] = (long long)t;
                    }
                }
                // Touching counts (measured: exactly on the boundary is
                // credited), hence <= and not <.
                // The margin is an allowance for THIS model's own error, so
                // it must never eat the window it is protecting: a wave's
                // coin bound is 25 px against a cube's 35, and the learnt 8
                // takes a third of it -- measured on lv21, where the search
                // then had no route left it could call a collection. A
                // quarter of the bound is the cap.
                const double mx = std::min(kCoinMargin, (lc.hw + ph) * 0.25);
                const double my = std::min(kCoinMargin, (lc.hh + ph) * 0.25);
                if (std::fabs(px - lc.cx) <= lc.hw + ph - mx
                    && std::fabs(py - lc.cy) <= lc.hh + ph - my) {
                    // --coindbg: WHERE the model decided it had one, once
                    // per coin. "The game refused a coin the plan claimed"
                    // names the coin and nothing else, so without this the
                    // claim's own position is only inferable -- and the two
                    // sides were compared at different ticks twice before.
                    if (g_coinDbgT >= 0 && !g_coinSaid[lc.bit]) {
                        g_coinSaid[lc.bit] = 1;
                        // ...with vy and the mode, because the next question
                        // this line gets asked is always "does the game
                        // survive from there", and that is an injection
                        // which needs the whole state. Without vy the
                        // probe has to guess it, and a cube dropped at
                        // this coin with vy=0 is inside the spikes three
                        // ticks later -- which says nothing about the plan.
                        std::printf("coincollect: t=%lld coin=%d player=(%.3f,%.3f)"
                                    " vy=%.4f mode=%d mini=%d frame=%d"
                                    " live=(%.3f,%.3f) |dx|=%.3f |dy|=%.3f"
                                    " bound=(%.3f,%.3f)\n",
                                    (long long)t, lc.bit, px, py,
                                    (double)kid.s.vy, (int)kid.s.mode,
                                    (int)kid.s.mini, gframe, lc.cx, lc.cy,
                                    std::fabs(px - lc.cx), std::fabs(py - lc.cy),
                                    lc.hw + ph - mx, lc.hh + ph - my);
                    }
                    kid.s.coins |= bit;
                }
            }
            // MISSING is judged on the load-time x instead, for the two
            // reasons the live row cannot serve: a switched-off coin has no
            // live row at all (and must still be counted as passed), and
            // the bound has to be the same one the game side uses to end an
            // attempt (hooks_gamelayer.cpp). Every coin this corpus moves is
            // moved in y only, so the two agree; a coin carried in x would
            // need its own bound and does not exist here.
            //
            // ...AND ONLY WHERE THE PLAYER CANNOT COME BACK. The whole
            // verdict rests on x growing for ever, which a level with
            // rotated gameplay breaks outright: lv22 travels -x through its
            // maze and its third coin sits at x=16,097, inside it. There the
            // prune would kill a lineage that has merely not turned round
            // yet. Without it the search only loses the early exit -- the
            // goal still is not reached without every coin.
            // `dead` and no early return: the bookkeeping below (flags,
            // key, the dbg reason) is the same for this death as for a
            // physical one, and duplicating it is how the two drift.
            // ---- the PICKUP ITEMS, and the Count triggers they fire -----
            // Same test as a coin's (the player's own box against the
            // object), and state for the same reason: lv21's third coin
            // only comes into reach once ten of item 1 have been taken.
            // The trigger they fire takes a bit in the SAME mask a touch
            // box does, so the chain, the fire tick and the dedupe key are
            // machinery that already exists.
            //
            // AND THE COUNT LOOP BELOW LIVES INSIDE THIS BLOCK, which was
            // opened only when there are pickups. It was written for lv21,
            // whose gate is fed by collectibles; on a level whose counter is
            // fed by CHAINS instead there are no pickups at all, the block
            // is skipped, and no Count trigger is ever asked anything. That
            // is lv22's first coin exactly: its three item-2 rows all have
            // pickup=0, g_collect is empty, and `item 2 == 5` never fired
            // even with every feeder forced -- a --coindbg print inside the
            // loop produced zero lines for any Count on that level. Opened
            // for either source now; the pickup loop just below is a no-op
            // when g_collect is empty, so a level with neither is unchanged
            // and a level with pickups takes the same path as before.
            // Frame 0 only, as everything in this block was before the coin
            // collect above learnt the turned frames: the pickups are world
            // rows, and the counters are asked where they always were.
            // The exception is an Item Compare gate (cmode 3): it reads a
            // count and nothing else, and lv22's third coin is collected in
            // frame 3, where an anchored call has to open it from GD's own
            // counter (--itembase) or never see the coin at all.
            // (The gates themselves are the model's now -- Model::fireGates, which the search
            // calls on the child before this; neither the coin test above nor the prunes below
            // write what the other reads.)
            // A gated coin passed for good (gatedMask, tapMissX/Y):
            // once its window has resolved, past the coin's far edge on the
            // shut channel without it. Same reading of "passed" as the
            // window's own ends.
            if (!dead && gatedMask
                && ((kid.s.trig & tapGateBits) || (kid.s.taps & 0x40))) {
                double wx = 0.0, wy = 0.0;
                fromFrame(gframe, (double)kid.s.xAbs, (double)kid.s.y, wx, wy);
                for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                    if (!((gatedMask >> ci) & 1) || (kid.s.coins & (uint8_t)(1u << ci)))
                        continue;
                    if (passedOn(kid.s, gframe, tapGive.closeChan, wx, wy,
                                 tapMissX[ci], tapMissY[ci])) {
                        dead = true;
                        break;
                    }
                }
            }
            // ...a coin whose group is OFF and whose every switch-on box is
            // behind the player (coinGate, built at load). "Off right now"
            // is read from the live rows: a switched-off coin is not in
            // them. Nothing to learn from flying the rest of the level.
            // Frame 0 only, and for the same reason as the miss test: `px`
            // is a world Y in a turned frame, and coinLive is empty there,
            // so every coin would read as "off, and the switch-ons are all
            // behind" and be killed on a comparison between two axes.
            for (size_t gi = 0; gframe == 0 && gi < coinGate.size(); ++gi) {
                const CoinOnGate& G = coinGate[gi];
                if (kid.s.coins & (uint8_t)(1u << G.coinIdx)) continue;
                bool liveNow = false;
                for (const CoinLive& lc : coinLive)
                    if (lc.bit == G.coinIdx) { liveNow = true; break; }
                if (liveNow) continue;
                bool canStill = false;
                for (const auto& on : G.ons)
                    if ((kid.s.trig & on.first) || px <= on.second) {
                        canStill = true;
                        break;
                    }
                if (!canStill) { dead = true; break; }
            }
            // ...and a coin that only an Item Compare on the counting tap's
            // item switches on, once that tap's window has shut with the
            // gate unfired: the Stop at its end halts the tap itself (lv22's
            // uid17958 sits in the Stop's group 535), nothing else feeds the
            // item (tapSole), and the gate reads only the count -- so the
            // coin stays off for good. Without this the branches that
            // pressed too few times fly on to the coin and only the goal
            // test tells them apart, a whole frontier later.
            if (!dead && tapSole && (kid.s.taps & 0x40)
                && !(kid.s.trig & tapGateBits))
                for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                    if (!coinEnable[ci] || (coinEnable[ci] & ~tapGateBits)) continue;
                    if (kid.s.coins & (uint8_t)(1u << ci)) continue;
                    dead = true;
                    break;
                }
            // ...and the COUNTER a coin needs (coinReq, built at load).
            // A counter never decreases, so once `have + ahead < need` the
            // coin can never come into reach again and the branch is done.
            // This is the prune the miss test could not be: it fires at the
            // last feeder the branch could still have taken, not at the
            // coin, which on lv21 is 2,759 px and a whole cap's worth of
            // frontier earlier.
            for (size_t ri = 0; gframe == 0 && ri < coinReq.size(); ++ri) {
                const CoinCountReq& R = coinReq[ri];
                if (kid.s.coins & (uint8_t)(1u << R.coinIdx)) continue;
                int have = 0, ahead = 0;
                for (const auto& ib : g_itemBase)
                    if (ib.first == R.item) have += ib.second;
                for (size_t ii = 0; ii < g_collect.size() && ii < 16; ++ii) {
                    if (g_collect[ii].item != R.item) continue;
                    if (kid.s.items & (uint16_t)(1u << ii)) ++have;
                    else if (g_collect[ii].cx > px) ++ahead;
                }
                if (have + ahead < R.need) { dead = true; break; }
            }
            // ONLY IN FRAME 0, and that is not a detail: px is the frame's
            // travel coordinate, so in a turned frame it is a world Y and
            // comparing it with the coin's world x kills branches for a
            // reason that does not exist. The old form could skip the guard
            // because it only ran on levels that never turn.
            for (size_t ci = 0; !dead && gframe == 0 && ci < L.coins.size(); ++ci) {
                if (!coinPruneOk[ci]) continue;
                if (kid.s.coins & (uint8_t)(1u << ci)) continue;
                const Obj& oc = L.coins[ci];
                // x only ever grows in frame 0, and coinPruneOk says every
                // later return to frame 0 is past this coin, so it is final.
                if (px > oc.cx + oc.hw + ph) { dead = true; break; }
            }
        }
    }
};

// ---- what a search answers ------------------------------------------------------------------------
// One shape for every search. Solved: a plan to the goal (or to where the caller asked it to stop --
// a horizon, the memory limit, a join onto an old plan), rebuilt from `goal`. Partial: no goal, but
// the caller asked for the deepest branch (a horizon search), rebuilt from `goal`. NoPlan: nothing
// to emit. `t` / `x` are how far the frontier got -- for NoPlan, where it got stuck, which is what
// the caller's fallback (the section search, repair.hpp) reads.
// Declined: this search does not take this call (a level it cannot walk) and has touched nothing;
// the caller's default search answers instead. `deterministic` is false when the search stopped on
// something other than its own budget (a wall-clock safety valve), so a comparison can leave it out.
struct SearchResult {
    enum Kind : uint8_t { Solved, Partial, NoPlan, Declined };
    Kind kind = NoPlan;
    long long t = -1;
    double x = -1.0;
    State goal{};
    bool deterministic = true;
    // The plan, Solved or Partial: the input level in effect at each tick from t0 + 1 up to the
    // goal (inputs[i] is tick t0 + i + 1). cliMainOnce reads the plan from here and nothing else of
    // the method's, so how a method keeps its states (the arena, planFromArena below) is its own.
    std::vector<uint8_t> inputs;
    // NoPlan: the first states (up to 12) of the last layer the frontier had, for the FAILED lines.
    std::vector<State> lastLayer;
};

// The layer searches' own frontier and dedupe scratch (searchLayers / searchBeam make one each).
// cliMainOnce used to declare these and lend them to every method; a method that is not a layer
// search never touched them.
struct LayerWork {
    std::vector<State> cur, nxt;
    CellMap seen;
    std::vector<ShardMap> shards;   // parallel dedupe (KeyRec, ShardMap above)
    std::vector<uint64_t> kidKeys;  // compact copy of kids[i].key (8 B strides
                                    // instead of pulling the whole Child in)
    std::vector<uint8_t> kidFlag;   // 0 = not expanded, 1 = dead, 2 = alive
    // per-ordinal slot events, written race-free (one shard owns each key):
    // evKind[i] = 0 none / 1+shard; evRec[i] = record index within that shard.
    // An ordinal is an 'a' event iff recs[evRec[i]].aOrd == i.
    // Cleared by the merge scan itself (every set entry is visited).
    std::vector<uint8_t> evKind;
    std::vector<uint32_t> evRec;
    std::vector<Child> kids;      // reused every layer
    std::vector<ClearSample> clearKids;   // --clearprobe only, reused too
    std::vector<size_t> gidx;     // this speed group's indices into `cur`
    std::vector<uint8_t> kidMoot;   // --twinaudit: per parent of the group, predicted twin
    // next arena size that triggers a GC (doubles after each compaction)
    size_t gcNext = g_gcNodes > 0 ? g_gcNodes : (size_t)-1;
    long long nBornPrev = 0, nDiedPrev = 0;   // last layer's expansion counters
    size_t prevAlive = 0;
    // The frontier starts at the root state -- the call's init, after every amendment.
    explicit LayerWork(const State& root) { cur.push_back(root); }
};

// Where a search method keeps its states (searchLayers / searchBeam / searchMontecarlo make one
// each): every node records its parent and the input that made it, and the plan is read back from
// the goal's parent (planFromArena above). Node 0 is the root.
// arenaMode is the mode of the state each node was created for, parallel to `arena` and ONLY filled
// when the checkpoint channel has a subscriber (dp/progress.hpp). A checkpoint's `input=` edges
// have to carry the plan writer's per-mode latency, and the writer reads that off `modeAt`, which
// does not exist until the witness resim runs at the end of the call. The lineage's own modes are
// the same sequence and they are available now. Costs one byte per node while a subscriber is
// listening and nothing at all otherwise -- which is why it is a side vector and not three spare
// bits of Node: Node's parent field is 31 bits wide and the arena has been measured at 147.9M
// nodes. No arena index leaves the method: a checkpoint carries its lineage as edges.
struct SearchArena {
    std::vector<Node> arena;
    std::vector<uint8_t> arenaMode;
    std::vector<uint8_t> arenaLossy;   // --lossystat (thread_pool.hpp): 1 = kept out of a thinned cap class, or a descendant of one; only filled under the flag
    explicit SearchArena(bool checkWanted, bool lossyWanted = false) {
        arena.push_back(Node{0});
        if (checkWanted) arenaMode.push_back(0);   // the root, which no state owns
        if (lossyWanted) arenaLossy.push_back(0);
    }
};

// --lossystat: the line after a search (cliMainOnce prints it beside capstat:). Nothing without the flag.
inline void lossyStatPrint() {
    if (!g_lossyStat) return;
    std::printf("lossystat: firstCapT=%lld lastCapT=%lld capLayers=%lld", g_lossy.firstCapT, g_lossy.lastCapT, g_lossy.capLayers);
    const std::vector<LossyStat::Row>& v = g_lossy.tail;
    if (!v.empty()) {
        const size_t n = v.size();
        for (size_t back : {(size_t)0, (size_t)9, (size_t)49, (size_t)99, (size_t)255}) {
            if (back >= n) break;
            const LossyStat::Row& r = v[n - 1 - back];
            std::printf(" end-%zu(t=%lld alive=%zu lossy=%zu pre=%zu)", back, r.t, r.alive, r.lossy, r.pre);
        }
        // the cap's pressure over the last w layers: how many it thinned and the mean of pre / alive over them
        for (size_t w : {(size_t)10, (size_t)50, (size_t)255}) {
            const size_t m = std::min(w, n);
            size_t thinned = 0;
            double ratio = 0.0;
            for (size_t k = 0; k < m; ++k) {
                const LossyStat::Row& r = v[n - 1 - k];
                if (r.pre > r.alive) ++thinned;
                ratio += r.alive ? (double)r.pre / (double)r.alive : 1.0;
            }
            std::printf(" win%zu(thinned=%zu/%zu pre/alive=%.2f)", w, thinned, m, ratio / (double)m);
        }
    }
    std::printf("\n");
}

// A method that keeps its states in the arena (searchLayers / searchBeam / searchMontecarlo) reads
// its plan off it: from the goal's parent back to the root, then reversed.
inline void planFromArena(const std::vector<Node>& arena, const State& goal,
                          std::vector<uint8_t>& inputs) {
    inputs.clear();
    for (uint32_t i = goal.parent; i != 0; i = arena[i].parent()) inputs.push_back(arena[i].action());
    std::reverse(inputs.begin(), inputs.end());
}

// E9 probe (observation only, gd-02 11-16, from gd-07's patch4): the lineage of a state the shadow declined, as `input=tick,v` edges (zero latency), to GD07_LINEAGE_LOG
inline const std::vector<Node>* g_probeArena = nullptr;
// the arena of the search running now, set for the length of the search (a search that ends hands the previous one back: a dump outside a live search -- the shadow stepped from a model seed, a reference pass --
// has no parent chain and says so instead of reading a dead arena)
struct ProbeArenaScope { const std::vector<Node>* prev; explicit ProbeArenaScope(const std::vector<Node>* a) : prev(g_probeArena) { g_probeArena = a; } ~ProbeArenaScope() { g_probeArena = prev; } };
inline void probeDumpLineage(const State& s, int input, long long t, int why, int uid) {
    static const char* path = std::getenv("GD07_LINEAGE_LOG");
    static std::mutex mu; static int nDump = 0; static std::set<long long> bucket;
    if (!path || !g_probeArena) return;
    std::lock_guard<std::mutex> lk(mu);
    if (nDump >= 60 || !bucket.insert(t / 40 + (long long)why * 100000000LL).second) return;
    std::vector<uint8_t> in;
    const bool inSearch = g_probeArena->size() > s.parent && g_probeArena->size() > 1;
    if (inSearch) for (uint32_t i = s.parent; i != 0 && i < g_probeArena->size(); i = (*g_probeArena)[i].parent()) in.push_back((*g_probeArena)[i].action());
    std::reverse(in.begin(), in.end()); in.push_back((uint8_t)input);
    const long long t0 = t - (long long)(in.size() - 1);
    if (std::FILE* f = std::fopen(path, "a")) {
        std::fprintf(f, "# dump %d t %lld t0 %lld mode %d frame %d x %.3f y %.3f why %d uid %d n %zu parent %u arena %zu\n", nDump, t, t0, (int)s.mode, (int)s.frame, (double)s.xAbs, (double)s.y, why, uid, in.size(), (unsigned)s.parent, g_probeArena->size());
        uint8_t prev = 0;
        for (size_t k = 0; k < in.size(); ++k) { if (in[k] != prev) { std::fprintf(f, "input=%lld,%d\n", t0 + (long long)k, (int)in[k]); prev = in[k]; } }
        std::fprintf(f, "# end %d\n", nDump); std::fclose(f);
    }
    ++nDump;
}

// --ladderprefix: a cap ladder's attempt k+1 starts from attempt k's layer loop where the two
// attempts can no longer differ. Inside a ladder the attempts' arguments differ only in --capmap,
// and a layer reads the map in exactly two places, capAtX(bestX) and gridAtX(bestX), both with
// the leader x the layer starts with. So attempt k logs that x per layer (layerX); once attempt
// k+1's map is known, the first layer whose cap under it differs is t* (cliMain), every layer
// before t* is the same in both attempts, and attempt k+1 starts from the last state saved at a
// boundary T < t*. A save holds everything the layer loop carries from one layer to the next:
// the frontier, the arena's size (the arena itself is kept, append-only; a GC after a save
// renumbers it and drops the saves), the loop's counters and the outcome it accumulates, the
// globals it writes (autonomous and rotation triggers, statistics, the rejoin node), and the
// moving geometry's carried placement per bound frame. --ladderprefixcheck (cliMain) runs such an
// attempt both ways and compares plan, trace and outcome.
// What a search writes into an AutoTrig: model.hpp fireAutoTriggers, the lock window in dynamics.hpp, the
// witness and rollout walks. The rest of it (the controls, toggles and stops: ~1 GB on a level with a
// hundred thousand objects) is built once per call by loadAutoTriggers and never changes, so a save keeps
// these four and the resumed attempt, which loaded the same triggers, gets them back.
struct AutoFireState {
    int fireT = -1;
    double fireX = 0.0;
    int fireSpreadT = 0;
    double lockLastX = 0.0;
};
// A group's turn of a layer by the arena nodes its children got: [a0, a1) -> the key the group's trigger timeline is stored under (searchLayers' wrPrev / wrCur).
struct LayerWorldRange { uint32_t a0, a1; uint64_t key; };
struct LadderPrefixSnap {
    long long T = -1;
    std::vector<State> cur;
    TrigLive::SavedLayer tl;             // the layer's trigger timelines (trigshadow_live.hpp saveLayer): the resumed attempt goes on with them instead of replaying one
    std::vector<LayerWorldRange> wr;     // ...and which of them each state of `cur` was stepped in (the ranges of the nodes layer T made)
    std::vector<uint64_t> objRefs;   // the frontier's Obj pointers, 12 per state (Model::encodeObj)
    gdcol::SlDiffRun sl;   // --shadowstep: the frontier's shadow blocks by value, as words that differ from the first (the store keeps only the live layer); empty without the flag
    gdcol::SlDiffRun sl2;  // ...and player 2's (State::slRef2), which a dual level's states carry as well
    size_t arenaSize = 0, arenaModeSize = 0;
    size_t gcNext = 0;
    long long nBornPrev = 0, nDiedPrev = 0;
    size_t prevAlive = 0;
    long long bestT = 0;
    double bestX = 0;
    State goalState{};
    State bestAnyS{};              // --lpbestany (removed): the furthest-in-x layer so far (partialbyx)
    double bestAnyX = -1e18;
    long long bestAnyT = -1;
    bool haveAny = false;
    State physS{};                 // --partialphys: the parent of the latest physical death (physT = the tick of its child)
    long long physT = -1;
    bool havePhys = false;
    long long lyPhysPrev = 0, lyPrunePrev = 0;   // --partialphys: the last layer's deaths by kind
    bool solved = false;
    size_t maxAlive = 0;
    long long capDropped = 0, capHits = 0, cellCuts = 0, cellSpared = 0, cellTwinsKept = 0;
    SearchCensus census;
    TwinStats twin;
    long long workStates = 0;
    std::vector<AutoFireState> autoFire;   // per g_autoTrig entry
    std::vector<RotTrig> rotTrig;
    std::vector<std::vector<double>> carry;   // per frame 0..3, empty when not bound
    std::vector<uint8_t> carryOn;
    long long gfireGroups = 0, gfireSpread = 0, gfireSum = 0;
    int gfireMax = 0;
    int groupXSplitMax = 0;
    long long rjJoinT = -1;
    uint32_t rjOldNode = 0xffffffffu;
    size_t layerXSize = 0;
    // What this save holds allocated, for the cap on all of them (g_ladderPrefixMaxMiB).
    size_t bytes() const {
        size_t n = sizeof(*this) + cur.capacity() * sizeof(State) + objRefs.capacity() * sizeof(uint64_t) + sl.bytes() + sl2.bytes()
                   + autoFire.capacity() * sizeof(AutoFireState) + rotTrig.capacity() * sizeof(RotTrig)
                   + carry.capacity() * sizeof(std::vector<double>) + carryOn.capacity()
                   + g_trigLive.savedBytes(tl) + wr.capacity() * sizeof(LayerWorldRange);
        for (const std::vector<double>& c : carry) n += c.capacity() * sizeof(double);
        return n;
    }
};
struct LadderPrefixCache {
    bool active = false;        // this call is a ladder attempt under --ladderprefix (cliMain)
    long long t0 = -1;          // the anchor the saves were made from
    std::vector<double> layerX; // the leader x each layer t0+1.. started with (the map's argument)
    std::vector<LadderPrefixSnap> snaps;
    std::vector<Node> arena;    // the last attempt's arena (append-only between its saves)
    std::vector<uint8_t> arenaMode;
    bool usable = false;        // the last attempt ended normally, so its saves can be resumed
    int resume = -1;            // index into snaps to start this attempt from (cliMain), -1 = t0
    long long resumedFrom = -1; // what this call started from, for the account
    // ...and at most this much in all (--ladderprefixmaxmib, read by cliMain), oldest dropped first; the newest
    // save is always kept. A save is one frontier plus the loop's counters, a few MiB at a frontier of
    // thousands of states, so 64 of them sit well under the default; the cap is there so that whatever a
    // save grows to hold can never again run the process out of memory unseen (the search's own budget,
    // g_memLimitMiB, does not count them).
    size_t maxMiB = 1024;
    bool capSaid = false;       // the over-the-cap line has been printed for this ladder
};
inline LadderPrefixCache g_lpc;            // --ladderprefix / --ladderprefixcheck are read by cliMain
inline int g_ladderPrefixEvery = 64;       // save every N layers
inline size_t g_ladderPrefixMaxSnaps = 64; // and keep at most this many (oldest dropped)

// The search's layer loop: from t0 + 1 until the plan is solved or tEnd is reached, step every
// state of the frontier through the model (model.hpp), dedupe the children, cap the frontier. `e`
// is cliMainOnce's SearchEnv -- references to what the loop shares with that call, in two parts:
// e.in (inputs) and e.outcome (what the call reads back); the plan goes back in out.inputs. The
// arena and the frontier and dedupe scratch are the loop's own (SearchArena, LayerWork above);
// `rules` is where it is going (CurrentRules above); `out` is the answer (SearchResult above).
// Returns -1 when the loop has ended, or the code cliMainOnce exits with (3: cancelled / stopped).
template <class E, class Rules>
inline int searchLayers(E& e, const Rules& rules, SearchResult& out) {
    auto& t0 = e.in.t0;
    auto& dbgLayers = e.in.dbgLayers;
    auto& init = e.in.init;
    auto& horizon = e.in.horizon;
    auto& planCut = e.in.planCut;
    auto& goalX = e.in.goalX;
    auto& tEnd = e.in.tEnd;
    LayerWork work(init);   // the frontier starts at the root
    auto& cur = work.cur;
    auto& nxt = work.nxt;
    // The arena nodes of this layer's states that exist only because a hold is carried towards a ring (heldRingInReach, below): each rides with the released child of
    // the same parent at the alive cap instead of being counted there. In node order (the layer is made in it), cleared when the cap has read it.
    std::vector<uint32_t> rideNodes;
    const bool lossyOn = g_lossyStat;   // --lossystat: observation only, the arena's side vector below
    SearchArena store(e.in.checkWanted, lossyOn);   // this method's states
    auto& arena = store.arena; ProbeArenaScope probeScope(&store.arena);
    auto& checkWanted = e.in.checkWanted;
    auto& arenaMode = store.arenaMode;
    auto& arenaLossy = store.arenaLossy;
    // The groups of the last layer by the arena nodes their children got: [a0, a1) -> the key the group's trigger timeline is stored under. A state's own node tells which timeline it was stepped in
    // (trigshadow_live.hpp GroupIn::world). wrCur is the layer being made; a layer that does not follow the last one starts with none.
    using WorldRange = LayerWorldRange;
    std::vector<WorldRange> wrPrev, wrCur;
    long long wrT = -2;
    // (a group's nodes are the ones the arena gets during its turn, by either dedupe path: the range is opened when the turn starts and closed when the next one starts, the arena is compacted or the layer ends)
    size_t wrA0 = 0; bool wrOpen = false; uint64_t wrOpenKey = 0;
    auto wrClose = [&]() {
        if (wrOpen && arena.size() > wrA0) wrCur.push_back(WorldRange{(uint32_t)wrA0, (uint32_t)arena.size(), wrOpenKey});
        wrOpen = false;
    };
    g_lossy = LossyStat{};
    auto& seen = work.seen;
    auto& shards = work.shards;
    auto& kidKeys = work.kidKeys;
    auto& kidFlag = work.kidFlag;
    auto& evKind = work.evKind;
    auto& evRec = work.evRec;
    auto& model = e.in.model;
    auto& kids = work.kids;
    auto& clearKids = work.clearKids;
    auto& gidx = work.gidx;
    auto& kParallelMin = e.in.kParallelMin;
    auto& pool = e.in.pool;
    auto& bestT = e.outcome.bestT;
    auto& bestX = e.outcome.bestX;
    auto& goalState = e.outcome.goalState;
    auto& solved = e.outcome.solved;
    auto& maxAlive = e.outcome.maxAlive;
    auto& capDropped = e.outcome.capDropped;
    auto& capHits = e.outcome.capHits;
    auto& cellCuts = e.outcome.cellCuts;
    auto& cellSpared = e.outcome.cellSpared;
    auto& cellTwinsKept = e.outcome.cellTwinsKept;
    auto& census = e.outcome.census;
    auto& twin = e.outcome.twin;
    auto& kidMoot = work.kidMoot;
    auto& gcNext = work.gcNext;
    auto& nBornPrev = work.nBornPrev;
    auto& nDiedPrev = work.nDiedPrev;
    auto& prevAlive = work.prevAlive;
    auto& curDxF = e.in.curDxF;
    auto& ppMark = e.in.ppMark;
    if (g_layerProf.on()) g_layerProf.beginSearch(layerProfHeader());   // --layerprof (observation only)
    if (g_stateCross) stateCrossPrepare();   // --statecross (search_key.hpp)
    // --ladderprefix: only inside a ladder, and not where the loop talks to the outside or
    // prints per layer (the checkpoint channel, the reference watch, the census, the band file).
    const bool lpOn = g_lpc.active && !checkWanted && !g_refWatch && g_searchCensus == 0
                      && g_bandPath.empty() && !lossyOn;
    long long tStart = t0 + 1;
    g_lpc.resumedFrom = -1;
    const long long lpWork0 = g_outcome.workStates;   // what the call had before this loop
    if (lpOn && g_lpc.usable && g_lpc.t0 == t0 && g_lpc.resume >= 0
        && (size_t)g_lpc.resume < g_lpc.snaps.size()
        && g_lpc.snaps[(size_t)g_lpc.resume].autoFire.size() == g_autoTrig.size()) {
        LadderPrefixSnap S = g_lpc.snaps[(size_t)g_lpc.resume];
        cur = S.cur;
        for (size_t i = 0; i < cur.size(); ++i) {   // the pointers into THIS attempt's level
            const uint64_t* r = &S.objRefs[i * 12];
            State& s = cur[i];
            s.snapObj = model.decodeObj(r[0]);
            s.usedOrb = model.decodeObj(r[1]);
            s.snapObj2 = model.decodeObj(r[2]);
            s.usedOrb2 = model.decodeObj(r[3]);
            for (int k = 0; k < 4; ++k) s.usedPad[k] = model.decodeObj(r[4 + k]);
            for (int k = 0; k < 4; ++k) s.usedPad2[k] = model.decodeObj(r[8 + k]);
            // --shadowstep: the save's blocks go back into the store (the handles the save holds are long retired)
            gdcol::ShadowLite b;
            s.slRef = (gdcol::g_slStore && S.sl.get(i, b) && (b.flags & gdcol::kSlValid)) ? gdcol::g_slStore->put(b) : 0;
            // ...and player 2's: the handle the save's copy of the state holds is as retired as slRef's (left alone, it named whichever block the store gave that slot since, and a dual state's second body
            // stepped from a stranger's: --ladderprefixcheck lv16 t0=3851 attempt 2, resumed after t=7050, said MISMATCH where the same attempt from t0 did not)
            s.slRef2 = (gdcol::g_slStore && S.sl2.get(i, b) && (b.flags & gdcol::kSlValid)) ? gdcol::g_slStore->put(b) : 0;
        }
        g_lpc.arena.resize(S.arenaSize);
        arena.swap(g_lpc.arena);
        g_lpc.arenaMode.resize(S.arenaModeSize);
        arenaMode.swap(g_lpc.arenaMode);
        if (S.tl.have) {   // the layer's trigger timelines and the nodes that name them: layer T + 1 follows layer T as in the attempt that made the save
            g_trigLive.restoreLayer(S.tl);
            wrPrev.clear(); wrCur = S.wr; wrT = S.T; wrOpen = false;
        }
        work.gcNext = S.gcNext;
        nBornPrev = S.nBornPrev;
        nDiedPrev = S.nDiedPrev;
        prevAlive = S.prevAlive;
        bestT = S.bestT;
        bestX = S.bestX;
        goalState = S.goalState;
        solved = S.solved;
        maxAlive = S.maxAlive;
        capDropped = S.capDropped;
        capHits = S.capHits;
        cellCuts = S.cellCuts;
        cellSpared = S.cellSpared;
        cellTwinsKept = S.cellTwinsKept;
        census = S.census;
        twin = S.twin;
        g_outcome.workStates = lpWork0 + S.workStates;   // the loop's own work up to T
        for (size_t i = 0; i < g_autoTrig.size(); ++i) {   // the entries are this attempt's own; the fire state is the save's
            AutoTrig& A = g_autoTrig[i];
            const AutoFireState& F = S.autoFire[i];
            A.fireT = F.fireT;
            A.fireX = F.fireX;
            A.fireSpreadT = F.fireSpreadT;
            A.lockLastX = F.lockLastX;
        }
        g_rotTrig = S.rotTrig;
        for (int f = 0; f < 4; ++f)
            if (S.carryOn[(size_t)f]) model.loadWorld(f, S.carry[(size_t)f], S.T);
        g_gfireGroups = S.gfireGroups;
        g_gfireSpread = S.gfireSpread;
        g_gfireSum = S.gfireSum;
        g_gfireMax = S.gfireMax;
        g_groupXSplitMax = S.groupXSplitMax;
        g_rjJoinT = S.rjJoinT;
        g_rjOldNode = S.rjOldNode;
        g_lpc.layerX.resize(S.layerXSize);
        g_lpc.snaps.resize((size_t)g_lpc.resume + 1);   // later saves belong to the old attempt
        tStart = S.T + 1;
        g_lpc.resumedFrom = S.T;
        std::printf("ladderprefix: attempt resumes after t=%lld (%lld layers not searched again)\n",
                    S.T, S.T - t0);
    } else if (lpOn) {
        g_lpc.layerX.clear();
        g_lpc.snaps.clear();
    }
    g_lpc.usable = false;
    g_lpc.arena.clear();
    g_lpc.arenaMode.clear();
    g_lpc.t0 = t0;
    bool lpBound = capHits > 0;   // save only once this attempt's cap has bound (a call that never binds never climbs)
    // --partialbyx: the layer whose leader got furthest in x, over the whole search (the Partial answer below). The last non-empty layer is the one that lived longest in TICKS,
    // which is not the one that got furthest: lv22 from t=565, a cube climbing out of the level (vy 15 up to maxplayy 3675, alive 1000 ticks longer) outlived the lane that
    // reached x=5229 by t=2820, and the loop replayed the climb for six iterations.
    State bestAnyS{};
    double bestAnyX = -1e18;
    long long bestAnyT = -1;
    bool haveAny = false;
    State physS{};
    long long physT = -1;
    bool havePhys = false;
    long long lyPhysPrev = 0, lyPrunePrev = 0;
    if (g_partialPhys && g_lpc.resumedFrom >= 0 && g_lpc.resume >= 0 && (size_t)g_lpc.resume < g_lpc.snaps.size()) {   // a resumed attempt carries the saved prefix's physical-death parent
        const LadderPrefixSnap& R = g_lpc.snaps[(size_t)g_lpc.resume];
        physS = R.physS; physT = R.physT; havePhys = R.havePhys; lyPhysPrev = R.lyPhysPrev; lyPrunePrev = R.lyPrunePrev;
    }
    // (always on: the switch --lpbestany is removed)
    // --lpbestany (removed): a save of the ladder's prefix keeps the furthest-in-x layer the attempt had so far (search_dp.hpp bestAnyS / bestAnyX / bestAnyT), and a resumed
    // attempt starts from it. Without it the resumed attempt looked for the furthest layer among the layers after the save only: --ladderprefixcheck on lv22's t0=857 call (--shadowframe) says MISMATCH for an
    // attempt resumed after t=2841, because the layer that got furthest (x=5229.1 at t=2820) lies before the save and the climbing lane that came after it (x=4445.9) was emitted instead.
    // On since the lv16-22 loop's 169 ladder calls (--ladderprefixcheck, --shadowstep=all) gave 3 MISMATCH with it and 6 without, the three the same ones in both.
    if (g_partialByX && g_lpc.resumedFrom >= 0 && g_lpc.resume >= 0 && (size_t)g_lpc.resume < g_lpc.snaps.size()) {   // a resumed attempt starts from the layer the saved prefix had got furthest with
        const LadderPrefixSnap& R = g_lpc.snaps[(size_t)g_lpc.resume];
        bestAnyS = R.bestAnyS; bestAnyX = R.bestAnyX; bestAnyT = R.bestAnyT; haveAny = R.haveAny;
    }
    for (long long t = tStart; t <= tEnd && !solved; ++t) {
        if (lpOn) g_lpc.layerX.push_back(bestX);   // the x this layer's capAtX / gridAtX read
        ppMark(6);
        if (g_layerProf.f) { double lpT[LayerProf::kMax]; const int lpN = layerProfTotals(lpT); g_layerProf.top(t, bestX, cur.size(), lpT, lpN); }   // --layerprof: the layer before this one is over
        nxt.clear();
        // ---- reference watch, part 1: where is it now (--refwatch) ---------
        // Locate the reference's PARENT in the frontier before the layer runs.
        // On the first layer it is looked up in `cur` directly; afterwards the
        // previous layer's check has already confirmed it is there.
        if (g_refWatch && g_refLostAt < 0) {
            if (t == t0 + 1) {
                // The trace's first row is t0+1: the state AT the anchor tick
                // is the anchor itself, which is what the frontier was seeded
                // with, so there is nothing to match. After this layer the
                // watch follows the state it is carrying, not the trace.
                g_refParent = cur.empty() ? -1 : 0;
            }
            if (g_refParent < 0 && t == t0 + 1) {
                // The anchor itself is not in the frontier. That is a fault in
                // the harness (wrong trace, wrong anchor tick), not a finding
                // about the model, and it must not read as one.
                std::printf("refwatch: the reference is not in the FIRST "
                            "frontier (t=%lld) -- wrong trace or wrong anchor, "
                            "not a search result\n", t - 1);
                g_refWatch = false;
            }
            if (t == t0 + 1)
                std::printf("refwatch: armed rows=%zu t0=%lld parent=%d "
                            "frontier=%zu\n",
                            g_refRows.size(), t0, g_refParent, cur.size());
            for (int a = 0; a < 2; ++a) {
                g_refKidFate[a] = -1;
                g_refKidWhy[a] = "";
                g_refKidKey[a] = 0;
            }
        }

        if (g_phaseProf) g_ppSubLast = std::chrono::steady_clock::now();   // ppSubMark's start for this layer's setup
        // Resolve autonomous fires from the frontier's leading edge. `cur`
        // holds states AT tick t-1, so the first layer whose lead x reaches
        // the trigger's x means the crossing happened at t-1 and the move
        // starts at t-1 + delay (measured). States behind the
        // lead see the move at most a tick or two early -- under 1 px at the
        // slide rates in play, where the recording-timeline error this
        // replaces was measured at 1,707 ticks (see AutoTrig).
        // The autonomous triggers the frontier's leader has crossed (model.hpp fireAutoTriggers).
        model.fireAutoTriggers(cur, t);
        if (g_stateCross) stateCrossSyncFire();   // the leader's dates, before any group overrides them
        ppSubMark(0);

        // ---- speed groups -------------------------------------------------
        // The frontier is no longer a single x. A state that took a speed
        // portal and one that flew past it are at the same TICK but at
        // different x, and the gap only grows (0.9 vs 1.1 is 0.316 px/tick;
        // on lv18 it reached 1,700 px). So the layer is processed once per
        // distinct speed: each group gets its own object windows, built from
        // that group's own x span, and its own dedupe map.
        // Within a group the x values differ only by stair snaps (sub-pixel to
        // a couple of px), so the windows stay as tight as the shared one was.
        // The previous scheme -- one shared accumulator, switched when ANY
        // state's y overlapped the portal -- is what put the search on
        // displaced geometry.
        // A separate `seen` per group also keeps two speeds from ever merging
        // into one cell, without putting dx in keyOf (which would repartition
        // every single-speed layer in the suite for nothing).
        // ...and by the touch-trigger mask for the same reason (see TouchTrig).
        // A state that flew through the box and one that flew under it disagree
        // about where a door IS, so they cannot share object windows and must
        // not merge in the dedupe. Levels without touch triggers keep exactly
        // one mask (0) and are partitioned exactly as before.
        // ...and by the gameplay FRAME (see RotTrig): two states that turned at
        // different ticks are in different worlds, so they cannot share object
        // windows any more than two speeds can.
        // ...and by REVERSE (id 2899) for the same reason again: a reversed
        // state's x runs the other way, so it cannot share a window with a
        // forward one. frame and rev are separate axes, so both go in the key.
        // NOT by the spent-gravity-portal mask, though it is carried per state
        // the same way `trig` is. What puts `trig` here is that it moves the
        // GEOMETRY: two masks need two sets of object windows. A spent portal
        // moves nothing -- both states see the same box, they only disagree
        // about whether it still fires -- so the mask has no business in a key
        // whose job is to choose windows, and an axis here would be paid for in
        // parent steps: gidx deliberately hands every parent to every group
        // (see the rebind note), so k masks in a layer means stepping each
        // parent k times for k identical children. It goes in keyOf instead,
        // where the separation is exactly as absolute and costs one xor.
        // The model's group key (model.hpp GroupKey), and the search's own split of it:
        // xc, --groupxsplit (removed)'s part, 0 without the flag (search_key.hpp) -- or, in a layer the x-crossing split
        // is made in, the index of that part and the state's two crossing counts in xcTab (below).
        struct GKey : GroupKey {
            uint16_t xc;
            bool operator==(const GKey& o) const {
                return GroupKey::operator==(o) && xc == o.xc;
            }
        };
        const bool sigOld = g_groupFire || g_stateCross;   // the signature the groups had before the game events: a level with no listener keeps its size and its contents (groupFireSig is not computed when neither flag is on)
        std::vector<uint64_t> curSig((sigOld || gdev::evChainOn()) ? cur.size() : 0);
        for (size_t i = 0; i < curSig.size(); ++i)
            curSig[i] = (sigOld ? (groupFireSig(cur[i], t) ^ (g_stateCross ? xcSig(cur[i], t) : 0)) : 0) ^ evSig(cur[i]);   // --statecross: what the members dated themselves; evSig: the game events they raised that an Event trigger listens for (0 on a level with none)
        auto sigOf = [&](size_t i) -> uint64_t { return curSig.empty() ? 0 : curSig[i]; };
        // --groupxsplit (removed): the members of each group in x order, a new part wherever the gap to the
        // previous member is wider than the threshold. Part 0 is the lowest x.
        std::vector<uint16_t> curXc;
        // ...only in a layer where every state is in frame 0 and runs forward. gidx hands a parent
        // of another frame to every group of this one (the note above); splitting changed that
        // (such a parent went to part 0 alone), and on lv22 -- whose layers carry turned frames --
        // the flag took the loop off d008ee2's route at the second iteration, the search's
        // chosen plan dying in its own resim in frame 1 (`resimdie=511@1758/spider/no-target
        // ... f1`). Where a turned or reversed frame is live the layer is processed as before.
        bool xsplitHere = cur.size() > 1;
        for (size_t i = 0; xsplitHere && i < cur.size(); ++i)
            if (model.progress(cur[i]).frame != 0 || model.progress(cur[i]).rev != 0) xsplitHere = false;
        if (xsplitHere) {
            curXc.assign(cur.size(), 0);
            std::vector<GKey> bases;
            std::vector<std::vector<size_t>> members;
            for (size_t i = 0; i < cur.size(); ++i) {
                const State& s = cur[i];
                const GKey b{model.groupKey(s, curDxF, sigOf(i)), 0};
                const size_t j = (size_t)(std::find(bases.begin(), bases.end(), b) - bases.begin());
                if (j == bases.size()) {
                    bases.push_back(b);
                    members.emplace_back();
                }
                members[j].push_back(i);
            }
            bool split = false;
            for (size_t j = 0; j < members.size(); ++j) {
                std::vector<size_t>& m = members[j];
                const double gap = 2.0 * kGroupWinMargin + std::fabs((double)bases[j].dx);
                std::sort(m.begin(), m.end(), [&](size_t a, size_t b) {
                    const double xa = model.progress(cur[a]).x, xb = model.progress(cur[b]).x;
                    return xa < xb || (xa == xb && a < b);
                });
                uint16_t c = 0;
                for (size_t k = 1; k < m.size(); ++k) {
                    if (model.progress(cur[m[k]]).x - model.progress(cur[m[k - 1]]).x > gap
                        && c < 0xFFFF)
                        ++c;
                    curXc[m[k]] = c;
                }
                if (c > 0) split = true;
                if ((int)c + 1 > g_groupXSplitMax) g_groupXSplitMax = (int)c + 1;
            }
            if (split) ++g_groupXSplitLayers;
        }
        // The x-crossing split (trigshadow_live.hpp GroupIn): in a layer of the plain frame, a state's part is also told by how many of the x-crossing triggers the shadow runs it has crossed before this
        // tick's step and after it. xc then indexes xcTab (the --groupxsplit (removed) part and the two counts); the tests of xc below compare it as before.
        struct XcPart { uint16_t part; uint32_t q0, q1; };
        std::vector<XcPart> xcTab;
        bool crossHere = g_trigLive.crossOn() && !cur.empty();
        for (size_t i = 0; crossHere && i < cur.size(); ++i)
            if ((int)cur[i].frame != 0 || cur[i].rev) crossHere = false;
        if (crossHere) {
            const bool had = !curXc.empty();
            if (!had) curXc.assign(cur.size(), 0);
            for (size_t i = 0; i < cur.size(); ++i) {
                const State& s = cur[i];
                const double x0 = (double)s.xAbs, x1 = x0 + (double)((s.dx > 0.f) ? s.dx : curDxF);   // (the x the group's timeline is stepped with is its front's, made the same way)
                const XcPart p{had ? curXc[i] : (uint16_t)0, g_trigLive.crossCount(x0), g_trigLive.crossCount(x1)};
                size_t j = 0;
                for (; j < xcTab.size(); ++j) if (xcTab[j].part == p.part && xcTab[j].q0 == p.q0 && xcTab[j].q1 == p.q1) break;
                if (j == xcTab.size()) {
                    if (xcTab.size() >= 0xFFFF) { j = 0; }   // (more parts than xc can name: the rest go with the first)
                    else xcTab.push_back(p);
                }
                curXc[i] = (uint16_t)j;
            }
        }
        auto xcOf = [&](size_t i) -> uint16_t { return curXc.empty() ? 0 : curXc[i]; };
        std::vector<GKey> groupDx;
        std::vector<uint32_t> groupOf(cur.size());   // each state's group (its index in groupDx), for the batching of a layer's groups below
        std::vector<size_t> groupFirst;              // each group's first member
        for (size_t i = 0; i < cur.size(); ++i) {
            const State& s = cur[i];
            const GKey k{model.groupKey(s, curDxF, sigOf(i)), xcOf(i)};
            const size_t j = (size_t)(std::find(groupDx.begin(), groupDx.end(), k) - groupDx.begin());
            if (j == groupDx.size()) {
                groupDx.push_back(k);
                groupFirst.push_back(i);
            }
            groupOf[i] = (uint32_t)j;
        }
        // each group's trigger timeline: the part of xc its key is made with, what it tells the shadow of its crossings and of the timeline its first member was stepped in, and the key it is stored under
        wrClose();
        if (wrT != (long long)t) {
            if (wrT == (long long)t - 1) wrPrev.swap(wrCur); else wrPrev.clear();
            wrCur.clear();
            wrT = (long long)t;
        }
        std::vector<TrigLive::GroupIn> gIn(groupDx.size());
        std::vector<int> gPart(groupDx.size(), 0);
        std::vector<uint64_t> gStore(groupDx.size(), 0);
        if (g_trigLive.on) {
            for (size_t j = 0; j < groupDx.size(); ++j) {
                const GKey& gk = groupDx[j];
                TrigLive::GroupIn& in = gIn[j];
                gPart[j] = crossHere ? (int)xcTab[gk.xc].part : (int)gk.xc;
                if (crossHere) { in.cross = true; in.cross0 = xcTab[gk.xc].q0; in.cross1 = xcTab[gk.xc].q1; }
                in.evg = cur[groupFirst[j]].evgate;   // the group's members share it (the signature of the key holds it)
                const uint32_t node = cur[groupFirst[j]].parent;
                const auto wi = std::upper_bound(wrPrev.begin(), wrPrev.end(), node, [](uint32_t n, const WorldRange& r) { return n < r.a1; });   // (the ranges are in arena order and do not overlap)
                if (wi != wrPrev.end() && node >= wi->a0) { in.hasWorld = true; in.world = wi->key; }
            }
            // a group that crosses on this tick and one that crossed before would store under one key: the first is salted
            for (size_t j = 0; crossHere && j < groupDx.size(); ++j) {
                if (gIn[j].cross0 == gIn[j].cross1) continue;
                for (size_t m = 0; m < groupDx.size() && !gIn[j].salt; ++m)
                    if (m != j && gIn[m].cross1 == gIn[j].cross1 && gPart[m] == gPart[j] && static_cast<const GroupKey&>(groupDx[m]) == static_cast<const GroupKey&>(groupDx[j])
                        && (gIn[m].cross0 == gIn[m].cross1 || m < j))
                        gIn[j].salt = true;
            }
            for (size_t j = 0; j < groupDx.size(); ++j) {
                const GKey& gk = groupDx[j];
                gStore[j] = TrigLive::storeKeyOf(TrigLive::keyOf(gk.dx, gk.trig.word(0), (int)gk.frame, (int)gk.rev, gk.fsig, gPart[j]), gIn[j]);
            }
        }
        // --layerprof: what tells this layer's groups apart -- the number of groups and of distinct values of each part of the group key over them (speed, touch mask, frame and direction,
        // fire signature, x part), and over the STATES the distinct touch fire signatures and own-crossing signatures (the two things the key's signature is made of)
        if (g_layerProf.f) {
            auto distinct = [](std::vector<uint64_t>& v) { std::sort(v.begin(), v.end()); return (double)(std::unique(v.begin(), v.end()) - v.begin()); };
            std::vector<uint64_t> a, b, c, d, e2;
            for (const GKey& g : groupDx) {
                uint32_t dxb; std::memcpy(&dxb, &g.dx, 4); a.push_back(dxb);
                uint64_t tw = 0; for (int k = 0; k < (kTouchBits + 63) / 64; ++k) tw = tw * 0x9e3779b97f4a7c15ULL + g.trig.word(k); b.push_back(tw);
                c.push_back((uint64_t)(uint32_t)g.frame | ((uint64_t)(uint32_t)g.rev << 32)); d.push_back(g.fsig); e2.push_back(g.xc);
            }
            double* ex = g_layerProf.ex;
            ex[0] = (double)groupDx.size(); ex[1] = distinct(a); ex[2] = distinct(b); ex[3] = distinct(c); ex[4] = distinct(d); ex[5] = distinct(e2);
            std::vector<uint64_t> fs, xs;
            for (size_t i = 0; i < cur.size(); ++i) { if (g_groupFire) fs.push_back(groupFireSig(cur[i], t)); if (g_stateCross) xs.push_back(xcSig(cur[i], t)); }
            ex[6] = fs.empty() ? 0.0 : distinct(fs); ex[7] = xs.empty() ? 0.0 : distinct(xs);
            // ...and, where the touch fire signature splits the layer, which boxes do it: a `#fs` row with, for every box that is in the signature of some state (entered, its window
            // g_touchMoveTicks still running) with more than one entry tick among the states: box index, the trigger's uid, the states that have it, the distinct entry ticks, the earliest
            // and the latest, and the window
            if (ex[6] >= 2.0) {
                const size_t nb = std::min<size_t>(g_touchMoveTicks.size(), (size_t)kTouchBits);
                std::string row;
                for (size_t b = 0; b < nb; ++b) {
                    const long long win = g_touchMoveTicks[b];
                    if (win <= 0) continue;
                    std::vector<uint64_t> tk;
                    double xEarly = 0.0, xLate = 0.0; long long tEarly = -1, tLate = -1;   // the x now of a state with the earliest entry tick and of one with the latest
                    for (size_t i = 0; i < cur.size(); ++i) if (cur[i].trig.test(b) && t - (long long)cur[i].fireB[b] < win) {
                        const long long f = (long long)cur[i].fireB[b];
                        tk.push_back((uint64_t)f);
                        if (tEarly < 0 || f < tEarly) { tEarly = f; xEarly = (double)cur[i].xAbs; }
                        if (f > tLate) { tLate = f; xLate = (double)cur[i].xAbs; }
                    }
                    if (tk.size() < 2) continue;
                    const size_t live = tk.size();
                    const double nd = distinct(tk);
                    if (nd < 2.0) continue;
                    char buf[200];
                    std::snprintf(buf, sizeof buf, "\tb%zu:uid%d:live%zu:n%.0f:lo%llu:hi%llu:win%lld:xe%.1f:xl%.1f", b, b < g_touch.size() ? g_touch[b].uid : -1, live, nd, (unsigned long long)tk.front(),
                                  (unsigned long long)*std::max_element(tk.begin(), tk.begin() + (std::ptrdiff_t)nd), win, xEarly, xLate);
                    row += buf;
                }
                if (!row.empty()) std::fprintf(g_layerProf.f, "#fs\t%lld\t%lld%s\n", g_layerProf.search, (long long)t, row.c_str());
                // ...and whether what such a box moves or switches is anywhere the player can get to while the box is in the signature: a `#fr` row. A box's class, by x only (no y test,
                // so "in reach" is the generous side): 0 nothing of its chain is among the moving objects, 1 none of them within the stretch the frontier can cover before the window runs
                // out (200 px behind to 200 px + 2.4 px a tick ahead, each object widened by its move's offset), 2 some there but none within 110 px now, 3 some within 110 px now,
                // 4 the box also switches triggers or arms roots (kept whatever its objects). Then the layer's number of groups as it is, with the boxes of class 0-1 left out of the
                // signature, with 0-2 left out, and with no fire signature at all. Frame 0 coordinates: a layer with a turned frame in it is marked f1.
                {
                    Dynamics& D0 = model.levelOf(0).dyn;
                    static std::unordered_map<int, size_t> uidAt; static long long uidAtSearch = -1;
                    if (uidAtSearch != g_layerProf.search) { uidAt.clear(); for (size_t i = 0; i < D0.objs.size(); ++i) uidAt.emplace(D0.objs[i].uid, i); uidAtSearch = g_layerProf.search; }
                    const double step = std::max(2.4, std::fabs((double)curDxF));
                    int cls[kTouchBits]; for (int& c : cls) c = -1;
                    std::string frow;
                    for (size_t b = 0; b < nb && b < g_touch.size(); ++b) {
                        const long long win = g_touchMoveTicks[b];
                        if (win <= 0) continue;
                        long long remain = -1;
                        for (size_t i = 0; i < cur.size(); ++i) if (cur[i].trig.test(b) && t - (long long)cur[i].fireB[b] < win) remain = std::max(remain, win - (t - (long long)cur[i].fireB[b]));
                        if (remain < 0) continue;
                        const TouchTrig& tb = g_touch[b];
                        const double farLo = bestX - 200.0, farHi = bestX + 200.0 + step * (double)remain;
                        int known = 0, far = 0, near = 0;
                        auto see = [&](int uid, double off) {
                            const auto it = uidAt.find(uid);
                            if (it == uidAt.end()) return;
                            const Obj& o = D0.objs[it->second];
                            const double e = std::max(o.hw, o.radius) + off;
                            ++known;
                            if (o.cx + e >= farLo && o.cx - e <= farHi) ++far;
                            if (o.cx + e >= bestX - 110.0 && o.cx - e <= bestX + 110.0) ++near;
                        };
                        for (const TrigCtl& c : tb.ctl) see(c.uid, std::fabs((double)c.dx));
                        for (const TouchTrig::TogEv& e : tb.togEv) see(e.uid, 0.0);
                        cls[b] = (!tb.trigTog.empty() || !tb.spawnsRoots.empty() || tb.countAct) ? 4 : near > 0 ? 3 : far > 0 ? 2 : known > 0 ? 1 : 0;
                        char buf[96]; std::snprintf(buf, sizeof buf, "\tb%zu:c%d:o%d:far%d:near%d", b, cls[b], known, far, near); frow += buf;
                    }
                    auto sigLess = [&](const State& s, int upTo) -> uint64_t {   // groupFireSig without the boxes of class <= upTo (upTo 9: none of them)
                        uint64_t k = 0;
                        for (size_t b = 0; b < nb; ++b) {
                            if (!s.trig.test(b) || cls[b] <= upTo) continue;
                            const long long moving = g_touchMoveTicks[b];
                            if (moving <= 0 || t - (long long)s.fireB[b] >= moving) continue;
                            uint64_t h = ((uint64_t)(b + 1) << 32) ^ (uint64_t)s.fireB[b];
                            h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull; h = (h ^ (h >> 27)) * 0x94D049BB133111EBull; k ^= h ^ (h >> 31);
                        }
                        return k;
                    };
                    size_t gN[3] = {0, 0, 0}; bool turned = false;
                    const int upTo[3] = {1, 2, 9};
                    for (int v = 0; v < 3; ++v) {
                        std::vector<GKey> ks;
                        for (size_t i = 0; i < cur.size(); ++i) {
                            if (v == 0 && model.progress(cur[i]).frame != 0) turned = true;
                            const GKey k{model.groupKey(cur[i], curDxF, sigLess(cur[i], upTo[v]) ^ (g_stateCross ? xcSig(cur[i], t) : 0)), xcOf(i)};
                            if (std::find(ks.begin(), ks.end(), k) == ks.end()) ks.push_back(k);
                        }
                        gN[v] = ks.size();
                    }
                    std::fprintf(g_layerProf.f, "#fr\t%lld\t%lld\t%zu\t%zu\t%zu\t%zu\t%s%s\n", g_layerProf.search, (long long)t, groupDx.size(), gN[0], gN[1], gN[2], turned ? "f1" : "f0", frow.c_str());
                }
            }
        }

        // --worldtree (modifiers.hpp): the layer's states by group and by world (observation only)
        if (g_worldTree.on && !cur.empty()) {
            std::unordered_map<uint64_t, long long> perWorld;
            std::unordered_set<uint64_t> pairs;
            std::unordered_map<uint64_t, std::unordered_set<size_t>> groupsOfWorld;
            for (size_t i = 0; i < cur.size(); ++i) {
                const GKey k{model.groupKey(cur[i], curDxF, sigOf(i)), xcOf(i)};
                const size_t j = (size_t)(std::find(groupDx.begin(), groupDx.end(), k) - groupDx.begin());
                const uint64_t wk = worldKeyOf(cur[i], false, true);
                ++perWorld[wk];
                pairs.insert(wk * 0x9E3779B97F4A7C15ull ^ (uint64_t)(j + 1) * 0xC2B2AE3D27D4EB4Full);
                groupsOfWorld[wk].insert(j);
            }
            auto& B = g_worldTree.b[(long long)t / 2000];
            ++B.layers; B.states += (long long)cur.size(); B.g += (long long)groupDx.size(); B.w += (long long)perWorld.size(); B.p += (long long)pairs.size();
            for (const auto& kv : perWorld) B.maxStatesPerWorld = std::max(B.maxStatesPerWorld, kv.second);
            for (const auto& kv : groupsOfWorld) B.maxGroupsPerWorld = std::max(B.maxGroupsPerWorld, (long long)kv.second.size());
        }

        uint32_t emitNode = 0;   // the node emit() gave the state it has just stored (0: it stored none), for rideNodes
        auto emit = [&](State s, const State& from, uint8_t action, uint64_t k) {
            auto finish = [&](State& stored) {
                arena.push_back(Node{from.parent | ((uint32_t)action << 31)});
                emitNode = (uint32_t)(arena.size() - 1);
                if (checkWanted) arenaMode.push_back(s.mode);
                if (lossyOn) { const uint8_t lf = arenaLossy[from.parent]; arenaLossy.push_back(lf); }
                s.parent = (uint32_t)(arena.size() - 1);
                s.action = action;
                stored = s;
                // the goal is reached by a STATE, at its own x -- not by the
                // layer's nominal timeline. Only in frame 0: `goalX` is a world
                // x, and in a turned frame xAbs is a different axis entirely
                // (lv22 ends un-rotated, so nothing is lost).
                // The goal is reached by a STATE, and by MORE THAN ONE of them:
                // the layer runs to the end (the loop tests `!solved` on the
                // NEXT iteration), so every other state that got past goalX is
                // emitted too and only loses to this latch. Measured on lv16:
                // 2,000 of them, whose lineages stay distinct for 3,345 ticks.
                // Keep the roomiest instead of the first; with the score off
                // they all read 0 and the incumbent always wins, which is the
                // old latch exactly.
                // --coins: the end of the level is not the goal on its own.
                // A lineage that reached it without the full set is not a
                // solution, and saying so HERE rather than filtering afterwards
                // is what makes the search look for the other one instead of
                // reporting the first thing that got to the end.
                if (rules.reached(s) && (!solved || rules.prefer(s.tight, goalState.tight))) {
                    solved = true;
                    goalState = s;
                }
            };
            Slots& sl = seen.at(k);
            // --hsmergeaudit: the shadow signature of a state (each body's H, S and carried consumers), compared with the survivors of its key
            auto hsSig = [](const State& x) -> uint32_t {
                return (uint32_t)(x.gdHS & 3) | ((uint32_t)(x.gdAux & 6) << 1) | (x.dual ? ((uint32_t)(x.gdHS2 & 3) | ((uint32_t)((x.gdAux >> 4) & 6) << 1)) << 4 : 0u);
            };
            if (sl.hi < 0) {
                nxt.push_back(State{});
                sl.hi = sl.lo = (int)nxt.size() - 1;
                finish(nxt.back());
                if (g_hsMergeAudit) ++g_hsMergeStored;
            } else if (s.vy > nxt[(size_t)sl.hi].vy + 1e-9) {
                if (gdcol::g_keyObs && sl.hi != sl.lo) noteDroppedPrev(nxt[(size_t)sl.hi], s);   // KEYOBS: the displaced rep
                if (g_hsMergeAudit && sl.hi != sl.lo) {
                    ++g_hsMergeReplaced;
                    const uint32_t gone = hsSig(nxt[(size_t)sl.hi]);
                    if (gone != hsSig(s) && gone != hsSig(nxt[(size_t)sl.lo])) ++g_hsMergeReplacedDistinct;
                }
                if (sl.hi == sl.lo) {
                    nxt.push_back(State{});
                    sl.hi = (int)nxt.size() - 1;
                }
                finish(nxt[(size_t)sl.hi]);
            } else if (s.vy < nxt[(size_t)sl.lo].vy - 1e-9) {
                if (gdcol::g_keyObs && sl.hi != sl.lo) noteDroppedPrev(nxt[(size_t)sl.lo], s);   // KEYOBS: the displaced rep
                if (g_hsMergeAudit && sl.hi != sl.lo) {
                    ++g_hsMergeReplaced;
                    const uint32_t gone = hsSig(nxt[(size_t)sl.lo]);
                    if (gone != hsSig(s) && gone != hsSig(nxt[(size_t)sl.hi])) ++g_hsMergeReplacedDistinct;
                }
                if (sl.hi == sl.lo) {
                    nxt.push_back(State{});
                    sl.lo = (int)nxt.size() - 1;
                }
                finish(nxt[(size_t)sl.lo]);
            } else if (g_hsMergeAudit || gdcol::g_keyObs) {
                if (gdcol::g_keyObs) { noteDroppedPrev(s, nxt[(size_t)sl.hi]); if (sl.lo != sl.hi) noteDroppedPrev(s, nxt[(size_t)sl.lo]); }   // KEYOBS: dropped; compared with the reps it lost to
                if (g_hsMergeAudit) {
                ++g_hsMergeDropped;
                const uint32_t me = hsSig(s);
                if (me != hsSig(nxt[(size_t)sl.hi]) && me != hsSig(nxt[(size_t)sl.lo])) ++g_hsMergeDroppedDistinct;
                }
            }
        };

        // "alive dropped" does not say whether the children DIED or were merged
        // away by the dedupe; those need opposite fixes, so count both.
        long long nBorn = 0, nDied = 0;
        long long lyPhys = 0, lyPrune = 0;   // --partialphys: this layer's deaths by kind
        // --partialphys: a child that died by the game's own rules, and the state that stepped it
        auto notePhys = [&](const State& from, uint8_t kind) {
            if (kind == kDeathPrune) { ++lyPrune; return; }
            ++lyPhys;
            if (!havePhys || physT != t || from.xAbs > physS.xAbs) { physS = from; physT = t; havePhys = true; }
        };
        // --qfoldwatch: which rotation-queue states this group's dedupe merged
        // away. keyOf has no rotSpent/rotChan/rotRev, so children that differ
        // only in the queue share a cell. For each key, the queue states among
        // the group's live children minus those among what the dedupe kept (the
        // survivors are copies of children, so they key the same). Print only.
        long long qfLost = 0, qfKeys = 0;
        auto qfoldAccount = [&](size_t before) {
            auto qs = [](const State& s) {
                return ((uint64_t)(uint32_t)s.rotSpent << 32) | ((uint64_t)s.rotChan << 16)
                       | (uint64_t)s.rotRev;
            };
            auto add = [](std::vector<uint64_t>& v, uint64_t q) {
                if (std::find(v.begin(), v.end(), q) == v.end()) v.push_back(q);
            };
            std::unordered_map<uint64_t, std::vector<uint64_t>> alive, kept;
            for (size_t i = 0; i < kids.size(); ++i)
                if (kidFlag[i] == 2) add(alive[kidKeys[i]], qs(kids[i].s));
            for (size_t j = before; j < nxt.size(); ++j)
                add(kept[modelKeyOf(nxt[j], (long long)t) ^ rules.keyBits(nxt[j])], qs(nxt[j]));
            for (const auto& [k, a] : alive) {
                const auto it = kept.find(k);
                long long lost = 0;
                for (uint64_t q : a)
                    if (it == kept.end()
                        || std::find(it->second.begin(), it->second.end(), q)
                               == it->second.end())
                        ++lost;
                if (lost) { qfLost += lost; ++qfKeys; }
            }
        };
        const bool qfHere = (g_qfoldLo >= 0 && t >= g_qfoldLo && t <= g_qfoldHi);
        ppSubMark(1);
        if (g_trigLive.take && g_trigLive.supplyOn()) {   // --trigsupplytake: the layer's group keys, before any group() call (trigshadow_live.hpp announce)
            std::vector<uint64_t> aks; aks.reserve(groupDx.size());
            for (const auto& gk : groupDx) aks.push_back(TrigLive::keyOf(gk.dx, gk.trig.word(0), (int)gk.frame, (int)gk.rev, gk.fsig, gPart[(size_t)(&gk - groupDx.data())]));
            g_trigLive.announce((long long)t, aks, gIn);
        }
        // ---- dupbatch (search_key.hpp): the layer's groups go to the trigger shadow first, and each gets the key of its tick ----
        // dupG[gi]: `view` what the shadow's group() left as the current group (made current again at the group's turn); `ka / kb` the key; `buffered` an earlier group with that key (the leader) steps this
        // group's states with its own; gidx / kids / keys / flags what that pass made for it.
        // (camY: --camy, the group's own camera input of this tick. It is not in the key: a follower's children are stepped with the follower's own, see kidCam)
        struct DupG { bool live = false, buffered = false, readTicks = false; uint64_t ka = 0, kb = 0; TrigLive::View view; CamYGroup camY; std::vector<size_t> gidx; std::vector<Child> kids; std::vector<uint64_t> keys; std::vector<uint8_t> flags; };
        std::vector<DupG> dupG;
        std::unordered_map<uint64_t, std::vector<uint32_t>> dupByKey;   // key -> the groups that have it, in the layer's order
        struct GStat { double lo = 1e18, hi = -1e18; int fire = -1, fireMin = 0x7fffffff; uint16_t fb[kTouchBits] = {}; float lockOff = 1e30f, d2lo = 0.f, d2hi = 0.f; size_t rep = (size_t)-1; std::vector<size_t> members; };
        std::vector<GStat> gs;
        bool dupOneFrame = true;   // every state of the layer is in one frame and direction: a group's parents are then its members (no parent of another frame is handed to it)
        // (not where something reads or prints every group's own tick: --needtrig, the per-layer instruments, and DUPOBS_OUT, which measures the groups as they are without the batching)
        const bool dupHere = g_trigLive.on && g_trigLive.supplyOn() && groupDx.size() > 1 && !g_needTrig && !(dbgLayers > 0 && t - t0 <= dbgLayers) && !g_refWatch && !g_clearProbe
                             && !g_portAudit && !qfHere && g_searchCensus <= 0 && !g_placeObs.on && g_trigLive.checkEvery == 0 && !(g_rotWatchLo >= 0 && t >= g_rotWatchLo && t <= g_rotWatchHi)
                             && !DupObs::on();
        g_trigLive.shareSteps = dupHere;   // (trigshadow_live.hpp: a group with an earlier group's parent timeline and inputs takes its stepped timeline)
        if (dupHere) {
            ++g_dupBatchLayers;
            // each group's members, their span, fire ticks, lock offset and second-body spread: what the loops at the head of the group's own turn find by walking every state for every group, taken here in
            // one pass over the states (the group's turn reads them from here instead)
            gs.assign(groupDx.size(), GStat{});
            for (size_t ci = 0; ci < cur.size(); ++ci) {
                const State& s = cur[ci];
                GStat& G = gs[groupOf[ci]];
                if (G.rep == (size_t)-1) G.rep = ci;
                G.members.push_back(ci);
                G.lo = std::min(G.lo, (double)s.xAbs); G.hi = std::max(G.hi, (double)s.xAbs);
                G.fire = std::max(G.fire, (int)s.trigT); G.fireMin = std::min(G.fireMin, (int)s.trigT);
                for (int b = 0; b < kTouchBits; ++b) if (s.fireB[b] > G.fb[b]) G.fb[b] = s.fireB[b];
                if (s.lockOff < G.lockOff) G.lockOff = s.lockOff;
                const float d2 = p2Dx(s);
                G.d2lo = std::min(G.d2lo, d2); G.d2hi = std::max(G.d2hi, d2);
                if ((int)s.frame != (int)cur[0].frame || (int)s.rev != (int)cur[0].rev) dupOneFrame = false;
            }
            dupG.resize(groupDx.size());
            std::unordered_map<const gdtrig::TrigShadow*, std::pair<gdtrig::Hash2, gdtrig::Hash2>> dupHashOf;
            for (size_t gi = 0; gi < groupDx.size(); ++gi) {
                const GKey& gk = groupDx[gi];
                GStat& G = gs[gi];
                if (G.lockOff > 1e29f) G.lockOff = 0.f;
                const int gf = (int)gk.frame, gr = (int)gk.rev;
                const double sdx2 = gr ? -(double)gk.dx : (double)gk.dx;
                const double x2 = (gr ? G.lo : G.hi) + sdx2;
                const double wLo2 = gr ? (G.lo + sdx2) : G.lo, wHi2 = gr ? G.hi : (G.hi + sdx2);
                g_trigLive.gin = gIn[gi];
                g_trigLive.group(t, gk.dx, gk.trig.word(0), gf, gr, gk.fsig, gPart[gi], x2, G.fb);   // (the call the group's own turn makes without the flag, with the same arguments)
                DupG& D = dupG[gi];
                D.view = g_trigLive.view();
                if (g_camy) D.camY = g_trigLive.camYOfLast();   // (made by group() for the group just given, from its parent timeline's camera)
                if (!D.view.cur) continue;   // the shadow stepped nothing for it (no supplied group): it is prepared for itself
                gdtrig::Hash2 h;
                auto hc = dupHashOf.find(D.view.cur.get());   // (groups that took one stepped timeline -- TrigLive::shareSteps -- hash it once)
                if (hc == dupHashOf.end()) hc = dupHashOf.emplace(D.view.cur.get(), std::make_pair(D.view.cur->contentHash(), D.view.cur->trackHash())).first;
                h.addH(hc->second.first); h.addH(hc->second.second);
                // ...and the objects as they stood BEFORE this group's step. The supply hands a mover its previous place and its on / off from there (TrigLive::supVal: px / py / pon; dcx, dcy and
                // pcy are made of them), and two timelines can step to one content from two different ones: a Move that ends this tick in the one and ended a tick earlier in the other leaves the
                // same place and another step. Found by --dupbatchcheck on lv19 and lv20 of the 22-level cold (trunk e3eb678d: vy 0 against 0.008 on a platform's last step).
                h.addH(gdtrig::TrigShadow::objsHash(D.view.pre));
                h.addD(x2); h.addD(G.lo); h.addD(G.hi); h.addD(wLo2); h.addD(wHi2); h.addF(gk.dx); h.addF2(G.d2lo, G.d2hi); h.add((uint64_t)(uint32_t)gf | ((uint64_t)(uint32_t)gr << 32));
                for (int k = 0; k < (kTouchBits + 63) / 64; ++k) h.add(gk.trig.word(k));
                h.addF(G.lockOff + (float)sdx2);
                // The touch fire ticks (gFire / gFireB) are what the MODEL's own placement of a touch-moved mover reads; the shadow's timeline has that mover's place in its content, and where the supply
                // puts every mover there the ticks are not read. They are part of the key wherever the model's placement can stand instead: a turned gameplay frame (supplyPlaceMovers leaves it whole
                // under --trigsupply-all), named groups only, a timeline with a blind object (its objects are not supplied), a mover with no slot in the supply, or one left on the model's place for a
                // rect below y = 0 (supGround). Found by --dupbatchcheck on lv22's first call (t=1758, a spider in a turned frame: a platform the model placed by the leader's ticks).
                // Every tick as it is, not only those whose window still runs (g_touchMoveTicks): with that narrower key the check passed on the calls tried, but the window is the model's
                // estimate, not a proof that a later tick is never read.
                // In two of those cases the ticks are NOT in the key, and the leader's build notes which ticks its placement READ instead (dynamics.hpp FireReadSet): a group with the leader's
                // key follows it only if it has the leader's value for each tick read (the test where the followers are gathered). The build is a function of its inputs, and of those only
                // the ticks are not in the key.
                //   a turned frame (why 2): every object is on the model's placement, and every object's reads are noted. With that test taken out, --dupbatchcheck finds the t=1758
                //   mismatch above again; with it, none;
                //   a mover with no slot (why 4): the reads of those movers are noted (Dynamics::tickTrack). With the ticks in the key, one such mover keyed every group of the plain frame
                //   by its ticks: lv22's first call has 39, its later calls 75 and more, and none of their groups was batched. What this case still takes on trust is what a level with
                //   every mover supplied takes: that nothing the step reads of a SUPPLIED mover hangs on the ticks.
                {
                    Dynamics& dyn0 = model.levelOf(0).dyn;
                    // (a ground-clamped mover before a mover with no slot: the first is keyed by every tick, and the set of the second does not note a supplied mover's reads)
                    const int why = !g_trigLive.supplyAll ? 1 : gf != 0 ? 2 : D.view.cur->anyBlind() ? 3 : g_trigLive.supGround > 0 ? 5 : g_trigLive.unsupMovers(dyn0.objs) > 0 ? 4 : 0;
                    if (why) ++g_dupBatchFireKeyed[why];
                    if (why == 4 || why == 2) {
                        if (why == 4 && (dyn0.tickTrackGen != g_trigLive.supGen || dyn0.tickTrack.size() != dyn0.objs.size()))
                            dyn0.setTickTrack([](int uid) { return !g_trigLive.hasSlot(uid); }, g_trigLive.supGen);
                        D.readTicks = true;
                        h.add(0x6674 + (uint64_t)why);
                    } else if (why) {
                        h.add(0x6672 + (uint64_t)(uint32_t)G.fire);
                        for (int b = 0; b < kTouchBits; ++b) h.add((uint64_t)G.fb[b]);
                    }
                }
                h.add(0x7470 + (uint64_t)D.view.tp.size()); for (int u : D.view.tp) h.add((uint64_t)(uint32_t)u);
                h.add(0x6772 + (uint64_t)D.view.grav.size()); for (int u : D.view.grav) h.add((uint64_t)(uint32_t)u);
                {   // the autonomous triggers as this group dates them (what its turn's AutoOwnGuard sets, put back when this block ends)
                    AutoOwnGuard ao;
                    g_autoOwnT0 = t0;
                    if (gf == 0 && !gr) {
                        if (g_stateCross && G.rep != (size_t)-1) autoOwnApplyCross(ao, G.hi, (double)gk.dx, t, cur[G.rep]);
                        else autoOwnApply(ao, G.hi, (double)gk.dx, t);
                    }
                    h.add(0x616f + (uint64_t)ao.saved.size());
                    for (const auto& p : ao.saved) { h.add((uint64_t)p.first); h.add((uint64_t)(uint32_t)g_autoTrig[p.first].fireT); }
                }
                D.ka = h.a; D.kb = h.b; D.live = true;
                dupByKey[h.a ^ (h.b * 0x9e3779b97f4a7c15ULL)].push_back((uint32_t)gi);
            }
        }
        for (const auto& gkey : groupDx) {
        const size_t dupGi = (size_t)(&gkey - groupDx.data());
        wrClose();
        if (g_trigLive.on) { wrA0 = arena.size(); wrOpen = true; wrOpenKey = gStore[dupGi]; }
        const bool dupBuf = dupHere && dupG[dupGi].buffered;                // an earlier group of the layer has stepped this group's states with its own
        const bool dupSkip = dupBuf && !g_dupBatchCheck;                     // ...and this group is not prepared or stepped for itself
        if (g_phaseProf) g_ppSubLast = std::chrono::steady_clock::now();   // this group's setup starts here (not at the end of the previous group's step)
        const float gdx = gkey.dx;
        const TouchMask gtrig = gkey.trig;
        const int gframe = (int)gkey.frame;
        const int grev = (int)gkey.rev;
        // this group's own x span, and the windows that cover it for the whole
        // tick (from where its slowest member starts to where it ends up)
        double gLo = 1e18, gHi = -1e18;
        int gFire = -1;   // latest tick any of them fired a touch trigger
        int gFireMin = 0x7fffffff;   // ...and the earliest, for the pass count only
        // ...and the same thing per box, for the formula path. The group key
        // holds `trig`, so every member has entered the SAME set of boxes --
        // what differs is when, and this keeps the latest for each one
        // separately instead of collapsing them onto a single scalar.
        uint16_t gFireB[kTouchBits] = {};
        // ...and how far the least-advanced member has carried a locked
        // platform. Same conservative direction as `gFire`, which takes the
        // LATEST firing tick so the group is shown a door that is still
        // opening: the smallest offset is the one that has ridden least.
        float gLockOff = 1e30f;
        size_t gRep = (size_t)-1;   // --statecross: a member, whose records are the group's
        if (dupHere) {   // dupbatch: the pass before the loop has them (the same values: the same folds over the same members, in the same order)
            const GStat& G = gs[dupGi];
            gLo = G.lo; gHi = G.hi; gFire = G.fire; gFireMin = G.fireMin; gLockOff = G.lockOff; gRep = G.rep;
            for (int b = 0; b < kTouchBits; ++b) gFireB[b] = G.fb[b];
        } else
        for (size_t ci = 0; ci < cur.size(); ++ci) {
            const State& s = cur[ci];
            // Asked for every state of every group, every layer: the model's test compares the
            // cheapest fields first and stops at the first difference; the fire signature and the
            // part, which this search holds per state, only after it.
            if (!model.inGroup(gkey, s, curDxF) || sigOf(ci) != gkey.fsig || xcOf(ci) != gkey.xc)
                continue;
            if (gRep == (size_t)-1) gRep = ci;
            // x read straight off the State, not through model.progress, here and in the three other
            // per-state loops of a layer (gidx's frame/rev, the cap's x range, the layer's leader):
            // through it, lv22's search took 3% longer (measured on 8eb7b5dc against e5eec93a).
            gLo = std::min(gLo, (double)s.xAbs);
            gHi = std::max(gHi, (double)s.xAbs);
            gFire = std::max(gFire, (int)s.trigT);
            gFireMin = std::min(gFireMin, (int)s.trigT);
            // gFireB is uint16_t[kTouchBits] -- fold all of it, not the first 32.
            for (int b = 0; b < kTouchBits; ++b)
                if (s.fireB[b] > gFireB[b]) gFireB[b] = s.fireB[b];
            if (s.lockOff < gLockOff) gLockOff = s.lockOff;
        }
        if (gLockOff > 1e29f) gLockOff = 0.f;   // no member (group is empty)
        // --p2xoff: how far the members' second bodies run from their first (model.hpp p2Widen). A
        // pass of its own, under the flag only: the loop above is the layer's hottest (one helper
        // call in it cost lv22's search 3%, see its note), so nothing of the flag's goes in it.
        float gD2Lo = 0.f, gD2Hi = 0.f;
        if (dupHere) { gD2Lo = gs[dupGi].d2lo; gD2Hi = gs[dupGi].d2hi; }
        else
        for (size_t ci = 0; ci < cur.size(); ++ci) {
            const State& s = cur[ci];
            if (!model.inGroup(gkey, s, curDxF) || sigOf(ci) != gkey.fsig || xcOf(ci) != gkey.xc)
                continue;
            const float d2 = p2Dx(s);
            gD2Lo = std::min(gD2Lo, d2);
            gD2Hi = std::max(gD2Hi, d2);
        }
        // The gate's pass count: this group is APPROXIMATED only if its members
        // disagree about when the trigger fired. Counted here, before any of the
        // reasons below can `continue` past it, so "placed" means placed.
        if (gFire >= 0) {
            ++g_gfireGroups;
            if (gFireMin < gFire) {
                ++g_gfireSpread;
                const int d = gFire - gFireMin;
                g_gfireSum += d;
                if (d > g_gfireMax) g_gfireMax = d;
            }
        }
        // Drop a whole group once it is past a box it was required to enter.
        // Killing it here rather than at the end keeps the frontier spent on
        // states that can still satisfy the requirement.
        if (g_needTrig) {
            bool missed = false;
            for (size_t b = 0; b < g_touch.size() && !missed; ++b)
                if ((g_needTrig & ~gtrig) & touchBit(b))
                    // --needtrigrev (removed) (triggers.hpp): a reversed group passes a box going left
                    missed = (grev) ? gHi < g_touch[b].cx - g_touch[b].hw - 40.0
                                                     : gLo > g_touch[b].cx + g_touch[b].hw + 40.0;
            if (missed) continue;
        }
        const double xPrev = gHi;                    // nominal, reports only
        // A reverse (id 2899) group's x decreases. The windows too are laid
        // out along the direction of travel.
        const double sdx = grev ? -(double)gdx : (double)gdx;
        const double x = (grev ? gLo : gHi) + sdx;   // ditto
        const float dxUsed = (float)sdx;
        const double wLo = grev ? (gLo + sdx) : gLo;
        const double wHi = grev ? gHi : (gHi + sdx);
        if (g_layerXStat) g_layerXStatData.add(t, x, gdx, gframe, grev, gtrig.any(), gHi - gLo);
        if (dupHere) g_trigLive.activate(dupG[dupGi].view, (long long)t);   // dupbatch: the shadow stepped this group before the loop; its timeline is the current one again
        else if (g_trigLive.on) { g_trigLive.gin = gIn[dupGi]; g_trigLive.group(t, gdx, gtrig.word(0), gframe, grev, gkey.fsig, gPart[dupGi], x, gFireB); }   // --trigshadowlive (trigshadow_live.hpp)
        const CamYGroup camyG = g_camy ? (dupHere ? dupG[dupGi].camY : g_trigLive.camYOfLast()) : CamYGroup{};   // --camy S2.1: this group's camera timeline input, a local of this group's scope (cam_y_search.hpp)
        ppSubMark(1);
        // Gameplay rotation triggers this group's advance reaches (model.hpp fireRotTriggers).
        model.fireRotTriggers(gframe, grev, gLo, gHi, wLo, wHi, t);
        // The model builds this group's tick (model.hpp prepareGroup): the moving geometry placed
        // for tick t, the doors the group has opened, the windows, the reachable touch boxes and
        // the coins where they are now.
        // --autoown (dynamics.hpp): this group's own crossings for the autonomous triggers, for its
        // placement and its step; the leader's come back when the group is done.
        AutoOwnGuard autoOwn;
        g_autoOwnT0 = t0;   // --autoownpre (removed) (dynamics.hpp); the same value from every thread
        if (gframe == 0 && !grev) {
            if (g_stateCross && gRep != (size_t)-1) autoOwnApplyCross(autoOwn, gHi, (double)gdx, t, cur[gRep]);
            else autoOwnApply(autoOwn, gHi, (double)gdx, t);
        }
        ppSubMark(2);
        GroupEnv env;
        double pLo = wLo, pHi = wHi;
        p2Widen(pLo, pHi, gD2Lo, gD2Hi);   // --p2xoff
        // dupbatch, a group keyed without its ticks (a turned frame, a mover with no supply slot): its build notes the ticks it reads; whoever follows it must have them.
        // In a turned frame every object counts (the frame's Level is its own copy, and whatever table of supplied objects it carries does not hold there).
        FireReadSet dupRd;
        const bool dupRead = dupHere && !dupBuf && dupG[dupGi].live && dupG[dupGi].readTicks;
        if (dupRead) { dupRd.everyObject = gframe != 0; model.levelOf(gframe).dyn.fireSink = &dupRd; }
        if (!dupSkip)   // (dupbatch: a follower's states were stepped with the leader's tick; its own is not built)
        model.prepare(env, gframe, gtrig, gFire, gFireB, gLockOff + (float)sdx, x, xPrev, dxUsed,
                      pLo, pHi, t);
        if (dupRead) {
            model.levelOf(gframe).dyn.fireSink = nullptr;
            ++g_dupBatchRead[dupRd.all.load(std::memory_order_relaxed) ? 0 : (dupRd.count() || dupRd.scalar.load(std::memory_order_relaxed)) ? 1 : 2];
        }
        ppSubMark(3);
        const std::vector<const Obj*>& near = env.near;
        const std::vector<const Obj*>& ports = env.ports;
        const std::vector<const Obj*>& pads = env.pads;
        const std::vector<const Obj*>& orbs = env.orbs;
        const std::vector<const Obj*>& slps = env.slps;
        const std::vector<std::pair<const TouchTrig*, TouchMask>>& trigs = env.trigs;
        if (g_trigLive.on && g_trigLive.checkEvery > 0) {   // --trigshadowcheck N (trigshadow_live.hpp, observation only): the model's placed movers, in the level's own frame, for the shadow's timeline of this group
            g_trigLive.markSnap(t);
            if (g_trigLive.snapping()) {
                const auto s0 = std::chrono::steady_clock::now();
                for (const std::vector<const Obj*>* lp : {&near, &ports, &pads, &orbs, &slps})
                    for (const Obj* o : *lp) {
                        if (!o || !o->dynObj) continue;
                        double wx = o->cx, wy = o->cy;
                        if (gframe) fromFrame(gframe, o->cx, o->cy, wx, wy);
                        g_trigLive.snapObj(o->uid, wx, wy);
                    }
                g_trigLive.modelSnapMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
            }
        }
        const long long dupGround0 = g_trigLive.supGround;
        const auto lpSup0 = g_phaseProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};   // (--phaseprof / --layerprof: the supply's own time)
        if (!dupSkip && g_trigLive.supplyOn()) supplyPlaceMovers(env, gframe, grev);   // --trigsupply-group (trigshadow_live.hpp, P6-0): the movers of the supplied groups take the shadow's place for tick t (the block above, shared with the walk)
        // dupbatch: this build left a SUPPLIED mover on the model's own placement (its rect lies below y = 0, supGround). That placement was made from the group's ticks, and no set noted its
        // reads (the mover has a slot): whoever follows this group must have every tick of it. Seen here, for the build it happens in -- the count in the key (why 5) only knows the builds before.
        const bool dupGroundHere = g_trigLive.supGround != dupGround0;
        if (g_phaseProf) g_ppSupplyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - lpSup0).count();
        if (g_supObs && !dupSkip && gframe == 0 && g_trigLive.supplyOn()) supObsGroup(env, model.levelOf(0).dyn, pLo, pHi);   // --supobs (observation only)
        seen.ensure(cur.size() * 2 + 64);
        seen.clear();   // per GROUP, not per layer (see the note above)
        (void)x; (void)xPrev;
        const StepCtx K = dupSkip ? StepCtx{} : env.ctx();   // (a follower steps nothing here: no tick was built for it)
        ppMark(0);
        // ---- phase 1: STEP every state of this group (parallel) -------------
        // Stepping is pure -- it reads the shared windows and writes only its
        // own child -- so it parallelises exactly. The dedupe, the arena and
        // `nxt` stay on the main thread in phase 2, walked in the SAME order as
        // the serial loop was, which keeps the emitted plan bit-identical.
        gidx.clear();
        if (dupHere && dupOneFrame) gidx = gs[dupGi].members;   // dupbatch, a layer of one frame and direction: the test below is the group's own key, so its parents are its members
        else
        for (size_t i = 0; i < cur.size(); ++i)
            if (((cur[i].dx > 0.f) ? cur[i].dx : curDxF) == gdx
                && cur[i].trig == gtrig && sigOf(i) == gkey.fsig
                // --groupxsplit (removed): a member of this frame and direction goes to its own part only.
                // A parent from another frame (gidx hands those to every group, see above) keeps
                // being stepped once per frame group, in its part 0.
                && (curXc.empty()
                    || ((((int)cur[i].frame == gframe && (int)cur[i].rev == grev))
                            ? curXc[i] == gkey.xc
                            : gkey.xc == 0)))
                gidx.push_back(i);
        if (g_phaseProf) { ++g_ppGroups; g_ppParents += (long long)gidx.size(); }
        if (g_placeObs.on) g_placeObs.group(env, gframe, (long long)t, gdx, grev, gkey.fsig, (int)gkey.xc, x, cur, gidx);   // --placetable (place_table.hpp, P7-1): the placement this group made, filed under the worlds of its states (observation only)
        if (g_trigLive.on) { g_trigLive.parents((long long)gidx.size()); g_trigLive.members(arena, cur, gidx, gdx); }   // --trigshadowlive: the parents this group steps (observation only)
        if (DupObs::on()) {   // DUPOBS_OUT (observation only)
            g_dupObs.keyParts(gtrig, gFire, gFireB, gLockOff + (float)sdx, (g_stateCross && gRep != (size_t)-1) ? xcSig(cur[gRep], t) : 0, gRep != (size_t)-1 ? groupFireSig(cur[gRep], t) : 0, (int)gkey.xc);
            g_dupObs.group(env, gframe, grev, gdx, wLo, wHi, (long long)t, gidx.size());
        }
        // dupbatch: a leader takes the states of the later groups that have its key into its own pass (each follower's parents by the test of the loop above with the follower's key, after its own)
        const size_t dupOwn = gidx.size();
        std::vector<std::pair<uint32_t, size_t>> dupSeg;   // (follower, where its parents start in gidx)
        if (dupHere && !dupBuf && dupG[dupGi].live) {
            const auto fi = dupByKey.find(dupG[dupGi].ka ^ (dupG[dupGi].kb * 0x9e3779b97f4a7c15ULL));
            if (fi != dupByKey.end())
                for (uint32_t hj : fi->second) {
                    DupG& H = dupG[hj];
                    if ((size_t)hj <= dupGi || H.buffered || H.ka != dupG[dupGi].ka || H.kb != dupG[dupGi].kb) continue;
                    if (dupRead || dupGroundHere) {   // the ticks this leader's build read: the candidate's must be the same (it stays its own leader otherwise)
                        const GStat& A = gs[dupGi]; const GStat& B = gs[hj];
                        bool same = true;
                        if (dupGroundHere || dupRd.all.load(std::memory_order_relaxed)) { same = A.fire == B.fire; for (int b = 0; b < kTouchBits && same; ++b) same = A.fb[b] == B.fb[b]; }
                        else {
                            if (dupRd.scalar.load(std::memory_order_relaxed)) same = A.fire == B.fire;
                            for (int b = 0; b < kTouchBits && same; ++b) if (dupRd.test((size_t)b)) same = A.fb[b] == B.fb[b];
                        }
                        if (!same) { ++g_dupBatchRead[3]; continue; }
                    }
                    const GKey& hk = groupDx[hj];
                    const size_t at = gidx.size();
                    if (dupOneFrame) gidx.insert(gidx.end(), gs[hj].members.begin(), gs[hj].members.end());
                    else
                    for (size_t i = 0; i < cur.size(); ++i)
                        if (((cur[i].dx > 0.f) ? cur[i].dx : curDxF) == hk.dx && cur[i].trig == hk.trig && sigOf(i) == hk.fsig
                            && (curXc.empty() || ((((int)cur[i].frame == (int)hk.frame && (int)cur[i].rev == (int)hk.rev)) ? curXc[i] == hk.xc : hk.xc == 0)))
                            gidx.push_back(i);
                    H.buffered = true;
                    H.gidx.assign(gidx.begin() + (std::ptrdiff_t)at, gidx.end());
                    dupSeg.push_back({hj, at});
                    ++g_dupBatchFollowers; g_dupBatchParents += (long long)(gidx.size() - at);
                }
            if (!dupSeg.empty()) ++g_dupBatchLeaders;
        }
        // --camy with dupbatch: the camera input of each parent of this pass -- the leader's own for its parents, each follower's own for the parents it brought (the key says the tick the model builds
        // is one; the camera input is the group's and is read per child, so it is handed per parent instead of being asked to agree)
        std::vector<const CamYGroup*> kidCam;
        if (g_camy && !dupSeg.empty()) {
            kidCam.assign(gidx.size(), &camyG);
            for (size_t k = 0; k < dupSeg.size(); ++k) {
                const size_t a = dupSeg[k].second, b = k + 1 < dupSeg.size() ? dupSeg[k + 1].second : gidx.size();
                for (size_t j = a; j < b; ++j) kidCam[j] = &dupG[dupSeg[k].first].camY;
            }
        }
        // Airborne cube input is normally moot, so only one child is expanded --
        // but NOT next to an orb, where a press in mid-air is the whole point.
        // Leaving this unconditional silently threw away every orb activation
        // (lv3: 0 boosts in 35k expansions). Decided per state up front so that
        // slot 2i+1 either holds that state's second child or nothing.
        // ...and NOT while DASHING, where the input is the only thing holding the
        // player up. A dash ends the tick after the button comes off (the
        // `s.dashing && s.action` rule in the step), so pruning the pressed child
        // means the frontier can never keep a dash alive further than 50 px past
        // the last orb in the window -- the dash is forcibly released the moment
        // the ring leaves the window, whatever the level needs.
        // Measured on lv21 (2026-08-10): the dash ring at (2445,229) is the only
        // way across the pit at x=2550..2700 (no floor at all, spikes at y<=181).
        // At t=1966 the layer expanded 1,402 pressed children; at t=1967 it
        // expanded ZERO, so every dashing state was released at t=1968 (x=2,557)
        // and fell into the pit. GD with the same press held to t=2200 reaches
        // x=2,996. The frontier collapse the previous session blamed on
        // granularity was this: the winning branch was never enumerated.
        kids.assign(gidx.size() * 2, Child{});
        kidKeys.assign(gidx.size() * 2, 0);
        kidFlag.assign(gidx.size() * 2, 0);
        const bool dbgHere = (dbgLayers > 0 && t - t0 <= dbgLayers);
        const bool orbsEmpty = orbs.empty();
        // --twinskip / --twinaudit (search_census.hpp): is this parent's pressed child a twin
        // of its released one? Only the airborne ground modes, whose button does nothing in
        // the air except through the things excluded here: something the press fires (a
        // ring or portal close enough to matter this tick, a toggle block, a tap or
        // the counting tap), a hold the release would change (ringHold / pressSpent /
        // holdDead, the robot's hover, a dash), and GD's mid-air coyote jump (ogLinger).
        // Off wherever an instrument reads the pressed child itself.
        bool groupPressTrig = false;
        for (const auto& tb : trigs)
            if (tb.first->tap || tb.first->press) { groupPressTrig = true; break; }
        // --heldall writes the button into `held` in every mode, and keyOf keys `held` in every
        // mode, so under it the two children part in the key and the pressed one is no longer
        // deduped away behind its released twin: it is kept, and it has to be the child stepping
        // would have made. The fill below sets what the button wrote (held, the jump buffer) and
        // takes the key again; --twinaudit checks that against the stepped child.
        const bool twinOn = !dbgHere && !g_refWatch
                            && !g_clearProbe && !qfHere && g_coinDbgT < 0 && !g_airPress
                            && false   // --latgap was always on, and the twin skip never ran with it
                            && !(g_rotWatchLo >= 0 && t >= g_rotWatchLo && t <= g_rotWatchHi);
        kidMoot.assign(twinOn ? gidx.size() : 0, 0);
        // UNREACHED while twinOn carries `&& false`: no run asks this, so nothing has checked it
        // against the input latency the step now models (--latgap). It stays here, in the search,
        // rather than in the model's plugs (model.hpp) until --twinaudit can vouch for it again.
        auto pressMoot = [&](const State& s) -> bool {
            if (s.dual || s.grounded || s.onSlope) return false;
            if (s.mode != 0 && s.mode != 2 && s.mode != 5 && s.mode != 6) return false;
            if ((int)s.frame != gframe || (int)s.rev != grev) return false;
            if (s.ringHold || s.pressSpent || s.rHover || s.dashing || s.ogLinger || s.holdDead)
                return false;
            // Inside the flip grace a head-side contact seats the body (step.hpp, flipGraceSeat),
            // and whether it does reads the press: a ball's tap after the collision pass keeps the
            // window open, so the pair can part there. No corpus state has shown it on this path;
            // the gravity pad below is the same window opened on the tick itself, and it has.
            if ((int)s.flipT + 1 < kFlipGraceTicks) return false;
            if (nearPressBox(s)) return false;
            if (model.pressMatters(s)) return false;
            // A fixup record is matched per INPUT (fixup.hpp fixupMatches), so one landing on
            // either child splits the pair -- measured in a loop call of a custom level at
            // t=4,325, an upside-down ball whose released child took a +2.83 vy record.
            // Any record within a few px of the parent's x rules the pair out.
            for (const std::vector<Fixup>* fv : {&g_fixupKills, &g_fixupDeltas}) {
                const float lo = s.xAbs - 8.f;
                const auto it = std::lower_bound(
                    fv->begin(), fv->end(), lo,
                    [](const Fixup& f, float xx) { return f.x < xx; });
                if (it != fv->end() && it->x <= s.xAbs + 8.f) return false;
            }
            const double px = (double)s.xAbs, py = (double)s.y;
            const double rx = 60.0 + 2.0 * std::fabs((double)K.dxF);
            const double ry = 60.0 + std::fabs((double)s.vy);
            auto within = [&](const std::vector<const Obj*>& v) {
                for (const Obj* o : v)
                    if (std::fabs(o->cx - px) <= o->hw + rx && std::fabs(o->cy - py) <= o->hh + ry)
                        return true;
                return false;
            };
            // Rings: the contact test reads this tick's and the previous tick's position, and a
            // rotated ring reaches 18 + 15*sqrt(2) = 39.2 px along its axis; the margin covers
            // both. Portals: a mode portal can hand the press to the new mode on its tick
            // (--portalpress (removed)). Pads read no input in the air, so they are not a reason.
            // Ramps: a contact made on this tick goes through slopeNudge, which reads the
            // input (a custom level t=15,834, an airborne cube at vy 0: released -2.000, pressed
            // -0.216).
            // A gravity pad (type 10) flips the body in the collision pass, which opens the flip
            // grace above from any flipT (flipGraceSeat's age 0), so one in reach rules the pair out
            // too. Gravity portals are among `ports` already. The twinaudit witnesses are this
            // path: lv11 t=8,968 and lv12 t=18,838, a ball whose pressed child is seated at vy 0
            // while the released one leaves at -3.072.
            for (const Obj* o : pads)
                if (o->type == 10 && std::fabs(o->cx - px) <= o->hw + rx
                    && std::fabs(o->cy - py) <= o->hh + ry)
                    return false;
            return !within(orbs) && !within(ports) && !within(slps);
        };
        // --inputgrid / --gridmap: this layer's grid, from the leader x of the layer before it
        // (the x --capmap reads, so a ladder window lifts both over the same stretch).
        const int gridNow = gridAtX(bestX);
        // --airpressnear (removed) (with --airpress): the air press child is kept only where a landing is within
        // reach. A press held from the air into a landing is one future whatever tick in the arc it began
        // on (the buffer is set at the first and the states merge once it is), so the children of the
        // ticks long before any surface add expansions and no reachable state.
        auto landingNear = [&](const State& s) -> bool {
            constexpr double n = 8.0, g = 0.958;
            const double px = (double)s.xAbs, vy = (double)s.vy;
            const double foot = (double)s.y - 15.0;
            const double lb = foot - 0.5 * g * n * n - std::max(0.0, -vy) * n;
            const double ub = foot + std::max(0.0, vy) * n + 1.0;
            const double rx = 15.0 + n * std::fabs((double)K.dxF);
            auto hit = [&](const std::vector<const Obj*>& v, bool solidOnly) {
                for (const Obj* o : v) {
                    if (solidOnly && o->type != 0) continue;
                    const double top = o->cy + o->hh;
                    if (top >= lb && top <= ub && std::fabs(o->cx - px) <= o->hw + rx) return true;
                }
                return false;
            };
            return hit(near, true) || hit(slps, false);
        };
        // A cube already HOLDING the button in the air, with a ring within the reach of this flight. GD tells a hold that has already made a jump from a fresh
        // press: carried into a landing it jumps again at once, and it does not fire a ring the body is touching there, where a fresh press fires the ring. The
        // air prune below made every jump a press let go on the next tick, so "held through the jump, landing beside a ring" was not a state of the search at
        // all (a custom level, t=2675..2800: the only way past its wall is a hold from the ceiling through the landing next to a hidden ring; the frontier
        // died at t=2882 with nothing dropped at cap 64000). A cube in the air with the button held has no say in where it goes: until it lands its path is
        // the one arc its y and vy give (the cube's gravity up to its terminal speed, kCubeG / kCubeTerm; y += 0.225 * vy a tick, state.hpp; this tick's x
        // speed). The landing is a point of that arc, so "lands beside a ring" is "the arc passes through a ring's box": the held child is kept while it does,
        // followed until the body has left the level's height (0 .. the level's maxY). With no ring on the arc the hold cannot differ from a press on the
        // landing tick, and it is pruned as before. Fixed rings only (Level::orbs): a moving ring is in the near list when it is close. The arc is the
        // cube's plain one (no gravity modifier, no speed portal on the way): it picks which states keep a child, it is not a step of the model.
        // (The first form kept the child while any ring lay between here and where a fall to the level's edge would end, whatever its height. On the official
        // levels that doubled airborne states in their cap class far from any ring the flight could touch: lv22's anchor call from t=1986 went from 22 s to
        // 252 s -- eight attempts and a plain search at a wall the base passes on its third attempt -- and lv11 and lv16 each gained iterations, 2026-11-20.)
        double orbMaxHw = 0.0;   // the widest fixed ring of this frame's level (the rings are sorted by cx, so the scan below starts that far back)
        for (const Obj& o : model.levelOf(gframe).orbs) orbMaxHw = std::max(orbMaxHw, (double)o.hw);
        // ...and the MOVING rings (Dynamics bucket ORB): a level whose rings are all movers has an empty fixed list (a custom level: level: 0 orbs, 35396 moving),
        // so the hold was pruned everywhere there. A mover's place along the arc's ticks is not this tick's; it is bounded instead by every place its rows
        // record and the one it has now (heldRingBoxesOf), and the arc meeting that box keeps the child -- a box too large only keeps more children. A mover with
        // no rows (its place comes from its triggers alone) has no bound from its rows in x or in y: it may be anywhere in the level (MovRingBox::anywhere), and
        // any held arc keeps the child while the level has one. (The first form spanned only the level's height at the mover's x at the build: a trigger that
        // moves it sideways took it out of that box, and a held child meeting it there was pruned -- a miss, where the test may only keep too many.)
        const MovRingBoxes& movRings = heldRingBoxesOf(model.levelOf(gframe).dyn);
        auto heldRingInReach = [&](const State& s) -> bool {
            const Level& LV = model.levelOf(gframe);
            if (LV.orbs.empty() && movRings.box.empty()) return false;
            const double g = s.flip ? -kCubeG : kCubeG, vt = std::fabs(kCubeTerm), half = playerHalf(s.mode, s.mini != 0);
            double x = (double)s.xAbs, y = (double)s.y, vy = (double)s.vy;
            const double dx = (double)K.dxF;
            if (dx == 0.0) return false;
            if (movRings.anywhere > 0) return true;
            while (y >= 0.0 && y <= LV.maxY) {
                vy = std::max(-vt, std::min(vt, vy + g));
                y += 0.225 * vy;
                x += dx;
                auto it = std::lower_bound(LV.orbs.begin(), LV.orbs.end(), x - orbMaxHw - half, [](const Obj& o, double xx) { return o.cx < xx; });
                for (; it != LV.orbs.end() && it->cx - it->hw - half <= x; ++it)
                    if (std::fabs(it->cx - x) <= it->hw + half && std::fabs(it->cy - y) <= it->hh + half) return true;
                for (const MovRingBox& b : movRings.box)
                    if (x >= b.x0 - half && x <= b.x1 + half && y >= b.y0 - half && y <= b.y1 + half) return true;
            }
            return false;
        };
        // The air prune of a pressed child, without the hold-towards-a-ring exception: the button does nothing for this cube on this tick.
        auto airPruned = [&](const State& s, bool edgeLocked, int forcedIn) -> bool {
            // (always on: the switch --airpressnear is removed)
            // --airpressnear (removed) (with --airpress): the air press child is expanded only where a surface lies within
            // reach of a landing (search_dp.hpp landingNear). The press held into a landing is one future whichever tick of
            // the arc it began on, so --airpress alone pays for the children of every tick of every air arc.
            return forcedIn < 0 && s.mode == 0 && !s.grounded && orbsEmpty
                   && !s.dashing && !(s.dual && s.dashing2)   // --dualdash
                   && !nearPressBox(s) && !nearCtrlHoldBox((double)s.xAbs)
                   && !(g_airPress && landingNear(s))
                   && !nearHoldTap((double)s.xAbs)   // --holdtap (object.hpp)
                   && !(edgeLocked && s.action == 1);
        };
        // Is the pressed child of `s` one that only the hold-towards-a-ring exception keeps (the same reading stepKidAt makes of it)?
        auto heldRide = [&](const State& s) -> bool {
            if (s.mode != 0 || s.grounded || s.action != 1) return false;
            const int gridS = (g_gridGroundPress && gridNow > 1 && (K.t % gridNow) != 0 && ((s.grounded && (!g_gridGroundPressCube || s.mode == 0)) || (s.mode == 0 && landingNear(s)))) ? 1 : gridNow;
            return airPruned(s, model.edgeLocked(s, gridS, K.t), mustPressAt((long long)K.t)) && heldRingInReach(s);
        };
        auto stepKidAt = [&](size_t i, bool mayTwin) {
            const State& s = cur[gidx[i >> 1]];
            const int input = (int)(i & 1);
            const bool ppK = g_phaseProf;   // --phaseprof: this child's step by part (progress.hpp g_ppKidSeg)
            auto ppT = ppK ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
            auto ppSeg = [&](int k) {
                if (!ppK) return;
                const auto n = std::chrono::steady_clock::now();
                g_ppKidSeg[k].fetch_add((long long)std::chrono::duration_cast<std::chrono::nanoseconds>(n - ppT).count(), std::memory_order_relaxed);
                ppT = n;
            };
            // --gridgroundpress (search_census.hpp): the grid does not lock a grounded state, nor a cube with a landing within reach (the press on the landing tick is a GD jump too)
            const int gridS = (g_gridGroundPress && gridNow > 1 && (K.t % gridNow) != 0 && ((s.grounded && (!g_gridGroundPressCube || s.mode == 0)) || (s.mode == 0 && landingNear(s)))) ? 1 : gridNow;
            // May the button change on this tick (model.hpp edgeLocked)?
            const bool edgeLocked = model.edgeLocked(s, gridS, K.t);
            const int forcedIn = mustPressAt((long long)K.t);   // --mustpress: the tick's button is given
            if (forcedIn >= 0 && input != forcedIn) return;
            if (forcedIn < 0 && edgeLocked && input != (int)s.action) return;
            // ...and NOT next to a toggle block it has not fired (nearPressBox), which the
            // button fires: SubZero 4003's uid 4001 is passed by an airborne cube, so with
            // the pressed child pruned no state ever entered it (2026-09-24).
            // ...and NOT under --airpress (g_airPress): a press held from the air into a
            // landing is a different jump from one pressed on the landing tick.
            // ...and not when the input rule has just forbidden the release: then the held child
            // is the only one left.
            if (input == 1 && airPruned(s, edgeLocked, forcedIn)
                && !(s.action == 1 && heldRingInReach(s)))   // ...and not a hold carried towards a ring (heldRingInReach; the child rides with its released sibling at the cap, rideNodes)
                return;
            // --latgap: no edge on a tick GD cannot put one (model.hpp inputAllowed)
            if (!model.inputAllowed(s, input, gridS, K.t))
                return;
            // --twinskip: decided after the step pass, from the released sibling (slot i-1):
            // copied in when that one stayed in the air, stepped for real otherwise.
            if (input == 1 && forcedIn < 0 && mayTwin && twinOn && pressMoot(s)) {
                if (!g_twinAudit) {
                    kidFlag[i] = 3;
                    return;
                }
                kidMoot[i >> 1] = 1;
            }
            Child& kid = kids[i];
            bool dead = false;
            // The model's tick for this state and input (model.hpp advanceState): the physics,
            // then the gameplay rotation if the tick crossed one -- BEFORE keyOf below, which must
            // key the child in the frame it ended in.
            RotNote rot;
            if (g_camy) camyChildBegin();   // --camy S2.1: this thread's portal event is cleared, then read as this child's own
            ppSeg(0);
            if (ppK) g_ppKidStepped.fetch_add(1, std::memory_order_relaxed);
            kid.s = model.advance(s, input, K, dead, &rot);
            ppSeg(1);
            if (g_camy) camyChild(s, kid.s, (long long)t, kidCam.empty() ? camyG : *kidCam[i >> 1], (float)g_maxPlayY);
            if (rot.set) {   // --rotwatch: print only, read in the serial pass below
                kid.rotUid = rot.uid;
                kid.rotF0 = rot.f0;
                kid.rotNf = rot.nf;
            }
            // The child's own action, set HERE and not only in emit()'s
            // finish(): keyOf reads it (for the robot's hover) and the key is
            // computed on this thread, before phase 2 runs. Without this the
            // key saw the PARENT's action and the held and released children
            // hashed to the same cell -- which is the whole bug the rHover
            // note in keyOf describes.
            // (model.hpp noteInput: the action, --minpulse's edge age, --latgap's lock.)
            model.noteInput(s, kid.s, input);
            // --worldkey (modifiers.hpp): the camera summary, the x curve at its change points (trigevent_camera.hpp dxHistStep)
            if (g_worldKey) kid.s.dxHist = dp::gdtrig::dxHistStep(s.dxHist, s.dx, kid.s.dx, (double)s.xAbs, (double)kid.s.xAbs, (long long)t);
            // the groups' on / off and the x-gate digest (trigevent_togstate.hpp)
            togWorldStep(s, kid.s, input);
            // --statecross: the late crossings of autonomous triggers this step made (search_key.hpp)
            if (g_stateCross && !dead) stateCrossNote(kid.s, s, t);
            // ---- --coins: collect, then kill whoever left one behind (rules.afterStep) ----
            model.fireGates(kid.s, s.action, input, K, gframe, t, dead);
            rules.afterStep(s, kid, input, K, gframe, t, dead);
            // --needsize: a state that is not the size the wall needs is dropped on the tick named
            if (!dead && t == g_needSizeT && (int)kid.s.mini != g_needSizeMini)
                DIE_AS(kDeathRule, "size/required", nullptr);
            // --needspeed: a state that crosses the wall's x at a speed the rule does not allow is dropped on the tick it does
            if (!dead && g_needSpeedX >= 0.0 && (double)s.xAbs < g_needSpeedX && (double)kid.s.xAbs >= g_needSpeedX
                && ((int)speedCodeForDx((double)kid.s.dx) == g_needSpeedCode) == g_needSpeedNot)
                DIE_AS(kDeathRule, "speed/required", nullptr);
            // --deadband x0,x1[,mode[,y0,y1]] (modifiers.hpp): a box of WORLD coordinates the search must not be in -- the repair loop's phantom veto draws one around GD's dump state, and the wedge
            // hint ("crossed a mode portal in the wrong mode") forces the crossing with `--deadband x0,x1,<mode>`. The rule is the old tick's, as it stood at its removal:
            // [R] a589807d dp/src/dp/step.hpp:3250-3270, inside stepOne:
            //     if (!g_deadBands.empty()) { double dbX = x, dbY = (double)s.y; if (s.frame & 3) fromFrame((int)s.frame, x, (double)s.y, dbX, dbY);
            //       for (const auto& db : g_deadBands) { if (dbX < db.x0 || dbX > db.x1 || dbY < db.y0 || dbY > db.y1) continue;
            //         const bool hit = (db.mode == -1) || (db.mode >= 0 && (int)s.mode != db.mode) || (db.mode <= -2 && (int)s.mode == -2 - db.mode);
            //         if (hit) { dead = true; g_deadWhy = "deadband"; g_deadObj = nullptr; g_deadKind = kDeathPhysics; ... return c; } } }
            // with x = c.xAbs (3119, this tick's x step), s.y and s.mode the ENTRY state's, the frame the entry state's. Under --shadowstep=all the tick is the shadow's and stepOne does not run, and the E9 removal
            // took the block with stepOne, so the boxes stopped acting: coin lv22 (E10_coins, 11-18) wedged twice at (20115, 829) with `--deadband 19050,19080,5` in the call that planned it, and the
            // plan passed the robot portal (19005, 705) as a ship. x is the child's own where the tick stayed in the frame, else the plain x step (a turn rewrites xAbs into the new frame).
            if (!dead && !g_deadBands.empty()) {
                const double dbx = (kid.s.frame == s.frame) ? (double)kid.s.xAbs
                                                              : (double)advanceX(s.xAbs, ((s.dx > 0.f) ? s.dx : K.dxF) * (s.rev ? -1.f : 1.f));
                double dbX = dbx, dbY = (double)s.y;
                if (s.frame & 3) fromFrame((int)s.frame, dbx, (double)s.y, dbX, dbY);
                for (const auto& db : g_deadBands) {
                    if (dbX < db.x0 || dbX > db.x1 || dbY < db.y0 || dbY > db.y1) continue;
                    const bool hit = (db.mode == -1) || (db.mode >= 0 && (int)s.mode != db.mode) || (db.mode <= -2 && (int)s.mode == -2 - db.mode);
                    if (hit) { DIE_AS(kDeathPhysics, "deadband", nullptr); break; }
                }
            }
            // Route tightness rides along with the child (State c = s copies it
            // already, so this only adds this tick's own contribution). Read by
            // the goal pick below; nothing else looks at it.
            if (!dead && kid.s.tight < 0xffff
                && tightHere(near, (double)kid.s.xAbs, (double)kid.s.y,
                             playerHalf(kid.s.mode, kid.s.mini != 0), kTightPx))
                ++kid.s.tight;
            // --wavetidy / --wavetoggle: a wave's tick is charged for how far it sits from the middle of its corridor,
            // and a change of the button for itself (thread_pool.hpp); saturating, never wrapping.
            if (!dead && (g_waveTidy > 0.0 || g_waveToggle > 0) && kid.s.mode == 4 && kid.s.frame == 0) {
                int add = 0;
                if (g_waveTidy > 0.0) {
                    const double half = playerHalf(kid.s.mode, kid.s.mini != 0);
                    const double off = waveOffCentre(near, (double)kid.s.xAbs, (double)kid.s.y, half,
                                                     (double)kid.s.bandFloor, (double)kid.s.bandCeil,
                                                     kid.s.mini ? kWaveClampMini : kWaveClamp);
                    add += (int)(g_waveTidy * off * off);
                }
                if (g_waveToggle > 0 && (int)s.action != input) add += g_waveToggle;
                kid.s.tight = (uint16_t)std::min(0xffff, (int)kid.s.tight + add);
            }
            kid.dead = dead ? 1 : 0;
            kid.deadKind = dead ? g_deadKind : 0;
            kid.valid = 1;
            // the dedupe key is a pure function of the child, so it belongs on
            // this thread rather than in the serial phase
            ppSeg(2);
            if (!dead) kid.key = modelKeyOf(kid.s, (long long)t) ^ rules.keyBits(kid.s);
            ppSeg(3);
            // fireB's invariant, checked where every surviving child passes.
            // (i) is invisible to every replay harness -- dedupe is not on the
            // replay path -- so "quick_regress is byte-identical" says the
            // change was not SEEN, not that it is right. This is the direct
            // witness that the array is written where it should be: a tick
            // present for a box the state never entered, or a box entered with
            // no tick, is an implementation defect and nothing else would
            // catch it.
            if (!dead && g_fireBCheck) {
                for (int b = 0; b < kTouchBits; ++b) {
                    const bool bit = kid.s.trig.test(b);
                    const bool has = kid.s.fireB[b] != 0;
                    if (bit && !has) ++g_fireBNoTick;
                    if (!bit && has) ++g_fireBNoBit;
                    // ...and against an INDEPENDENT tick. g_touchFireT[b] is
                    // written in a different scope (level-wide, first entry
                    // only), so "no state entered box b before the first time
                    // any state entered box b" is a real check on the index and
                    // the value rather than a restatement of the line that
                    // wrote them. An anchored run seeds fireB from --start, so
                    // those may legitimately predate it and are skipped.
                    if (bit && has && g_touchFireT[b] >= 0
                        && (int)kid.s.fireB[b] < g_touchFireT[b]
                        && !init.trig.test(b))
                        ++g_fireBTooEarly;
                }
            }
            // compact side arrays for phase 2p: the shard scans read 9 B per
            // child instead of dragging the whole Child through the cache
            kidKeys[i] = kid.key;
            kidFlag[i] = dead ? 1 : 2;
            if (dbgHere || g_refWatch) { kid.why = g_deadWhy; kid.obj = g_deadObj; }
            ppSeg(4);
        };
        auto stepKid = [&](size_t i) {
            if (!g_phaseProf) { stepKidAt(i, true); return; }
            const auto k0 = std::chrono::steady_clock::now();   // --phaseprof: the work inside the step, per child
            stepKidAt(i, true);
            g_ppKidNs.fetch_add((long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - k0).count(),
                                std::memory_order_relaxed);
        };
        // --dbg stays serial: it prints per child, in order, and the reason
        // globals are per thread.
        const auto pf0 = g_phaseProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        if (dupSkip) {   // dupbatch: this group's children as the leader's pass made them
            const DupG& B = dupG[dupGi];
            kids = B.kids; kidKeys = B.keys; kidFlag = B.flags;
        } else if (pool && !dbgHere && kids.size() >= kParallelMin)
            pool->parallelFor(kids.size(), stepKid);
        else {
            for (size_t i = 0; i < kids.size(); ++i) stepKid(i);
            if (g_phaseProf) ++g_ppSerialGroups;
        }
        if (dupBuf && g_dupBatchCheck) {   // --dupbatchcheck: this follower was stepped for itself above; the leader's pass must have made the same children
            const DupG& B = dupG[dupGi];
            bool same = B.kids.size() == kids.size() && B.gidx == gidx;
            // (the canonical record of state_serial.hpp, not the State's bytes: a state names its shadow body by a slot of the store (slRef), and the two passes took different slots for the same body)
            const stser::Stats keepStats = stser::g_serialStats;
            std::vector<uint8_t> ra, rb;
            size_t bad = (size_t)-1; int badWhy = 0;   // the first child that differs, and in what: 1 its flag, 2 its dedupe key, 3 a field of the canonical record, 4 a camera field
            for (size_t i = 0; same && i < kids.size(); ++i) {
                if (B.flags[i] != kidFlag[i]) { same = false; bad = i; badWhy = 1; break; }
                if (kidFlag[i] != 2) continue;
                if (B.keys[i] != kidKeys[i]) { same = false; bad = i; badWhy = 2; break; }
                ra.clear(); rb.clear(); stser::serializeState(B.kids[i].s, ra); stser::serializeState(kids[i].s, rb);
                if (ra != rb) { same = false; bad = i; badWhy = 3; break; }
                if (g_camy) {   // (the camera fields --camy carries in the State have no row in the serializer: compared here, bit for bit)
                    const State& x = B.kids[i].s; const State& y = kids[i].s;
                    if (std::memcmp(&x.camY, &y.camY, sizeof x.camY) || x.camPortal != y.camPortal || std::memcmp(&x.camTwStart, &y.camTwStart, sizeof x.camTwStart)
                        || std::memcmp(&x.camDy, &y.camDy, sizeof x.camDy) || std::memcmp(&x.camTan, &y.camTan, sizeof x.camTan)) { same = false; bad = i; badWhy = 4; break; }
                }
            }
            stser::g_serialStats = keepStats;
            ++g_dupBatchChecked;
            if (!same && ++g_dupBatchMismatch <= 20) {
                std::printf("dupbatch: MISMATCH at t=%lld: a follower's own children are not the ones its leader's tick made (group %zu of %zu, %zu parents)\n", (long long)t, dupGi, groupDx.size(), gidx.size());
                if (bad == (size_t)-1) std::printf("dupbatch:   the two passes do not hold the same parents (%zu / %zu children)\n", B.kids.size(), kids.size());
                else {
                    const State& x = B.kids[bad].s; const State& y = kids[bad].s; const State& par = cur[gidx[bad >> 1]];
                    std::printf("dupbatch:   child %zu (parent %zu, input %d; parent x %.4f y %.4f vy %.4f mode %d): %s; flags leader's %d own %d, keys %016llx / %016llx\n", bad, bad >> 1, (int)(bad & 1), (double)par.xAbs, (double)par.y,
                                (double)par.vy, (int)par.mode, badWhy == 1 ? "the flag" : badWhy == 2 ? "the dedupe key" : badWhy == 3 ? "the record" : "a camera field", (int)B.flags[bad], (int)kidFlag[bad],
                                (unsigned long long)B.keys[bad], (unsigned long long)kidKeys[bad]);
                    if (B.flags[bad] == 2 && kidFlag[bad] == 2) {
                        ra.clear(); rb.clear(); stser::serializeState(x, ra); stser::serializeState(y, rb); stser::g_serialStats = keepStats;
                        std::vector<std::pair<uint32_t, uint32_t>> sa, sb;
                        stser::fieldSlices(ra, sa); stser::fieldSlices(rb, sb);
                        const auto& fl = stser::fields();
                        int shown = 0;
                        for (size_t k = 0; k < fl.size() && k < sa.size() && k < sb.size() && shown < 12; ++k) {
                            const bool eq = sa[k].second == sb[k].second && sa[k].first + sa[k].second <= ra.size() && sb[k].first + sb[k].second <= rb.size()
                                            && !std::memcmp(ra.data() + sa[k].first, rb.data() + sb[k].first, sa[k].second);
                            if (eq) continue;
                            ++shown;
                            std::printf("dupbatch:     field %s (%c): leader's pass", fl[k].name, fl[k].kind);
                            for (uint32_t b = 0; b < sa[k].second && b < 16 && sa[k].first + b < ra.size(); ++b) std::printf(" %02x", ra[sa[k].first + b]);
                            std::printf(" | own");
                            for (uint32_t b = 0; b < sb[k].second && b < 16 && sb[k].first + b < rb.size(); ++b) std::printf(" %02x", rb[sb[k].first + b]);
                            if (fl[k].kind == 'f' && sa[k].second == 4 && sb[k].second == 4) { float fa, fb; std::memcpy(&fa, ra.data() + sa[k].first, 4); std::memcpy(&fb, rb.data() + sb[k].first, 4); std::printf("  (%.6f | %.6f)", (double)fa, (double)fb); }
                            std::printf("\n");
                        }
                        if (g_camy) std::printf("dupbatch:     camera: leader's pass camY %.4f portal %d start %.4f | own camY %.4f portal %d start %.4f\n", (double)x.camY, (int)x.camPortal, (double)x.camTwStart, (double)y.camY, (int)y.camPortal, (double)y.camTwStart);
                    }
                }
            }
        }
        if (!dupSeg.empty()) {   // dupbatch: the followers' children leave the leader's arrays, each to wait for its group's turn
            for (size_t k = 0; k < dupSeg.size(); ++k) {
                DupG& H = dupG[dupSeg[k].first];
                const size_t a = dupSeg[k].second * 2, b = (k + 1 < dupSeg.size() ? dupSeg[k + 1].second : gidx.size()) * 2;
                H.kids.assign(kids.begin() + (std::ptrdiff_t)a, kids.begin() + (std::ptrdiff_t)b);
                H.keys.assign(kidKeys.begin() + (std::ptrdiff_t)a, kidKeys.begin() + (std::ptrdiff_t)b);
                H.flags.assign(kidFlag.begin() + (std::ptrdiff_t)a, kidFlag.begin() + (std::ptrdiff_t)b);
            }
            kids.resize(dupOwn * 2); kidKeys.resize(dupOwn * 2); kidFlag.resize(dupOwn * 2); gidx.resize(dupOwn);
        }
        if (g_phaseProf)
            g_ppStepNs += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pf0).count();
        // --twinskip: fill each skipped pressed slot with its released sibling, which is what
        // stepping it would have produced in everything the dedupe and the counts read.
        // --twinaudit: both were stepped; check that claim instead.
        // The claim needs the released sibling to have STAYED in the air, alive: a body that
        // lands on this tick jumps on this tick if the button is down (measured on lv22 t=372:
        // released vy 0.000, pressed 11.180, same y), and a death is not worth reasoning about.
        if (twinOn) {
            auto airborne = [&](size_t a) {
                const Child& k = kids[a];
                return kidFlag[a] == 2 && !k.s.grounded && !k.s.onSlope;
            };
            std::vector<size_t> redo;   // pending pressed slots whose sibling did not qualify
            for (size_t j = 0; j < gidx.size(); ++j) {
                const size_t a = 2 * j, b = a + 1;
                const State& p = cur[gidx[j]];
                const int m = p.mode < 8 ? p.mode : 0;
                // The pressed child as the fill makes it from the released one. Without --heldall
                // the two share a key and the pressed one never survives the dedupe, so only the
                // key is carried; under it the button's own writes are set too and the key taken
                // again (the note above twinOn).
                auto pressedFrom = [&](const Child& rel) {
                    Child k = rel;
                    k.s.action = 1;
                    if (g_minPulse > 1 || g_wavePulse > 1)   // --wavepulse reads edgeAge too
                        k.s.edgeAge = (p.action != 1)
                                          ? 0 : (uint8_t)std::min(254, (int)p.edgeAge + 1);
                    if (g_heldAll) {
                        k.s.held = 1;
                        k.s.jumpBuf = 1;
                        k.key = modelKeyOf(k.s, (long long)t) ^ rules.keyBits(k.s);
                    }
                    return k;
                };
                if (g_twinAudit) {
                    if (!kidMoot[j] || !airborne(a)) continue;
                    ++twin.predicted;
                    ++twin.byMode[m];
                    const int fa = kidFlag[a], fb = kidFlag[b];
                    bool ok = fa == fb;
                    if (ok && fa == 2) {
                        if (g_heldAll) {
                            const Child pk = pressedFrom(kids[a]);
                            ok = pk.key == kidKeys[b] && pk.s.vy == kids[b].s.vy
                                 && pk.s.y == kids[b].s.y && pk.s.trig == kids[b].s.trig
                                 && pk.s.held == kids[b].s.held
                                 && pk.s.jumpBuf == kids[b].s.jumpBuf;
                        } else {
                            ok = kidKeys[a] == kidKeys[b] && kids[a].s.vy == kids[b].s.vy
                                 && kids[a].s.trig == kids[b].s.trig;
                        }
                    }
                    if (!ok)
                        twin.violation(t, p, fa, fb, kids[a].s, kids[b].s, kidKeys[a],
                                       kidKeys[b]);
                } else if (kidFlag[b] == 3) {
                    if (!airborne(a)) {
                        kidFlag[b] = 0;
                        redo.push_back(b);
                        continue;
                    }
                    kids[b] = pressedFrom(kids[a]);
                    kidKeys[b] = g_heldAll ? kids[b].key : kidKeys[a];
                    kidFlag[b] = kidFlag[a];
                    ++twin.skipped;
                    ++twin.byMode[m];
                }
            }
            if (!redo.empty()) {
                auto stepRedo = [&](size_t k) { stepKidAt(redo[k], false); };
                if (pool && redo.size() >= kParallelMin)
                    pool->parallelFor(redo.size(), stepRedo);
                else
                    for (size_t k = 0; k < redo.size(); ++k) stepRedo(k);
            }
        }
        ppMark(1);
        // --searchcensus: did the two inputs of each parent lead anywhere different? Print only.
        if (g_searchCensus > 0)
            for (size_t j = 0; j < gidx.size(); ++j)
                census.siblings(cur[gidx[j]], kidFlag[2 * j], kidFlag[2 * j + 1], kids[2 * j].s,
                                kids[2 * j + 1].s, kidKeys[2 * j], kidKeys[2 * j + 1],
                                (long long)t);
        // --rotwatch: the rotations this group's children actually took, counted
        // serially after the parallel step. Print only.
        if (g_rotWatchLo >= 0 && t >= g_rotWatchLo && t <= g_rotWatchHi) {
            struct RW { int uid, f0, nf; size_t n; };
            std::vector<RW> rws;
            size_t nk = 0;
            for (const Child& k : kids) {
                if (!k.valid) continue;
                ++nk;
                if (k.rotNf < 0) continue;
                bool found = false;
                for (RW& r : rws)
                    if (r.uid == k.rotUid && r.f0 == k.rotF0 && r.nf == k.rotNf) { ++r.n; found = true; break; }
                if (!found) rws.push_back(RW{k.rotUid, (int)k.rotF0, (int)k.rotNf, 1});
            }
            for (const RW& r : rws)
                std::printf("rotwatch: t=%lld gdx=%.4f uid=%d f0=%d nf=%d kids=%zu/%zu\n",
                            (long long)t, (double)gdx, r.uid, r.f0, r.nf, r.n, nk);
        }

        // ---- reference watch, part 2: what happened to its child ------------
        // Every child has been stepped and keyed, and nothing has been merged
        // or capped yet, so this is where the reference's own continuation can
        // still be told apart from the ones that replaced it. Read-only, in the
        // same position and for the same reason as the clearance probe below.
        //
        // The parent may be stepped in more than one group when a level has
        // several live frames (gidx filters on dx and trig only), so an ALIVE
        // fate wins over a dead one rather than the last one written.
        if (g_refWatch && g_refParent >= 0) {
            for (size_t i = 0; i < kids.size(); ++i) {
                if ((int)gidx[i >> 1] != g_refParent) continue;
                const int a = (int)(i & 1);
                if (g_refKidFate[a] == 2) continue;   // already alive somewhere
                g_refKidFate[a] = (int)kidFlag[i];
                g_refKidWhy[a] = kids[i].why ? kids[i].why : "";
                g_refKidKey[a] = kidKeys[i];
                g_refKidState[a] = kids[i].s;
            }
        }

        // ---- clearance probe (instrumentation, --clearprobe) ----------------
        // Every child has been stepped and keyed by now, and this only READS
        // them -- no slot, no ordinal and no key is touched -- so the emitted
        // plan is the same with the flag on or off. That equality is the
        // acceptance test (py/quick_regress.py, bit-identical).
        // Serial on purpose: the stepping above is what needed the threads, and
        // a probe that took a lock per child would be measuring the lock.
        // --portaudit (port_audit.hpp; print only): do two candidates that share a dedupe key and differ only in portSeen part on the next tick?
        if (g_portAudit) {
            // (1) the pairs the layer before sampled whose group this is: step both one tick, each input, under this layer's context
            for (auto& pp : g_portAuditPending) {
                if (pp.done) continue;
                if (pp.t < t - 1) { pp.done = true; ++g_portAuditStats.expired; continue; }
                if (pp.t != t - 1) continue;
                if (!((((pp.a.dx > 0.f) ? pp.a.dx : curDxF) == gdx) && pp.a.trig == gtrig)) continue;
                portAuditStepPair(pp, [&](const State& s, int input, State& out, uint64_t& key) -> int {
                    if (!model.inputAllowed(s, input, gridNow, K.t)) return -1;
                    bool dead = false;
                    RotNote rot;
                    out = model.advance(s, input, K, dead, &rot);
                    model.noteInput(s, out, input);
                    if (dead) return 0;
                    key = modelKeyOf(out, (long long)t) ^ rules.keyBits(out);
                    return 1;
                });
            }
            g_portAuditPending.erase(std::remove_if(g_portAuditPending.begin(), g_portAuditPending.end(), [](const PortAuditPair& p) { return p.done; }), g_portAuditPending.end());
            // (2) this group's candidates, before the dedupe: the collision pairs, and a few of them kept for the next layer
            long long alive = 0, shared = 0;
            const auto prs = portAuditFind(kids, &alive, &shared);
            g_portAuditStats.kids += alive;
            g_portAuditStats.keyShared += shared;
            g_portAuditStats.pairs += (long long)prs.size();
            for (const auto& pr : prs)
                if (portSeenReal(kids[pr.first].s) || portSeenReal(kids[pr.second].s)) ++g_portAuditStats.pairsReal;
            if (!prs.empty()) ++g_portAuditStats.layers;
            static long long auditLayerT = -1;
            static int auditTaken = 0;
            if (auditLayerT != t) { auditLayerT = t; auditTaken = 0; }
            for (const auto& pr : prs) {
                if (auditTaken >= g_portAuditSample) break;
                PortAuditPair np;
                np.a = kids[pr.first].s;
                np.b = kids[pr.second].s;
                np.t = t;
                g_portAuditPending.push_back(np);
                ++auditTaken;
                ++g_portAuditStats.sampled;
            }
        }
        if (g_clearProbe) {
            clearKids.clear();
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kidFlag[i] != 2) continue;   // 1 = died, 0 = never expanded
                const State& c = kids[i].s;
                // A turned frame has "above" and "below" on a different axis;
                // measuring it here would mix two coordinate systems in one
                // histogram. Counted instead, so the omission is visible.
                if (c.frame != 0) { ++g_clear.framesSkipped; continue; }
                const double px = (double)c.xAbs, py = (double)c.y;
                ClearSample cs;
                cs.key = kidKeys[i];
                cs.vy = c.vy;
                cs.solid = solidClearance(near, px, py,
                                          playerHalf(c.mode, c.mini != 0),
                                          &g_clear.slopesSkipped);
                cs.haz = hazardClearance(near, px, py,
                                         hazardHalfFor(c.mode, c.mini),
                                         c.mode, c.mini);
                cs.mode = c.mode;
                cs.goal = (uint8_t)(c.frame == 0 && px >= goalX ? 1 : 0);
                clearKids.push_back(cs);
            }
            g_clear.addLayer(clearKids);
        }

        // --worldkey: the group's CHILDREN before its dedupe -- K = distinct dedupe keys, KD = distinct (key, dxHist) (trigevent_camera.hpp countKeys)
        if (g_worldKey) {
            std::vector<std::pair<uint64_t, uint64_t>> kh;
            kh.reserve(kids.size());
            for (const Child& kd : kids) if (kd.valid && !kd.dead) kh.emplace_back(kd.key, kd.s.dxHist);
            const dp::gdtrig::KeyCount kc = dp::gdtrig::countKeys(kh);
            auto& W = g_worldKeyStat;
            const long long gain = kc.kd - kc.k;
            ++W.candGroups; W.candK += kc.k; W.candKD += kc.kd; W.candMaxExtra = std::max(W.candMaxExtra, gain); if (gain > 0) ++W.candGroupsSplit;
            auto& B = W.cb[(long long)t / 2000];
            ++B.layers; B.k += kc.k; B.kd += kc.kd; B.maxExtra = std::max(B.maxExtra, gain);
        }
        // --camy S2.4 (observation only): per dedupe key of this group's live children, do their camY agree? If not, the camera Y is not a function of the key and the dedupe keeps one of them
        if (g_camy) {
            std::vector<std::pair<uint64_t, float>> kc;
            kc.reserve(kids.size());
            for (const Child& kd : kids) if (kd.valid && !kd.dead && kd.s.camY > -0.5f) kc.emplace_back(kd.key, kd.s.camY);
            std::sort(kc.begin(), kc.end());
            long long keys = 0, mixed = 0, mixed05 = 0, kidsIn = 0, kidsIn05 = 0; float maxSp = 0.0f;
            for (size_t a = 0; a < kc.size();) {
                size_t b = a + 1;
                while (b < kc.size() && kc[b].first == kc[a].first) ++b;
                ++keys;
                const float sp = kc[b - 1].second - kc[a].second;   // sorted by camY inside a key
                if (sp > 1e-3f) { ++mixed; kidsIn += (long long)(b - a); if (sp > 0.5f) { ++mixed05; kidsIn05 += (long long)(b - a); } maxSp = std::max(maxSp, sp); }
                a = b;
            }
            {   // S3-0 (a): the children that are a 1914 static with a known start, per key
                std::vector<std::pair<uint64_t, float>> sk;
                for (const Child& kd : kids) if (kd.valid && !kd.dead && kd.s.camPortal == 254 && kd.s.camTan > -1e29f && kd.s.camY > -0.5f) sk.emplace_back(kd.key, kd.s.camY);
                std::sort(sk.begin(), sk.end());
                long long sKeys = 0, m005 = 0, m025 = 0, m1 = 0; float maxS = 0.0f;
                for (size_t a = 0; a < sk.size();) {
                    size_t b2 = a + 1; while (b2 < sk.size() && sk[b2].first == sk[a].first) ++b2;
                    if (b2 - a >= 2) { ++sKeys; const float sp = sk[b2 - 1].second - sk[a].second; if (sp > 0.05f) ++m005; if (sp > 0.25f) ++m025; if (sp > 1.0f) ++m1; maxS = std::max(maxS, sp); }
                    a = b2;
                }
                g_camyObs.stChildren += (long long)sk.size(); g_camyObs.stKeys += sKeys; g_camyObs.stMixed005 += m005; g_camyObs.stMixed025 += m025; g_camyObs.stMixed1 += m1;
                const int sms = (int)std::min(1.0e6f, maxS * 1000.0f); if (sms > g_camyObs.stMaxMilli.load()) g_camyObs.stMaxMilli = sms;
            }
            ++g_camyObs.mxGroups; g_camyObs.mxKeys += keys; g_camyObs.mxMixed += mixed; g_camyObs.mxMixed05 += mixed05; g_camyObs.mxKids += kidsIn; g_camyObs.mxKids05 += kidsIn05;
            const int ms = (int)std::min(1.0e6f, maxSp * 1000.0f); if (ms > g_camyObs.mxMaxSpreadMilli.load()) g_camyObs.mxMaxSpreadMilli = ms;
        }
        // KEYOBS=1 (env; observation only; cli.hpp prints the `rotobs:` line): per dedupe key of this group's live children, do their angles agree? The key leaves the angle out (search_key.hpp), so
        // the dedupe keeps one child of a key whatever the others' are. Two counts: the raw angles differ, and they differ as a square's (the angle within a quarter turn; the first body only).
        if (gdcol::g_keyObs) {
            std::vector<std::pair<uint64_t, float>> kr;
            kr.reserve(kids.size());
            for (const Child& kd : kids) if (kd.valid && !kd.dead) kr.emplace_back(kd.key, kd.s.rot);
            std::sort(kr.begin(), kr.end());
            long long keys = 0, mixed = 0, mixedQ = 0, kidsQ = 0, byMode[3] = {0, 0, 0};
            std::vector<float> q;
            for (size_t a = 0; a < kr.size();) {
                size_t b = a + 1;
                while (b < kr.size() && kr[b].first == kr[a].first) ++b;
                ++keys;
                if (b - a >= 2 && kr[b - 1].second != kr[a].second) {   // (sorted by the angle inside a key)
                    ++mixed;
                    q.clear();
                    for (size_t k = a; k < b; ++k) q.push_back(kr[k].second - 90.0f * std::floor(kr[k].second / 90.0f));
                    std::sort(q.begin(), q.end());
                    float gap = q.front() + 90.0f - q.back();   // the largest gap of the angles on the quarter circle: the rest is the arc they span
                    for (size_t k = 1; k < q.size(); ++k) gap = std::max(gap, q[k] - q[k - 1]);
                    if (90.0f - gap > 1e-3f) { ++mixedQ; kidsQ += (long long)(b - a); }
                }
                a = b;
            }
            for (const Child& kd : kids) if (kd.valid && !kd.dead) ++byMode[kd.s.mode == 0 ? 0 : kd.s.mode == 2 ? 1 : 2];
            ++g_rotKeyObs.groups; g_rotKeyObs.kids += (long long)kr.size(); g_rotKeyObs.keys += keys; g_rotKeyObs.mixed += mixed; g_rotKeyObs.mixedQ += mixedQ; g_rotKeyObs.kidsQ += kidsQ;
            for (int k = 0; k < 3; ++k) g_rotKeyObs.byMode[k] += byMode[k];
        }
        // KEYOBS=1 (env; observation only; progress.hpp Boost2Obs, cli.hpp prints the `boost2obs:` line): what a dual's second exemption bit does to this group's keys before its dedupe.
        // (The band is asked of the State's float vy2, the game's of the image's double: a child on the band's edge may be counted on the other side.)
        if (gdcol::g_keyObs) {
            std::vector<uint64_t> k0, k1, k2;
            long long up[3] = {0, 0, 0};
            for (const Child& kd : kids) {
                if (!kd.valid || kd.dead || !kd.s.dual) continue;
                const State& z = kd.s;
                int cls = -1;
                if (z.boost2) {
                    const int m = (int)z.mode2;
                    const bool flies = m == 1 || m == 3 || m == 4 || m == 7, reads = m == 1 || m == 3 || m == 7;
                    const double v = (double)z.vy2, f22 = z.mini2 ? 0.85f : 1.0f;   // updateJump's clear95a (upd.hpp), the flying modes' size factor
                    const bool inBand = !z.flip2 ? ((0.0 <= v && v < (double)(8.0f / (float)f22)) || (v <= 0.0 && (double)(-6.4f / (float)f22) < v))
                                                 : ((v <= 0.0 && (double)-(8.0f / (float)f22) < v) || (0.0 <= v && v < (double)-(-6.4f / (float)f22)));
                    cls = (flies && inBand) ? 1 : reads ? 0 : 2;
                    ++up[cls];
                }
                k0.push_back(kd.key);
                k1.push_back(cls >= 0 ? kd.key ^ kKeyBoost2 : kd.key);
                k2.push_back(cls == 1 ? kd.key ^ kKeyBoost2 : kd.key);
            }
            if (!k0.empty()) {
                auto distinct = [](std::vector<uint64_t>& v) { std::sort(v.begin(), v.end()); return (long long)(std::unique(v.begin(), v.end()) - v.begin()); };
                ++g_boost2Obs.groups; g_boost2Obs.dualKids += (long long)k0.size();
                for (int k = 0; k < 3; ++k) g_boost2Obs.up[k] += up[k];
                g_boost2Obs.keys += distinct(k0); g_boost2Obs.keysNoBit += distinct(k1); g_boost2Obs.keysCanon += distinct(k2);
            }
        }
        // KEYOBS=1 (env; observation only; progress.hpp Boost1Obs, cli.hpp prints the `boostobs:` line): the same for the first body's bit (State::boost), over every live child. Its class
        // [1] is the narrower one the second body's write-back uses (upd.hpp exemptionClearedNext): a flying body, not dashing, no touched ring, vy inside the part the two bands share.
        if (gdcol::g_keyObs) {
            std::vector<uint64_t> k0, k1, k2;
            long long up[3] = {0, 0, 0};
            for (const Child& kd : kids) {
                if (!kd.valid || kd.dead) continue;
                const State& z = kd.s;
                int cls = -1;
                if (z.boost) {
                    const int m = (int)z.mode;
                    const bool flies = m == 1 || m == 3 || m == 4 || m == 7, reads = m == 1 || m == 3 || m == 7;
                    const float f22 = z.mini ? 0.85f : 1.0f;
                    const double v = (double)z.vy;
                    const bool inBoth = (double)(-6.4f / f22) < v && v < (double)-(-6.4f / f22);
                    const bool noRing = z.touchRing[0] < 0 && z.touchRing[1] < 0 && z.touchRing[2] < 0 && z.touchRing[3] < 0;
                    cls = (flies && inBoth && !z.dashing && noRing) ? 1 : reads ? 0 : 2;
                    ++up[cls];
                }
                k0.push_back(kd.key);
                k1.push_back(cls >= 0 ? kd.key ^ kKeyBoost : kd.key);
                k2.push_back(cls == 1 ? kd.key ^ kKeyBoost : kd.key);
            }
            if (!k0.empty()) {
                auto distinct = [](std::vector<uint64_t>& v) { std::sort(v.begin(), v.end()); return (long long)(std::unique(v.begin(), v.end()) - v.begin()); };
                ++g_boost1Obs.groups; g_boost1Obs.kids += (long long)k0.size();
                for (int k = 0; k < 3; ++k) g_boost1Obs.up[k] += up[k];
                g_boost1Obs.keys += distinct(k0); g_boost1Obs.keysNoBit += distinct(k1); g_boost1Obs.keysCanon += distinct(k2);
            }
        }
        // --bandpairs (modifiers.hpp): children with the same dedupe key and bandProgress but a different raw bandAnim byte
        if (g_bandPair.on) {
            std::vector<std::tuple<uint64_t, long long, uint8_t>> bp;
            for (const Child& kd : kids) if (kd.valid && !kd.dead) bp.emplace_back(kd.key, std::llround(bandProgress(kd.s.bandAnim) * 1000000.0), kd.s.bandAnim);
            std::sort(bp.begin(), bp.end());
            long long d1 = 0, d2 = 0;
            for (size_t i = 0; i < bp.size(); ++i) {
                const bool newRaw = i == 0 || bp[i] != bp[i - 1];
                const bool newProg = i == 0 || std::get<0>(bp[i]) != std::get<0>(bp[i - 1]) || std::get<1>(bp[i]) != std::get<1>(bp[i - 1]);
                d2 += newRaw; d1 += newProg;
            }
            ++g_bandPair.groups; g_bandPair.kids += (long long)bp.size(); g_bandPair.d1 += d1; g_bandPair.d2 += d2;
            if (d2 > d1) { ++g_bandPair.groupsWithExtra; g_bandPair.maxExtra = std::max(g_bandPair.maxExtra, d2 - d1); }
        }
        // --worldtree (P5-2b): the two-level key against the flat one, over this group's children before the dedupe (observation only)
        if (g_worldTree.on) {
            std::vector<uint64_t> a, f;
            std::vector<std::pair<uint64_t, uint64_t>> pr;
            for (const Child& kd : kids) {
                if (!kd.valid || kd.dead) continue;
                const uint64_t w = worldKeyOf(kd.s, false, true);
                const uint64_t rest = kd.key ^ w;
                a.push_back(rest); pr.emplace_back(w, rest); f.push_back(rest ^ w);
            }
            std::sort(a.begin(), a.end()); a.erase(std::unique(a.begin(), a.end()), a.end());
            std::sort(pr.begin(), pr.end()); pr.erase(std::unique(pr.begin(), pr.end()), pr.end());
            std::sort(f.begin(), f.end()); f.erase(std::unique(f.begin(), f.end()), f.end());
            auto& W2 = g_worldTree;
            ++W2.cGroups; W2.cK += (long long)a.size(); W2.cKW += (long long)pr.size(); W2.cKF += (long long)f.size();
            if (pr.size() != f.size()) ++W2.cGroupsMismatch;
        }
        // ---- phase 2p: dedupe in parallel shards (big layers) ---------------
        // Same result as the serial loop below, built in three steps:
        //   1. each shard walks the ordinals ascending and, for ITS keys only,
        //      replays emit()'s hi/lo hysteresis (a per-key sequential scan --
        //      exactness needs order only WITHIN a key, and a key lives in one
        //      shard). Slot-allocation events (first sight of a key, first
        //      hi/lo split) are marked per ordinal -- race-free, one owner.
        //   2. a serial scan over the ordinals materialises the slots in the
        //      same order the serial emit() would have pushed them.
        //   3. the goal is the roomiest ACCEPTED state that reached it
        //      (rules.reached), the smallest ordinal breaking ties -- the state
        //      the serial loop would have ended up holding.
        const size_t qfBefore = nxt.size();   // --qfoldwatch: this group's survivors start here
        const bool parDedupe = pool && !dbgHere && g_threads > 1
                               && kids.size() >= 4096;
        if (parDedupe) {
            const size_t nsh = (size_t)g_threads;
            if (shards.size() < nsh) shards.resize(nsh);
            if (evKind.size() < kids.size()) {
                evKind.resize(kids.size(), 0);
                evRec.resize(kids.size(), 0);
            }
            pool->parallelTasks(nsh, [&](size_t si) {
                ShardMap& sm = shards[si];
                sm.clear();
                sm.ensure(kids.size() / nsh + 64);
                for (uint32_t i = 0; i < (uint32_t)kids.size(); ++i) {
                    if (kidFlag[i] != 2) continue;
                    const uint64_t k = kidKeys[i];
                    if ((size_t)((k * 0x9E3779B97F4A7C15ull) >> 32) % nsh != si)
                        continue;
                    const float vy = kids[i].s.vy;
                    uint32_t& ri = sm.at(k);
                    bool accepted = false;
                    if (ri == kNone) {
                        ri = (uint32_t)sm.recs.size();
                        sm.recs.push_back(KeyRec{k, i, kNone, i, i, vy, vy, 0});
                        evKind[i] = (uint8_t)(1 + si);
                        evRec[i] = ri;
                        accepted = true;
                    } else {
                        KeyRec& r = sm.recs[ri];
                        // the comparisons mirror emit() exactly, including the
                        // float->double promotion of `vy + 1e-9`
                        if ((double)vy > (double)r.hiVy + 1e-9) {
                            if (gdcol::g_keyObs && r.hiIdx != r.loIdx) noteDroppedPrev(kids[r.hiIdx].s, kids[i].s);   // KEYOBS: the displaced rep
                            if (r.bOrd == kNone) {
                                r.bOrd = i;
                                r.bWasHi = 1;
                                evKind[i] = (uint8_t)(1 + si);
                                evRec[i] = ri;
                            }
                            r.hiIdx = i;
                            r.hiVy = vy;
                            accepted = true;
                        } else if ((double)vy < (double)r.loVy - 1e-9) {
                            if (gdcol::g_keyObs && r.hiIdx != r.loIdx) noteDroppedPrev(kids[r.loIdx].s, kids[i].s);   // KEYOBS: the displaced rep
                            if (r.bOrd == kNone) {
                                r.bOrd = i;
                                r.bWasHi = 0;
                                evKind[i] = (uint8_t)(1 + si);
                                evRec[i] = ri;
                            }
                            r.loIdx = i;
                            r.loVy = vy;
                            accepted = true;
                        } else if (gdcol::g_keyObs) {   // KEYOBS: dropped; compared with the reps it lost to (State::prevWX / prevWY is not in the key)
                            noteDroppedPrev(kids[i].s, kids[r.hiIdx].s);
                            if (r.loIdx != r.hiIdx) noteDroppedPrev(kids[i].s, kids[r.loIdx].s);
                        }
                    }
                    // Same test as the serial path (rules.reached): keep the
                    // roomiest goal state this shard saw rather than its first.
                    // `goalTight` starts at 0xffff, so with the score off the
                    // first one wins and nothing after it can tie-break past it.
                    // Asked when the layer is already solved too: the serial path
                    // replaces a goal with a roomier one, and the merge below
                    // decides against the one held. This test used to read x
                    // alone, and only while unsolved -- no frame, no coins -- so a
                    // big layer could take a goal the serial path would refuse.
                    if (accepted && rules.reached(kids[i].s)
                        && (sm.goalOrd == kNone
                            || rules.prefer(kids[i].s.tight, sm.goalTight))) {
                        sm.goalOrd = i;
                        sm.goalTight = kids[i].s.tight;
                    }
                }
            });
            // counters (compact array, trivial serial pass)
            for (size_t i = 0; i < kids.size(); ++i) {
                if (!kidFlag[i]) continue;
                ++nBorn;
                if (kidFlag[i] == 1) { ++nDied; if (g_partialPhys) notePhys(cur[gidx[i >> 1]], kids[i].deadKind); }
            }
            // materialise nxt in the serial loop's slot order
            for (uint32_t i = 0; i < (uint32_t)kids.size(); ++i) {
                if (!evKind[i]) continue;
                ShardMap& sm = shards[(size_t)evKind[i] - 1];
                const KeyRec& r = sm.recs[evRec[i]];
                evKind[i] = 0;   // leave the array clean for the next group
                uint32_t src;
                if (r.bOrd == kNone) src = r.hiIdx;   // never split: hi == lo
                else if (i == r.aOrd) src = r.bWasHi ? r.loIdx : r.hiIdx;
                else src = r.bWasHi ? r.hiIdx : r.loIdx;
                const State& from = cur[gidx[src >> 1]];
                const uint8_t action = (uint8_t)(src & 1);
                arena.push_back(Node{from.parent | ((uint32_t)action << 31)});
                if (checkWanted) arenaMode.push_back(kids[src].s.mode);
                if (lossyOn) { const uint8_t lf = arenaLossy[from.parent]; arenaLossy.push_back(lf); }
                nxt.push_back(kids[src].s);
                nxt.back().parent = (uint32_t)(arena.size() - 1);
                nxt.back().action = action;
                if (action == 1 && heldRide(from)) rideNodes.push_back(nxt.back().parent);
            }
            {
                // Across shards, the same rule as within one: roomiest first,
                // the smaller ordinal breaking ties. With the score off every
                // tight is 0, no candidate is ever "roomier", and this reduces
                // to min(goalOrd). Against a goal already held, the serial
                // path's rule: only a roomier one replaces it.
                uint32_t g = kNone;
                uint16_t gt = 0xffff;
                for (size_t si = 0; si < nsh; ++si) {
                    const uint32_t o = shards[si].goalOrd;
                    if (o == kNone) continue;
                    const uint16_t ot = shards[si].goalTight;
                    if (g == kNone || ot < gt || (ot == gt && o < g)) {
                        g = o;
                        gt = ot;
                    }
                }
                if (g != kNone && (!solved || rules.prefer(gt, goalState.tight))) {
                    const State& from = cur[gidx[g >> 1]];
                    const uint8_t action = (uint8_t)(g & 1);
                    arena.push_back(
                        Node{from.parent | ((uint32_t)action << 31)});
                    if (checkWanted) arenaMode.push_back(kids[g].s.mode);
                    if (lossyOn) { const uint8_t lf = arenaLossy[from.parent]; arenaLossy.push_back(lf); }
                    goalState = kids[g].s;
                    goalState.parent = (uint32_t)(arena.size() - 1);
                    goalState.action = action;
                    solved = true;
                }
            }
            if (qfHere) qfoldAccount(qfBefore);
            continue;   // next speed group -- skips the serial phase 2
        }
        // ---- phase 2: dedupe and record (serial, in order) ------------------
        for (size_t i = 0; i < kids.size(); ++i) {
            const Child& kid = kids[i];
            if (!kid.valid) continue;
            const State& s = cur[gidx[i >> 1]];
            const int input = (int)(i & 1);
            const State& c = kid.s;
            const bool dead = kid.dead != 0;
            ++nBorn;
            if (dead) { ++nDied; if (g_partialPhys) notePhys(s, kid.deadKind); }
            {
                if (dbgHere) {
                    // name the killer. "dead=1" alone never says whether the
                    // model is over-killing or under-killing (see g_deadWhy).
                    char why[128] = "";
                    if (dead) {
                        if (kid.obj)
                            std::snprintf(why, sizeof why,
                                          " %s id=%d type=%d (%.0f,%.0f) %gx%g",
                                          kid.why, kid.obj->id,
                                          (int)kid.obj->type, kid.obj->cx,
                                          kid.obj->cy, kid.obj->hw * 2,
                                          kid.obj->hh * 2);
                        else
                            std::snprintf(why, sizeof why, " %s", kid.why);
                    }
                    // The second body, when there is one. Without it a dual
                    // layer prints two parents with the SAME (y, vy) and no way
                    // to tell what separated them -- which is how lv20's
                    // t=17,115 layer reads: four lines, two distinct children,
                    // nothing on the line that explains the split.
                    // `held` is on every line for the same reason: GD runs
                    // buttons AFTER the update, so a wave's direction on this
                    // tick follows the PREVIOUS tick's button, and two parents
                    // differing only in `held` step apart under the same input.
                    char two[160] = "";
                    if (s.dual)
                        std::snprintf(two, sizeof two,
                                      " | p2 (%.3f,%.3f)->(%.3f,%.3f) flip2=%d "
                                      "g2=%d free=%d",
                                      (double)s.y2, (double)s.vy2,
                                      (double)c.y2, (double)c.vy2,
                                      (int)c.flip2, (int)c.grounded2,
                                      (int)c.freeHalf);
                    std::printf("dbg t=%lld in=%d: (%.3f,%.3f)->(%.3f,%.3f) "
                                "x=%.2f mode=%d flip=%d g=%d held=%d dead=%d%s%s\n",
                                t, input, (double)s.y, (double)s.vy, (double)c.y,
                                (double)c.vy, (double)c.xAbs, (int)c.mode,
                                (int)c.flip, (int)c.grounded, (int)s.held,
                                (int)dead, why, two);
                }
                if (!dead) {
                    emitNode = 0;
                    emit(c, s, (uint8_t)input, kid.key);
                    if (emitNode && input == 1 && heldRide(s)) rideNodes.push_back(emitNode);
                }
            }
        }
        if (qfHere) qfoldAccount(qfBefore);
        }   // speed group
        // ---- SAFE BANDS ------------------------------------------------------
        // How wide the frontier is at this tick, written out for the driver.
        //
        // The driver's anchor policy is "just before the death, then back off
        // geometrically", and that is what breaks on a deep wall: it can only
        // crawl backwards from the wall, so when a stretch of the route becomes
        // invalid it never gets far enough back to redo it. Measured on lv16
        // (60 iterations, oriented rule): the loop reached x=19,174 once and
        // then fell to 13,310 and stayed there; on lv19 it bounced 29,024 <->
        // 27,319 three times. Both are the same shape.
        // A tick where the frontier is WIDE is a tick where many different
        // states survive -- i.e. a place where the level does not care much how
        // you arrived. Those are the places to cut a level into segments, and
        // the DP already knows them; it just never told anyone.
        // Cheap: two words per layer, flushed once at the end.
        if (!g_bandPath.empty()) {
            float ylo = 1e9f, yhi = -1e9f;
            double xr = 0;
            // semantic classes: mode/mini/dual/speed. Small in practice (a
            // handful per layer), so a flat vector beats a map.
            struct Cls { uint32_t k; int n; float lo, hi; };
            std::vector<Cls> cs;
            for (const State& s : nxt) {
                ylo = std::min(ylo, s.y);
                yhi = std::max(yhi, s.y);
                xr = std::max(xr, (double)s.xAbs);
                const uint32_t k = ((uint32_t)s.mode << 24)
                                   ^ ((uint32_t)s.mini << 20)
                                   ^ ((uint32_t)s.dual << 16)
                                   ^ (uint32_t)std::lround(s.dx * 1000.0);
                size_t j = 0;
                for (; j < cs.size(); ++j) if (cs[j].k == k) break;
                if (j == cs.size()) cs.push_back({k, 0, 1e9f, -1e9f});
                cs[j].n++;
                cs[j].lo = std::min(cs[j].lo, s.y);
                cs[j].hi = std::max(cs[j].hi, s.y);
            }
            if (!nxt.empty()) {
                BandRow b;
                b.t = t; b.alive = (int)nxt.size();
                b.ylo = ylo; b.yhi = yhi; b.x = (float)xr;
                // merged this layer = children born - children that died -
                // survivors (what the dedupe folded away)
                b.merged = (int)std::max<long long>(
                    0, nBorn - nDied - (long long)nxt.size());
                char buf[64];
                for (const Cls& c : cs) {
                    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%d:%.0f:%.0f",
                                  c.k >> 24, (c.k >> 20) & 0xf,
                                  (c.k >> 16) & 0xf, c.k & 0xffff, c.n,
                                  (double)c.lo, (double)c.hi);
                    if (!b.cls.empty()) b.cls += ';';
                    b.cls += buf;
                }
                g_bands.push_back(std::move(b));
            }
        }
        // Say it the FIRST time a touch trigger fires anywhere in the frontier.
        // Without this the only symptom of "the search never found the box" is
        // that the wall does not move, which is exactly the symptom of a dozen
        // other things (see the three sessions spent on x=28,075).
        if (!g_touch.empty()) {
            TouchMask m{};
            for (const State& s : nxt) m |= s.trig;
            if (m & ~g_trigReported) {
                for (size_t b = 0; b < g_touch.size(); ++b)
                    if ((m & ~g_trigReported) & touchBit(b))
                        std::printf("triggers: box (%.0f,%.0f) entered at "
                                    "t=%lld\n", g_touch[b].cx, g_touch[b].cy, t);
                g_trigReported |= m;
            }
        }
        ppMark(2);
        // ---- stride cap, PER CLASS ------------------------------------------
        // A "class" is what makes two states play a different game: mode, size,
        // gravity, dual, speed. The cap used to be one stride over the whole
        // layer, which quietly starves any class that expands less per tick --
        // and one does: an AIRBORNE CUBE gets one child (its input is moot)
        // while a ship gets two. So a cube branch's share halves every layer
        // once the cap binds, no matter how healthy it is.
        // Measured on lv18 from t=14,200: the branch that takes the RegularSize
        // portal at x=20,386 (the one route that survives -- it reaches
        // x=27,356 while the mini one dies at 24,716) is 416 states at
        // t=15,000 and ZERO at t=15,500 with cap 2,000, while the same run at
        // cap 40,000 still has 2,945 of them. It was not being out-competed on
        // merit; it was being out-multiplied.
        // Max-min fair instead: a small class keeps everything it has, and the
        // big ones split what is left. Kept states stay in their original order
        // so the layer is otherwise unchanged.
        // ---- reference watch, part 3: is it still there, and if not, why ----
        // Checked twice, on purpose: once here, with the layer complete and the
        // cap not yet applied, and once after the cap. Only the pair can tell
        // "the cap evicted it" from "it never got into the layer", and those
        // two have nothing to do with each other.
        bool refPreCap = false;
        State refCarried{};     // the tracked state, captured BEFORE the cap
        if (g_refWatch && g_refLostAt < 0) {
            const char* gate = nullptr;
            char detail[256];
            detail[0] = '\0';
            const auto it = g_refRows.find(t);
            // Which child IS the reference's next state: the one that matches
            // the trace. Both are examined, so a dead one can still be named.
            int want = -1;
            if (it != g_refRows.end() && it->second.act >= 0
                && it->second.act < 2 && g_refKidFate[it->second.act] >= 1) {
                // The reference recorded which input stepped this tick. Read it
                // rather than infer it: on a swing's press tick the two children
                // are identical in everything the trace shows.
                want = it->second.act;
            } else {
                for (int a = 0; a < 2 && it != g_refRows.end(); ++a)
                    if (g_refKidFate[a] >= 1 && refMatches(g_refKidState[a],
                                                           it->second))
                        want = a;
            }
            if (g_refParent < 0) {
                gate = "parent-gone";
            } else if (it == g_refRows.end()) {
                gate = "trace-ended";
            } else if (want < 0) {
                // Neither child is the reference's next state. The search's
                // carrier and the replay have parted company -- a difference in
                // stepping or in context, not a pruning decision, and it must
                // not be reported as one.
                gate = "cannot-reproduce";
                snprintf(detail, sizeof(detail),
                         " ref y=%.4f vy=%.4f | kid0 y=%.4f vy=%.4f f=%d"
                         " | kid1 y=%.4f vy=%.4f f=%d",
                         it->second.y, it->second.vy,
                         (double)g_refKidState[0].y, (double)g_refKidState[0].vy,
                         g_refKidFate[0],
                         (double)g_refKidState[1].y, (double)g_refKidState[1].vy,
                         g_refKidFate[1]);
            } else if (g_refKidFate[want] == 2
                       && !refMatches(g_refKidState[want], it->second)) {
                // The reference says this input stepped the tick, and the search
                // stepping the same state with the same input produced something
                // else. That is a transition difference, not a pruning decision.
                gate = "cannot-reproduce";
                snprintf(detail, sizeof(detail),
                         " act=%d ref y=%.4f vy=%.4f flip=%d fr=%d | kid y=%.4f "
                         "vy=%.4f flip=%d fr=%d", want, it->second.y,
                         it->second.vy, it->second.flip, it->second.frame,
                         (double)g_refKidState[want].y,
                         (double)g_refKidState[want].vy,
                         (int)g_refKidState[want].flip,
                         (int)g_refKidState[want].frame);
            } else if (g_refKidFate[want] == 1) {
                gate = "kill";
                snprintf(detail, sizeof(detail), " rule=%s action=%d",
                         *g_refKidWhy[want] ? g_refKidWhy[want] : "(unnamed)",
                         want);
            } else {
                const int idx = refFindExact(nxt, g_refKidState[want]);
                if (idx >= 0) {
                    g_refParent = idx;      // carried, untouched
                    refPreCap = true;
                } else {
                    // Merged away. The watch stops here rather than adopting the
                    // survivor and carrying on: past a merge the carrier is no
                    // longer on the reference's trajectory, so the next layer's
                    // children cannot be matched against the trace and every
                    // line after it would be about a different path.
                    //
                    // What IS recorded is how far the kept state is, because
                    // that is the whole question about a merge: 0.19 px/tick of
                    // vy is a state the cell may fairly stand in for, and a
                    // larger gap is a passage being represented by something
                    // that cannot make it.
                    gate = "dedupe";
                    const int alt = refNearestInCell(nxt, g_refKidKey[want],
                                                     g_refKidState[want],
                                                     [t, &rules](const State& st) {
                                                         return modelKeyOf(st, (long long)t) ^ rules.keyBits(st);
                                                     });
                    int n = 0;
                    for (const State& s : nxt)
                        if ((modelKeyOf(s, (long long)t) ^ rules.keyBits(s)) == g_refKidKey[want]) ++n;
                    // A merge whose survivor differs from the reference only in WHEN
                    // boxes fired (fireB / trigT) -- a box whose chain moves nothing,
                    // entered a tick apart -- is followed on the survivor instead of
                    // ending the watch. Printed, so the reader knows the carrier is no
                    // longer the reference to the bit (SubZero 4003's toggle block:
                    // the held and the released entry merge, and the watch stopped
                    // 400 ticks before the question it was asked).
                    bool adopted = false;
                    // Every survivor in the cell, not just the nearest: the nearest in (y, vy)
                    // can be one from another group (a different trigger mask), which is never
                    // the reference's merge partner.
                    for (size_t si = 0; si < nxt.size() && !adopted; ++si) {
                        if ((modelKeyOf(nxt[si], (long long)t) ^ rules.keyBits(nxt[si])) != g_refKidKey[want]) continue;
                        State a = nxt[si], b = g_refKidState[want];
                        // Which of the overlooked fields actually differ, by name.
                        std::string diff;
                        if (a.action != b.action) diff += " action";
                        if (std::memcmp(a.fireB, b.fireB, sizeof(a.fireB)) != 0) diff += " fireB";
                        if (a.trigT != b.trigT) diff += " trigT";
                        if (a.tight != b.tight) diff += " tight";
                        if (a.rot != b.rot || a.rotNeg != b.rotNeg || a.rotStep != b.rotStep
                            || a.rotBoost != b.rotBoost)
                            diff += " rot";
                        if (a.groundUid != b.groundUid) diff += " groundUid";
                        a.parent = b.parent = 0;
                        a.action = b.action = 0;
                        std::memcpy(a.fireB, b.fireB, sizeof(a.fireB));
                        a.trigT = b.trigT;
                        a.tight = b.tight;
                        // ...and the sprite angle (rot / rotNeg / rotStep), which the key
                        // leaves out too; named when it is the difference.
                        const bool rotDiff = a.rot != b.rot || a.rotNeg != b.rotNeg
                                             || a.rotStep != b.rotStep || a.rotBoost != b.rotBoost;
                        a.rot = b.rot; a.rotNeg = b.rotNeg; a.rotStep = b.rotStep;
                        a.rotBoost = b.rotBoost;
                        // ...and --stickseam's tie (groundUid), which the key leaves out.
                        const bool tieDiff = a.groundUid != b.groundUid;
                        a.groundUid = b.groundUid;
                        (void)rotDiff; (void)tieDiff;
                        // --refadoptmem: ...and the press buffer and the ring memory, which the key
                        // leaves out as well (triggers.hpp, g_refAdoptMem).
                        if (g_refAdoptMem) {
                            if (a.jumpBuf != b.jumpBuf) diff += " jumpBuf";
                            if (a.usedOrb != b.usedOrb || a.usedOrb2 != b.usedOrb2
                                || a.usedOrbOld != b.usedOrbOld || a.usedOrbOld2 != b.usedOrbOld2
                                || std::memcmp(a.usedOrbHist, b.usedOrbHist, sizeof(a.usedOrbHist)) != 0
                                || std::memcmp(a.usedOrbHist2, b.usedOrbHist2, sizeof(a.usedOrbHist2)) != 0)
                                diff += " usedOrb";
                            a.jumpBuf = b.jumpBuf;
                            a.jumpBuf2 = b.jumpBuf2;   // --dualjumpbuf
                            a.usedOrb = b.usedOrb; a.usedOrb2 = b.usedOrb2;
                            a.usedOrbOld = b.usedOrbOld; a.usedOrbOld2 = b.usedOrbOld2;
                            std::memcpy(a.usedOrbHist, b.usedOrbHist, sizeof(a.usedOrbHist));
                            std::memcpy(a.usedOrbHist2, b.usedOrbHist2, sizeof(a.usedOrbHist2));
                        }
                        if (g_refAdoptSl) {   // --refadoptsl (refwatch.hpp): a shadow block that differs only in the benign words does not keep the survivor from standing in
                            for (int p = 0; p < 2; ++p) {
                                const gdcol::ShadowLite* x = gdcol::slOf(p ? nxt[si].slRef2 : nxt[si].slRef);
                                const gdcol::ShadowLite* y = gdcol::slOf(p ? g_refKidState[want].slRef2 : g_refKidState[want].slRef);
                                std::string nm;
                                if (x && y && slDiffBenign(*x, *y, nm)) {
                                    if (p) a.slRef2 = b.slRef2; else a.slRef = b.slRef;
                                    if (!nm.empty()) diff += std::string(" shadow") + (p ? "2:" : "1:") + nm;
                                }
                            }
                        }
                        if (std::memcmp(&a, &b, sizeof(State)) == 0 && !g_refAdopt) {
                            std::printf("refwatch: merge t=%lld the survivor differs only in:%s "
                                        "(--refadopt would carry on with it)\n", t, diff.c_str());
                        } else if (std::memcmp(&a, &b, sizeof(State)) == 0) {
                            std::printf("refwatch: ADOPT t=%lld a SUBSTITUTE from here, differing in:%s\n",
                                        t, diff.c_str());
                            if (g_refAdoptT < 0) g_refAdoptT = t;
                            for (const char* fld : {"action", "fireB", "trigT", "tight", "rot", "groundUid",
                                                    "jumpBuf", "usedOrb"})
                                if (diff.find(std::string(" ") + fld) != std::string::npos
                                    && g_refAdoptFields.find(fld) == std::string::npos)
                                    g_refAdoptFields += std::string(" ") + fld;
                            g_refParent = (int)si;
                            refPreCap = true;
                            gate = nullptr;
                            adopted = true;
                        } else {
                            const unsigned char* pa = reinterpret_cast<const unsigned char*>(&a);
                            const unsigned char* pb = reinterpret_cast<const unsigned char*>(&b);
                            std::printf("refwatch: cell-mate %zu trig 0x%llx/0x%llx differs at:", si,
                                        (unsigned long long)nxt[si].trig.word(0),
                                        (unsigned long long)g_refKidState[want].trig.word(0));
                            int nd = 0;
                            for (size_t o = 0; o < sizeof(State) && nd < 16; ++o)
                                if (pa[o] != pb[o]) { std::printf(" %zu", o); ++nd; }
                            std::printf("\n");
                            // the shadow's own block differs where the handle's bytes say nothing: name what differs (refwatch.hpp slDiffNames)
                            const gdcol::ShadowLite* sa1 = gdcol::slOf(nxt[si].slRef);
                            const gdcol::ShadowLite* sb1 = gdcol::slOf(g_refKidState[want].slRef);
                            if (sa1 && sb1) std::printf("refwatch: cell-mate %zu shadow(p1) differs:%s\n", si, slDiffNames(*sa1, *sb1).c_str());
                            else std::printf("refwatch: cell-mate %zu shadow(p1) block %s\n", si, (!sa1 && !sb1) ? "absent on both" : (!sa1 ? "absent on the survivor" : "absent on the reference"));
                            const gdcol::ShadowLite* sa2 = gdcol::slOf(nxt[si].slRef2);
                            const gdcol::ShadowLite* sb2 = gdcol::slOf(g_refKidState[want].slRef2);
                            if (sa2 && sb2) std::printf("refwatch: cell-mate %zu shadow(p2) differs:%s\n", si, slDiffNames(*sa2, *sb2).c_str());
                        }
                    }
                    if (adopted) {
                    } else if (alt < 0) {
                        snprintf(detail, sizeof(detail), " cell-gone");
                    } else {
                        g_refDriftY = std::fabs((double)nxt[(size_t)alt].y
                                                - (double)g_refKidState[want].y);
                        g_refDriftVy = std::fabs((double)nxt[(size_t)alt].vy
                                                 - (double)g_refKidState[want].vy);
                        snprintf(detail, sizeof(detail),
                                 " cell=%d action=%d ref y=%.4f vy=%.4f "
                                 "kept y=%.4f vy=%.4f", n, want,
                                 (double)g_refKidState[want].y,
                                 (double)g_refKidState[want].vy,
                                 (double)nxt[(size_t)alt].y,
                                 (double)nxt[(size_t)alt].vy);
                    }
                }
            }
            if (refPreCap) refCarried = nxt[(size_t)g_refParent];
            if (gate) {
                std::printf("refwatch: LOST t=%lld gate=%s%s "
                            "dy=%.3f dvy=%.3f alive=%zu\n",
                            t, gate, detail, g_refDriftY,
                            g_refDriftVy, nxt.size());
                g_refLostAt = t;
            }
        }
        maxAlive = std::max(maxAlive, nxt.size());
        const bool censusHere = g_searchCensus > 0 && ((t - t0) % g_searchCensus) == 0;
        if (g_searchCensus > 0 || g_censusLayersOn) census.layer(nxt, (long long)t, curDxF, nBorn, nDied, censusHere);
        // --capmap: this layer's cap, from the leader x of the layer before it.
        const size_t capNow = capAtX(bestX);
        g_outcome.workStates += (long long)std::min(nxt.size(), capNow);
        const size_t qfPre = nxt.size();   // --qfoldwatch: the frontier before the alive cap
        // --heldcellcap: the layer's cells, keyed without the button (held, jumpBuf), each named by
        // its first state in layer order -- the population the water-fill samples -- and the cell
        // of every state. Built only for a layer over the cap; a layer whose CELLS fit is kept whole.
        std::vector<uint32_t> cellPool;
        std::vector<uint32_t> cellOf;
        bool overCap = nxt.size() > capNow;
        // --capholdpair: the cells are instead the two children of one parent that the press has
        // not told apart yet (same parent, same y / vy / grounded / mode / size / flip / dual): the
        // released and the pressed child, which the new model's key keeps as two states (slKeyBits).
        const bool pairCells = g_capHoldPair;
        if (overCap && ((g_heldCellCap && g_heldAll) || pairCells)) {
            // The keys on the pool's threads (the layer is thousands of states on every capped
            // layer, and keyOf is the costly part), then (key, index) sorted: a cell's first
            // state is its lowest index.
            const size_t n = nxt.size();
            std::vector<std::pair<uint64_t, uint32_t>> ck(n);
            auto keyAt = [&](size_t i) {
                State s = nxt[i];
                if (pairCells) {
                    uint32_t yb, vyb;
                    static_assert(sizeof s.y == 4 && sizeof s.vy == 4, "float body");
                    std::memcpy(&yb, &s.y, 4);
                    std::memcpy(&vyb, &s.vy, 4);
                    const uint64_t k =
                        ((uint64_t)(arena[s.parent].parent() + 1) * 0x9E3779B97F4A7C15ull)
                        ^ ((((uint64_t)yb << 32) | vyb) * 0xBF58476D1CE4E5B9ull)
                        ^ (((uint64_t)s.grounded << 1 | (uint64_t)s.mode << 8 | (uint64_t)s.mini << 16
                            | (uint64_t)s.flip << 24 | (uint64_t)s.dual << 32) * 0x94D049BB133111EBull);
                    ck[i] = {k, (uint32_t)i};
                    return;
                }
                s.held = 0;
                s.jumpBuf = 0;
                ck[i] = {modelKeyOf(s, (long long)t) ^ rules.keyBits(s), (uint32_t)i};
            };
            if (pool && n >= 512) pool->parallelFor(n, keyAt);
            else for (size_t i = 0; i < n; ++i) keyAt(i);
            std::sort(ck.begin(), ck.end());
            cellOf.resize(n);
            for (size_t j = 0; j < n; ++j) {
                const uint32_t rep = (j > 0 && ck[j].first == ck[j - 1].first)
                                         ? cellOf[ck[j - 1].second] : ck[j].second;
                cellOf[ck[j].second] = rep;
                if (rep == ck[j].second) cellPool.push_back(rep);
            }
            std::sort(cellPool.begin(), cellPool.end());
            overCap = cellPool.size() > capNow;
            if (overCap) ++cellCuts;
            else ++cellSpared;
        }
        // A held child kept towards a ring (rideNodes) is not a state the cap chooses: in the air it is its released sibling with another button history, and counted in
        // the layer it took a share of its class from states that are different places. It rides instead: left out of what the water-fill samples, kept exactly when
        // the state it is the same body as is kept -- same x, y, vy, mode, size, gravity, dual and frame, the button aside (the cells below, as --heldcellcap's twins
        // are). The released child of its own parent is that state only on the first tick of the hold: after it, that child has merged with the released lineage's.
        // A held child with no such state in the layer is counted as any state.
        if (overCap && cellOf.empty() && !rideNodes.empty()) {
            const size_t n = nxt.size();
            auto bodyKey = [](const State& q) -> uint64_t {
                uint32_t xb, yb, vb;
                static_assert(sizeof q.xAbs == 4 && sizeof q.y == 4 && sizeof q.vy == 4, "float body");
                std::memcpy(&xb, &q.xAbs, 4); std::memcpy(&yb, &q.y, 4); std::memcpy(&vb, &q.vy, 4);
                return ((((uint64_t)yb << 32) | vb) * 0xBF58476D1CE4E5B9ull) ^ ((uint64_t)xb * 0x9E3779B97F4A7C15ull)
                       ^ (((uint64_t)q.mode | (uint64_t)q.mini << 8 | (uint64_t)q.flip << 16 | (uint64_t)q.dual << 24 | (uint64_t)q.frame << 32 | (uint64_t)q.grounded << 40) * 0x94D049BB133111EBull);
            };
            auto sameBody = [](const State& a, const State& b) {
                return a.xAbs == b.xAbs && a.y == b.y && a.vy == b.vy && a.mode == b.mode && a.mini == b.mini && a.flip == b.flip && a.dual == b.dual && a.frame == b.frame && a.grounded == b.grounded;
            };
            std::vector<uint8_t> isRide(n, 0);
            std::unordered_map<uint64_t, uint32_t> body;
            for (size_t i = 0; i < n; ++i) {
                isRide[i] = std::binary_search(rideNodes.begin(), rideNodes.end(), nxt[i].parent) ? 1 : 0;
                if (!isRide[i]) body.emplace(bodyKey(nxt[i]), (uint32_t)i);
            }
            cellOf.resize(n);
            for (size_t i = 0; i < n; ++i) {
                cellOf[i] = (uint32_t)i;
                if (isRide[i]) {
                    const auto it = body.find(bodyKey(nxt[i]));
                    if (it != body.end() && sameBody(nxt[it->second], nxt[i])) cellOf[i] = it->second;
                }
                if (cellOf[i] == (uint32_t)i) cellPool.push_back((uint32_t)i);
            }
            overCap = cellPool.size() > capNow;
            if (!overCap) { cellOf.clear(); cellPool.clear(); }
        }
        rideNodes.clear();
        const bool cellCap = !cellOf.empty();
        if (overCap) {
            ++capHits;
            if (e.outcome.capHitX0 < 0.0) e.outcome.capHitX0 = (double)bestX;   // the first layer the cap thinned: where (SearchOutcome::capHitX0)
            // the first / last layer the cap thinned (--lossystat prints them; kept always, they are three integers)
            if (g_lossy.firstCapT < 0) g_lossy.firstCapT = (long long)t;
            g_lossy.lastCapT = (long long)t;
            ++g_lossy.capLayers;
            if (!cellCap) capDropped += (long long)(nxt.size() - capNow);
            if (!g_bandPath.empty() && !g_bands.empty() && g_bands.back().t == t)
                g_bands.back().capdrop = (int)((cellCap ? cellPool.size() : nxt.size()) - capNow);
            // The flight band as a class of its own, and --coinwin's cell (rules.bandClass,
            // rules.winCell).
            const bool bandClass = rules.bandClass();
            auto coinWinCell = [&rules](const State& s) -> uint64_t { return rules.winCell(s); };
            // --capholdclass: the holding bits of a state's shadow block (ShadowLite::hold, GD's
            // m_holdingButtons[1..3]); 0 for a state the shadow did not step
            auto holdOf = [](const State& s) -> uint64_t {
                const gdcol::ShadowLite* p = gdcol::slOf(s.slRef);
                return (p && (p->flags & gdcol::kSlValid)) ? (uint64_t)p->hold : 0;
            };
            auto classOf = [bandClass, &coinWinCell, &rules, &holdOf](const State& s) {
                return coinWinCell(s) ^ ((uint64_t)s.mode << 32) ^ ((uint64_t)s.mini << 24)
                       ^ ((uint64_t)s.flip << 16) ^ ((uint64_t)s.dual << 8)
                       // the route's own class (CurrentRules::classBits: the coins taken)
                       ^ rules.classBits(s)
                       // ...and the FLIGHT BAND. Every ship state is otherwise
                       // one class, so the water-fill samples the whole layer at
                       // one stride -- and a route that has to thread many
                       // saturated layers as a minority survives that with
                       // probability (kept/born)^n. lv1's coin corridor is
                       // exactly such a route, and it is what this is for: at
                       // cap 16,000 the frontier at t=19,000 is y=[113,367]
                       // while the reference route is at 457, so the search
                       // reports the coin unreachable even though the model
                       // replays the human's own route through it to 0.00 px.
                       // At cap 200,000 the frontier is y=[435,495] and it
                       // solves -- so it was never physics, only thinning.
                       // The band is the right axis rather than a y grid (tried
                       // first, floor(y/60)): it is what a BRANCH actually is
                       // here. lv1's corridor is entered through a portal at
                       // cy=405 that a low route never touches, and firing it is
                       // what rewrites bandCeil; keyOf already treats two bands
                       // as two worlds. The y grid recovered lv1 too but cost
                       // capHits on levels that have no branch at all (lv19
                       // +30%, lv14 +13%, lv18 +10%), while the band left
                       // lv14/17/18/19 identical to the digit.
                       // (Only where the band clamps -- search_key.hpp bandClamps.)
                       ^ ((bandClass && bandClamps(s)) ? ((uint64_t)(uint32_t)(int32_t)
                              std::lround((double)s.bandCeil) << 48) : 0)
                       // ...and which ACTIVATORS have fired (g_actMask, empty
                       // without --activators). SubZero 4003: the one lineage that
                       // pressed the toggle block, and so has the ring that leads
                       // to the second coin, was evicted by this cap at t=8,288
                       // (refwatch on a human route the model replays to 0 px).
                       ^ (bandClass ? (s.trig & g_actMask).word(0) * 0x94D049BB133111EBull
                                    : 0)
                       ^ (uint64_t)(uint32_t)std::lround(s.dx * 1000.0)
                       // --capdashclass (removed): a state holding a dash is a class of its own
                       // (always on: the switch --capdashclass is removed)
                       // --capdashclass (removed): the frontier cap's per-class stride (search_dp.hpp classOf) counts a state holding a dash (State::dashing) as a class of its own, so a thin dash line keeps all its states while the big cube class is thinned. lv21 x~2480: the only route over the pit holds a dash orb for 100+ ticks; under a binding cap the stride kept it at random, and the new model, whose layer is wider, lost it at t=1909.
                       ^ ((uint64_t)s.dashing << 40)
                       // --capholdclass: ...and so is each set of the new model's holding bits, so a
                       // stride over (released, pressed) pairs does not keep one side only
                       ^ (g_capHoldClass ? (holdOf(s) << 44) : 0)
                       // ...and whether a counting Tap's window is open with its Count not fired yet (State::taps / tapsX, bit 0x80: set while
                       // the presses are being counted, cleared when the Count on the Tap's item has fired or the window has shut -- cli.hpp).
                       // The two sides are different futures: past the level's own threshold the Count switches its group, before it the
                       // route is still free. As one class, the side that has fired outnumbers the other until the stride keeps none of it:
                       // on a custom level (a Tap at x=49095 counts into item 2, a Count at 6 turns a spike back on at x=49433) the states
                       // that had not fired were 857 of 2000 45 ticks into the window, 31 after 69 and none after 117, and every attempt of
                       // the call from t=33983 died on the spike up to cap 8000; with the class it solves on the first attempt at cap 125
                       // (the count itself as the class also solves, at cap 500 on the second attempt: more classes, a smaller share each).
                       // The threshold is the level's (the Count's target, which is what clears the bit); nothing here knows a number. 0 for a
                       // state with no open window, so a level without a counting Tap keeps every class as it was.
                       ^ ((uint64_t)(((s.taps >> 7) & 1u) | (((s.tapsX[0] >> 7) & 1u) << 1) | (((s.tapsX[1] >> 7) & 1u) << 2) | (((s.tapsX[2] >> 7) & 1u) << 3))
                          * 0xC2B2AE3D27D4EB4Full);
            };
            std::vector<uint64_t> cls;
            std::vector<std::vector<uint32_t>> byCls;
            const size_t popN = cellCap ? cellPool.size() : nxt.size();
            for (size_t p = 0; p < popN; ++p) {
                const uint32_t i = cellCap ? cellPool[p] : (uint32_t)p;
                const uint64_t k = classOf(nxt[i]);
                size_t j = 0;
                for (; j < cls.size(); ++j) if (cls[j] == k) break;
                if (j == cls.size()) { cls.push_back(k); byCls.push_back({}); }
                byCls[j].push_back(i);
            }
            // water-fill: smallest class first, each takes at most an equal
            // share of what is still free
            std::vector<size_t> ord(byCls.size());
            for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
                return byCls[a].size() < byCls[b].size();
            });
            std::vector<uint32_t> keepIdx;
            keepIdx.reserve(capNow);
            size_t left = capNow;
            std::vector<uint8_t> clsThinned;   // --lossystat: byCls index -> the water-fill kept fewer states than the class had
            if (lossyOn) clsThinned.assign(byCls.size(), 0);
            for (size_t n = 0; n < ord.size(); ++n) {
                const std::vector<uint32_t>& v = byCls[ord[n]];
                const size_t fair = left / (ord.size() - n);
                const size_t take = std::min(v.size(), fair);
                if (lossyOn && take < v.size()) clsThinned[ord[n]] = 1;
                if (take == v.size()) {
                    keepIdx.insert(keepIdx.end(), v.begin(), v.end());
                } else if (g_capEnvelope && take >= 3) {
                    // --capenvelope: the class's two extremes, then the stride over the rest.
                    auto lower = [&](uint32_t a, uint32_t b) {
                        if (nxt[a].y != nxt[b].y) return nxt[a].y < nxt[b].y;
                        if (nxt[a].vy != nxt[b].vy) return nxt[a].vy < nxt[b].vy;
                        return a < b;
                    };
                    size_t lo = 0, hi = 0;
                    for (size_t k = 1; k < v.size(); ++k) {
                        if (lower(v[k], v[lo])) lo = k;
                        if (lower(v[hi], v[k])) hi = k;
                    }
                    keepIdx.push_back(v[lo]);
                    keepIdx.push_back(v[hi]);
                    std::vector<uint32_t> rest;
                    rest.reserve(v.size() - 2);
                    for (size_t k = 0; k < v.size(); ++k)
                        if (k != lo && k != hi) rest.push_back(v[k]);
                    const double stride = (double)rest.size() / (double)(take - 2);
                    for (double i = 0; (size_t)i < rest.size(); i += stride)
                        keepIdx.push_back(rest[(size_t)i]);
                } else if (take > 0) {
                    const double stride = (double)v.size() / (double)take;
                    for (double i = 0; (size_t)i < v.size() && take > 0; i += stride)
                        keepIdx.push_back(v[(size_t)i]);
                }
                left -= std::min(left, take);
            }
            // --heldcellcap: every state of a sampled cell comes back with it, in layer order.
            auto withTwins = [&](std::vector<uint32_t>& idx) {
                if (!cellCap) return;
                std::vector<uint8_t> pick(nxt.size(), 0);
                for (uint32_t i : idx) pick[i] = 1;
                idx.clear();
                if (g_heldCellTwins <= 0 || pairCells) {
                    for (uint32_t i = 0; i < (uint32_t)nxt.size(); ++i)
                        if (pick[cellOf[i]]) idx.push_back(i);
                    return;
                }
                // --heldcelltwins N: the cell's first state, then the states whose held differs
                // from it, then the rest, until the cell has N.
                // TRIED AND REMOVED (2026-09-26): a bound on the layer's total as well
                // (--heldcelltotal P, at most (100 + P)% of the cap, the twins taken at an even
                // stride). With N = 3 it did not bring lv16's seconds under 2x (P=50 2.63x, P=25
                // 2.48x, against 2.6x without it) and it broke the ladder on lv14 (kids 4.2x at
                // P=50, 39x at P=25) and lv11 (13x, not solved, against 0.55x and SOLVED at N = 3
                // alone): official 22 from t=0, the loop's base argv.
                std::vector<uint8_t> kept(nxt.size(), 0);
                std::vector<int> nOf(nxt.size(), 0);
                for (int pass = 0; pass < 3; ++pass)
                    for (uint32_t i = 0; i < (uint32_t)nxt.size(); ++i) {
                        const uint32_t r = cellOf[i];
                        if (!pick[r] || kept[i] || nOf[r] >= g_heldCellTwins) continue;
                        if (pass == 0 && i != r) continue;
                        if (pass == 1 && nxt[i].held == nxt[r].held) continue;
                        kept[i] = 1;
                        ++nOf[r];
                        idx.push_back(i);
                    }
                std::sort(idx.begin(), idx.end());
            };
            const size_t cellsKept = keepIdx.size();
            withTwins(keepIdx);
            if (cellCap) {
                capDropped += (long long)(nxt.size() - keepIdx.size());
                cellTwinsKept += (long long)(keepIdx.size() - cellsKept);
            }
            std::sort(keepIdx.begin(), keepIdx.end());
            if (lossyOn) {
                // --lossystat: every kept state of a thinned class is marked; its descendants inherit the mark when their nodes are made. cellOf names a twin's cell (--heldcellcap), whose first state is in byCls.
                std::vector<int32_t> clsOfIdx(nxt.size(), -1);
                for (size_t c = 0; c < byCls.size(); ++c) for (uint32_t i : byCls[c]) clsOfIdx[i] = (int32_t)c;
                for (uint32_t i : keepIdx) {
                    const uint32_t rep = cellCap ? cellOf[i] : i;
                    const int32_t c = clsOfIdx[rep];
                    if (c >= 0 && clsThinned[(size_t)c]) arenaLossy[nxt[i].parent] = 1;
                }
            }
            if (g_capAudit) capAuditLayer(t, nxt, keepIdx);   // --capaudit (cap_audit.hpp): what the cap did to the layer's worlds; print only
            // --searchcensus: the same water-fill, with each class's stride taken over the
            // class sorted by (y, vy) instead of in layer order. Counted, never kept.
            if (g_searchCensus > 0 || g_censusLayersOn) census.cap(nxt.size(), capNow, nDied);
            if (censusHere) {
                std::vector<uint32_t> altIdx;
                size_t leftA = capNow;
                for (size_t n = 0; n < ord.size(); ++n) {
                    std::vector<uint32_t> v = byCls[ord[n]];
                    std::sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) {
                        if (nxt[a].y != nxt[b].y) return nxt[a].y < nxt[b].y;
                        if (nxt[a].vy != nxt[b].vy) return nxt[a].vy < nxt[b].vy;
                        return a < b;
                    });
                    const size_t fair = leftA / (ord.size() - n);
                    const size_t take = std::min(v.size(), fair);
                    if (take == v.size()) {
                        altIdx.insert(altIdx.end(), v.begin(), v.end());
                    } else if (take > 0) {
                        const double stride = (double)v.size() / (double)take;
                        for (double i = 0; (size_t)i < v.size() && take > 0; i += stride)
                            altIdx.push_back(v[(size_t)i]);
                    }
                    leftA -= std::min(leftA, take);
                }
                withTwins(altIdx);
                census.coverage(nxt, keepIdx, altIdx, curDxF);
            }
            std::vector<State> kept;
            kept.reserve(keepIdx.size());
            for (uint32_t i : keepIdx) kept.push_back(nxt[i]);
            nxt.swap(kept);
            // ...and the second half of the pair: it was in the layer before
            // the cap and is not in it now.
            if (g_refWatch && g_refLostAt < 0 && refPreCap) {
                const int idx = refFindExact(nxt, refCarried);
                if (idx < 0) {
                    std::printf("refwatch: LOST t=%lld gate=cap alive=%zu "
                                "cap=%zu\n",
                                t, nxt.size(), capNow);
                    g_refLostAt = t;
                } else {
                    g_refParent = idx;    // the cap reordered the layer
                }
            }
        }
        if (lossyOn && !nxt.empty()) {   // --lossystat: the layer's lossy count, the last kTail layers
            size_t nl = 0;
            for (const State& s : nxt) nl += arenaLossy[s.parent];
            if (g_lossy.tail.size() >= LossyStat::kTail) g_lossy.tail.erase(g_lossy.tail.begin());
            g_lossy.tail.push_back({(long long)t, nxt.size(), nl, qfPre});
        }
        // --heldcellcap keeps more states than the cap counts: charge what the layer really keeps.
        if (cellCap) g_outcome.workStates += (long long)nxt.size() - (long long)std::min(qfPre, capNow);
        if (g_searchCensus > 0 || g_censusLayersOn) census.kept(nxt);
        // How many complete routes the final pick is choosing between, and how
        // far back they are actually different (--clearprobe only; reads the
        // arena, changes nothing).
        if (g_clearProbe && solved) measureGoalDiversity(nxt, arena, goalX);
        if (g_rjOn) {
            if ((long long)t == g_rjAfter) {
                const auto rr = g_rjRows.find((long long)t);
                const int oi = rr == g_rjRows.end() ? -1 : refFind(nxt, rr->second);
                g_rjOldNode = oi >= 0 ? nxt[(size_t)oi].parent : 0xffffffffu;
                std::printf("rejoin: the old path is %s the frontier at its death tick %lld\n",
                            oi >= 0 ? "in" : "NOT in", (long long)t);
            }
            const long long tt = (long long)t;
            const int ji = rejoinLayer(nxt, tt, [&](const State& s) {
                if (g_rjOldNode == 0xffffffffu) return false;
                uint32_t n = s.parent;
                for (long long k = tt; k > g_rjAfter; --k) n = arena[n].parent();
                return n == g_rjOldNode;
            }, [&](const State& s, const RefRow& r) {
                return !g_rjFull || (r.haveKey && (modelKeyOf(s, tt) ^ rules.keyBits(s)) == r.key);
            });
            // ...and only onto an old plan whose inputs reach where this call's plan has to end (refwatch.hpp
            // rejoinTailReaches): a tail that stops short of the goal and of t0 + horizon is where the old search
            // stopped, and joining it would turn that into a SOLVED. The search goes on instead.
            long long rjEndT = -1; double rjEndX = 0.0;
            const bool rjGoesOn = ji >= 0 && rejoinTailReaches(tt, (double)goalX, horizon > 0 ? (long long)t0 + (long long)horizon : 0, &rjEndT, &rjEndX);
            if (g_rjUse && ji >= 0 && !solved && !rjGoesOn) {
                if (g_rjCall.shortN++ == 0) { g_rjCall.shortT = tt; g_rjCall.shortEndT = rjEndT; }
                if (g_outcome.rejoinShortN++ == 0) {
                    g_outcome.rejoinShortT = tt; g_outcome.rejoinShortEndT = rjEndT;
                    std::printf("rejoin: NOT joined at t=%lld: the old plan's inputs end at t=%lld x=%.1f, short of the goal (x %.1f) and of this call's end\n",
                                tt, rjEndT, rjEndX, (double)goalX);
                }
            }
            // ...and, with --coins, not where the route still owes a press or a coin
            // (rules.rejoinOk).
            const State* rjS = ji >= 0 ? &nxt[(size_t)ji] : nullptr;
            const bool rjCoins = rules.rejoinOk(rjS);
            if (g_rjUse && ji >= 0 && !solved && rjGoesOn && rjCoins) {
                solved = true;
                goalState = nxt[(size_t)ji];
                planCut = 0;
                g_rjJoinT = tt;
                g_outcome.rejoinT = tt;
                bestT = tt;
                std::printf("rejoin: JOINED the old plan at t=%lld (%lld ticks past its death), "
                            "stopping the search here\n", tt, tt - g_rjAfter);
                break;
            }
        }
        if (qfHere)
            std::printf("qfold: t=%lld frontier=%zu kept=%zu lostq=%lld keys=%lld\n",
                        (long long)t, qfPre, nxt.size(), qfLost, qfKeys);
        // --wavetidy / --wavetoggle: the next layer is stepped tidiest route first, so the first-accepted-wins dedupe
        // of its children keeps the tidier route into a cell. Stable: equal scores keep the order they had.
        // Only in a layer that has a wave in it: elsewhere the order is the one it was, and so is the cap's stride.
        if ((g_waveTidy > 0.0 || g_waveToggle > 0)
            && std::any_of(nxt.begin(), nxt.end(), [](const State& s) { return s.mode == 4; }))
            std::stable_sort(nxt.begin(), nxt.end(),
                             [](const State& a, const State& b) { return a.tight < b.tight; });
        // --canonorder / --canonorder-y (modifiers.hpp): the frontier in a canonical order, a function of the set of states and not of how it was built
        if (g_canonOrder && nxt.size() > 1) {
            if (g_canonShuffle) { std::mt19937 rng((uint32_t)g_canonShuffle * 2654435761u + (uint32_t)t); std::shuffle(nxt.begin(), nxt.end(), rng); }
            struct CK { uint64_t k; float y, vy; double x; uint32_t i; };
            std::vector<CK> ck(nxt.size());
            for (size_t i = 0; i < nxt.size(); ++i) ck[i] = {modelKeyOf(nxt[i], (long long)t) ^ rules.keyBits(nxt[i]), nxt[i].y, nxt[i].vy, (double)nxt[i].xAbs, (uint32_t)i};
            const bool yFirst = g_canonOrder == 2;
            std::stable_sort(ck.begin(), ck.end(), [yFirst, &nxt](const CK& a, const CK& b) {
                if (!yFirst && a.k != b.k) return a.k < b.k;
                if (a.y != b.y) return a.y < b.y;
                if (a.vy != b.vy) return a.vy < b.vy;
                if (a.x != b.x) return a.x < b.x;
                if (yFirst && a.k != b.k) return a.k < b.k;
                // equal in the comparator so far: the hidden fields the tie census found (bandAnim, tight, portSeen), then the lineage (parent: the arena index, which follows the previous layer's canonical order)
                const State& sa = nxt[a.i]; const State& sb = nxt[b.i];
                if (sa.bandAnim != sb.bandAnim) return sa.bandAnim < sb.bandAnim;
                if (sa.tight != sb.tight) return sa.tight < sb.tight;
                const int c = std::memcmp(&sa.portSeen, &sb.portSeen, sizeof sa.portSeen);
                if (c != 0) return c < 0;
                return sa.parent < sb.parent;
            });
            std::vector<State> sorted;
            sorted.reserve(nxt.size());
            for (const CK& c : ck) sorted.push_back(nxt[c.i]);
            ++g_canonStat.layers;
            g_canonStat.states += (long long)sorted.size();
            for (size_t i = 1; i < ck.size(); ++i)
                if (ck[i].k == ck[i - 1].k && ck[i].y == ck[i - 1].y && ck[i].vy == ck[i - 1].vy && ck[i].x == ck[i - 1].x) {
                    ++g_canonStat.tiePairs;
                    if (std::memcmp(&sorted[i], &sorted[i - 1], sizeof(State)) != 0) ++g_canonStat.tieDiff;
                    { const unsigned char* pa = (const unsigned char*)&sorted[i]; const unsigned char* pb = (const unsigned char*)&sorted[i - 1];
                      for (size_t w = 0; w < sizeof(State) / 4 && w < 128; ++w) if (std::memcmp(pa + w * 4, pb + w * 4, 4) != 0) ++g_canonStat.wordDiff[w]; }
                }
            nxt.swap(sorted);
        }        // --worldkeytable (worldkey.hpp): the world key of every state of the new frontier, interned, with its parent worlds and the camera-summary criteria
        if (g_worldTable.on) { g_worldTable.withTog = true; g_worldTable.layer((long long)t, nxt, arena); }
        // --worldkey (modifiers.hpp): how many distinct states a layer would hold if the dx history entered the dedupe key
        if (g_worldKey && !nxt.empty()) {
            std::vector<uint64_t> a, b;
            a.reserve(nxt.size()); b.reserve(nxt.size());
            for (const State& s : nxt) {
                const uint64_t mk = modelKeyOf(s, (long long)t) ^ rules.keyBits(s);
                a.push_back(mk);
                uint64_t h = mk ^ (s.dxHist + 0x9e3779b97f4a7c15ULL + (mk << 6) + (mk >> 2));
                b.push_back(h * 0xff51afd7ed558ccdULL);
            }
            std::sort(a.begin(), a.end()); a.erase(std::unique(a.begin(), a.end()), a.end());
            std::sort(b.begin(), b.end()); b.erase(std::unique(b.begin(), b.end()), b.end());
            auto& W = g_worldKeyStat;
            const long long gain = (long long)b.size() - (long long)a.size();
            ++W.layers; W.states += (long long)nxt.size(); W.k += (long long)a.size(); W.kd += (long long)b.size();
            W.maxExtra = std::max(W.maxExtra, gain); if (gain > 0) ++W.layersSplit;
            auto& B = W.b[(long long)t / 2000];
            ++B.layers; B.states += (long long)nxt.size(); B.k += (long long)a.size(); B.kd += (long long)b.size(); B.maxExtra = std::max(B.maxExtra, gain);
        }        // --layerdigest (modifiers.hpp): the layer's order-independent fingerprint, for tools/layer_diff.py
        if (!g_layerDigestPath.empty()) {
            if (!g_layerDigestFile && layerDigestCall((long long)t0)) {
                static int ldSeq1 = 0;
                g_layerDigestFile = std::fopen(layerDigestName(g_layerDigestPath, (long long)t0, ldSeq1).c_str(), "w");
                if (g_layerDigestFile) std::fprintf(g_layerDigestFile, "# layerdigest v1: rung,t,n,sum,xor,ymin,ymax\n");
            }
            if (g_layerDigestFile) {
                if ((long long)t <= g_ldLastT) ++g_ldRung;
                g_ldLastT = (long long)t;
                auto mix = [](uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; };
                uint64_t sum = 0, xr = 0;
                float ymin = 1e30f, ymax = -1e30f;
                for (const State& s : nxt) {
                    const uint64_t h = layerStateFingerprint(s, modelKeyOf(s, (long long)t) ^ rules.keyBits(s));
                    sum += h; xr ^= h;
                    if (s.y < ymin) ymin = s.y;
                    if (s.y > ymax) ymax = s.y;
                }
                std::fprintf(g_layerDigestFile, "%lld,%lld,%zu,%016llx,%016llx,%.9g,%.9g\n", g_ldRung, (long long)t, nxt.size(), (unsigned long long)sum, (unsigned long long)xr, (double)ymin, (double)ymax);
                std::fflush(g_layerDigestFile);
            }
        }
        // --layerdigest2 (modifiers.hpp, state_serial.hpp): the canonical serializer's digest of the layer, 128 bits over every field of every state, the records in byte order
        if (!g_layerDigest2Path.empty()) {
            if (!g_layerDigest2File && layerDigestCall((long long)t0)) {
                static int ldSeq2 = 0;
                g_layerDigest2Name = layerDigestName(g_layerDigest2Path, (long long)t0, ldSeq2);
                g_layerDigest2File = std::fopen(g_layerDigest2Name.c_str(), "w");
                if (g_layerDigest2File) { std::fprintf(g_layerDigest2File, "# layerdigest v2: rung,t,n,hashA,hashB\n"); stser::writeFieldsJson(g_layerDigest2Name + ".fields.json"); }
            }
            if (g_layerDigest2File) {
                if ((long long)t <= g_ld2LastT) ++g_ld2Rung;
                g_ld2LastT = (long long)t;
                std::vector<std::vector<uint8_t>> recs;
                stser::layerRecords(nxt, recs);
                const stser::Dig128 dg = stser::layerDigest(recs);
                std::fprintf(g_layerDigest2File, "%lld,%lld,%zu,%016llx,%016llx\n", g_ld2Rung, (long long)t, nxt.size(), (unsigned long long)dg.a, (unsigned long long)dg.b);
                std::fflush(g_layerDigest2File);
                if (std::find(g_layerBytes2T.begin(), g_layerBytes2T.end(), (long long)t) != g_layerBytes2T.end()) {
                    const std::string bp = g_layerDigest2Name + ".t" + std::to_string((long long)t) + ".r" + std::to_string(g_ld2Rung) + ".bin";
                    if (std::FILE* bf = std::fopen(bp.c_str(), "wb")) {
                        const uint32_t magic = 0x324c4447u, n = (uint32_t)recs.size();   // "GDL2"
                        const int64_t tt = (int64_t)t;
                        std::fwrite(&magic, 4, 1, bf); std::fwrite(&tt, 8, 1, bf); std::fwrite(&n, 4, 1, bf);
                        for (const auto& rc : recs) { const uint32_t len = (uint32_t)rc.size(); std::fwrite(&len, 4, 1, bf); std::fwrite(rc.data(), 1, rc.size(), bf); }
                        std::fclose(bf);
                    }
                }
            }
        }
        prevAlive = cur.size();
        cur.swap(nxt);
        // --shadowstep: the shadow's blocks of the new frontier are kept, every other one goes (shadow_lite.hpp SlStore::gc). No store without the flag.
        if (gdcol::g_slStore) {
            const auto gc0 = g_phaseProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
            gdcol::g_slStore->gc(cur);
            if (g_phaseProf) g_ppSlGcNs += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - gc0).count();   // (inside the `cap` bucket)
        }
        nBornPrev = nBorn;
        nDiedPrev = nDied;
        lyPhysPrev = lyPhys; lyPrunePrev = lyPrune;
        // The layer no longer HAS one x (see the speed groups above), so the
        // number everything downstream reports is the frontier's own leader.
        double x = 0.0;
        for (const State& s : cur) x = std::max(x, (double)s.xAbs);
        if (!cur.empty()) { bestT = t; bestX = x; }
        if (g_partialByX)
            for (const State& s : cur)
                if ((double)s.xAbs > bestAnyX) { bestAnyX = (double)s.xAbs; bestAnyS = s; bestAnyT = t; haveAny = true; }
        if (cur.empty()) break;
        // horizon reached: take the surviving state with the most clearance
        // (highest |vy| is a bad proxy; just take the first survivor -- they
        // are all alive, and the driver re-anchors from GD anyway)
        // The horizon bounds the PLAN, not the SEARCH. It used to stop the
        // search dead at t0+horizon and emit `cur.front()`, an arbitrary
        // survivor -- and a survivor at the horizon is not the same thing as a
        // survivor that goes anywhere. On lv11 that prefix was a dead end: GD
        // replayed it, died 37 ticks after the plan ran out, and every anchor
        // the driver backed off to (six of them, down to t=1537) was doomed,
        // because they all sat on that one doomed branch. The full search
        // reached x=6,067 on a different branch the whole time.
        // So: keep searching another `horizon` ticks past the cut, and only
        // then take a survivor. The emitted plan is truncated back to the cut
        // below, so what the driver replays is a prefix that is KNOWN to still
        // be alive `horizon` ticks later. Cost is at most 2x the search.
        if (horizon > 0 && !solved && t - t0 >= horizon * 2) {
            solved = true;
            g_outcome.horizonCut = true;   // survived, did not reach the end (SearchOutcome)
            goalState = rules.pick(cur);
            planCut = t0 + horizon;
            break;
        }
        ppMark(3);
        // ---- mark-compact GC ----
        // The arena is append-only and keeps every dead lineage's whole
        // ancestor chain; on lv16 cap=40000 it reached 147.9M nodes (564 MiB)
        // and the process died growing it. The frontier's chains coalesce a
        // few hundred ticks back, so the REACHABLE set is a tiny fraction
        // (measure it with --memstat). Compact whenever the arena doubles
        // past the threshold: amortized O(1) per node. Indices are internal,
        // so the emitted plan is bit-identical with GC on or off.
        if (g_gcNodes > 0 && !solved && arena.size() >= gcNext) {
            std::vector<bool> mark(arena.size(), false);
            mark[0] = true;
            auto markFrom = [&](const std::vector<State>& v) {
                for (const State& s : v)
                    for (uint32_t i = s.parent; i != 0 && !mark[i];
                         i = arena[i].parent())
                        mark[i] = true;
            };
            markFrom(cur);
            markFrom(nxt);   // the PARTIAL path reads nxt after the loop
            if (haveAny) {   // --partialbyx: the best-x state is read after the loop too
                std::vector<State> one(1, bestAnyS);
                markFrom(one);
            }
            if (havePhys) {   // --partialphys: so is the physical-death parent
                std::vector<State> one(1, physS);
                markFrom(one);
            }
            size_t keep = 0;
            for (size_t i = 0; i < arena.size(); ++i) keep += mark[i];
            std::vector<uint32_t> remap(arena.size(), 0);
            std::vector<Node> na;
            na.reserve(keep);
            // The mode side vector is indexed by the same numbers, so it is compacted in the
            // same pass or it would describe the nodes the arena used to hold. Nothing that has
            // ALREADY been published can be reached from here -- a published prefix is ticks
            // and levels, never indices (dp/progress.hpp) -- so this only has to keep the live
            // array consistent for the next LCA walk.
            std::vector<uint8_t> nm, nl;
            if (checkWanted) nm.reserve(keep);
            if (lossyOn) nl.reserve(keep);   // --lossystat's side vector, compacted in the same pass
            for (size_t i = 0; i < arena.size(); ++i) {
                if (!mark[i]) continue;
                remap[i] = (uint32_t)na.size();
                // a parent always precedes its children in the arena, so
                // remap[parent()] is already final here
                na.push_back(Node{remap[arena[i].parent()]
                                  | ((uint32_t)arena[i].action() << 31)});
                if (checkWanted) nm.push_back(arenaMode[i]);
                if (lossyOn) nl.push_back(arenaLossy[i]);
            }
            // the last two layers' ranges of nodes (WorldRange) follow their nodes: the kept ones of a range stay next to each other, in order
            const bool wrWas = wrOpen;
            wrClose();
            for (std::vector<WorldRange>* wv : {&wrPrev, &wrCur}) {
                size_t w = 0;
                for (const WorldRange& r : *wv) {
                    uint32_t first = 0, n = 0;
                    for (uint32_t i = r.a0; i < r.a1 && (size_t)i < arena.size(); ++i) if (mark[i]) { if (!n) first = remap[i]; ++n; }
                    if (n) (*wv)[w++] = WorldRange{first, first + n, r.key};
                }
                wv->resize(w);
            }
            const size_t before = arena.size();
            arena.swap(na);
            if (wrWas) { wrA0 = arena.size(); wrOpen = true; }   // (the turn the compaction fell in goes on in a new range)
            na = std::vector<Node>();   // release the old allocation NOW
            if (checkWanted) {
                arenaMode.swap(nm);
                nm = std::vector<uint8_t>();
            }
            if (lossyOn) arenaLossy.swap(nl);
            for (State& s : cur) s.parent = remap[s.parent];
            for (State& s : nxt) s.parent = remap[s.parent];
            if (haveAny) bestAnyS.parent = remap[bestAnyS.parent];
            if (havePhys) physS.parent = remap[physS.parent];
            if (g_rjOldNode != 0xffffffffu)
                g_rjOldNode = mark[g_rjOldNode] ? remap[g_rjOldNode] : 0xffffffffu;
            gcNext = std::max(g_gcNodes, arena.size() * 2);
            if (lpOn) g_lpc.snaps.clear();   // --ladderprefix: their arena sizes refer to the old numbering
            std::printf("gc t=%lld arena %zu -> %zu nodes (%.1f%% live)\n", t,
                        before, arena.size(),
                        100.0 * (double)arena.size() / (double)before);
            std::fflush(stdout);
        }
        ppMark(4);
        // ---- --ladderprefix: save the loop's state after this layer ----
        if (lpOn && !solved) {
            if (!lpBound && capHits > 0) lpBound = true;
            if (lpBound && g_ladderPrefixEvery > 0 && ((t - t0) % g_ladderPrefixEvery) == 0) {
                LadderPrefixSnap S;
                S.T = t;
                S.cur = cur;
                bool refsOk = true;
                S.objRefs.resize(cur.size() * 12);
                for (size_t i = 0; i < cur.size(); ++i) {
                    const State& s = cur[i];
                    uint64_t* r = &S.objRefs[i * 12];
                    r[0] = model.encodeObj(s.snapObj, refsOk);
                    r[1] = model.encodeObj(s.usedOrb, refsOk);
                    r[2] = model.encodeObj(s.snapObj2, refsOk);
                    r[3] = model.encodeObj(s.usedOrb2, refsOk);
                    for (int k = 0; k < 4; ++k) r[4 + k] = model.encodeObj(s.usedPad[k], refsOk);
                    for (int k = 0; k < 4; ++k) r[8 + k] = model.encodeObj(s.usedPad2[k], refsOk);
                }
                if (gdcol::g_slStore) {   // --shadowstep: the blocks by value
                    for (size_t i = 0; i < cur.size(); ++i) S.sl.add(gdcol::slOf(cur[i].slRef));
                    S.sl.data.shrink_to_fit();
                    S.sl.at.shrink_to_fit();
                    for (size_t i = 0; i < cur.size(); ++i) S.sl2.add(gdcol::slOf(cur[i].slRef2));
                    S.sl2.data.shrink_to_fit();
                    S.sl2.at.shrink_to_fit();
                }
                S.arenaSize = arena.size();
                S.arenaModeSize = arenaMode.size();
                wrClose();
                if (wrT == (long long)t) { g_trigLive.saveLayer(S.tl, (long long)t); if (S.tl.have) S.wr = wrCur; }
                S.gcNext = work.gcNext;
                S.nBornPrev = nBornPrev;
                S.nDiedPrev = nDiedPrev;
                S.prevAlive = prevAlive;
                S.bestT = bestT;
                S.bestX = bestX;
                S.goalState = goalState;
                S.bestAnyS = bestAnyS; S.bestAnyX = bestAnyX; S.bestAnyT = bestAnyT; S.haveAny = haveAny;
                if (g_partialPhys) { S.physS = physS; S.physT = physT; S.havePhys = havePhys; S.lyPhysPrev = lyPhysPrev; S.lyPrunePrev = lyPrunePrev; }
                S.solved = solved;
                S.maxAlive = maxAlive;
                S.capDropped = capDropped;
                S.capHits = capHits;
                S.cellCuts = cellCuts;
                S.cellSpared = cellSpared;
                S.cellTwinsKept = cellTwinsKept;
                S.census = census;
                S.twin = twin;
                S.workStates = g_outcome.workStates - lpWork0;
                S.autoFire.resize(g_autoTrig.size());
                for (size_t i = 0; i < g_autoTrig.size(); ++i) {
                    const AutoTrig& A = g_autoTrig[i];
                    S.autoFire[i] = {A.fireT, A.fireX, A.fireSpreadT, A.lockLastX};
                }
                S.rotTrig = g_rotTrig;
                S.carry.assign(4, {});
                S.carryOn.assign(4, 0);
                for (int f = 0; f < 4; ++f)
                    if (model.frameBound(f)) {
                        model.saveWorld(f, S.carry[(size_t)f]);
                        S.carryOn[(size_t)f] = 1;
                    }
                S.gfireGroups = g_gfireGroups;
                S.gfireSpread = g_gfireSpread;
                S.gfireSum = g_gfireSum;
                S.gfireMax = g_gfireMax;
                S.groupXSplitMax = g_groupXSplitMax;
                S.rjJoinT = g_rjJoinT;
                S.rjOldNode = g_rjOldNode;
                S.layerXSize = g_lpc.layerX.size();
                if (refsOk) {   // a pointer outside the level's lists: this layer is not saved
                    if (g_lpc.snaps.size() >= g_ladderPrefixMaxSnaps) g_lpc.snaps.erase(g_lpc.snaps.begin());
                    g_lpc.snaps.push_back(std::move(S));
                    // The byte cap: the oldest saves go first, the newest one stays. Only a save that has grown past
                    // anything the levels measured reaches this; it says so once per ladder.
                    const size_t capBytes = g_lpc.maxMiB * (size_t)1048576;
                    size_t held = 0;
                    for (const LadderPrefixSnap& q : g_lpc.snaps) held += q.bytes();
                    if (held > capBytes && g_lpc.snaps.size() > 1) {
                        size_t dropped = 0;
                        while (held > capBytes && g_lpc.snaps.size() > 1) {
                            held -= g_lpc.snaps.front().bytes();
                            g_lpc.snaps.erase(g_lpc.snaps.begin());
                            ++dropped;
                        }
                        if (!g_lpc.capSaid) {
                            g_lpc.capSaid = true;
                            std::printf("ladderprefix: the saves passed %zu MiB at t=%lld - the %zu oldest dropped "
                                        "(%zu kept, %zu MiB)\n",
                                        g_lpc.maxMiB, t, dropped, g_lpc.snaps.size(), held >> 20);
                        }
                    }
                }
            }
        }
        // ---- memory budget ----
        // Estimate the search's own structures and stop GROWING before the OS
        // stops us (the lv16 run died as exit 255 with zero diagnostics). The
        // emitted plan keeps the horizon guarantee: a prefix that is known to
        // still be alive (t-t0)/2 ticks past its end, i.e. the same contract
        // as the horizon cut, just with a smaller effective horizon.
        if (g_memLimitMiB > 0 && !solved && (t & 127) == 0 && t - t0 > 2) {
            // arenaMode is empty with no subscriber, so this term is 0 and the estimate --
            // and therefore the tick this limit fires on -- is exactly what it was.
            const size_t est = arena.capacity() * sizeof(Node) + arenaMode.capacity()
                               + (cur.capacity() + nxt.capacity()) * sizeof(State)
                               + seen.bucket_count() * 8 + seen.size() * 48;
            if (est > g_memLimitMiB * (size_t)1048576) {
                solved = true;
                goalState = rules.pick(cur);
                planCut = t0 + (t - t0) / 2;
                std::printf("MEMORY_LIMIT: est=%zuMiB arena=%zu alive=%zu "
                            "t=%lld x=%.0f -> plan cut at t=%lld "
                            "(half the searched depth)\n",
                            est >> 20, arena.size(), cur.size(), t, x,
                            planCut);
                break;
            }
        }
        // ---- checkpoints (dp/progress.hpp) ----
        //
        // At a checkpoint layer, publish the lineage of the frontier's FIRST state, cut at this
        // layer, for the caller to fly. The first state because the frontier's order is a
        // function of the search alone, so the same call publishes the same lineage every time
        // -- which state is flown matters less than that the choice never depends on the clock.
        // Every state in `cur` is alive in the model through tick t, so the game killing this
        // lineage at or before t is a disagreement with the model by construction.
        if (checkWanted && !solved && !cur.empty() && isCheckpointLayer(t - t0)) {
            std::vector<uint8_t> plvl, pmode;
            plvl.reserve((size_t)(t - t0));
            pmode.reserve((size_t)(t - t0));
            for (uint32_t i = cur.front().parent; i != 0; i = arena[i].parent()) {
                plvl.push_back(arena[i].action());
                pmode.push_back(arenaMode[i]);
            }
            std::reverse(plvl.begin(), plvl.end());
            std::reverse(pmode.begin(), pmode.end());
            SearchCheckpoints::Point p;
            p.t0 = t0;
            p.tick = t0 + (long long)plvl.size();
            for (const PlanEdge& e :
                 planEdges(plvl, pmode, t0, planPrevHeld(init), init.mode))
                p.edges.emplace_back(e.press, e.level);
            g_check.publish(std::move(p));
        }
        // ---- the caller has stopped caring ----
        // Read every layer: once the game has refuted a checkpoint, every layer after it is paid
        // for and thrown away. One relaxed load -- and read with no subscriber too, because the
        // mod also cancels when its session ends (the player left the level). Read only under
        // `dpcheck`, which is off by default, that cancel did nothing: the search ran on to its
        // end on its detached thread, and the next level's Solve sat queued behind it with the
        // old search's numbers on its HUD (lv16 left mid-search went on for 22 s at 2,000 states
        // while lv1 waited). The CLI never sets the flag, so its output is unchanged.
        if (g_check.cancel.load(std::memory_order_relaxed)) {
            std::printf("CANCELLED: at t=%lld (search abandoned by the caller)\n", t);
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = t;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;    // distinct from FAILED (1) and from --replay's unreadable plan (2)
        }
        // ...and the ladder that has taken the plain search beside it (the gamble, cliMain):
        // this attempt's answer can no longer be used. Never set outside a ladder.
        if (g_ladderStop.load(std::memory_order_relaxed)) {
            std::printf("STOPPED: at t=%lld (the ladder took the plain search beside it)\n", t);
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = t;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;
        }
        ppMark(5);
        // Publish where the loop has got to. Every layer, not every 500th: this is what a UI
        // watching from another thread samples, and the printed line below is far too coarse
        // to look alive on screen (see dp/progress.hpp)
        g_progress.layer(t, x, cur.size());
        // "alive=1" tells you the frontier collapsed but not WHERE the survivors
        // are; the y span and mode mix is what says whether the search is stuck
        // on one ledge or spread out. Report more often once it gets thin.
        // also report the tick the frontier actually collapses on -- a periodic
        // sample tells you it happened somewhere in the last 500 ticks, which is
        // not enough to find the cause
        const bool crashed = prevAlive >= 8 && cur.size() * 4 <= prevAlive;
        if ((t % 500) == 0 || crashed
            || (cur.size() <= 64 && (t % 10) == 0)) {
            double ylo = 1e9, yhi = -1e9;
            // cube / ship / ball / ufo / wave / robot / spider / swing.
            // ALL of them: the print used to stop at 4, so a mode portal that
            // never fired was invisible here -- which is exactly how lv19's
            // robot section was planned as a cube for a whole session.
            int nm[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            // ...and how many are MINI. A size portal that the frontier is
            // missing shows up here and nowhere else: on lv18 every state
            // stayed mini past x=20,386 and the level is unplayable that way.
            int nMini = 0;
            for (const State& s : cur) {
                ylo = std::min(ylo, (double)s.y);
                yhi = std::max(yhi, (double)s.y);
                if (s.mode < 8) ++nm[s.mode];
                if (s.mini) ++nMini;
            }
            // ...and the SPEED spread. Since speed is per state the frontier can
            // hold branches that took a speed portal and branches that flew past
            // it, and those are at different x at the same tick -- which is
            // exactly the freedom the old shared timeline could not represent.
            // "x" above is the leader; xlo says how far back the slowest is.
            double xlo = 1e18;
            std::vector<float> spds;
            for (const State& s : cur) {
                xlo = std::min(xlo, model.progress(s).x);
                const float d = (s.dx > 0.f) ? s.dx : curDxF;
                if (std::find(spds.begin(), spds.end(), d) == spds.end())
                    spds.push_back(d);
            }
            std::sort(spds.begin(), spds.end());
            char sp[64] = "";
            for (size_t i = 0, o = 0; i < spds.size() && o + 8 < sizeof sp; ++i)
                o += (size_t)std::snprintf(sp + o, sizeof sp - o, "%s%.2f",
                                           i ? "/" : "", (double)spds[i]);
            std::printf("t=%lld x=%.0f alive=%zu arena=%zu y=[%.0f,%.0f] "
                        "cbswrpg=%d/%d/%d/%d/%d/%d/%d/%d mini=%d dx=%s xlo=%.0f "
                        "born=%lld died=%lld merged=%lld\n",
                        t, x, cur.size(), arena.size(),
                        cur.empty() ? 0.0 : ylo, cur.empty() ? 0.0 : yhi,
                        nm[0], nm[1], nm[2], nm[3], nm[4], nm[5], nm[6], nm[7],
                        nMini, sp,
                        cur.empty() ? 0.0 : xlo, nBornPrev, nDiedPrev,
                        nBornPrev - nDiedPrev - (long long)cur.size());
            if (g_memStat) {
                // 1) nodes still reachable from the live frontier. This is the
                // exact size a mark-compact GC would shrink the arena to, so
                // this one number decides whether a GC is worth building.
                std::vector<bool> mark(arena.size(), false);
                size_t reach = 0;
                for (const State& s : cur)
                    for (uint32_t i = s.parent; i != 0 && !mark[i];
                         i = arena[i].parent()) {
                        mark[i] = true;
                        ++reach;
                    }
                // 2) dual census. The mirror-compression proposal assumes the
                // frontier in a mirror region is a 2-body PRODUCT of the two
                // state sets; but while the pair IS a mirror, p2 is a function
                // of (p1, split axis) and the dedupe key already collapses it.
                // If dual ~= p1cells * axes there is no product to compress;
                // if dual >> p1cells * axes, the product is real.
                size_t nDual = 0, nMirror = 0;
                std::unordered_set<uint64_t> p1All, p1Mir, axes;
                for (const State& s : cur) {
                    if (!s.dual) continue;
                    ++nDual;
                    const bool flying = (s.mode == 1 || s.mode == 3);
                    const double ys = flying ? g_shipYq : g_cubeYq;
                    const double vs = flying ? g_shipVq : g_cubeVq;
                    const uint64_t c1 =
                        ((uint64_t)(uint32_t)(int32_t)std::lround(s.y * ys) << 32)
                        ^ (uint32_t)(int32_t)std::lround(s.vy * vs);
                    p1All.insert(c1);
                    // exact mirror: opposite gravity, opposite vy, both airborne
                    if (s.flip2 != s.flip && !s.grounded && !s.grounded2
                        && std::fabs((double)s.vy + (double)s.vy2) < 0.05) {
                        ++nMirror;
                        p1Mir.insert(c1);
                        // the split axis, 0.5 px bins: y2 = 2*axis - y
                        axes.insert((uint64_t)(int64_t)std::llround(
                            ((double)s.y + (double)s.y2)));
                    }
                }
                const double MiB = 1024.0 * 1024.0;
                std::printf(
                    "memstat t=%lld arena=%zu (%.0f MiB) reach=%zu (%.1f%%) "
                    "state=%zuB cur=%zu/%zu nxtcap=%zu seen=%zu/%zu\n",
                    t, arena.size(),
                    (double)(arena.size() * sizeof(Node)) / MiB, reach,
                    arena.empty() ? 0.0 : 100.0 * (double)reach / (double)arena.size(),
                    sizeof(State), cur.size(), cur.capacity(), nxt.capacity(),
                    seen.size(), seen.bucket_count());
                if (nDual)
                    std::printf(
                        "memstat-dual t=%lld dual=%zu p1cells=%zu product=%.1f "
                        "mirror=%zu (%.0f%%) p1mir=%zu axes=%zu\n",
                        t, nDual, p1All.size(),
                        (double)nDual / (double)p1All.size(), nMirror,
                        100.0 * (double)nMirror / (double)nDual, p1Mir.size(),
                        axes.size());
            }
            std::fflush(stdout);
        }
    }
    // The answer. A goal (or a stop the caller asked for) is Solved; without one, a horizon search
    // still emits its deepest branch (Partial: the driver wants to reach the wall); otherwise there
    // is no plan, and the frontier's last tick and x say where it stuck.
    if (solved) {
        out.kind = SearchResult::Solved;
        out.goal = goalState;
    } else if (horizon > 0 && !nxt.empty()) {
        out.kind = SearchResult::Partial;
        out.goal = rules.pick(nxt);
        if (g_partialByX && haveAny) {
            double lastX = -1e18;
            for (const State& s : nxt) lastX = std::max(lastX, (double)s.xAbs);
            if (bestAnyX > lastX + 1.0) {
                std::printf("partialbyx: the last layer (t=%lld x=%.1f) is not the furthest; emitting the layer at t=%lld x=%.1f\n", bestT, lastX, bestAnyT, bestAnyX);
                out.goal = bestAnyS;
                bestT = bestAnyT;
                bestX = bestAnyX;
            }
        }
        // --partialphys: the frontier ended by the loop's own bounds (every child of the last layer died by the out-of-play prune, none by the game's rules),
        // so the deepest branch is a body flying out of the level, which GD takes to the ceiling (lv22 R7: nine of 55 rounds). Emit the state that stepped
        // the latest PHYSICAL death instead: that plan ends where the model says the game kills it, which GD confirms or refutes.
        if (g_partialPhys && havePhys && physT >= 0 && lyPrunePrev > 0 && lyPhysPrev == 0 && physT < bestT) {
            std::printf("partialphys: the last layer (t=%lld x=%.1f) ended by the out-of-play bound only (%lld children); emitting the parent of the latest physical death, t=%lld x=%.1f\n",
                        bestT, bestX, lyPrunePrev, physT, (double)physS.xAbs);
            out.goal = physS;
            bestT = physT;
            bestX = (double)physS.xAbs;
        }
    } else {
        out.kind = SearchResult::NoPlan;
        // after the final swap the previous layer's states sit in nxt
        out.lastLayer.assign(nxt.begin(), nxt.begin() + (std::ptrdiff_t)std::min<size_t>(nxt.size(), 12));
    }
    out.t = bestT;
    out.x = bestX;
    if (out.kind != SearchResult::NoPlan) planFromArena(arena, out.goal, out.inputs);
    if (lpOn) {   // --ladderprefix: the next attempt may start from this one's saves
        g_lpc.arena.swap(arena);
        g_lpc.arenaMode.swap(arenaMode);
        g_lpc.usable = true;
    }
    return -1;
}

// One solve: everything a command line asks for. cliMain below is this, unless --capladder
// asks for a sequence of them.

// ---- a second search: a plain beam (prototype, --search beam) ------------------------------------
// The search contract's second implementation, for the yardstick to compare the DP against -- not a
// replacement. Each tick: the model fires the autonomous triggers; every state of the frontier is
// stepped on its own (a group of one, as the model's walk does) with each input the model allows;
// the rules credit and kill (afterStep); children the model keys the same are merged, the first one
// kept; and the B with the best rules.score survive (--beam B). No group sharing, no hi/lo pair per
// cell, no cap classes, no GC, no threads. Deterministic: states are stepped in (frame, x, index)
// order and ties keep that order. It fills the same frontier / arena / goal as searchLayers, so
// cliMainOnce rebuilds, walks and writes its plan the same way, and answers in the same shape.
template <class E, class Rules>
inline int searchBeam(E& e, const Rules& rules, SearchResult& out) {
    auto& t0 = e.in.t0;
    auto& horizon = e.in.horizon;
    auto& planCut = e.in.planCut;
    auto& tEnd = e.in.tEnd;
    LayerWork work(e.in.init);   // the frontier starts at the root
    auto& cur = work.cur;
    auto& nxt = work.nxt;
    SearchArena store(e.in.checkWanted);   // this method's states
    auto& arena = store.arena; ProbeArenaScope probeScope(&store.arena);
    auto& checkWanted = e.in.checkWanted;
    auto& arenaMode = store.arenaMode;
    auto& model = e.in.model;
    auto& bestT = e.outcome.bestT;
    auto& bestX = e.outcome.bestX;
    auto& goalState = e.outcome.goalState;
    auto& solved = e.outcome.solved;
    auto& maxAlive = e.outcome.maxAlive;
    auto& capHits = e.outcome.capHits;
    auto& capDropped = e.outcome.capDropped;
    auto& prevAlive = work.prevAlive;
    auto& curDxF = e.in.curDxF;
    const size_t B = (size_t)std::max(1, g_beamWidth);
    std::printf("search: beam, width %zu\n", B);
    struct Cand {
        State s;          // the child, its parent the PARENT's arena node until it is kept
        uint8_t action;
        double score;
    };
    std::vector<Cand> cand;
    std::vector<size_t> order, rank;
    std::unordered_set<uint64_t> seenKeys;
    // keep one candidate: its arena node, and the state that now owns it
    auto keep = [&](const Cand& c) -> State {
        arena.push_back(Node{c.s.parent | ((uint32_t)c.action << 31)});
        if (checkWanted) arenaMode.push_back(c.s.mode);
        State k = c.s;
        k.parent = (uint32_t)(arena.size() - 1);
        k.action = c.action;
        return k;
    };
    for (long long t = t0 + 1; t <= tEnd && !solved; ++t) {
        nxt.clear();
        cand.clear();
        seenKeys.clear();
        model.fireAutoTriggers(cur, t);
        order.resize(cur.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const Progress pa = model.progress(cur[a]), pb = model.progress(cur[b]);
            return pa.frame != pb.frame ? pa.frame < pb.frame : pa.x < pb.x;
        });
        long long goalAt = -1;   // index into cand of the goal found this tick
        for (size_t oi : order) {
            const State& s = cur[oi];
            const int frame = (int)s.frame;
            const bool rev = s.rev != 0;
            // a group of one: its window is its own advance, its doors and lock its own
            const double sdx = rev ? -(double)((s.dx > 0.f) ? s.dx : curDxF)
                                   : (double)((s.dx > 0.f) ? s.dx : curDxF);
            const float dxUsed = (float)sdx;
            const double xPrev = (double)s.xAbs;
            const double x = (double)advanceX(s.xAbs, dxUsed);
            const double wLo = std::min(xPrev, x), wHi = std::max(xPrev, x);
            model.fireRotTriggers(frame, rev, xPrev, xPrev, wLo, wHi, t);
            GroupEnv env;
            double pLo = wLo, pHi = wHi;
            p2Widen(pLo, pHi, p2Dx(s), p2Dx(s));   // --p2xoff
            model.prepare(env, frame, s.trig, (int)s.trigT, s.fireB, s.lockOff + (float)sdx, x, xPrev,
                          dxUsed, pLo, pHi, t, GroupOpts{true, true, false});
            const StepCtx K = env.ctx();
            for (int input = 0; input < 2; ++input) {
                if (!model.inputAllowed(s, input, 1, t)) continue;
                Child kid;
                bool dead = false;
                kid.s = model.advance(s, input, K, dead, nullptr);
                model.noteInput(s, kid.s, input);
                togWorldStep(s, kid.s, input);
                model.fireGates(kid.s, s.action, input, K, frame, t, dead);
                rules.afterStep(s, kid, input, K, frame, t, dead);
                if (dead) continue;
                if (!seenKeys.insert(modelKeyOf(kid.s, t) ^ rules.keyBits(kid.s)).second) continue;
                kid.s.parent = s.parent;
                cand.push_back(Cand{kid.s, (uint8_t)input, rules.score(kid.s)});
                if (rules.reached(kid.s)
                    && (goalAt < 0 ? !solved || rules.prefer(kid.s.tight, goalState.tight)
                                   : rules.prefer(kid.s.tight, cand[(size_t)goalAt].s.tight)))
                    goalAt = (long long)cand.size() - 1;
            }
        }
        if (goalAt >= 0) {
            goalState = keep(cand[(size_t)goalAt]);
            solved = true;
        }
        // the B best survive, ties in stepping order
        rank.resize(cand.size());
        for (size_t i = 0; i < rank.size(); ++i) rank[i] = i;
        std::stable_sort(rank.begin(), rank.end(),
                         [&](size_t a, size_t b) { return cand[a].score > cand[b].score; });
        if (cand.size() > maxAlive) maxAlive = cand.size();
        if (cand.size() > B) {
            ++capHits;
            capDropped += (long long)(cand.size() - B);
            rank.resize(B);
        }
        std::sort(rank.begin(), rank.end());   // the frontier in stepping order
        for (size_t i : rank) nxt.push_back(keep(cand[i]));
        prevAlive = cur.size();
        cur.swap(nxt);
        if (gdcol::g_slStore) gdcol::g_slStore->gc(cur);   // --shadowstep (searchLayers' note)
        double xl = 0.0;
        for (const State& s : cur) xl = std::max(xl, model.progress(s).x);
        if (!cur.empty()) { bestT = t; bestX = xl; }
        if (cur.empty()) break;
        // the caller's horizon, as searchLayers keeps it: search on to twice the horizon, then cut
        if (horizon > 0 && !solved && t - t0 >= horizon * 2) {
            solved = true;
            g_outcome.horizonCut = true;
            goalState = rules.pick(cur);
            planCut = t0 + horizon;
            break;
        }
        if (g_check.cancel.load(std::memory_order_relaxed)
            || g_ladderStop.load(std::memory_order_relaxed)) {
            std::printf("CANCELLED: at t=%lld (beam search abandoned by the caller)\n", t);
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = t;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;
        }
    }
    if (solved) {
        out.kind = SearchResult::Solved;
        out.goal = goalState;
    } else if (horizon > 0 && !nxt.empty()) {
        out.kind = SearchResult::Partial;
        out.goal = rules.pick(nxt);
    } else {
        out.kind = SearchResult::NoPlan;
        // after the final swap the previous layer's states sit in nxt
        out.lastLayer.assign(nxt.begin(), nxt.begin() + (std::ptrdiff_t)std::min<size_t>(nxt.size(), 12));
    }
    out.t = bestT;
    out.x = bestX;
    if (out.kind != SearchResult::NoPlan) planFromArena(arena, out.goal, out.inputs);
    return -1;
}


// ---- the run: what a search may keep across the calls of one level's solve ------------------------
// A level's solve is many calls (the first search, the ladder's anchors, the fine passes, the
// resims), and the host -- the mod's repair loop -- tells the core where the run begins and ends and
// what each flight of a plan did (dp_bridge runBegin / runVerified / runEnd). Kept OUT of
// resetInvocationState on purpose: a call does not end a run. A run is one level, so begin refuses
// while another is still open -- the carry-over a one-session cold run exists to catch, caught at
// the start of the level it would leak into. The searches here keep no run state yet; the run
// records what it was told, and the plan this core emitted last, so a verification can say whether
// the plan flown was the search's own.
struct SearchRun {
    bool open = false;
    int level = -1;
    uint64_t seed = 0;
    long long verified = 0;   // flights reported
    long long own = 0;        // ...of which flew the plan this core emitted last
    long long calls = 0;      // search calls so far (the call index a seeded search mixes in)
    long long nondet = 0;     // ...of which stopped other than on their own budget (not reproducible)
    long long lastT0 = -1;    // the plan this core emitted last (cliMainOnce): its anchor...
    std::vector<std::pair<long long, int>> lastEdges;   // ...and its input edges (press, level)
    // The plan the host's section search spliced in last (runNoteSplice): from its seam on it is
    // the section search's, not this core's. A flight of it is counted apart (`via=secsolve`).
    long long spliceT0 = -1;
    std::vector<std::pair<long long, int>> spliceEdges;
    long long viaSplice = 0;  // flights reported that flew it
};
inline SearchRun g_run;

inline bool runBegin(int level, uint64_t seed, std::string& why) {
    if (g_run.open) {
        why = "a run is still open (level " + std::to_string(g_run.level) + ", "
              + std::to_string(g_run.verified) + " flights reported) at the start of level "
              + std::to_string(level);
        return false;
    }
    g_run = SearchRun{};
    g_run.open = true;
    g_run.level = level;
    g_run.seed = seed;
    return true;
}
// The plan a call finally answered with, read back from its file (`input=press,level` lines), in
// place of the one its last search wrote: the cap ladder can put an earlier attempt's plan back
// (cliMain step 5) and the gamble takes the second core's (PlainElsewhere::take). The host's bridge
// calls this after cliMain returns, when the call wrote a plan. The anchor (lastT0) is the call's:
// every attempt of one call searches from the same --start. False when the file cannot be read.
inline bool runNotePlanFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::vector<std::pair<long long, int>> e;
    std::string ln;
    while (std::getline(in, ln)) {
        long long press = 0;
        int level = 0;
        if (std::sscanf(ln.c_str(), "input=%lld,%d", &press, &level) == 2) e.emplace_back(press, level);
    }
    g_run.lastEdges = std::move(e);
    return true;
}
// Did the flight fly the plan this core emitted last? What the button does from that plan's seam on
// is what the emitted plan says -- compared as levels, not as edge lists: the plan writer can put
// the seam's edge a tick before the anchor (its latency) or emit one that changes nothing, and the
// host's splice onto the verified prefix keeps or merges such an edge as it sees fit. The prefix
// before the seam is not compared. No plan emitted yet -> no. "Emitted" is what the call answered
// with (the host reads it back with runNotePlanFile), not the last of a ladder's attempts.
inline bool runFlewPlan(const std::vector<std::pair<long long, int>>& flown, long long t0,
                        const std::vector<std::pair<long long, int>>& edges) {
    if (t0 < 0) return false;
    using Edge = std::pair<long long, int>;
    auto byTick = [](const Edge& a, const Edge& b) { return a.first < b.first; };
    std::vector<Edge> fl(flown.begin(), flown.end()), em(edges.begin(), edges.end());
    std::stable_sort(fl.begin(), fl.end(), byTick);
    std::stable_sort(em.begin(), em.end(), byTick);
    long long cut = t0;
    if (!em.empty()) cut = std::min(cut, em.front().first);
    int start = 0;   // the button as the flown plan left it just before the seam
    for (const Edge& e : fl)
        if (e.first < cut) start = e.second;
    auto changes = [&](const std::vector<Edge>& v) {   // only the edges that change the level
        std::vector<Edge> out;
        int lvl = start;
        for (const Edge& e : v) {
            if (e.first < cut || e.second == lvl) continue;
            lvl = e.second;
            out.push_back(e);
        }
        return out;
    };
    return changes(fl) == changes(em);
}
inline bool runOwnPlan(const std::vector<std::pair<long long, int>>& flown) {
    return runFlewPlan(flown, g_run.lastT0, g_run.lastEdges);
}
// The host's section search spliced `edges` (the whole plan as installed, press / level) from
// tick `t0` on. Compared like runOwnPlan, from that seam.
inline void runNoteSplice(const std::vector<std::pair<long long, int>>& edges, long long t0) {
    if (!g_run.open) return;
    g_run.spliceT0 = t0;
    g_run.spliceEdges = edges;
}
// A flight of `flown` ended: died at `deathT` / `x`, or cleared. `divT`: the first tick at which
// the host's recorder saw the model and GD part (-1: none recorded). A GD death is always a
// physical one, so no kind is passed. Returns the line the host logs.
inline std::string runVerified(const std::vector<std::pair<long long, int>>& flown, long long deathT,
                               double x, bool cleared, long long divT) {
    if (!g_run.open) return "dprun: verified with no run open";
    const bool own = runOwnPlan(flown);
    const bool splice = !own && runFlewPlan(flown, g_run.spliceT0, g_run.spliceEdges);
    ++g_run.verified;
    if (own) ++g_run.own;
    if (splice) ++g_run.viaSplice;
    char b[240];
    std::snprintf(b, sizeof b,
                  "dprun: verified %s t=%lld x=%.1f diverged=%lld own=%d (%lld of %lld) nondet=%lld%s",
                  cleared ? "cleared" : "death", deathT, x, divT, own ? 1 : 0, g_run.own,
                  g_run.verified, g_run.nondet, splice ? " via=secsolve" : "");
    return b;
}
inline std::string runEnd() {
    if (!g_run.open) return "dprun: end (no run open)";
    const std::string s = "dprun: end level=" + std::to_string(g_run.level) + " flights="
                          + std::to_string(g_run.verified) + " own=" + std::to_string(g_run.own)
                          + " calls=" + std::to_string(g_run.calls)
                          + " nondet=" + std::to_string(g_run.nondet)
                          + (g_run.viaSplice ? " via-secsolve=" + std::to_string(g_run.viaSplice)
                                             : std::string());
    g_run = SearchRun{};
    return s;
}


// ---- a third search: Monte Carlo rollouts (reference, --search montecarlo) -------------------------
// The search contract's implementation that draws random numbers. A round samples up to
// --mcrollouts input plans, each flipping the button at --mctoggles random ticks inside the next
// --mcwindow ticks, walks each one through the model until it dies, keeps the one that lived longest
// and commits the first third of that gain. A round that gains fewer than 3 ticks backs off, further
// each time, and the back-off resets whenever the committed path goes deeper than it ever has.
// Nothing prefers margin: the plan is as tight as luck made it.
//   Reproducible: the random numbers come from (seed, anchor, the run's call index), and the budget
// is a step count (--mcsteps), not seconds. The wall clock (--mcsafety) is only a safety valve; a
// call it stops is marked not deterministic (SearchResult::deterministic, and the run's count).
// Rollouts run one after another, so the draws do not depend on the thread count.
//   Declined (the caller's default search answers instead) where a rollback cannot rewind: gameplay
// rotation and the control windows keep one-shot state outside State, a turned start frame, and
// coins, whose route this walk does not model.
// The button's press latency is the search's own: a press at t takes effect at t+1 or t+2 (mode),
// and what the arena records per tick is the input in effect -- the same thing the other searches
// record, so cliMainOnce's witness walks the plan and writes its edges with the usual latency.
inline uint64_t mcMix(uint64_t z) {   // splitmix64's finaliser
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
template <class E, class Rules>
inline int searchMontecarlo(E& e, const Rules& rules, SearchResult& out) {
    auto& t0 = e.in.t0;
    auto& init = e.in.init;
    auto& tEnd = e.in.tEnd;
    SearchArena store(e.in.checkWanted);   // this method's states
    auto& arena = store.arena; ProbeArenaScope probeScope(&store.arena);
    auto& checkWanted = e.in.checkWanted;
    auto& arenaMode = store.arenaMode;
    auto& model = e.in.model;
    auto& bestT = e.outcome.bestT;
    auto& bestX = e.outcome.bestX;
    auto& goalState = e.outcome.goalState;
    auto& solved = e.outcome.solved;
    auto& curDxF = e.in.curDxF;
    auto& horizon = e.in.horizon;
    auto& planCut = e.in.planCut;
    // The caller's horizon, kept as searchLayers keeps it: alive at twice the horizon is a goal, and
    // the plan is then cut back to t0 + horizon -- a prefix KNOWN to stay alive that much longer.
    const long long hzEnd = horizon > 0 ? t0 + horizon * 2 : -1;
    // (...nor a world with item gates, which its walks never fire: Model::fireGates.)
    if (!g_rotTrig.empty() || !g_ctrlWin.empty() || (!g_ctrlWin2.empty())
        || init.frame != 0 || rules.needsAfterStep() || model.hasGates()) {
        std::printf("search: montecarlo declines (rotation=%zu control-windows=%zu frame=%d "
                    "coins=%d%s)\n", g_rotTrig.size(),
                    g_ctrlWin.size() + (g_ctrlWin2.size()), (int)init.frame,
                    rules.needsAfterStep() ? 1 : 0,
                    model.hasGates() && !rules.needsAfterStep() ? " gates=1" : "");
        out.kind = SearchResult::Declined;
        return -1;
    }
    const uint64_t seed = g_run.open ? g_run.seed : (uint64_t)g_searchSeed;
    const long long call = g_mcCall >= 0 ? g_mcCall : g_run.calls;
    std::mt19937_64 rng(mcMix(seed ^ mcMix((uint64_t)t0 ^ mcMix((uint64_t)call))));
    std::printf("search: montecarlo seed=%llu t0=%lld call=%lld rollouts=%d window=%d toggles=%d "
                "steps=%lld stall=%lld\n", (unsigned long long)seed, t0, call, g_mcRollouts,
                g_mcWindow, g_mcToggles, g_mcSteps, g_mcStall);
    using Clock = std::chrono::steady_clock;
    const auto mcStart = Clock::now();
    auto elapsedS = [&] { return std::chrono::duration<double>(Clock::now() - mcStart).count(); };
    auto latOf = [](uint8_t m) { return (m == 1 || m == 3) ? 2 : 1; };
    // One committed or rolled-out tick: the state AFTER the tick, the input in effect, the button as
    // pressed, and the effects already scheduled (a press lands 1 or 2 ticks later).
    struct McNode {
        State s;
        int curIn = 0;
        int held = 0;
        int nfx = 0;
        long long fxT[4] = {0, 0, 0, 0};
        int fxV[4] = {0, 0, 0, 0};
        bool pressed = false;   // the button changed on this tick
    };
    long long steps = 0;
    auto press = [&](McNode& n, long long t) {
        n.held ^= 1;
        n.pressed = true;
        if (n.nfx < 4) {
            n.fxT[n.nfx] = t + latOf(n.s.mode);
            n.fxV[n.nfx] = n.held;
            ++n.nfx;
        }
    };
    // Step tick t on n in place, through the model (a group of one, as the model's walk builds it).
    // Returns true if the player died on it.
    auto stepTick = [&](McNode& n, long long t) -> bool {
        ++steps;
        n.pressed = false;
        int k2 = 0;   // effects due now, in scheduling order (a later press wins a tie)
        for (int k = 0; k < n.nfx; ++k) {
            if (n.fxT[k] <= t) n.curIn = n.fxV[k];
            else { n.fxT[k2] = n.fxT[k]; n.fxV[k2] = n.fxV[k]; ++k2; }
        }
        n.nfx = k2;
        State& s = n.s;
        const float dxU = ((s.dx > 0.f) ? s.dx : curDxF) * (s.rev ? -1.f : 1.f);
        const double xPrev = (double)s.xAbs;
        const double x = (double)advanceX(s.xAbs, dxU);
        for (AutoTrig& A : g_autoTrig) {   // autonomous fires on this walk's own crossing, read on the x of tick t-1 as the search reads it (model.hpp fireAutoTriggers; cli.hpp --replay has the GD measurement)
            if (A.fireT >= 0) continue;
            if (xPrev < A.cx) break;   // sorted by cx
            A.fireT = (int)(t - 1) + A.delay;
            A.fireX = A.lockLastX = xPrev;
        }
        GroupEnv env;
        double pLo = x, pHi = x;
        p2Widen(pLo, pHi, p2Dx(s), p2Dx(s));   // --p2xoff
        model.prepare(env, (int)s.frame, s.trig, (int)s.trigT, s.fireB,
                      s.lockOff + (float)(x - xPrev), x, xPrev, dxU, pLo, pHi, t,
                      GroupOpts{false, false, false});
        const StepCtx K = env.ctx();
        g_nearOrb = 0;
        bool dead = false;
        State c = model.advance(s, n.curIn, K, dead, nullptr);
        c.action = (uint8_t)n.curIn;
        s = c;
        return dead;
    };
    // Walk from `from` (the state after tick T) with the button flipped at the press ticks in `flips`
    // (sorted, all >= T). Returns the last tick the player was alive on, capped at `until`; `goal` is
    // set if it reached the goal. With `keep`, every tick is appended to it.
    // Every walk starts its world afresh (a jump to T + 1), so what it meets does not depend on where
    // the walk before it ended -- which the rounds' lanes below rely on.
    auto walk = [&](const McNode& from, long long T, const std::vector<long long>& flips,
                    long long until, bool& goal, std::vector<McNode>* keep) -> long long {
        McNode n = from;
        size_t fi = 0;
        goal = false;
        model.forgetWorld((int)from.s.frame);
        while (fi < flips.size() && flips[fi] == T) { press(n, T); ++fi; }
        for (long long t = T + 1; t <= until; ++t) {
            if (stepTick(n, t)) return t - 1;
            while (fi < flips.size() && flips[fi] == t) { press(n, t); ++fi; }
            if (keep) keep->push_back(n);
            if (rules.reached(n.s) || (hzEnd > 0 && t >= hzEnd)) { goal = true; return t; }
        }
        return until;
    };
    std::vector<McNode> path;   // path[k] = the state after tick t0 + k
    {
        McNode n0;
        n0.s = init;
        n0.curIn = (int)init.held;
        n0.held = (int)init.held;
        path.push_back(n0);
    }
    // Commit a rollout: a flip on T itself belongs to the node already on the path, the rest is
    // appended by walking it.
    auto commit = [&](long long T, const std::vector<long long>& fl, long long upto) {
        McNode& head = path.back();
        std::vector<long long> rest;
        for (long long p : fl) {
            if (p == T) press(head, T);
            else rest.push_back(p);
        }
        bool g = false;
        const McNode start = head;
        walk(start, T, rest, upto, g, &path);
    };
    // The autonomous triggers' one-shot state lives in g_autoTrig, which every walk writes. Before a
    // walk from tick T it is rebuilt from the COMMITTED path: fired at the first committed tick whose
    // x reached cx, else unfired; what the start had already resolved is kept.
    struct AutoInit { int fireT; double fireX, lockLastX; };
    std::vector<AutoInit> autoInit;
    for (const AutoTrig& A : g_autoTrig) autoInit.push_back({A.fireT, A.fireX, A.lockLastX});
    auto resetAuto = [&](long long T) {
        const size_t last = (size_t)(T - t0);   // path[0..last] is committed
        for (size_t i = 0; i < g_autoTrig.size(); ++i) {
            AutoTrig& A = g_autoTrig[i];
            if (autoInit[i].fireT >= 0) {
                A.fireT = autoInit[i].fireT;
                A.fireX = autoInit[i].fireX;
                A.lockLastX = autoInit[i].lockLastX;
                continue;
            }
            size_t a = 1, b = last + 1;   // the first k in [1, last] with x >= cx
            while (a < b) {
                const size_t mid = (a + b) / 2;
                if ((double)path[mid].s.xAbs < A.cx) a = mid + 1; else b = mid;
            }
            if (a <= last) {
                A.fireT = (int)(t0 + (long long)a) + A.delay;
                A.fireX = A.lockLastX = (double)path[a].s.xAbs;
            } else {
                A.fireT = -1;
            }
        }
    };
    // A round's rollouts are stepped kMcLanes at a time, together, tick by tick (runLanes). Each
    // lane's result is the one walk() gives it alone, but the world -- the moving geometry
    // Model::prepare places, measured 2026-10-05 at 97% of a step on lv19/lv20 (35-55 us against
    // 1 us for the step itself) -- is placed once a tick for every lane whose inputs to it are the
    // same: a class. A class splits when its lanes' x, fired boxes, their ticks or the lock differ,
    // and while there are several, each class's world is carried from tick to tick
    // (Model::saveWorld). The lanes are taken in rollout order and the width is fixed, so the
    // result depends on the seed alone, not on the machine or the thread count.
    constexpr int kMcLanes = 32;
    struct Lane {
        McNode n;
        size_t fi = 0;   // the next flip
        long long lived = 0, steps = 0;
        bool goal = false, done = false;
        int cls = 0;
    };
    struct LaneClass {
        std::vector<AutoInit> autoSt;   // the autonomous triggers as this class's walk left them
        std::vector<double> carry;      // its world after its last tick (Model::saveWorld)
        bool hasCarry = false;
    };
    std::vector<Lane> lanes((size_t)kMcLanes);
    std::vector<std::vector<long long>> laneFlips((size_t)kMcLanes);
    auto saveAuto = [&](std::vector<AutoInit>& v) {
        v.resize(g_autoTrig.size());
        for (size_t i = 0; i < g_autoTrig.size(); ++i)
            v[i] = {g_autoTrig[i].fireT, g_autoTrig[i].fireX, g_autoTrig[i].lockLastX};
    };
    auto loadAuto = [&](const std::vector<AutoInit>& v) {
        for (size_t i = 0; i < g_autoTrig.size() && i < v.size(); ++i) {
            g_autoTrig[i].fireT = v[i].fireT;
            g_autoTrig[i].fireX = v[i].fireX;
            g_autoTrig[i].lockLastX = v[i].lockLastX;
        }
    };
    // What a lane hands Model::prepare on a tick, beyond its state's own fields.
    struct LanePre { float dxU = 0.f, lockAt = 0.f; double xPrev = 0.0, x = 0.0; };
    std::vector<LanePre> pre((size_t)kMcLanes);
    std::vector<LaneClass> cls, nxt;
    std::vector<int> act, rep, prevOf, kids;
    // walk() for lanes 0..nl-1 at once, from `from` (the state after tick T), each with its
    // laneFlips; leaves each lane's lived / goal / steps.
    auto runLanes = [&](const McNode& from, long long T, long long until, size_t nl) {
        const int f = (int)from.s.frame;
        cls.assign(1, LaneClass{});
        resetAuto(T);
        saveAuto(cls[0].autoSt);
        for (size_t l = 0; l < nl; ++l) {
            Lane& L = lanes[l];
            L = Lane{};
            L.n = from;
            const std::vector<long long>& fl = laneFlips[l];
            while (L.fi < fl.size() && fl[L.fi] == T) { press(L.n, T); ++L.fi; }
        }
        // The class whose world the model holds, as of tick heldT. The first tick seeks afresh, as
        // walk() does, so class 0 at T has no world to carry: putting it back is forgetting.
        model.forgetWorld(f);
        int held = -1;
        long long heldT = T;
        auto sameInputs = [&](size_t a, size_t b) {
            const State& sa = lanes[a].n.s;
            const State& sb = lanes[b].n.s;
            return pre[a].x == pre[b].x && pre[a].xPrev == pre[b].xPrev && pre[a].dxU == pre[b].dxU
                   && pre[a].lockAt == pre[b].lockAt && sa.frame == sb.frame && sa.trig == sb.trig
                   && sa.trigT == sb.trigT && !std::memcmp(sa.fireB, sb.fireB, sizeof sa.fireB);
        };
        for (long long t = T + 1; t <= until; ++t) {
            act.clear();
            for (size_t l = 0; l < nl; ++l)
                if (!lanes[l].done) act.push_back((int)l);
            if (act.empty()) break;
            // Each lane's own half of stepTick: the effects due now, and where it goes.
            for (int l : act) {
                Lane& L = lanes[(size_t)l];
                McNode& n = L.n;
                ++L.steps;
                n.pressed = false;
                int k2 = 0;
                for (int k = 0; k < n.nfx; ++k) {
                    if (n.fxT[k] <= t) n.curIn = n.fxV[k];
                    else { n.fxT[k2] = n.fxT[k]; n.fxV[k2] = n.fxV[k]; ++k2; }
                }
                n.nfx = k2;
                const State& s = n.s;
                LanePre& P = pre[(size_t)l];
                P.dxU = ((s.dx > 0.f) ? s.dx : curDxF) * (s.rev ? -1.f : 1.f);
                P.xPrev = (double)s.xAbs;
                P.x = (double)advanceX(s.xAbs, P.dxU);
                P.lockAt = s.lockOff + (float)(P.x - P.xPrev);
            }
            // This tick's classes: a lane joins the first class of its parent with the same inputs.
            nxt.clear();
            rep.clear();
            prevOf.clear();
            for (int l : act) {
                Lane& L = lanes[(size_t)l];
                int c = 0;
                while (c < (int)rep.size()
                       && !(prevOf[(size_t)c] == L.cls && sameInputs((size_t)rep[(size_t)c], (size_t)l)))
                    ++c;
                if (c == (int)rep.size()) {
                    rep.push_back(l);
                    prevOf.push_back(L.cls);
                    nxt.emplace_back();
                }
                L.cls = c;   // (this tick's numbering from here on)
            }
            // A parent with several children has its world saved now, while the model holds it.
            kids.assign(cls.size(), 0);
            for (int p : prevOf) ++kids[(size_t)p];
            if (held >= 0 && heldT == t - 1 && kids[(size_t)held] > 1 && !cls[(size_t)held].hasCarry) {
                model.saveWorld(f, cls[(size_t)held].carry);
                cls[(size_t)held].hasCarry = true;
            }
            for (size_t c = 0; c < rep.size(); ++c) {
                const int p = prevOf[c];
                const Lane& R = lanes[(size_t)rep[c]];
                const LanePre& P = pre[(size_t)rep[c]];
                if (!(held == p && heldT == t - 1)) {
                    if (cls[(size_t)p].hasCarry) model.loadWorld(f, cls[(size_t)p].carry, t - 1);
                    else model.forgetWorld(f);   // class 0 at T
                }
                loadAuto(cls[(size_t)p].autoSt);
                for (AutoTrig& A : g_autoTrig) {   // autonomous fires on this class's own crossing, read on the x of tick t-1 (stepTick above)
                    if (A.fireT >= 0) continue;
                    if (P.xPrev < A.cx) break;   // sorted by cx
                    A.fireT = (int)(t - 1) + A.delay;
                    A.fireX = A.lockLastX = P.xPrev;
                }
                GroupEnv env;
                double pLo = P.x, pHi = P.x;
                p2Widen(pLo, pHi, p2Dx(R.n.s), p2Dx(R.n.s));   // --p2xoff
                model.prepare(env, (int)R.n.s.frame, R.n.s.trig, (int)R.n.s.trigT, R.n.s.fireB,
                              P.lockAt, P.x, P.xPrev, P.dxU, pLo, pHi, t,
                              GroupOpts{false, false, false});
                const StepCtx K = env.ctx();
                saveAuto(nxt[c].autoSt);
                held = (int)c;
                heldT = t;
                if (rep.size() > 1) {
                    model.saveWorld(f, nxt[c].carry);
                    nxt[c].hasCarry = true;
                }
                for (int l : act) {
                    Lane& L = lanes[(size_t)l];
                    if (L.cls != (int)c) continue;
                    g_nearOrb = 0;
                    bool dead = false;
                    State s2 = model.advance(L.n.s, L.n.curIn, K, dead, nullptr);
                    s2.action = (uint8_t)L.n.curIn;
                    L.n.s = s2;
                    if (dead) { L.done = true; L.lived = t - 1; continue; }
                    const std::vector<long long>& fl = laneFlips[(size_t)l];
                    while (L.fi < fl.size() && fl[L.fi] == t) { press(L.n, t); ++L.fi; }
                    if (rules.reached(L.n.s) || (hzEnd > 0 && t >= hzEnd)) {
                        L.done = true;
                        L.goal = true;
                        L.lived = t;
                    } else if (t == until) {
                        L.done = true;
                        L.lived = until;
                    }
                }
            }
            cls.swap(nxt);
        }
    };
    long long deepest = t0, back = 5, rounds = 0, rollouts = 0, backoffs = 0;
    // The rollout count when deepest last grew, and the longest run of rollouts without it.
    long long gainAt = 0, maxGap = 0;
    bool reached = false, valve = false, stalled = false;
    std::vector<long long> flips, bestFlips, f2;
    // The caller has stopped caring (the mod's session ended, or the ladder took the plain search
    // beside it), read as searchLayers and searchBeam read it. Once a round and once a batch of
    // lanes, so a cancel waits for one batch at most, not for the safety valve. The CLI never sets
    // either flag, so the draws and the output are unchanged.
    auto stopAsked = [&] {
        return g_check.cancel.load(std::memory_order_relaxed)
               || g_ladderStop.load(std::memory_order_relaxed);
    };
    auto cancelled = [&] {
        const long long t = t0 + (long long)path.size() - 1;
        std::printf("CANCELLED: at t=%lld (montecarlo search abandoned by the caller)\n", t);
        std::fflush(stdout);
        g_outcome.verdict = VerdictCancelled;
        g_outcome.cancelT = t;
        g_outcome.deepT = t;
        g_outcome.deepX = (double)path.back().s.xAbs;
        return 3;
    };
    while (true) {
        if (stopAsked()) return cancelled();
        if (steps >= g_mcSteps) break;
        if (g_mcStall > 0 && rollouts - gainAt >= g_mcStall) { stalled = true; break; }
        if (elapsedS() > g_mcSafetyS) { valve = true; break; }
        ++rounds;
        const long long T = t0 + (long long)path.size() - 1;
        if (T >= tEnd) break;
        const McNode from = path.back();
        long long best = T;
        bool bestGoal = false;
        bestFlips.clear();
        const long long until = std::min(T + (long long)g_mcWindow, tEnd);
        std::uniform_int_distribution<long long> pick(T, until - 1);
        auto draw = [&](std::vector<long long>& out) {
            flips.clear();
            for (int k = 0; k < g_mcToggles; ++k) flips.push_back(pick(rng));
            std::sort(flips.begin(), flips.end());
            out.clear();   // two flips on one tick cancel: keep a tick only if odd
            for (size_t i = 0; i < flips.size();) {
                size_t j = i;
                while (j < flips.size() && flips[j] == flips[i]) ++j;
                if ((j - i) & 1) out.push_back(flips[i]);
                i = j;
            }
        };
        // The rollouts in rollout order, kMcLanes at a time (runLanes); a round ends at the first
        // rollout, in that order, that reaches the goal or gets far enough -- as if they ran one by
        // one. The lanes drawn past it are thrown away and the generator is put back to just after it.
        bool roundDone = false;
        for (int r0 = 0; r0 < g_mcRollouts && !roundDone; r0 += kMcLanes) {
            if (r0 > 0 && stopAsked()) return cancelled();
            const int nl = std::min(kMcLanes, g_mcRollouts - r0);
            const std::mt19937_64 rngAt = rng;
            const auto pickAt = pick;
            for (int l = 0; l < nl; ++l) draw(laneFlips[(size_t)l]);
            runLanes(from, T, until, (size_t)nl);
            for (int l = 0; l < nl; ++l) {
                const Lane& L = lanes[(size_t)l];
                ++rollouts;
                steps += L.steps;
                if (L.goal || L.lived > best) {
                    best = L.lived;
                    bestFlips = laneFlips[(size_t)l];
                    bestGoal = L.goal;
                }
                if (bestGoal || best - T > g_mcWindow / 2) {
                    rng = rngAt;
                    pick = pickAt;
                    for (int k = 0; k <= l; ++k) draw(f2);
                    roundDone = true;
                    break;
                }
            }
        }
        if (bestGoal) {
            resetAuto(T);
            commit(T, bestFlips, best);
            reached = true;
            maxGap = std::max(maxGap, rollouts - gainAt);
            break;
        }
        if (best - T >= 3) {
            const long long commitT = T + (best - T) / 3;
            resetAuto(T);
            commit(T, bestFlips, commitT);
            if (commitT > deepest) {
                deepest = commitT;
                back = 5;
                maxGap = std::max(maxGap, rollouts - gainAt);
                gainAt = rollouts;
            }
            continue;
        }
        ++backoffs;   // stuck: back off, further each time, never past the start
        const long long keepT = std::max(t0, T - back);
        path.resize((size_t)(keepT - t0) + 1);
        back += (back > 100) ? 50 : 5;
        if (back > 20000) back = 5;
    }
    const long long lastT = t0 + (long long)path.size() - 1;
    resetAuto(lastT);   // the witness walks the committed path, with its triggers' firing
    if (!reached) maxGap = std::max(maxGap, rollouts - gainAt);
    std::printf("search: montecarlo %s at t=%lld x=%.1f in %.3fs rounds=%lld rollouts=%lld "
                "backoffs=%lld steps=%lld deepest=%lld maxgap=%lld%s\n",
                reached ? "SOLVED" : "GAVE UP", lastT, (double)path.back().s.xAbs, elapsedS(),
                rounds, rollouts, backoffs, steps, deepest, maxGap,
                valve ? " -- stopped by the wall-clock safety valve: NOT deterministic"
                      : stalled ? " -- stalled: no deeper commit in --mcstall rollouts" : "");
    // The committed path into the arena: one node per tick, its action the input in effect.
    uint32_t parent = init.parent;
    for (size_t k = 1; k < path.size(); ++k) {
        arena.push_back(Node{parent | ((uint32_t)(path[k].curIn & 1) << 31)});
        if (checkWanted) arenaMode.push_back(path[k].s.mode);
        parent = (uint32_t)(arena.size() - 1);
        path[k].s.parent = parent;
        path[k].s.action = (uint8_t)path[k].curIn;
    }
    out.deterministic = !valve;
    bestT = lastT;
    bestX = (double)path.back().s.xAbs;
    out.t = bestT;
    out.x = bestX;
    if (reached) {
        solved = true;
        goalState = path.back().s;
        out.kind = SearchResult::Solved;
        out.goal = goalState;
        if (!rules.reached(goalState)) {   // alive at twice the horizon, not at the goal
            g_outcome.horizonCut = true;
            planCut = t0 + horizon;
            std::printf("search: montecarlo alive at t=%lld = t0 + 2 x horizon; the plan is cut "
                        "to t=%lld\n", lastT, planCut);
        }
    } else if (path.size() > 1) {
        out.kind = SearchResult::Partial;
        out.goal = path.back().s;
    } else {
        out.kind = SearchResult::NoPlan;
    }
    if (out.kind != SearchResult::NoPlan) planFromArena(arena, out.goal, out.inputs);
    return -1;
}

}  // namespace dp
