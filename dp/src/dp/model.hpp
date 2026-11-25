#pragma once
#include "dp/fixup.hpp"
#include "dp/shadow_step.hpp"

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace dp {

// THE MODEL'S SIDE OF A SEARCH. A search decides which states to keep and which inputs to try;
// the model answers what one tick does to a state. These are the model's half of that line:
// the world a call plans in (ModelWorld), what one group of states shares for one tick
// (GroupEnv, built by prepareGroup), and one state's tick (advanceState). The search's layer
// loop (cli.hpp) calls them; it reads the physics through nothing else.
//
// A group is a set of states that share the tick's geometry: the same x advance, the same
// touch boxes fired, the same gameplay frame and direction. They are stepped against one set
// of windows, so the windows are built once per group.

// One gameplay frame's x-sorted views of the level (see RotTrig). Frame 0 is the level as loaded;
// the others are built on first use. The cursors are monotone in that frame's travel coordinate, so
// a group whose window starts BEHIND where the cursor already is has to rewind -- states in one
// frame are no longer guaranteed to advance together once a turn splits them.
struct FrameSlices {
    Level* lv = nullptr;
    std::unique_ptr<XSlice> near, port, pad, orb, slope, speed;
    double lastLo = -1e18;
};

// (Re)build one frame's slices over that frame's level, cursors at the start.
inline void bindFrameSlices(FrameSlices& fs, Level& lv) {
    fs.lv = &lv;
    fs.near = std::make_unique<XSlice>(lv.objs);
    fs.port = std::make_unique<XSlice>(lv.portals);
    fs.pad = std::make_unique<XSlice>(lv.pads);
    fs.orb = std::make_unique<XSlice>(lv.orbs);
    fs.slope = std::make_unique<XSlice>(lv.slopes);
    fs.speed = std::make_unique<XSlice>(lv.speeds);
}

// The world one search call plans in: the level, the vehicle parameters, the coin tables the
// step reads, and the per-frame slices and ramp-veto storage the groups reuse.
struct ModelWorld {
    Level& L;
    const gdapprox::ShipParams* SP;
    const gdapprox::ShipParams* SPmini;
    const gdapprox::UfoParams* UP;
    const gdapprox::UfoParams* UPmini;
    bool coinOn;
    const std::vector<TouchMask>* coinEnable;
    const std::vector<uint8_t>* coinAnyFrame;
    std::array<FrameSlices, 4> fsl;
    SlopeVetoIndex vetoIdx;       // rebuilt per group, storage reused
    // The level's item gates -- the Count, Tap and Item Compare triggers and the pickups that feed
    // them (cliMainOnce's itemGates) -- behind a plain function pointer, since their tables are
    // the call's. Null while they are not modelled (today: without --coins). Model::fireGates.
    using GatesFn = void (*)(void* ctx, State& s, uint8_t prevAction, int input, const StepCtx& K,
                             int gframe, double px, double py, double ph, long long t);
    GatesFn gates = nullptr;
    void* gatesCtx = nullptr;
    // The counting tap's bit and the boxes of the gate it feeds (Model::pressMatters).
    TouchMask tapBit{}, tapGateBits{};
    // --tap2: how many counting taps follow the first, and the boxes of the gate on each one's item (State::tapsX)
    int nTapX = 0;
    TouchMask tapXGate[3]{};

    FrameSlices& slicesFor(int f, double wLo) {
        FrameSlices& fs = fsl[(size_t)(f & 3)];
        if (!fs.lv) {
            bindFrameSlices(fs, frameLevel(L, f));
        } else if (wLo + 1e-6 < fs.lastLo) {
            // a group behind the cursor: rewind by binary search, do not
            // rebuild (see XSlice::seekTo)
            fs.near->seekTo(wLo - 40);
            fs.port->seekTo(wLo - 60);
            fs.pad->seekTo(wLo - 40);
            fs.orb->seekTo(wLo - 50);
            fs.slope->seekTo(wLo - 60);
            fs.speed->seekTo(wLo - 80);
        }
        fs.lastLo = wLo;
        return fs;
    }
};

// Where a state has got to: its x along the direction of travel of its frame (a world x only in
// frame 0 -- in a turned frame it is another axis), the frame, and the direction (id 2899 reverse).
// What a search reads to know how far it is; the goal (Rules::reached) is judged on the same.
struct Progress {
    double x;
    int frame;
    int rev;
};
inline Progress progressOf(const State& s) { return Progress{(double)s.xAbs, (int)s.frame, (int)s.rev}; }

// Which states can share one tick of the world (prepareGroup): the same speed (`fallbackDx` for a
// state that carries none), the same doors opened, the same frame and direction, and under
// --groupfire the same fire ticks (`fsig`, search_key.hpp groupFireSig; 0 without the flag).
// TouchMask, not uint32_t: this key GROUPS states, so a narrower field would merge two states whose
// only difference is a box above bit 31 -- silently, and only once kTouchBits passes 32.
struct GroupKey {
    float dx;
    TouchMask trig;
    uint8_t frame;
    uint8_t rev;
    uint64_t fsig;
    bool operator==(const GroupKey& o) const {
        return dx == o.dx && trig == o.trig && frame == o.frame && rev == o.rev && fsig == o.fsig;
    }
};
inline GroupKey groupKeyOf(const State& s, float fallbackDx, uint64_t fsig) {
    return GroupKey{(s.dx > 0.f) ? s.dx : fallbackDx, s.trig, s.frame, s.rev, fsig};
}
// Is `s` in the group `k`, as far as its own fields say (the fire signature aside: the caller holds
// it per state)? groupKeyOf's fields, compared cheapest-first and stopping at the first that
// differs, without building a key. A search asks this for every state of every group on every
// layer -- building the whole key there cost lv22's search 3% (measured on 8eb7b5dc).
inline bool inGroup(const GroupKey& k, const State& s, float fallbackDx) {
    return ((s.dx > 0.f) ? s.dx : fallbackDx) == k.dx && s.trig == k.trig && s.frame == k.frame
           && s.rev == k.rev;
}

// --p2xoff: a second dual body that runs dx2 away from the first (State::dx2) meets the blocks,
// orbs, pads and ramps around ITS x, so the windows of its tick are stretched over that body too.
// p2Dx is 0 without the flag and for every pair that never separated, so those windows are as
// before.
inline float p2Dx(const State& s) { return (s.dual && s.frame == 0) ? s.dx2 : 0.f; }
inline void p2Widen(double& wLo, double& wHi, float dLo, float dHi) {
    if (dLo < 0.f) wLo += (double)dLo;
    if (dHi > 0.f) wHi += (double)dHi;
}

// What a caller of prepareGroup wants beyond the windows. The search wants all three; a walk of one
// plan (the replay) builds no coin list and no veto index -- its step then walks the slopes whole,
// as it always has -- and prints no --trigdbg line (the replay prints its own `trigcand:`).
struct GroupOpts {
    bool coins = true;     // --coins: the coins where they are now (only while ModelWorld::coinOn)
    bool veto = true;      // the group's SlopeVetoIndex over near and the slopes
    bool trigDbg = true;   // --trigdbg's per-group line
};

// What one group shares for one tick: its windows over the level, the touch boxes it can reach,
// the coins where they are now, and the numbers StepCtx carries. Built by prepareGroup; StepCtx
// points into it, so it must outlive every step of the group and not be moved.
struct GroupEnv {
    double x = 0.0, xPrev = 0.0;
    float dxF = 0.f;
    long long t = 0;
    std::vector<const Obj*> near, ports, pads, orbs, slps, spds;
    std::vector<std::pair<const TouchTrig*, TouchMask>> trigs;
    std::vector<CoinLive> coinLive;   // --coins, filled below the windows
    std::vector<const Obj*> nearSolid;   // --solidorder (removed): near in GD's solid order, once per group
    std::vector<FollowRef> fol;          // --followy: the follow groups' objects in the windows
    const ModelWorld* w = nullptr;
    GroupOpts opts;
    uint64_t sgen = 0;                   // --shadowstep: this prepareGroup call's number (StepCtx::sgen)

    StepCtx ctx() const {
        StepCtx c{x, xPrev, dxF, t, &near, &ports, &pads, &orbs, &slps, &spds,
                  w->SP, w->SPmini, w->UP, w->UPmini, &trigs,
                  opts.coins ? &coinLive : nullptr, opts.veto ? &w->vetoIdx : nullptr,
                  &nearSolid};
        c.fol = fol.empty() ? nullptr : &fol;
        c.sgen = sgen;
        return c;
    }
};

// Build one group's tick: place the moving geometry for tick t, open the doors the group has
// opened, and gather the windows. `FS`: the slices of the group's frame (the search's come from
// ModelWorld::slicesFor; a walk of one plan keeps its own). `gframe` / `gtrig`: the group's frame
// and fired boxes; `gFire` / `gFireB`: its latest firing tick, over all boxes and per box;
// `lockAt`: where a locked platform is this tick -- the least a member has carried it, plus this
// tick's advance; `x`, `xPrev`, `dxUsed`, `wLo`, `wHi`: its x at t, the x it came from, its signed
// advance and the window that covers it for the whole tick.
inline void prepareGroup(ModelWorld& w, FrameSlices& FS, GroupEnv& e, int gframe,
                         const TouchMask& gtrig, int gFire, const uint16_t* gFireB, float lockAt,
                         double x, double xPrev, float dxUsed, double wLo, double wHi,
                         long long t, const GroupOpts& opts = GroupOpts{}) {
    Level& L = w.L;
    if (g_shadowStepModes) e.sgen = g_shadowGroupGen.fetch_add(1, std::memory_order_relaxed) + 1;   // --shadowstep: the shadow re-reads the movers (shadow_step.hpp)
    e.w = &w;
    e.opts = opts;
    e.x = x;
    e.xPrev = xPrev;
    e.dxF = dxUsed;
    e.t = t;
    // Moving geometry is placed for THIS tick before any window is built.
    // seek() mutates the rects in place, so the pointers the search already
    // holds (snapObj / usedOrb / usedPad) keep pointing at the same object.
    Level& LG = *FS.lv;
    // The group's own previous place for the movers its boxes move (Dynamics::groupPrevPass): the Dynamics holds the last placement of whichever group this thread stepped before, and a
    // group of other boxes (touch mask, fire ticks, lock) is another world. A group with the key of the last one keeps the carry, which is its own.
    bool gpDone = false;
    uint64_t gpKey = 0;
    {
        auto mix = [](uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; };
        uint32_t lb; std::memcpy(&lb, &lockAt, 4);
        gpKey = mix(mix(mix(0x5151ULL, gtrig.word(0)), (uint64_t)(uint32_t)gFire), (uint64_t)lb);
        if (gFireB) for (int b = 0; b < kTouchBits; ++b) if (gtrig.test(b)) gpKey = mix(gpKey, (uint64_t)gFireB[b]);
        Dynamics& D = LG.dyn;
        const int t1 = (int)t - 1;
        const bool sameKey = D.lastGroupKeyValid && D.lastGroupKey == gpKey;
        const bool gpWill = (D.lastT == t1 || D.lastT == (int)t) && !sameKey;
        D.sqHead((int)t, gpWill, gtrig, gFireB);   // the quiet objects: which of them this group's walks (the previous place, seek, applyTriggers, groupPrevApply) pass by (dynamics.hpp)
        if (gpWill)
            gpDone = D.groupPrevPass(gtrig, gFire, gFireB, lockAt - (float)(x - xPrev), t1, xPrev);
        // dupbatch (dynamics.hpp FireReadSet): without that pass the previous place of this group's movers is what the Dynamics carries from the group before it, made from ticks this build
        // does not read -- the set then says every tick was read.
        if (D.fireSink && !gpDone) D.fireSink->all.store(true, std::memory_order_relaxed);
    }
    LG.dyn.seek((int)t, true);   // (applyTriggers follows below: see Dynamics::seek)
    seekOtherFrames(&LG, (int)t);   // --rotpretap only; serial here, before the workers
    ppSubMark(4);   // --phaseprof's sub-buckets (progress.hpp)
    // ...and then the doors this group has (or has not) opened. Must come
    // after seek and before any window is built. `gFire` is the LATEST
    // firing tick in the group, so a state that touched the box early is
    // shown a door that is still opening -- conservative, never the other
    // way round.
    // ...plus THIS tick's advance (in `lockAt`). A lock is a per-tick copy of
    // where the player is NOW, so the geometry placed for tick t needs the
    // player's x at t -- while the members' offsets come from states that have
    // only stepped to t-1. Measured without it: lv19's platform sits exactly
    // one dx (1.614 px) behind its recording, at every tick.
    LG.dyn.areaPxPrev = xPrev;   // --areamove: GD's area pass reads P1 before the step
    LG.dyn.areaTickPrev = (int)t - 1;
    LG.dyn.applyTriggers(gtrig, gFire, gFireB, lockAt, (int)t, x);
    if (gpDone) LG.dyn.groupPrevApply();
    LG.dyn.sqAfterA();
    LG.dyn.lastGroupKey = gpKey; LG.dyn.lastGroupKeyValid = true;
    ppSubMark(5);
    { static const bool census = std::getenv("PP_CENSUS") != nullptr; if (g_phaseProf && census) LG.dyn.censusAlways((int)t, wLo, wHi); }   // (the census is a measurement of its own, and reads every placed-every-tick object: PP_CENSUS=1)
    // --trigsupply: the shadow's movers inside this group's window (widest bucket window + 40: a mover is kept by collect() against its own bucket's window); frame 0 only, as supplyPlaceMovers is
    thread_local SupHits supHits;
    supHits.clear();
    const SupHits* supP = nullptr;
    if (gframe == 0 && g_supplyHitsHook) { SqTimer tmHits(g_sqStats.nsHits, g_phaseProf); g_supplyHitsHook(wLo - 120, wHi + 120, supHits); if (!supHits.empty()) supP = &supHits; }
    std::vector<const Obj*>& near = e.near;
    FS.near->forRange(wLo - 40, wHi + 40, [&](const Obj& o) { near.push_back(&o); });
    LG.dyn.collect(Dynamics::NEAR, wLo - 40, wHi + 40, near, supP);
    ppSubMark(6);
    // --trigdbg <tick>: the mask this GROUP applied, and where it left the
    // object named by --hazdbg. The resim prints the same pair from the
    // state's own mask (`resimwho:`), so the two lines can be put side by
    // side -- which is the only way to say "at this tick the two masks
    // differ" rather than "they could differ".
    if (opts.trigDbg && g_trigDbgT >= 0 && (long long)t == g_trigDbgT) {
        const Obj* seen = nullptr;
        if (g_hazDbgUid >= 0)
            for (const Obj* o : near)
                if (o->uid == g_hazDbgUid) { seen = o; break; }
        // gFireB is an ARRAY: this line used to print `(unsigned)gFireB`, the low
        // bits of its address. It prints the group's per-box ticks now (the
        // latest over the members, which is what applyTriggers is handed).
        std::printf("trigdbg t=%lld gtrig=0x%08x gFire=%d lockOff=%.3f "
                    "win=[%.1f,%.1f] n=%zu uid=%d %s gFireB=",
                    (long long)t, (unsigned)gtrig.word(0), gFire,
                    (double)lockAt, wLo, wHi, near.size(),
                    g_hazDbgUid,
                    seen ? "" : "NOT-IN-WINDOW");
        for (int b = 0; b < kTouchBits; ++b)
            if (gtrig.test(b)) std::printf("%d:%u,", b, (unsigned)gFireB[b]);
        std::printf("\n");
        if (seen)
            std::printf("trigdbg t=%lld uid=%d at (%.3f,%.3f) hw=%.3f hh=%.3f\n",
                        (long long)t, seen->uid, seen->cx, seen->cy,
                        seen->hw, seen->hh);
    }
    FS.port->forRange(wLo - 60, wHi + 60, [&](const Obj& o) { e.ports.push_back(&o); });
    LG.dyn.collect(Dynamics::PORT, wLo - 60, wHi + 60, e.ports, supP);
    // --portorder (constants.hpp): the collision pass's order, cell by cell and by uid in a cell
    std::stable_sort(e.ports.begin(), e.ports.end(), [](const Obj* a, const Obj* b) {
        const int ax = gdSectionCell(a->cx), bx = gdSectionCell(b->cx);
        if (ax != bx) return ax < bx;
        const int ay = g_dynamicLevelHeight ? gdSectionCell(a->cy) : 0;
        const int by = g_dynamicLevelHeight ? gdSectionCell(b->cy) : 0;
        if (ay != by) return ay < by;
        return a->uid < b->uid;
    });
    FS.pad->forRange(wLo - 40, wHi + 40, [&](const Obj& o) { e.pads.push_back(&o); });
    LG.dyn.collect(Dynamics::PAD, wLo - 40, wHi + 40, e.pads, supP);
    FS.orb->forRange(wLo - 50, wHi + 50, [&](const Obj& o) { e.orbs.push_back(&o); });
    LG.dyn.collect(Dynamics::ORB, wLo - 50, wHi + 50, e.orbs, supP);
    FS.slope->forRange(wLo - 60, wHi + 60, [&](const Obj& o) { e.slps.push_back(&o); });
    LG.dyn.collect(Dynamics::SLOPE, wLo - 60, wHi + 60, e.slps, supP);
    FS.speed->forRange(wLo - 80, wHi + 80, [&](const Obj& o) { e.spds.push_back(&o); });
    LG.dyn.collect(Dynamics::SPEED, wLo - 80, wHi + 80, e.spds);
    ppSubMark(7);
    // touch-trigger boxes this group could reach this tick, with their bits
    // ...in THIS group's frame (see touchFor): a world-coordinate box is
    // unreachable once the frame turns, which silently shut every door in
    // lv22's rotated sections.
    const std::vector<TouchTrig>& tfg = touchFor(gframe);
    for (size_t b = 0; b < tfg.size(); ++b) {
        const TouchTrig& T = tfg[b];
        if (gtrig & touchBit(b)) continue;   // already fired
        // A STANDING COUNT DOES NOT STOP LISTENING WHEN IT SCROLLS OFF.
        // Count (1611) is the standing form -- the file already says so at
        // the test that makes 1811 ask once -- but the window dropped it
        // the moment the player was 40 px past, so it was only ever asked
        // while it was nearby. On lv22's first coin that is fatal to the
        // whole mechanism: the gate `item 2 == 5` sits at x=2,835 and every
        // box that feeds item 2 is at 3,195..3,674, so the player crosses
        // the gate with a count of 0, and by the time the count rises the
        // gate is no longer in this list. The coin could never be raised,
        // which is why every plan collects it at its load position.
        //
        // Only the BEHIND half of the window goes. A gate the player has
        // not reached yet is still skipped, so this only ever adds
        // occasions to ask a question whose answer was already false.
        // An Item Compare root (3620) is asked from anywhere for the same
        // reason: what fires it is a press, not the player's x.
        // --tapdisarm: ...and a Tap. GD appends its touch action when the player crosses it (triggerObject,
        // id 1595) and playerButton walks every action on each press, so a press anywhere past it fires it;
        // dropped here 40 px past, a later press did nothing in the model. A custom level: a press at t=19,410,
        // 76 px past the Tap at x 37,200, kept the rig's spikes out in GD (its recording: the Toggle that
        // brings them in never fired), and the model's Tap was no longer in this list.
        const bool standing =
            ((T.id == 1611 || T.id == 3620) && T.count >= 0 && !T.tap) || (g_tapDisarm && T.tap);
        // ...and an Item Compare is not ahead either: nothing about it is a
        // place, and in a turned frame its x is not even on this axis.
        const bool anywhere = T.id == 3620 && T.count >= 0;
        if (!anywhere && !standing && T.cx + T.hw < wLo - 40) continue;
        if (!anywhere && T.cx - T.hw > wHi + 40) continue;
        e.trigs.push_back({&T, touchBit(b)});
    }
    // --coins: this group's coins where THEY are now. A controlled coin is
    // read out of the dyn rows the applyTriggers above has just placed (and
    // is absent while its group is switched off, which is what GD does with
    // it); an uncontrolled one is its load-time row. Frame 0 only, like the
    // collect in the step: in a turned frame xAbs is a different axis.
    // ...and in a TURNED frame, only the coins that never move
    // (coinAnyFrame), turned into this frame the way the geometry is.
    std::vector<CoinLive>& coinLive = e.coinLive;
    const std::vector<TouchMask>& coinEnable = *w.coinEnable;
    const std::vector<uint8_t>& coinAnyFrame = *w.coinAnyFrame;
    const bool coinOn = opts.coins && w.coinOn;
    if (coinOn && gframe != 0)
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            if (!coinAnyFrame[ci] || !(gtrig & coinEnable[ci])) continue;
            const Obj& c = L.coins[ci];
            double u = 0.0, v = 0.0;
            toFrame(gframe, c.cx, c.cy, u, v);
            const bool odd = (gframe & 1) != 0;
            coinLive.push_back({u, v, odd ? c.hh : c.hw, odd ? c.hw : c.hh, (int)ci});
        }
    if (coinOn && gframe == 0) {
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            const int di = ci < L.coinDyn.size() ? L.coinDyn[ci] : -1;
            // A coin an Item Compare switches on is live exactly when this
            // group's gate has fired (see coinEnable), wherever it sits.
            if (coinEnable[ci]) {
                if (gtrig & coinEnable[ci]) {
                    const Obj& c = di < 0 ? L.coins[ci] : LG.dyn.objs[(size_t)di];
                    coinLive.push_back({c.cx, c.cy, c.hw, c.hh, (int)ci});
                }
                continue;
            }
            if (di < 0) {
                const Obj& c = L.coins[ci];
                coinLive.push_back({c.cx, c.cy, c.hw, c.hh, (int)ci});
            } else if (LG.dyn.on[(size_t)di]) {
                const Obj& c = LG.dyn.objs[(size_t)di];
                coinLive.push_back({c.cx, c.cy, c.hw, c.hh, (int)ci});
            }
        }
        // --coindbg <tick>: where THIS group put each coin, and which ones
        // its mask left switched off. The mod prints the same thing from
        // GD (`coinlive:`), so the two can be put side by side -- which is
        // the only way to say "at this tick the model and GD disagree about
        // where the coin is" rather than "they could".
        if (g_coinDbgT >= 0 && (long long)t == g_coinDbgT) {
            for (const CoinLive& C : coinLive)
                std::printf("coindbg: t=%lld gtrig=0x%08llx coin=%d live=(%.3f,%.3f)"
                            " box %.1fx%.1f load=(%.3f,%.3f)\n",
                            (long long)t, (unsigned long long)gtrig.word(0),
                            C.bit, C.cx, C.cy,
                            C.hw * 2, C.hh * 2,
                            L.coins[(size_t)C.bit].cx, L.coins[(size_t)C.bit].cy);
            for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                bool live = false;
                for (const CoinLive& C : coinLive)
                    if ((size_t)C.bit == ci) live = true;
                if (!live)
                    std::printf("coindbg: t=%lld gtrig=0x%08llx coin=%zu OFF"
                                " (its group is switched off)\n",
                                (long long)t, (unsigned long long)gtrig.word(0), ci);
            }
        }
    }
    // --followy: the crossing tick of each Follow Player Y (every state of the group shares this
    // tick's x), and where its group's objects sit in the windows -- the step puts each one at
    // its own state's height (followPassPlan, step_shared.hpp, which the shadow's tick runs).
    e.fol.clear();
    const bool folOn = !g_followYs.empty() || !g_followSecObjs.empty();
    if (folOn || !g_bigSecObjs.empty() || !g_bigSecDyn.empty()) {
        if (folOn)
            for (FollowY& f : g_followYs)
                if (!f.touch && xPrev < f.cx && f.cx <= x) f.tAct = t;
        const std::vector<const Obj*>* wins[6] = {&near, &e.ports, &e.pads, &e.orbs, &e.slps,
                                                  &e.spds};
        for (uint8_t wi = 0; wi < 6; ++wi)
            for (uint32_t i = 0; i < (uint32_t)wins[wi]->size(); ++i) {
                const int u = (*wins[wi])[i]->uid;
                if (folOn) {
                    for (uint16_t c = 0; c < (uint16_t)g_followYs.size(); ++c)
                        for (const auto& m : g_followYs[c].objs)
                            if (m.first == u) e.fol.push_back({wi, i, c, m.second});
                    for (const auto& m : g_followSecObjs)   // --followsection: placed by nobody
                        if (m.first == u) e.fol.push_back({wi, i, kFollowSecCmd, m.second});
                }
                for (const auto& m : g_bigSecObjs)   // --bigsection: placed by nobody either
                    if (m.first == u) e.fol.push_back({wi, i, kBigSecCmd, m.second});
                for (const auto& m : g_bigSecDyn)    // ...and the grouped ones
                    if (m.first == u) e.fol.push_back({wi, i, kBigSecDynCmd, m.second});
            }
    }
    ppSubMark(8);
    // Which ramps can veto which solid (SlopeVetoIndex): the same for every
    // state of the group, so worked out here once.
    if (opts.veto) w.vetoIdx.build(near, e.slps);
    solidOrderInto(near, e.nearSolid);
    ppSubMark(9);
}

