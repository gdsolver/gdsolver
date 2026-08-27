#pragma once
#include "dp/triggers.hpp"

namespace dp {

// OPT-IN and unused by default. Nothing passes --obb yet -- not the driver, not
// the batch, not fidelity_diff -- so every level behaves exactly as before
// unless the flag is given by hand. See the note in hazardHit for why it is not
// on: the recorded box beats the bound but is still not the shape GD tests.
// Checked anyway: replaying the verified lv17/lv18/lv19 plans with and without
// --obb gives byte-identical traces, so those routes never graze a turned one.
//
// --obb <file>: the MOD's obb dump, `uid,id,type,cx,cy,rot,x0,y0,..,x3,y3`
// (solver.hpp writes it from GD's own getOrientedBox, for every object turned
// by something other than a multiple of 90). Four corners in order, so the two
// side lengths and the long axis fall straight out of them.
//
// The centre is NOT taken from this file: a grouped object MOVES, and the dump
// is one snapshot. objrects' cx,cy is the same point (checked on lv20 uid 424
// and 434, both exact) and the moving-geometry code already keeps it current,
// so only the SHAPE is read here and it rides along with the object.
struct ObbBox { double hw, hh, c, s; };
inline std::unordered_map<int, ObbBox> loadObb(const std::string& path) {
    std::unordered_map<int, ObbBox> m;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "obb: cannot open %s\n", path.c_str());
        return m;
    }
    std::string line;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string f[14];
        int n = 0;
        for (; n < 14 && std::getline(ss, f[n], ','); ++n) {}
        if (n < 14) continue;
        const int uid = std::atoi(f[0].c_str());
        double px[4], py[4];
        for (int i = 0; i < 4; ++i) {
            px[i] = std::atof(f[6 + 2 * i].c_str());
            py[i] = std::atof(f[7 + 2 * i].c_str());
        }
        // oob = m_shouldUseOuterOb. Without it GD kills on the AABB alone and
        // applying the box would make the model MISS deaths. Measured on every
        // level that has turned objects: it is 1 for all of them (lv16 598/598,
        // lv17 1084, lv18 666, lv19 718, lv20 1584, lv21 2096, lv22 1270), so
        // the column has never yet changed a verdict -- it is there so that the
        // day one of them reads 0, the model notices instead of guessing.
        if (n >= 15 && std::atoi(f[14].c_str()) == 0) continue;
        const double e1x = px[1] - px[0], e1y = py[1] - py[0];
        const double e2x = px[2] - px[1], e2y = py[2] - py[1];
        const double l1 = std::hypot(e1x, e1y), l2 = std::hypot(e2x, e2y);
        if (l1 < 1e-6 || l2 < 1e-6) continue;
        // The second axis is the first turned by a right angle either way, and
        // the test only ever uses |projection|, so the sign does not matter.
        m[uid] = ObbBox{l1 * 0.5, l2 * 0.5, e1x / l1, e1y / l1};
    }
    std::printf("obb: %zu turned boxes from %s\n", m.size(), path.c_str());
    return m;
}
inline std::unordered_map<int, ObbBox> g_obb;

