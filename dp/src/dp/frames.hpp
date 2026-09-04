#pragma once
#include "dp/object.hpp"

namespace dp {

// ---- MOVING GEOMETRY ------------------------------------------------------
//
// Everything above is a STATIC grid: the objrects dump is one snapshot taken at
// level entry, and the whole model treats it as the truth for all 30,000 ticks.
// That holds for lv1-18, which contain exactly ZERO moving objects
// (scripts/mechanic_census.ps1, "movable" column), and it collapses at lv19,
// where the same column reads 820 / 1,015 / 1,731 / 761 for lv19-22.
//
// The failure is not subtle. lv19's cold DP dies at x=1,459 because the static
// grid has nothing to stand on there -- while GD, replaying a working plan, has
// the player GROUNDED at (1,352, 131.6). The thing it is standing on is
// `id 470 type 0 groups=1`, which the dump lists at (1,365, **45**), i.e. below
// the floor: it rises into place before the player arrives. lv22 is the same
// shape at x=525 and it is what stops that level at x=604.
//
// Rather than implement GD's trigger system (move / rotate / toggle / spawn,
// with easing), the MOD records what actually happened: `grouptrace=1` writes
// `tick,uid,cx,cy,w,h` for every grouped object, every tick it changes. That
// makes rotation and scaling fall out for free -- they change the rect, and the
// rect is all the model ever asks for.
//
// Between two samples an object holds its last rect, and past the final sample
// it holds it forever. That is exactly right for a platform that has finished
// moving, which is the common case: the trigger fires when the object comes on
// SCREEN, so a platform is normally already in place by the time the player
// reaches it.
// `on` is GD's own toggle state (GameObject::m_isGroupDisabled). A toggle does
// NOT move the object, so a recorder that watched positions alone would report
// a wall that never changes -- which is exactly what lv19 x=27,705 looked like:
// two 30x30 blocks filling the only 60 px gap in a wall the player flies
// straight through.
// ---- ROTATED GAMEPLAY (id 2900) --------------------------------------------
//
// GD 2.2 turns the whole gameplay frame: the world does not move, the player's
// travel direction and gravity do. lv22 has 20 of these (no other level in the
// suite has any, so all of this is inert there).
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
//   - the world VELOCITY carries over: the forward speed becomes the new
//     perpendicular one, and the old perpendicular is dropped (the forward
//     speed is the section's, not a free variable).
//
// The model keeps its state in the CURRENT frame's coordinates, so every
// physics rule stays written as "x is the clock, y is height"; what changes is
// the geometry handed to it. (u,v) = R(-rot)*(X,Y), which for 90-degree
// multiples is a pure index swap with signs -- an AABB stays an AABB with
// hw/hh swapped.
struct RotTrig {
    double cx, cy;     // WORLD position of the trigger
    int frame;         // rot/90 mod 4
    // ONE SHOT, like every other trigger GD activates from the level: the tick
    // it fired, -1 while it is still live. Measured on lv22 (2026-08-13): the
    // player leaves the first rotated section at world x=2,143.5 heading +X and
    // re-crosses uid 1343 (2265,316) at t=1,911 -- injected back up to y=315.9,
    // i.e. INSIDE the trigger's own box -- and GD does nothing at all.
    // Without this the model turns again, exits at 2,143.5, walks back to
    // 2,265, turns again: a closed loop the frontier can never leave (x pinned
    // at <=2,266 for 1,000 ticks). It only looked like progress before because
    // the broken frame floor dropped the player 700 px out of the section.
    int firedT = -1;
    // A separate one-shot for the reverse run (a 2900 pointing at the same
    // frame). **Must NOT be shared with firedT**: if shared, a trigger consumed
    // at the entrance as a reverse-run toggle can no longer be used later as its
    // real frame change. Measured 2026-08-15: lv22's cold run could no longer
    // get out of x=2,343 (the old exe was at 2,441 by iter 11).
    int revT = -1;
    // The gravity direction this trigger sets (from mvdir), in the model's local
    // convention. -1 = an old export without the column, in which case flip is
    // carried over as before.
    int setFlip = -1;
    // The reverse run this trigger sets (from gnddir). -1 = an old export
    // without the column; only then does it fall back to the old "toggle if it
    // is the same frame" rule.
    int setRev = -1;
    // objrects' uid. Used only to match --spentrot (the driver naming the 2900s
    // that had already fired before the anchor).
    int uid = -1;
    // Effective multiplier on vy [2026-08-19]: m_velocityModY(583, member
    // default 0.0) if m_editVelocity(169) is set, 1.0 (pass-through) if not.
    // Folded at parse time. An old export (no column) falls back to 1.0 -- lv22
    // is the only level with a 2900, and its objrects was re-dumped the same day.
    float vmodY = 1.0f;
    // m_overrideVelocity(584). When set, vmodY is an absolute assignment, not a
    // multiplier. lv22 has no instance (carried only).
    uint8_t ovrVel = 0;
    // The object's RAW rotation and flipX, kept because the queue's sort order
    // is a function of them and of nothing else. `frame` above cannot serve:
    // it is overridden from gnddir, and GD's own sort reads the rotation.
    // `determineSlopeDirection` truncates the rotation to an int before its
    // exact comparisons against 0 / +-180 / 90 / 270, so a non-cardinal
    // rotation matches no branch and the direction stays x-ascending.
    double rawRot = 0.0;
    uint8_t flipX = 0;
    // The raw gnddir, for the reverse predicate. The frame table above folds
    // this into (frame, setRev) through a FITTED mapping; the queue needs the
    // value itself, because GD's reverse flag is the pure predicate
    // `gnddir - 2 <u 2` (i.e. gnddir is 2 or 3) and no mapping is involved.
    int gndDir = 0;
};
inline std::vector<RotTrig> g_rotTrig;

// ---- THE 2.2 TRIGGER QUEUE (channel / ord) ---------------------------------
//
// GD does not fire these on proximity. Every trigger with m_objectType==1 sits
// in a per-channel bucket, sorted once at load, and `checkSpawnObjects` walks
// ONE channel per tick from a per-channel cursor, stopping at the first element
// whose firing point the player has not passed. So an unconsumed element blocks
// everything behind it, and a channel switch can release several at once -- not
// because bulk firing is a rule, but because the loop keeps going until it
// breaks.
//
// The model had none of this: it tested "did the player cross this trigger's
// axis coordinate" against every 2900, with a perpendicular window standing in
// for channel separation. That is why lv22's t=6,315 is missing -- uid5957 is
// released by channel 3's REVERSE flag, which uid5809 set 335 ticks earlier.
struct RotQEntry {
    int uid = -1;
    // 2900 or 2899. Both are queue residents -- they carry m_channelValue and
    // m_ordValue and are consumed the same way -- but ONLY a 2900 touches the
    // channel machinery: EffectGameObject::triggerObject sends 2900 to
    // rotateGameplay and 2899 to processOptionsTrigger, and the writers of the
    // active channel (+0x33c) are rotateGameplay, resetSpawnChannelIndex,
    // loadUpToPosition, createCheckpoint and init -- no Options path. Ten of
    // lv22's thirty are 2899 and two of those carry m_changeChannel, so reading
    // that field without checking the id switches the channel twice over.
    int id = 0;
    int chan = 0;     // m_channelValue: the bucket this object lives in
    int ord = 0;      // m_ordValue: the first sort key, stronger than position
    double px = 0.0, py = 0.0;   // the firing point: the LOAD position, frozen
    int swarm = 0;    // m_changeChannel: only these switch the active channel
    int swch = 0;     // m_targetChannelID
    int chanOnly = 0; // 1 = switches the channel WITHOUT rotating the player
    int rotIdx = -1;  // index into g_rotTrig
};
inline std::vector<RotQEntry> g_rotQ;      // grouped by channel, sorted in it
// channel -> [beg, end) in g_rotQ. Two arrays rather than one of size 17,
// because the channels a level uses are not consecutive (lv22 uses 15 of them
// but not 0..14), so "the next channel's begin" is not this one's end.
inline std::array<int, 16> g_rotQBeg{};
inline std::array<int, 16> g_rotQEnd{};
// Bits of State::rotSpent that belong to each channel, so "how many of this
// channel have been consumed" is one popcount.
inline std::array<uint32_t, 16> g_rotQChanMask{};
// --rotqueue: consume rotations from the queue instead of the pre-queue
// selection (the travel-axis crossing, the perpendicular window, the
// last/nearest rule). OPT-IN, and the reason is measured rather than cautious:
//
// the queue is right from t=0 and WRONG AT AN ANCHOR. State::rotSpent /
// rotChan / rotRev are built up tick by tick, so a state handed to --start
// mid-level begins on channel 0 with nothing consumed and re-fires everything
// the run had already passed. Measured: from t=0 the queue fixes the
// transition it was built for (lv22 t=6,315, frame AND gravity, 191 of 192
// transitions agreeing), while quick_regress -- which is anchored sections
// throughout -- loses tracking in 12 of lv22's, worst 400 -> 17 at t=1,800.
//
// This is the third instance today of the same hole: a per-state value the
// anchor scan does not seed (State::fireB, State::lockOff, and now these).
// The old path had --spentrot for exactly it, fed from the GD dump's frame
// transitions before t0; the queue needs the equivalent before it can be the
// default, and until then it is what the flag turns on.
inline bool g_rotQueue = false;
// std::popcount is C++20 and this tree builds as C++17.
inline int popCount32(uint32_t v) {
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}
inline int g_rotQChans = 0;                // how many channels actually appear

// GD's `getObjectDirection`, which is what decides a bucket's sort axis:
// 1 = y ascending, 2 = y descending, 3 = x descending, 4/default = x ascending.
// flipY does not reach the result and is not read here. The rotation is
// truncated to an int and then compared for exact equality, so 89.5 degrees
// matches nothing and falls through to x-ascending -- deliberately, that is
// GD's own behaviour rather than a tolerance this code chose.
inline int rotQDirection(double rot, bool flipX) {
    const int r = ((int)rot) % 360;
    if (r == 0)                  return flipX ? 3 : 4;   // x desc : x asc
    if (r == 180 || r == -180)   return flipX ? 4 : 3;
    if (r == 90 || r == -270)    return flipX ? 1 : 2;   // y asc : y desc
    if (r == 270 || r == -90)    return flipX ? 2 : 1;
    return 4;
}

// The signed coordinate a bucket is ordered along.
inline double rotQAxis(int dir, double px, double py) {
    switch (dir) {
        case 1:  return  py;    // y ascending
        case 2:  return -py;    // y descending
        case 3:  return -px;    // x descending
        default: return  px;    // x ascending
    }
}

// Sort each channel's bucket the way LevelTools::sortChannelOrderObjects does:
// ord first, then the axis coordinate TRUNCATED TO AN INT (so two objects in
// the same integer bucket are not separated by position at all), then uid.
// The direction comes from the FIRST 2900 in uid order that switches TO this
// channel -- not from the objects in it, and not from the level's rotation.
inline void buildRotQueue() {
    g_rotQBeg.fill(0);
    g_rotQEnd.fill(0);
    g_rotQChans = 0;
    if (g_rotQ.empty()) return;
    // Pass 1: the direction of each channel, from the first switcher that
    // names it. "First" is array order, which is uid order.
    std::array<int, 17> dir{};
    dir.fill(0);
    std::vector<const RotQEntry*> byUid;
    byUid.reserve(g_rotQ.size());
    for (const RotQEntry& e : g_rotQ) byUid.push_back(&e);
    std::sort(byUid.begin(), byUid.end(),
              [](const RotQEntry* a, const RotQEntry* b) { return a->uid < b->uid; });
    for (const RotQEntry* e : byUid) {
        if (!e->swarm || e->swch < 0 || e->swch > 15) continue;
        if (dir[e->swch]) continue;                       // first one wins
        if (e->rotIdx < 0 || (size_t)e->rotIdx >= g_rotTrig.size()) continue;
        const RotTrig& rt = g_rotTrig[(size_t)e->rotIdx];
        dir[e->swch] = rotQDirection(rt.rawRot, rt.flipX != 0);
    }
    // Pass 2 + 3: bucket by channel, then sort inside each bucket.
    std::stable_sort(g_rotQ.begin(), g_rotQ.end(),
                     [](const RotQEntry& a, const RotQEntry& b) {
                         return a.chan < b.chan;
                     });
    size_t i = 0;
    while (i < g_rotQ.size()) {
        size_t j = i;
        const int ch = g_rotQ[i].chan;
        while (j < g_rotQ.size() && g_rotQ[j].chan == ch) ++j;
        const int d = (ch >= 0 && ch <= 15 && dir[ch]) ? dir[ch] : 4;
        std::sort(g_rotQ.begin() + (long long)i, g_rotQ.begin() + (long long)j,
                  [d](const RotQEntry& a, const RotQEntry& b) {
                      if (a.ord != b.ord) return a.ord < b.ord;
                      const int ka = (int)((float)a.ord
                                           + (float)rotQAxis(d, a.px, a.py));
                      const int kb = (int)((float)b.ord
                                           + (float)rotQAxis(d, b.px, b.py));
                      if (ka != kb) return ka < kb;
                      return a.uid < b.uid;
                  });
        if (ch >= 0 && ch <= 15) {
            g_rotQBeg[(size_t)ch] = (int)i;
            g_rotQEnd[(size_t)ch] = (int)j;
            uint32_t m = 0;
            for (size_t k = i; k < j && k < 32; ++k) m |= (uint32_t)1 << k;
            g_rotQChanMask[(size_t)ch] = m;
            ++g_rotQChans;
        }
        i = j;
    }
}

// Read the MOD's rotgameplay.txt (solver.hpp has written it for a while; until
// now nothing read it). Joins to g_rotTrig by uid, so it runs AFTER the level.
inline bool loadRotQueue(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::unordered_map<int, int> byUid;
    for (size_t k = 0; k < g_rotTrig.size(); ++k)
        byUid[g_rotTrig[k].uid] = (int)k;
    std::string line;
    std::getline(in, line);                       // header
    g_rotQ.clear();
    while (std::getline(in, line)) {
        RotQEntry e{};
        int sord = 0, sordd = 0, spx = 0, target = 0, chanChanged = 0;
        const int n = std::sscanf(
            line.c_str(), "%d,%d,%lf,%lf,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
            &e.uid, &e.id, &e.px, &e.py, &e.chan, &e.ord, &sord, &sordd, &spx,
            &target, &chanChanged, &e.swarm, &e.chanOnly, &e.swch);
        if (n < 14) continue;
        const auto it = byUid.find(e.uid);
        e.rotIdx = (it == byUid.end()) ? -1 : it->second;
        g_rotQ.push_back(e);
    }
    buildRotQueue();
    return true;
}
// --spentrot uid,uid,...: the uids of the 2900s this run had already fired
// before the anchor (--start's t0). The one-shot (firedT) cannot be carried
// through --start, so re-anchoring behind the maze re-fires a spent trigger at
// the next crossing and only the model turns (lv22 t=14,321 uid6337: GD had
// consumed it at t=12,795 and passes straight through; the model turned into
// frame 3 and the whole world broke. A fixup cannot carry the frame, so the
// driver re-stacked the same fixup for 30 iterations and marked time). The
// driver derives it from the gframe transitions (t<t0) in the GD dump and
// passes it in.
inline std::vector<int> g_spentRot;
// A/B switch that turns off the reverse-run toggle (a 2900 of the same frame)
// (--norevtoggle).
inline bool g_revToggle = true;

// ---- CONTROL-DISABLED WINDOWS (--ctrlwin t0:t1,...) -------------------------
//
// **id 2899 is not a reverse run but an Options trigger** (GD's
// GameOptionsTrigger). All 10 of lv22's raise/lower m_disableP1Controls, and
// while it is raised **the button is ignored entirely** -- neither a press nor
// the cube's held-button re-jump happens. The windows measured in GD
// (2026-08-18, cfg endtrace=1) are four:
//   t=13,461..14,224 / 18,259..18,612 / 19,152..19,576 / 20,110..20,487
// 1,921 ticks in total. The model thought it could jump there, and this was the
// identity of fixcensus's `m0/mini0/g1/gdg1/sp0.9/air/in1` family (edy=0.000 /
// edvy=-11.180, only the model jumps).
//
// **Why not derive it from the triggers ourselves**: which x crossing makes GD
// raise it is still unsolved. Crossing the same x of the same trigger many times
// fires it only once, and that once is not necessarily the first crossing
// (uid 18316 on the 5th, uid 18086 on the 3rd). Nor is it the distance to the
// player's y (no fire at Δ=1,236, fires at Δ=1,324). The gate is GD's camera or
// something like it, which the current model does not have. **Firing on a guess
// does more harm than good** (burning uid 18315 at its first crossing t=10,673
// creates 393 ticks of control loss that GD does not have, and on top of that
// misses the real window). So the windows are passed only "when GD knows them":
// section-anchor replays (fixcensus / quick_regress / the driver's re-anchor)
// already receive gframe and pmin/pmax from the GD dump, so this fits the same
// scheme. A cold run from the head has no such information and the model jumps
// as before (an unresolved hole).
inline std::vector<std::pair<long long, long long>> g_ctrlWin;

inline bool ctrlOffAt(long long t) {
    for (const auto& w : g_ctrlWin)
        if (t >= w.first && t <= w.second) return true;
    return false;
}
// Jump ticks that come from a window's re-push (= w1+1). GD's buffered jump has
// holding already at 0 when it fires, so **no hover starts** -- the model side,
// which mirrors it with a synthetic press, would create s.action=1, so the jump
// on exactly this tick does not accumulate rHover. Filled only when a replay is
// loaded (always empty during the search).
inline std::vector<long long> g_winRePushJump;
inline bool rePushNoHoverAt(long long t) {
    for (long long v : g_winRePushJump)
        if (t == v) return true;
    return false;
}

inline void toFrame(int f, double X, double Y, double& u, double& v) {
    switch (f & 3) {
        case 0:  u =  X; v =  Y; break;
        case 1:  u = -Y; v =  X; break;   // rot 90:  travel -Y
        case 2:  u = -X; v = -Y; break;   // rot 180: travel -X (the reverse runs)
        default: u =  Y; v = -X; break;   // rot 270 (= -90): travel +Y
    }
}
inline void fromFrame(int f, double u, double v, double& X, double& Y) {
    switch (f & 3) {
        case 0:  X =  u; Y =  v; break;
        case 1:  X =  v; Y = -u; break;
        case 2:  X = -u; Y = -v; break;
        default: X = -v; Y =  u; break;
    }
}
// The travel coordinate of a world point in this frame (what `cx` becomes).
inline double frameU(int f, double X, double Y) {
    double u, v;
    toFrame(f, X, Y, u, v);
    return u;
}
// ...and the PERPENDICULAR one (what `cy` becomes). A 2900 fires only when the
// player's box overlaps it on this axis -- see the gate in applyRotation.
inline double frameV(int f, double X, double Y) {
    double u, v;
    toFrame(f, X, Y, u, v);
    return v;
}

// ---- turning one object into a frame's coordinates -------------------------
//
// What has to come along:
//   - the box: centre through toFrame, hw/hh swapped on the odd frames
//   - `rot`: the object's own turn is measured against the screen, so it moves
//     with the frame (used by the oriented rings and the dash angle)
//   - the slope line (sy0/sy1 are absolute heights at the box's left/right
//     edge): a turned slope is a different line, so the two endpoints are
//     carried as points and re-read
//   - tpY (an absolute target y for a teleport) becomes the target's v
inline void turnObj(Obj& o, int f) {
    if ((f & 3) == 0) return;
    const double cx = o.cx, cy = o.cy;
    double u, v;
    toFrame(f, cx, cy, u, v);
    // the slope's two surface points, in world, before the box moves
    const double sxL = cx - o.hw, sxR = cx + o.hw;
    const double syL = o.sy0, syR = o.sy1;
    o.cx = u; o.cy = v;
    if (f & 1) std::swap(o.hw, o.hh);
    o.rot += 90.0 * (f & 3);
    if (o.slope) {
        double uL, vL, uR, vR;
        toFrame(f, sxL, syL, uL, vL);
        toFrame(f, sxR, syR, uR, vR);
        if (uL <= uR) { o.sy0 = vL; o.sy1 = vR; }
        else          { o.sy0 = vR; o.sy1 = vL; }
    }
    if (o.tpY != 0.0) {
        double tu, tv;
        toFrame(f, cx, o.tpY, tu, tv);
        o.tpY = tv;
    }
    // the exit half is a POINT of its own (its x is not the portal's cx), so
    // it turns as one -- the teleport block reads its v as the target
    if (o.tpEx != 0.0 || o.tpEy != 0.0) {
        double eu, ev;
        toFrame(f, o.tpEx, o.tpEy, eu, ev);
        o.tpEx = eu; o.tpEy = ev;
    }
}

}  // namespace dp