// What --rotwatch keeps from one child's turn (print only).
// GAMEPLAY ROTATION, one shot (see RotTrig::firedT). Spend a trigger the moment a group's
// advance can reach it: `gLo`..`gHi` is where the group's states are at t-1, `wLo`..`wHi` the
// window that covers its advance to t, `grev` its direction. Called once per group, single-
// threaded, before its states step -- applyRotation runs on the worker threads and must stay a
// pure read. All states in a group share dx and start from one x, so "some state crosses this
// tick" is "every state crosses this tick". Those pointing at the same frame (= reverse
// toggles) are consumed here too. Excluded, applyRotation reads firedT and can never fire.
// This is the model state the search writes; a walk of one plan (the replay) spends its own.
inline void fireRotTriggers(int gframe, bool grev, double gLo, double gHi, double wLo,
                            double wHi, long long t) {
    for (RotTrig& r : g_rotTrig) {
        const bool same = (r.frame == gframe);
        if ((same ? r.revT : r.firedT) >= 0) continue;
        const double ru = frameU(gframe, r.cx, r.cy);
        if (grev ? (wLo <= ru && ru < gHi) : (gLo < ru && wHi >= ru)) {
            if (same) r.revT = (int)t; else r.firedT = (int)t;
        }
    }
}

struct RotNote {
    bool set = false;
    int uid = -1;
    int8_t f0 = -1, nf = -1;
};

