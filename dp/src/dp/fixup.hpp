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
inline void applyFixup(const State& s, int input, State& c, bool& dead,
                       bool deltasToo = true) {
    if (const Fixup* f = findFixup(g_fixupKills, s, input)) {
        (void)f;
        dead = true;
        ++g_fixupHits;
        return;
    }
    if (!deltasToo) return;
    if (const Fixup* f = findFixup(g_fixupDeltas, s, input)) {
        c.y = s.y + f->dy;
        c.vy = s.vy + f->dvy;
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
        ++g_fixupHits;
        return;
    }
}

inline State stepBoth(const State& s, int input, const StepCtx& K, bool& dead) {
    bool d1 = false;
    State c = stepOne(s, input, K, d1);
    if (!s.dual) {
        dead = d1;
        markTouched(c, K, (double)s.y);
        // No fixup of either kind applies near moving geometry (nearDynObject
        // above). This exact constellation is the one that breaks the lv22
        // corridor oscillation: both breakout runs (2026-08-26 01:15 and
        // 03:15, iter ~214 each) ran it, while kills-exempt, drop-both and
        // newest-wins variants all sat pinned for 70+ minutes. The dynamics
        // are not fully understood -- what is measured is which build climbs.
        if (!g_fixups.empty() && !nearDynObject(s, K))
            applyFixup(s, input, c, dead);
        return c;
    }
    State sb = s;
    swapHalves(sb);
    bool d2 = false;
    State cb = stepOne(sb, input, K, d2);
    swapHalves(cb);
    // shared fields (x, size, speed, dual) come from the first half; the second
    // half only contributes its own body -- and `mode` / the ceiling press
    // counters are ITS fields now, not shared ones (State::mode2 says why).
    c.y2 = cb.y2;            c.vy2 = cb.vy2;
    c.mode2 = cb.mode2;
    c.ceilT2 = cb.ceilT2;    c.ceilM42 = cb.ceilM42;
    c.slopeM2 = cb.slopeM2;  c.snapDist2 = cb.snapDist2;
    c.grounded2 = cb.grounded2; c.flip2 = cb.flip2;
    c.ringHold2 = cb.ringHold2; c.onSlope2 = cb.onSlope2;
    c.slopeT2 = cb.slopeT2;
    c.snapObj2 = cb.snapObj2;   c.usedOrb2 = cb.usedOrb2;
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
