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
inline long long g_fixupHitFrame[4] = {0, 0, 0, 0};
inline float g_fixupRotX[8] = {0, 0, 0, 0, 0, 0, 0, 0};
inline int g_fixupRotSeen = 0;
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
// (the (frame, rev) pair of counters this used to describe lives in bands.hpp:
// step.hpp does the counting and is upstream of this header)
inline long long g_fixupCallFrame[4] = {0, 0, 0, 0};
inline float g_fixupCallRotX[8] = {0, 0, 0, 0, 0, 0, 0, 0};
inline int g_fixupCallRotSeen = 0;
// The x window is ONE TICK (1.2 px at 1.3-1.6 px/tick, still covering the
// +-1 px stair-snap offset class). It was 2.5 and a DELTA record then
// re-matched the NEXT tick's transition too: applied every tick it kept the
// state inside its own window, drifted it along, and -- being earlier in the
// file -- shadowed the KILL record 1.6 px behind it forever (lv19's saw at
// x=15,350: the single-record A/B killed the replay, the full file did not).
// Kills are also matched FIRST for the same reason: a verdict must not lose
// to a trajectory patch from the neighbouring tick.
inline bool fixupMatches(const Fixup& f, const State& s, int input) {
    if ((int)f.in != input || f.mode != s.mode || f.mini != s.mini
        || f.flip != s.flip || f.g != s.grounded)
        return false;
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
// KILLS are exempt. A phase-wrong kill costs one route point and the search
// replans around it; a GATED kill leaves a phantom survival open forever --
// the lv22 corridor deaths at (9,827, y~1,738) and (8,286, y~1,072) are dyn
// kills, and with them gated the cold run sat in that oscillation past the
// point earlier runs had learnt their way out of it.
inline bool nearDynObject(const State& s, const StepCtx& K) {
    for (const Obj* o : *K.near)
        if (o->dynObj
            && std::fabs((double)s.xAbs - o->cx) < o->hw + 40.0
            && std::fabs((double)s.y - o->cy) < o->hh + 40.0)
            return true;
    return false;
}
// One place, so a kill hit and a delta hit cannot be counted by different
// rules. Records the x of the first few rotated firings too: applyFixup has no
// tick to hand and giving it one would change a signature for a census.
inline void noteFixupFrame(const State& s) {
    const unsigned f = (unsigned)s.frame;
    if (f < 4) ++g_frameRevHit[f][s.rev ? 1 : 0];
    if (f < 4) ++g_fixupHitFrame[f];
    if (f != 0) {
        if (g_fixupRotSeen < 8) g_fixupRotX[g_fixupRotSeen] = s.xAbs;
        ++g_fixupRotSeen;
    }
}

inline void noteFixupCall(const State& s) {
    const unsigned f = (unsigned)s.frame;
    if (f < 4) ++g_frameRevCall[f][s.rev ? 1 : 0];
    if (f < 4) ++g_fixupCallFrame[f];
    if (f != 0) {
        if (g_fixupCallRotSeen < 8) g_fixupCallRotX[g_fixupCallRotSeen] = s.xAbs;
        ++g_fixupCallRotSeen;
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

inline State stepBoth(const State& s, int input, const StepCtx& K, bool& dead) {
    bool d1 = false;
    g_halfNow = 0;
    // Out-parameter, not a global: phase 1 steps the layer in parallel.
    bool p1FlippedGravity = false;
    State c = stepOne(s, input, K, d1, &p1FlippedGravity);
    if (!s.dual) {
        dead = d1;
        markTouched(c, K, (double)s.y);
        // No fixup of either kind applies near moving geometry (nearDynObject
        // above). This exact constellation is the one that breaks the lv22
        // corridor oscillation: both breakout runs (2026-08-26 01:15 and
        // 03:15, iter ~214 each) ran it, while kills-exempt, drop-both and
        // newest-wins variants all sat pinned for 70+ minutes. The dynamics
        // are not fully understood -- what is measured is which build climbs.
        const long long fxHits0 = g_fixupHits;
        if (!g_fixups.empty() && !nearDynObject(s, K))
            applyFixup(s, input, c, dead);
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
    // [2026-09-05] ...and the SECOND cross-body read: p1's gravity flip reaches
    // the partner. GD's flipGravity fires the other player with the polarity
    // inverted, from inside p1's collision pass -- so it lands BEFORE p2's own
    // update. Applying it to sb (p2's entering state) is what puts the halving
    // on the correct side of p2's integration; leaving it to p2's own portal
    // pass gives (vy + a)/2 where GD gives vy/2 - a.
    // The gate is GD's: dual, and the six mode bytes equal between the bodies
    // (wave is not compared -- so a cube and a wave still count as matching,
    // which is the asm's own shape, not a simplification).
    // Not applied when p1's firing was the same-box case: r101's skip in the
    // portal pass already models that round-trip, and this would count it twice.
    // p2's own portal on the same tick then finds itself already at the target
    // and no-ops, so nothing double-flips.
    if (!g_noDualFlip && p1FlippedGravity && s.dual
        && sameModeFlags(s.mode, s.mode2)) {
        sb.flip = (uint8_t)!sb.flip;
        sb.vy = (float)((double)sb.vy * 0.5);
    }
    bool d2 = false;
    g_halfNow = 1;
    State cb = stepOne(sb, input, K, d2);
    g_halfNow = 0;
    swapHalves(cb);
    // shared fields (x, speed, dual) come from the first half; the second half
    // only contributes its own body -- and `mode` / `mini` / the ceiling press
    // counters are ITS fields now, not shared ones (State::mode2 / mini2 say
    // why). SIZE left this list on 2026-08-28: it was the last thing a portal
    // could change for one half while the merge threw the answer away.
    c.y2 = cb.y2;            c.vy2 = cb.vy2;
    c.mode2 = cb.mode2;      c.mini2 = cb.mini2;
    c.ceilT2 = cb.ceilT2;    c.ceilM42 = cb.ceilM42;
    c.slopeM2 = cb.slopeM2;  c.snapDist2 = cb.snapDist2;
    c.grounded2 = cb.grounded2; c.flip2 = cb.flip2;
    c.ringHold2 = cb.ringHold2; c.onSlope2 = cb.onSlope2;
    c.pressSpent2 = cb.pressSpent2;
    c.slopeT2 = cb.slopeT2;
    c.rideLanded2 = cb.rideLanded2;
    // ...and the second body's velocity-limit exemption (State::boost2, added
    // 2026-09-06 with swapHalves). The third of the three sites the SIZE note
    // above is about: without this line p2's latch is written inside its own
    // stepOne and thrown away here every tick.
    c.boost2 = cb.boost2;
    c.slopeUid02 = cb.slopeUid02;  c.slopeUidNow2 = cb.slopeUidNow2;
    c.snapObj2 = cb.snapObj2;   c.usedOrb2 = cb.usedOrb2;
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
    if (c.mode != s.mode) {
        const uint8_t want = c.flip ? 0 : 1;
        if (c.flip2 != want) {
            c.flip2 = want;
            c.vy2 *= 0.5f;
        }
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
    dead = d1 || d2;
    markTouched(c, K, (double)s.y);
    // The dual half used to skip this entirely, so a divergence measured in a dual section was
    // unusable even if something had managed to write it down. Applied here, after both bodies
    // are merged, for the same reason the single case applies it after stepOne: the record is
    // GD's answer to the whole transition.
    if (!g_fixups.empty()) applyFixup(s, input, c, dead);
    return c;
}

}  // namespace dp