// One state's tick: the physics, then the gameplay rotation if this tick crossed one. Must turn
// BEFORE the caller keys the child -- the turn rewrites x, y, vy and the frame, and a key taken in
// the old frame would put two different worlds in one cell. `rot` (may be null) gets what
// --rotwatch prints, and only when the watch covers this tick and the child turned.
// A ball turned by the rotation queue. GD's Rotate Gameplay (22091 0x4f7fa0) calls flipGravity (0x4f8440: its ball arm zeroes +0x730 / +0x670 / +0x728 and calls runBallRotation2) and then
// setLeftward (FUN_1404f6830 = setLeftward in collide.hpp): for a ball that is not dashing it zeroes the three words again and calls runBallRotation(1), so every turn leaves the GROUND stake,
// signed by the new up / leftward / sideways bytes, and updateRotation spends it (+0x728 / 240 a tick, added to the angle) on the tick the turn happens in. The shadow's pass does this for a body it
// turned itself (playerRotateGameplay); a turn the queue makes after the pass kept the old stake and the old tick's angle (lv22 t=6307: GD 285.85 -> 282.74 and -3.108 a tick, the model 288.05 and +2.202).
// `rotBefore` is the angle before the tick.
inline void ballRestakeAfterTurn(State& c, double rotBefore) {
    if (c.mode != 2 || c.dual || c.dashing) return;
    const bool mi = c.mini != 0;
    const double dxr = c.dx > 0.f ? (double)c.dx : 1.29825;
    const double rate = ballRotRate(mi, false, dxr);   // runBallRotation(1): +-0x78 / (size * 0.2 * speed factor) / 240
    const double sgn = (gdUpOf(c) ? -1.0 : 1.0) * (shGdLeft((int)c.frame, c.rev) ? -1.0 : 1.0) * (c.frame != 0 ? -1.0 : 1.0);
    c.rotStep = (float)rate;
    c.rot = (float)(rotBefore + sgn * rate);
}
inline State advanceState(const State& s, int input, const StepCtx& K, bool& dead, RotNote* rot) {
    // The shadow step (shadow_step.hpp) is the tick: there is no old model to run first or to fall back to.
    State c = shadowAdvance(s, input, K, dead);
    if (!dead && !g_rotTrig.empty()) {
        const bool rw = rot && g_rotWatchLo >= 0 && K.t >= g_rotWatchLo
                        && K.t <= g_rotWatchHi;
        if (rw) g_rotWatchUid = -1;
        const int f0R = (int)c.frame;
        const uint8_t up0R = gdUpOf(c);   // (GD's upside-down byte before the turn: Rotate Gameplay calls flipGravity, which stamps the flip time only when that byte really changes)
        // --tpjumpnext (removed): the crossing starts where last tick's forward jump left (step.hpp crossBaseX)
        const int nfR = applyRotation(c, crossBaseX(s), (double)K.dxF, K.t,
                                      input, s.grounded != 0, (double)s.y, true, s.action == 0 || s.pSpiderTap != 0);
        // The turn is a gravity flip when it changes GD's up byte (frame 3 is the mirror of frame 0 for the same State::flip): the flip clock restarts, which the end of the tick (step.hpp, c.flipT) cannot see
        // because the turn is applied after it. A shadow seed in the turned frame reads it as the stamp GD's flipGravity wrote (shadow_step.hpp, +0x808): the grace of a body inside a solid.
        if (nfR >= 0 && gdUpOf(c) != up0R) c.flipT = 0;
        if (nfR >= 0) ballRestakeAfterTurn(c, (double)s.rot);
        if (rw && nfR >= 0) {   // --rotwatch: print only, read in the serial pass
            rot->set = true;
            rot->uid = g_rotWatchUid;
            rot->f0 = (int8_t)f0R;
            rot->nf = (int8_t)nfR;
        }
    }
    shadowRotFinish(c, s.rotSpent);   // the shadow's body over the old tick's turn, when both turned with the same trigger (shadow_step.hpp)
    return c;
}


