#pragma once
#include "dp/step.hpp"

namespace dp {

// ---- divergence-driven local fixups (--fixups) -----------------------------
// When the driver's GD replay splits from the model trace, the driver records
// the ONE transition where they split -- the state before it, the input, and
// GD's observed outcome as deltas -- and the model substitutes GD's outcome
// for any transition that matches. GD is the authority, the model is the
// approximation; this makes an observed disagreement cost one iteration
// instead of thirty re-anchors around the same wall (measured shape on lv19:
// x=15,352 and x=21,386 each ate 15-30 iterations of oscillation).
// Design constraints, in order of importance:
//   * DELTAS, not absolutes: neighbours in the frontier keep their structure.
//   * The window is ONE TICK wide (x +-2.5 px at 1.3-1.6 px/tick) and gated on
//     input, mode, size, gravity, grounded and a y/vy neighbourhood -- a fixup
//     must never generalise beyond the transition it was measured on.
//   * Applied inside stepBoth, so the search, the witness resim and --replay
//     see the same physics (two drifting copies cost a session once).
//   * Records come from OUR OWN replay of OUR OWN plan (same provenance as
//     the groups live recording), so the cold rule is untouched.
//   * Every applied fixup is also a MEASURED fidelity gap; the file doubles
//     as the to-fix list for the model (log it, do not let it rot).
// Limits: y/vy/grounded/dead only (a fixup does not restore a lost portal or
// trigger side effect).
//
// [2026-08-23] DUAL SECTIONS ARE NO LONGER EXCLUDED. They were, and the reason
// was never the physics: the driver's recorder resimulates with the second body
// zeroed, so it cannot start inside a dual and therefore never had a dual
// transition to write down. The in-process recorder reads both bodies off the
// game every tick, so the record can hold both -- and it has to, because a
// record measured on one body must never be applied to a pair (the half nobody
// measured would be carried by a rule that never saw it). `dual` is part of the
// match for exactly that reason, so every record written before this change
// keeps matching precisely what it used to.
struct Fixup {
    float x, y, vy, dy, dvy;
    uint8_t in, mode, mini, flip, g, gAfter;
    // kill record: GD ended the run on this transition while the model let it
    // live (the saw-boundary class: the trajectories agree to the last tick
    // and only the verdict differs). Applied as dead=true, nothing else.
    uint8_t kill = 0;
    // The second body, when this was measured on a pair.
    uint8_t dual = 0, gAfter2 = 255;
    float y2 = 0, vy2 = 0, dy2 = 0, dvy2 = 0;
    // A two-player level's second body has a button of its own: the transition the record was measured on was made with this one (named tail `,in2=`).
    // 255 = not said (every record of a level that is not two-player, and every record written before the tail), which matches whatever the second body pressed.
    uint8_t in2 = 255;
    // File position at load. The file is append-only within a run, so a
    // larger ord is a NEWER measurement -- the conflict filter (cli.hpp)
    // keeps the newest of two records that share a key but disagree.
    int ord = 0;
};
// The load-time conflict filter (cli.hpp). Off: both no-filter builds broke
// the lv22 corridor oscillation and no filtered build did.
inline bool g_fixupConflictFilter = false;
inline std::vector<Fixup> g_fixups;
// x-sorted views, kills and deltas separately (built after loading). With a
// few hundred records a linear scan per CHILD costs real layer time (200
// compares x 160k children x 6k layers), while records live at fixed x -- a
// lower_bound plus the 2-3 in-window candidates is effectively free.
inline std::vector<Fixup> g_fixupKills, g_fixupDeltas;
inline long long g_fixupHits = 0;
// ---- WHICH FRAME WAS THE MODEL IN WHEN A RECORD FIRED? ------------------
//
// A record's dy is GD's WORLD dy (the mod reads it off PlayerObject), while the
// state it is added to keeps its y in the CURRENT frame's coordinates
// (State::frame, state.hpp:483). Those are the same axis only while the model
// is in frame 0, and applyFixup below has never looked: its only gates are
// "there are records" and "not near moving geometry".
//
// 2026-09-11 measured 34 of one lv22 run's 69 records as written while the
// model was rotated. That says the records EXIST, not that any of them was
// ever applied -- a different claim, and this project has been wrong about
// exactly that distinction before (a gate passed 593 times and fired 0). So
// count the firings before proposing anything: a census, not a gate.
//
// s.frame is the frame AT APPLICATION, not the one the record was written in.
// They are the same tick of nearly the same trajectory and normally agree, but
// that is an assumption. Enough to ask "did a rotated state ever take a
// delta?"; NOT enough to count how many records are corrupt.
// (the counts are in g_fixupCensus below; g_fixupRotSeen is how many of the
// eight x slots are taken)
inline float g_fixupRotX[8] = {0, 0, 0, 0, 0, 0, 0, 0};
inline std::atomic<int> g_fixupRotSeen{0};
// ...AND THE DENOMINATOR. The counters above are firings, and a firing count of
// zero has two readings: the lookup ran often while rotated and never matched,
// or it hardly ran while rotated at all. The first says something about frames;
// the second says nothing, and the two are indistinguishable from the numerator.
//
// This is the shape of the very example the commit above cited -- a gate passed
// 593 times and fired 0 -- and that lesson needs BOTH numbers. Reporting the 0
// alone was the same mistake with the roles swapped.
//
// Counted at applyFixup's entry, before either lookup, so one call is one tick
// of one state whichever branch it takes. The x of the first few rotated CALLS
// is kept as well: if the denominator turns out to be zero, its x says where
// the lookup stopped being reached, which the firing counters cannot.
//
// ONE ROW PER THREAD, summed where they are printed (fixupCensus), as
// g_frameRevReach in bands.hpp and for the same reason: the search's pool
// threads count them, and shared counters were a cache line every lookup wrote
// and lost increments to the race. A row holds the lookups and the firings by
// (frame, rev) -- framerev:'s lookups= and hits=; fixupcall: and fixupframe:
// print their per-frame sums -- and the rotated (frame != 0) totals. Frame 0's
// hits are worn by both the plain frame and the anchor rewrite's (frame 0,
// rev 1), so a frame-only hit count could not say whether any record was ever
// applied to a state GD recorded as frame 2.
// Only the x lists are shared: a compare-exchange takes a slot while fewer than
// eight are taken, so past the eighth a rotated state only reads the count.
struct alignas(64) FixupCensusRow {
    long long call[4][2];
    long long hit[4][2];
    long long callRot, hitRot;
};
inline FixupCensusRow g_fixupCensus[kThreadSlots] = {};
inline float g_fixupCallRotX[8] = {0, 0, 0, 0, 0, 0, 0, 0};
inline std::atomic<int> g_fixupCallRotSeen{0};   // x slots taken, as g_fixupRotSeen
inline FixupCensusRow fixupCensus() {
    FixupCensusRow t{};
    for (const FixupCensusRow& r : g_fixupCensus) {
        for (int f = 0; f < 4; ++f)
            for (int v = 0; v < 2; ++v) {
                t.call[f][v] += r.call[f][v];
                t.hit[f][v] += r.hit[f][v];
            }
        t.callRot += r.callRot;
        t.hitRot += r.hitRot;
    }
    return t;
}
// The x window is ONE TICK (1.2 px at 1.3-1.6 px/tick, still covering the
// +-1 px stair-snap offset class). It was 2.5 and a DELTA record then
// re-matched the NEXT tick's transition too: applied every tick it kept the
// state inside its own window, drifted it along, and -- being earlier in the
// file -- shadowed the KILL record 1.6 px behind it forever (lv19's saw at
// x=15,350: the single-record A/B killed the replay, the full file did not).
// Kills are also matched FIRST for the same reason: a verdict must not lose
// to a trajectory patch from the neighbouring tick.
// The second body's button of the step being matched, set by stepBoth (-1: the pair shares the first body's button, or there is no pair).
inline thread_local int t_fixIn2 = -1;
inline bool fixupMatches(const Fixup& f, const State& s, int input) {
    if ((int)f.in != input || f.mode != s.mode || f.mini != s.mini
        || f.flip != s.flip || f.g != s.grounded)
        return false;
    if (f.in2 != 255 && t_fixIn2 >= 0 && (int)f.in2 != t_fixIn2) return false;
    // A record measured on one body and a state with two are not the same transition, whatever
    // the first body is doing. Records written before duals were recordable carry dual=0 and so
    // keep matching exactly the states they always did.
    if (f.dual != s.dual) return false;
    if (std::fabs((double)s.xAbs - (double)f.x) > 1.2) return false;
    if (std::fabs((double)s.y - (double)f.y) > 4.0) return false;
    if (std::fabs((double)s.vy - (double)f.vy) > 1.0) return false;
    if (f.dual) {
        if (std::fabs((double)s.y2 - (double)f.y2) > 4.0) return false;
        if (std::fabs((double)s.vy2 - (double)f.vy2) > 1.0) return false;
    }
    return true;
}
inline const Fixup* findFixup(const std::vector<Fixup>& v, const State& s,
                              int input) {
    const float lo = s.xAbs - 1.2f;
    auto it = std::lower_bound(
        v.begin(), v.end(), lo,
        [](const Fixup& f, float x) { return f.x < x; });
    // NEAREST match, not first: the recorder now writes a refined record when
    // an existing one's deltas disagree with a new observation on the same
    // key window (repair.hpp fixupOnFile), so two records can legitimately
    // share a window. Each must win exactly where it was measured; file order
    // would make the older one shadow the refinement forever. Distances are
    // normalised by the match window so no axis dominates.
    const Fixup* best = nullptr;
    double bestD = 1e18;
    for (; it != v.end() && it->x <= s.xAbs + 1.2f; ++it)
        if (fixupMatches(*it, s, input)) {
            const double d =
                std::fabs((double)s.xAbs - (double)it->x) / 1.2
                + std::fabs((double)s.y - (double)it->y) / 4.0
                + std::fabs((double)s.vy - (double)it->vy) / 1.0;
            if (d < bestD) { bestD = d; best = &*it; }
        }
    return best;
}
// DELTA fixups are keyed on the player alone. Next to MOVING geometry the
// transition also depends on the world's phase, which a record from another
// worldline cannot carry: lv22 x=8,185 -- records carrying dvy=+3.426 (the
// carry of a lift that was RISING in the plans they were measured on) fired
// on a worldline whose lift was SINKING, and taught the resim an ascent GD
// does not have. One key, two truths, and no way to split them by player
// state -- so no DELTA applies while a dynamic object is within reach;
// moving-geometry trajectories come from the recordings and the trigger
// definitions instead.
// KILLS ARE NOT EXEMPT, whatever this note used to say: stepBoth refuses the
// whole lookup here, kills included, and its note carries the measurement that
// chose that (the kills-exempt variant sat pinned in the lv22 corridor). The
// case for exempting them is still worth keeping in view: a phase-wrong kill
// costs one route point and the search replans around it, while a GATED kill
// leaves a phantom survival open -- the lv22 corridor deaths at (9,827,
// y~1,738) and (8,286, y~1,072) are dyn kills, and so were GD's deaths at
// t=2,416 and 2,842 under the ceiling of group 265, which the gate held off
// until the ceiling's own timing was right (the autonomous lag, dynamics.hpp).
// --fixreach (ON since 2026-10-24; --no-fixreach is the off arm): the gate holds a record back only for a moving object that could have taken part in this step -- its box meets the box the player
// sweeps over the step, the player's own box from where it is to where the step takes it (dxF in x, vy in y; no margin) -- instead of any moving object within hw + 40 px
// of the player. The flat gate held back the kill record of a custom level's wall at x=28,498 (a mini flipped ship inside a pair of slopes, NULL killer) for ten loop
// iterations, for two small thorns 22 and 26 px clear of the player's box (hitbox trace, dmg lines at t=20,797: uid 172427 3.3x3.7 and uid 69001 8.6x7.1) that the
// kill does not involve; neither meets the swept box. The object's own sweep is only what Obj carries of it (its y step dcy, its half-height change dhh): its x step is
// not in Obj, so an object closing in on the player sideways within the tick is judged from where it stands. An object the recording holds on ONE row, at a tick the
// recording covers, never moves there and is not a reason to hold a record back (a custom level's Tidal wall at x=81,705: three thorns that never move, one of them the killer).
// One wall measured; the lv22 corridor the 40 px
// gate was chosen on (above) is the case to run before this goes on.
inline bool g_fixNearReach = true;
inline const Obj* nearDynObjectPtr(const State& s, const StepCtx& K) {
    if (g_fixNearReach) {
        const double half = playerHalf(s.mode, s.mini != 0);
        const double px0 = (double)s.xAbs, px1 = px0 + (double)K.dxF;
        const double py0 = (double)s.y, py1 = py0 + (double)s.vy;
        const double pxLo = std::min(px0, px1) - half, pxHi = std::max(px0, px1) + half;
        const double pyLo = std::min(py0, py1) - half, pyHi = std::max(py0, py1) + half;
        for (const Obj* o : *K.near) {
            if (!o->dynObj) continue;
            // an object the recording holds still, at a tick the recording covers (its trailer `end,<tick>`), has no phase for a record to be wrong about
            if (o->recStatic && (long long)K.t <= g_groupsEndT) continue;
            const double hh = o->hh + std::fabs(o->dhh);
            const double oyLo = std::min(o->cy, o->cy - o->dcy) - hh, oyHi = std::max(o->cy, o->cy - o->dcy) + hh;
            if (o->cx + o->hw > pxLo && o->cx - o->hw < pxHi && oyHi > pyLo && oyLo < pyHi) return o;
        }
        return nullptr;
    }
    for (const Obj* o : *K.near)
        if (o->dynObj
            && std::fabs((double)s.xAbs - o->cx) < o->hw + 40.0
            && std::fabs((double)s.y - o->cy) < o->hh + 40.0)
            return o;
    return nullptr;
}
inline bool nearDynObject(const State& s, const StepCtx& K) {
    return nearDynObjectPtr(s, K) != nullptr;
}
// The uid of the moving object that stopped a MATCHING record on this step, -1
// when nothing was stopped (no record matched, or none was near moving
// geometry, or the record fired). Written by stepBoth on every step and read by
// the replay's trace (column `fxblk`), so the loop's recorder can tell "a record
// covers this and fires" from "a record covers this and the gate above keeps it
// from firing" -- the two looked identical to it ("a record already covers this
// state"), and SubZero 4002 re-flew one plan 25+ times on the second.
// The recorder cannot ask the gate itself: it needs the
// moving geometry's position at this tick, which only the solver has.
inline thread_local int g_fxBlockedUid = -1;
// One place, so a kill hit and a delta hit cannot be counted by different
// rules. Records the x of the first few rotated firings too: applyFixup has no
// tick to hand and giving it one would change a signature for a census.
// The slot is the value the compare-exchange took, not a second read of the
// count: a test and an index read separately let another thread's bump in
// between write past the end of the list.
inline void takeRotX(std::atomic<int>& taken, float (&xs)[8], float x) {
    int n = taken.load(std::memory_order_relaxed);
    while (n < 8 && !taken.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) {}
    if (n < 8) xs[n] = x;
}

inline void noteFixupFrame(const State& s) {
    const unsigned f = (unsigned)s.frame;
    FixupCensusRow& row = g_fixupCensus[g_threadSlot & (kThreadSlots - 1)];
    if (f < 4) ++row.hit[f][s.rev ? 1 : 0];
    if (f != 0) {
        ++row.hitRot;
        takeRotX(g_fixupRotSeen, g_fixupRotX, s.xAbs);
    }
}

inline void noteFixupCall(const State& s) {
    const unsigned f = (unsigned)s.frame;
    FixupCensusRow& row = g_fixupCensus[g_threadSlot & (kThreadSlots - 1)];
    if (f < 4) ++row.call[f][s.rev ? 1 : 0];
    if (f != 0) {
        ++row.callRot;
        takeRotX(g_fixupCallRotSeen, g_fixupCallRotX, s.xAbs);
    }
}

inline void applyFixup(const State& s, int input, State& c, bool& dead,
                       bool deltasToo = true) {
    noteFixupCall(s);
    if (const Fixup* f = findFixup(g_fixupKills, s, input)) {
        (void)f;
        dead = true;
        noteFixupFrame(s);
        ++g_fixupHits;
        return;
    }
    if (!deltasToo) return;
    if (const Fixup* f = findFixup(g_fixupDeltas, s, input)) {
        YSET(c.y) = s.y + f->dy;
        VYSET(c.vy) = s.vy + f->dvy;
        // 255 = "leave the model's grounded flag alone" -- GD's onGround is
        // sticky for the flying modes and cannot be copied into the model's
        // stricter semantics.
        if (f->gAfter != 255) c.grounded = f->gAfter;
        if (f->dual) {
            c.y2 = s.y2 + f->dy2;
            c.vy2 = s.vy2 + f->dvy2;
            if (f->gAfter2 != 255) c.grounded2 = f->gAfter2;
        }
        dead = false;
        noteFixupFrame(s);
        ++g_fixupHits;
        return;
    }
}

// --fxwatch <t>: WHY a fixup did or did not fire on the replayed state at one
// step. Print only -- it reads what applyFixup and its gate read and writes
// nothing the step uses; the replay's per-tick line prints it (cli.hpp).
//
// Written for lv22 t=11,239, where the recorder calls a transition "covered" by
// a record the resim never applies. The two sides do not ask the same question:
// the recorder's test (repair.hpp fixupOnFile) is the key window plus delta
// agreement, while this path first refuses the whole lookup next to moving
// geometry (nearDynObject, below in stepBoth). So the line says which of them
// refused: the gate and the object that tripped it, then every record in the
// x window with each key field's match and its distances, kills and deltas both.
inline void fxDescribe(const State& s, int input, const StepCtx& K,
                       const State& c, long long hits0) {
    const int cap = (int)sizeof g_fxWhy;
    int o = 0;
    auto clampO = [&]() { if (o >= cap) o = cap - 1; };
    o += std::snprintf(g_fxWhy + o, cap - o,
                       "Kt=%lld xAbs=%.4f y=%.4f vy=%.4f in=%d mode=%d mini=%d flip=%d "
                       "g=%d dual=%d frame=%d records=%zu",
                       (long long)K.t, (double)s.xAbs, (double)s.y, (double)s.vy, input,
                       (int)s.mode, (int)s.mini, (int)s.flip, (int)s.grounded,
                       (int)s.dual, (int)s.frame, g_fixups.size());
    clampO();
    const Obj* dyn = nullptr;
    if (K.near)
        for (const Obj* ob : *K.near)
            if (ob->dynObj && std::fabs((double)s.xAbs - ob->cx) < ob->hw + 40.0
                && std::fabs((double)s.y - ob->cy) < ob->hh + 40.0) { dyn = ob; break; }
    o += std::snprintf(g_fxWhy + o, cap - o, " nearDyn=%d", dyn ? 1 : 0);
    clampO();
    if (dyn) {
        o += std::snprintf(g_fxWhy + o, cap - o, " dynUid=%d |dx|=%.1f<%.1f |dy|=%.1f<%.1f",
                           dyn->uid, std::fabs((double)s.xAbs - dyn->cx), dyn->hw + 40.0,
                           std::fabs((double)s.y - dyn->cy), dyn->hh + 40.0);
        clampO();
    }
    o += std::snprintf(g_fxWhy + o, cap - o, " fired=%lld vyOut=%.4f",
                       g_fixupHits - hits0, (double)c.vy);
    clampO();
    for (int kd = 0; kd < 2; ++kd) {
        const std::vector<Fixup>& v = kd ? g_fixupDeltas : g_fixupKills;
        auto it = std::lower_bound(v.begin(), v.end(), s.xAbs - 1.2f,
                                   [](const Fixup& f, float x) { return f.x < x; });
        for (; it != v.end() && it->x <= s.xAbs + 1.2f && o < cap - 200; ++it) {
            const Fixup& f = *it;
            o += std::snprintf(g_fxWhy + o, cap - o,
                               " | %s x=%.4f y=%.4f vy=%.4f key in%d mode%d mini%d flip%d g%d dual%d"
                               " |dx|=%.3f |dy|=%.3f |dvy|=%.3f matches=%d",
                               kd ? "delta" : "kill", (double)f.x, (double)f.y, (double)f.vy,
                               (int)f.in == input, f.mode == s.mode, f.mini == s.mini,
                               f.flip == s.flip, f.g == s.grounded, f.dual == s.dual,
                               std::fabs((double)s.xAbs - (double)f.x),
                               std::fabs((double)s.y - (double)f.y),
                               std::fabs((double)s.vy - (double)f.vy),
                               fixupMatches(f, s, input) ? 1 : 0);
            clampO();
        }
    }
}

// markTouched's preY: the button's own row (--touchprey=button; the parent
// arm, the previous row's y, is gone since the flag clean-up).
inline double touchPreY(const State& s, const State& c, bool set, double y) {
    return set ? y : (double)c.y;
}

// `input2` is the second body's own button (a plan's input bit 1) in a two-player level (g_twoPlayer); -1, and every call in a level
// that is not two-player, gives it the first body's, as a dual's two halves always had.
inline State stepBoth(const State& s, int input, const StepCtx& K, bool& dead, int input2 = -1) {
    bool d1 = false;
    g_halfNow = 0;
    g_fxBlockedUid = -1;
    t_fixIn2 = -1;
    // Out-parameter, not a global: phase 1 steps the layer in parallel.
    bool p1FlippedGravity = false;
    if (g_touchCensus) g_tcBranch = 0;
    g_preBtnSet = false;
    t_spiderFreshWarp = false;   // --spidertptouch
    t_ringLinkLate = 0;   // --ringtaplink
    State c = stepOne(s, input, K, d1, &p1FlippedGravity);
    const bool p1FreshWarp = t_spiderFreshWarp;   // p1's, before p2's stepOne can overwrite it
    const bool p1RingLate = t_ringLinkLate != 0;
    const int p1RingKind = t_ringLinkLate;
    t_ringLinkLate = 0;
    if (g_touchCensus) g_tcBranchP1 = g_tcBranch;
    // p1's pre-button y, saved before p2's stepOne can overwrite it
    const bool preBtnSet = g_preBtnSet;
    const double preBtnY = g_preBtnY;
    const bool p1Tapped = g_ballTapped;   // --repelland, likewise
    const uint8_t p1HitG = g_hitGRepel;   // --repela0c, likewise
    // --p1soloend (constants.hpp): player 1's own pass ended the dual, so there is no second body
    // to step on this tick.
    if (!s.dual || (g_p1SoloEnd && !c.dual)) {
        // --p2born: the tick a dual portal makes the second body is the tick it is born not holding (3); its level in the
        // plan does not count until the plan shows it released (below)
        c.action2 = (g_p2Born && g_twoPlayer && c.dual && !s.dual) ? (uint8_t)3 : (uint8_t)2;   // --dualinlat: no second body, nothing to remember
        dead = d1;
        markTouched(c, K, touchPreY(s, c, preBtnSet, preBtnY), s.action != 0,
                    input != 0 && s.action == 0, &s, input != 0);
        // No fixup of either kind applies near moving geometry (nearDynObject
        // above). This exact constellation is the one that breaks the lv22
        // corridor oscillation: both breakout runs (2026-08-26 01:15 and
        // 03:15, iter ~214 each) ran it, while kills-exempt, drop-both and
        // newest-wins variants all sat pinned for 70+ minutes. The dynamics
        // are not fully understood -- what is measured is which build climbs.
        const long long fxHits0 = g_fixupHits;
        if (!g_fixups.empty()) {
            const Obj* dyn = nearDynObjectPtr(s, K);
            if (!dyn)
                applyFixup(s, input, c, dead);
            else if (findFixup(g_fixupKills, s, input) || findFixup(g_fixupDeltas, s, input))
                g_fxBlockedUid = dyn->uid;   // matched, and the gate kept it back
        }
        if (g_fxWatchT >= 0 && K.t == g_fxWatchT) fxDescribe(s, input, K, c, fxHits0);
        return c;
    }
    State sb = s;
    swapHalves(sb);
    // GD PROCESSES p1 FIRST, so the second body sees the first one's FINISHED tick -- and the
    // one thing that reads across the pair is the dual ball's flip, whose gate is "has the
    // partner landed" (stepOne's `s.grounded2`). Both halves are stepped from the same start
    // state here, so that gate was answering with p1's state BEFORE its own step, and the flip
    // came out a tick late. Same class as [[gd-same-tick-landing]].
    //
    // Measured on lv16 t=12,617 (dual ball, both full size, both flipped, climbing):
    //   GD     p1 lands at y=585 on this tick; p2 y2=555.604 and vy2 := **-2.000**, up 1 -> 0
    //   model  p2 y2=555.6039 -- the position to four decimals -- but vy2 8.973, still
    //          climbing, and it flips on 12,618 instead
    // The threshold itself is right: two attempts bracket it at |sep| 33.4 fires / 35.4 does
    // not, against the rule's 2*pHalf+5 = 35. Only the tick was wrong.
    //
    // Just the flag, not the whole of `c`. Handing p2 every post-step field of p1 is what GD's
    // ordering really means, but it is a much larger change than the one measurement here
    // supports, and this gate is the only cross-body read in stepOne.
    sb.grounded2 = c.grounded;
    // --repellpolarity: the same bounce's polarity gate reads p1's finished polarity
    // too. stepOne reads flip2 nowhere else, so this reaches only that gate.
    sb.flip2 = c.flip;
    // [2026-09-05] ...and the SECOND cross-body read: p1's gravity flip reaches
    // the partner. GD's flipGravity fires the other player with the polarity
    // inverted, from inside p1's collision pass -- so it lands BEFORE p2's own
    // update. Applying it to sb (p2's entering state) is what puts the halving
    // on the correct side of p2's integration; leaving it to p2's own portal
    // pass gives (vy + a)/2 where GD gives vy/2 - a.
    // The gate is GD's: dual, and the six mode bytes equal between the bodies
    // (wave is not compared -- so a cube and a wave still count as matching,
    // which is the asm's own shape, not a simplification).
    // (It used to stand aside for r101's same-box skip in the portal pass; that
    // skip was the --no-dualcouple arm and is gone since the flag clean-up.)
    // p2's own portal on the same tick then finds itself already at the target
    // and no-ops, so nothing double-flips.
    // --dualcouple [2026-09-19]: GD's partner call SETS the other body to the
    // inverse of the new polarity and halves only if that changed it; a plain
    // toggle would equal that only for a mirrored pair. See the p2 -> p1 half after
    // p2's step.
    // --dualunlink (frames.hpp): none of the partner calls below happens while an Options trigger
    // has unlinked the pair's gravity (the layer's +0x860, which flipGravity and
    // playerWillSwitchMode test first) -- each body's flip stays its own.
    const bool unlinked = dualUnlinkedAt((int)s.frame, (double)s.xAbs, (double)s.y);
    if (p1FlippedGravity && s.dual && !unlinked
        && sameModeFlags(s.mode, s.mode2)) {
        const uint8_t want = (uint8_t)!c.flip;
        if (sb.flip != want) {
            sb.flip = want;
            sb.vy = (float)((double)sb.vy * 0.5);
            // --ballscaleair: the partner call is a flipGravity, which lowers a ball's +0x728
            if (sb.mode == 2) sb.ball728 = (uint8_t)((sb.ball728 & ~3u) | 1u);
        }
    }
    // --ballscaleair: each body's polarity as its own step left it; the partner calls and the repel
    // below that turn a ball's gravity again are flipGravity too (checked after the last of them)
    const uint8_t ballF1Own = c.flip;
    bool d2 = false;
    bool p2FlippedGravity = false;
    g_halfNow = 1;
    // --dualband: the second half's portal band reads p1's finished mode, and says if it wrote
    g_bandPortalWrote = false;
    g_dualOtherMode = (int)c.mode;
    g_p2SoloHalf = true;   // the second body that has left runs alone (formerly --p2solo)
    g_p2SoloFired = false;
    t_followHalf2 = 1;   // --followy: player 1's y is sb's y2
    t_p2RingFresh = -1;  // --rtlp2ring
    // --p2xoff: the second body steps where IT is, dx2 from player 1 (the windows cover it: model.hpp
    // p2Widen). Its x comes back to the pair's below, and the offset moves by however much farther
    // one half went than the other this tick -- a teleport trigger takes player 1 alone. Below 0.5 px
    // the two advances are the same step rounded at two magnitudes, and are not a move.
    const bool p2x = s.frame == 0;
    if (p2x && s.dx2 != 0.f) sb.xAbs = (float)((double)sb.xAbs + (double)s.dx2);
    // --dualinlat (speed.hpp): a second body with the longer input latency (ship, UFO) against a first without takes the
    // first one's input of the tick before, and sees its own tick before that as its previous button
    int p2in = input;
    bool p2Delayed = false;
    // a two-player level's second body takes its own button, and sees its own previous one (State::action2; the first body's
    // where nothing has said yet). The latency of its input is its own mode's, which the caller's schedule has already applied.
    if (g_ghostOn && g_twoPlayer) input2 = ghostLevelAt((long long)K.t);   // the other body's plan (frames.hpp)
    const bool own2 = g_twoPlayer && input2 >= 0;
    const bool p2BornNow = g_p2Born && s.action2 == 3;
    if (own2) {
        p2in = input2;
        // --p2born: a body born holding nothing takes the plan's level 1 for a held button only after a press the plan
        // makes (a level of 0 first): the game gets an input only where the mask changes, and a level that was 1 before
        // the body existed is no press for it
        if (p2BornNow && input2 == 1) p2in = 0;
        sb.action = (s.action2 == 3) ? (uint8_t)0 : ((s.action2 == 2) ? s.action : s.action2);
        // --p2held (speed.hpp): the second body's wave reads the button it held, not the first body's
        if (g_p2Held) sb.held = (uint8_t)(sb.action != 0);
    }
    if (!own2 && g_dualInLat && (s.mode2 == 1 || s.mode2 == 3) && !(s.mode == 1 || s.mode == 3)) {
        p2in = (int)s.action;
        sb.action = (s.action2 == 3) ? (uint8_t)0 : ((s.action2 == 2) ? s.action : s.action2);
        p2Delayed = true;
    }
    State cb = stepOne(sb, p2in, K, d2, &p2FlippedGravity);
    if (p2x) {
        const double moved = ((double)cb.xAbs - (double)sb.xAbs) - ((double)c.xAbs - (double)s.xAbs);
        c.dx2 = (std::fabs(moved) > 0.5) ? (float)((double)s.dx2 + moved) : s.dx2;
    }
    t_followHalf2 = 0;
    const int p2RingFresh = t_p2RingFresh;
    t_p2RingFresh = -1;
    g_p2SoloHalf = false;
    const bool p2Solo = g_p2SoloFired;
    g_dualOtherMode = -1;
    const bool p2WroteBand = g_bandPortalWrote;
    g_halfNow = 0;
    const bool p2Tapped = g_ballTapped;   // --repelland
    const uint8_t p2HitG = g_hitGRepel;   // --repela0c
    swapHalves(cb);
    const uint8_t ballF2Own = cb.flip2;   // --ballscaleair (see ballF1Own)
    // shared fields (x, speed, dual) come from the first half; the second half
    // only contributes its own body -- and `mode` / `mini` / the ceiling press
    // counters are ITS fields now, not shared ones (State::mode2 / mini2 say
    // why). SIZE left this list on 2026-08-28: it was the last thing a portal
    // could change for one half while the merge threw the answer away.
    c.action2 = (p2Delayed || own2) ? (uint8_t)p2in : (uint8_t)2;   // --dualinlat; a two-player level's own button
    if (p2BornNow && own2 && input2 == 1) c.action2 = 3;   // --p2born: still holding nothing
    c.y2 = cb.y2;            c.vy2 = cb.vy2;
    c.mode2 = cb.mode2;      c.mini2 = cb.mini2;
    c.ceilT2 = cb.ceilT2;    c.ceilM42 = cb.ceilM42;
    c.slopeM2 = cb.slopeM2;  c.snapDist2 = cb.snapDist2;
    c.grounded2 = cb.grounded2; c.flip2 = cb.flip2;
    c.ringHold2 = cb.ringHold2; c.onSlope2 = cb.onSlope2;
    c.pressSpent2 = cb.pressSpent2;
    c.slopeT2 = cb.slopeT2;
    c.rideLanded2 = cb.rideLanded2;
    c.seatT2 = cb.seatT2;   // --dualseatt
    // ...and the second body's velocity-limit exemption (State::boost2, added
    // 2026-09-06 with swapHalves). The third of the three sites the SIZE note
    // above is about: without this line p2's latch is written inside its own
    // stepOne and thrown away here every tick.
    c.boost2 = cb.boost2;
    // --dualslide: ...and the second body's dart slide arm (State::slideT2), which its own
    // stepOne wrote into the swapped slot.
    if (g_dualSlide) c.slideT2 = cb.slideT2;
    // --dualhover: ...and its robot hover budget (State::rHover2)
    c.rHover2 = cb.rHover2;
    // --dualdash: ...and its dash (State::dashing2 / dashSlope2), for the same reason
    { c.dashing2 = cb.dashing2; c.dashSlope2 = cb.dashSlope2; }
    c.slopeUid02 = cb.slopeUid02;  c.slopeUidNow2 = cb.slopeUidNow2;
    c.snapObj2 = cb.snapObj2;   c.usedOrb2 = cb.usedOrb2;
    c.usedOrbOld2 = cb.usedOrbOld2;
    for (int i = 0; i < 3; ++i) c.usedOrbHist2[i] = cb.usedOrbHist2[i];
    for (int i = 0; i < 2; ++i) c.spdFired2[i] = cb.spdFired2[i];
    for (int i = 0; i < 2; ++i) c.spdMore2[i] = cb.spdMore2[i];   // --spdsameonce
    c.hitG2 = cb.hitG2;   // --repela0c
    c.halfPrev2 = cb.halfPrev2;   // --portprevhalf
    // --a1clatch: the second body's +0xa1c is bit 1 (0 in both halves when the flag is off)
    c.a1cLatch = (uint8_t)((c.a1cLatch & 1u) | (cb.a1cLatch & 2u));
    // --ballscaleair: the second body's +0x728 is bits 2-3 (0 in both halves when the flag is off)
    c.ball728 = (uint8_t)((c.ball728 & 3u) | (cb.ball728 & 12u));
    {         // --rot2
        c.rot2 = cb.rot2;
        c.rotStep2 = cb.rotStep2;
        c.rotNeg2 = cb.rotNeg2;
    }
    c.holdDead2 = cb.holdDead2;
    if (g_dualJumpBuf) c.jumpBuf2 = cb.jumpBuf2;   // --dualjumpbuf
    { c.flipT2 = cb.flipT2; c.modeT2 = cb.modeT2; }   // --dualflipclock
    for (int i = 0; i < 3; ++i) c.portSeen2[i] = cb.portSeen2[i];
    for (int i = 0; i < 4; ++i) c.touchRing2[i] = cb.touchRing2[i];
    c.touchRingT2 = cb.touchRingT2;
    // [2026-09-05] ...and the SPENT-GRAVITY-PORTAL mask, which 67ab13f added to
    // State and to swapHalves and then left out of this list -- exactly what the
    // SIZE note above records happening on 2026-08-28. The second body's latch
    // was written inside its own stepOne and thrown away here on every tick, so
    // p2's mask read 0x0 forever and it re-fired the same portal indefinitely.
    // Measured on lv16 uid 3450: `gplatch half=1 ... bit=5 spent=0 mask=0x0` on
    // 65 consecutive ticks from t=8,016, with the dual `wasInBoxPrev` skip doing
    // the suppression the latch was supposed to do.
    c.portalLatch2 = cb.portalLatch2;
    for (int i = 0; i < 4; ++i) c.usedPad2[i] = cb.usedPad2[i];
    // --dualspeedp2 [2026-09-29]: ...but a SPEED portal the second body touches sets the
    // speed of the pair, and it is written after the first body's (the game handles p1's
    // collisions first), so the second half's dx wins when its step changed it. Measured on
    // lv20 t=17,573 (a mirrored dual wave pair, 3x speed portal uid 13878, box y 211..267):
    // p2's top crosses the box's bottom while p1 is 31 px below it, and the game's speed goes
    // 1.1 -> 1.3 on that tick (x steps 1.6133 -> 1.9492 from the next); with the button held
    // from t=17,560, so that p1 climbs into the box and p2 falls away, the switch comes at
    // t=17,575, the tick p1's top crosses it -- a touch, not an x trigger. The model's p2 step
    // fired and the merge kept p1's dx, so the pair ran on at 1.1 and the recorder, which
    // cannot carry x, stopped 5 px later.
    if (s.dual && cb.dx != sb.dx) c.dx = cb.dx;
    // --dualband: ...and so does a MODE portal: playerWillSwitchMode calls updateDualGround for
    // either body, after p1's, so the band the second half wrote is the pair's.
    // Taken only when that half's portal wrote it (g_bandPortalWrote); otherwise
    // p1's stands.
    if (s.dual && p2WroteBand) {
        c.bandFloor = cb.bandFloor;
        c.bandCeil = cb.bandCeil;
        c.bandRefY = cb.bandRefY;
        c.bandBranch = cb.bandBranch;
        c.bandAnim = cb.bandAnim;
    }
    // --dualcouple: ...and p2's own flip reaches p1 the same way. GD processes
    // p1 first, so p1 has already integrated when p2's gravity portal calls it:
    // the halving lands on p1's finished vy. lv16 t=13,496 (dual mini ship
    // pair, blue gravity portal uid6748): p1 is already normal and does not
    // fire; p2 fires 1->0 and sets p1 0->1, vy (-0.842 + 0.101) * 0.5 =
    // -0.3705, GD's value. The same two calls give r101's round trip when both
    // bodies start flipped (p1 fires 1->0, the partner call leaves p2 alone,
    // p2 fires and sets p1 back to 1: vy * 1/4), which is why the portal pass
    // no longer has r101's same-box skip.
    // --dualbornportal (constants.hpp): GJBaseGameLayer::flipGravity compares the two bodies' mode
    // flags as they stand when p2's portal fires -- p1 has finished its pass by then (c.mode), p2
    // has not reached a mode portal after the gravity one (s.mode2).
    if (p2FlippedGravity && s.dual && !unlinked
        && sameModeFlags(c.mode, s.mode2)) {
        const uint8_t want = (uint8_t)!c.flip2;
        if (c.flip != want) {
            c.flip = want;
            c.vy = (float)((double)c.vy * 0.5);
        }
    }
    // --ringtaplink (constants.hpp): p1's ring on a press that began this tick fired in the buttons
    // phase, after both bodies moved, so the partner call halves p2's finished vy, and compares the
    // modes as both bodies finished them. A custom level, mini balls: p2 0.012 - 0.129 = -0.117, then
    // x 0.5 = -0.0585, GD's row.
    // --rtlp2ring (constants.hpp): ...but a ring p2 fired on the same press is p2's own button, which
    // runs after p1's: a yellow, pink, red or drop ring sets vy for the gravity the partner call left,
    // so it is not halved.
    if (p1RingLate && s.dual && !unlinked && sameModeFlags(c.mode, c.mode2)) {
        const uint8_t want = (uint8_t)!c.flip;
        if (c.flip2 != want) {
            c.flip2 = want;
            if ((p2RingFresh == 11 || p2RingFresh == 12 || p2RingFresh == 35
                                || p2RingFresh == 32))
                c.vy2 = -c.vy2;
            else c.vy2 = (float)((double)c.vy2 * 0.5);
        }
    }
    // --dualorblate (speed.hpp): both bodies took a gravity ring on a press that began this tick -- p2's ring turns p2 back and its partner call turns p1 back
    if (g_dualOrbLate && p1RingKind == 2 && p2RingFresh == 13 && s.dual && !unlinked && sameModeFlags(c.mode, c.mode2)
        && c.flip != s.flip && c.flip2 != s.flip2 && s.flip != s.flip2) {
        c.flip = s.flip;
        c.vy = (float)((double)c.vy * 0.5);
        c.flip2 = s.flip2;
        c.vy2 = -c.vy2;
    }
    // Dual mode portal: GD re-MIRRORS the pair. playerWillSwitchMode
    // (0x212ef0) runs per toucher; for the SECOND one the other player is
    // already in the portal's target mode, and it then calls
    // flipGravity(!other.flip) on the toucher (0x39a1d0) -- and flipGravity
    // HALVES vy whenever the flip actually changes (mulsd 0.5 at 0x39a2dc).
    // Players are processed p1-first, so the second toucher is our half 2.
    // Measured on lv16 t=13219, ball->ship at the portal (20499,545), both
    // bodies flipped (NOT mirrored) going in:
    //   p1: vy (3.022+0.129)/2 = 1.5755, flip kept
    //   p2: vy 0.7877 = halved AGAIN, p2up 1 -> 0 = !p1.flip
    // The model shares `mode`, so "both fired this tick" is exactly "the
    // mode changed". A same-mode portal touch also re-mirrors in GD but is
    // invisible here (no mode change to see) -- left out until a level
    // needs it.
    // --dualremirror [2026-09-29]: the toucher is whichever body switched INTO the mode
    // the other already has, and since mode2 became its own field that is not always p2.
    // lv16 t=13,498 (dual, p2 a mini ship since its own touch of the same portal a few
    // ticks earlier, p1 a mini ball): p1 enters the ship portal (20499,545) alone, and GD
    // sets p1 to !p2.flip -- up 0 -> 1, vy -5.989 * 0.5 * 0.5 = -1.49725 -- and leaves p2
    // alone. The rule below flipped p2 instead.
    const bool p1IsToucher = c.mode != s.mode
                             && c.mode2 == s.mode2 && s.mode2 == c.mode;
    if (unlinked) {
        // --dualunlink: playerWillSwitchMode skips the re-mirror when the layer's +0x860 is up
    } else if (p1IsToucher) {
        const uint8_t want = c.flip2 ? 0 : 1;
        if (c.flip != want) {
            c.flip = want;
            c.vy *= 0.5f;
        }
    } else if (c.mode2 != s.mode2 && c.mode2 == c.mode) {
        const uint8_t want = c.flip ? 0 : 1;
        if (c.flip2 != want) {
            c.flip2 = want;
            c.vy2 *= 0.5f;
        }
    }
    // --repelland: the dual balls' repel, decided where GD decides it. checkRepellPlayer
    // (0x2398d0) runs from GJBaseGameLayer::update (0x238845) after both players' collisions and
    // rotation, and the taps come from the next step's processQueuedButtons -- in a model tick,
    // between the two halves' collision passes and their taps. Its gate: both balls, the same
    // upside-down byte, |y1 - y2| < half1 + half2 + 5; then if p1's +0xa0c (hitGround sets it, a
    // jump or a pad clears it) is up it flips p2, else if p2's is up it flips p1, and the flipped
    // body leaves at vy -2 upright / +2 upside down. stepOne already runs the rule inside each
    // half, reading the partner's landing from the start of the tick (p1's half) or from p1's
    // finished step (p2's half), so the one case it cannot see is p2 LANDING on this tick while p1
    // closes in: p1 has already stepped. That case is taken here, with the pre-tap reading of p2
    // (a tap leaves y where the landing put it and only turns the polarity).
    // A custom level, the dual balls at x~6,083 (custom level B, a 1.8 level, attempt 13 and its
    // neighbours): p2 lands at 134.950 on t=4,320 and taps (vy 3.426, up 0 -> 1), p1 is falling
    // 0.694 px above it at 135.644, both upright before the tap, and GD's row has p1 at up 1 with
    // vy 2.000 -- the repel, then the tap. The model left p1 upright and landed it on 4,321.
    // --repela0c: ...or the whole rule, with GD's own choice of body. checkRepellPlayer reads the
    // two +0xa0c bytes (State::hitG, as they stand after this tick's collisions and before either
    // tap) and flips p2 whenever p1's is up -- even a p2 that has just landed -- and p1 only when
    // p1's is down and p2's up. The byte outlives a repel and a walk off an edge. Same model-only check
    // custom level B, iteration 10), where both bodies land at 134.950 on t=4,321: GD
    // flips p2 (up 1, vy 2.000) and leaves p1 standing; p1 taps on 4,323 (up 1, vy 3.426); and on
    // 4,324 GD flips p1 back (up 0, vy -2.000) -- p1's byte went down with its tap, p2's has been
    // up since its landing on 4,321. The rule inside each half asked
    // for the flipped body to be airborne and read the partner's landing, so it did neither.
    // A flipped body that tapped on this tick did not tap in GD: the repel came first and
    // flipGravity took its ground away, so the tap's writes are undone here -- the polarity the
    // tap left is the repel's too; its vy, spent press and cleared byte are put back.
    if (s.dual && !d1 && !d2 && c.mode == 2 && c.mode2 == 2
        && (p1HitG || p2HitG)) {
        const uint8_t f1 = p1Tapped ? (uint8_t)!c.flip : c.flip;
        const uint8_t f2 = p2Tapped ? (uint8_t)!c.flip2 : c.flip2;
        const double halves = playerHalf(2, c.mini != 0) + playerHalf(2, c.mini2 != 0);
        if (f1 == f2 && std::fabs((double)c.y - (double)c.y2) < halves + 5.0) {
            if (p1HitG) {
                c.flip2 = (uint8_t)!f2;
                c.vy2 = c.flip2 ? 2.0f : -2.0f;
                c.grounded2 = 0;
                if (p2Tapped) {
                    c.pressSpent2 = sb.pressSpent;
                    c.holdDead2 = sb.holdDead;
                    c.hitG2 = p2HitG;
                    if (g_dualJumpBuf) c.jumpBuf2 = (uint8_t)(input ? 1 : 0);
                }
            } else {
                c.flip = (uint8_t)!f1;
                c.vy = c.flip ? 2.0f : -2.0f;
                c.grounded = 0;
                if (p1Tapped) {
                    c.pressSpent = s.pressSpent;
                    c.holdDead = s.holdDead;
                    c.hitG = p1HitG;
                    c.jumpBuf = (uint8_t)(input ? 1 : 0);
                }
            }
        }
    }
    // --p2solo: the second body took a solo portal (step.hpp, the portal pass), and the game ends
    // the dual there with player 1's copyAttributes from it (0x3a04a0): the position, the gravity
    // (an unlinked flipGravity), the seven mode bytes, the speed, the size, then vy (setYVelocity,
    // last), and +0x985 / +0x986. Not the ground byte (+0x9c1), the rotation or anything else, so
    // player 1 keeps its own; only the model's ride bookkeeping, which names a support at the old
    // position, is dropped. The speed is the pair's already (`dx` above).
    if (p2Solo && s.dual) {
        c.y = cb.y2;
        c.vy = cb.vy2;
        c.flip = cb.flip2;
        c.mode = cb.mode2;
        c.mini = cb.mini2;
        c.pressSpent = cb.pressSpent2;
        if (g_dualJumpBuf) c.jumpBuf = cb.jumpBuf2;   // --dualjumpbuf: +0x985 too
        // --dualendrot (constants.hpp): ...but a wave takes the second body's angle, as it eased this tick
        if (cb.mode2 == 4) c.rot = cb.rot2;
        c.dual = 0;
        c.onSlope = 0;
        c.slopeM = 0.f;
        c.snapObj = nullptr;
        c.snapDist = 0.f;
        // --p2xoff: ...the position includes x, which is the second body's own once they ran apart
        if (s.frame == 0 && s.dx2 != 0.f) c.xAbs = cb.xAbs;
    }
    if (!c.dual) c.dx2 = 0.f;   // --p2xoff: the dual ended, the offset with it
    // --ballscaleair: a ball whose gravity a partner call or the repel turned after its own step
    if (!p2Solo) {
        if (c.mode == 2 && c.flip != ballF1Own) c.ball728 = (uint8_t)((c.ball728 & ~3u) | 1u);
        if (c.dual && c.mode2 == 2 && c.flip2 != ballF2Own)
            c.ball728 = (uint8_t)((c.ball728 & ~12u) | 4u);
    }
    // Is the second body in OPEN AIR? Both halves take the same input, so in a
    // stretch where only one of them can touch anything, the other carries no
    // decision -- it just drifts. It still has to be simulated (it comes back),
    // but keeping it in the dedupe key at full resolution makes the frontier
    // the PRODUCT of two bodies' states, and that is what saturates the cap:
    // measured on lv16's dual section, born=4002 died=0 merged=2001 with
    // alive pinned at the 2000 cap for hundreds of ticks.
    // 26% of the 30 px columns in that section have geometry on one side only,
    // so this is worth taking. See g_dualFreeQ for what it does with it.
    c.freeHalf = 1;
    if (K.near) {
        for (const Obj* o : *K.near) {
            if (std::fabs((double)c.y2 - o->cy) < o->hh + 45.0) { c.freeHalf = 0; break; }
        }
    }
    if (c.freeHalf && K.slopes) {
        for (const Obj* sp : *K.slopes) {
            if (std::fabs((double)c.y2 - sp->cy) < sp->hh + 45.0) { c.freeHalf = 0; break; }
        }
    }
    dead = d1 || (d2 && !g_ghostOn);   // a ghost (frames.hpp g_ghostOn) is stepped with its plan, and its death ends nothing
    t_spiderFreshWarp = p1FreshWarp;
    markTouched(c, K, touchPreY(s, c, preBtnSet, preBtnY), s.action != 0,
                    input != 0 && s.action == 0, &s, input != 0);
    // The dual half used to skip this entirely, so a divergence measured in a dual section was
    // unusable even if something had managed to write it down. Applied here, after both bodies
    // are merged, for the same reason the single case applies it after stepOne: the record is
    // GD's answer to the whole transition.
    t_fixIn2 = own2 ? p2in : -1;   // a record may name the second body's button (Fixup::in2)
    if (!g_fixups.empty()) applyFixup(s, input, c, dead);
    t_fixIn2 = -1;
    return c;
}

}  // namespace dp
