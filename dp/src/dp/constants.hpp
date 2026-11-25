#pragma once
#include "dp/modifiers.hpp"
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>

namespace dp {

// ---- the load's listings, printed once a process (lputs / lprintf) ----
// A call lists what it read of the level -- one line per trigger box, static camera, zoom, force block ... -- and a level's calls list the same lines again and again: on Dash one cold run printed
// about 45,000 such lines over 147 calls, and inside the game a printed line costs about 90 us (stdout is the pipe the loader logs from; 1 us to a file). A line that goes through lputs is printed
// unless this process has already printed exactly that text; what was held back is said in one `listonce:` line a call (lprintfNote, when cliMainOnce returns), by the lines' first word.
// --listfull prints everything as before, byte for byte. Print only.
// Through it go: everything the level loader prints (level_loader.hpp loadPrintf, and the kept text the ladder's level cache prints again), and the listing lines of the CLI and the trigger tables
// (lprintf). The text may arrive in pieces: a line is judged when its end arrives, and an unfinished one is printed as it is at the note.
struct ListOnce {
    bool full = false;                       // --listfull
    std::mutex m;
    std::unordered_set<uint64_t> seen;       // the lines printed so far (a hash of each)
    std::map<std::string, long long> held;   // since the last note: the lines not printed again, by first word
    long long heldAll = 0;
    std::string part;                        // a line whose end has not arrived yet
};
inline ListOnce g_listOnce;
inline void lputs(const char* s, size_t n) {
    if (g_listOnce.full) { std::fwrite(s, 1, n, stdout); return; }
    std::lock_guard<std::mutex> lk(g_listOnce.m);
    ListOnce& L = g_listOnce;
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && s[e] != '\n') ++e;
        if (e == n) { L.part.append(s + i, n - i); break; }   // no end of line yet
        const char* line = s + i; size_t len = e + 1 - i;
        if (!L.part.empty()) { L.part.append(line, len); line = L.part.data(); len = L.part.size(); }
        uint64_t h = 1469598103934665603ULL;
        for (size_t k = 0; k < len; ++k) { h ^= (unsigned char)line[k]; h *= 1099511628211ULL; }
        if (L.seen.size() > 4000000) L.seen.clear();   // (a very long session: start again rather than grow without end)
        if (L.seen.insert(h).second) std::fwrite(line, 1, len, stdout);
        else {
            size_t a = 0; while (a < len && (line[a] == ' ' || line[a] == '\t')) ++a;
            size_t b = a; while (b < len && line[b] != ':' && line[b] != ' ' && line[b] != '\n' && b - a < 24) ++b;
            ++L.held[std::string(line + a, b - a)];
            ++L.heldAll;
        }
        L.part.clear();
        i = e + 1;
    }
}
inline int lprintf(const char* fmt, ...) {
    char sb[2048];
    std::string big;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(sb, sizeof sb, fmt, ap);
    va_end(ap);
    if (n <= 0) return n;
    const char* text = sb;
    if ((size_t)n >= sizeof sb) {
        big.resize((size_t)n + 1);
        va_start(ap, fmt);
        std::vsnprintf(&big[0], big.size(), fmt, ap);
        va_end(ap);
        text = big.c_str();
    }
    lputs(text, (size_t)n);
    return n;
}
inline void lprintfNote() {
    std::lock_guard<std::mutex> lk(g_listOnce.m);
    ListOnce& L = g_listOnce;
    if (!L.part.empty()) { std::fwrite(L.part.data(), 1, L.part.size(), stdout); L.part.clear(); }
    if (!L.heldAll) return;
    std::string s;
    for (const auto& kv : L.held) { s += ' '; s += kv.first; s += ' '; s += std::to_string(kv.second); }
    std::printf("listonce: %lld lines of the load's listings were printed by an earlier call of this process and are not repeated (--listfull prints them):%s\n", L.heldAll, s.c_str());
    L.held.clear();
    L.heldAll = 0;
}