// ---- the model walks a plan ----------------------------------------------------------------------
// From `start` at tick t0, tick t0+1+i takes input lvl[i]. Each tick is built the way the search
// builds a group's (prepareGroup), as a group of one: the state's own x for its window, its own
// speed (`fallbackDx` for a state that carries none), the doors THIS lineage opened, the boxes in
// its own frame's coordinates, and its locked platform carried by this tick's advance. No coin
// list, no veto index, no --trigdbg line. Then the tick is stepped and turned (advanceState); a
// turn re-binds the new frame's slices. The walk runs on through a death -- what a death means is
// the caller's. `pre(i, t, prev, c, dead, K)` sees each tick before it is committed and may change
// `c`; `post(i, t, s, dead)` sees it after (`s` is the committed state, its action the input).
// Returns the last state. (The witness resim is a caller; a search that judges whole plans is
// another.)
// `bindFrame` (>= 0): the frame the slices start bound to, when it is not the start's own -- a walk
// started mid-way keeps the binding the walk it continues had.
// The world's tog update: one tick of State::tog / xgate for the step from `s` to `c` with `input` -- the Taps' button edges, the Toggle Orbs the press hits (the push path: the player's box at the parent row against the
// orb's rect; State::mode 6 is the spider, `mini` 18 the small size) and the x-gate crossings (trigevent_togstate.hpp). The parent's x is the farthest x when --tpback has moved the player back (crossXOf).
inline void togWorldStep(const State& s, State& c, int input, std::vector<gdtrig::TogChange>* out = nullptr) {
    static thread_local std::vector<int> hits;
    gdtrig::OrbPlayer pl;
    pl.x = (double)s.xAbs; pl.y = (double)s.y; pl.vsize = s.mini ? 0.6 : 1.0; pl.spider = s.mode == 6;   // (State::mini is 0 / 1; this read `== 18`, the size portal's type, and a mini player had the full-size box)
    if (t_bodyFiredOut.valid) {   // the shadow stepped this tick: the Toggle Orbs its body fired (the game's own press latch, box and row), ascending uid as the push path gives them
        hits.clear();
        for (int u : t_bodyFiredOut.uids) if (gdtrig::g_togTable.findOrb(u)) hits.push_back(u);
        std::sort(hits.begin(), hits.end());
        t_bodyFiredOut.valid = false;
    } else
        gdtrig::togOrbPushHits(gdtrig::g_togTable, pl, input && !s.action, hits);
    gdtrig::togStep(gdtrig::g_togTable, c.tog, c.xgate, crossXOf((double)s.xAbs, s.tpBack), crossXOf((double)c.xAbs, c.tpBack), (int)s.action, input, out, &hits);
}