// Parse an objrects CSV from any stream.
//
// The stream, rather than a path, is what makes the solver embeddable: the mod
// builds the very same CSV in memory out of PlayLayer's objects and feeds it in
// here, so THERE IS ONE PARSER. A second loader that walked GD's objects
// directly would be a second definition of what a level is, and the two would
// drift -- the model's fidelity is measured against this text.
inline Level loadLevelFrom(std::istream& in, const GroupTimeline* gt = nullptr,
                const std::vector<TouchTrig>* tt = nullptr,
                const std::vector<AutoTrig>* at = nullptr) {
    Level L;
    g_forceFields.clear();
    g_forceBoxes.clear();
    g_flipHeadBoxes.clear();
    g_dashStopBoxes.clear();
    g_timeWarps.clear();
    g_zoomTrigs.clear();
    std::string line;
    // header: id,type,cx,cy,w,h,groups,uid,radius,rot[,sy0,sy1,shz]
    std::getline(in, line);
    // uid -> which triggers move it, and where to. Built once so the
    // routing below can ask in O(1). Touch and autonomous controls share the
    // map; an object under both keeps the touch mask (per-state truth beats
    // the level-wide approximation, and no level has the overlap anyway).
    // The easing follows the longest hop, the same rule the chain walks use:
    // when two controllers stack on one object the long one is the one still
    // moving when the short one is done, so its curve is what the tail looks
    // like. (No level has a real conflict here -- lv20's stacks are all one
    // move deep -- but the rule has to be written down somewhere.)
    struct TrigOf {
        uint32_t mask = 0; float dx = 0, dy = 0; double dur = 0;
        int ease = 0; double erate = 2.0;
        int aAnchor = -1; float adx = 0, ady = 0; double adur = 0;
        int aease = 0; double aerate = 2.0;
        // The lockToPlayer window and WHICH controller opened it. The lock is
        // not tied to the anchor: lv20's structure is anchored on a toggle at
        // x=22,455 while the lock comes from a Move at x=24,285, so the window
        // has to carry its own trigger (fireT / fireX live there).
        double alock = 0, alockY = 0; int alockTrig = -1; int alockN = 0;
        // The ANCHOR controller's own contribution, kept apart from the sum.
        // recFire is that ONE trigger's first effect, so the recorder's
        // threshold lag has to be computed from ITS move, not from every
        // controller's offset added together. Measured on lv20 uid 13276, which
        // five triggers move: the offsets (+24, -30, +120, -120, ...) cancel to
        // -6 px over 208 ticks, which takes 14 ticks to clear 0.05 px, so the
        // recording was replayed 14 ticks late. Its real anchor is a toggle
        // that moves nothing at all -- lag 0.
        float andx = 0, andy = 0; double andur = 0;
        int anease = 0; double anerate = 2.0;
        std::vector<Dynamics::AutoPart> parts;   // one per moving controller
        std::vector<Dynamics::AutoPart> tparts;  // touch, one per BOX (trig=bit)
    };
    std::unordered_map<int, TrigOf> trigOf;
    if (tt) {
        for (size_t b = 0; b < tt->size(); ++b)
            for (const TrigCtl& c : (*tt)[b].ctl) {
                TrigOf& e = trigOf[c.uid];
                e.mask |= (uint32_t)1 << b;
                e.dx += c.dx;
                e.dy += c.dy;
                if (c.durTicks >= e.dur) { e.ease = c.ease; e.erate = c.erate; }
                e.dur = std::max(e.dur, c.durTicks);
                // ...and the same effect kept per box. One box can reach the
                // uid down several chain paths; those are one controller, so
                // they merge into one part per (box, uid).
                if (std::fabs(c.dx) > 0.001f || std::fabs(c.dy) > 0.001f) {
                    Dynamics::AutoPart* found = nullptr;
                    for (auto& p : e.tparts)
                        if (p.trig == (int)b) { found = &p; break; }
                    if (found) {
                        found->dx += c.dx;
                        found->dy += c.dy;
                        if (c.durTicks >= found->dur) {
                            found->ease = c.ease; found->erate = c.erate;
                        }
                        found->dur = std::max(found->dur, c.durTicks);
                    } else {
                        e.tparts.push_back({(int)b, c.dx, c.dy, c.durTicks,
                                            c.ease, c.erate});
                    }
                }
            }
    }
    if (at) {
        for (size_t b = 0; b < at->size(); ++b)
            for (const TrigCtl& c : (*at)[b].ctl) {
                TrigOf& e = trigOf[c.uid];
                // anchor on the earliest-crossed controller (see Dynamics)
                if (e.aAnchor < 0 || (*at)[b].cx < (*at)[(size_t)e.aAnchor].cx)
                    e.aAnchor = (int)b;
                e.adx += c.dx;
                e.ady += c.dy;
                if (c.durTicks >= e.adur) { e.aease = c.ease; e.aerate = c.erate; }
                e.adur = std::max(e.adur, c.durTicks);
                e.alockY = std::max(e.alockY, c.lockYTicks);
                if (c.lockTicks > 0.0) {
                    ++e.alockN;              // two locks on one object: unmodelled
                    if (e.alockTrig < 0) { e.alockTrig = (int)b; e.alock = c.lockTicks; }
                }
                if (std::fabs(c.dx) > 0.001f || std::fabs(c.dy) > 0.001f)
                    e.parts.push_back({(int)b, c.dx, c.dy, c.durTicks,
                                       c.ease, c.erate});
            }
        // Second pass for the anchor's own move: the anchor is only known once
        // every controller has been seen, and one trigger can reach the same
        // object down two chains (so this accumulates rather than assigns).
        for (auto& kv : trigOf) {
            TrigOf& e = kv.second;
            if (e.aAnchor < 0) continue;
            for (const TrigCtl& c : (*at)[(size_t)e.aAnchor].ctl) {
                if (c.uid != kv.first) continue;
                e.andx += c.dx;
                e.andy += c.dy;
                if (c.durTicks >= e.andur) {
                    e.anease = c.ease; e.anerate = c.erate;
                }
                e.andur = std::max(e.andur, c.durTicks);
            }
        }
    }
    // Route one object either into its static bucket or into the dynamic set.
    // An object is dynamic when the MOD recorded a timeline for its uid; a
    // grouped object that never actually moves has no rows and stays in the
    // sorted index, which is where it belongs (it is cheaper there).
    // ...UNLESS a touch trigger controls it. Those are exactly the objects that
    // never move in any recording -- that is what makes them doors -- so they
    // have to be forced into the dynamic set or the mask would have nothing to
    // act on. lv19's two door blocks have one sample each and would otherwise
    // sit in the static grid as a permanently closed wall.
    auto emit = [&](uint8_t bucket, const Obj& o) {
        const auto tit = (o.uid >= 0) ? trigOf.find(o.uid) : trigOf.end();
        // A touch trigger's group walk is TRANSITIVE, so an object whose own
        // mover is autonomous can be claimed by an unrelated touch trigger --
        // and `controlled` used to win outright, throwing the autonomous move
        // away and leaving the object frozen at its static position.
        //
        // Measured on lv22 (2026-08-11), the level's first wall:
        //   uid195 (525,57) is in group 39, and group 39 has TWO controllers:
        //     uid89  id901  Move cx=375 touch=0 dur=0.5 oy=+120  <- the lifter
        //     uid199 id1346 Rot  cx=511 touch=1 dur=0.5 ox=0 oy=0
        //   GD lifts it 57 -> 177 from t=292 (x=379, right after the x=375
        //   crossing) and the player lands on its top: 192 + half 15 = y=207.000
        //   exactly, with the dump's snapuid=195. The model kept it at 57, fell
        //   through the gap and died on the spike carpet at x=598.5.
        // A touch entry that moves the object NOWHERE and takes NO time cannot
        // be what opens it, so it must not veto a controller that does. Zero
        // offsets are still honoured when they carry a duration: lv19's third
        // touch trigger is GD's "move to target" mode, which legitimately
        // records no offset (see loadTouchTriggers).
        //
        // The autonomous side has to MOVE something too. A Rotate-derived ctl
        // entry carries a zero offset and exists only to give the uid an
        // aAnchor (see the g_rotated walk), so without this the swap fires on
        // objects whose autonomous "controller" places nothing -- and the door
        // then loses its mask, which is what --needtrig-unseen steers by.
        // Measured 2026-08-11: the un-narrowed form left lv20 stuck at x=8,457
        // for 28 iterations (baseline: CLEARED in 42).
        const bool autoMoves = (tit != trigOf.end())
                               && (tit->second.adx != 0.f || tit->second.ady != 0.f);
        const bool noopTouch = (tit != trigOf.end()) && tit->second.mask != 0
                               && tit->second.dx == 0.f && tit->second.dy == 0.f
                               && tit->second.dur == 0.0
                               && tit->second.aAnchor >= 0 && autoMoves;
        const bool controlled = (tit != trigOf.end()) && tit->second.mask != 0
                                && !noopTouch;
        const bool autoCtl = (tit != trigOf.end()) && !controlled
                             && tit->second.aAnchor >= 0;
        const auto it = (gt && o.uid >= 0) ? gt->find(o.uid) : GroupTimeline::const_iterator();
        const bool timed = gt && o.uid >= 0 && it != gt->end() && !it->second.empty();
        if (!controlled && !autoCtl && !timed) {
            switch (bucket) {
                case Dynamics::NEAR:  L.objs.push_back(o); break;
                case Dynamics::PORT:  L.portals.push_back(o); break;
                case Dynamics::PAD:   L.pads.push_back(o); break;
                case Dynamics::ORB:   L.orbs.push_back(o); break;
                case Dynamics::SPEED: L.speeds.push_back(o); break;
                default:              L.slopes.push_back(o); break;
            }
            return;
        }
        L.dyn.objs.push_back(o);
        // fixups are gated off near moving geometry (fixup.hpp); set on the
        // stored copy -- `o` is const here
        L.dyn.objs.back().dynObj = 1;
        // --dynhazpad: inflate moving hazards' boxes at the sample stage (default
        // 0). Measured on lv22 x=3,495: the bottom edge of a descending spike
        // (id392, recorded box 2.6x4.8) is at 258.19 at t=2776 while the (mini)
        // player's head is at 258.00 -- 0.19 px short. The model missed it, GD
        // killed. GD's effective box is slightly larger than the recording.
        // Provisional until the real size is measured by a sweep; only affects
        // runs that pass the flag.
        {
            std::vector<DynSample> sm =
                timed ? it->second
                      : std::vector<DynSample>{{0, (float)o.cx, (float)o.cy,
                                                (float)o.hw, (float)o.hh, 1}};
            // A recording from ANOTHER attempt can land in the same uid's row as
            // a continuation (the MOD's tick keeps counting across attempts).
            // Within one run the recording is dense tick by tick, so a jump of
            // MORE THAN 20,000 TICKS can only be a seam (even a whole level does
            // not reach 20,000 ticks).
            //
            // Left alone, the autonomous-trigger re-timing grabs that chunk:
            // lv22's uid6270 (an actual block) is at rest with on=1 at t=1 in
            // the live recording, yet the chunk at t=81,515 (another run, where
            // on=0) was picked and the block DISAPPEARS. GD lands on this block
            // at t=11,430 and the model fell straight through -- the wall at
            // x=16,165 (67% of the level) was this one thing.
            // The threshold is 30,000: even the longest level, lv19, runs
            // 20,600 ticks, so no gap that large occurs inside one run. At
            // 20,000 it cut a legitimate lv19 recording and the regression
            // failed (follow 232 -> 107).
            for (size_t k = 1; k < sm.size(); ++k)
                if (sm[k].t - sm[k - 1].t > 30000) { sm.resize(k); break; }
            if (o.type == 2 && g_dynHazPad > 0.0) {
                for (DynSample& ds : sm) {
                    ds.hw += (float)g_dynHazPad;
                    ds.hh += (float)g_dynHazPad;
                }
                // hazardHit, when obbOk, tests against the STATIC OBB (bhw/bhh)
                // and radius, not the sample's hw/hh. Unless those are inflated
                // too, this padding passes straight through with no effect
                // (measured: even with pad 1.0 the kill fixup remained)
                Obj& dob = L.dyn.objs.back();
                dob.bhw += g_dynHazPad;
                dob.bhh += g_dynHazPad;
                if (dob.radius > 0.0) dob.radius += g_dynHazPad;
            }
            L.dyn.samples.push_back(std::move(sm));
        }
        L.dyn.cur.push_back(0);
        L.dyn.bucket.push_back(bucket);
        L.dyn.baseCy.push_back((float)o.cy);
        L.dyn.baseSy0.push_back((float)o.sy0);
        L.dyn.baseSy1.push_back((float)o.sy1);
        L.dyn.on.push_back(1);
        L.dyn.prevCy.push_back((float)o.cy);
        L.dyn.trigMask.push_back(controlled ? tit->second.mask : 0u);
        L.dyn.trigDx.push_back(controlled ? tit->second.dx : 0.f);
        L.dyn.trigDy.push_back(controlled ? tit->second.dy : 0.f);
        L.dyn.trigDur.push_back(controlled ? tit->second.dur : 0.0);
        L.dyn.trigEase.push_back(controlled ? tit->second.ease : 0);
        L.dyn.trigErate.push_back(controlled ? tit->second.erate : 2.0);
        L.dyn.autoAnchor.push_back(autoCtl ? tit->second.aAnchor : -1);
        L.dyn.autoDx.push_back(autoCtl ? tit->second.adx : 0.f);
        L.dyn.autoDy.push_back(autoCtl ? tit->second.ady : 0.f);
        L.dyn.autoDur.push_back(autoCtl ? tit->second.adur : 0.0);
        L.dyn.autoEase.push_back(autoCtl ? tit->second.aease : 0);
        L.dyn.autoErate.push_back(autoCtl ? tit->second.aerate : 2.0);
        L.dyn.autoLock.push_back(autoCtl ? tit->second.alock : 0.0);
        L.dyn.autoLockTrig.push_back(autoCtl ? tit->second.alockTrig : -1);
        L.dyn.autoParts.push_back(autoCtl ? tit->second.parts
                                          : std::vector<Dynamics::AutoPart>{});
        L.dyn.touchParts.push_back(controlled ? tit->second.tparts
                                              : std::vector<Dynamics::AutoPart>{});
        // When did this object first move in the recording? That tick is what
        // the replayed trajectory is re-timed against (see applyTriggers).
        int recFire = -1;
        if (controlled || autoCtl) {
            const auto& sm = L.dyn.samples.back();
            for (size_t k = 1; k < sm.size(); ++k)
                if (std::fabs(sm[k].cx - sm[0].cx) > 0.05f
                    || std::fabs(sm[k].cy - sm[0].cy) > 0.05f
                    || std::fabs(sm[k].hw - sm[0].hw) > 0.05f
                    || std::fabs(sm[k].hh - sm[0].hh) > 0.05f
                    || sm[k].on != sm[0].on) { recFire = sm[k].t; break; }
        }
        L.dyn.trigRecFire.push_back(recFire);
        // recAuto: is this object's recorded motion a WORLDLINE fact rather
        // than the state's own touch? Undecidable from the summed (mask, dx,
        // dy) -- an earlier direction test was tried and reverted (2026-08-26):
        // yellow (+30 up) and red (-840 down) boxes drive the same spikes and
        // the sum points down like the recorded descent. Per box it IS
        // decidable: opposing dy signs across boxes mean no single box's chain
        // can be "the" recording (lv22's switch band, whose descent starter is
        // a collision trigger the dump cannot even time), while a door -- one
        // box, one direction -- keeps the mask-gated path (lv19/20 unchanged).
        {
            uint8_t ra = 0;
            if (controlled) {
                bool up = false, down = false;
                for (const auto& p : tit->second.tparts) {
                    if (p.dy > 0.01f) up = true;
                    if (p.dy < -0.01f) down = true;
                }
                ra = (up && down) ? 1 : 0;
            }
            L.dyn.recAuto.push_back(ra);
        }
        // ...and how late that first ROW is against the true start. The 0.05
        // above is not a choice made here: it is grouptrace's own threshold
        // (grouptrace.hpp kEps), so the lag is computable from the curve.
        int lag = autoCtl
            ? recordLag(tit->second.andx, tit->second.andy, tit->second.andur,
                        tit->second.anease, tit->second.anerate)
            : 0;
        // ...but that formula only knows about the TRANSLATION, while the
        // recorder writes a row when ANY channel moves -- cx, cy, w, h (a turned
        // or scaled object) or the toggle. An object that is being rotated trips
        // the threshold on its bounding box long before its 15 px slide does, so
        // its first row is EARLIER than the translation predicts and the lag
        // comes out too large. Measured on lv22's spider portal uid 434
        // (autoD=(-15,0), dur 120, ease 1): the formula says 5, the true lag is
        // 3, and applyTriggers' shift then reads the recording TWO TICKS IN THE
        // PAST -- 3.4 degrees of rotation, which is fatal for an OBB test.
        //
        // The recording itself says what the lag really is: at its first row the
        // object has travelled `moved`, and the analytic curve reaches that same
        // displacement after `n` ticks. Invert it. Only ever SHRINKS the lag
        // (the row cannot be later than the translation predicts), so an object
        // whose bound is not being touched keeps the old value exactly.
        if (autoCtl && lag > 1 && recFire >= 0) {
            const auto& sm = L.dyn.samples.back();
            const double full = std::hypot((double)tit->second.andx,
                                           (double)tit->second.andy);
            const double dur = tit->second.andur;
            if (full > 0.5 && dur > 0.0) {
                double moved = 0.0;
                for (const DynSample& q : sm)
                    if (q.t == recFire) {
                        moved = std::hypot((double)q.cx - (double)sm[0].cx,
                                           (double)q.cy - (double)sm[0].cy);
                        break;
                    }
                for (int n = 1; n <= lag; ++n) {
                    const double e = gdEase(tit->second.anease,
                                            tit->second.anerate, (double)n / dur);
                    if (full * e >= moved - 1e-6) { lag = n; break; }
                }
            }
        }
        L.dyn.recLag.push_back(lag);
        // Can the formula stand in for this object's recording? Replay it over
        // every recorded sample and see. An object that is really moved by
        // something else -- a rotation about a centre group, a lockToPlayer
        // move, a move-to-target with no offset -- fails here without needing
        // to be recognised by name. With no recording there is nothing to
        // check against, and the formula is what the analytic path uses anyway.
        // A LOCKED object is the exception that has to skip the check rather
        // than fail it: its recording followed the recorded run's player, so
        // comparing the formula to it would only measure how differently that
        // run moved. For those the formula wins by construction -- there is no
        // other source that can be right for this plan.
        uint8_t closed = 0;
        if (autoCtl && tit->second.alockN == 1 && tit->second.alockY <= 0.0) {
            closed = 1;
        // The gate is "does it have a move at all", NOT "does the sum of its
        // moves come to something". An OSCILLATING group sums to zero -- lv20's
        // group 54 is -240 +240 -240 +240 -- and testing the sum threw every one
        // of them back onto the recording. That was the wall at x=32,349: a
        // 4.8x2.6 spike (uid 18425) 26 px below where the model had it.
        } else if (autoCtl && tit->second.alockN == 0
            && tit->second.alockY <= 0.0
            && !tit->second.parts.empty()) {
            closed = 1;
            // With ONE controller the recording can be checked right here: its
            // origin is recFire minus the recorder's lag and nothing else
            // enters. With several, each one starts at its own crossing and the
            // crossings are not resolved yet at load time (they come from the
            // solve's own x), so there is nothing to check against -- those
            // stand on the offline measurement instead (all 384 multi-
            // controller objects in lv19+lv20 match to 0.002 px) plus the
            // structural exclusion of rotated objects above.
            if (recFire >= 0 && tit->second.parts.size() <= 1) {
                const auto& sm = L.dyn.samples.back();
                const double bx = sm[0].cx, by = sm[0].cy;
                const int t0r = recFire - lag;
                for (const DynSample& s : sm) {
                    const double e = gdEase(tit->second.aease, tit->second.aerate,
                                            (double)(s.t - t0r) / tit->second.adur);
                    if (std::fabs((double)s.cx - (bx + tit->second.adx * e)) > 0.5
                        || std::fabs((double)s.cy - (by + tit->second.ady * e)) > 0.5) {
                        closed = 0;
                        break;
                    }
                }
            }
        }
        // A rotation moves its group about a centre, which no offset describes.
        // Those objects keep their recording (the loader has always left 1346
        // roots out; this is the same rule seen from the object's side).
        if (closed && g_rotated.count(o.uid)) closed = 0;
        L.dyn.autoClosed.push_back(closed);
        if (controlled) L.dyn.anyTrig = true;
        if (autoCtl) L.dyn.anyAuto = true;
        // the level's extent has to cover where the object GOES, not where it
        // was parked at load -- lv22's intro blocks start below the floor
        if (timed)
            for (const DynSample& s : it->second) {
                L.maxX = std::max(L.maxX, (double)s.cx);
                if (bucket == Dynamics::NEAR)
                    L.maxY = std::max(L.maxY, (double)s.cy + (double)s.hh);
            }
        if (controlled && bucket == Dynamics::NEAR)
            L.maxY = std::max(L.maxY,
                              (double)o.cy + (double)tit->second.dy + (double)o.hh);
        if (autoCtl && bucket == Dynamics::NEAR)
            L.maxY = std::max(L.maxY,
                              (double)o.cy + (double)tit->second.ady + (double)o.hh);
    };
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        // 21 fields now: ...,sup,w0,h0,tpy,tpg,tpix,tpiy (see the objrects
        // header in solver.hpp). An older dump simply leaves the extra ones
        // empty, which reads as "not known" and falls back to the previous
        // behaviour -- a level with no teleport is unaffected either way.
        // 26 = ...,tpiy,tw,zoom,zdur,zease,zrate (added 2026-08-14).
        // 26 -> 28 (mvdir, gnddir added: id 2900's travel / gravity direction)
        // 28 -> 34 (optp1, optp2, flipx, flipy, nofx, notouch). Only flipy
        // (f[31]) is used so far -- the blue pad's gate (objFacingDown).
        // 34 -> 36 (tpex, tpey: the real position of the teleport exit half.
        //           2026-08-18)
        // 36 -> 41 (after dis=36: editvel, vmodx, vmody, ovrvel -- id 2900's
        //           velocity change. 2026-08-19. dis itself is still unread by
        //           leveldp)
        std::string f[41];
        // 41, not 28. The bound was left at 26 when mvdir/gnddir were added
        // (2026-08-15), so f[26]/f[27] were never filled and BOTH of them read
        // as "column absent" -- the whole id-2900 direction change was dead
        // code from the day it was written, silently falling back to the old
        // rot/90 guess. Found by the gravity after lv22's second rotation
        // stepping the wrong way with mvdir sitting right there in the file.
        // WHEN A COLUMN IS ADDED, RAISE THIS BOUND TOO. Forgetting it throws no
        // exception and only ever shows up as "that rule was dead from the
        // start".
        for (int i = 0; i < 41 && std::getline(ss, f[i], ','); ++i) {}
        if (f[5].empty()) continue;
        const int type = std::atoi(f[1].c_str());
        Obj o{std::atof(f[2].c_str()), std::atof(f[3].c_str()),
              std::atof(f[4].c_str()) / 2, std::atof(f[5].c_str()) / 2,
              (uint8_t)type, std::atoi(f[0].c_str()),
              f[7].empty() ? -1 : std::atoi(f[7].c_str()),
              f[8].empty() ? 0.0 : std::atof(f[8].c_str())};
        // A saw's rect is NOT its bounding circle: id=88 is 44x85 with radius
        // 32.3, so the rect is 10 px narrower than the circle on x. Every x
        // window in this file (the near-slice, the prefilter) is built from
        // hw/hh, and a window narrower than the hitbox silently MISSES
        // collisions -- which is the one error class that produces plans GD
        // kills. Normalising the box to the circle here makes all of them
        // correct at once, and hazardHit still does the round test.
        // ...and m_objectRadius is the UNSCALED radius. An object placed at a
        // scale other than 1 keeps that member but its hitbox grows with it,
        // and objrects' w,h (getObjectRect) already carries the scale, so the
        // factor comes back as w / m_width. lv20 has both cases of the SAME
        // object id: uid 650 sits at scale 1 (48 / 48) and uid 1842 at 1.2
        // (57.6 / 48), and using the raw 24 for the second one is lv20's wall
        // at x=2,703 -- model and GD agreed to the last digit for all 2,084
        // ticks and only the verdict differed.
        //
        // Measured by stepping ONE tick from an injected state and reading the
        // position back out of the trace, so the contact point is known rather
        // than assumed (2026-08-05). Offsets from each hazard's centre, wave:
        //   uid 1842  dx -35.0 lived, -33.7 lived, -33.4 DIED, -32.7 DIED
        //             dy +34.05 lived, +33.05 DIED
        //             dx -25 / dy +25 (diagonal) DIED
        //   uid 650   dx -27.42 / dy +17.35 lived
        // All eight agree with a CIRCLE of 24 x scale and disagree with the raw
        // 24 (which clears three of the deaths by 4-10 px). A rect of the same
        // w,h was tried too and is wrong in the other direction: it kills uid
        // 650's case, where GD lives.
        // The player's half is not the wobble -- GD reports the wave's own rect
        // as exactly 10x10 (cfg hitboxtrace=1, `pbox:` lines), i.e. the 5.0 the
        // model already uses.
        // Unscaled objects are untouched, which is every saw in lv11-19.
        if (o.radius > 0.0) {
            const double wUn = f[15].empty() ? 0.0 : std::atof(f[15].c_str());
            if (wUn > 1.0) o.radius *= (std::atof(f[4].c_str()) / wUn);
            o.hw = o.hh = o.radius;
        }
        o.rot = f[9].empty() ? 0.0 : std::atof(f[9].c_str());
        o.flipY = (uint8_t)(f[31] == "1" ? 1 : 0);
        // Recover the real box from the bound when the object is turned by
        // something other than a multiple of 90 (see Obj::oriented). Multiples
        // of 90 are left alone: there the bound IS the shape, so nothing that
        // already works can move.
        // OFF BY DEFAULT (--oriented turns it on). The measurement behind it is
        // right -- lv18's 51-degree portal really is 34x86 inside an 88x80
        // bound, and GD really does fire it 48 px later than the bound says
        // (two injected points, see orientedHit) -- but switching it on cost
        // lv16, which was CLEARED and now dies at x=11,944 (and takes 93 min
        // instead of 13). Something in lv16 NEEDS the wide bound, so the rule
        // is incomplete, not merely unpolished: find out what lv16 fires on the
        // bound before making this the default again. lv18 moved too (27,713 ->
        // 27,388), so it is not a fix for that level on its own either.
        // w0,h0 = GD's own m_width / m_height, i.e. the size BEFORE rotation.
        // Added to objrects 2026-08-04 for exactly this. Deriving it back out of
        // the bound and the angle was tried first and gave the SAME numbers
        // (34x86 / 51x56 / 31x90 on lv18's stack, 25x75 on lv16's) -- the
        // inversion was right, the mistake was not believing it and filtering on
        // "looks like 34x86". These really are three differently sized portals
        // stacked at one x, and lv16's really are scaled to 25x75.
        const double w0 = f[15].empty() ? 0.0 : std::atof(f[15].c_str());
        const double h0 = f[16].empty() ? 0.0 : std::atof(f[16].c_str());
        if (type == 28) {
            o.tpY = f[17].empty() ? 0.0 : std::atof(f[17].c_str());
            o.tpGrav = f[18].empty() ? 0 : (uint8_t)std::atoi(f[18].c_str());
            o.tpEx = f[34].empty() ? 0.0 : std::atof(f[34].c_str());
            o.tpEy = f[35].empty() ? 0.0 : std::atof(f[35].c_str());
            const bool igx = (!f[19].empty() && std::atoi(f[19].c_str()));
            const bool igy = (!f[20].empty() && std::atoi(f[20].c_str()));
            // m_ignoreX keeps the PLAYER's x -- which is what this model does
            // for every teleport anyway (x is the DP's clock), so it IS
            // modelled now. The two remaining inexpressible shapes stay loud:
            // an x-moving exit (non-747, exit x away from the portal, ignoreX
            // off) and m_ignoreY (keep player y).
            if (o.id != 747 && (o.tpEx != 0.0 || o.tpEy != 0.0)
                && !igx && std::fabs(o.tpEx - o.cx) > 0.5)
                std::printf("teleport: uid %d at (%.0f,%.0f) moves x to %.0f "
                            "- NOT modelled\n", o.uid, o.cx, o.cy, o.tpEx);
            if (igy)
                std::printf("teleport: uid %d at (%.0f,%.0f) uses ignoreY "
                            "- NOT modelled\n", o.uid, o.cx, o.cy);
            // A non-747 teleport resolves its target from the linked exit half
            // (m_orangePortal), NOT the tpy closed formula -- an old dump
            // without the tpex/tpey columns plans against a target measured
            // wrong on lv22 (705 saved vs 1905 real). Refresh objrects.
            if (o.id != 747 && o.tpEx == 0.0 && o.tpEy == 0.0)
                std::printf("teleport: uid %d id %d at (%.0f,%.0f) has NO exit "
                            "columns (old objrects dump) - target unreliable, "
                            "refresh objrects\n", o.uid, o.id, o.cx, o.cy);
            if (o.tpGrav)
                std::printf("teleport: uid %d at (%.0f,%.0f) sets gravity mode %d\n",
                            o.uid, o.cx, o.cy, (int)o.tpGrav);
        }
        if (g_oriented && o.radius == 0.0 && !o.slope && w0 > 1.0 && h0 > 1.0) {
            const double m = std::fabs(std::fmod(o.rot, 90.0));
            if (m > 0.5 && m < 89.5) {
                const double th = o.rot * 3.14159265358979 / 180.0;
                {
                    // [2026-08-19] w0/h0 are the raw sprite dimensions, and for
                    // a scaled object they are not the real box. The factor to
                    // the real size is recovered from the bound (w,h) -- the
                    // ratio of o.hw to the predicted half-bound
                    // (w0/2)|c| + (h0/2)|s|. Same trap and same formula the
                    // rings (the ORB branch below) hit first.
                    // Measured on lv21 uid24194 (id111, rot=43, raw 34x86,
                    // bound 167x172): k = 2.0 exactly. The 17x43 real box built
                    // from the raw size did not reach GD's firing point
                    // (t=18,087, x54.07 from the centre).
                    // Within 1% is dump rounding = snapped to 1 (only a scaled
                    // object is allowed to move).
                    const double ccL = std::fabs(std::cos(th));
                    const double ssL = std::fabs(std::sin(th));
                    const double denomL = (w0 * 0.5) * ccL + (h0 * 0.5) * ssL;
                    double kSc = (denomL > 1.0) ? o.hw / denomL : 1.0;
                    if (std::fabs(kSc - 1.0) < 0.01) kSc = 1.0;
                    const double W = w0 * kSc, H = h0 * kSc;
                    // The old rule here refused to believe any inversion that
                    // did not land on 34x86 -- see git for the three stacked
                    // lv18 portals (k = 1) and lv16's (12,122.9,556.7), bound
                    // 75.37x64.31 rot -54, which now scales by k = 0.84. That
                    // lv16 portal shrinking is exactly what once killed a
                    // CLEARED lv16 at x=11,944, so this change is condition-
                    // class: the 4th layer (lv16/22 cold) must pass, a green
                    // census is not enough.
                    if (W > 1.0 && H > 1.0) {
                        o.oriented = 1;
                        o.ohw = W * 0.5; o.ohh = H * 0.5;
                        // cocos2d's getRotation() is CLOCKWISE-positive, so the
                        // world->local transform takes -rot. Getting this sign
                        // wrong is what made the box test still fire early even
                        // after the shape was right -- and it looks correct on a
                        // point near the object's centre, which is why it
                        // survived three rounds of "fix the shape".
                        // Checked against the three GD measurements on lv18's
                        // portal (offsets from its centre, wave half 5,
                        // |local_x| vs the 17+7.03 threshold):
                        //   dx=+2.0 dy=27.5 -> 20.1  fires      (GD: fires)
                        //   dx=-2.7 dy=33.8 -> 27.97 does not   (GD: does not)
                        //   dx=-46  dy=13.4 -> 39.3  does not   (GD: does not)
                        // With +rot the same three come out 22.6 / 24.6 / 18.5,
                        // i.e. "fires" for all three. That last one is the tick
                        // the model turned into a cube 48 px early.
                        const double thc = -th;
                        o.rc = std::cos(thc); o.rs = std::sin(thc);
                    }
                    // MEASURED, and the 34x86 filter is too tight to fix lv18.
                    // The three portals stacked at x=27,756 all sit at rot 51
                    // and all bound differently, so they invert differently:
                    //   id=12  type 6  bound 88.23x80.54 -> 33.99x86.01  USED
                    //   id=99  type 17 bound 89.45x80.73 -> 31.01x89.99  rejected
                    //   id=202 type 20 bound 75.61x74.88 -> 51.01x55.99  rejected
                    // Fixing only the cube portal is not enough: the size and
                    // speed portals still fire 48 px early on their bounds, and
                    // the DP still returns SOLVED on a plan GD kills.
                    // Widening the filter to a plausible RANGE does not separate
                    // the cases either -- lv16's (25x75) sits inside any range
                    // that admits these three. **Do not guess another filter.**
                    // Measure lv16's portal at (12,122.9,556.7) in GD the way
                    // lv18's was measured (hold the player at a fixed y through
                    // it and find the x where it fires) and let the rule follow
                    // from the two data points.
                }
            }
        }
        L.maxX = std::max(L.maxX, o.cx);
        // 21 is a ONE-WAY PLATFORM (see Obj::oneway). The loader dropped it
        // entirely, so the four-block column at lv13 x=1665 -- the only footing
        // in a 450 px spike field -- did not exist for the model, and the search
        // could not get past x=1724 no matter how it flew. Everything downstream
        // splits objs by `type == 0` (solid) vs anything else (hazard), so it is
        // stored AS 0 with the oneway flag set; left as 21 it would have killed
        // on contact instead of holding the player up. Only lv13 (32) and lv18
        // (8) have any, both id 143, so lv1-12 cannot be affected by this.
        if (type == 21) { o.type = 0; o.oneway = 1; }
        // HAZARDS ONLY, for now. A turned SOLID has the same bound problem, but
        // the solid branches also LAND on and ride their objects, and a landing
        // resolved against a slanted face is a different piece of work; the
        // hazard branch only ever asks "touching?", which the box answers on
        // its own. One variable per regression.
        if ((type == 2 || type == 47) && o.radius == 0.0 && o.uid >= 0) {
            const auto it = g_obb.find(o.uid);
            if (it != g_obb.end()) {
                o.obbOk = 1;
                o.bhw = it->second.hw; o.bhh = it->second.hh;
                o.bc = it->second.c;   o.bs = it->second.s;
            }
        }
        if (o.type == 0 || type == 2 || type == 47) {
            L.maxY = std::max(L.maxY, o.cy + o.hh);
            emit(Dynamics::NEAR, o);
        }
        // type 3 = InverseGravityPortal. It was MISSING here, so every blue
        // gravity portal in the game was invisible to the model: 109 of them
        // across lv4..lv21, including the one at lv4 x=8775 that produced the
        // "semantics gap" wall at x=9,326. The old code expected both gravity
        // portals to be type 4 and told them apart by objectID (10 vs 11), but
        // GD reports the inverse one as its own GameObjectType, so the
        // `id == 11` branch below was dead code that never once ran.
        // 17 = RegularSize, 18 = MiniSize. Same box-overlap rule as the rest;
        // they only change the player's half extent (see kMiniHalf).
        else if (type == 3 || type == 4 || type == 5 || type == 6 || type == 16
        // 19 = UFO portal. Measured on lv12 (portal box x[6928,6962]): GD
        // switches at x = 6914.28 and not at 6912.98, i.e. left edge - 15 --
        // the same box-overlap rule as every other portal. vy halves too:
        // -2.160 becomes -1.188 = (-2.160 - 0.216) / 2, i.e. the tick's cube
        // gravity applied first and then halved. Nothing special is needed.
        // 23 = dual portal (splits into two players), 24 = solo (back to one)
                 || type == 17 || type == 18 || type == 19
        // 26 = wave portal. lv17 (2), lv18 (3), lv19 (2), lv20 (3), lv21 (2)
        // have them; lv1-16 have none, so nothing already cleared can move.
                 || type == 26
        // 27 = ROBOT portal. It was missing here for a whole session while
        // `wantMode` above already knew about it, so the model happily planned
        // lv19's robot section as a CUBE: measured against GD at t=1,470, the
        // model stepped vy by -0.216 per tick where GD stepped -0.194 (= 0.216
        // x 0.9, the robot's gravity scale), and by t=1,514 it was 6 px low and
        // landing where GD was still falling.
        // The lesson is the same one the ball, the UFO and the wave each taught
        // once: a new mode has to be added in BOTH places, and the frontier
        // print (`cube/ship/ball/ufo=`) does not show the new one, so nothing
        // says out loud that it never fired.
                 || type == 27
        // 33 = SPIDER portal (lv21 x=10,695 and lv22 x=975 are the first two).
                 || type == 33
        // 41 = SWING portal (lv22 x=4,641 / x=9,315; nowhere else in lv1-21,
        // checked with the census -- adding it cannot move a cleared level).
                 || type == 41
        // 28 = TELEPORT portal (lv20 x20, lv21 x2, lv22 x1; none before). It
        // rides in this bucket because the firing rule is the same box overlap
        // every other portal uses -- measured on a full nodeath pass of lv20:
        // all four portals the player's box entered fired, the sixteen it
        // missed did not, nearest miss 3.96 px. What it does is not a mode
        // change though; see Obj::tpY and the teleport block in stepOne.
                 || type == 28
                 || type == 23 || type == 24)
            emit(Dynamics::PORT, o);   // 16 = ball
        // 9 = pink pad, 12 = pink orb (lv12 onward)
        // 34 = RED pad (see kPadRed). Only lv22 has one, so adding it cannot
        // move a level that already clears.
        else if (type == 8 || type == 9 || type == 10 || type == 34)
            emit(Dynamics::PAD, o);
        // 29 = green ring, 35 = red ring. Neither exists anywhere in lv1-18
        // (checked with scripts/mechanic_census.ps1), so adding them here
        // cannot move a level that already clears. lv22 needs the red one at
        // x=375: it is the only way over the spike carpet that runs from
        // x=435 to x=945, and without it the cold DP dies at x=458.
        // 32 = drop ring (lv21 only, 4 of them)
        // 37 = dash ring, 38 = gravity dash ring (lv21 has 7+2, lv22 3)
        // 43 = SPIDER ORB (GameObjectType::SpiderOrb, id 3004). lv22 has the
        // only two in lv1-22 (x=13,965 and x=14,325, both cy=975), so adding it
        // cannot move a level that already clears. It fires like every other
        // orb (tap while the box overlaps) and teleports like the spider --
        // see the type 43 branch in stepOne.
        else if (type == 11 || type == 12 || type == 13 || type == 29
                 || type == 32 || type == 35 || type == 37 || type == 38
                 || type == 43) {
            // Firing box of a rotated ring. The 45-degree dash ring at lv22
            // x=13,333 (1704, 1.2x) is the only non-multiple-of-90 one in the
            // level. A press-tick sweep pins the boundary to one tick
            // (press<=9984 fires / >=9985 does not): neither the AABB nor a
            // fitted circle but the ROTATED BOX -- the local-axis SAT threshold
            // 18+15*sqrt(2)=39.21 is bracketed exactly by the firing side
            // v=36.77 and the non-firing side v=39.88 (findings 2026-08-13).
            // The scale is recovered from the bound: w0/h0 are the raw sprite
            // size, which for this ring is 15 and would drop the firing side
            // (the real size is 18 at 1.2x). The portal's oriented path (above)
            // is a separate thing with its own tuning history, so it is left
            // alone. A rotation by a multiple of 90 does not set oriented and
            // behaves as before.
            const double om = std::fabs(std::fmod(o.rot, 90.0));
            if (om > 0.5 && om < 89.5 && w0 > 1.0 && h0 > 1.0) {
                const double th = o.rot * 3.14159265358979 / 180.0;
                const double cc = std::fabs(std::cos(th));
                const double ssn = std::fabs(std::sin(th));
                const double denom = (w0 * 0.5) * cc + (h0 * 0.5) * ssn;
                if (denom > 1.0) {
                    const double k = o.hw / denom;
                    o.oriented = 1;
                    o.ohw = k * w0 * 0.5;
                    o.ohh = k * h0 * 0.5;
                    const double thc = -th;
                    o.rc = std::cos(thc);
                    o.rs = std::sin(thc);
                }
            }
            emit(Dynamics::ORB, o);  // 13 = gravity
        }
        // Speed portals live under the generic "modifier" type 20 (which is
        // otherwise all triggers), so they are picked out by object id.
        else if (type == 20 && isSpeedId(o.id)) emit(Dynamics::SPEED, o);
        // GAMEPLAY ROTATION (id 2900). Also a "modifier", also picked by id.
        // Not emitted into any collider bucket -- it turns the frame, it does
        // not exist for the player to touch (see RotTrig).
        else if (type == 20 && o.id == 2900) {
            // [2026-08-15 revised] 2900 is not a "rotation": it SETS THE
            // DIRECTION AS AN ABSOLUTE VALUE. gnddir = the new travel direction
            // (1=up 2=down 3=left 4=right). rot alone cannot tell a +X-travel
            // one from a -X-travel one (= reverse) at the same orot=0, and the
            // code relied on a one-sample guess of "same frame means toggle".
            // Confirmed to agree on all 20 firings in lv22.
            //   4 -> frame 0 / rev 0     3 -> frame 0 / rev 1
            //   2 -> frame 1             1 -> frame 3
            const int gd = f[27].empty() ? 0 : std::atoi(f[27].c_str());
            int fr = ((int)std::lround(o.rot / 90.0)) & 3;
            int rv = -1;   // -1 = column absent (old export); follow rot
            switch (gd) {
                case 4: fr = 0; rv = 0; break;
                case 3: fr = 0; rv = 1; break;
                case 2: fr = 1; rv = 0; break;
                case 1: fr = 3; rv = 0; break;
                default: break;
            }
            // mvdir = the new GRAVITY DIRECTION (1=up 2=down 3=left 4=right).
            // It has been in the export since 2026-08-15 but was never read.
            // The model's flip means "local -v is down", so it is 1 only when
            // mvdir points along that frame's local +v. From fromFrame:
            //   frame 0: +v = +Y (up=1)     frame 1: +v = +X (right=4)
            //   frame 2: +v = -Y (down=2)   frame 3: +v = -X (left=3)
            static const int kUpFor[4] = {1, 4, 2, 3};
            const int mv = f[26].empty() ? 0 : std::atoi(f[26].c_str());
            RotTrig rt{o.cx, o.cy, fr};
            rt.setRev = rv;
            if (mv >= 1 && mv <= 4) rt.setFlip = (mv == kUpFor[fr & 3]) ? 1 : 0;
            rt.uid = o.uid;
            // Velocity change (editvel=169 / vmody=583 / ovrvel=584)
            // [2026-08-19]. The MOD dumps the member values after parsing, so
            // the default 0.0 when 583 is absent is also GD's own value as is.
            // The effective multiplier is folded in here (the applyRotation
            // side only multiplies by vmodY).
            const int ev = f[37].empty() ? 0 : std::atoi(f[37].c_str());
            if (ev)
                rt.vmodY = f[39].empty() ? 0.0f : (float)std::atof(f[39].c_str());
            rt.ovrVel = f[40].empty() ? 0 : (uint8_t)std::atoi(f[40].c_str());
            g_rotTrig.push_back(rt);
        }
        // [correction 2026-08-18] id 2899 is NOT REVERSE -- it is an Options
        // trigger. It is GD's GameOptionsTrigger, and all 10 in lv22 touch
        // m_disableP1Controls (objrects' optp1: On=1 / Off=-1 / unchanged=0).
        // Reverse itself is handled by a SAME-FRAME 2900 (RotTrig::setRev), and
        // the g_revTrig that used to be pushed here was never read (= wrong,
        // but never visible in behaviour). For the effect and the window see
        // g_ctrlWin / ctrlOffAt. The firing gate is unresolved, so nothing is
        // pushed here.
        else if (type == 25 && !f[10].empty()) {
            o.slope = 1;
            o.sy0 = std::atof(f[10].c_str());
            o.sy1 = std::atof(f[11].c_str());
            o.slopeHazard = f[12].empty() ? 0 : (uint8_t)std::atoi(f[12].c_str());
            o.slopeDir = f[13].empty() ? 0 : (uint8_t)std::atoi(f[13].c_str());
            emit(Dynamics::SLOPE, o);
        }
        // FLIP-ON-HEAD-HIT (id 2866): a modifier, picked by id like 2900 above.
        // Not a collider, so it is not emitted into any bucket.
        // CAMERA ZOOM (id 1913): the invisible ceiling is 270 / zoom.
        else if (o.id == 1913) {
            const double zm = f[22].empty() ? 0.0 : std::atof(f[22].c_str());
            if (zm > 0.0) {
                g_zoomTrigs.push_back({o.cx, zm,
                                       (f[23].empty() ? 0.0
                                          : std::atof(f[23].c_str())) * 240.0,
                                       f[25].empty() ? 2.0
                                          : std::atof(f[25].c_str()),
                                       f[24].empty() ? 0
                                          : std::atoi(f[24].c_str())});
                std::printf("zoom: uid %d at x=%.0f -> %.6f over %.2fs\n",
                            o.uid, o.cx, zm,
                            f[23].empty() ? 0.0 : std::atof(f[23].c_str()));
            }
        }
        // TIME WARP (id 1935): the `tw` column carries m_timeWarpTimeMod.
        else if (o.id == 1935) {
            const double tw = f[21].empty() ? 0.0 : std::atof(f[21].c_str());
            if (tw <= 0.0) {
                std::printf("timewarp: uid %d at x=%.0f has NO tw column - "
                            "objrects is older than the 2026-08-14 dump, "
                            "IGNORED\n", o.uid, o.cx);
            } else {
                g_timeWarps.push_back({o.cx, tw});
                std::printf("timewarp: uid %d at x=%.0f mod=%.4f\n",
                            o.uid, o.cx, tw);
            }
        }
        else if (o.id == 2866) {
            g_flipHeadBoxes.push_back({o.cx, o.cy, o.hw, o.hh});
            std::printf("fliphead: uid %d at (%.0f,%.0f) %.1fx%.1f\n",
                        o.uid, o.cx, o.cy, o.hw * 2.0, o.hh * 2.0);
        }
        else if (o.id == 1829) {
            g_dashStopBoxes.push_back({o.cx, o.cy, o.hw, o.hh});
            std::printf("dashstop: uid %d at (%.0f,%.0f) %.1fx%.1f\n",
                        o.uid, o.cx, o.cy, o.hw * 2.0, o.hh * 2.0);
        }
        // FORCE FIELD (id 3645): a circular pusher, not a collider -- see
        // forceFieldAcc at the top. Not stored in L; stepOne reads the global.
        // The radius block above has already scaled o.radius by w/w0.
        else if (o.id == 3645) {
            double rm = std::fmod(std::fabs(o.rot), 360.0);
            if (rm > 180.0) rm = 360.0 - rm;
            if (std::fabs(rm - 90.0) < 45.0)
                std::printf("forcefield: uid %d at (%.0f,%.0f) rot %.0f is "
                            "SIDEWAYS - not measured, object IGNORED\n",
                            o.uid, o.cx, o.cy, o.rot);
            else {
                g_forceFields.push_back(
                    {o.cx, o.cy, o.radius, (rm >= 90.0) ? -1 : +1});
                std::printf("forcefield: uid %d at (%.0f,%.0f) R=%.1f push %s\n",
                            o.uid, o.cx, o.cy, o.radius,
                            (rm >= 90.0) ? "down" : "up (UNMEASURED mirror)");
            }
        }
        // FORCE BOX (id 2069): the AABB version of 3645, a flat push (measured
        // at kFF2069's declaration). All 13 in lv22 are rot=0 (upward), so the
        // orientation is not read. The strength is per-instance (the table at
        // kFF2069's declaration).
        else if (o.id == 2069) {
            double k = kFF2069;
            // [2026-08-22 r108b] 17701 has DIFFERENT VALUES FOR SHIP AND ROBOT.
            // robot 0.330 is the existing measurement from the CLEARED fall
            // (t=16,367-370). ship 0.172 is worker-98's 3-point measurement
            // (cut the CLEARED plan at t=15,805, duck under the portal's y
            // window and inject into the box still as a ship):
            //   outside the box, no input: dvy=-0.069 (bare g)
            //   inside the box, no input:  dvy=+0.103 -> field = +0.172
            //   inside the box, pressed:   dvy=+0.280 = 0.172 + push 0.108
            //                              (matches 14472's known value)
            // The dvy switch at vy~+2.1 is the ship's own per-speed regime
            // (r85); the field is flat. THE 0.433 ENTERED FIRST WAS A
            // MIS-DECOMPOSITION THAT LOOKED ONLY AT THE FIXUP DELTA (it added
            // the +0.103 difference to 0.330 -- correctly the difference is
            // explained by "field 0.172 + the difference in g", and the
            // model-side application path still needs verifying).
            // A mode-ratio rule still does not hold -- it stays a measured
            // uid x mode table.
            if (o.uid == 17701) k = 0.172;   // ship (worker-98 3-point measure)
            // uid11412 (30x30, above the shaft exit): GD dvy +0.088 = -0.195 +
            // 0.283 (CLEARED t=16,957-16,965). Same size as 14472 with a
            // different strength = the strength is a per-object editor
            // property, and there is no scale law.
            if (o.uid == 11412) k = 0.283;
            // The 7-piece carpet at cy=105 (30 px spacing, so 2 always
            // overlap). The swing's measured fall (t=3,459) gives +0.216
            // combined -> 0.108 per piece.
            if (o.uid == 3396 || o.uid == 3397 || o.uid == 3452
                || o.uid == 3453 || o.uid == 3454 || o.uid == 3476
                || o.uid == 3477)
                k = 0.108;
            // Re-measured with the robot (note at the declaration). The same
            // box differs by mode.
            double kRobot = 0.0;
            if (o.uid == 14472) kRobot = 0.304;
            if (o.uid == 17701) kRobot = 0.330;  // existing CLEARED-fall measure
            g_forceBoxes.push_back({o.cx, o.cy, o.hw, o.hh, k, kRobot});
            std::printf("forcebox: uid %d at (%.0f,%.0f) %gx%g push up k=%.3f"
                        "%s\n",
                        o.uid, o.cx, o.cy, 2 * o.hw, 2 * o.hh, k,
                        kRobot > 0.0 ? " (robot 0.304)" : "");
        }
    }
    auto byX = [](const Obj& a, const Obj& b) { return a.cx < b.cx; };
    std::sort(L.objs.begin(), L.objs.end(), byX);
    std::sort(L.portals.begin(), L.portals.end(), byX);
    std::sort(L.pads.begin(), L.pads.end(), byX);
    std::sort(L.orbs.begin(), L.orbs.end(), byX);
    std::sort(L.speeds.begin(), L.speeds.end(), byX);
    std::sort(L.slopes.begin(), L.slopes.end(), byX);
    std::sort(g_timeWarps.begin(), g_timeWarps.end(),
              [](const TimeWarp& a, const TimeWarp& b) { return a.cx < b.cx; });
    std::sort(g_zoomTrigs.begin(), g_zoomTrigs.end(),
              [](const ZoomTrig& a, const ZoomTrig& b) { return a.cx < b.cx; });
    // Which moving objects does a trigger ROTATE? Read off the recording rather
    // than derived from the trigger chain: the recording is what this run
    // actually observed, and turnedBox needs the answer before the angle has
    // grown away from zero (see its axis-aligned branch).
    L.dyn.everRot.assign(L.dyn.size(), 0);
    for (size_t i = 0; i < L.dyn.size(); ++i)
        for (const DynSample& s : L.dyn.samples[i])
            if (std::fabs((double)s.rot) > 0.001) { L.dyn.everRot[i] = 1; break; }
    {
        size_t n = 0;
        for (uint8_t v : L.dyn.everRot) n += (v != 0);
        if (n) std::printf("dynamics: %zu of %zu moving objects are rotated by "
                           "a trigger\n", n, L.dyn.size());
    }
    return L;
}

// When this is not empty, the level is parsed FROM IT and no file is opened. The mod fills it
// with the table it built out of PlayLayer, so an in-process solve needs nothing on disk. The
// CLI never touches it, which is why its behaviour is unchanged (equiv suite).
inline std::string g_levelCsv;

// The CLI's way in: the same parse, reading the dump the mod wrote to disk.
inline Level loadLevel(const std::string& path, const GroupTimeline* gt = nullptr,
                const std::vector<TouchTrig>* tt = nullptr,
                const std::vector<AutoTrig>* at = nullptr) {
    if (!g_levelCsv.empty()) {
        std::istringstream in(g_levelCsv);
        return loadLevelFrom(in, gt, tt, at);
    }
    std::ifstream in(path);
    return loadLevelFrom(in, gt, tt, at);
}

}  // namespace dp
