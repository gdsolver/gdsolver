// buttons_test: dp/src/dp/buttons.hpp against the table (notes/pushbutton-state-table-2091-1027.md, section 4.1 / 4.3), on synthetic player images.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/buttons_test.cpp
// Each case builds an image, runs one call and checks the registers the table names (H S Hm Sm slots RF G BOOST HB, the used-rings set, the touching array).
// The comparison with the game's own bytes (btraw.csv, mod cfg btraw) is a second tool; this one holds the rules that must not move.
#include "dp/buttons.hpp"

#include <cstdio>
#include <cstdlib>

using namespace dp::gdcol;

static int g_fail = 0, g_ran = 0;
#define CHECK(cond) do { ++g_ran; if (!(cond)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

struct Rig {
    Mem img;
    Ctx c;
    RingTouches rt;
    std::set<int> used;
    std::vector<GObj> objs;
    ButtonsBody b;
    ButtonsHooks hk;
    int updateJumpCalls = 0;
    Rig() : img(0xc80), c(img) {
        c.objs = &objs;
        c.p22 = true;
        b.c = &c;
        b.touch = &rt;
        b.usedRings = &used;
        img.f32(0x9f8, 1.0f);
        img.f64(bo::JumpCoef, 11.180032);   // the cube's jump coefficient is not what is tested; any value
        hk.updateJump = [this](Ctx&, float) { ++updateJumpCalls; };
    }
    GObj& ring(int uid, int type, bool multi = false) {
        GObj o;
        o.uid = uid;
        o.type = type;
        o.multiAct = multi;
        objs.push_back(o);
        return objs.back();
    }
    void arm() {   // a press was already registered: Hm, Sm up (what a pushButton leaves)
        img.u8(bo::Hm, 1);
        img.u8(bo::Sm, 1);
    }
};