// --trigsupply-group in a walk (search_dp.hpp installs both when it is included; each is a no-op unless a supplied group is on): `group` tells the shadow this tick's one-state group (the call the search makes per
// group before prepare), `place` puts the supplied groups' movers of the tick's env where the shadow has them (the block the search runs after prepare). Null / no-op: the walk is what it was.
inline void (*g_rolloutGroupHook)(long long t, float dx, const State& s, double x) = nullptr;
inline void (*g_rolloutPlaceHook)(const GroupEnv& env, int gframe, int grev) = nullptr;

template <class Pre, class Post>
inline State rollout(ModelWorld& w, const State& start, long long t0,
                     const std::vector<uint8_t>& lvl, float fallbackDx, Pre&& pre, Post&& post,
                     int bindFrame = -1) {
    FrameSlices fs;
    bindFrameSlices(fs, frameLevel(w.L, bindFrame >= 0 ? bindFrame : (int)start.frame));
    State s = start;
    for (size_t i = 0; i < lvl.size(); ++i) {
        const long long t = t0 + (long long)i + 1;
        // Under reverse (State::rev) x decreases, with the same sign as the search's windows.
        const float dxUsed = ((s.dx > 0.f) ? s.dx : fallbackDx) * (s.rev ? -1.f : 1.f);
        const double xPrev = (double)s.xAbs;
        const double x = (double)advanceX(s.xAbs, dxUsed);
        // --witnessauto (search_key.hpp): the autonomous triggers fire on THIS walk's own
        // crossing, as --replay fires them and as the search dates them (fireAutoTriggers): the
        // crossing is read on the x of tick t-1, so a trigger crossed in tick c takes effect from
        // the placement of tick c+1 (GD's crossing loop runs after the player's collisions; cli.hpp
        // --replay has the measurement).
        if (g_rolloutFiresAuto)
            for (AutoTrig& A : g_autoTrig) {
                if (A.fireT >= 0) continue;
                if (xPrev < A.cx) break;   // sorted by cx
                A.fireT = (int)(t - 1) + A.delay;
                A.fireX = A.lockLastX = xPrev;
                std::printf("witnessauto: uid %d x=%.0f crossed at t=%lld, effect t=%d\n", A.uid,
                            A.cx, t - 1, A.fireT);
            }
        GroupEnv env;
        double pLo = x, pHi = x;
        p2Widen(pLo, pHi, p2Dx(s), p2Dx(s));   // --p2xoff
        if (g_rolloutGroupHook) g_rolloutGroupHook(t, (s.dx > 0.f) ? s.dx : fallbackDx, s, x);
        prepareGroup(w, fs, env, (int)s.frame, s.trig, (int)s.trigT, s.fireB,
                     s.lockOff + (float)(x - (double)s.xAbs), x, xPrev, dxUsed, pLo, pHi, t,
                     GroupOpts{false, false, false});
        if (g_rolloutPlaceHook) g_rolloutPlaceHook(env, (int)s.frame, (int)s.rev);
        const StepCtx K = env.ctx();
        bool dead = false;
        const int frame0 = (int)s.frame;
        State c = advanceState(s, (int)lvl[i], K, dead, nullptr);
        // The groups' on / off and the x-gate digest; --togtrace prints each change at this walk's tick t
        {
            std::vector<gdtrig::TogChange> ch;
            togWorldStep(s, c, (int)lvl[i], g_togTrace ? &ch : nullptr);
            for (const gdtrig::TogChange& e : ch) {
                if (e.gate) std::printf("togtrace: t=%lld gate uid %d fired %d\n", t, e.uid, e.fired ? 1 : 0);
                else std::printf("togtrace: t=%lld tap uid %d group %d %s\n", t, e.uid, e.group, e.on ? "on" : "off");
            }
        }
        if (!dead && !g_rotTrig.empty() && (int)c.frame != frame0)
            bindFrameSlices(fs, frameLevel(w.L, (int)c.frame));
        pre(i, t, (const State&)s, c, dead, K);
        c.action = (uint8_t)lvl[i];
        s = c;
        post(i, t, (const State&)s, dead);
    }
    return s;
}

