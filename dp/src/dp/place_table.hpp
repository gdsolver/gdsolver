#pragma once
// P7-1 (restructure draft section 7.16): is the placement of the moving geometry a function of the WORLD?  Observation only (--placetable, default off).
//
// The search places the moving objects (Obj::dynObj) once per GROUP (dx, frame, rev, fire signature, xc: model.prepare). P7 would place them once per WORLD (worldKeyOf(s, no camera, with tog / xgate)). This
// instrument builds the table the reference version would give -- for every group of a layer, the placement of every dynamic object near it -- and asks, for each world that two or more groups of the layer hold
// states of: do those groups place every dynamic object at the same place?  A world whose groups disagree is a world the key does not tell apart yet; the first disagreements are printed with the group keys,
// so that what is missing from the key can be read off (the hypothesis: nothing is, for a world no player-lock Move acts on).
//
// A group's states can belong to several worlds (a group is wider than a world: restructure draft 7.11), so a group is entered once under each of its worlds. A placement is the dynamic objects' (uid, x, y, dx, dy)
// in the LEVEL's frame (a rotated gameplay frame is undone), sorted by uid, hashed twice: over every object, and over the objects no player-lock Move acts on (the P7 key for lock groups is a separate step).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dp/frames.hpp"
#include "dp/model.hpp"
#include "dp/trigshadow_live.hpp"
#include "dp/worldkey.hpp"

namespace dp {

struct PlaceObs {
    bool on = false;
    struct Ent { int32_t uid; double x, y, dx, dy; };
    struct G { uint64_t pdAll = 0, pdFree = 0; std::vector<Ent> ents; float dx = 0.f; int frame = 0, rev = 0; uint64_t fsig = 0; int xc = 0; size_t states = 0; double gx = 0.0; };   // gx: the group's own x (where its slowest-to-fastest members end up)
    long long lastT = -2;
    std::vector<G> groups;
    std::unordered_map<uint64_t, std::vector<int>> byWorld;   // world key -> indices into groups
    std::unordered_set<int32_t> lockUids;
    bool lockInit = false;
    // totals
    long long layers = 0, groupsSeen = 0, worldPairs = 0, worldsSeen = 0, shareable = 0, equalAll = 0, conflictAll = 0, conflictFree = 0, conflictLockOnly = 0, noDyn = 0;
    long long conflictRelX = 0, conflictOther = 0, otherFree = 0;   // a conflict is "relX" when every differing object differs only in x and (x - the group's x) is the same in both groups; "other" otherwise
    std::map<int32_t, long long> uidConflict;   // uid -> worlds in which two groups placed it differently
    int shown = 0;
    // which part of the group key, added to the world, makes the placement a function: a world is "split" under a subset S of (dx, frame, rev, xc, fsig) when two groups that agree on S disagree on the placement
    static constexpr int kSubsets = 8;
    static const char* subsetName(int i) { static const char* n[kSubsets] = {"W", "W+dx", "W+dx+frame+rev", "W+dx+frame+rev+xc", "W+dx+frame+rev+fsig", "W+frame+rev+xc+fsig", "W+dx+xc+fsig", "W+dx+frame+rev+xc+fsig"}; return n[i]; }
    static uint64_t subsetKey(int i, const G& g) {
        const uint64_t dxb = (uint64_t)*reinterpret_cast<const uint32_t*>(&g.dx);
        uint64_t h = 0x5355ULL;
        const bool dx = i == 1 || i == 2 || i == 3 || i == 4 || i == 6 || i == 7, fr = i == 2 || i == 3 || i == 4 || i == 5 || i == 7, xc = i == 3 || i == 5 || i == 6 || i == 7, fs = i == 4 || i == 5 || i == 6 || i == 7;
        if (dx) h = mix(h, dxb);
        if (fr) { h = mix(h, (uint64_t)(uint32_t)g.frame); h = mix(h, (uint64_t)(uint32_t)g.rev); }
        if (xc) h = mix(h, (uint64_t)(uint32_t)g.xc);
        if (fs) h = mix(h, g.fsig);
        return h;
    }
    long long splitWorlds[kSubsets] = {0, 0, 0, 0, 0, 0, 0, 0};
    long long placedGroups = 0, distinctPlacements = 0, placedGroupsFree = 0, distinctFree = 0;   // groups with a dynamic object, and how many different placements (whole / without lock-moved objects) they make in their layer

    static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; }
    static uint64_t bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

