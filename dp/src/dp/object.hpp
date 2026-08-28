#pragma once
#include "dp/bands.hpp"

namespace dp {

struct Obj {
    double cx, cy, hw, hh;
    uint8_t type;  // 0 solid, 2/47 hazard, 4 gravity portal, 5 ship, 6 cube,
                   // 8 yellow pad, 10 blue pad
    int id = 0;    // object id (10/11 = normal/reverse gravity portal)
    // m_uniqueID (level-load order). The stair snap walks the solids the player
    // touches in DESCENDING uid and the last one processed becomes the snapped
    // object, so this ordering is load-bearing, not cosmetic. -1 = column absent
    // (an objrects.txt dumped before the column was added).
    int uid = -1;
    // m_objectRadius. Non-zero ONLY for saws (object ids 88/89/98, measured on
    // lv11: 60x60 -> 21.6, 44x85 -> 32.3, 40x40 -> 12). Those are CIRCLES, not
    // rectangles, and the rect is merely their bounding box -- which is why the
    // model over-killed them. Confirmed against two survivals GD recorded:
    //   lv11 t=3751, mini, saw(4859,149) r=12   -> nearest box point 12.24 > 12
    //   lv12 t=10673, mini, saw(13831,153) r=21.6 -> 23.1 > 21.6
    // Both clear by under 1.5 px, so the rect (half 20 / 30) had no chance.
    // 0 = rectangle, i.e. every spike stays exactly as before.
    double radius = 0.0;
    // One-way platform (GD object type 21, id 143 -- lv13 and lv18 only). It
    // holds a falling player up exactly like a solid, but does NOT block from
    // the side or from below. Measured through the gd MCP on lv13's column at
    // x=2055: the cube walks straight through the type-21 blocks at ground
    // level (t=1212, 15 px deep inside one) and then jumps UP through another,
    // and only dies when it reaches the ordinary solid stacked above them.
    // Dropped onto the same column from above it lands on the top face and
    // stands there. Modelled as: landing yes, kill no.
    uint8_t oneway = 0;
    // Lives in L.dyn.objs (a moving object; its cx/cy/box are rewritten per
    // tick from the recording or the trigger definitions). Fixups do not
    // apply near one -- see nearDynObject in fixup.hpp.
    uint8_t dynObj = 0;
    // Slope (GameObjectType 25). The MOD samples GD's own slopeYPos() at the
    // left and right edge of the object's rect, so sy0/sy1 ARE the surface line
    // and the model never has to know which of the 30 slope object ids faces
    // which way. Only lv16+ have any, so nothing before that can be affected.
    uint8_t slope = 0;
    uint8_t slopeHazard = 0;   // m_slopeIsHazard (spiked slope)
    // m_slopeDirection, straight out of GD. The surface line alone cannot say
    // which side of it is solid -- lv18 stacks a floor ramp at y=195 directly
    // under a ceiling ramp at y=465 and both lines run the same way. Measured
    // there: 0 = floor uphill, 2 = floor downhill, 3 = ceiling (uphill line),
    // 1 = ceiling (downhill line). 4..7 are the 90/270-rotated ones (walls),
    // which nothing rides yet.
    uint8_t slopeDir = 0;
    double sy0 = 0.0, sy1 = 0.0;
    // getRotation(). The DASH rings read it as their dash angle -- and for
    // anything turned by something that is NOT a multiple of 90 degrees it is
    // also the only way back to the object's real shape (see `oriented`).
    double rot = 0.0;
    // objrects' w,h come from getObjectRect(), which is the AXIS-ALIGNED
    // BOUNDING BOX. For a 90-degree turn that IS the shape, but lv18's cube
    // portal at (27,755.8, 371.162) is turned 51 degrees and its box reads
    // 88.23 x 80.54 for an object that is really 34 x 86: the bound is 48 px
    // too wide, and the model fired the portal 48 px early, turned into a cube,
    // and sailed over the ramp that kills GD's wave. That is lv18's whole wall
    // (x=27,713) -- the DP returned SOLVED on every one of five anchors while GD
    // died at the same tick.
    // The nominal size comes back out of the bound and the angle:
    //   a = W|cos| + H|sin| ,  b = W|sin| + H|cos|
    // which inverts whenever |cos2t| is not ~0. On that portal it gives
    // 33.96 x 86.00 -- i.e. exactly GD's 34 x 86.
    uint8_t oriented = 0;      // 1 = use the turned box, not the bound
    double ohw = 0, ohh = 0;   // half sizes of the real (unturned) box
    double rc = 1, rs = 0;     // cos/sin of rot
    // The same thing again, but MEASURED rather than inverted: GD's own
    // getOrientedBox corners, out of obb_lv*.txt (--obb). Separate fields on
    // purpose -- `oriented` above is built from w0,h0 (the SPRITE size) and is
    // wired to the PORTAL test, where it was tuned and where lv16 is known to
    // break if it moves. For a HAZARD w0,h0 is simply not the hitbox: checked
    // against the recorded corners it is right for 0 of 64 (lv17), 0/72
    // (lv18), 0/63 (lv19), 0/117 (lv20), 0/50 (lv22).
    uint8_t obbOk = 0;
    double bhw = 0, bhh = 0;   // half sizes along the box's own axes
    double bc = 1, bs = 0;     // its long axis as a unit vector (bc, bs)
    // How far this object moved in y on THIS tick (0 for static ones). Only the
    // dynamic set ever writes it -- see Dynamics::seek. It exists because a
    // contact rule has to be read in the RELATIVE frame once the surface itself
    // moves; see the ceiling ride.
    double dcy = 0.0;
    // TELEPORT PORTAL (GameObjectType 28, object id 747). tpY is the ABSOLUTE y
    // the player is put at, dumped by the MOD as
    // getRealPosition().y + m_teleportYOffset -- not an offset, because the base
    // it is measured from is the real position and cx/cy here are the bounding
    // box centre, which differ for a turned portal (lv20's uid 1343 at rot -53:
    // 204.58 vs 195, and 364 is the y GD lands on).
    // Everything else is untouched: x keeps its value (GD's own target for id
    // 747 is the PLAYER's x), vy carries straight through (measured across the
    // teleport tick: the cube's -0.216/tick step does not skip), and the mode
    // does not change. tpGrav is m_gravityMode -- 1 normal, 2 flipped, 3 toggle,
    // 0 = leave gravity alone, which is what all 20 of lv20's are.
    double tpY = 0.0;
    uint8_t tpGrav = 0;
    // The EXIT half's real position (objrects' tpex/tpey, added 2026-08-18).
    // teleportPlayer resolves the target from m_orangePortal (portal+0x748)
    // when it is linked, and the closed tpY formula above is the id-747 branch
    // ONLY (disasm 0x20ff5e: `cmp [rsi+0x40c], 0x2eb`). The 2.2 teleport
    // (id 2902) takes the exit object's own position instead -- lv22's
    // uid 6425 carries tpy=705 (saved yOffset 0) but GD lands the player at
    // 1905.000, exactly its unexported orange half. 0,0 = column absent or no
    // linked exit (old dumps); the teleport block falls back to tpY and warns.
    double tpEx = 0.0, tpEy = 0.0;
    // getFlipY(). Used only for the blue (gravity) pad's gate -- see
    // objFacingDown(). objrects' 31st column (flipy). Falls back to 0 in an old
    // export that lacks the column.
    uint8_t flipY = 0;
};

// A verbatim port of GameObject::isFacingDown() (2.2081 win 0x1a1910). GD turns
// getRotation() into an int with **cvttss2si** before dividing by 90, so the
// truncation toward 0 is copied exactly.
inline bool objFacingDown(const Obj& o) {
    const int r = (int)o.rot;
    const bool f = (o.flipY != 0);
    if (r % 90 == 0) return (std::abs(r) == 180) ? !f : f;
    const bool in = (r >= 91 && r <= 269) || (r >= -269 && r <= -91);
    return f ? !in : in;
}

// Does the player's axis-aligned box (centre px,py, half `half`) touch `o`?
// Separating-axis test when the object is turned, the plain bound otherwise so
// that nothing in lv1-17 can move.
// Checked against GD on lv18's portal (wave held at y=405, 14 injections):
//   x=27,753.1 -> still a wave   (this says 24.59 vs 24.03: separated)
//   x=27,759.5 -> a cube         (t=20,189 gives 22.75 vs 24.03: touching)
// GD's own two-box test, read off collisionCheckObjects (win 0x214960):
//
//   00214B5E  cmp  byte ptr [rdi+0x2e8], 0   ; is this object turned?
//   00214B65  je   ...                       ; no -> the AABB result stands
//   00214B86  call [obj    vtable+0x570]     ; obj->getOrientedBox()
//   00214BA1  call [player vtable+0x570]     ; player->getOrientedBox()
//   00214BB0  call 0x06E130 (obj, player)    ; ...and again reversed at 0x214BBF
//
// so for a TURNED object the AABB overlap is only a necessary condition.
// Measured on lv22's spider portal (uid 434, turned -4.512 deg at t=773): the
// AABB overlaps by 1.61 px in y, but projecting the object's four corners onto
// the PLAYER's own v axis (the cube is at -52.435 deg) gives max = -15.880
// against the player's half of 15 -- separated by 0.88 px. GD does not fire.
inline bool obbSat(double cx1, double cy1, double hw1, double hh1, double a1,
                   double cx2, double cy2, double hw2, double hh2, double a2) {
    const double dx = cx2 - cx1, dy = cy2 - cy1;
    const double c1 = std::cos(a1), s1 = std::sin(a1);
    const double c2 = std::cos(a2), s2 = std::sin(a2);
    const double ax[4] = { c1, -s1, c2, -s2 };
    const double ay[4] = { s1,  c1, s2,  c2 };
    for (int i = 0; i < 4; ++i) {
        const double ux = ax[i], uy = ay[i];
        const double r1 = hw1 * std::fabs(c1 * ux + s1 * uy)
                        + hh1 * std::fabs(-s1 * ux + c1 * uy);
        const double r2 = hw2 * std::fabs(c2 * ux + s2 * uy)
                        + hh2 * std::fabs(-s2 * ux + c2 * uy);
        if (std::fabs(dx * ux + dy * uy) > r1 + r2) return false;
    }
    return true;
}
// `pRot` is the PLAYER's sprite rotation in degrees. 0 (every caller that has
// none to give) skips the extra test entirely, so nothing that works moves.
inline bool orientedHit(const Obj& o, double px, double py, double half,
                        double pRot = 0.0) {
    if (!o.oriented)
        return std::fabs(px - o.cx) <= o.hw + half
            && std::fabs(py - o.cy) < o.hh + half;
    if (pRot != 0.0) {
        // BOTH angles have to be in the same handedness. GD's rotations are
        // cocos's (clockwise positive) and obbSat is written in the ordinary
        // counter-clockwise convention, so both get negated -- negating only
        // one mirrors one box against the other and the test is silently wrong.
        // ...and RETURN it. The test below projects the player as an
        // AXIS-ALIGNED square onto the object's axes, which is strictly more
        // conservative than the real two-box SAT -- running both lets the
        // approximation veto the exact answer. Measured at lv22 t=776, where GD
        // fires: all four true axes overlap (tightest margin 1.06 px) while the
        // old test reads |lx| = 36.165 against ohw + r = 33.25 and rejects.
        const double thO = std::atan2(-o.rs, o.rc);  // rc/rs hold cos/sin(-th)
        return obbSat(o.cx, o.cy, o.ohw, o.ohh, -thO,
                      px, py, half, half,
                      -pRot * 3.14159265358979 / 180.0);
    }
    const double dx = px - o.cx, dy = py - o.cy;
    // the player's box projected onto the object's two axes
    const double r = half * (std::fabs(o.rc) + std::fabs(o.rs));
    const double lx = dx * o.rc + dy * o.rs;
    const double ly = -dx * o.rs + dy * o.rc;
    if (std::fabs(lx) > o.ohw + r || std::fabs(ly) > o.ohh + r) return false;
    // ...and the object's box projected onto the world axes
    const double ex = o.ohw * std::fabs(o.rc) + o.ohh * std::fabs(o.rs);
    const double ey = o.ohw * std::fabs(o.rs) + o.ohh * std::fabs(o.rc);
    return std::fabs(dx) <= ex + half && std::fabs(dy) < ey + half;
}

// Does the player's axis-aligned box (centre px,py, half `half`) touch hazard
// `o`? Saws carry a radius and are round; everything else is its rect.
// Circle-vs-AABB is the standard nearest-point test: clamp the centre into the
// box, measure. Kept as one function so the cube and ship branches cannot drift.
// SAWS ONLY: a bound on a known error, not a tuning knob. GD's velocity lands
// on a 0.001 grid after a portal halving and the model's does not, which is
// 0.0001 px of y per tick and had grown to 0.049 px by lv12 t=16796. Saws are
// where that matters: they are large and round, so clearance changes slowly and
// a route can sit 0.05 px from the edge for many ticks. lv12 t=16797 is exactly
// that -- the model reads 32.3457 against radius 32.3 and lives, GD reads
// 32.2997 and dies. Spikes are small rects crossed at full speed, so they get
// nothing (kHazMargin stays 0; any margin there kills cases GD survives).
// Drop this to 0 once the velocity grid is reproduced.
// TRIED AND REVERTED (2026-08-03): 0.0, on the theory that lv18's winning jump
// clears the saw (24,571.7,375.5) by ~0.04 px at the tick boundary and the
// 0.05 margin eats it. The frontier was UNCHANGED (same alive counts, same
// death tick), so that jump is not in the frontier for some other reason.
// TRIED AND REVERTED (2026-08-05): 0.15, after measuring a ROBOT arcing over
// lv19's saw 187 (15379,425, r=21.87) at a model clamp distance of 21.94 that
// GD killed -- the injected sweep put GD's boundary at r + 0.07..0.12 for
// that pair. But 0.15 regressed lv16 (stuck at x~4,909 in the ship section:
// a needed route grazes a saw inside the widened band), consistent with lv12
// where GD SURVIVES a ball graze at r - 0.05. The saw's effective radius is
// evidently per-mode (ball smaller, robot bigger); a single global margin
// cannot express that. The robot-vs-saw case is absorbed by the divergence
// fixups instead (a kill record at the measured transition), and stays in
// fixups_log as the to-fix entry for a per-mode hazard box.
constexpr double kSawMargin = 0.05;
// ...and the per-mode deviation the note above calls for. Only the two modes
// with an actual measurement deviate from the default:
//   robot: GD's boundary is r + 0.07..0.12 (lv19 saw 187, injected sweep) ->
//          take the middle, 0.10. The old 0.05 let the search graze 0.02..0.07
//          closer than GD tolerates, and every such plan came back as a GD
//          death + a kill fixup (36 of them in fixups_log_lv19).
//   ball:  GD survives a graze at r - 0.05 (lv12 t=16797) -> the kill radius
//          sits below that, so the addend is NEGATIVE. Loosening only ADDS
//          candidate states; a false pass dies in replay and records a fixup.
// Ship stays at the default: 0.15 across the board regressed lv16's ship
// section, which is how the margin was found to be per-mode at all.
inline double sawMarginFor(uint8_t mode) {
    if (mode == 5) return 0.10;
    if (mode == 2) return -0.05;
    return kSawMargin;
}

// SAT between the player's box (centre px,py, half `ph`, turned by `pdeg`
// degrees clockwise) and the object's recorded turned box. Four axes: two per
// box. Both boxes are rectangles, so this is exact.
inline bool obbOverlap(const Obj& o, double px, double py, double ph,
                       double pdeg) {
    const double th = -pdeg * (1.0 / kRadToDeg);   // cocos rotation is CW +
    const double pc = std::cos(th), ps = std::sin(th);
    const double dx = px - o.cx, dy = py - o.cy;
    // the object's axes
    const double lx = dx * o.bc + dy * o.bs;
    const double ly = -dx * o.bs + dy * o.bc;
    const double rOnObj = ph * (std::fabs(pc * o.bc + ps * o.bs)
                              + std::fabs(-ps * o.bc + pc * o.bs));
    if (std::fabs(lx) >= o.bhw + rOnObj || std::fabs(ly) >= o.bhh + rOnObj)
        return false;
    // the player's axes
    const double mx = dx * pc + dy * ps;
    const double my = -dx * ps + dy * pc;
    const double rOnPlrX = o.bhw * std::fabs(o.bc * pc + o.bs * ps)
                         + o.bhh * std::fabs(-o.bs * pc + o.bc * ps);
    const double rOnPlrY = o.bhw * std::fabs(-o.bc * ps + o.bs * pc)
                         + o.bhh * std::fabs(o.bs * ps + o.bc * pc);
    return std::fabs(mx) < ph + rOnPlrX && std::fabs(my) < ph + rOnPlrY;
}

// GD's radius-hazard test is GJBaseGameLayer::playerCircleCollision (win
// 0x211df0, read 2026-08-26). TWO branches on a flag at [[layer+0xdb0]+0x1cf]:
//   A: dist(playerCentre, circleCentre) <= getObjectRect().width * 0.5
//      + max(scaleX,scaleY) * m_objectRadius        (plain circle-vs-circle)
//   B: circleCentre inside getObjectRect(), OR any of the rect's 4 CORNERS
//      within the scaled radius of the circleCentre  (a corner-disc union)
// Which branch applies and the rect's half are measured on the sawcal rig
// (data/rigs/calib_sawcal*.lvl, 2026-08-26, all 8 modes x 5 stations), where
// the fit lands within 0.02px of the bisected outline at every angle probed:
//   STATIC saws take branch B in every mode. Rect half: wave 5.0, spider
//   13.5 (its 27x27 box: 24.65 = 13.5 + sqrt(17.51^2-13.5^2) on the nose),
//   every other mode 15.0. Effective radius = dumped radius * scale exactly
//   (id1734 32 / id1735 17.51 / @0.5 16 / id88 32.3 / id918 24, and id918's
//   rot does not move the circle).
//   The lv22 uid710 sweep (spider vs a GROUP-MOVED saw r=4) measured branch
//   A: a dome of centre distance 17.5..17.7 = 13.5 + 4 + margins. Branch B
//   with h=13.5 cannot produce that (its corner test can never fire at r=4 <
//   h), so the A/B flag follows the object being dynamic -- movers keep the
//   legacy circle below, which IS branch A whenever the caller's half equals
//   rectWidth/2 (true for the spider).
// Branch B is NOT the old max(0,|d|-half) nearest-point rect: on the edges
// the corner arcs sag INWARD (axis boundary h + sqrt(r^2-h^2), measured
// 36.60 = 5 + sqrt(32^2-25) on the nose), and at the corners they bulge to
// h*sqrt2 + r. MINI scales the rect by the vsize, 0.6 exactly: mini ball
// 9.0 = 15*0.6 (id918 axis 31.24 = 9+sqrt(24^2-81)), mini wave 3.0 = 5*0.6
// (id1734 axis 34.854 -> h=3.00 on the quadratic). Combos with no
// measurement return -1 and keep the legacy circle -- measure, never guess.
inline double sawRectHalfB(int mode, int mini) {
    if (mode < 0) return -1.0;
    const double s = mini ? 0.6 : 1.0;
    if (mode == 4) return 5.0 * s;   // wave
    if (mode == 6) return 13.5 * s;  // spider
    return 15.0 * s;                 // cube/ship/ball/ufo/robot/swing
}

inline bool hazardHit(const Obj* o, double px, double py, double half,
                      double margin, double sawAdd = kSawMargin,
                      int mode = -1, int mini = 0) {
    if (o->radius > 0.0) {
        const double dx = px - o->cx, dy = py - o->cy;
        const double hRect = sawRectHalfB(mode, mini);   // -1 when unmeasured
        if (hRect >= 0.0) {
            // GD branch B, exact. The rig puts the true boundary at the
            // dumped radius itself (+-0.01), so the only addend is the flat
            // float-drift allowance -- NOT the caller's per-mode sawAdd,
            // whose values (robot +0.10 / ball -0.05) were fitted to the old
            // nearest-point reading and do not transfer to this shape.
            const double r = o->radius + margin + kSawMargin;
            const double ex = std::fabs(dx) - hRect, ey = std::fabs(dy) - hRect;
            if (ex < 0.0 && ey < 0.0) return true;      // centre inside rect
            return ex * ex + ey * ey < r * r;           // nearest corner
        }
        // [2026-08-28] MOVERS TAKE BRANCH B TOO. This used to read
        // `hRect >= 0.0 && !o->dynObj`, on the strength of the lv22 uid710 sweep
        // below. That reading does not reproduce: the sawcal rig, rebuilt with
        // its saws carried by an autonomous Move trigger (mklevel.py
        // build_sawcal_moved), puts the SAME configuration -- spider, moved saw,
        // r=4 -- on branch B.
        //
        //   spider vs moved r=4   dead 13.04 / alive 14.54  -> 13.5, not A's 17.5
        //   cube   vs moved r=4   dead 14.50 / alive 17.00  -> 15.0, not A's 19.0
        //   spider vs moved r=24  alive at dy=35.0 for two ticks, dead on the
        //                         third -- B calls the death TICK exactly (corner
        //                         distance 24.66 -> 23.90 against r=24), while A
        //                         would have killed on the first
        //
        // and lv21 says the same in a real level: 12 probes, mini ship vs a
        // ROTATED r=4 blade and full cube vs a moved r=24, every one on B, with A
        // wrong in BOTH directions (it over-kills on axis and under-kills on the
        // diagonal, so no margin constant reconciles it). 17 probes, 3 modes, 3
        // radii, moved and rotated, rig and real level; nothing left supporting A.
        //
        // The disassembly agrees that the object cannot be what selects the
        // branch: 0x211df0 reads the flag at [[layer+0xdb0]+0x1cf] -- off the
        // LAYER, not off either object. What that field is has not been pinned
        // down; `dynObj` simply was not it.
        // GDSOLVER_LAB/notes/measure-moving-saw-branch-2026-08-28.md
        //
        // Branch A now only catches combos sawRectHalfB has no measurement for.
        // 0x211df0 reads getObjectRect().width*0.5
        // for the player term, so use the SAME rect half as branch B when it
        // is known: the one measured mover (lv22 uid710 vs the spider, dome
        // 17.5..17.7 = 13.5+4+m) confirms it, and lv20's moving id918s kill
        // the wave at centre distance ~27 = 5+24-ish, which the old hazard
        // half (3) read as survivable and turned into a fresh rim crawl.
        // Unmeasured combos keep the caller's half and sawAdd bit-for-bit.
        const double hA = hRect >= 0.0 ? hRect : half;
        const double r = hA + o->radius + margin
                       + (hRect >= 0.0 ? kSawMargin : sawAdd);
        return dx * dx + dy * dy < r * r;
    }
    // A TURNED hazard, against its recorded box rather than the bound around
    // it. OPT-IN (--obb) and NOT yet wired into the driver: it is a strict
    // improvement on the bound but it is not GD's shape either. Read the
    // measurements before turning it on anywhere.
    //
    // The BOUND is definitely wrong, and no half rescues it. lv20 t=811, mini
    // wave, spike id667 rot -63 at (1057.19,167.595), the player's x fixed by
    // the tick at 1051.593:
    //     y=173.24 lives      y=172.05 lives      y=172.00 dies
    // The bound (9.43 x 10.74) spans y 162.22..172.97, so it gives the SAME
    // verdict for all three whatever the half is -- only the x overlap can
    // separate them, and x is identical. A shape that is not the bound is
    // forced. The recorded box (obb_lv20.txt: 9 x 6, long axis 63 deg) does
    // separate them, at a half of 2.96..2.98 -- i.e. the contact half.
    //
    // ...but it is not the whole story, and the sweep that followed says the
    // recorded box is not GD's shape either -- see the note at the wave's
    // hazard branch for the four-piece envelope. This code is kept because it
    // is a strict improvement on the bound and because the loader is the part
    // that will be reused; the SHAPE it feeds is still provisional.
    //
    // Standard SAT, four axes: the box's two and the player square's two.
    if (o->obbOk) {
        const double dx = px - o->cx, dy = py - o->cy;
        const double h = half + margin;
        // the player's square projected onto the box's axes (same width on
        // both, the axes being a rotation of each other)
        const double r = h * (std::fabs(o->bc) + std::fabs(o->bs));
        const double lx = dx * o->bc + dy * o->bs;
        const double ly = -dx * o->bs + dy * o->bc;
        if (std::fabs(lx) >= o->bhw + r || std::fabs(ly) >= o->bhh + r)
            return false;
        // ...and the box projected onto the world axes
        const double ex = o->bhw * std::fabs(o->bc) + o->bhh * std::fabs(o->bs);
        const double ey = o->bhw * std::fabs(o->bs) + o->bhh * std::fabs(o->bc);
        return std::fabs(dx) < ex + h && std::fabs(dy) < ey + h;
    }
    // <= and not <: TOUCHING IS A HIT. The danger loop rejects with `ja`
    // (strictly-greater) on each of the four faces, so equality falls through
    // to the kill -- the same reading that kWaveKillHalf's brackets confirmed
    // on solids, applied to the branch that actually kills hazards.
    // It only ever changes the exact-equality case, and that case is not
    // hypothetical: a mini wave clamped to its band ceiling sits at y=384.000
    // for many ticks (bandCeil 390 - kWaveClampMini 6), the spike id989 at
    // (3345,375) is 9x12, and |384 - 375| is 9.000 against hh + half = 9.000.
    // GD kills there (probed) and the model rode straight through -- lv20's
    // cold run recorded 12 `kill` fixups in that one corridor, all at y=384.
    return std::fabs(px - o->cx) <= o->hw + half + margin
        && std::fabs(py - o->cy) <= o->hh + half + margin;
}

}  // namespace dp