// ---- the autonomous triggers ----------------------------------------------------------------------
// --autospread (print only): the fire tick below is the frontier LEADER's crossing, and every state
// is placed by it. A state still behind the trigger's x crosses it later on its own -- a speed portal
// taken by part of the frontier is enough -- and meets the trigger's movers that many ticks early.
// For each trigger fired with states behind it, one `autospread:` line once none is left behind: the
// leader's crossing, the tick the last of them crossed (or died), how many were behind out of how
// many, and whether the trigger moves anything. Nothing here is read by the search. (The switch and
// the open records are declared in dynamics.hpp, beside g_autoTrig.)
inline bool autoTrigMoves(const AutoTrig& A) {
    for (const TrigCtl& c : A.ctl)
        if (std::fabs(c.dx) > 0.001f || std::fabs(c.dy) > 0.001f || c.lockTicks > 0.0
            || c.tmode != 0 || c.orbit)
            return true;
    return false;
}
inline void autoSpreadClose(const std::vector<State>& frontier, long long t) {
    for (size_t k = 0; k < g_autoSpreadOpen.size();) {
        const AutoSpreadOpen& o = g_autoSpreadOpen[k];
        const AutoTrig& A = g_autoTrig[o.i];
        size_t behind = 0;
        for (const State& s : frontier)
            if (progressOf(s).x < A.cx) ++behind;
        if (behind) { ++k; continue; }
        std::printf("autospread: uid=%d cx=%.0f moves=%d lead=%lld last=%lld spread=%lld "
                    "behind=%zu of=%zu\n", A.uid, A.cx, autoTrigMoves(A) ? 1 : 0, o.leadT, t - 1,
                    t - 1 - o.leadT, o.behind, o.of);
        g_autoSpreadOpen.erase(g_autoSpreadOpen.begin() + (std::ptrdiff_t)k);
    }
}
// Fire every autonomous trigger (g_autoTrig, in x order) the frontier's leader crossed by tick t-1:
// its effect starts `delay` ticks later. Called once per layer, before the layer is stepped.
inline void fireAutoTriggers(const std::vector<State>& frontier, long long t) {
    if (g_autoTrig.empty()) return;
    if (g_autoSpread && !g_autoSpreadOpen.empty()) autoSpreadClose(frontier, t);
    double xLead = -1e18;
    bool haveLead = false;
    for (AutoTrig& A : g_autoTrig) {
        if (A.fireT >= 0) continue;
        if (!haveLead) {
            for (const State& s : frontier)
                if (progressOf(s).x > xLead) xLead = progressOf(s).x;
            haveLead = true;
        }
        // sorted by cx: the first unfired one that has not been crossed ends the scan (two
        // triggers CAN share an x -- lv19 has pairs at 5,955 and 29,265 -- so no early break on a
        // fire)
        double xl = xLead;   // --chanmove (removed): a channelled Move is fired by the states that have that channel up
        if (A.chan != 0 && g_rotQueue && !g_rotQ.empty()) {
            // a state that has the channel up passes it in its own travel direction (a reversed rider when x falls to cx)
            bool any = false;
            xl = 0.0;
            for (const State& s : frontier) {
                if ((int)s.rotChan != A.chan) continue;
                const double px = progressOf(s).x;
                // --autotrigframe (dynamics.hpp): the trigger's coordinate along the travel axis of the state's frame
                double ucx = A.cx;
                if (g_autoTrigFrame && (int)s.frame != 0) { double v; toFrame((int)s.frame, A.cx, A.cy, ucx, v); }
                if (s.rev ? (px > ucx) : (px < ucx)) continue;
                if (!any || (s.rev ? px < xl : px > xl)) xl = px;
                any = true;
            }
            if (!any) continue;
        } else {
            if (A.chan != 0 && A.chanStatic) continue;   // no rotation queue: --chantog's static test
            if (xl < A.cx) break;
        }
        // `frontier` holds states at t-1, so the crossing is t-1 and the effect lands `delay`
        // ticks later.
        A.fireT = (int)(t - 1) + A.delay;
        A.fireX = A.lockLastX = xl;
        // --autoown: the most ticks a state still behind it needs to reach it at its own speed (dynamics.hpp).
        A.fireSpreadT = 0;
        for (const State& s : frontier) {
            if ((int)s.frame != 0 || s.rev) continue;
            const double sx = progressOf(s).x;
            if (sx < A.cx && s.dx > 0.f)
                A.fireSpreadT = std::max(A.fireSpreadT, (int)std::ceil((A.cx - sx) / (double)s.dx) + 1);
        }
        std::printf("autotrig: uid %d x=%.0f crossed at t=%lld, "
                    "effect t=%d\n",
                    A.uid, A.cx, t - 1, A.fireT);
        if (g_autoSpread) {
            size_t behind = 0;
            for (const State& s : frontier)
                if (progressOf(s).x < A.cx) ++behind;
            if (behind)
                g_autoSpreadOpen.push_back({(size_t)(&A - g_autoTrig.data()), t - 1, behind,
                                            frontier.size()});
        }
    }
}