constexpr double kFloorY = 105.0;   // cube rest height on the ground line
// ...which is a REST HEIGHT, not the ground. The ground plane is at 90 and the
// rest height is 90 + the player's half, so a mini player rests at 99, not 105.
// Measured on lv11 t=9793: an inverted mini ship descending at vy = -3.424 is
// stopped dead at y = 99.000 and held there. The code below therefore uses
// kGroundY + pHalf everywhere it used to use kFloorY.
constexpr double kGroundY = kFloorY - 15.0;   // 90.0
constexpr double kCubeHalf = 15.0;  // outer box (landing)
// Mini. Measured on lv11: the cube crosses the mini portal (rect x[4409.5,
// 4440.5], so the boxes touch at x = 4394.5) at t=3384 and immediately starts
// falling from its resting y=165, then settles at **y=159.000** on the same
// continuous platform whose top is 150. 159 - 150 = 9, i.e. the half size goes
// 15 -> 9 (0.6x). Without this the model kept standing at 165 on a surface GD
// had already dropped away from.
constexpr double kMiniHalf = 9.0;
// ...and the same 0.800 applies to the launch impulses. Measured by injecting
// the mini cube onto lv11's yellow pad (x[7888.5,7913.5], t=6076): GD returned
// vy = 12.800, and the full-size pad is 16.0 -- 12.800 / 16.0 = 0.800 exactly,
// the same factor the jump showed. Two independent measurements agreeing to
// four digits is a rule, so it is applied to the orbs too.
// NOTE the orb side is INFERRED from those two, not measured directly. lv11's
// yellow orb at (8055,255) is the first plan that will depend on it: if the
// factor is wrong there, the GD replay diverges at that orb and says so.
// (This is separate from the per-MODE ratios -- ball is 0.600/0.700 -- which
// really do differ per constant and must stay measured one by one.)
constexpr double kMiniImpulse = 0.800;
constexpr int kRobotHoverTicks = 67;
// ---- SPIDER (mode 6) ------------------------------------------------------
// Same shape as the robot: the mode table in updateJump gives it the BALL's
// gravity scale (0.6), and its action is not a jump at all -- it is
// PlayerObject::spiderTestJump, a teleport to the surface on the other side.
//
// Measured on lv22 (injected onto the spider section at x~1,010, full size):
//   gravity  -0.129 / tick, i.e. 0.216 x 0.600 -- the ball's number to three
//            digits, on 13 consecutive airborne ticks
//   rest     y = 163.500 on a floor whose top is 150, and y = 286.500 under a
//            ceiling whose underside is 300. Both are 13.5 off the surface,
//            not the 15 every other ground mode uses.
//   teleport t=262 y=163.5 upright -> t=263 y=286.5 upsideDown, one tick, and
//            the gravity is flipped. The target was the block at (1035,315),
//            whose x range starts 9.5 px to the RIGHT of the player, so the
//            search is at least as wide as the player's own box.
//   gate     a tap in MID-AIR does nothing (13 ticks of undisturbed free fall
//            after one). GD only calls spiderTestJump from the grounded branch
//            of updateJump, same as the cube's jump.
// Rings in flight: GD closes the CONTACT path in ship/UFO/wave/swing
// (playerTouchedRing's last gate), so there a ring fires only from the press
// itself. (--no-ringmode, held-fires-in-every-mode, is gone since the flag clean-up.)
// --oriringnow: a ROTATED ring (Obj::oriented) is decided by the row after the
// press alone, not by whichever of the press row and that row is nearer (the
// ring loop's `usePre`). On since 2026-09-21, and always on since the flag
// clean-up. The measurement is at the ring loop.
// --swingpushtol: the swing's push-out onto a face needs the face within
// kLandTol, as GD's face pick does; beyond it the contact is a side hit.
// On by default since 2026-10 (was opt-in); see the push-out branch in step.hpp.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// A ring's press latch is GD's +0x986, which every consumer clears, so the
// grounded jump spends the press too. (--no-pressspent, the pre-2026-09-05
// `!s.ringHold` gate, is gone since the flag clean-up.)
// Deaths during the PLAN RECONSTRUCTION, which is a different question from
// deaths during the search. 2026-09-08, lv22@5400 at cap 8000: the search never
// put a state on the spike uid 5897 (0 of ~1,478 evaluations), the reconstructed
// plan walked into it (16 hits), the sub-step loop exited on `!dead` so DIE ran
// -- and the run still reported SOLVED and wrote a trace past the death. So a
// SOLVED verdict does not mean the plan survives its own replay, and nothing
// counted how often that happens.
//
// ONLY the reconstruction is counted, deliberately. The search runs on eight
// threads and a plain counter there would be a data race; `g_inRecon` is written
// once, between the search and the reconstruction, and only read during the
// search, so the branch below costs a predictable read and no synchronisation.
inline bool g_inRecon = false;      // set after "reconstructing plan"
inline long long g_dieRecon = 0;    // DIEs while g_inRecon
// ...and split by cause, because the total is not a number about physics.
// `out-of-play` (step.hpp:8211) is a SEARCH PRUNE -- the upright escapee bound,
// not a kill -- so a run reporting n=666 last=out-of-play may be 666 prunes and
// zero kills, or 665 and one. The total plus the last cause cannot tell those
// apart, and reading a whole census off the last entry is the mistake this file
// records elsewhere as "the last witness is not the population".
inline std::vector<std::pair<const char*, long long>> g_dieReconWhy;
// The same question asked where it can be answered: the witness resim's own
// `rdead`, which is the final plan's single walk. g_dieRecon counts every DIE
// the reconstruction runs; these count only the walk, so they say whether the
// PLAN dies. Contiguity (last - first + 1 == dead) separates "one corpse
// re-dying every tick" from "several deaths", which share a total.
inline long long g_resimDead = 0;
// The plain head bonk's discriminant is the id-1859 arm (State::armT), not the `robot ||
// mini cube` proxy: always so since 2026-09-26 (it was --bonkarm (removed)). See the bonk gate in
// step.hpp; the search key reads armT for it too.
// --witnessframe: the witness walk binds the geometry of the frame the
// call STARTS in, as --replay does, instead of frame 0. Not print-only: the walk also
// fills modeAt, which sets the emitted plan's edge latencies. See cli.hpp at rLf.
// Always on; the switch is gone since the flag clean-up.
// --vetophys (default off): a SOLVED whose correctly bound witness walk dies of a
// physical cause (hazard / solid-side / crush) is published as the PARTIAL at that
// death. See the demotion after the witness walk in cli.hpp.
inline bool g_vetoPhys = false;
constexpr int kResimLadderMax = 4;                        // searches per call
constexpr long long kResimLadderRungs[] = {6, 24, 96, 240};   // ticks before the walk's death
// --resimladderpartial (default off, and nothing without --resimladder): the ladder also takes a
// PARTIAL whose walk dies short of its frontier, and keeps a PARTIAL answer for any call. Without
// it only a SOLVED (or joined) plan is laddered, and only a SOLVED answer is kept. Measured on
// a custom level, new base 9d1f41ba: without the ladder 305 s, with it and this
// 509-517 s (two builds), a wall at t~10,300 that cost a 102 s section solve.
inline bool g_resimLadderPartial = false;
// --rlbound N (default 1500 since 2026-10-21, 0 = off): every rung of the resim ladder is first tried as a probe, a search
// with a horizon of (the rung's distance to the walk's death) + N ticks instead of the rest of the
// level. A probe whose walk dies again no later than the plan's does is the rung's answer (the
// full search would be 130-155 s on a level of 58,000 ticks to say the same); a probe that lives on
// is followed by the rung's usual full search.
inline long long g_resimLadderBound = 1500;
// --rlspread0 (default off): a rung's search starts from ONE state (the walk's, with the crossings the plan made before it),
// so a trigger crossed at or before the rung is crossed on one tick by every group of that search. Without the flag the
// rung keeps the spread (fireSpreadT) the first search measured for it, and a group worked back at its present speed
// puts its movers up to that many ticks behind the walk's -- the walk dies at the same place again, rung after rung.
inline bool g_resimLadderSpread0 = false;
// ...held by the cap ladder (cliMain) around an attempt whose answer the ladder may still replace:
// such an attempt's verdict is the ladder's to read, so a PARTIAL is left as it is and a SOLVED
// keeps only a SOLVED. Not reset per call; cliMain sets and clears it. On that level the same
// PARTIAL was laddered once per attempt, seven times over, at about 7 s each.
inline bool g_resimLadderHold = false;
// --dropnocollide: do not ingest id-1910 as a collidable solid.
//
// Measured (ledger run D): GD sets [obj+0x515] on lv22's uid 4705 (id 1910) and
// collidedWithObjectInternal returns false for it regardless of geometry -- 50 of 81
// hbin rows had the player's 9x9 inner box inside the object, one at full penetration,
// and hit was 0 on every one. Both neighbouring id-1 solids read [obj+0x515] == 0 and
// collide normally. The model ingests 4705 as an ordinary 15x15 solid and kills at
// t=4,889 on an object the game will never touch.
//
// The discriminator here is the ID, not the flag: +0x515 is NOT exported to objrects
// (it is absent from solver.hpp's column list), so the level data cannot express the
// real condition. id 1910 occurs exactly once in the corpus -- this object -- so the
// exclusion is one object wide, not a class rule. Match on the id COLUMN only: the same
// dump carries an unrelated uid 1910 (id 1268, type 20), and a loose numeric match would
// silently drop the wrong row. Always on; the switch is gone since the flag clean-up.
inline constexpr int kNoCollideId = 1910;
// --verdictinfo (default off): when a SOLVED plan's own witness walk died, print one
// `vinfo:` line carrying the verdict beside that death -- tick, cause, uid, object,
// frame. Print only. The verdict is NOT changed and the plan is NOT withheld: the
// direction is "send the plan, attach the known death as information" (audit 05:02),
// and the SOLVED/PARTIAL definition stays exactly where it is.
//
// Why it is worth a line at all: 23 of 423 witness walks in the 2026-09-08 cold run
// died, all on lv20/lv22, and none of those deaths matched GD's within +-30 ticks
// (0/18). So the witness death is not evidence the route dies -- which is precisely
// why this is information attached to a plan rather than a reason to demote it.
inline bool g_verdictInfo = false;
inline float g_resimPX = 0.f;   // the player's world x at the witness's first death
// ...and what killed it, taken at the FIRST dying tick. Without this, "the
// model died and GD died" can only be matched on the fact of a death, and two
// deaths at different places in the level read as agreement -- the model would
// be credited with knowing something it did not know.
inline const char* g_resimWhy = nullptr;
// ...and WHO, taken at the same tick. A cause string says `cube/hazard`; it does
// not say which object, and on lv20 the answer decides an open question: the
// model-visible hazards near the death (type 2/47 -- level_loader.hpp:923 emits
// nothing else) are all static, all `groups=0`, and none of them overlaps the
// player's box, so the killer has to be moving geometry. Naming it turns that
// from an elimination into an identification.
// The search's side of the same question the resim's `trig=` answers. The two
// walks apply triggers with different masks -- the group's (cli.hpp:3026) and
// the state's own (cli.hpp:4066) -- so a moving object can be in one place for
// the search and another for the resim, and on lv20 that is the whole
// disagreement. -1 = off; set to the tick to look at.
inline long long g_trigDbgT = -1;
// --coindbg <tick>: where the model put each coin at that tick, and which ones
// its mask left switched off (cli.hpp). The mod prints GD's own answer as
// `coinlive:`, so the pair SAYS whether the two disagree about a coin's
// position, where otherwise it can only be inferred from a refused credit.
// -1 = off.
inline long long g_coinDbgT = -1;
// One "I have this one" line per coin under --coindbg, not one per state: the
// search reaches the same collection from thousands of branches.
inline unsigned char g_coinSaid[8] = {0, 0, 0, 0, 0, 0, 0, 0};
// ...and one per pickup, for the same reason (g_collect is capped at 16).
inline unsigned char g_itemSaid[16] = {};
// --coindbg: the CLOSEST any state got to each coin while inside its x bound.
// "Never collected" has three readings -- the x bound was never entered, or it
// was entered but no state came within the y bound, or a state was inside both
// and the collect still did not happen -- and the layer lines cannot separate
// them, because they print a min/max ENVELOPE of y rather than whether any
// state sits in the coin's band. This is the number that does. -1 = the x bound
// was never entered.
inline double g_coinNearDy[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
inline double g_coinNearX[8] = {};
inline double g_coinNearY[8] = {};
inline long long g_coinNearT[8] = {};
// ...AND THE DENOMINATOR, because "never entered" is the shape of 0 this
// project keeps mis-reading: it reads the same whether no state entered the
// bound or the search never got that far. g_coinSeen is how many states were
// examined inside the x bound (0 is the interesting case) and g_coinProbeMaxX
// is the deepest x the coin test itself ever saw, so a 0 can be told from a
// "not yet" on the line that reports it.
inline long long g_coinSeen[8] = {};
inline double g_coinProbeMaxX = -1e18;
// How often does the group's collapse of the firing tick actually collapse
// anything? `gFire` is max(trigT) over the members (cli.hpp:2974) and the group
// key already pins `trig`, so the masks always agree -- the spread is in trigT
// alone. A group whose members share one trigT is not approximated at all.
// Counting the passes separately from the firings is the lesson of `!c.flip`,
// where a gate was entered 593 times and fired zero.
inline long long g_gfireGroups = 0;   // groups placed
inline long long g_gfireSpread = 0;   // ...of which the members disagree on trigT
inline long long g_gfireSum = 0;      // sum of (max-min) over those
inline int g_gfireMax = 0;            // widest spread seen, in ticks
inline int g_resimUid = -1;
inline float g_resimObjX = 0.f, g_resimObjY = 0.f;
inline TouchMask g_resimTrig{};  // the walk's own trigger mask at that tick
// ...and the FRAME the walk was in. g_resimObjX/Y are read in whatever frame
// the resim currently occupies (the loop re-binds rLf = &frameLevel(L,
// c.frame)), so two walks that report different positions may be reporting the
// same place seen from different frames -- on lv22 the two sides' killers turn
// out to differ by exactly a quarter turn, which was inferred from the numbers
// rather than measured. Reading the frame is what makes it a measurement.
inline int g_resimFrame = -1;
inline long long g_resimTicks = 0;
inline int g_resimFirst = -1;
inline int g_resimLast = -1;
inline void noteReconDeath(const char* why) {
    ++g_dieRecon;
    for (auto& e : g_dieReconWhy)
        if (e.first == why || (e.first && why && !std::strcmp(e.first, why))) {
            ++e.second;
            return;
        }
    g_dieReconWhy.emplace_back(why, 1LL);
}
// --hazdbg <uid>: trace ONE object down the ground-mode hazard loop in
// step.hpp, gate by gate. -1 = off, which is the default, so the shipped search
// is untouched and quick_regress stays byte-identical. A uid rather than a
// blanket switch because the search evaluates many states per tick and an
// unconditional print buries the answer.
inline int g_hazDbgUid = -1;
// --gravtoggle: the gravity toggle portal (GameObjectType 42, id 2926) is a gravity portal that
// asks for the other gravity (level_loader.hpp, the portal bucket; step.hpp, the portal pass).
// On by default since 2026-10-05 (the disassembly, two injections, a 19-unit rig that GD and the
// model flip on the same 36 ticks, a kitref of 15 fixed and 0 broken in 1,773 episodes);
// --no-gravtoggle is the off arm, where the loader leaves the portal out.
inline bool g_gravToggle = true;
// --teleporttrig: the teleport trigger (id 3022) moves player 1 when its x is crossed: to its
// target's position, x and y unless kept, with the gravity its mode asks for (modifiers.hpp,
// TpTrig; step.hpp, the end of the tick). On by default since 2026-10-06 (the disassembly and an
// 18-unit rig the model follows; on the corpus neither a first divergence nor a clear moved). One
// body in a plain frame moving right; a sideways one, a touch- or spawn-started one and one in a
// dual or a turned frame stay out. --no-teleporttrig is the off arm, where the trigger is not
// loaded.
inline bool g_teleportTrig = true;
// --widescan (default on since 2026-10): the x-window scan over a sorted-by-cx object list (state.hpp
// XSlice::forRange) moves its cursor and stops by each object's own edges, which follow cx only
// for objects of about one width: a wider one later in cx can still reach back into the window,
// and one behind the cursor can still reach forward into it. Under the flag the scan is exact: an
// object with a half width under kWideScanHw is caught by a rewind and a scan kWideScanHw past the
// break, and a wider one by its own list with a range test. A custom level's floor uid 3379 (2,250 px
// wide, centre x=2,835): GD's cube walks on it at x=2,061, and the model, whose scan never reached it,
// fell through. What the old scan reached is visited first and in the same order.
inline bool g_wideScan = true;
constexpr double kWideScanHw = 40.0;
// --startp2dx X: the anchor's dx2 (GD's p2x - x), read only under --p2xoff and only for a dual anchor.
inline double g_startP2dx = 0.0;
// --tppush: a teleport sets the body's velocity after the move when its static force (345) or its
// redirected force (347) is on -- the portals, and the teleport trigger under its own flag; an
// orb's is reported, not read (modifiers.hpp, TpForce; step.hpp, tpForceVy). On by default since
// 2026-10-06 (the disassembly and a 30-unit rig the model follows to its end; one body in a plain
// frame). --no-tppush is the off arm, where only lv22's gravity-switching 747 gets its exit vy
// (the fixed +18 in the portal pass).
inline bool g_tpPush = true;
// --moverextendvel (default off): the FALLBACK hypothesis of the same tail, terminal velocity with no cap and no Move data behind it. --moverextend is the Move-data form: it needs one of the object's own linear Moves with exactly that step and goes on only for the Move's remaining duration.
inline bool g_moverExtendVel = false;
inline constexpr int kRecEndExtendTicks = 2;
// --touchoff: a touch-triggered trigger does not fire while an x-crossing Toggle has switched off
// a group it sits in (step.hpp markTouched, TouchTrig::ownTog). GD (2.2081): toggleGroup
// (0x223bc0) keeps a per-object count of the groups switched off and sets the byte at +0x28e
// while it is negative, and collisionCheckObjects (0x214960) skips an object with that byte set
// at the top of its loop, before it hands a type-0x14 object to playerTouchedTrigger. SubZero
// 4003 is built on it: the twelve touch triggers of group 4 are switched off at x=1,815 and back
// on at x=2,535, so the player crosses them on the way right without effect and sets them off on
// the way back. On by default; --no-touchoff is the off arm.
inline bool g_touchOwnOff = true;
// --tapdisarm (default on since 2026-10): an x-crossing trigger that a box's chain has switched off before the player crosses
// it does not fire (dynamics.hpp autoDisarmed), and a Toggle's target group is not walked for the triggers it
// holds (triggers.hpp, the box chain). GD (2.2081): checkSpawnObjects (0x21a8f0) walks the x-sorted triggers
// with a cursor and calls triggerObject only for an object whose +0x28e (toggleGroup's "switched off") is
// clear; the cursor moves on either way, so a trigger passed while off never fires. A Toggle (1049) only calls
// toggleGroup (triggerObject case 0x419) -- it spawns nothing -- while a Tap (1595) switching its group ON goes
// through toggleGroupTriggered, which spawns the group as well. A custom level: a press past the Tap at x
// 37,200 spawns a Toggle that switches off the group holding the rig's 60-degree Rotate and the Toggle that
// brings its spikes in, and GD's cube passes where the model's met the spikes.
inline bool g_tapDisarm = true;
inline int g_nanSlopeDropped = 0;   // how many the loader left out (printed after the load)
// --nocolone (on by default since 2026-10-07; --no-nocolone is the off arm): a MOVING solid (it has a
// recorded timeline) whose [obj+0x515] byte is set
// is loaded as a one-way top instead of being dropped (level_loader.hpp), so the cube can stand on it
// and a snap seed can find it. Static ones stay dropped.
inline bool g_noColOneWay = true;
// --nocolstatic (on by default since 2026-10-13; --no-nocolstatic is the off arm): ...and a STATIC one
// too, in the unturned frame only (level_loader.hpp marks it oneway 2, frameLevel drops it from a turned
// frame). collidedWithObjectInternal gates the head-hit, side and push/kill branches on the byte but not
// the landing ones, so the top holds: a custom level's upside-down ball rides one from t=1,723 where the
// model fell through, and another's cube stands on one at t=670. In a turned frame (+0x9c3) the player
// passes it: lv22 t=4,899, uid 4705, where landing on it moved nine quick sections.
inline bool g_noColStatic = true;
// --rotboost: a gameplay rotation that swaps the vertical and horizontal axes raises GD's
// velocity-limit exemption (State::boost). rotateGameplay's swap block writes [+0x952] = 1 at
// 0x399faa unconditionally and lowers it again at 0x39a00d only when the trigger's Dont Slide
// (property 585) is set. No 2900 in official level 22 sets 585. On by default since 2026-10-06;
// --no-rotboost is the off arm.
inline bool g_rotBoost = true;
// --obbscale (on by default; --no-obbscale is the off arm): a portal or speed portal that is placed turned (oriented) and then SCALED by a Scale trigger is tested with its box scaled too. turnedBox redrew only the angle of an oriented box and kept the loaded halves (ohw / ohh), so a shrinking portal fired on the box it was placed with. a custom level t=24379: the robot portal uid 42639 (id 745, 34 x 86, rot 45) is shrunk by a Scale trigger from 84.9 to 75.7 (the bound's side, recorded by the rows); GD's oriented box (obb.txt corners scaled about the centre by the rect size over the loaded one) first meets the 10 x 10 wave at 24384 (depth 18.2), the loaded box at 24379 (0.55), and the model fired it at 24379, five ticks early.
inline bool g_obbScale = true;
// --capholdclass (default off): the frontier cap's per-class stride (search_dp.hpp classOf) also splits on the new model's holding bits (ShadowLite::hold,
// GD's m_holdingButtons[1..3]; 0 for a state the shadow did not step). Under --shadowstep the key keeps a parent's pressed and released children apart
// (slKeyBits) even where the press changed nothing visible yet, and the layer lists them as pairs (kid 0, kid 1) in parent order; a stride of about 2 over
// one class then keeps one side of every pair and drops the other side wholesale. lv20 with --shadowmini (removed): the mini cube stretch (t~11550-12400) is twice
// as wide (alive 68 against 34), and at cap 2000 the one line that survives later was thinned out at t=12448 (refwatch gate=cap); cap 8000 or
// --no-shadowmini (removed) solved it. The two children are never merged or pruned here: they only take their own share of the cap.
inline bool g_capHoldClass = false;
// --capholdpair (default off): the frontier cap (search_dp.hpp) samples the released and the pressed child of one parent as ONE cell while the press has not
// changed the body yet (same parent, y, vy, grounded, mode, size, flip, dual), and keeps or drops the two together, the way --heldcellcap does for the old
// model's button; a layer can then hold up to twice the cap. Under --shadowstep about 98% of a capped air layer is such pairs (lv20 from the anchor at
// t=11750: 985 adjacent sibling pairs in a layer of 2019 at t=12350), listed (kid 0, kid 1) in parent order, so the stride either takes one side of every
// pair or (with --capholdclass) half the bodies. lv20 with --shadowmini (removed) lost its line to the cap at t=12448 (refwatch gate=cap); with this the anchor
// solves at cap 2000. No child is merged or pruned: the pair only stops being split by the stride.
inline bool g_capHoldPair = false;
// --shadowsupply (on by default, --shadowstep only; --no-shadowsupply is the off arm): the world supply that the old step makes from the model's body moves to the new model's body. First the touch boxes (State::trig / trigT / fireB):
// shadowAdvance runs markTouched again with the shadow's mode, size and position, from the trigger words the model's tick started with, instead of keeping what the model's tick made at the model's position.
// (--shadowsupply=check, which ran it at the old tick's position and counted the differences from the old tick's result, went when there was no old tick to compare with.)
inline uint32_t g_shadowStepModes = 0;          // bit m: the model's mode m (0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing) is stepped by the shadow
inline bool g_shadowSupply = true;
// --shadowspiderjump (default off), P9 STALE fields / restructure draft 7.26: State::spiderJumpT (the search key inside the grace window) from the shadow body's own spider stamp (+0x828) and the PHASE the stamp was written in. The old tick counts from 0 at the fire and
// adds one at the end of that tick; the body's age (clock - stamp) is one more when the stamp was written by the collision pass (an orb or a pad touched in the pass: the head of the next step then ages it) and equal when a button wrote it (the pushButton ring scan). The phase is
// not in the stamp's value, so the shadow's own firing path writes one bit (ShadowLite::flags2 kSlSpiderPass) on the tick the stamp changes: the pass sets it, a head clears it, and it persists with the stamp. The old tick is not read.
inline bool g_shadowSpiderJump = false;
// --shadowdual (on by default, --shadowstep only; --no-shadowdual is the off arm): a dual of a one-player level is stepped by the new model with its player 2 (selfshadow_dual.hpp, the second block
// State::slRef2) instead of being left to the old model. Off: a dual is declined as before.
inline bool g_shadowDual = true;
// --ballstakeseed (default off): an anchor's State::ball728 comes from the hist payload's `ball728` (version 7, the mod's cfg
// `histball728`), GD's own ground-stake byte, instead of starting unknown. Unknown reads as "rolled since", and a size portal then
// re-stakes a ball turning at the air rate at the ground rate; whether GD does depends on the byte, which the spin cannot tell
// (a ball that rolled and was then flipped in the air turns at the air rate with the byte up). The spin-rate guess that
// stood here once was falsified on a custom level (a window re-staked by GD and 756 ticks lost to the guess).
inline bool g_ballStakeSeed = false;
// --fixkillworld (default off): a KILL fixup (fixup.hpp) is matched on the player's state alone, but what killed the player was an object
// in a world the plan made: a spike a toggle ring had left on. A plan that pressed the ring puts the same player state in a world where
// that spike is off, and the record killed it all the same - on a custom level four such records stood in the one corridor the game
// walks through (the same plan, minus one tap, lives to t=836 in the game and dies at t=804 in the model; without the records the
// model lives to t=811 and a second off-spike, not carried by the anchor, is the next wall). With the flag, a kill record that names its
// killer (the named tail `,kuid=`, written by the mod when this flag is among its dparg) is applied only while that object is there in
// the world of the state it is matched on: a moving object that is off in this world is not. A still killer, or a record without a
// name, applies as it always did.
inline bool g_fixKillWorld = false;
// --rotq64: the 2.2 rotation queue may hold up to 64 entries (State::rotSpent is a 64-bit set; it was 32 bits and a queue of 33 or
// more was refused outright, so the level ran without the queue and the loop got no seed for it). A custom level holds 49: its 2900s ran by the
// pre-queue rule, which does not follow the game's cursor.
// ON since 2026-10-27 (--no-rotq64 is the off arm): the same custom-level run, with the queue now accepted there.
inline bool g_rotQ64 = true;
// The toggle-ring chain (was --togarm, removed 2026-11-21; triggers.hpp): the chain a toggle ring (1594, "activate group") starts when the group it activates
// holds an x-crossing Toggle (1049) that an earlier Toggle switched off at the start: ring -> the group's trigger is armed -> the Toggle switches ITS group at its
// crossing. The ring is a box that arms the Toggle (--boxarm's way: TouchTrig::trigTog, AutoTrig::offInit), and a state that never pressed it keeps the objects as
// they stood (--tapdisarm's disarmEv). Before, such a ring was left out ("a chain this does not follow") and the Toggle fired at its crossing in every state, so the
// objects it switches were off past the crossing whether or not the state had pressed the ring -- and, being switched by an x-crossing Toggle only, they stayed on the
// recording, which may come from a flight that took another route (the bootstrap pass). a custom level (2026-11-21): two pairs of tiny spikes at x 1147.5 /
// 1162.5 that rings at x 943 and x 979.5 switch off; the bootstrap recording kept one pair on, every width of the first search died at t=805 and the next five rounds
// crept 5 ticks each (the run's own recording reaching one tick past each death). With the chain, one search of that call at cap 8000 passes 805 (to t=1447).
// A toggle block (1594) that a fresh press is made inside fires by the body's position when the press was handed out - the
// PARENT's, the previous tick's - as well as by where the step takes it (markTouched `parIn`). GD hands a press to the rings the previous tick's collision pass
// touched (PlayerObject::pushButton), so the body that has since moved a pixel clear of the block still fires it. Measured in the game on a custom level
// (a sweep of the tap tick over six ticks, the cube passing a 48 x 48 block's lower edge, 38.22 px from its centre in y on the tap's own row and 39.32 on
// the next, half sum 39): the block fires for a tap on the row at 38.22 and not for one on the row at 39.32; the model, which read the row after the
// tap, fired a tick earlier on both counts. Always on since the flag (--pressparent) was folded into the default; GD's fire dates of the 208 toggle rings
// (1594) in the 64 customs' flights (the touchseed / ringseed maps) are the press tick itself, in every mode (2026-11-18).
// --touchrearm (default on since 2026-11-20, --no-touchrearm is the off arm): a toggle ring (1594, "activate group") whose group holds touch-triggered Moves / Toggles that an earlier x-crossing Toggle
// switched off (--touchoff) is not left out: pressing it switches their group back on, so the touch rows fire on contact again - in a state that has
// pressed the ring, and only there (TouchTrig::ringArm, read by touchOwnOn against the state's own fire bits). Until now such a ring was "a chain this
// does not follow", the rows stayed off past the Toggle in every state, and what they move or switch off (here a hazard's group) kept its recorded
// position. A custom level: the ring at x=1907 re-arms a Move that lifts a spike's group 39 px and a Toggle that switches it off; the game fires
// both at t=1352 on the press at 1350 (the spike's group off from 1352), the model's recording had the spike in the corridor and killed the run.
inline bool g_touchReArm = true;
// --tracehold (default off, a diagnostic): the replay trace gains the columns jb, ps, rh, hd -- State::jumpBuf, pressSpent, ringHold, holdDead after the
// step -- which the press / hold comparison against the game's own bytes (mod cfg hstrace=1) reads. Not an input to anything.
inline bool g_traceHold = false;
// --hsshadow (default on since the jumpgate flip, --no-hsshadow for the off arm; an observer by itself): State::gdHS keeps GD's own press bytes -- H (m_jumpBuffered), S (m_stateRingJump), their mirrors Hm / Sm, the ring slots and
// RF -- written ONLY by the table's writers (notes/pushbutton-state-table-2091-1027.md: a press, a release, a ring by its type, the jump arms, the ball's tap, the
// UFO's flap, a spider's teleport, a dash stop) and read by nothing the physics does. With --tracehold it adds the columns gh, gs, ghm, gsm, gslot (after the step)
// and ghb, gsb (just after the step's button), which hs_compare.py sets against the game's hstrace.csv. (The count of rings fired through a closed Hm / Sm / slot
// gate, and the gate probes, went with the old tick that made them.) --hsp22 N is player 1's m_enable22Changes (a spawn orb clears S only if 1; the
// game's levelflags.txt, written beside hstrace.csv, says; the default 1 is the official levels' lv1-21).
inline bool g_hsShadow = true;
// --ringslots (default on, --no-ringslots is the off arm; needs --hsshadow, which --no-hsshadow turns this off with): a ring's gate is read from the shadow of GD's own bytes, not from the model's pressSpent / jumpBuf. pushButton scans the
// touching array only on a press that reaches the player (the early rings of a ship / UFO and of a body on foot); the collision pass fires a ring only on foot, and only
// while the mirrors Hm and Sm are up (the update's tail copied H and S after the held-button consumers spent theirs) and the ring's class slot is free. Changes behaviour
// only where the model's bookkeeping and the table's writers disagree about whether the press is still alive (notes/pushbutton-state-table-2091-1027.md, D1 D8 D9).
inline bool g_ringSlots = true;
// --no-ringslotskey (an A/B arm of --ringslots only): leave the shadow out of the search key. The gate then reads bytes the dedupe does not tell apart, so two states the
// search merges can answer the next ring differently; it exists to separate "the gate reads the shadow" from "the key changed" when a level behaves differently.
inline bool g_ringSlotsKey = true;
// --holdrecbox (default on, --no-holdrecbox is the off arm; needs --rectouch): the punch hold (Dynamics, the recAuto object's "the recording, held min(60, t - F) ticks") skips a box the recorded attempt fired itself on that
// same tick (slamBoxRecorded, the test the slam steps use), when the rows are that attempt's own (formulaRecProvOk: --recprov names one attempt for the recording and the anchor, within its reach). The hold is the measured answer to a yellow punch the recording does not contain; a box GD fired in the recorded attempt IS in the recording's rows, and holding
// the rows for it reads them that many ticks early. A custom level's plate (uid 8917: boxes 13596-13599 in pairs, a -6 and a +6 Move on one group): the box at 5,796 is the recording's own, the hold took 20 ticks at
// t=5,816 and the model read the row of 5,796 (cy 3274.121) where GD, and the row of 5,816, say 3271.766; the ship it killed at 5,816 lives to 6,061 in GD.
inline bool g_holdRecBox = true;
// --hsmergeaudit (a diagnostic, serial dedupe only): how many children the dedupe throws away (one that lands between the kept highest- and lowest-vy of its key, or
// the one a new extreme replaces) whose shadow signature -- H, S and the carried consumers of each body -- differs from BOTH survivors of that key. With the shadow out of
// the key (--no-ringslotskey) those are the states the gate would have answered differently and the search folded away; the count says how much the key matters.
inline bool g_hsMergeAudit = false;
inline std::atomic<long long> g_hsMergeDropped{0}, g_hsMergeDroppedDistinct{0}, g_hsMergeReplaced{0}, g_hsMergeReplacedDistinct{0}, g_hsMergeStored{0};
inline int g_hsP22 = 1;
// --tracecontact (default off, a diagnostic): the replay trace gains the columns snapuid, slopeuid, gunid -- the unique IDs of the solid stood on
// (State::snapObj), the ramp ridden (State::slopeUidNow) and the stick re-land's solid (State::groundUid) -- beside the clamp columns it already has,
// which the contact comparison against the game's own record (mod cfg ctrace=1, tools/contact_cmp.py) reads. Not an input to anything.
inline bool g_traceContact = false;
// --partialbyx (off): a Partial answer (the search died without a goal, a horizon call) emits the layer whose leader got furthest in x over the whole search, not the last non-empty layer (search_dp.hpp).
inline bool g_partialByX = false;
// --partialphys (off): a Partial answer whose last layer ended ONLY by the out-of-play bound (every child of it died as kDeathPrune, none by the game's rules) emits the parent of the latest physical death instead (search_dp.hpp). That layer is a body flying out of the level, which GD takes to the ceiling and kills there (lv22 R7: nine of 55 rounds, each a replay that learns nothing); the physical-death parent is where the model says the game kills it, which a replay confirms or refutes.
inline bool g_partialPhys = false;
// --skyprune (off): a shadow death with no killer object (whyUid < 0) at y >= --maxplayy - 1 (GD's ceiling kill, the loop's skyKill) is classed kDeathPrune, so that --partialphys sees a frontier that ended in the sky as ended by the out-of-play bound only (a custom level: an upside-down cube climbs at terminal speed for 8,300 ticks with no input to a ceiling death, and is the only branch left, so every Partial was that flight and the honest wall never got flown).
inline bool g_skyPrune = false;
// --ladderbyx (default off; needs --partialbyx): the cap ladder keeps the attempt whose emitted layer got furthest in x (SearchOutcome::deepX) and compares the plain search with it by x, not by the tick the frontier died on
// (deepT). The tick is the sky-escape lane's: one attempt whose cap window cut the real lane left a climbing lane as its furthest layer (lv22 x=4445.9 at t=3063, against 5229.1 at t=2820 for the others), and the
// ladder took it for the deepest and kept it over a plain search that reached 5229.9 (--shadowframe, the 8f223b4f package's call at t0=857).
inline bool g_ladderByX = false;
// --ladderlevelcache (off): an attempt of a cap ladder after the first takes a copy of the Level the first one loaded (cli.hpp LadderLevelCache) instead of loading it again. Off, every attempt loads its own, as the first
// did. The copy is not equivalent to a load under --shadowstep=all: lv22's first loop call (t0=231, --ladderprefixcheck) has attempt 2 from t0 leave attempt 1 at t=751 (a kid's pressSpent / spiderJumpT / pSpiderTap /
// holdDead / gdHS, the parent byte for byte the same) although the two maps agree until t=2675, and its MISMATCHes are gone with a Level loaded afresh -- even one whose copy is thrown away straight after, so what
// differs is a side effect of the load that the cached path (LoadLevelWrites) does not repeat. The load is 0.2 s here, the attempts seconds to minutes.
inline bool g_ladderLevelCache = false;
// --shadowcolderive (off; observation only, P9 / the collision family): after the shadow stepped a tick, derive the collision family's State fields from the body the pass left (m_objectSnappedTo, m_snapDistance, m_isOnSlope / m_wasOnSlope,
// the slope clock +0x598 ...) and count how often they equal the old tick's (shadow_step.hpp ShadowColStats). Nothing is written to the State.
inline bool g_shadowColDerive = false;
// (P9: the shadow without the old tick -- was --shadowfroms, folded in; dp/removed_flags.txt): the eligibility / control-gate decisions that look at the dual, reverse and frame bytes and the trigger channel read the state before the tick instead of what the old tick left.
// --shadowsdiff (off, observation): count, per field and for the eligibility decision itself, the ticks the two readings differ in.
inline bool g_shadowSDiff = false;
// (the touch words the gravity-box test starts from, when the old tick kept none -- t_trigBefore invalid -- are the state before the tick's, s.trig, not the old tick's output, c.trig: was --shadowfroms2, folded in; dp/removed_flags.txt)
// (the seed check -- a tick the shadow was seeded from the model's state, declined when the old tick died or the shadow's body differs from the old tick's -- no longer declines: the shadow's answer stands; the reasons are counted either way (--shadowsdiff). Was --shadowfromseed, folded in; dp/removed_flags.txt)
// --shadowseedtol X (default 0.01, as before): the seed check COUNTS (--shadowsdiff) a tick whose shadow body is more than X px (position) or X (vy) from the old tick's. The seeded shadow and the old tick differ by 0.0322 px for hundreds of ticks on a UFO riding the ceiling
// (a custom level t=12868..13319, the old tick's clamp); every decline re-seeds the next tick from the State, so a small constant gap kept the shadow out for the whole run.
inline double g_shadowSeedTol = 0.01;

// --shadowcoldump FILE (diagnosis, off; P9 collision family): one record per shadow-stepped single-player tick -- t, the collision family's State fields as the old tick left them, and the body's first 0xc80 bytes after the pass -- for a tool that
// looks for the body offset each field equals (tools/col_map.py). Single-threaded runs only.
inline std::string g_shadowColDumpPath;
// --shadowquietdump FILE (diagnosis, off; P9 "quiet ticks"): one 128-byte record per shadow-stepped single-player tick -- which State fields the old tick changed (bit per stser field), which fields of the state before differ from the default,
// the shadow's own signals (objects it overlapped, the body before and after the pass). Single-threaded runs only.
inline std::string g_shadowQuietDumpPath;
// --shadowrotbook (default off; restructure P9, gd-04; needs --shadowrotapply, --rotqueue, --rottouch): the touch half of the rotation queue's bookkeeping (which slot a touch Rotate Gameplay spends, with its channel / reverse bits and the turn) is decided by the shadow's pass -- the slots it turned the body with -- on a tick the shadow stepped; applyRotation's rotTouchHit no longer decides it there (a channel-only slot, and a tick the shadow did not step, stay the old tick's).
inline bool g_shadowRotBook = false;
inline thread_local bool t_rotBookOn = false;       // the shadow stepped this tick normally (set at the end of shadowAdvanceImpl)
inline thread_local uint64_t t_rotBookBits = 0;     // the queue slots (g_rotQ) the shadow's pass turned the body with this tick
// --spinseed (off; needs the hist payload's version 8, the mod's cfg `histseed2`): an airborne cube's rotation rate (m_rotationSpeed, +0x728) and its switch (+0x730) at the anchor are GD's own, not runNormalRotation's standard spin. After a ramp exit GD turns the cube the other way and
// slower (-207.692 deg/s = -0.865 a tick against +1.73): a custom level t=7000, whose rotated OBB then misses a thin rotated portal (the mod reads the float; the payload carries it in 1/1000 deg/s). Applies to the seed of the anchor's first step only, to player 1.
inline bool g_spinSeed = false;
inline bool g_seedSpinKnown = false;
inline int g_seedSpin = 0;
inline long long g_seedSpinT = -1;
inline long long g_seedHistT0 = -1;   // the anchor tick of the hist payload that carried the press bytes (-1: none)
// ...the second body's gravity factor the hist payload (version 10, `gmod2`) names at the anchor, in 1/1000, and the anchor's x: the factor holds until a player-2 x-crossing trigger beyond that x takes over
inline bool g_seedGmod2Known = false;
inline int g_seedGmod2Milli = -1;
inline double g_seedGmod2X = 0.0;
// --seedslope2 (off; needs the hist payload's version 11, cfg `histslope2`): the SECOND body's slope ride at the anchor, as GD had it (`slopeOn2` / `slopeUnder2` / `slopeUid2` / `slopeAge2` / `slopeLanded2`), seeds State::onSlope2 / slopeT2 /
// rideLanded2 / slopeM2 / slopeUid02 / slopeUidNow2 / seatT2 (the same regimes --anchorride gives player 1: floor-landed, floor-seat) in place of the geometric guess, and lets the shadow's second body take the ride's words (--shadowseedslope's block
// for player 1 only). 16:7800 (dual cube, both bodies on mirrored ramps, both leave at 7818): the payload said only player 1's ride, so the shadow's player 2 had none and fell. Needs --shadowseedslope for the shadow's words.
inline bool g_seedSlope2 = false;
// --shadowdashage (off; with --shadowstep, --shadowseeddash, --spinseed-style hist payload version 9): an anchor inside a dash starts with the age GD's dash action has, not 0. The payload's `dashAge` is the number of ticks since the dash's first row (0 = the anchor's own tick is that row, -1 = not said); the frames the action has run since, one head each after the first (which counts nothing), are summed at the anchor's warp (--shadowdashsched) into the shadow's dashElapsed / dashRot. a custom level t=20600: the dash began at t=20582 and GD hands 96 at the stop, the unseeded shadow 16.
inline bool g_shadowDashAge = false;
inline bool g_seedDashAgeKnown = false;
inline int g_seedDashAge = -1;
inline long long g_seedDashAgeT = -1;
// The anchor's CEILING-side ramp ride (hist payload slopeUnder = 1, --anchorride's "underside" regime): the model keeps it as ceilT / ceilM4 with onSlope = 0 and no ramp uid, so the shadow's seed (shadow_step.hpp, the ride's clock +0x598 and its words) had nothing to write and the first pass re-stamped the ride (lv16 8901: GD's ride is 30 ticks old at its exit, the shadow's 14 -> launch 4.001 where GD has 6.906). The ramp's uid and the anchor tick, read where --anchorride seeds ceilT; consumed by the shadow seed at t0 / t0 + 1 only.
inline int g_seedUnderUid = -1;
inline long long g_seedUnderT = -1;
// ...and the same for the SECOND body (--seedslope2's "underside" regime: ceilT2 / ceilM42 and no ramp uid in the model), read where it seeds ceilT2; consumed by the second body's shadow seed at t0 / t0 + 1 only. lv16 t0=8901: GD's p2 leaves ramp uid 3868 at t=8,913 with vy2 -6.618 = 6.906 x 23/24.
inline int g_seedUnderUid2 = -1;
inline long long g_seedUnderT2 = -1;
inline std::vector<std::pair<int, int>> g_shGpUid;   // the portals (the objects with an Obj::gpBit: gravity, teleport, toggle) as (bit, uid), filled by shadowStepInit under the flag
inline int g_startP2Left = 0;
// --startrhover2 N (a start option, 0 = absent): the second body's robot hover budget at an anchor (State::rHover2); the --start fields carry the first body's only. a custom level t=12600: GD's player 2 is a robot mid-hover (H = 1, y falls 1.263 a tick),
// the shadow's player 2 starts with no budget and falls with gravity.
inline int g_startRHover2 = 0;
// A mode portal seats on its own tick: GD runs the activation pass before the
// solid pass. (--no-portalseat is gone since the flag clean-up.)
// Force fields add AFTER the terminal clamp, unclamped (PlayerObject::update, not
// updateJump). (--no-forceorder, the pre-2026-09-05 order, is gone since the flag clean-up.)
// A same-frame 2900 turn that changes the polarity halves vy: flipGravity's
// mulsd 0.5 @0x39a2dc. (--no-rot2900halve is gone since the flag clean-up.)
// A gravity portal latches on its first OVERLAP (hasBeenActivated).
// (--no-portallatch, fire on every pass, is gone since the flag clean-up.)
// A ceiling ramp acquires from below: GD's underside gate takes the player
// under the line within tol_u (the site in step.hpp). (--no-ceilseat is
// gone since the flag clean-up.)

// A ramp already governing the surface vetoes the solid (slopeVetoesSolid:
// 1,673 of 1,673 rig ticks, all 14 corpus ticks). (--no-slopeveto is
// gone since the flag clean-up.)

// A FRESH ramp contact passes GD's inset-rect reach test first (0x38fc0e,
// 0x38fc3c-0x38fc7b; slopeWouldAcquire). (--no-slopefreshrect is
// gone since the flag clean-up.)

// --no-slopeseat: a ramp seats the player at the surface sampled at an x
// CLAMPED into the ramp's span, plus/minus a flat player half (the
// pre-2026-09-06 behaviour). GD's own seat is
// `slopeYPos(centreX) -/+ playerRect.h/(2 cos t)` -- the line EXTRAPOLATED past
// the span, with the bounds applied to the TARGET y instead
// (0x38fd42-0x38fdf0, re-evaluated on every acquiring tick 0x39072c) -- see
// slopeSeatTarget in slopes.hpp for the formula, the RVAs and the lv16 t=9,241
// witness. The two agree everywhere except on the ENTRY side of a ramp.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)

// The UFO's floor acquisition allowance used to be kShipLandTol (6.0), before
// 2026-09-06 (the --no-ufolandtol arm, gone since the flag clean-up). That 6.0 was never measured -- the
// site's own comment said so ("kShipLandTol (6.0) stays for the UFO, whose own
// acquisition has NOT BEEN MEASURED this way"), the ship having been measured
// down to 0.001 while the UFO was left alone.
//
// GD's floor gate is `acquire <=> s*targetY > s*y - (extraTol ? tol : 0)`
// (@0x38fea5) and its only widths are 0 / 1 fresh / 2 continuing, selected by
// extraTol and m_wasOnSlope. 6.0 is not among them at any setting.
//
// Witness lv19 t=14,587 (UFO, uid 9522): targetY 315.928 against a free y of
// 317.298, so gap = 1.370. GD refuses -- `315.928 > 316.298` is false -- and
// seats a tick later once the line rises to meet the player. The model admits
// it through the main test (1.370 <= 6.0) and pulls the player 1.370 px DOWN
// onto a line it has not reached, one tick early. The 1.370 also settles which
// width is live: 2.0 would admit it, so fresh/continuing is load-bearing here.
//
// The corpus reach was counted before the change: of 3,424 UFO rows, 1,612
// carry the 6.0, and the rows this narrows are 4 -- of which exactly ONE is
// actually seated (the witness). No changed row is admitted by another
// disjunct, so nothing keeps its seat by a second path, and no UFO row anywhere
// has a gap in (0, 1.0], which is the only interval where "assume the band
// applies" and "clamp to zero" could have disagreed.

// A NORMAL-SIZE UFO that flaps while seated on a floor ramp leaves at this
// value instead of its plain 6.871. Measured on the calibration rig
// `calib_ufojump` (14 cells, worker 99): the two control cells, pressing
// on the flat run-up, return 6.8710 and 6.6480 exactly, and every one of the
// six normal sweep cells returns 8.0000 -- at |m| 0.5, 1 and 2, and at both 1
// and 3 ramps. So it is an ASSIGNMENT, not a bonus added to the jump:
//   * gradient does not enter it (three gradients, one value),
//   * ride time does not enter it (ramp factor 0.674 vs 1.000, same value),
//     which refutes every slopeRampFactor-shaped hypothesis outright,
//   * and it is not the cube's on-ramp bonus (kSlopeJumpBonus), whose |m| ratio
//     is 1.4192 where the measured ratio is 1.0000.
// The bonus this implies, 1.129, is well under the 0.4x cap (2.748), so the cap
// is not what flattens it.
//
// MINI IS DELIBERATELY NOT FIXED and stays wrong. The same rig gives a mini
// 9.3680 at |m| 1 and 2 and 8.1470 at |m| 0.5, against the model's unchanged
// 6.6480. Two rules fit those three points equally well -- a threshold at
// |m| >= 1, and the ramp's width -- and they cannot be told apart, because the
// 22 levels contain exactly three slope shapes ((30,30), (30,60), (60,30)), in
// which |m| >= 1 <=> w = 30 without exception. Separating them needs a 60x60 or
// a 30x15 slope, i.e. an object these levels never use. Guessing between the
// two would put a width rule into the model under a gradient's name.
// (The --no-uforampflap arm is gone since the flag clean-up.)

// --shipheldflap: a ship->UFO portal buffers the flap for a held button too, as
// wave->UFO already does. Measured on lv14 (2026-09-18, cfg `presstrace`): the
// press at t=14,189 sets +0x985/+0x986, the SHIP's updates leave +0x986 alone for
// two ticks, and the UFO's first update after the portal (id111 at 18461) consumes
// it and flaps (vy 0.788 -> 6.648); released before the portal, +0x986 is cleared
// with the button and nothing fires. The model buffered only a fresh edge on the
// portal tick, so a press one tick before the portal was lost.
// A press that STARTED in a consuming mode is excluded through pressSpent: on the
// rig calib_heldflap (2026-09-18) a grounded cube pressed two ticks before the ship
// portal jumps, +0x986 goes to 0, and held through ship into UFO it stays 0 and
// nothing flaps; pressed inside the ship, the same hold flaps (vy 6.871) on the
// UFO's first update. wave->UFO has no such gate and is unmeasured for it.
// On since 2026-09-19, with the anchor's press-latch payload that feeds it;
// always on since the flag clean-up.

// --wavespentgate: the same gate on the wave->UFO buffered flap, which is on by
// default. The wave twin of calib_heldflap (2026-09-18, presstrace): pressed inside
// the wave and held, the UFO's first update flaps (6.871); pressed as a grounded
// cube (the jump spends +0x986) and held through the wave, +0x986 stays 0 and the
// UFO does not flap (vy 1.298 -> 1.212) -- where the model flapped to 6.871.
// On since 2026-09-19; always on since the flag clean-up.


// GD resolves ramp-then-solid (the model's own order, every solid first, was the
// --no-rampfirst arm, gone since the flag clean-up). GJBaseGameLayer::checkCollisions (0x2137f0) is two passes:
// the bucket scan resolves slopes (type 0x19) and teleports (0x1c) in place,
// while solids (type 0 / 0x15) and hazards are only pushed onto a list and
// resolved afterwards through PlayerObject::collidedWithObject (0x214687), so
// in GD a solid always sees the seat the ramp already wrote. See the rule at
// the end of the slope block in step.hpp for the witness (lv16 t=4,142) and
// for what it deliberately does not cover.

// A ramp contact takes GD's own V3/V4/V5 velocity writes (slopeNudge in
// slopes.hpp). (--no-slopenudge, the pre-2026-09-06 +-2.0 / kShipRampG / walkIn0
// rules, is gone since the flag clean-up.)

// The flight loop's mover-catch (`fly/mpush`) seats within GD's reach-back
// `kShipLandTol + |dcy|/0.25`. (--no-mpushreach, any penetration, is
// gone since the flag clean-up.)

// GD's velocity-limit exemption (State::boost) is carried by ship, UFO and
// swing (swing only before 2026-09-06; --no-boostlatch is gone since the flag clean-up). The byte [player+0x952] has no
// mode test at any of its ten writers and is read by the ship's acceleration
// selector (0x38c5be / 0x38c5d8) and by the terminal clamp shared by ship, UFO
// and swing (0x38ca9f), so the real scope is those three -- see boostLatchMode
// in slopes.hpp for the reading and the sites in step.hpp for the consumers.

// When several rings touch on the same tick GD fires the one touched FIRST (the
// lowest uid, before 2026-09-06, was --no-ringfirsttouch, gone since the flag clean-up). GD's container
// `PlayerObject +0xa38` (m_touchedRings) is appended to only on a ring's FIRST
// contact (playerTouchedRing 0x217e40, containsObject 0x217ea1 / addObject
// 0x217eb5) and is pruned in place, order preserved, at every tick's head
// (resetTouchedRings(p,0) 0x3982e0), so pushButton's forward walk (0x3980b7)
// fires the FIRST-TOUCHED ring; ascending uid is only the same-tick tie-break
// (the bucket sort 0x2143a6 / comparator 0x205210). See the site in step.hpp.

// (--no-padobb, the raw player rotation for a rotated pad, is gone since the flag clean-up.)

// The BALL's hang window takes one tick's movement |useDx| as the grace on its
// exit end (a constant 1.5 before 2026-09-06; --no-ballcorng is gone since the flag clean-up), which is what the ship (r68/r74) and the swing (r71c/r74) already
// use. Two witnesses bracket the grace from opposite sides and leave no
// constant available:
//   lv17 t=18,571/18,572  full ball,  m=-1, x1=24,120, xoff 6.213, dx 1.29825
//                         last ride cx 24,113.838 / first drop 24,115.137
//                         => 0.051 <= G < 1.350
//   lv16 t=13,116/13,117  mini ball,  m=-1, x1=19,872, xoff 3.728, dx 1.614258
//                         last ride cx 19,869.660 / first drop 19,871.273
//                         => 1.388 <= G < 3.001
// The two intervals are disjoint, and |useDx| lands inside both, so these two
// ticks do not merely permit a dx-proportional grace, they require one. The
// reverse-travel mirror (okLo) is NOT converted: it has zero witnesses in the
// whole corpus, and an untested arm is better left where it was measured to be
// harmless. See the site in step.hpp.

// A pad's same-tick rotation step turns toward the gravity at the moment GD
// called runNormalRotation (116 witnesses, the site in step.hpp). (--no-padspinpre,
// the end-of-tick gravity, is gone since the flag clean-up.)



// Which modes may hand the pad gate the player's real rotation.
//
// GD turns the player's square in EVERY mode; this list is a hedge against the
// MODEL's own rotation, not against GD's. Handing the SAT an angle the model
// does not track would make the test worse than the axis-aligned square it
// replaces, so the list is derived, not chosen:
//
//   * `State::rot` is written in exactly four places -- the wave's easing
//     (step.hpp, `kWaveRotK`), the mode-portal edge table, the cube's spin and
//     the ship's bank, and the ball's `rotStep`. The rotation block itself is
//     `if (c.mode == 0) ... else if (c.mode == 1) ... else if (c.mode == 2)`,
//     plus the wave inside its own branch. So modes 3 (ufo), 5 (robot), 6
//     (spider) and 7 (swing) carry rot == 0 for their whole life (the edge
//     table writes 0 on entry for every mode but cube and ball, and nothing
//     advances it), and for them this list cannot change a single digit
//     whichever way it is written.
//   * Of the four modes that DO carry an angle, the rate census over the whole
//     corpus (415,581 ticks, |d rot/tick| error > 0.01 deg) reads cube 3.7% /
//     ship 13.8% / wave 21.1% / **ball 70.3%** -- and the ball's number is
//     categorical rather than a coefficient error: its air rate is a stake only
//     `flipGravity` and `ringJump` write and the model does not carry one, so a
//     ball that left the ground off a step or a pad is turning at a rate that
//     is simply not GD's. Sweeping the threshold from 0.001 to 1.0 leaves the
//     ball at 70.3% (categorical) while ship and wave collapse (boundary).
//
// So the only load-bearing exclusion is the BALL. UNWITNESSED EITHER WAY: all
// 21 corpus contact rows against a rotated pad are mode 0, full size (census
// below), so nothing here is measured -- it is the shape of the hedge.
inline bool padPlayerRotMode(uint8_t mode) {
    return mode == 0 || mode == 1 || mode == 4;   // cube / ship / wave
}

constexpr double kSpiderGScale = 0.6;
constexpr double kSpiderHalf = 13.5;
constexpr double kSpiderHalfMini = 8.1;   // 13.5 * 0.6, UNVERIFIED
// ...and the teleport SEARCH is 1.0 px wider in x than that. The lv22 note above
// could only say "at least as wide as the player's own box"; lv21 pins it.
// Measured 2026-08-10 on lv21's flipped spider resting at y=406, tapping next to
// the block (11791,375) whose top is 390 (so the near target is y=403.5 and the
// far one is y=223.5, 180 px apart -- the level's whole route turns on which).
// Seven injections of the player's x through the MCP, reading GD's landing:
//   |x-cx| = 28.799  29.299  29.499 -> 403.5 (near)
//   |x-cx| = 29.510  29.650  29.799  29.999 -> 223.5 (far)
// so the boundary is 29.5 to within 0.011 px, i.e. hw + 14.5 for a 30-wide
// block. The model was using hw + kSpiderHalf = 28.5 and took the far target
// on every tick in the 28.5..29.5 band.
// NOT SEPARABLE from this one site: "the spider's x half is 14.5" and "the box
// is 15 wide and GD wants more than 0.5 px of overlap" are the same formula
// (hw + 14.5) for every object. Only the vertical half is independently pinned
// at 13.5, by the rest positions (403.5 = 390 + 13.5, 223.5 = 210 + 13.5).
// Applied to the teleport search ONLY, which is where it was measured. The
// support test a few lines up has the same shape and may well want the same
// number, but that is a second variable and lv1-20 cannot referee it (nothing
// before lv21 has a spider).
constexpr double kSpiderSearchHalfX = 14.5;
constexpr double kSpiderSearchHalfXMini = 8.7;   // 14.5 * 0.6, UNVERIFIED
// The wave has TWO different sizes, and they are not the same number.
//
// The BAND clamp keeps its centre 10 off the surface: measured on lv17 t=3565,
// it comes down the diagonal and stops dead at y=100.000 (vy=0, onGround=1)
// on a band whose floor is 90, then slides for 30+ ticks. That 10 is the
// clamp's own constant (the same slot that holds 15 for everyone else), not a
// hitbox.
//
// Its HITBOX is far smaller -- about 5. Two independent brackets on lv17:
//   * the ball portal at (11055,197) w=34 fires at x=11033.47, and the tick
//     before is 11032.17, so half is in [4.53, 5.83)
//   * the ship portal at (8865,195) h=86 spans y=[152,238] and the wave rides
//     past it at y=140.25/141.54 WITHOUT switching, so half < 10.46
// 5.0 is the only round value in both. This half is what portal, pad and orb
// contact use as well -- with 15 the model entered that ship portal four ticks
// early and came out in the wrong mode while GD was still a wave.
// The mini values carry the established 0.600 ratio and are UNVERIFIED.
// CONFIRMED against GD 2026-08-06: injecting the player at y=600 in lv20's wave
// section and reading where GD puts it back gives y=380.000 at three x well
// apart (4,550 / 5,121 / 5,199). The band there is the x=867 wave portal's
// (cy=239 -> floor 90, ceil 390) -- the second wave portal at x=4,445 is INERT
// because the player is already a wave, so it never rewrites the band. 390-10
// = 380 exactly. 10 it is; pHalf (5) would give 385 and 15*vsize would give 375.
constexpr double kWaveClamp = 10.0;
constexpr double kWaveClampMini = 6.0;
// The side/crush kill's grace after a gravity flip: GD stamps the flip time in
// flipGravity (player+0x800) and collidedWithObjectInternal (:1180-1226) takes
// the kill arm only when `now - stamp >= 0.1` s, otherwise re-seating the
// player on the face and calling hitGround.
//
// MEASURED IN THE GAME, lv18's blue pad at (24375,273) and the two 30x30 blocks
// under and beside it (2026-09-01, worker 98). The player is injected 2.5 px
// into a block's top face -- the inner box crosses it, the outer box does not
// resolve it -- and the only thing varied is how long ago gravity flipped:
//     no flip at all                         DEATH
//     flip on the same tick                  LIVES, re-seated to top+15, og=1
//     flip 12 ticks earlier                  LIVES, og=1
//     flip 24 ticks earlier                  LIVES
//     flip 25 ticks earlier                  DEATH
//     flip 36 ticks earlier                  DEATH
// so the window is 25 ticks counted from the tick the dump first shows the flip
// -- 0.1 s x 240 Hz = 24, plus one because GD stamps the time after that tick's
// row is written. Counted the model's way (flipT = 0 on the tick gdUpOf
// changes), the grace holds while flipT < 25.
//
// SIMPLIFICATION: GD measures TIME and this counts TICKS. They agree everywhere
// the corpus goes except inside a timewarp zone (id 1935), where the substep
// density changes; lv22's is the only one.
constexpr int kFlipGraceTicks = 25;
// Ages (State::spiderJumpT) for which a spider is spared a solid's side kill: now - jump < 0.04 s.
constexpr int kSpiderJumpGraceTicks = 10;
inline int spiderJumpGraceTicks() {
    return kSpiderJumpGraceTicks + 1;
}
// How long an id-1859 touch keeps the ceiling resolution armed. GD sets its
// counter (player+0xb7c) to 2 on the touch and decays it once per tick, so the
// arm covers the touch tick and the one after it. See modifiers.hpp's
// ArmBox for the three measurements that pin the object as the real
// discriminant.
constexpr int kArmTicks = 2;
constexpr double kWaveHalf = 5.0;
// ...and the MINI one is NOT 0.600 of it. That carry-over was flagged
// UNVERIFIED above and this session refuted the same 0.600 twice already (the
// kill box does not scale at all, the ramp box scales by ~0.70). GD brackets it
// on lv20's 1.1x speed portal uid4303 (rot 41, real box 51 x 56 -- obb and
// w0,h0 agree, so the oriented shape is not in question). Writing the fire test
// on the portal's own long axis, the threshold is `25.5 + 1.41077 * half`:
//   x=6128.008 y=298.316  |lx| = 28.979  GD does NOT fire  -> half <  2.466
//   x=6131.903 y=300.912  |lx| = 27.743  GD DOES fire      -> half >= 1.590
// (both read off a plain replay's dump, the ticks GD's `speed` column steps
// from 0.9 to 1.1; the model and GD are bit-identical up to the first of them).
// 3.0 fires three ticks early, and after that the run carries 0.316 px of extra
// x per tick for the rest of the level -- the `dy is 0 and only dx grows`
// signature. 2.0 sits in the middle of the bracket; it is BRACKETED, not
// measured, so tighten it before deriving anything from it.
constexpr double kWaveHalfMini = 2.0;
// ...and the HAZARD stage-1 box is a third one again: GD tests the AABB of
// `player->getObjectRect()`, which cfg `hitboxtrace=1` reads as 10x10 full and
// **6x6 mini** -- half 5.0 and 3.0. The 2.0 above is a bracket off a speed
// portal's firing, a different code path, and it is 1 px too small here.
// Bisected live in GD on both faces, two levels, two speeds; see the table at
// `wHazHalf` in stepOne. 3.000 exactly, touching counts, not speed-dependent.
constexpr double kWaveHazHalfMini = 3.0;
// ...and the PORTAL contact box is that same 6x6 rect, so it is 3.0 too.
// Kept as its own name because the two are measured separately.
// [2026-09-01] **The speed portal uses this one as well.** It was the last
// portal path still on kWaveHalfMini, on the strength of the lv20 bracket
// above -- and that bracket does not constrain it: its site (uid4303) is a
// FULL-SIZE wave in the reference, and it was computed with an axis-aligned
// player square (`25.5 + 1.41077*half`) where GD tests the sprite-rotated box.
// The model fires that same site on GD's exact tick with the full-size 5.0.
// What the 2.0 did cost: lv21's 0.9 portal at (19587.1,239.18) fired THIRTY
// TICKS and 48 px late, running that stretch 24% fast (census family
// `m4/mini1/g0/gdg0/sp1.1/air/in1`, which the fix removes).
constexpr double kWaveContactHalfMini = 3.0;
// [2026-08-19 night 3] **The mini's portal contact half-width is smaller than the
// collision's 9.** Lining up the ramps calibration rig tick by tick between GD and
// the model (py/calib_diff.py), **only the mini->normal size portal has the model
// 1 tick early, 8/8** (norm->mini is the same tick 8/8, i.e. the normal size's 15
// is right). Bracket: against portal id99 (cx=4470, w=31 -> hw 15.5)
//   GD does not fire at |x-cx| = 23.342  -> half < 7.842
//   GD fires at        |x-cx| = 22.044  -> half >= 6.544
// Same family as the wave's mini being 3.0 (6x6 box). 7.5 is 12.5x0.6, the middle
// of the bracket. **All 8 units share the same geometry, so it cannot be narrowed
// further** -- tighten it with a rig that varies the phase (shifts the portal x).
constexpr double kMiniContactHalf = 7.5;
constexpr double kRadToDeg = 57.29577951308232;
// ...but the RAMP test uses its own box, and it is NOT centred on the player.
// Point-probed on lv20's zig-zag wave corridor (t=4042, one tick, x and y both
// injected, boundary bisected to 0.004 px) against the two ceiling ramps
// (5625,295) 310->280 and (5655,295) 280->310 and the two floor ramps under
// them. Every one of the 26 boundaries is reproduced by ONE rectangle:
//   corner sampled at x + 3.072 when the wedge thickens to the RIGHT
//                     x - 4.068 when it thickens to the LEFT
//   surface offset by 4.0 vertically, and the sample x clamped to the ramp box
// e.g. ceiling line - 7.072 in the far field, flat at corner - 4.0 across the
// 3 px left / 4 px right of the two ceilings' shared corner (that flat is the
// clamp), floor line + 8.072 on the mirrored side -- and the floor pair has NO
// flat, because there the sample points away from their shared corner.
// The old single 8.0 is that rectangle read at 45 degrees from
// one side only (lv18's downhill ramp at (19125,195): kill at line + 8.0 +-
// 0.4, and this box says 8.070 there).
// Only |m| = 1 is measured; the split into "sample x" and "offset y" is what
// separates it from a plain half extent, and it is what makes the corner flat.
// The mini values carry the established 0.600 ratio and are UNVERIFIED.
constexpr double kWaveSlopeDxR = 3.072;
constexpr double kWaveSlopeDxL = 4.068;
constexpr double kWaveSlopeDy = 4.0;
// ...and the mini ratio is BRACKETED, not carried over. The old 0.600 was
// flagged UNVERIFIED and lv20 refutes it; 1.000 is refuted the other way. Two
// GD verdicts in the same corridor pin it, writing the box as `s` x the
// measured full-size one (3.072 / 4.068 / 4.0):
//   LIVES  lv20 t=3691, mini wave (4790.70, 236.210), ramp uid3321 (4785,225)
//          sy0=210 sy1=240 (m=+1, floor half, sample to the RIGHT)
//          survival needs 236.210 >= 230.70 + s*(3.072 + 4.0)  ->  s <= 0.779
//   DIES   lv20 t=3712, mini wave (4819.28, 225.825), ramp uid3383 (4815,225)
//          sy0=240 sy1=210 (m=-1, floor half, sample to the LEFT)
//          the kill needs 225.825 <  220.72 + s*(4.068 + 4.0)  ->  s >  0.633
// so s is in (0.633, 0.779] and 0.700 sits in the middle of it. 0.600 misses
// GD's kill by 0.26 px -- and that one corridor cost the cold run ~40
// iterations of 5 px each, because every tick GD killed and the model did not
// came back as a `kill` fixup instead of as a rule.
// Two brackets, one constant: TIGHTEN IT with py/hitbox_sweep.py before
// deriving anything from the value.
constexpr double kWaveSlopeMiniScale = 0.700;
constexpr double kWaveSlopeDxRMini = kWaveSlopeDxR * kWaveSlopeMiniScale;
constexpr double kWaveSlopeDxLMini = kWaveSlopeDxL * kWaveSlopeMiniScale;
constexpr double kWaveSlopeDyMini = kWaveSlopeDy * kWaveSlopeMiniScale;
constexpr double kShipHalf = 15.0;  // ship outer box (any contact excludes)
// player-vs-hazard half: the OUTER 30x30 box. Measured on lv1: spike (525,105)
// hitbox 6x12 kills at |dx|=17.4 -> 3+15. (rectcorridor's 10 was a safe
// under-estimate for blocking, not the true kill range.)
constexpr double kHazHalf = 15.0;
constexpr double kHazMargin = 0.0;
inline bool objLocksX(int id) {
    switch (id) {
    case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
    case 32: case 33: case 34: case 55: case 56: case 57: case 58: case 59: case 105:
    case 200: case 201: case 202: case 203: case 744: case 899: case 900: case 901: case 915:
    case 1006: case 1007: case 1049: case 1268: case 1334: case 1346: case 1347: case 1616:
    case 1812: case 1814: case 1815: case 1912: case 1913: case 1914: case 1915: case 1916:
    case 1917: case 1934: case 1935: case 2015: case 2062: case 2067: case 2068: case 2900:
    case 2901: case 2925: case 2999: case 3023: case 3024: case 3029: case 3030: case 3031:
    case 3602: case 3603: case 3604: case 3605: case 3606: case 3607: case 3608: case 3609:
    case 3612: case 3613: case 3640: case 3660: case 3661: case 3662:
        return true;
    default:
        return id >= 3006 && id <= 3021;
    }
}
// Cube: how far below a surface the foot may already be and still be pushed up.
// 3.0 was a guess and it was too small: lv6 t=13606 has GD landing the cube on
// the ledge at x=17700 (top y=180) with the foot 3.29 px below it, and the model
// missed by 0.29 px and fell past.
// Swept at that ledge (inject y in 0.5 steps, read land/die):
//   penetration <= 10.50 -> lands,  >= 11.00 -> dies
// so the true limit is ~10.5-11. 6.0 is used instead: it is the ship's exactly
// measured boundary (kShipLandTol) and it covers everything observed in play,
// while a 10 px upward teleport would be a large licence to hand the planner on
// the strength of one sweep whose contact had already begun at injection time.
// Redo as a contact-onset sweep (like the ship's) before raising it further.
// TRIED AND REVERTED (2026-08-03): 10.5 (the swept value quoted above), to see
// whether lv18's frontier could then reach the ledge 269 (24,675,255) top 270
// that it tops out 8.35 px below. It changed NOTHING -- the frontier's y range,
// alive counts and death tick were bit-identical. kLandTol only forgives a foot
// that is ALREADY within N px BELOW the face; at y=262 the foot is at 253, i.e.
// 17 px below 270, which no tolerance in this range reaches. lv18's wall is not
// the landing tolerance.
// [2026-08-18] 6.0 -> 10.5 (the measured value the sweep above produced, as is).
// The landing at lv19 t=9,451 has a 6.05px penetration and fell **0.05px outside
// the threshold**. Once the robot's g was corrected to the measured -0.194 the
// player's y matched GD, and as a result the penetration went from 5.958 to 6.047
// and the landing was dropped (GD does land). Restoring the constant to the
// measured value passes both. --landtol A/Bs it.
// [2026-08-18, revised the same day] 10.5 -> 10.0. 10.5 made lv9 STUCK in the same
// day's cold run: the gravity-flipped ball at x=11,265 "landed" from **10.2125px**
// past the underside (180) of the slab (11285,187) and burrowed in, whereas GD does
// not land it and it dies on the wall with an x penetration of 10.9
// (gd_death_context names it). So the top face of the same slab was re-swept finely
// with worker-98 injections (x centred / entering sideways from non-overlapping x,
// vy -2 / 0):
//   d=10.0 (centred)       lands
//   d~10.05 (sideways)     refused -> floats 7 ticks, then side death at xpen~11
//                          (same as the ball)
//   d=10.2125 (real run)   refused (lv9's ball itself)
//   d=10.5 (centred)       dies on the injection tick (the crush side fires first)
//   d=6.047 (real run)     lands (lv19 t=9,451 -- the reason for raising to 10.5;
//                          holds at 10.0 too)
// The boundary is a single one at [10.0, 10.05]; no sideways/centred distinction is
// needed. Yesterday's 10.5 was the value of lv6's 0.5px-step sweep, whose own note
// warned of contamination with "contact had already begun at injection time"
// ([[gd-injection-resolution]]). The side where the penetration keeps deepening
// while refused is killed by cube/solid-side at the same xpen ~11 as GD (lv9's
// replay is confirmed to match on all 8,672 ticks).
inline double kLandTol = 10.0;
// --shadowrotprobe: observation only. The shadow's collision pass prints (shrotprobe lines, to SHSTEP_DEBUG_FILE) the tick and uid of every touch Rotate Gameplay (2900) its player box overlaps,
// so that the ticks can be compared with the ticks GD's frame changes. It changes nothing the pass computes.
inline bool g_shadowRotProbe = false;
// --layerxstat (print only): how many layers of the DP hold more than one group, and more than one K.x (the x a group's moving geometry is placed for), with the keys that differ -- the premise of a trigger
// timeline that is one per layer (x(t) the same for every state). Printed at exit as `layerxstat:`.
inline bool g_layerXStat = false;
struct LayerXStat {
    struct Row { double x; float dx; int frame, rev; bool trig; double spread; };
    std::vector<Row> rows;
    long long lastT = -1, layers = 0, multiGroup = 0, multiX = 0, spreadLayers = 0, byDx = 0, byFrameRev = 0, byTrig = 0, groups = 0;
    double maxSpread = 0.0;
    long long distinctSum = 0, distinctMax = 0;   // distinct K.x per layer: the number of timelines a per-x design would carry
    std::vector<long long> multiXat;
    void flush() {
        if (rows.empty()) return;
        ++layers; groups += (long long)rows.size();
        if (rows.size() > 1) ++multiGroup;
        bool mx = false, dx = false, fr = false, tr = false, sp = false;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].spread > 1e-3) sp = true;
            if (rows[i].spread > maxSpread) maxSpread = rows[i].spread;
            for (size_t j = i + 1; j < rows.size(); ++j)
                if (std::fabs(rows[i].x - rows[j].x) > 1e-3) {
                    mx = true;
                    if (rows[i].dx != rows[j].dx) dx = true;
                    if (rows[i].frame != rows[j].frame || rows[i].rev != rows[j].rev) fr = true;
                    if (rows[i].trig != rows[j].trig) tr = true;
                }
        }
        if (sp) ++spreadLayers;
        {
            std::vector<double> xs;
            for (const Row& r : rows) xs.push_back(r.x);
            std::sort(xs.begin(), xs.end());
            long long d = 0;
            for (size_t i = 0; i < xs.size(); ++i) if (i == 0 || xs[i] - xs[i - 1] > 1e-3) ++d;
            distinctSum += d;
            if (d > distinctMax) distinctMax = d;
        }
        if (mx) { ++multiX; if (dx) ++byDx; if (fr) ++byFrameRev; if (tr) ++byTrig; if (multiXat.size() < 8) multiXat.push_back(lastT); }
        rows.clear();
        if (layers % 1000 == 0) print();   // a run that is killed still leaves its last count
    }
    void add(long long t, double x, float dx, int frame, int rev, bool trig, double spread) {
        if (t != lastT) { flush(); lastT = t; }
        rows.push_back({x, dx, frame, rev, trig, spread});
    }
    void print() {
        if (!layers) return;
        std::printf("layerxstat: layers %lld groups %lld multigroup-layers %lld multiX-layers %lld (dx %lld frame/rev %lld trig %lld) spread>1e-3 layers %lld max spread %.4f distinctX/layer mean %.2f max %lld first multiX ticks:", layers, groups, multiGroup, multiX, byDx, byFrameRev, byTrig, spreadLayers, maxSpread, (double)distinctSum / (double)layers, distinctMax);
        for (long long t : multiXat) std::printf(" %lld", t);
        std::printf("\n");
        std::fflush(stdout);
    }
    ~LayerXStat() { flush(); print(); }
};
inline LayerXStat g_layerXStatData;
// The "gap between the extrapolated seat and the player's y" that ride continuation
// (stickHere) allows. **A constant in px** -- measured on the seam rig at 3 speeds
// (see the stickHere note). --stickgap A/Bs it.
// The value is the midpoint of the range [4.0307, 4.0371) that satisfies **130** of
// the 132 units (seam/seam07/seam11 x cube/ball x |m|{1,0.5} x 12 phases). The 2
// outliers are one-offs (seam unit8 wants >= 4.087, seam11 unit43 wants < 4.026),
// 1 tick at the edge of the phase -- the remaining systematic error is under 0.05px.
inline double kStickGap = 4.034;
// How far off a rotation trigger (id 2900) the player may be on the PERPENDICULAR axis and still
// fire it. Bracketed by GD's own gframe changes across two full lv22 runs -- fires at |dv| up to
// 116.7, skips at 274.4 and 657.8 -- so anything in that gap fits every measurement; 150 sits in
// it with margin either side. The reasoning, and why the old 30 was falsified, is at the gate in
// step.hpp's applyRotation. --rotperp A/Bs it.
inline double kRotPerpWin = 150.0;
inline double kStepDepth = 3.15;
// The plain (unarmed) cube CEILING STOP is OFF. [2026-08-16]
// It was fitted to two samples: lv1's false stop (xPen 6.67, full size) and
// lv22 t=2,749 (xPen 19.78, mini) -- but t=2,749 is the 2866 FLIPHEAD event,
// which `acquireFlip` handles on its own. A third sample, measured 2026-08-16
// by sweeping GD directly (gd_run inject at lv22 t=11,385, mini), has GD NOT
// pinning at xPen 15.957 -- and not at the block's dead centre either
// (xPen 24.0, the deepest possible), so no threshold on xPen can separate it
// from t=2,749. GD's unarmed didHitHead does nothing: the player keeps moving
// into the block and dies of the overlap, which is exactly what it did.
// (The A/B switch that restored the pin, --ceilpin, is gone; `held` read it and went with it.)
// --coins: make the level's coins part of the goal. Off by default, and when it
// is off State::coins stays 0, so the dedupe key, the cap families and the goal
// test are all bit-identical to a run without the feature.
inline bool g_coinRoute = false;
// --worldgates (default off, a measurement): model the level's item gates -- the Count (1611 /
// 1811), Tap (1595) and Item Compare (3620) triggers and the pickups that feed them -- whether or
// not --coins asks for the coins. GD runs them either way; without this the model only has them
// under --coins, and then only the ones whose chain reaches a coin. Only the world: the goal and
// the coin prunes stay --coins's.
inline bool g_worldGates = false;
// Are the item gates part of the world this call? (--coins or --worldgates)
inline bool gatesModelled() { return g_coinRoute || g_worldGates; }
// --coinmargin: how far INSIDE a coin's (or a pickup's) box the model has to be
// before it counts the thing as taken. **0 by default, which is GD's own rule.**
//
// It was briefly 3. The reasoning was that a plan allowed to graze the boundary
// has a verdict that flips on a fraction of a pixel -- lv21's route passed a
// coin at |dy| = 25.5 against a bound of 25 and GD credited nothing. But the
// 0.5 px was a TRAJECTORY difference, not a plan that aimed at the edge, and
// the fix for a trajectory difference is the fixup recorder (which now runs on
// a missed coin too). A margin instead shrinks the target: the wave's coin
// bound is only 25 px, so 3 takes an eighth of the window away, and every coin
// route through a wave section is the one that needs it most.
// Kept as a knob because "how tight is too tight to plan" is a real question,
// but it is not answered by a number nobody measured.
inline double kCoinMargin = 0.0;
// --coindbg: per touch bit, (the highest count already printed for that Count
// trigger) + 1, so the `countgate:` line appears once per distinct n. Global
// and reset, not a function-local static: a second solve in the same process
// would otherwise print nothing for any count the first one had reached.
inline std::atomic<int> g_countSaid[kTouchBits] = {};
// --coinmask: the collected set an anchored (--start) search begins with.
inline int g_coinMaskSeed = 0;
// --coinskip: coins that are not a goal this run, seeded as collected on every
// call so neither the goal test nor the miss prunes ask for them.
inline int g_coinSkip = 0;
// --itembase "<item>:<n>,...": what GD's item counters already held at that
// anchor. A Count trigger compares the COUNT, and the pickups behind an anchor
// are not in the window State::items numbers, so the history arrives as a
// number and the bits only ever add to it.
inline std::vector<std::pair<int, int>> g_itemBase;
// Ship zones have an invisible ceiling. It was modelled as "ship portal cy +
// 120" from a single lv1 measurement (portal cy 255, ceiling 375.000) -- but
// that was a coincidence. lv4's ship portal sits at cy = 233, so the rule
// predicts 353, and GD's measured ceiling in that zone is again 375.00 -- the
// model clamped the ship 22 px low and it could never reach the route above.
// Two zones with different portal heights, same ceiling: it is ABSOLUTE.
// ...and 375 is not universal either. lv10 t=17836 has GD flying the ship at
// y=427 and still climbing, so a hard clamp at 375 blocked that route outright.
// maxY does not explain it (lv1 maxY=540 -> 375, lv2 maxY=390 -> 375,
// lv10 maxY=630 -> above 427), so there is no level-derived rule in hand.
// TRIED AND REVERTED: disabling the clamp entirely (relying on g_yBound alone).
// It let lv10 through to t=18,743 but **lv2 went from cleared to stuck** at
// t=10,569 and lv1 needed 5 iterations instead of 2 (2.7 min vs 0.3). The clamp
// is load-bearing where it is right, so it stays at the value that is right for
// lv1/lv2/lv4.
// What it actually is: a SEARCH BOUND, not physics. GD exceeds it (lv10 y=427)
// and no level-derived rule reproduces it -- neither maxY nor the ship zone's
// own highest surface (lv1 zone 450 -> 375, lv2 zone 390 -> 375). But lv1/lv2
// need it, because without it the DP spends its frontier on high routes that
// GD then refuses.
//
// [2026-08-31] ALL OF THE ABOVE IS HISTORY, AND THIS VALUE IS INERT. It is read
// in exactly two places (step.hpp's two band sites and cli.hpp's --start seed)
// and every one of them is gated on `g_shipCeilSet`, which only the --shipceil
// flag ever set -- and that flag is now gone, because nothing in the pipeline
// passed it. What the model actually uses is bandFor(the firing portal's cy, H),
// and that is not a fallback but the better answer: GD reports [90,390] in lv1's
// first ship zone and bandFor(239, 300) gives exactly that, while the branch
// portal at cy=405 gives [240,540] -- a route this hard 375 would have forbidden
// outright. The reverted experiment recorded above removed the clamp WITHOUT
// putting the derived band in its place, which is why it read as a regression.
// (g_shipCeil itself, 375, and its readers were removed 2026-11-20: gated on a flag nothing could set.)
// ...and the flying modes have a hard FLOOR too, which is not the world's.
// Measured on lv12's UFO section through the gd MCP: injecting the UFO at y=60
// at t=5400 / 5500 / 5640 (x = 7010 .. 7321) has GD put it back at y=195.000
// with onGround=1 every time, and injecting at y=700 clamps it down to 465.
// So the band is [180, 480] for the player's surfaces -- exactly 300 tall --
// while there is NOT ONE object under it (checked: zero type-0 in y[140,220]
// across x[7000,7160]). A cube injected the same way at t=5300 just dies, so
// the band belongs to the flying modes, not to the level's geometry.
// The model had the ground pinned at 90 and simply fell through, which is why
// the DP planned a route GD never lets it take.
// Same status as g_shipCeil: measured per level, not derived. 0 = no floor.
// The band is per SECTION, not per level: forcing lv12's UFO band (180/465)
// onto its ship sections killed the search inside the first one immediately.
// So these two apply to the UFO portal (type 19) only, and a cube or ball
// portal clears them again.
// ...and for the UFO the band is DERIVED from the portal, not a knob at all.
// lv12 has two UFO sections at wildly different heights and one number cannot
// serve both, so the portal's own cy was checked against the measurements:
//   portal (6945, 345)   -> floor 180 measured, 345 - 165 = 180   ceiling 480
//                           measured, 345 + 135 = 480
//   portal (16665, 1095) -> floor 930 measured, 1095 - 165 = 930
// Two sections, three bounds, exact on all of them. The same arithmetic also
// reproduces the ship bound that was tuned by hand long ago: lv1's ship portal
// sits at cy = 255, and 255 + 135 - 15 = 375 -- exactly g_shipCeil's default.
// THE DERIVATION IS THE RULE; the two below were its overrides and are now
// inert, for the same reason as g_shipCeil (their flags are gone, and nothing
// had been passing them).
constexpr double kFlyBandBelow = 165.0;
constexpr double kFlyBandAbove = 135.0;
// (the two overrides, g_flyFloor and g_ufoCeil, were removed 2026-11-20 with their readers: always 0.)

}  // namespace dp
