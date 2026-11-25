#pragma once
#include "dp/step_shared.hpp"

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
//   * Looked up in one place (the shadow's tick, shadow_step.hpp; the old tick's stepBoth before it left), so the search, the witness resim and --replay
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
    // A kill record's killer, as GD named it (named tail `,kuid=`); -1 = not said (--fixkillworld, constants.hpp).
    int kuid = -1;
    // File position at load. The file is append-only within a run, so a
    // larger ord is a NEWER measurement -- the conflict filter (cli.hpp)
    // keeps the newest of two records that share a key but disagree.
    int ord = 0;
};
// The load-time conflict filter (cli.hpp). Off: both no-filter builds broke
// the lv22 corridor oscillation and no filtered build did.
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
// The second body's button of the step being matched (-1: the pair shares the first body's button, or there is no pair). The old tick set it; the shadow's tick (shadow_step.hpp) only resets it to -1, since it looks records up for a single body alone.
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
// (always on: the switch --fixreach is removed)
// DELTA fixups are keyed on the player alone. Next to MOVING geometry the
// transition also depends on the world's phase, which a record from another
// worldline cannot carry: lv22 x=8,185 -- records carrying dvy=+3.426 (the
// carry of a lift that was RISING in the plans they were measured on) fired
// on a worldline whose lift was SINKING, and taught the resim an ascent GD
// does not have. One key, two truths, and no way to split them by player
// state -- so no DELTA applies while a dynamic object is within reach;
// moving-geometry trajectories come from the recordings and the trigger
// definitions instead.
// KILLS ARE NOT EXEMPT, whatever this note used to say: the tick (shadow_step.hpp, as the old tick's stepBoth did) refuses the
// whole lookup here, kills included (the measurement that
// chose that: the kills-exempt variant sat pinned in the lv22 corridor). The
// case for exempting them is still worth keeping in view: a phase-wrong kill
// costs one route point and the search replans around it, while a GATED kill
// leaves a phantom survival open -- the lv22 corridor deaths at (9,827,
// y~1,738) and (8,286, y~1,072) are dyn kills, and so were GD's deaths at
// t=2,416 and 2,842 under the ceiling of group 265, which the gate held off
// until the ceiling's own timing was right (the autonomous lag, dynamics.hpp).
// --fixreach (removed) (ON since 2026-10-24): the gate holds a record back only for a moving object that could have taken part in this step -- its box meets the box the player
// sweeps over the step, the player's own box from where it is to where the step takes it (dxF in x, vy in y; no margin) -- instead of any moving object within hw + 40 px
// of the player. The flat gate held back the kill record of a custom level's wall at x=28,498 (a mini flipped ship inside a pair of slopes, NULL killer) for ten loop
// iterations, for two small thorns 22 and 26 px clear of the player's box (hitbox trace, dmg lines at t=20,797: uid 172427 3.3x3.7 and uid 69001 8.6x7.1) that the
// kill does not involve; neither meets the swept box. The object's own sweep is only what Obj carries of it (its y step dcy, its half-height change dhh): its x step is
// not in Obj, so an object closing in on the player sideways within the tick is judged from where it stands. An object the recording holds on ONE row, at a tick the
// recording covers, never moves there and is not a reason to hold a record back (a custom level's Tidal wall at x=81,705: three thorns that never move, one of them the killer).
// One wall measured; the lv22 corridor the 40 px
// gate was chosen on (above) is the case to run before this goes on.
inline const Obj* nearDynObjectPtr(const State& s, const StepCtx& K) {
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
// The uid of the moving object that stopped a MATCHING record on this step, -1
// when nothing was stopped (no record matched, or none was near moving
// geometry, or the record fired). Written by the shadow's tick (shadow_step.hpp) on every single-body step and read by
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

// --fixkillworld: the uids of the level's moving objects (the ones a toggle can switch off), filled after the level is built. A killer that
// is not one is a still object and is always there.
inline std::unordered_set<int> g_dynKillUids;
// Kill records skipped under --fixkillworld because their killer is off in the world of the state (a census, printed by the replay).
inline std::atomic<long long> g_fixKillStale{0};
// Is the object `uid` off in the world this step is made in? A moving object that is on is among the windows the group built for the tick (the
// collect() of each bucket leaves an off one out), and the player's box overlaps whatever it killed, so it is inside them.
inline bool killerOff(int uid, const StepCtx& K) {
    if (!g_dynKillUids.count(uid)) return false;
    auto in = [&](const std::vector<const Obj*>* v) {
        if (v)
            for (const Obj* o : *v)
                if (o->uid == uid) return true;
        return false;
    };
    return !(in(K.near) || in(K.slopes) || in(K.orbs) || in(K.pads) || in(K.ports) || in(K.speeds) || in(K.nearSolid));
}
// The nearest kill record that matches the state and whose killer, when it names one, is there in this world.
inline const Fixup* findKillRecord(const State& s, int input, const StepCtx* K) {
    if (!g_fixKillWorld || !K) return findFixup(g_fixupKills, s, input);
    const std::vector<Fixup>& v = g_fixupKills;
    const float lo = s.xAbs - 1.2f;
    auto it = std::lower_bound(v.begin(), v.end(), lo, [](const Fixup& f, float x) { return f.x < x; });
    const Fixup* best = nullptr;
    double bestD = 1e18;
    bool stale = false;
    for (; it != v.end() && it->x <= s.xAbs + 1.2f; ++it)
        if (fixupMatches(*it, s, input)) {
            if (it->kuid >= 0 && killerOff(it->kuid, *K)) { stale = true; continue; }
            const double d = std::fabs((double)s.xAbs - (double)it->x) / 1.2 + std::fabs((double)s.y - (double)it->y) / 4.0
                             + std::fabs((double)s.vy - (double)it->vy) / 1.0;
            if (d < bestD) { bestD = d; best = &*it; }
        }
    if (!best && stale) g_fixKillStale.fetch_add(1, std::memory_order_relaxed);
    return best;
}
// --shadowsupply, the repair loop's fixups: applyFixup says what it did on this thread's last single-body tick (the shadow's tick calls it, shadow_step.hpp) (a kill record, a delta record and its numbers), so the new model can apply it
// to ITS body -- the delta overwrites the model's y / vy only to have the shadow's overwrite them again, and a model death is overridden by the shadow's.
inline thread_local bool t_fixKill = false, t_fixDelta = false;
inline thread_local float t_fixDy = 0.f, t_fixDvy = 0.f;
inline thread_local uint8_t t_fixG = 255;
inline void applyFixup(const State& s, int input, State& c, bool& dead,
                       bool deltasToo = true, const StepCtx* K = nullptr) {
    noteFixupCall(s);
    if (const Fixup* f = findKillRecord(s, input, K)) {
        (void)f;
        dead = true;
        t_fixKill = true;
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
        t_fixDelta = true; t_fixDy = f->dy; t_fixDvy = f->dvy; t_fixG = f->gAfter;
        noteFixupFrame(s);
        ++g_fixupHits;
        return;
    }
}



// --shadowsupply: what markTouched started from and was given on this thread's last single-body tick, so the new model can run it again at its own position.
struct TrigBefore {
    bool valid = false;
    TouchMask trig{};
    int32_t trigT = 0;
    uint16_t fireB[kTouchBits] = {};
    double preY = 0.0;
};
inline thread_local TrigBefore t_trigBefore;

// `input2` is the second body's own button (a plan's input bit 1) in a two-player level (g_twoPlayer); -1, and every call in a level
// that is not two-player, gives it the first body's, as a dual's two halves always had.

}  // namespace dp