// ---- which inputs a state may take, and what taking one writes ------------------------------------
// May the button change on tick t (--minpulse: not within g_minPulse ticks of the last change;
// --inputgrid / --gridmap: only on the search's grid, `gridNow`, 1 or less when off)?
inline bool edgeLocked(const State& s, int gridNow, long long t) {
    return (g_minPulse > 1 && (int)s.edgeAge + 1 < g_minPulse)
           || (g_wavePulse > 1 && s.mode == 4 && (int)s.edgeAge + 1 < g_wavePulse)   // --wavepulse
           || (gridNow > 1 && (t % gridNow) != 0);
}
// May `s` take `input` on tick t? Not a change the button cannot make (edgeLocked), and not an edge
// on a tick GD cannot put one (--latgap, frames.hpp g_latGap: s.latLock). A search that steps an
// input this refuses plans something GD will not play.
inline bool inputAllowed(const State& s, int input, int gridNow, long long t) {
    // --mustpress (frames.hpp): the tick's button is given
    if (const int forced = mustPressAt(t); forced >= 0) return input == forced;
    if (edgeLocked(s, gridNow, t) && input != (int)s.action) return false;
    if (s.latLock && input != (int)s.action) return false;
    return true;
}
// What taking `input` from `prev` writes into the stepped child `c`: its own action (keyOf reads it,
// for the robot's hover, before anything else would set it), --minpulse's edge age, and --latgap's
// lock (a mode with latency 2 entered from one without).
inline void noteInput(const State& prev, State& c, int input) {    c.action = (uint8_t)input;
    if (g_minPulse > 1 || g_wavePulse > 1)
        c.edgeAge = (input != (int)prev.action) ? 0 : (uint8_t)std::min(254, (int)prev.edgeAge + 1);
    const auto lat2 = [](uint8_t m) { return m == 1 || m == 3; };
    c.latLock = (lat2(c.mode) && !lat2(prev.mode) && !c.dual) ? 1 : 0;
}

// ---- the model as a search sees it ---------------------------------------------------------------
// One object over the world (ModelWorld) with the model's plugs, so a search names the model it
// asks and nothing else: build a group's tick, step and turn a state in it, spend the rotation
// triggers a group's advance reaches, which states share a tick, where a state has got to, and a
// walk of a whole plan. Each forwards to the function of the same name above.
struct Model {
    ModelWorld& w;

