#pragma once
// The load-time table of --worldtog (trigevent_togstate.hpp): the Taps (Touch trigger 1595, touch = 0 spawn = 0) with the Stop that ends each, and the x-crossing triggers that sit in a group a state-kind toggle
// (a Tap, a Toggle Orb 1594) can switch. Static: it reads triggers.txt and objgroups.txt only. A Toggle Orb gets its group a bit (so the index space is complete) but no producer yet: a gate whose groups only
// an orb switches is counted and reported, and its digest sees the group on throughout (restructure draft section 7.7a).
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "dp/trigevent_togstate.hpp"
#include "dp/triggers.hpp"

namespace dp {
namespace gdtrig {

struct TogLoadReport { int taps = 0, gates = 0, groups = 0, orbGroups = 0, gatesOnlyOrb = 0, gatedObjs = 0; bool overflow = false; };

inline TogTable buildTogTable(const std::unordered_map<int, TrigRow>& rows, const std::unordered_map<int, std::vector<int>>& mem, TogLoadReport* rep = nullptr) {
    TogTable T;
    TogLoadReport r;
    std::set<int> tapG, orbG;
    for (const auto& kv : rows) {
        const TrigRow& R = kv.second;
        if (R.id == 1595 && !R.touch && !R.spawn) tapG.insert(R.target);
        if (R.id == 1594) orbG.insert(R.target);
    }
    std::set<int> all = tapG;
    all.insert(orbG.begin(), orbG.end());
    all.erase(0);
    if (all.size() > 128) { r.overflow = true; if (rep) *rep = r; return T; }
    std::map<int, int> idxOf;
    for (int g : all) { idxOf[g] = (int)T.groups.size(); T.groups.push_back(g); T.initialOn.push_back(1); }
    for (const auto& kv : rows) {
        const TrigRow& R = kv.second;
        if (R.id != 1594 || !idxOf.count(R.target)) continue;
        TogOrb o; o.uid = R.uid; o.idx = idxOf[R.target]; o.target = R.target; o.on = R.actgrp == 1; o.r.cx = R.cx; o.r.cy = R.cy; o.r.w = R.w; o.r.h = R.h;
        T.orbs.push_back(o);
    }
    std::sort(T.orbs.begin(), T.orbs.end(), [](const TogOrb& a, const TogOrb& b) { return a.uid < b.uid; });
    // the Taps, each with the first Stop (1616) whose target group holds the Tap's own object
    for (const auto& kv : rows) {
        const TrigRow& R = kv.second;
        if (!(R.id == 1595 && !R.touch && !R.spawn) || !idxOf.count(R.target)) continue;
        TogTap t; t.uid = R.uid; t.idx = idxOf[R.target]; t.target = R.target; t.thold = R.thold; t.ttog = R.ttog; t.cx = R.cx;
        for (const auto& sk : rows) {
            const TrigRow& S = sk.second;
            if (S.id != 1616) continue;
            const auto g = mem.find(S.target);
            if (g == mem.end() || std::find(g->second.begin(), g->second.end(), R.uid) == g->second.end()) continue;
            t.stopCx = std::min(t.stopCx, S.cx);
        }
        T.taps.push_back(t);
    }
    std::sort(T.taps.begin(), T.taps.end(), [](const TogTap& a, const TogTap& b) { return a.uid < b.uid; });
    // the x-crossing triggers in a toggleable group
    std::unordered_map<int, std::vector<int>> idxByUid;
    for (const auto& gi : idxOf) {
        const auto g = mem.find(gi.first);
        if (g == mem.end()) continue;
        for (int u : g->second) idxByUid[u].push_back(gi.second);
    }
    for (const auto& kv : rows) {
        const TrigRow& R = kv.second;
        if (R.touch || R.spawn || R.id == 1594 || R.id == 1595 || (R.id == 1049 && R.touch)) continue;
        const auto it = idxByUid.find(R.uid);
        if (it == idxByUid.end()) continue;
        TogGate g; g.uid = R.uid; g.cx = R.cx; g.idx = it->second;
        std::sort(g.idx.begin(), g.idx.end());
        g.idx.erase(std::unique(g.idx.begin(), g.idx.end()), g.idx.end());
        bool onlyOrb = true;
        for (int i : g.idx) onlyOrb = onlyOrb && !tapG.count(T.groups[(size_t)i]);
        if (onlyOrb) ++r.gatesOnlyOrb;
        T.gates.push_back(g);
    }
    std::sort(T.gates.begin(), T.gates.end(), [](const TogGate& a, const TogGate& b) { return a.cx != b.cx ? a.cx < b.cx : a.uid < b.uid; });
    // The level's own objects (not triggers) in a group that only a Tap switches. GD's toggleGroup (0x32b860) counts a disabled group against each of its objects, which is what the
    // pass skips (+0x28e); the shadow's world takes it from State::tog (shadow_step.hpp shadowApplyTapGates). A group some other trigger also switches (a Toggle 1049, a Count 1611 / 1811, a
    // Collision 1815: the world's own timeline for it) or a Toggle Orb switches (its producer is the push path only) is not read here -- the bit would not be the group's state.
    {
        std::set<int> trigUid, otherG;
        for (const auto& kv : rows) {
            const TrigRow& R = kv.second;
            trigUid.insert(R.uid);
            if (R.id == 1049 || R.id == 1611 || R.id == 1811 || R.id == 1815) otherG.insert(R.target);
        }
        std::map<int, std::vector<int>> objBits;
        for (const auto& gi : idxOf) {
            if (!tapG.count(gi.first) || orbG.count(gi.first) || otherG.count(gi.first)) continue;
            const auto g = mem.find(gi.first);
            if (g == mem.end()) continue;
            for (int u : g->second) if (!trigUid.count(u)) objBits[u].push_back(gi.second);
        }
        for (auto& kv : objBits) {
            TogObj o; o.uid = kv.first; o.idx = kv.second;
            std::sort(o.idx.begin(), o.idx.end());
            o.idx.erase(std::unique(o.idx.begin(), o.idx.end()), o.idx.end());
            T.objs.push_back(std::move(o));
        }
        {   // every object in a group a Tap targets, also where a Toggle / Count / Collision switches the group too (T.objs leaves those groups out)
            std::set<int> tm;
            for (int g : tapG) { const auto gm = mem.find(g); if (gm == mem.end()) continue; for (int u : gm->second) if (!trigUid.count(u)) tm.insert(u); }
            T.tapMembers.assign(tm.begin(), tm.end());
        }
        r.gatedObjs = (int)T.objs.size();
    }
    r.taps = (int)T.taps.size(); r.gates = (int)T.gates.size(); r.groups = (int)T.groups.size(); r.orbGroups = (int)orbG.size();
    if (rep) *rep = r;
    return T;
}

}  // namespace gdtrig
}  // namespace dp