int main() {
    // pushButton: H, S, Hm, Sm up, the slots down, RF down, the used-rings set empty
    {
        Rig r;
        r.img.u8(bo::SlotA, 1); r.img.u8(bo::SlotB, 1); r.img.u8(bo::SlotC, 1); r.img.u8(bo::SlotD, 1);
        r.img.u8(bo::Rf, 1);
        r.used.insert(7);
        const bool ok = pushButton(r.b, 1, r.hk);
        CHECK(ok);
        CHECK(r.img.u8(bo::H) == 1 && r.img.u8(bo::S) == 1 && r.img.u8(bo::Hm) == 1 && r.img.u8(bo::Sm) == 1);
        CHECK(r.img.u8(bo::SlotA) == 0 && r.img.u8(bo::SlotB) == 0 && r.img.u8(bo::SlotC) == 0 && r.img.u8(bo::SlotD) == 0);
        CHECK(r.img.u8(bo::Rf) == 0);
        CHECK(r.used.empty());
        CHECK(r.b.holding[1]);
    }
    // pushButton with the controls disabled or the body locked: nothing changes (the holding map still takes the press)
    {
        Rig r;
        r.img.u8(bo::ControlsDisabled, 1);
        CHECK(!pushButton(r.b, 1, r.hk));
        CHECK(r.img.u8(bo::H) == 0 && r.img.u8(bo::S) == 0 && r.b.holding[1]);
        Rig q;
        q.img.u8(bo::Locked, 1);
        CHECK(!pushButton(q.b, 1, q.hk));
        CHECK(q.img.u8(bo::H) == 0);
    }
    // pushButton: no ring touching -> the cube on the ground runs updateJump(0), in the air it does not; a ship never does; a ball does without H
    {
        Rig r; r.img.u8(po::G, 1);
        pushButton(r.b, 1, r.hk);
        CHECK(r.updateJumpCalls == 1);
        Rig a; pushButton(a.b, 1, a.hk);
        CHECK(a.updateJumpCalls == 0);
        Rig s; s.img.u8(po::G, 1); s.img.u8(po::ModeShip, 1);
        pushButton(s.b, 1, s.hk);
        CHECK(s.updateJumpCalls == 0);
        Rig w; w.img.u8(po::G, 1); w.img.u8(po::ModeBall, 1);
        pushButton(w.b, 1, w.hk);
        CHECK(w.updateJumpCalls == 1);
    }
    // releaseButton(1): H, S down, HB up; the mirrors are not touched
    {
        Rig r;
        pushButton(r.b, 1, r.hk);
        r.img.u8(bo::Hb, 0);
        CHECK(releaseButton(r.b, 1, r.hk));
        CHECK(r.img.u8(bo::H) == 0 && r.img.u8(bo::S) == 0 && r.img.u8(bo::Hm) == 1 && r.img.u8(bo::Sm) == 1 && r.img.u8(bo::Hb) == 1);
        CHECK(!r.b.holding[1]);
        Rig d;
        d.img.u8(bo::ControlsDisabled, 1);
        d.img.u8(bo::H, 1);
        CHECK(!releaseButton(d.b, 1, d.hk));
        CHECK(d.img.u8(bo::H) == 1);
    }
    // ringJump through pushButton: a yellow ring in the touching array fires once, S down, the class-A slot and RF up, G / G2 down, BOOST up, the ring leaves the array
    {
        Rig r;
        r.ring(11, 0xb);
        r.rt.touched(11);
        r.img.u8(po::G, 1);
        pushButton(r.b, 1, r.hk);
        CHECK(r.img.u8(bo::S) == 0 && r.img.u8(bo::H) == 1);
        CHECK(r.img.u8(bo::SlotA) == 1 && r.img.u8(bo::SlotB) == 0 && r.img.u8(bo::Rf) == 1);
        CHECK(r.img.u8(po::G) == 0 && r.img.u8(po::G2) == 0 && r.img.u8(po::Boost) == 1);
        CHECK(r.used.count(11) == 1 && r.rt.arr.empty());
        CHECK(r.updateJumpCalls == 0);   // a ring in reach: updateJump(0) is not run
        CHECK(r.img.f64(po::Vy) != 0.0);
        CHECK(r.img.u8(bo::RingJumpRelated) == 1);
        CHECK(r.objs[0].activated);
    }
    // a second ring of class A in the same press: slot A is up, so it does not fire; a spawn orb (slot B) and a teleport orb (slot C) do
    {
        Rig r;
        r.ring(11, 0xb); r.ring(12, 0xc); r.ring(13, 0x24); r.ring(14, 0x2e);
        for (int u : {11, 12, 13, 14}) r.rt.touched(u);
        pushButton(r.b, 1, r.hk);
        CHECK(r.used.count(13) == 1);            // the spawn orbs go first
        CHECK(r.used.count(11) == 1 && r.used.count(12) == 0);
        CHECK(r.img.u8(bo::SlotA) == 1 && r.img.u8(bo::SlotB) == 1);
        CHECK(r.used.count(14) == 1 && r.img.u8(bo::SlotC) == 1);   // the teleport orb: its slot (C) is down and the mirrors (Hm, Sm) are still up, so it fires after the ring
    }
    // ringJump gate: Hm or Sm down, dashing, dead, a ring already in the used set -> returns without a write
    {
        Rig r; r.ring(11, 0xb);
        r.img.u8(bo::Sm, 1);
        CHECK(!ringJump(r.b, &r.objs[0], r.hk));     // Hm down
        r.img.u8(bo::Hm, 1);
        r.img.u8(bo::Dashing, 1);
        CHECK(!ringJump(r.b, &r.objs[0], r.hk));
        r.img.u8(bo::Dashing, 0);
        r.used.insert(11);
        CHECK(!ringJump(r.b, &r.objs[0], r.hk));
        r.used.clear();
        r.img.u8(bo::Dead, 1);
        CHECK(!ringJump(r.b, &r.objs[0], r.hk));
        r.img.u8(bo::Dead, 0);
        CHECK(ringJump(r.b, &r.objs[0], r.hk));
    }
    // the ring's effect on H: a ball, a spider or a swing loses H; a cube keeps it. Blue and green turn the gravity; drop leaves G; dash rings write no press register
    {
        Rig r; r.arm(); r.ring(1, 0xb); r.img.u8(bo::H, 1); r.img.u8(bo::S, 1); r.img.u8(po::ModeBall, 1);
        ringJump(r.b, &r.objs[0], r.hk);
        CHECK(r.img.u8(bo::H) == 0 && r.img.u8(bo::S) == 0);
        Rig q; q.arm(); q.ring(1, 0xb); q.img.u8(bo::H, 1); q.img.u8(bo::S, 1);
        ringJump(q.b, &q.objs[0], q.hk);
        CHECK(q.img.u8(bo::H) == 1 && q.img.u8(bo::S) == 0);
        Rig u; u.arm(); u.ring(1, 0xd); u.img.u8(bo::H, 1); u.img.u8(bo::S, 1);
        ringJump(u.b, &u.objs[0], u.hk);
        CHECK(u.img.u8(po::Upside) == 1);
        CHECK(u.img.u8(0x369) == 1 && u.img.u8(0x368) == 0);   // updatePlayerScale's setScale marks the oriented rect dirty, not the cached rect
        Rig g; g.arm(); g.ring(1, 0x1d);
        ringJump(g.b, &g.objs[0], g.hk);
        CHECK(g.img.u8(po::Upside) == 1);
        Rig dr; dr.arm(); dr.ring(1, 0x20); dr.img.u8(po::G, 1); dr.img.u8(bo::S, 1); dr.img.u8(bo::H, 1);
        ringJump(dr.b, &dr.objs[0], dr.hk);
        CHECK(dr.img.u8(po::G) == 1 && dr.img.u8(bo::S) == 0 && dr.img.u8(bo::H) == 1);
        Rig da; da.arm(); da.ring(1, 0x25); da.img.u8(bo::S, 1); da.img.u8(bo::H, 1);
        ringJump(da.b, &da.objs[0], da.hk);
        CHECK(da.img.u8(bo::S) == 1 && da.img.u8(bo::H) == 1 && da.img.u8(bo::SlotA) == 1);
    }
    // the spawn orb clears S only on the 2.2 level (p22); the teleport orb always
    {
        Rig r; r.arm(); r.ring(1, 0x24); r.img.u8(bo::S, 1);
        ringJump(r.b, &r.objs[0], r.hk);
        CHECK(r.img.u8(bo::S) == 0 && r.img.u8(bo::SlotB) == 1 && r.img.u8(bo::Rf) == 0);
        Rig q; q.arm(); q.c.p22 = false; q.ring(1, 0x24); q.img.u8(bo::S, 1);
        ringJump(q.b, &q.objs[0], q.hk);
        CHECK(q.img.u8(bo::S) == 1);
        Rig t; t.arm(); t.ring(1, 0x2e); t.img.u8(bo::S, 1);
        ringJump(t.b, &t.objs[0], t.hk);
        CHECK(t.img.u8(bo::S) == 0 && t.img.u8(bo::SlotC) == 1);
    }
    // resetTouchedRings: the array keeps what the pass of the step before touched, drops the rest, empties the set
    {
        RingTouches t;
        t.touched(5); t.touched(6);
        t.reset();                       // both were touched: both stay
        CHECK(t.arr.size() == 2 && t.now.empty());
        t.touched(6); t.touched(7);      // this pass: 6 again, 7 new
        CHECK(t.arr.size() == 3);
        t.reset();                       // 5 was not touched this pass: gone
        CHECK(t.arr.size() == 2 && t.arr[0] == 6 && t.arr[1] == 7);
        t.reset();                       // nothing touched: all gone
        CHECK(t.arr.empty());
    }
    // enablePlayerControls: the window lifts and a held button presses again
    {
        Rig r;
        r.img.u8(bo::ControlsDisabled, 1);
        r.b.holding[1] = true;
        enablePlayerControls(r.b, r.hk);
        CHECK(r.img.u8(bo::ControlsDisabled) == 0);
        CHECK(r.img.u8(bo::H) == 1 && r.img.u8(bo::S) == 1);
        Rig q;
        q.img.u8(bo::ControlsDisabled, 1);
        enablePlayerControls(q.b, q.hk);
        CHECK(q.img.u8(bo::H) == 0);
    }
    // releaseAllButtons on a held body: H, S down, HB up, holding down
    {
        Rig r;
        pushButton(r.b, 1, r.hk);
        releaseAllButtons(r.b, r.hk);
        CHECK(r.img.u8(bo::H) == 0 && r.img.u8(bo::S) == 0 && r.img.u8(bo::Hb) == 1 && !r.b.holding[1]);
    }
    // buttonsPhase: a release before a press in one step; a second body only when dual
    {
        Rig a, b2;
        ButtonsIn in;
        in.press1 = 1; in.release1 = 1; in.press2 = 1; in.dual = false;
        buttonsPhase(a.b, &b2.b, in, a.hk);
        CHECK(a.img.u8(bo::H) == 1 && b2.img.u8(bo::H) == 0);
        in.dual = true;
        buttonsPhase(a.b, &b2.b, in, a.hk);
        CHECK(b2.img.u8(bo::H) == 1);
    }
    std::printf("buttons_test: %d checks, %d failed\n", g_ran, g_fail);
    return g_fail ? 1 : 0;
}