    // prepareGroup, with the group's frame slices from the world's cache.
    void prepare(GroupEnv& e, int gframe, const TouchMask& gtrig, int gFire, const uint16_t* gFireB,
                 float lockAt, double x, double xPrev, float dxUsed, double wLo, double wHi,
                 long long t, const GroupOpts& opts = GroupOpts{}) const {
        prepareGroup(w, w.slicesFor(gframe, wLo), e, gframe, gtrig, gFire, gFireB, lockAt, x,
                     xPrev, dxUsed, wLo, wHi, t, opts);
    }
    // The moving geometry as one lineage left it (Dynamics::saveCarry), for a search that steps
    // several lineages through the world tick by tick: forget (the next prepare seeks afresh, as
    // after a jump), save what the last prepare placed, and put a saved lineage back as of the
    // tick it was placed for. Frame f's level.
    void forgetWorld(int f) const { levelOf(f).dyn.forgetPlacement(); }
    void saveWorld(int f, std::vector<double>& carry) const { levelOf(f).dyn.saveCarry(carry); }
    // --ladderprefix: frame f's level exists already (levelOf binds it lazily otherwise).
    bool frameBound(int f) const { return w.fsl[(size_t)(f & 3)].lv != nullptr; }
    // --ladderprefix: a state's Obj pointer (snapObj, usedOrb, usedPad...) as (frame, list, index)
    // into the frame's level, which is the same in every attempt of a ladder while the addresses
    // are not (each attempt has its own copy of the level). 0 = null; `ok` goes false for a pointer
    // into none of the lists (the save is then not taken).
    static void objLists(const Level& lv, const std::vector<Obj>* out[8]) {
        out[0] = &lv.objs; out[1] = &lv.portals; out[2] = &lv.pads; out[3] = &lv.orbs;
        out[4] = &lv.speeds; out[5] = &lv.slopes; out[6] = &lv.coins; out[7] = &lv.dyn.objs;
    }
    uint64_t encodeObj(const Obj* p, bool& ok) const {
        if (!p) return 0;
        for (int f = 0; f < 4; ++f) {
            if (!frameBound(f)) continue;
            const std::vector<Obj>* ls[8];
            objLists(*w.fsl[(size_t)f].lv, ls);
            for (int k = 0; k < 8; ++k) {
                const std::vector<Obj>& v = *ls[k];
                if (!v.empty() && p >= v.data() && p < v.data() + v.size())
                    return 1 + ((uint64_t)f << 60) + ((uint64_t)k << 56) + (uint64_t)(p - v.data());
            }
        }
        ok = false;
        return 0;
    }
    const Obj* decodeObj(uint64_t c) const {
        if (!c) return nullptr;
        c -= 1;
        const int f = (int)((c >> 60) & 3), k = (int)((c >> 56) & 15);
        const size_t i = (size_t)(c & ((1ull << 56) - 1));
        const std::vector<Obj>* ls[8];
        objLists(levelOf(f), ls);
        return &(*ls[k])[i];
    }
    void loadWorld(int f, const std::vector<double>& carry, long long tPrev) const {
        levelOf(f).dyn.loadCarry(carry, (int)tPrev);
    }
    Level& levelOf(int f) const {
        FrameSlices& fs = w.fsl[(size_t)(f & 3)];
        if (!fs.lv) bindFrameSlices(fs, frameLevel(w.L, f));
        return *fs.lv;
    }
    State advance(const State& s, int input, const StepCtx& K, bool& dead, RotNote* rot) const {
        return advanceState(s, input, K, dead, rot);
    }
    // The item gates on `s`, which has just stepped through tick t from a state whose action was
    // `prevAction` (ModelWorld::gates): fire the Count / Tap / Item Compare triggers into its trig
    // and fireB, and count its items and taps. Only for a living state on a tick that did not turn
    // it (frame == gframe) -- the search's step, the replay walk and the witness all ask it so.
    void fireGates(State& s, uint8_t prevAction, int input, const StepCtx& K, int gframe,
                   long long t, bool dead) const {
        if (!w.gates || dead || (int)s.frame != gframe) return;
        w.gates(w.gatesCtx, s, prevAction, input, K, gframe, (double)s.xAbs, (double)s.y,
                hazardHalfFor(s.mode, s.mini != 0), t);
    }
    // Are the item gates modelled this call (a search that never calls fireGates cannot walk this
    // world, and declines)?
    bool hasGates() const { return w.gates != nullptr; }
    // A press is never moot while the counting tap's window is open and its gate unfired: the
    // presses are what it counts (searchLayers pressMoot).
    bool pressMatters(const State& s) const {
        if (w.tapBit && !(s.trig & w.tapGateBits) && !(s.taps & 0x40)) return true;
        for (int e = 0; e < w.nTapX; ++e)
            if (!(s.trig & w.tapXGate[e]) && !(s.tapsX[e] & 0x40)) return true;
        return false;
    }
    void fireRotTriggers(int gframe, bool grev, double gLo, double gHi, double wLo, double wHi,
                         long long t) const {
        dp::fireRotTriggers(gframe, grev, gLo, gHi, wLo, wHi, t);
    }
    GroupKey groupKey(const State& s, float fallbackDx, uint64_t fsig) const {
        return groupKeyOf(s, fallbackDx, fsig);
    }
    bool inGroup(const GroupKey& k, const State& s, float fallbackDx) const {
        return dp::inGroup(k, s, fallbackDx);
    }
    Progress progress(const State& s) const { return progressOf(s); }
    void fireAutoTriggers(const std::vector<State>& frontier, long long t) const {
        dp::fireAutoTriggers(frontier, t);
    }
    bool edgeLocked(const State& s, int gridNow, long long t) const {
        return dp::edgeLocked(s, gridNow, t);
    }
    bool inputAllowed(const State& s, int input, int gridNow, long long t) const {
        return dp::inputAllowed(s, input, gridNow, t);
    }
    void noteInput(const State& prev, State& c, int input) const { dp::noteInput(prev, c, input); }
    template <class Pre, class Post>
    State rollout(const State& start, long long t0, const std::vector<uint8_t>& lvl,
                  float fallbackDx, Pre&& pre, Post&& post, int bindFrame = -1) const {
        return dp::rollout(w, start, t0, lvl, fallbackDx, pre, post, bindFrame);
    }
};

}  // namespace dp
