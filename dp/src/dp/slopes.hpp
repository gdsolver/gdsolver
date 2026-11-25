#pragma once
#include "dp/search_key.hpp"

namespace dp {

// Everything a tick needs. ONE implementation for both the search and the
// witness re-simulation: they used to be two copies and drifted -- the resim
// never learned about pads, so its trace disagreed with GD from the first pad
// onward and made every comparison useless (measured on lv2, t=292).
// Slope riding, measured on lv19's first two slopes (a 60x30 at x=300 and two
// 30x30 at x=615/645, speed 0.9, full size):
//
//  * the player's foot sits on the surface sampled at x + pHalf*tan(theta/2),
//    where theta is the slope's own angle. Measured offsets 3.5405 px for the
//    m=0.5 slope and 6.213 px for the m=1.0 one; 15*tan(atan(0.5)/2) = 3.5410
//    and 15*tan(atan(1)/2) = 6.2132. That is just the lowest corner of the
//    player's box once GD has rotated it to the slope's angle (the dump's `rot`
//    column follows the angle exactly).
//  * the surface is clamped to the slope's own x range, so the player keeps
//    riding the top edge until its reference point leaves the object.
//  * LEAVING the top launches the player. Measured vy 3.999 (m=0.5) and 7.405
//    (m=1.0), applied like a pad -- y moves with the OLD velocity on that tick
//    and vy is set afterwards. Two points only, so the table is interpolated
//    linearly in m and flagged; a third angle (the 30x60, m=2) is unmeasured.
inline double slopeXOffset(double m, double pHalf) {
    return pHalf * std::tan(std::atan(std::fabs(m)) * 0.5);
}
// Which side of the surface line is solid. GD's own test, read off the binary
// at 0x3921B5 (PlayerObject::collidedWithObjectInternal, the m_currentSlopes
// loop): `dir == 1 || (dir - 3 <= 3 && dir != 4)`, i.e. exactly {1,3,5,6}.
// m_slopeUphill is a pure function of m_slopeDirection ({0,3,6,7} rise,
// {1,2,4,5} fall -- re-checked 2026-09-07 over all 3,091 type-25 objects in the
// 22 dumps, was 2,631 when first written),
// so the flag splits each of those families 2-2, and the half that GD picks
// out is the CEILING half: lv20's corridors pair 0 with 3, 2 with 1, 4 with 5
// and 7 with 6, low member first, every time. 4..7 are the 90-degree-rotated
// sprites of the same four shapes, not walls.
inline bool slopeIsCeiling(uint8_t dir) {
    return dir == 1 || dir == 3 || dir == 5 || dir == 6;
}







// The overlap test slopeVetoesSolid starts with, taken with the widest
// inflation either way (1.2 on both axes): whatever `turned` says below,
// iw <= hw*1.2 and ih <= hh*1.2, so a pair that fails this fails the real test
// too -- and this one needs no std::fmod, which profiled at ~11% of the search
// on lv16 (one call per solid x ramp x state). It reads nothing but the two
// rects, so a caller can also take it once per pair ahead of the states (see
// SlopeVetoIndex). A negative half would break the ordering, so it passes.
inline bool rampMayVetoSolid(const Obj* R, const Obj* o) {
    if (!(R->hw >= 0.0 && R->hh >= 0.0)) return true;
    const double sx0 = o->cx - o->hw, sx1 = o->cx + o->hw;
    const double sy0 = o->cy - o->hh, sy1 = o->cy + o->hh;
    const double jw = R->hw * 1.2, jh = R->hh * 1.2;
    return R->cx - jw <= sx1 && R->cx + jw >= sx0
        && R->cy - jh <= sy1 && R->cy + jh >= sy0;
}


// The ramps each solid of a window can be vetoed by, worked out once per
// frontier group instead of once per state. rampMayVetoSolid reads only the two
// rects, and both stay put while the group's states are stepped, so the pairs it
// rejects are the same for every state. The solid loops ask the veto of every
// solid in the window (the flying one twice), and on lv16 walking the whole ramp
// list for each was 28% of the search. `off` is indexed by position in `near`;
// solid i's candidates are ramps[off[i], off[i+1]), in the list's own order.
struct SlopeVetoIndex {
    const std::vector<const Obj*>* near = nullptr;     // the lists it was built
    const std::vector<const Obj*>* slopes = nullptr;   // from
    std::vector<uint32_t> off;
    std::vector<const Obj*> ramps;
    void build(const std::vector<const Obj*>& nr, const std::vector<const Obj*>& sl) {
        near = &nr;
        slopes = &sl;
        off.assign(nr.size() + 1, 0);
        ramps.clear();
        for (size_t i = 0; i < nr.size(); ++i) {
            off[i] = (uint32_t)ramps.size();
            const Obj* o = nr[i];
            if (o->type != 0) continue;
            for (const Obj* R : sl)
                if (rampMayVetoSolid(R, o)) ramps.push_back(R);
        }
        off[nr.size()] = (uint32_t)ramps.size();
    }
};


// The player's half-box. Split out of stepOne's `pHalf` so the touch-trigger
// test cannot drift from the collision test -- the firing boundary was measured
// to 1 px (see TouchTrig) and a different half here would silently move it.
inline double playerHalf(uint8_t mode, bool mini) {
    return (mode == 4) ? (mini ? kWaveHalfMini : kWaveHalf)
         : (mode == 6) ? (mini ? kSpiderHalfMini : kSpiderHalf)
                       : (mini ? kMiniHalf : kCubeHalf);
}

}  // namespace dp