    void initLock() {
        lockInit = true;
        if (!g_trigLive.on || !g_trigLive.base) return;
        const auto& S = *g_trigLive.base;
        std::set<int> targets;
        for (const auto& t : S.triggers()) if (t.lockPlayer || t.lockPX || t.lockPY) targets.insert(t.target);
        for (int g : targets) {
            const auto mi = S.members.find(g);
            if (mi == S.members.end()) continue;
            for (size_t i : mi->second) lockUids.insert(S.objs[i].uid);
        }
    }

    template <class States, class Idx>
    void group(const GroupEnv& env, int gframe, long long t, float dx, int rev, uint64_t fsig, int xc, double gx, const States& cur, const Idx& gidx) {
        if (!on) return;
        if (!lockInit) initLock();
        if (t != lastT) { flush(); lastT = t; }
        G g;
        g.dx = dx; g.frame = gframe; g.rev = rev; g.fsig = fsig; g.xc = xc; g.states = gidx.size(); g.gx = gx;
        std::unordered_set<const Obj*> seenObj;
        for (const std::vector<const Obj*>* lp : {&env.near, &env.ports, &env.pads, &env.orbs, &env.slps})
            for (const Obj* o : *lp) {
                if (!o || !o->dynObj || !seenObj.insert(o).second) continue;
                double wx = o->cx, wy = o->cy, px = o->cx - o->dcx, py = o->cy - o->dcy, wpx = px, wpy = py;
                if (gframe) { fromFrame(gframe, o->cx, o->cy, wx, wy); fromFrame(gframe, px, py, wpx, wpy); }
                g.ents.push_back({(int32_t)o->uid, wx, wy, wx - wpx, wy - wpy});
            }
        std::sort(g.ents.begin(), g.ents.end(), [](const Ent& a, const Ent& b) { return a.uid < b.uid; });
        if (g.ents.empty()) ++noDyn;
        g.pdAll = g.pdFree = 0x504c4143ULL;
        for (const Ent& e : g.ents) {
            const uint64_t h = mix(mix(mix(mix(mix(0x77ULL, (uint64_t)(uint32_t)e.uid), bits(e.x)), bits(e.y)), bits(e.dx)), bits(e.dy));
            g.pdAll = mix(g.pdAll, h);
            if (!lockUids.count(e.uid)) g.pdFree = mix(g.pdFree, h);
        }
        const int gi = (int)groups.size();
        groups.push_back(std::move(g));
        std::unordered_set<uint64_t> ws;
        for (size_t i : gidx) ws.insert(worldKeyOf(cur[i], false, true));
        for (uint64_t w : ws) byWorld[w].push_back(gi);
        ++groupsSeen;
    }

