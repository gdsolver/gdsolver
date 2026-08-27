#pragma once
#include "dp/dynamics.hpp"

namespace dp {

// ---- ROTATED GAMEPLAY (id 2900) --------------------------------------------
//
// GD 2.2 turns the whole gameplay frame: the world does not move, the player's
// travel direction and gravity do. lv22 has 20 of these (every other level in
// the suite has none, so all of this is inert there).
//
// Measured on lv22 with the reference replay (findings.md 2026-08-13):
//   - `rot` is the ABSOLUTE screen angle, not a delta. Eight of lv22's twenty
//     are rot=0, i.e. "turn it back".
//   - travel = R(-rot) * xhat:  0 -> +X,  90 -> -Y,  180 -> -X,  270 -> +Y.
//     uid 1343 (2265,316, rot=90): from t=1,747 x freezes at 2,266.5 and y
//     falls 1.2982/tick (= the section's dx). uid 6286 (16125,489, rot=-90):
//     x freezes at 16,131 and y RISES 1.298/tick.
//   - it fires on the tick AFTER the player's forward coordinate crosses the
//     object's -- the same crossing+1 rule every autonomous trigger uses. The
//     boxes overlap for ~15 ticks before that and nothing happens.
//   - the perpendicular velocity carries over (0 for a grounded player; a
//     player that turns in mid-air keeps falling, now sideways in world terms).
//
// The model keeps its state in the CURRENT frame's coordinates, so every
// physics rule below is untouched; only the geometry has to be handed over
// already turned. (u,v) = R(-rot)*(X,Y), which for 90-degree multiples is a
// pure index swap with signs -- an AABB stays an AABB with hw/hh swapped.
struct Level {
    std::vector<Obj> objs;        // solids + hazards, sorted by cx
    std::vector<Obj> portals;     // mode/gravity portals, sorted by cx
    std::vector<Obj> pads;        // type 8/10, sorted by cx
    std::vector<Obj> orbs;        // type 11, sorted by cx
    std::vector<Obj> speeds;      // speed portals (see dxForSpeedId), by cx
    std::vector<Obj> slopes;      // GameObjectType 25, by cx
    Dynamics dyn;                 // objects with a recorded timeline
    // "The highest surface ahead" in this frame (suffix max over 30px buckets).
    // Frame 0 uses g_topAhead as is, so it stays empty there. In a rotated frame
    // the world-coordinate g_topAhead is meaningless (which is why
    // [[escapee-prune]] used to be disabled there -- and disabling it let
    // branches that never come back crowd the frontier).
    std::vector<double> topAhead;
    double bucketX0 = 0.0;
    double maxX = 0;
    // highest surface in the level. Anything meaningfully above it is a branch
    // that can never come back: in flipped gravity the cube falls UPWARD at
    // terminal velocity and, with nothing left to hit, simply leaves. GD does
    // not kill it (measured on lv10: the player reached y = 2,792 alive), so
    // nothing pruned it and the search happily spent its frontier out there.
    double maxY = 0;
};

// The level turned into one frame, built on first use and kept for the whole
// run. It has to be kept: the step function compares Obj POINTERS (snapObj,
// usedOrb, usedPad), so a temporary copy per tick would break the stair snap
// and the pad memory.
//
// NOT YET TURNED: the moving geometry. Dynamics carries per-object parallel
// arrays, a recording per object and the closed form's offsets, and all of it
// would have to turn together. Until that is done a turned frame is handed the
// STATIC world only, and the count of moving objects that fall inside the
// rotated stretch is printed so the gap is never silent.
inline std::array<std::unique_ptr<Level>, 4> g_frameLv;
// The un-turned level, so a frame can be looked up by number alone. Set once at
// load; needed by applyRotation, which has to see the NEW frame's geometry on
// the tick it turns (see the re-tap there) and only knows the frame index.
inline Level* g_baseLv = nullptr;
inline Level* levelOfFrame(int f) {
    f &= 3;
    return f == 0 ? g_baseLv : g_frameLv[(size_t)f].get();
}

inline Level& frameLevel(Level& L0, int f) {
    f &= 3;
    if (f == 0) return L0;
    auto& slot = g_frameLv[(size_t)f];
    if (!slot) {
        auto p = std::make_unique<Level>();
        auto turnVec = [&](const std::vector<Obj>& src, std::vector<Obj>& dst) {
            dst = src;
            for (Obj& o : dst) turnObj(o, f);
            std::sort(dst.begin(), dst.end(),
                      [](const Obj& a, const Obj& b) { return a.cx < b.cx; });
        };
        turnVec(L0.objs, p->objs);
        turnVec(L0.portals, p->portals);
        turnVec(L0.pads, p->pads);
        turnVec(L0.orbs, p->orbs);
        turnVec(L0.speeds, p->speeds);
        turnVec(L0.slopes, p->slopes);
        p->dyn = L0.dyn;
        p->dyn.turn(f);
        double mx = -1e18, my = -1e18;
        for (const Obj& o : p->objs) {
            mx = std::max(mx, o.cx + o.hw);
            my = std::max(my, o.cy + o.hh);
        }
        p->maxX = mx; p->maxY = my;
        {   // suffix max in this frame (built the same way as frame 0's g_topAhead)
            double lo = 1e18, hi = -1e18;
            for (const Obj& o : p->objs) {
                lo = std::min(lo, o.cx - o.hw);
                hi = std::max(hi, o.cx + o.hw);
            }
            if (lo < hi) {
                p->bucketX0 = lo;
                const size_t n = (size_t)((hi - lo) / 30.0) + 2;
                p->topAhead.assign(n, -1e18);
                for (const Obj& o : p->objs) {
                    long long i = (long long)((o.cx - lo) / 30.0);
                    if (i < 0) i = 0;
                    if (i >= (long long)n) i = (long long)n - 1;
                    p->topAhead[(size_t)i] =
                        std::max(p->topAhead[(size_t)i], o.cy + o.hh);
                }
                for (size_t i = n - 1; i-- > 0;)
                    p->topAhead[i] = std::max(p->topAhead[i], p->topAhead[i + 1]);
            }
        }
        std::printf("rotation: built frame %d (%zu colliders, %zu portals, "
                    "%zu moving)\n",
                    f, p->objs.size(), p->portals.size(), p->dyn.size());
        slot = std::move(p);
    }
    return *slot;
}

// Measured on lv2 (dump_lv2_ref.csv, replay of a known solution -- physics
// calibration only, no path information is taken from it):
//   yellow pad (id 35, type 8): vy := 16 in the player frame, regardless of
//     the incoming velocity (4 samples: -10.42 / -7.396 / -6.912 / -15 -> 16)
//   blue pad (id 67, type 10): flips gravity AND sets vy := 6.4 toward the NEW
//     floor (one sample)
//   gravity portal (type 4, id 10 = normal / 11 = reversed): sets the gravity
//     direction and HALVES the velocity, keeping its world sign. Confirmed by
//     32 colprobe samples spanning vy 4.4..15 (ratio 0.510..0.524, the excess
//     being that tick's gravity) -- it is a halving, not a clamp.
// pad trigger reach beyond (pad half + player half). Bracketed by two lv2
// measurements at exact tick x: GD fires at |dx| = 27.21 and 28.22 but not at
// 28.51, so the threshold lies in (28.22, 28.51] where hw+15 alone gives 27.5.
constexpr double kPadReach = 0.0;
constexpr double kPadYellow = 16.0;
// Pink pad (type 9), first needed by lv12. Measured on the real game through
// the gd MCP: injected onto lv12's pad at cx=491 twice, with incoming vy of
// -1.000 and -9.432, and GD came out at 10.400 both times. So it is a SET like
// every other pad, and 10.4 / 16.0 = 0.650 -- neither the orbs' 0.700 nor the
// mini 0.800, which is the usual story with these constants.
constexpr double kPadPink = 10.4;
// RED pad (id 1332, GameObjectType 34). lv22 is the only level in the suite
// that has one (uid 2841 at (3765,242.5), 29x7) and the model had no type 34 at
// all, so it simply walked past it. Measured there directly: the MINI cube is
// launched to vy = 16.000 exactly, and mini scales an impulse by 0.8
// (kMiniImpulse), so the base is 20. GD's t=2,724 -- the model stayed grounded
// at y=249 while GD rose, and that one missing pad is the whole x=3,956 wall.
constexpr double kPadRed = 20.0;
// ...and like the orbs, the pad is weaker in ball mode. Measured on lv9 t=9126:
// a flipped ball riding a ceiling touches the yellow pad under it and GD gives
// vy = -9.600, not -16. That is 0.600x, close to the orbs' 0.700x but NOT the
// same number, so there is no single ball ratio -- each one has to be measured.
constexpr double kPadYellowBall = 9.6;
// ...and the pink pad has no single ball ratio either. padtrace measurement at
// lv22 t=6,738 (uid6143, a gravity-flipped ball touching a ceiling-facing pink
// pad): vy 8.586 -> -6.720. 6.72 / 9.6 = 0.700 -- the same value as the orbs'
// 0.700, and distinct from the cube's 0.650. The old implementation produced
// kPadPink * (9.6/16) = 6.24, and that 0.48 difference changed the arc after
// the bounce and stalled the cold run at t=7,831 for 26 iterations (the DP was
// drawn to a plan that exploited this grazing bounce and drew a different arc
// from GD's).
constexpr double kPadPinkBall = 6.72;
constexpr double kOrbYellow = 11.18;  // yellow orb (id 36): same as a cube jump
// Pink orb (type 12), first needed by lv12 -- the cold DP died at x=988, right
// after the pair at 879/969, because the model did not load type 12 at all.
// Measured through the gd MCP: injected at (862,245) with vy = 0, pressed two
// ticks later, and GD set vy = 8.050 from an incoming -0.432.
// 8.05 / 11.18 = 0.720.
constexpr double kOrbPink = 8.05;
// ...and ball mode has its own value, not a ratio. Measured on lv12 t=9603:
// a ball at vy = -12.5130 hits the pink orb at (12435,225) and GD sets 6.0263.
// The yellow orb's ball ratio (0.700) would have given 8.05 * 0.7 = 5.635, and
// that guess put the model 0.391 low for the rest of the level. 6.0263/8.05 is
// 0.7486 -- no shared ball ratio exists, exactly as the yellow pad (0.600) and
// yellow orb (0.700) already showed. Measure each one.
constexpr double kOrbPinkBall = 6.0263;
// ...but the boost is PER MODE. Measured on lv10 t=2249: the same yellow orb
// gives a ball vy = 7.826, not 11.18. The model used the cube value and ended
// up 6 px above GD within eight ticks, which is why its ball route survived to
// x=3,244 while GD died at x=3,057.
constexpr double kOrbYellowBall = 7.826;
// Gravity ring (type 13, objectID 84). It does NOT boost -- it FLIPS gravity
// and starts the player moving toward the new floor at 4.472, the same shape as
// the ball's tap. Measured on lv20's first one (x=419) with orbtrace=1:
//   t=300  flip after = 1   vy 6.2120 -> +4.4720
//   t=342  flip after = 0   vy 13.544 -> -4.4720
// Both are 4.472 toward the new floor, and the incoming vy does not matter --
// same "set, don't add" behaviour as the yellow orb.
constexpr double kOrbGravity = 4.472;
// ...and like the yellow orb it is weaker in ball mode. Measured on lv10
// t=15465 (mode = ball): 4.472 becomes 3.130. That is exactly 0.7x, the same
// ratio the yellow orb has (7.826 / 11.18 = 0.7), so ball orbs look like a
// flat 70% of the cube values -- but only these two are measured.
constexpr double kOrbGravityBall = 3.130;
// The orb hitbox is a CIRCLE, not the object's box: the test is circle-vs-
// player-box (clamp the orb centre to the box, then compare the distance).
// A plain 33x33 box test fires in the corners where GD does not -- measured on
// lv3: activations up to a clamped distance of 19.5 px, while (dx,dy) =
// (32.5, 32.0) -> 24.4 px did NOT activate even though both axes were under 33.
// That corner case is what stalled the solver at x=13,945 for four rounds.
//
// 12.0 was far too SMALL, and the comment above said so without the value ever
// being updated: it records an activation at 19.5. Re-measured over every orb
// firing on record (200 of them, data/orbtrace_lv*.txt, each clamped with that
// sample's own player half so the mini ones count correctly):
//   largest clamped distance GD ACCEPTED : 22.996  (lv12 t=3670)
//   smallest clamped distance GD REJECTED: 24.4    (lv3, the corner case above)
// which brackets a CIRCLE boundary to [22.996, 24.4).
//
// TRIED AND REVERTED: 23.0, the low end of that bracket. lv3 went cleared ->
// STUCK at x=21,494, i.e. the model started firing an orb GD does not. So a
// single circle radius cannot separate the cases: 22.996 is accepted somewhere
// and something under 23 is rejected in lv3. The shape is not a plain circle,
// or it is not the same for every ring id -- OPEN, and worth re-measuring with
// non-firings instrumented (orbtrace only records firings, so there is no
// systematic record of what GD refused).
//
// 19.5 is the largest value that keeps lv1-10 cleared, and it is itself an
// observed GD activation (the lv3 note above), so it is a real lower bound
// rather than a tuned number. It also covers lv11's gravity orb at x=12,315,
// where GD fired at a clamped distance of 17.53 and the old 12.0 made the model
// wait another 10 ticks.
constexpr double kOrbRadius = 19.5;
// ...and mini needs its own value. Bracketed by two lv11 measurements, both
// mini, both clamped with the mini half of 9:
//   t=8567  clamped 17.68  GD FIRED     (orb at 11099,285)
//   t=9492  clamped 17.17  GD FIRED     (orb at 12315,157)
//   t=9490  clamped 17.94  GD did NOT   (same orb, two ticks earlier)
// so the mini boundary is in [17.68, 17.94) and 17.8 sits inside it.
// This is NOT a law -- 19.5/17.8 is neither the 0.800 impulse factor nor the
// 0.600 size factor, and no single circle-plus-clamp rule fitted the full-size
// and mini samples together (see the 23.0 experiment above). Two brackets,
// two constants, and the shape left OPEN until the non-firing side can be
// instrumented in the MOD.
constexpr double kOrbRadiusMini = 17.8;
// ---- the ring table, read out of PlayerObject::ringJump (win RVA 0x398c00) --
//
// Every ring starts from the SAME number the cube's jump uses (the double at
// this+0x7c0), multiplies it by a per-type, per-mode ratio, and then applies
// the mini factor. After the velocity is set, three modes scale it again:
// ball x0.700, spider x0.700, swing x0.600.
//
// That structure is not a guess -- it reproduces every value in this file that
// WAS measured one at a time, which is the reason to trust the entries that
// have not been:
//   yellow (11) cube  : 11.18 * 1.00          = 11.18   = kOrbYellow
//   yellow (11) ball  : 11.18 * 1.00 * 0.70   =  7.826  = kOrbYellowBall
//   pink   (12) cube  : 11.18 * 0.72          =  8.0496 = kOrbPink (8.05)
//   pink   (12) ball  : 11.18 * 0.77 * 0.70   =  6.0255 = kOrbPinkBall (6.0263)
//   gravity(13) cube  : 11.18 * 0.80 * 0.5    =  4.472  = kOrbGravity
//                       (the 0.5 is the gravity flip, which ringJump does
//                        AFTER setting vy for this type -- see below)
//   gravity(13) ball  : 4.472 * 0.70          =  3.130  = kOrbGravityBall
// Four independent measurements, four exact hits, including the two that the
// comments above say have "no shared ball ratio" -- there IS one (0.700), and
// the pink orb only looked different because its own ratio changes with the
// mode (0.72 as a cube, 0.77 as a ball).
//
// RED ring (35, id 1333). lv22 x=375 is the first one in the game and there is
// no way past its spike carpet without it.
constexpr double kRingRedCube = 1.38;
constexpr double kRingRedShip = 1.00;      // mini ship: 1.40
constexpr double kRingRedShipMini = 1.40;
constexpr double kRingRedUfo = 1.02;       // mini UFO: 1.36
constexpr double kRingRedUfoMini = 1.36;
constexpr double kRingRedBall = 1.34;
constexpr double kRingRedRobot = 1.28;
constexpr double kRingRedSpider = 1.34;
// GREEN ring (29). Ratio 1.00 (0.70 for a ship), and it FLIPS GRAVITY -- but
// unlike the blue gravity ring it flips FIRST and sets vy afterwards, so the
// value is NOT halved. That ordering is visible in the function: type 29's
// flip call sits at 0x3993bf, before the setYVelocity at 0x39951b, while type
// 13's is at 0x399608, after it.
constexpr double kRingGreen = 1.00;
constexpr double kRingGreenShip = 0.70;
// ...and a ROBOT scales every ring that has no entry of its own by 0.9
// (the `else if (m_isRobot) *= 0.9` that closes the table).
constexpr double kRingRobotDefault = 0.9;
// DROP ring (32, the black orb). It does not boost at all -- it SLAMS the
// player at its own floor at a fixed speed, ignoring the incoming velocity and
// ignoring the mini factor (its branch at 0x399886 goes straight to
// setYVelocity without touching either the size scale or the ball/spider/swing
// post-multipliers that every other ring passes through).
//   cube / ball / robot  15.0
//   spider               15.0 * 1.10 = 16.5
//   ship / wave / swing  14.0
//   UFO                  14.0 * 0.80 = 11.2
// lv21 x=1,583 is the first one in the game and the cold DP dies 45 px past it.
constexpr double kRingDrop = 15.0;
constexpr double kRingDropSpider = 16.5;
constexpr double kRingDropFly = 14.0;
constexpr double kRingDropUfo = 11.2;
// The three post-multipliers applied after the velocity is set.
constexpr double kRingBallPost = 0.700;
constexpr double kRingSpiderPost = 0.700;
constexpr double kRingSwingPost = 0.600;
constexpr double kPadBlueVy = 6.4;
// ...and ball mode weakens it, which this branch was missing entirely.
// Measured on lv12 t=18838: a MINI ball hits the gravity pad at (24481,1003)
// and GD gives -3.0720 where the model produced -5.1200 (the mini factor and
// nothing else). Dividing out the established mini 0.800 leaves 3.84 for a
// full-size ball, i.e. 0.600 of 6.4 -- the same ball ratio the yellow pad has.
// The full-size value is therefore inferred from one mini measurement, not
// measured directly; re-check when a plan first hits a full-size ball pad.
constexpr double kPadBlueBall = 3.84;
constexpr double kGravPortalScale = 0.5;

}  // namespace dp