    void flush() {
        if (groups.empty()) return;
        ++layers;
        worldsSeen += (long long)byWorld.size();
        { std::unordered_set<uint64_t> da, df;
          for (const G& g : groups) if (!g.ents.empty()) { ++placedGroups; da.insert(g.pdAll); df.insert(g.pdFree); }
          distinctPlacements += (long long)da.size(); placedGroupsFree = placedGroups; distinctFree += (long long)df.size(); }
        for (auto& kv : byWorld) {
            const std::vector<int>& gs = kv.second;
            worldPairs += (long long)gs.size();
            if (gs.size() < 2) continue;
            ++shareable;
            bool allSame = true, freeSame = true;
            for (size_t k = 1; k < gs.size(); ++k) {
                if (groups[(size_t)gs[k]].pdAll != groups[(size_t)gs[0]].pdAll) allSame = false;
                if (groups[(size_t)gs[k]].pdFree != groups[(size_t)gs[0]].pdFree) freeSame = false;
            }
            for (int si = 0; si < kSubsets; ++si) {
                std::unordered_map<uint64_t, uint64_t> pd;
                bool split = false;
                for (int gi : gs) {
                    const G& g = groups[(size_t)gi];
                    const uint64_t k = subsetKey(si, g);
                    const auto it = pd.find(k);
                    if (it == pd.end()) pd[k] = g.pdAll; else if (it->second != g.pdAll) { split = true; break; }
                }
                if (split) ++splitWorlds[si];
            }
            if (allSame) { ++equalAll; continue; }
            ++conflictAll;
            if (!freeSame) ++conflictFree; else ++conflictLockOnly;
            // which objects differ (against the first group), and the first few cases in full
            const G& a = groups[(size_t)gs[0]];
            std::unordered_set<int32_t> diffUids;
            bool anyOther = false, anyOtherFree = false;
            for (size_t k = 1; k < gs.size(); ++k) {
                const G& b = groups[(size_t)gs[k]];
                size_t ia = 0, ib = 0;
                while (ia < a.ents.size() || ib < b.ents.size()) {
                    const bool onlyA = ib >= b.ents.size() || (ia < a.ents.size() && a.ents[ia].uid < b.ents[ib].uid);
                    const bool onlyB = ia >= a.ents.size() || (ib < b.ents.size() && b.ents[ib].uid < a.ents[ia].uid);
                    if (onlyA) { diffUids.insert(a.ents[ia].uid); anyOther = true; if (!lockUids.count(a.ents[ia].uid)) anyOtherFree = true; ++ia; }
                    else if (onlyB) { diffUids.insert(b.ents[ib].uid); anyOther = true; if (!lockUids.count(b.ents[ib].uid)) anyOtherFree = true; ++ib; }
                    else {
                        const Ent &x = a.ents[ia], &y = b.ents[ib];
                        if (bits(x.x) != bits(y.x) || bits(x.y) != bits(y.y) || bits(x.dx) != bits(y.dx) || bits(x.dy) != bits(y.dy)) {
                            const bool relX = bits(x.y) == bits(y.y) && bits(x.dx) == bits(y.dx) && bits(x.dy) == bits(y.dy) && std::fabs((x.x - a.gx) - (y.x - b.gx)) < 1e-6;
                            if (!relX) { anyOther = true; if (!lockUids.count(x.uid)) anyOtherFree = true; }
                            if (diffUids.insert(x.uid).second && shown < 14)
                                std::printf("placetable: t=%lld world %016llx: uid %d (%s) group[dx %g frame %d rev %d fsig %016llx xc %d, %zu states] at (%.4f, %.4f) d(%.4f, %.4f) | group[dx %g frame %d rev %d fsig %016llx xc %d, %zu states] at (%.4f, %.4f) d(%.4f, %.4f)\n",
                                            (long long)lastT, (unsigned long long)kv.first, x.uid, lockUids.count(x.uid) ? "lock" : "free",
                                            a.dx, a.frame, a.rev, (unsigned long long)a.fsig, a.xc, a.states, x.x, x.y, x.dx, x.dy,
                                            b.dx, b.frame, b.rev, (unsigned long long)b.fsig, b.xc, b.states, y.x, y.y, y.dx, y.dy), ++shown;
                        }
                        ++ia; ++ib;
                    }
                }
            }
            if (anyOther) { ++conflictOther; if (anyOtherFree) ++otherFree; } else ++conflictRelX;
            for (int32_t u : diffUids) ++uidConflict[u];
        }
        groups.clear(); byWorld.clear();
    }

    ~PlaceObs() {
        if (!on) return;
        flush();
        if (!layers) return;
        std::printf("placetable: %lld layers, %lld groups placed, %lld (world, group) pairs in %lld world-layers (%.2f worlds a layer); %lld groups had no dynamic object\n",
                    layers, groupsSeen, worldPairs, worldsSeen, (double)worldsSeen / (double)layers, noDyn);
        std::printf("placetable: %lld worlds are held by two or more groups of their layer: %lld place every dynamic object identically, %lld disagree (%lld of them also on the objects no player-lock Move acts on; %lld only on lock-moved objects)\n",
                    shareable, equalAll, conflictAll, conflictFree, conflictLockOnly);
        std::printf("placetable: of the %lld disagreeing worlds, %lld differ only by a placement that follows the group's own x (x minus the group's x equal, y and the step equal), %lld have another difference (%lld of those on objects no player-lock Move acts on)\n", conflictAll, conflictRelX, conflictOther, otherFree);
        std::printf("placetable: %lld groups with a dynamic object make %lld different placements in their layers (%.1f%%); leaving out the lock-moved objects %lld (%.1f%%): the most a placement shared by what it IS (not by a key) could save\n", placedGroups, distinctPlacements, placedGroups ? 100.0 * (double)distinctPlacements / (double)placedGroups : 0.0, distinctFree, placedGroups ? 100.0 * (double)distinctFree / (double)placedGroups : 0.0);
        for (int si = 0; si < kSubsets; ++si) std::printf("placetable: key %-26s : %lld of the %lld shareable worlds still have two groups with the same key and a different placement\n", subsetName(si), splitWorlds[si], shareable);
        if (!lockInit || lockUids.empty()) std::printf("placetable: no player-lock Move in the shadow (or no --trigshadowlive): the lock-free split equals the full one\n");
        std::vector<std::pair<long long, int32_t>> top;
        for (const auto& kv : uidConflict) top.push_back({kv.second, kv.first});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; ++i)
            std::printf("placetable:   uid %d (%s) differs in %lld worlds\n", top[i].second, lockUids.count(top[i].second) ? "lock-moved" : "free", top[i].first);
    }
};
inline PlaceObs g_placeObs;

}  // namespace dp
