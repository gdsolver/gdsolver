// The self-running contact shadow: one GJBaseGameLayer::update sub-step of player 1, on the player's byte image, from the button edges alone.
// Unlike ContactShadow (shadow.hpp), which is handed what the update phase produced, this one produces it: the order is the game's (notes, collision-state table 12.13):
//   clock -> updateTimeMod (a speed change from the last step) -> buttons (buttons.hpp) -> resetTouchedRings -> the collision logs of the last pass -> (the world: the caller moves
//   the objects) -> PlayerObject::update (upd.hpp, with updateJump and the spider's teleport) -> checkCollisions (collide.hpp) -> the time accumulator -> updateRotation -> the
//   position the next rotation reads.
// Observation only; nothing includes it but tools. Not in it: player 2 / dual, the speed portal's updateTimeMod call (the caller says setSpeed), the half step a click between steps makes
// (dt is the caller's), trigger effects. What is not ported is counted in Ctx::unsupportedBy, summed in SelfOut::unsupported.
#pragma once
#include "dp/buttons.hpp"
#include "dp/collide.hpp"
#include "dp/collide_tmpl.gen.hpp"
#include "dp/ever_payload.hpp"
#include "dp/upd.hpp"

#include <mutex>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>

namespace dp {
namespace gdcol {

// Objects seedMemory left fresh because the rect the body had at the start of its last update (PrevX / PrevY) did not overlap them: the seeds State::prevWX / prevWY changed the answer for (print only, shadowstep line).
inline std::atomic<long long> g_seedPrevFresh{0};

// SSPROF=1 (any value): thread-ns per sub-part of a step (head, tail) and of shadow_state.hpp's pack / unpack, printed by leveldp's shadowstep line
// (shadow_step.hpp shadowStepPrint) with the neighbourhood sizes. Timing only: nothing a step computes reads it. Off, each mark is one bool test.
// The sums are kept per thread (SsTl, one block a thread, never freed) and added up when they are printed (ssSum): eight threads adding to one set of atomics made every mark wait for
// the others' cache line, and the wait fell into the bucket after it -- the buckets then said more about the profiler than about the step.
struct SsProf { bool on = std::getenv("SSPROF") != nullptr; };
inline SsProf& ssProf() { static SsProf p; return p; }
struct SsTl { long long ns[24] = {}; long long cnt[16] = {}; };
struct SsReg { std::mutex m; std::vector<SsTl*> all; };
inline SsReg& ssReg() { static SsReg r; return r; }
inline SsTl& ssTl() {
    static thread_local SsTl* t = [] { SsTl* p = new SsTl(); SsReg& r = ssReg(); std::lock_guard<std::mutex> lk(r.m); r.all.push_back(p); return p; }();
    return *t;
}
// (read while no step runs: the print at the end of a call)
inline SsTl ssSum() { SsTl s; SsReg& r = ssReg(); std::lock_guard<std::mutex> lk(r.m); for (const SsTl* p : r.all) { for (int k = 0; k < 24; ++k) s.ns[k] += p->ns[k]; for (int k = 0; k < 16; ++k) s.cnt[k] += p->cnt[k]; } return s; }
struct SsMark {
    bool on; std::chrono::steady_clock::time_point last;
    SsMark() : on(ssProf().on) { if (on) last = std::chrono::steady_clock::now(); }
    void operator()(int k) { if (!on) return; const auto n = std::chrono::steady_clock::now(); ssTl().ns[k] += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(n - last).count(); last = n; }
};

// Every member of a Ctx back to what Ctx(P) gives it (objs, which the constructor leaves unset and every caller sets, to null), keeping the
// containers' storage: MSVC's std::set / std::map allocate their head node when constructed, six of them a Ctx, and a step built three Ctx.
// (Here and not in collide.hpp, whose line numbers self_test prints for its kills.) A member added to Ctx must be added here.
inline void ctxReset(Ctx& c) {
    c.L = Layer{};
    c.objs = nullptr;
    c.lkWorld = nullptr; c.lkIndex = nullptr; c.lkObjs = nullptr; c.lkStamp = nullptr; c.lkPos = nullptr; c.lkGen = 0;
    c.died = false;
    c.touched.clear();
    c.why = 0; c.curUid = -1; c.whyUid = -1;
    c.unsupported = 0;
    c.unsupportedBy.clear();
    c.tpLog.clear();
    c.forceSeen.clear();
    c.topDict.clear(); c.botDict.clear();
    c.gateIn = nullptr; c.gateNow.clear();
    c.everIn = nullptr; c.everRule = nullptr; c.everRuleCtx = nullptr; c.everBase = nullptr; c.everNow.clear();
    c.bornFired = false; c.bornY = 0; c.bornVy = 0;
    c.ringsTouched.clear();
    c.hsKnown = false;
    c.ringJump = nullptr; c.slopeRotation = nullptr; c.stopDash = nullptr; c.dashRot = nullptr; c.restartDashSpin = nullptr; c.spiderJump = nullptr;
    c.p22 = true; c.primeWords = false;
    c.usedRings = nullptr;
    c.fired.clear(); c.everGuess = false; c.gameEvents = nullptr;
    c.rotApply = false; c.rotSpent = nullptr; c.rotFired.clear();
    c.actLog = nullptr;
    c.dualH = nullptr;
}

struct SelfIn {
    uint8_t press1 = 0, release1 = 0;   // the edges this step's processCommands hands to player 1 (button 1 = jump)
    float dt = 0.25f;                   // the step in 1/60 s: (D / n) * 60, 0.25 for four steps to a frame
    long long tick = 0;                 // the pass's tick (the ctraw / dump numbering): the frame's last step is the one with tick % frameLen == frameEndMod
    int ctrl = 0;                       // an Options trigger in this step: 1 = player 1's controls disabled, 2 = enabled (the timeline trig_replay --hidden prints); applied before the step's button edges
    int ctrl2 = 0;                      // --shadowdualctrl: the same event for PLAYER 2 (the dual shadow hands it to player 2's head as its ctrl)
    int ctrlLate = 0;                   // 1 = the controls event is applied at the end of the collision pass, not before the buttons (a touch-triggered Options trigger runs inside the pass: the game's byte changes at the pass's exit)
    int hasGrav = 0; float gravMod = 1.0f;   // a Gravity trigger set player 1's gravity factor (+0xb8c) in this step (trig_replay --hidden: "gravity V (player 0, trigger U) in step T")
    int tpSpawn = 0;                   // that trigger is spawn-fired (not touched / x-crossed): the previous-position floats +0xa00 / +0xa04 keep the old position
    int ctrlAfter = 0;                 // 1 / 2 = player 1's controls disabled / enabled after the pass has been recorded (an event that runs between the end of one step and the next head: the pass's exit image does not have it, the next head does)
    int tpEndUid = -1;                 // a spawn-fired Teleport trigger: it runs between two steps, after the previous-position store, so the next head already has it
    int tpLateUid = -1;                // the same, for an x-crossing trigger: applied at the end of this step's pass (after atExit, before the rotation and the a00 / a04 store)
    int tpUid = -1;                    // a Teleport trigger (a Modifier object that calls GJBaseGameLayer::teleportPlayer) fired in this step: its uid; the shadow moves the player as the portal path does (trig_replay --hidden: "trigger N fired in step T", applied at T + 1)
    int head = -1, end = -1;            // 1 / 0 = the game's own frame boundaries for this step (a frame is not always four steps: GJBaseGameLayer::update takes as many as the frame time asks); -1 = by the modulus
    bool rotApply = false;              // --shadowrotapply: a touch Rotate Gameplay the pass overlaps turns the body (collide.hpp playerRotateGameplay); rotSpent = the uids that have fired already (null: turns nothing)
    const std::vector<int>* rotSpent = nullptr;
    bool recordGameEvents = false;
};

struct SelfOut {
    bool ran = false;
    bool died = false;
    int ret = 0;
    int why = 0, whyUid = -1;
    int unsupported = 0;
    std::map<int, int> unsupportedBy;
    std::vector<TpEvt> tps;                   // the teleports the pass (and the bridged triggers) made, in order (collide.hpp Ctx::tpLog)
    bool everGuess = false;                       // --shadowrev: the pass went left through an object the gate holds as seen by the seeding guess alone (Ctx::everGuess)
std::vector<std::pair<int, int>> fired;   // (type, uid) of the non-solid objects the pass overlapped, 1000 added when the once-only gate let one act
    std::vector<int> rotFired;                // the touch Rotate Gameplay triggers the pass turned the body with (SelfIn::rotApply)
    std::vector<gdev::EvTok> gameEvents;      // events emitted by the head or the tail, in call order
    // back to what SelfOut{} holds, keeping the storage (a caller that steps many times reuses one; a member added above must be added here)
    void reset() { ran = false; died = false; ret = 0; why = 0; whyUid = -1; unsupported = 0; unsupportedBy.clear(); tps.clear(); fired.clear(); rotFired.clear(); gameEvents.clear(); everGuess = false; }
};

class SelfShadow {
public:
    Mem img;
    Layer L;                           // what the layer holds that the steps read (set once by the caller)
    bool platformerLevel = false;      // levelsettings platformerMode=1: the platformer's own code (updateMove, animatePlatformerJump, the jump ramp, the carried speed +0x748, the left / right buttons) is not modelled; the first step counts unsupported[4010] once, so a window of such a level says so
    bool platformerNoted = false;
    bool dynHeight = false;            // the level keeps dynamic level height (one row of section cells otherwise)
    bool p22 = true;
    std::vector<GObj>* world = nullptr;   // every object of the level; the caller keeps the positions of the moving ones current
    RingTouches rt;
    FlatSet<int> usedRings;
    std::vector<int> fireOrder;   // --shadowfireorder: the rings fired, newest first (kFireOrder); carried in the shadow block (ShadowLite::fired)
    bool holding[4] = {false, false, false, false};
    FlatSet<int> gate, ever;
    // ever's rule part when it is not held in `ever` (shadow_state.hpp slUnpack with SlWorld::lazyEver): the pass asks says(w, lo, hi, uid) after `ever` (Ctx::everRule).
    // A player's own (selfshadow_dual.hpp swaps it with the rest of a player); off unless an unpack turned it on.
    struct EverRule { bool on = false; bool rev = false; float lo = 0.0f, hi = 0.0f; const void* w = nullptr; bool (*says)(const void* w, float lo, float hi, bool rev, int uid) = nullptr; };   // (rev: the body seeded travelling left, hi = the right edge it had, an object whose LEFT edge is right of hi was seen)
    EverRule everRule;
    static bool everRuleSays(const void* r, int uid) { const EverRule& e = *static_cast<const EverRule*>(r); return e.says(e.w, e.lo, e.hi, e.rev, uid); }
    double layerTime = 0.0;           // GJBaseGameLayer +0x3c8: the clock the player's +0xaa8 is set from at the head of a step
    double stepSeconds = (1.0 / 60.0) / 4.0;   // dVar9 of the layer's update: the frame delta over the sub-steps (measured: the clock advances by 1/240 to 12 digits)
    int frameLen = 4;                  // sub-steps to a frame
    int frameEndMod = 0;               // measured on 4 levels: the frame's last sub-step has tick % 4 == 0 (the mod numbers them so)
    std::function<void(long long)> worldPhase;   // called at the point of the step where the layer moves the objects (after the buttons, before the update): a live run puts the movers where they are now
    bool speedDirty = false;
    float pendingSpeed = 1.0f;
    // The three points of the last step: after the buttons, after the update (the pass's entry), after the pass (its exit). The layer writes the State's body from the exit image, so
    // atExit is always the whole image. Of the other two the step's own readers take a handful of values (StepPoint: the position, the left edge of the rect, the gravity-flip stamp),
    // so those are what is kept -- a whole image is 3.2 KB, and two of them a stepped tick were a quarter of a microsecond of its three. The whole images atButtons / atEntry are
    // filled only under keepImages: the tools' tests (self_test, shadow_state_test) and the two readers that print or dump them (SHSTEP_DEBUG, --shadowquietdump).
    struct StepPoint { bool ok = false; float posX = 0.f, posY = 0.f, left = 1e30f; uint64_t flipStamp = 0; };
    static StepPoint pointOf(const Mem& m) {
        StepPoint p;
        if (m.b.size() < 0xc80) return p;
        p.ok = true; p.posX = m.f32(po::PosX); p.posY = m.f32(po::PosY); p.left = playerLeftOf(m); p.flipStamp = m.get<uint64_t>(0x828);
        return p;
    }
    static float playerWidthOf(const Mem& m) {
        return m.u8(0x368) ? plainPlayerRectSize(m, m.f32(0x394), m.f32(0x398)).first : m.f32(0x360);
    }
    // the left edge of the player's rect in an image (1e30 for no image): shadow_state.hpp's slPlayerLeft is this
    static float playerLeftOf(const Mem& m) {
        if (m.b.size() < 0xc80) return 1e30f;
        return m.f32(po::PosX) - playerWidthOf(m) * 0.5f;
    }
    StepPoint ptButtons, ptEntry;
    bool keepImages = false;
    Mem atButtons, atEntry, atExit;
    // The scheduler updates the dash's child action once per frame; its first update counts no time.
    // w_128992032_710 measures 16.0000305 after one counted frame at normal gravity.
    float dashRot = 0.0f, dashElapsed = 0.0f, dashSpinRate = 0.0f;
    bool dashOn = false, dashFirstDone = false;
    static float dashRate(const Mem& P) {
        if (P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeRobot) || P.u8(po::ModeSpider)) return 0.0f;
        // GD Win 2.2091: updateDashArt (RVAs 0x4f4713..0x4f47cd) latches gravity and orientation into a +/-288 degree, 0.3s action.
        const bool neg = (P.u8(po::Upside) != 0) != (P.u8(po::Sideways) != 0);
        return (neg ? -960.0f : 960.0f) * P.f32(0xb8c);
    }
    void restartDashAction(const Mem& P) {
        dashOn = true; dashFirstDone = false;
        dashRot = dashElapsed = 0.0f; dashSpinRate = dashRate(P);
    }
    void bindDashRestart(Ctx& c) {
        g_self() = this;
        c.restartDashSpin = [](Ctx& cc) { g_self()->restartDashAction(cc.P); };
    }
    int frameSteps = 0, prevFrameSteps = 4;
    std::map<int, std::vector<int>> tpExit;   // a Teleport trigger's uid -> the uids of its target group, in group order
    long long curTickNow = 0;
    std::set<int> act2;   // --shadowringp2 (constants.hpp): the uids player 2 has fired (the world's activated is player 1's then)
    bool p2Phase = false; // --shadowringp2: player 2 is swapped in (selfshadow_dual.hpp swapPlayer toggles it)
    DualHooks* dualH = nullptr;                // --shadowdual: the other player, for the buttons and the pass (selfshadow_dual.hpp); null = player 1 alone
    uint32_t rng = 0; bool rngOn = false;     // the C runtime generator's state before the next pick (only its low 31 bits decide a pick between two objects)

    SelfShadow() : img(0xc80) {}

    // GJBaseGameLayer::teleportPlayer for a Teleport trigger: at the head of a step (a spawn-fired trigger runs between the steps) or, for an x-crossing one, at the end of the previous step's pass (checkSpawnObjects runs after the pass, before the rotation and the previous-position store)
    void doTeleport(int uid, bool spawnFired, SelfOut& out) {
        Mem& P = img;
        const long long curTick = curTickNow;

            auto it = uidIndex.find(uid);
            if (it != uidIndex.end()) {
                Ctx ct(P);
                ct.L = L; ct.objs = world; ct.p22 = p22; ct.hsKnown = true; ct.usedRings = &usedRings; ct.fireOrder = &fireOrder;
                bindDashRestart(ct);
                GObj trg = (*world)[it->second];
                // GJBaseGameLayer::teleportPlayer (0x315f20) picks the exit at random among the target group's objects when there are several: idx = (int)(count * r / 32767) from the C runtime's
                // generator, state = state * 0x343fd + 0x269ec3 (one step per pick; a single object does not advance it). Without a seed (rngOn) the dump's exit stays.
                const auto ex = tpExit.find(uid);
                if (ex != tpExit.end() && ex->second.size() == 1) {   // one object in the group: the exit is where that object is NOW (an Advanced Follow can carry it with the player: wtrace.csv, grouptraceall=1)
                    const auto mi = uidIndex.find(ex->second[0]);
                    if (mi != uidIndex.end()) { trg.tpEx = (*world)[mi->second].cx; trg.tpEy = (*world)[mi->second].cy; }
                } else if (rngOn && ex != tpExit.end() && ex->second.size() > 1) {
                    rng = rng * 0x343fdu + 0x269ec3u;
                    const float f = (float)((rng >> 16) & 0x7fff) / 32767.0f;
                    size_t k = f == 1.0f ? 0 : (size_t)((float)ex->second.size() * f);
                    if (k >= ex->second.size()) k = 0;
                    const auto mi = uidIndex.find(ex->second[k]);
                    static const bool rngDebug = std::getenv("ST_RNGDEBUG") != nullptr;   // read once (shadow_step.hpp shDbgUid)
                    if (rngDebug) std::fprintf(stderr, "pick tick %lld trigger %d rng %08x -> k %zu of %zu uid %d\n", curTick, uid, (unsigned)rng, k, ex->second.size(), ex->second[k]);
                    if (mi != uidIndex.end()) { trg.tpEx = (*world)[mi->second].cx; trg.tpEy = (*world)[mi->second].cy; }
                }
                const float a00 = P.f32(0xa00), a04 = P.f32(0xa04);
                teleportPortal(ct, &trg);
                if (spawnFired) { P.f32(0xa00, a00); P.f32(0xa04, a04); }
                addUnsupported(out, ct);
            }
    }

    void setSpeed(float s) { pendingSpeed = s; speedDirty = true; }
    // A segment that starts mid-level does not know which portals the player has already passed (the once-per-attempt memory is not in the image): the non-solid objects entirely behind the
    // player count as seen, those it overlaps now as the last tick's overlaps.
    // --shadowframe: seedMemory leaves out the objects that are switched off. A mover the caller has not placed in this tick is `inactive` and stands where whichever tick this thread placed it in last put it; asked
    // about, it said "seen" for a player that overlapped that stale place, and which thread seeded a state changed what the state's block held (the search depended on the schedule).
    bool seedSkipInactive = false;
    // `teleportedAfterPass` (--seedspiderpass): the body's pose is one it reached after the last collision pass (a spider teleport in the buttons phase): nothing it overlaps now was touched by that pass, so only the objects entirely behind it are seen.
    // --shadowturnedever: the world x of the rotation trigger that turned the frame the body is in (0 = not a turned frame, or not known); set by the caller before seedMemory(). In a turned frame the body travels along y and
    // its x is the jump axis: "left of the body" says nothing about what it has passed, but what lies left of where the frame turned was passed before the turn.
    float seedTurnX = 0.0f;
    void seedMemory(bool teleportedAfterPass = false) {
        ever.clear(); gate.clear();
        everRule.on = false;   // (this fills ever whole)
        if (!world) return;
        float rx, ry, rw, rh;
        playerRect(img, rx, ry, rw, rh);
        // E9 (11-18): a body travelling left (Leftward, not a turned frame) has passed what lies to its RIGHT; the rule below was written for forward travel only
        const bool revSeed = img.u8(po::Leftward) && !img.u8(po::Sideways);
        for (const GObj& o : *world) {
            if (o.type == 0 || o.type == 2 || o.type == 7 || o.type == 0x15 || o.type == 0x19 || o.type == 0x27) continue;
            if (seedSkipInactive && o.inactive) continue;
            // With the anchor payload (g_everPayloadOn: ever_payload.hpp) GD's own once-per-attempt flags say what the body has touched; the position rule ("the object lies behind the body, so it was passed") is a
            // guess that the payload replaces, and it is wrong for a body that comes back to the object: a custom level t=20586, a Teleport sends the ship from x 30803 back to 30555, the two pads (47863 / 47864) the rule had
            // seeded as passed are first touched there, and GD fires them (padseed 20586) while the pass called them seen and gave no bump (vy -1.07 for GD's +10.08, 20200:20588 diverged). The objects that lie behind
            // the body are skipped here, not the ones overlapped now (below: the contact map and the flag of an object the body is inside are set in the game too).
            if (g_everPayloadOn) { if (revSeed ? (o.rx > rx + rw) : (o.rx + o.rw < rx)) continue; }
            if (revSeed) { if (o.rx > rx + rw) { ever.insert(o.uid); continue; } }
            else
            if (o.rx + o.rw < rx) { if (seedTurnX <= 0.0f || o.rx + o.rw < seedTurnX) ever.insert(o.uid); continue; }   // (--shadowturnedever: in a turned frame only what lies before the turn counts as passed)
            const bool now = o.rx <= rx + rw && rx <= o.rx + o.rw && o.ry <= ry + rh && ry <= o.ry + o.rh;
            if (!now) continue;
            if (teleportedAfterPass) continue;
            // a rotated portal / pad / orb is touched only where the oriented boxes overlap as well (collisionCheckObjects asks them after the rect): the bounding box alone says "seen" a step too early
            const bool oriented = o.radius <= 0.0f && objRotatedBox(o);
            if (oriented && !rotAskOverlap(2, img, o)) continue;
            // overlapped now: seen already only if the rect the player had at the start of this update overlapped it as well (+0x4d0 / +0x4d4); first contact fires in the next pass
            const float qx = img.f32(po::PrevX) - rw * 0.5f, qy = img.f32(po::PrevY) - rh * 0.5f;
            if (!(o.rx <= qx + rw && qx <= o.rx + o.rw && o.ry <= qy + rh && qy <= o.ry + o.rh)) { ++g_seedPrevFresh; continue; }
            if (oriented && !obbOverlap(makeObb(img.f32(po::PrevX), img.f32(po::PrevY), rw, rh, -img.f32(0x38) * 0.017453292f), objObb(o))) continue;
            ever.insert(o.uid); gate.insert(o.uid);
        }
    }
    void index() {
        uidIndex.clear();
        if (world) for (size_t i = 0; i < world->size(); ++i) uidIndex[(*world)[i].uid] = i;
        uidOnce.clear();   // Ctx::byUid's index: the uids that name one object of the world only (with two objects of a uid the scan's answer is the first in the list's order; those are left to the scan)
        if (world) { uidOnce.reserve(world->size()); for (size_t i = 0; i < world->size(); ++i) { auto r = uidOnce.emplace((*world)[i].uid, (long long)i); if (!r.second) r.first->second = -1; } }
        nb.built = false;
        sgrid = spider::Grid();
    }
    // The uids of the objects the caller moves (worldPhase or between steps). With them the neighbourhood files every other object in its section cell once and re-files only the
    // movers; without them (or when two objects share a uid) it scans the whole level at every call as before. Nothing else may change an object's centre. `notifies`: the caller
    // calls moverMoved(world index) whenever it changes a mover's cx / cy (else every mover's cell is read again at each call).
    void setMovers(const std::vector<int>& uids, bool notifies) {
        moverUids = uids;
        moversKnown = true;
        moversNotify = notifies;
        nb.built = false;
        sgrid = spider::Grid();
    }

    SelfOut step(const SelfIn& in) {
        SelfOut out;
        if (!world) return out;
        stepHead(in, out);
        stepTail(in, out);
        return out;
    }

    // The two halves of step(), split at atButtons (S2 stage 2: a model tick is the tail of one step and the head of the next, since the model's row t is the
    // player after the buttons of step t). stepHead: the frame head, the clock, updateTimeMod, the buttons, resetTouchedRings, the collision logs -> atButtons.
    // stepTail: the world, the update, the pass and what follows it. step() is exactly stepHead then stepTail.
    // --spidergrid: the x-column index the spider's two world queries read (spider.hpp), built once the movers are named and the uids are unique (nb.ok); handed to spider.hpp for the length of a step
    spider::Grid sgrid;
    const spider::Grid* spiderGridFor() {
        if (!g_spiderGrid || !world || !moversKnown) return nullptr;
        if (!nb.built || nb.w != world || nb.n != world->size() || nb.dyn != dynHeight) buildIndex();
        if (!nb.ok) return nullptr;
        if (!sgrid.ok || sgrid.world != world || sgrid.n != world->size()) {
            std::vector<char> mv(world->size(), 0);
            for (int uid : moverUids) { auto it = uidIndex.find(uid); if (it != uidIndex.end()) mv[it->second] = 1; }
            spider::gridBuild(sgrid, *world, mv);
        }
        sgrid.liveGen = (nbReuse && moversNotify) ? &moverGen : nullptr;   // (the promise of setNbReuse: the caller says worldChanged / moverMoved for every change of a mover; without it the spider reads the movers live)
        return &sgrid;
    }
    struct SpiderGridScope {
        explicit SpiderGridScope(const spider::Grid* g) { spider::gridPtr() = g; }
        ~SpiderGridScope() { spider::gridPtr() = nullptr; }
    };

    void stepHead(const SelfIn& in, SelfOut& out) {
        if (!world) return;
        SpiderGridScope sgs(spiderGridFor());
        SsMark sm;
        Mem& P = img;
        if (in.head >= 0 ? in.head != 0 : in.tick % frameLen == (frameEndMod + 1) % frameLen) {   // the head of a frame (GJBaseGameLayer::update, before its sub-steps): the byte at +0x98c goes down and the player is set to the position the last frame ended with
            P.u8(0x98c, 0);
            if (!P.u8(0xa32)) { upd::setPlayerPosition(P, P.f32(0xa98), P.f32(0xa9c)); }
            if (frameSteps > 0) prevFrameSteps = frameSteps;
            frameSteps = 0;
            if (dashOn) {
                if (dashFirstDone) {
                    const float sched = (float)rawTimeWarpAt((double)P.f32(po::PosX));   // --shadowdashsched: the action manager runs on the scheduler's scaled dt
                    dashElapsed += 0.0166667f * ((float)prevFrameSteps / 4.0f) * sched; dashRot = dashSpinRate * dashElapsed;
                }
                else dashFirstDone = true;
            }
        }
        ++frameSteps;
        layerTime += stepSeconds;
        P.f64(po::Time, layerTime);
        if (speedDirty) { upd::updateTimeMod(P, pendingSpeed); speedDirty = false; }
        sm(0);

        // ---- buttons ----
        std::vector<GObj>& local = nbLocal;
        neighbourhood(P.f32(po::PosX), P.f32(po::PosY), true, local, nbLocalIdx);
        if (sm.on) { ssTl().cnt[0] += 1; ssTl().cnt[1] += (long long)local.size(); }
        sm(1);
        Ctx& cb = stepCtx(0);
        if (in.recordGameEvents) cb.gameEvents = &out.gameEvents;
        cb.L = L; cb.objs = &local; cb.p22 = p22; cb.hsKnown = true; cb.usedRings = &usedRings; cb.fireOrder = &fireOrder; cb.dashRot = &dashRot; cb.dualH = dualH;
        bindDashRestart(cb);
        actLog.clear(); cb.actLog = &actLog;
        ButtonsBody b; b.c = &cb; b.touch = &rt; b.usedRings = &usedRings;
        for (int k = 1; k <= 3; ++k) b.holding[k] = holding[k];
        if (!headHooksSet) {   // (made once: they read only this shadow's members)
            headHooks.updateJump = [this](Ctx& cc, float dt) { auto* s = cc.objs; cc.objs = world; upd::updateJump(cc, dt); cc.objs = s; };
            headHooks.spiderTestJump = [this](Ctx& cc, const GObj*) { auto* s = cc.objs; cc.objs = world; const bool r = spider::spiderTestJump(cc); cc.objs = s; return r; };
            headHooks.startDashing = [this](Ctx& cc, const GObj* ring) { upd::startDashing(cc, ring); restartDashAction(cc.P); };
            headHooks.stopDashing = [this](Ctx& cc) { upd::stopDashing(cc); dashOn = false; };
            // the teleport orb (ringJump's 0x2e arm -> GJBaseGameLayer::teleportPlayer, the orb's own exit tpEx / tpEy) is the portal's teleport on a copy of the orb
            headHooks.teleportPlayer = [this](Ctx& cc, const GObj* ring) { GObj o = *ring; teleportPortal(cc, &o); };
            headHooksSet = true;
        }
        const ButtonsHooks& hk = headHooks;
        curTickNow = in.tick;
        g_tpExitTick = (long long)in.tick;
        sm(2);
        if (platformerLevel && !platformerNoted) { platformerNoted = true; out.unsupportedBy[4010] += 1; ++out.unsupported; }   // platformer: not modelled
        if (in.hasGrav) P.f32(0xb8c, in.gravMod);
        if (in.tpUid >= 0) doTeleport(in.tpUid, in.tpSpawn != 0, out);
        if (!in.ctrlLate) {
            if (in.ctrl == 1) disablePlayerControls(b, hk);
            else if (in.ctrl == 2) enablePlayerControls(b, hk);
        }
        int edgeBucket = 3;
        if (sm.on) {
            sm(2);
            ssTl().cnt[4] += in.press1 ? 1 : 0; ssTl().cnt[5] += in.release1 ? 1 : 0; ssTl().cnt[6] += (in.press1 && !rt.arr.empty()) ? 1 : 0;
            const bool spiderM = P.u8(po::ModeSpider) != 0, ballM = P.u8(po::ModeBall) != 0;
            if (in.press1) { edgeBucket = spiderM ? 22 : ballM ? 23 : 20; ssTl().cnt[spiderM ? 7 : ballM ? 8 : 9] += 1; }
            else if (in.release1) edgeBucket = 21;
        }
        applyEdges(b, in.press1, in.release1, hk);
        sm(edgeBucket);
        for (int k = 1; k <= 3; ++k) holding[k] = b.holding[k];
        addUnsupported(out, cb);
        writeBack(local, nbLocalIdx);
        rt.reset();
        // the collision logs the last pass left (GJBaseGameLayer::update, right after resetTouchedRings)
        P.put<int32_t>(0x5e0, P.i32(0x5d0));
        P.put<int32_t>(0x5e4, P.i32(0x5d4));
        P.put<int64_t>(0x5d0, -1);
        P.put<int64_t>(0x5d8, -1);
        ptButtons = pointOf(P);
        if (keepImages) atButtons = P;
        sm(4);
    }

    void stepTail(const SelfIn& in, SelfOut& out) {
        if (!world) return;
        SpiderGridScope sgs(spiderGridFor());
        SsMark sm;
        Mem& P = img;
        if (worldPhase) worldPhase(in.tick);

        // ---- update ----
        Ctx& cu = stepCtx(1);
        if (in.recordGameEvents) cu.gameEvents = &out.gameEvents;
        cu.L = L; cu.objs = world; cu.p22 = p22; cu.hsKnown = true; cu.usedRings = &usedRings; cu.fireOrder = &fireOrder;
        sm(5);
        upd::playerUpdate(cu, in.dt, [](Ctx& cc, float d) { upd::updateJump(cc, d); });
        sm(6);
        addUnsupported(out, cu);
        ptEntry = pointOf(P);
        if (keepImages) atEntry = P;
        sm(7);
        if (P.u8(0x9c8)) { out.ran = true; out.died = true; atExit = P; return; }   // dead before the pass: the layer does not step further in this port

        // ---- the collision pass ----
        std::vector<GObj>& objs = nbPass;
        neighbourhood(P.f32(po::PosX), P.f32(po::PosY), false, objs, nbPassIdx);
        if (sm.on) { ssTl().cnt[2] += 1; ssTl().cnt[3] += (long long)objs.size(); }
        const size_t nCand = objs.size();
        for (size_t off : {(size_t)po::SlopeObj, (size_t)po::SlopeObjB, (size_t)po::Collided, (size_t)po::GroundObj, (size_t)po::LeftObj, (size_t)po::RightObj, (size_t)po::Cf, (size_t)po::Cd0, (size_t)po::SnappedTo}) {
            const int uid = (int)P.get<int64_t>(off) - 1;
            if (uid < 0) continue;
            if (nb.ok) {   // uids are unique: the copy of this uid is in objs iff its world index was stamped by this call
                auto it = uidIndex.find(uid);
                if (it == uidIndex.end() || nb.stamp[1][it->second] == nb.gen[1]) continue;
                objs.push_back((*world)[it->second]); nbPassIdx.push_back((uint32_t)it->second); nb.stamp[1][it->second] = nb.gen[1]; nb.pos[1][it->second] = (uint32_t)(objs.size() - 1);   // (its place too, for Ctx::byUid: these stand after the cells' part, which is all the other reader of pos looks at)
                continue;
            }
            bool have = false;
            for (const GObj& o : objs) if (o.uid == uid) { have = true; break; }
            if (have) continue;
            auto it = uidIndex.find(uid);
            if (it != uidIndex.end()) objs.push_back((*world)[it->second]);
        }
        sm(8);
        std::vector<GObj*>& cand = nbCand;
        cand.resize(nCand);   // (the same list as a clear and nCand push_backs, which were 0.5 us of a tick with 170 objects around the player)
        { GObj* const o0 = objs.data(); GObj** const c0 = cand.data(); for (size_t i = 0; i < nCand; ++i) c0[i] = o0 + i; }
        Ctx& cp = stepCtx(2);
        if (in.recordGameEvents) cp.gameEvents = &out.gameEvents;
        cp.objs = &objs; cp.L = L;
        if (nb.ok) { cp.lkWorld = &uidOnce; cp.lkIndex = [](const void* m, int u) -> long long { const auto& M = *(const std::unordered_map<int, long long>*)m; const auto it = M.find(u); return it == M.end() ? -1 : it->second; }; cp.lkObjs = &objs; cp.lkStamp = nb.stamp[1].data(); cp.lkPos = nb.pos[1].data(); cp.lkGen = nb.gen[1]; }   // Ctx::byUid: nbPass by place (the cells' part and the slot uids appended above)
        cp.spiderJump = [](Ctx& cc) { return spider::spiderTestJump(cc); };
        cp.slopeRotation = [](Ctx& cc, float d) { upd::updateSlopeRotation(cc.P, d); };
        cp.dashRot = &dashRot;
        cp.stopDash = [](Ctx& cc) { SelfShadow& S = *g_self(); upd::stopDashing(cc); S.dashOn = false; };
        bindDashRestart(cp);
        cp.ringJump = [](Ctx& cc, GObj* o) {   // the pass touches a ring: playerTouchedRing registers it, then PlayerObject::ringJump decides (buttons.hpp)
            SelfShadow& S = *g_self();
            S.rt.touched(o->uid);
            S.passRings.insert(o->uid);
            ButtonsBody b; b.c = &cc; b.touch = &S.rt; b.usedRings = &S.usedRings;
            ButtonsHooks hk;
            hk.updateJump = [&](Ctx& c2, float dt) { auto* sv = c2.objs; c2.objs = S.world; upd::updateJump(c2, dt); c2.objs = sv; };
            hk.spiderTestJump = [&](Ctx& c2, const GObj*) { return spider::spiderTestJump(c2); };
            hk.startDashing = [&](Ctx& c2, const GObj* ring) { upd::startDashing(c2, ring); S.restartDashAction(c2.P); };
            hk.stopDashing = [&](Ctx& c2) { upd::stopDashing(c2); S.dashOn = false; };
            hk.teleportPlayer = [&](Ctx& c2, const GObj* ring) { GObj og = *ring; teleportPortal(c2, &og); };
            return ringJump(b, o, hk);
        };
        passRings.clear();
        spider::worldOverride() = world;
        cp.gateIn = &gate; cp.everIn = &ever; if (g_everPayloadOn) cp.everBase = p2Phase ? &g_everPayloadSet2 : &g_everPayloadSet;   /* (each body has its own: ever_payload.hpp) */ if (everRule.on) { cp.everRule = &SelfShadow::everRuleSays; cp.everRuleCtx = &everRule; }
        cp.hsKnown = true; cp.p22 = p22; cp.usedRings = &usedRings; cp.fireOrder = &fireOrder; cp.dualH = dualH;
        cp.rotApply = in.rotApply; cp.rotSpent = in.rotSpent;
        actLog.clear(); cp.actLog = &actLog;
        sm(9);
        out.ret = checkCollisions(cp, in.dt, false, cand);
        sm(10);
        spider::worldOverride() = nullptr;
        gate.swap(cp.gateNow);   // (= cp.gateNow: nothing reads the pass's Ctx after this, and the next stepCtx(2) clears it)
        ever.insert(cp.everNow.begin(), cp.everNow.end());
        for (int uid : cp.ringsTouched) if (!passRings.count(uid)) rt.touched(uid);
        out.fired = cp.fired;
        out.rotFired = cp.rotFired;
    out.everGuess = cp.everGuess;
        for (const auto& f : cp.fired) {                                 // a speed portal (a Modifier object, ids 200-203 and 1334) sets the layer's speed; PlayerObject::updateTimeMod runs at the head of the next step
            if (f.first != 0x14 + 1000) continue;
            auto it = uidIndex.find(f.second);
            if (it == uidIndex.end()) continue;
            const int id = (*world)[it->second].objectId;
            if (id == 200) setSpeed(0.7f); else if (id == 201) setSpeed(0.9f); else if (id == 202) setSpeed(1.1f); else if (id == 203) setSpeed(1.3f); else if (id == 1334) setSpeed(1.6f);
        }
        out.died = cp.died; out.why = cp.why; out.whyUid = cp.whyUid;
        addUnsupported(out, cp);
        sm(11);
        writeBack(objs, nbPassIdx);
        if (in.ctrlLate && in.ctrl) {
            Ctx cl(P);
            cl.L = L; cl.objs = world; cl.p22 = p22; cl.hsKnown = true; cl.usedRings = &usedRings; cl.fireOrder = &fireOrder;
            ButtonsBody bl; bl.c = &cl; bl.touch = &rt; bl.usedRings = &usedRings;
            for (int k = 1; k <= 3; ++k) bl.holding[k] = holding[k];
            ButtonsHooks hl;
            hl.stopDashing = [&](Ctx& c3) { upd::stopDashing(c3); dashOn = false; };
            if (in.ctrl == 1) disablePlayerControls(bl, hl); else enablePlayerControls(bl, hl);
            for (int k = 1; k <= 3; ++k) holding[k] = bl.holding[k];
        }
        atExit = P;
        sm(12);
        if (in.tpLateUid >= 0) doTeleport(in.tpLateUid, false, out);
        if (in.ctrlAfter) {
            Ctx cl(P);
            cl.L = L; cl.objs = world; cl.p22 = p22; cl.hsKnown = true; cl.usedRings = &usedRings; cl.fireOrder = &fireOrder;
            ButtonsBody bl; bl.c = &cl; bl.touch = &rt; bl.usedRings = &usedRings;
            for (int k = 1; k <= 3; ++k) bl.holding[k] = holding[k];
            ButtonsHooks hl;
            hl.stopDashing = [&](Ctx& c3) { upd::stopDashing(c3); dashOn = false; };
            if (in.ctrlAfter == 1) disablePlayerControls(bl, hl); else enablePlayerControls(bl, hl);
            for (int k = 1; k <= 3; ++k) holding[k] = bl.holding[k];
        }

        // ---- after the pass ----
        {
            float f = (float)stepSeconds + P.f32(0xae0);
            P.f32(0xae0, f);
            if (0.01f <= f) {                                 // (measured: the subtraction is in float; a double constant is off by an ulp on every wrap)
                P.f32(0xae0, f - 0.01f);
                P.i32(0xac4, P.i32(0xac4) + 1);
            }
        }
        P.f32(0xa10, P.f32(0x38));
        upd::updateRotation(P, in.dt);
        P.f32(0xa00, P.f32(po::PosX));
        P.f32(0xa04, P.f32(po::PosY));
        if (in.end >= 0 ? in.end != 0 : in.tick % frameLen == frameEndMod) {                      // the end of a frame: the position the next one starts from (the visual interpolation after it is not ported)
            P.f32(0xa98, P.f32(po::PosX));
            P.f32(0xa9c, P.f32(po::PosY));
        }
        if (in.tpEndUid >= 0) doTeleport(in.tpEndUid, true, out);
        out.ran = true;
        sm(13);
    }

    // the world index of a uid (-1 when the world does not hold it)
    long long worldIndexOf(int uid) const { auto it = uidIndex.find(uid); return it == uidIndex.end() ? -1 : (long long)it->second; }

private:
    // The three Ctx of a step (the buttons, the update, the pass), made once over `img` and reset at each use (ctxReset: what a fresh Ctx(img) holds)
    // instead of constructed: a Ctx's six sets / maps allocated on construction. unique_ptr: a copied SelfShadow must not hold Ctx bound to another's image.
    std::unique_ptr<Ctx> ctxs[3];
    ButtonsHooks headHooks;   // stepHead's hooks (capture `this`; a copied SelfShadow is not possible: ctxs)
    bool headHooksSet = false;
    Ctx& stepCtx(int k) {
        if (!ctxs[k] || &ctxs[k]->P != &img) ctxs[k].reset(new Ctx(img));
        else ctxReset(*ctxs[k]);
        return *ctxs[k];
    }
    std::unordered_map<int, size_t> uidIndex;
    std::unordered_map<int, long long> uidOnce;
    std::set<int> passRings;   // rings the pass handed to ringJump (already registered)
    static SelfShadow*& g_self() { static thread_local SelfShadow* s = nullptr; return s; }   // (thread_local: leveldp's search steps shadows on several threads)

    // the objects a phase changed (a ring marked activated by its jump) go back into the world. `idx` is where each copy came from when the neighbourhood knows it (uids unique:
    // the same object uidIndex names); an entry it does not cover is looked up by uid.
    // With the index (idx parallel to v) and every logged copy (actLog: what ringJump marked, the only writer of `activated`) inside v, only those
    // copies can differ from the world -- the rest were copied from it (or read back from it) at the neighbourhood call and nothing wrote either
    // since -- so only those are written; the other buffer's reused copies may then be stale and are read again at their next use.
    void setActivated(size_t i, bool v) {
        GObj& w = (*world)[i];
        if (actUndo && w.activated != v) actUndo->emplace_back((uint32_t)i, (uint8_t)w.activated);
        if (v && actTrue) actTrue->push_back((uint32_t)i);
        w.activated = v;
    }
    void writeBack(const std::vector<GObj>& v, const std::vector<uint32_t>& idx) {
        if (p2Phase) {   // Rings are per player; destroyObject (2.2091 0x31d536) writes the shared object.
            for (const GObj* p : actLog) {
                if (p < v.data() || p >= v.data() + v.size() || !p->activated) continue;
                if (p->type != 0x15) { act2.insert(p->uid); continue; }
                auto it = uidIndex.find(p->uid);
                if (it != uidIndex.end()) { setActivated(it->second, true); activatedChangedAt((uint32_t)it->second); }
            }
            actLog.clear();
            return;
        }
        bool logged = nb.ok && idx.size() == v.size();
        for (const GObj* p : actLog) if (p < v.data() || p >= v.data() + v.size()) { logged = false; break; }
        if (logged) {
            for (const GObj* p : actLog) {
                const size_t i = (size_t)(p - v.data());
                const GObj& o = v[i];
                if ((*world)[idx[i]].uid == o.uid) { setActivated(idx[i], o.activated); activatedChangedAt(idx[i]); continue; }
                auto it = uidIndex.find(o.uid);
                if (it != uidIndex.end()) { setActivated(it->second, o.activated); activatedChangedAt((uint32_t)it->second); }
            }
            actLog.clear();
            return;
        }
        actLog.clear();
        activatedChanged();
        for (size_t i = 0; i < v.size(); ++i) {
            const GObj& o = v[i];
            if (i < idx.size() && (*world)[idx[i]].uid == o.uid) { setActivated(idx[i], o.activated); continue; }
            auto it = uidIndex.find(o.uid);
            if (it != uidIndex.end()) setActivated(it->second, o.activated);
        }
    }

    static void addUnsupported(SelfOut& o, const Ctx& c) {
        o.unsupported += c.unsupported;
        for (const auto& u : c.unsupportedBy) o.unsupportedBy[u.first] += u.second;
        o.tps.insert(o.tps.end(), c.tpLog.begin(), c.tpLog.end());
    }

    // ---- the section index (S2 stage 0.5): what neighbourhood() reads instead of the whole level ----
    // Every object that is not a mover and not Extended Collision is filed once in its section cell, each cell's world indices in uid order. The Extended Collision objects are one flat
    // list in uid order (their position does not decide their place). The movers are filed in cells of their own, kept in uid order, and re-filed when they move: the caller says which
    // ones moved (moverMoved) or, if it does not (setMovers(..., false)), every mover's cell is read again at each call. Built at the first call after index() / setMovers(), and again
    // when the world's size or dynHeight changes. Not used (ok false) unless the caller named its movers and every uid is unique, so the merge by uid equals the old sort.
    struct NbIndex {
        bool built = false, ok = false, dyn = false;
        const std::vector<GObj>* w = nullptr;
        size_t n = 0;
        std::unordered_map<uint64_t, std::vector<uint32_t>> cells, mcells;   // static / movers: cell -> world indices in uid order
        std::vector<uint32_t> flat, movers;
        std::vector<uint64_t> mkey;    // a mover's cell now (by world index)
        std::vector<char> isMover;
        std::vector<uint32_t> dirty;   // movers the caller moved since the last call
        std::vector<char> isDirty;
        std::vector<uint32_t> stamp[2];   // == gen[b]: the world index is in the neighbourhood the last call into buffer b made (0 nbLocal, 1 nbPass)
        uint32_t gen[2] = {0, 0};
        std::vector<uint32_t> pos[2];     // where stamp[b] == gen[b]: the object's place in buffer b (the cells' part; activatedChangedAt's patch reads it)
    };
    NbIndex nb;
    std::vector<int> moverUids;
    bool moversKnown = false, moversNotify = false;
    std::vector<GObj> nbLocal, nbPass;            // the two neighbourhoods of a step, reused (fresh copies every call, as before)
    std::vector<uint32_t> nbLocalIdx, nbPassIdx;  // the world index of each copy (empty when the index is not used)
    std::vector<GObj*> nbCand;
    struct NbReuse { bool valid = false, needResync = false; int pcx = 0, pcy = 0; size_t base = 0; };   // what the last call into nbLocal / nbPass copied (the cells' part)
    std::vector<GObj*> actLog;   // Ctx::actLog of the buttons' and the pass's Ctx: the copies ringJump marked in this phase
    NbReuse nbReuseAt[2];
    bool nbReuse = false;
    std::vector<uint32_t> patchPend[2];   // per buffer: the movers written in place since the buffer last agreed with the world (their copies are read again when the buffer is reused)
    static constexpr size_t kPatchMax = 1024;   // past this a buffer is copied afresh (a patch of that many objects costs about what the whole copy does)

    static int cellOfD(double v, float s) { if (!(v > 0.0)) return 0; return (int)(v < 1e7 ? (double)s * v : (double)(s * 1e7f)); }
    static uint64_t cellKey(int cx, int cy) { return ((uint64_t)(uint32_t)cx << 32) | (uint64_t)(uint32_t)cy; }
    uint64_t keyOf(const GObj& o) const { return cellKey(cellOfD(o.realX(), tmpl::kScaleX), cellOfD(o.realY(), dynHeight ? 0.01f : 0.0f)); }
    void fileMover(uint32_t i, uint64_t k) {
        std::vector<uint32_t>& v = nb.mcells[k];
        const int u = (*world)[i].uid;
        v.insert(std::lower_bound(v.begin(), v.end(), u, [&](uint32_t a, int b) { return (*world)[a].uid < b; }), i);
        nb.mkey[i] = k;
    }
    void refileMover(uint32_t i) {
        const uint64_t k = keyOf((*world)[i]);
        if (k == nb.mkey[i]) return;
        std::vector<uint32_t>& v = nb.mcells[nb.mkey[i]];
        v.erase(std::find(v.begin(), v.end(), i));
        fileMover(i, k);
    }

    void buildIndex() {
        nb = NbIndex();
        nbReuseAt[0].valid = nbReuseAt[1].valid = false;
        nb.built = true; nb.w = world; nb.n = world ? world->size() : 0; nb.dyn = dynHeight;
        if (!world || !moversKnown) return;
        {
            std::vector<int> u; u.reserve(world->size());
            for (const GObj& o : *world) u.push_back(o.uid);
            std::sort(u.begin(), u.end());
            if (std::adjacent_find(u.begin(), u.end()) != u.end()) return;   // two objects with one uid: the old sort's order of the pair is not the merge's
        }
        nb.isMover.assign(world->size(), 0);
        nb.isDirty.assign(world->size(), 0);
        nb.mkey.assign(world->size(), 0);
        nb.stamp[0].assign(world->size(), 0);
        nb.stamp[1].assign(world->size(), 0);
        nb.pos[0].assign(world->size(), 0);
        nb.pos[1].assign(world->size(), 0);
        actDirty[0].clear(); actDirty[1].clear();
        for (int uid : moverUids) { auto it = uidIndex.find(uid); if (it != uidIndex.end()) nb.isMover[it->second] = 1; }
        for (size_t i = 0; i < world->size(); ++i) {
            const GObj& o = (*world)[i];
            if (o.extCol) { nb.flat.push_back((uint32_t)i); nb.isMover[i] = 0; continue; }   // its place is the flat list wherever it is
            if (nb.isMover[i]) { nb.movers.push_back((uint32_t)i); continue; }
            nb.cells[keyOf(o)].push_back((uint32_t)i);
        }
        auto byUid = [&](uint32_t a, uint32_t b) { return (*world)[a].uid < (*world)[b].uid; };
        for (auto& kv : nb.cells) std::sort(kv.second.begin(), kv.second.end(), byUid);
        std::sort(nb.flat.begin(), nb.flat.end(), byUid);
        for (uint32_t i : nb.movers) fileMover(i, keyOf((*world)[i]));
        nb.ok = true;
    }

public:
    // build the section index now (else the first step does): after world, index(), setMovers() and dynHeight are set
    void prepareNeighbourhood() { buildIndex(); }
    // Reuse a neighbourhood's copies from the last call for the same section cells instead of copying the objects again (S2 speed; leveldp's
    // shadow_step.hpp turns it on, self_test does not). The caller's promise: it calls worldChanged() whenever it changes anything of an object but
    // `activated` (a mover's place, box, angle, inactive byte...), and it names its movers with notification (setMovers(..., true)) or has none.
    void setNbReuse(bool on) { nbReuse = on; worldChanged(); }
    void worldChanged() { nbReuseAt[0].valid = nbReuseAt[1].valid = false; ++moverGen; patchPend[0].clear(); patchPend[1].clear(); }
    // The movers the caller wrote in place (shadowPlaceMovers: the objects it placed, and the ones the last placement had on that it has switched off now), whether or not they moved: a reused neighbourhood
    // reads the copies of those objects again instead of being dropped whole (patchPend, neighbourhood()), as long as none of them changes section cell. An index that is not a named mover of the index,
    // or the index not in use: worldChanged(), as before.
    // ...one object the caller wrote in place, whether or not its centre changed (its prevCy, box, inactive or farHold byte may have): the copy of it is read again; not a named mover of the index: worldChanged().
    void moverWritten(size_t i) {
        if (!nb.ok || i >= nb.isMover.size() || !nb.isMover[i]) { worldChanged(); return; }
        moverMoved(i);
    }
    void moversTouched(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
        if (!nb.ok) { worldChanged(); return; }
        for (const std::vector<uint32_t>* l : {&a, &b})
            for (uint32_t i : *l) {
                if (i >= nb.isMover.size() || !nb.isMover[i]) { worldChanged(); return; }
                moverMoved(i);
            }
    }
    uint64_t moverGen = 1;   // raised by worldChanged and moverMoved: what the spider's kept reaches of the movers are good for (spider.hpp Grid::liveGen)
    // ...and activatedChanged() whenever it changes an object's `activated` (shadow_state.hpp slUnpack, a seed)
    void activatedChanged() { nbReuseAt[0].needResync = nbReuseAt[1].needResync = true; actDirty[0].clear(); actDirty[1].clear(); }
    // ...or activatedChangedAt(i) when it knows which object's flag it wrote: a reused neighbourhood then reads that one copy again and not all of them
    // (on Geometrical Dominator four reuses in five read every copy again, about 170 objects each, because the state stepped before had fired a ring or a pad).
    // Past kActDirtyMax objects a buffer falls back to reading everything.
    static constexpr size_t kActDirtyMax = 64;
    std::vector<uint32_t> actDirty[2];   // per buffer: the world indices whose `activated` was written since the buffer last agreed with the world
    void activatedChangedAt(uint32_t i) {
        for (int b = 0; b < 2; ++b) {
            if (nbReuseAt[b].needResync) continue;
            if (actDirty[b].size() >= kActDirtyMax) { nbReuseAt[b].needResync = true; actDirty[b].clear(); continue; }
            actDirty[b].push_back(i);
        }
    }
    // non-null: every change writeBack makes to a world object's `activated`, as (world index, old value), so a caller can undo a step's head
    // (shadow_step.hpp's tail memo)
    std::vector<std::pair<uint32_t, uint8_t>>* actUndo = nullptr;
    // non-null: the world index of every object writeBack sets activated (shadow_state.hpp SlWorld::actTrueLog)
    std::vector<uint32_t>* actTrue = nullptr;
    // the caller changed this object's centre (cx / cy); a no-op for an object that is not a named mover
    void moverMoved(size_t worldIdx) {
        ++moverGen;   // (before the early return: a mover already noted dirty may move again)
        if (!nb.ok || worldIdx >= nb.isMover.size() || !nb.isMover[worldIdx] || nb.isDirty[worldIdx]) return;
        nb.isDirty[worldIdx] = 1;
        nb.dirty.push_back((uint32_t)worldIdx);
    }

private:
    // GJBaseGameLayer::checkCollisions 0x31aae0 walks the section cells x - 1 .. x + 1 outward and y - 1 .. y + 1 inward, each cell's objects in uid order, then the flat list of the objects with
    // Extended Collision (shadow.hpp spells it out). With `forButtons` the rings the player touches are added as well (pushButton looks them up by uid).
    // Fills `ordered` with fresh copies and `idx` with their world indices (left empty on the scan path).
    void neighbourhood(float px, float py, bool forButtons, std::vector<GObj>& ordered, std::vector<uint32_t>& idx) {
        if (!nb.built || nb.w != world || nb.n != (world ? world->size() : 0) || nb.dyn != dynHeight) buildIndex();
        if (!nb.ok) {
            ordered.clear(); idx.clear();
            ordered = neighbourhoodScan(px, py, forButtons);
            return;
        }
        if (moversNotify) {
            if (!nb.dirty.empty()) {
                // a mover that stays in its section cell keeps the place of every copy in the buffers (a cell's objects stand in uid order): only the copy of the mover itself is out of date, and the buffers
                // read it again (patchPend). One that changes cell changes which objects a buffer holds and in what order: the buffers are dropped, as before.
                bool cellMoved = false;
                for (uint32_t i : nb.dirty) { nb.isDirty[i] = 0; const uint64_t before = nb.mkey[i]; refileMover(i); if (nb.mkey[i] != before) cellMoved = true; }
                if (cellMoved) worldChanged();
                else {
                    ++moverGen;
                    for (int b = 0; b < 2; ++b) {
                        if (!nbReuseAt[b].valid) { patchPend[b].clear(); continue; }
                        if (patchPend[b].size() + nb.dirty.size() > kPatchMax) { nbReuseAt[b].valid = false; patchPend[b].clear(); continue; }
                        patchPend[b].insert(patchPend[b].end(), nb.dirty.begin(), nb.dirty.end());
                    }
                }
                nb.dirty.clear();
            }
        } else {
            for (uint32_t i : nb.movers) refileMover(i);
        }
        const float sxs = tmpl::kScaleX, sys = dynHeight ? 0.01f : 0.0f;
        const int pcx = px > 0.0f ? (int)(px < 1e7f ? px * sxs : sxs * 1e7f) : 0, pcy = py > 0.0f ? (int)(py < 1e7f ? py * sys : sys * 1e7f) : 0;
        const std::vector<GObj>& W = *world;
        // The reuse (setNbReuse): the copies the last call into this buffer made, for the same cells, are what a fresh copy would be except for
        // `activated` (written by ringJump and destroyBreakable; writeBack has put it into the world) and the extras appended after
        // them (the rings below, the pass's slot uids): cut back to the cells' part (the extras' stamps taken off, the cells' part keeps this buffer's
        // stamps), and `activated` read again from the world when it may have changed there since (needResync: the other buffer's writeBack wrote a
        // ring, or the caller said activatedChanged).
        const int slot = &ordered == &nbPass ? 1 : 0;
        std::vector<uint32_t>& stamp = nb.stamp[slot];
        uint32_t& gen = nb.gen[slot];
        NbReuse* ru = nullptr;
        if (nbReuse && (moversNotify || nb.movers.empty()) && (&ordered == &nbLocal || &ordered == &nbPass)) ru = &nbReuseAt[slot];
        if (ru && ru->valid && ru->pcx == pcx && ru->pcy == pcy && ordered.size() >= ru->base && idx.size() >= ru->base) {
            if (ssProf().on) { ssTl().cnt[10] += 1; if (ru->needResync) ssTl().cnt[14] += 1; }   // SSPROF: a neighbourhood taken from the last call's copies (and how many of those read `activated` again)
            for (size_t k = ru->base; k < idx.size(); ++k) stamp[idx[k]] = 0;
            ordered.resize(ru->base); idx.resize(ru->base);
            if (!patchPend[slot].empty()) {   // the movers written in place since: the copy of each read again, the object whole (`activated` too: the buffer agrees with the world after this)
                const std::vector<uint32_t>& pos = nb.pos[slot];
                for (uint32_t i : patchPend[slot]) if (stamp[i] == gen && pos[i] < ru->base && std::memcmp(&ordered[pos[i]], &W[i], sizeof(GObj)) != 0) ordered[pos[i]] = W[i];
                patchPend[slot].clear();
            }
            if (ru->needResync) {
                for (size_t k = 0; k < ru->base; ++k) ordered[k].activated = W[idx[k]].activated;
                ru->needResync = false;
            } else {   // only the objects whose flag was written since (activatedChangedAt): those of them that are in this buffer's cells' part
                const std::vector<uint32_t>& pos = nb.pos[slot];
                for (uint32_t i : actDirty[slot]) if (stamp[i] == gen && pos[i] < ru->base) ordered[pos[i]].activated = W[i].activated;
            }
            actDirty[slot].clear();
            if (nbReuseCheck()) {   // ST_NBREUSECHECK=1: the fresh copies beside the reused ones, whole objects
                std::vector<GObj> fo; std::vector<uint32_t> fi;
                fillNeighbourhood(pcx, pcy, fo, fi);
                bool same = fi == idx && fo.size() == ordered.size();
                for (size_t k = 0; same && k < fo.size(); ++k) same = std::memcmp(&fo[k], &ordered[k], sizeof(GObj)) == 0;
                if (!same) { std::fprintf(stderr, "ST_NBREUSECHECK: the reused neighbourhood differs from a fresh one at (%f, %f) forButtons %d\n", px, py, (int)forButtons); std::abort(); }
            }
        } else {
            if (ssProf().on) { ssTl().cnt[11] += 1; ssTl().cnt[!ru ? 12 : 13] += 1; }   // SSPROF: copied afresh -- 12 the reuse is not allowed here (movers the caller does not announce), 13 another cell or nothing kept
            ordered.clear(); idx.clear();
            fillNeighbourhood(pcx, pcy, ordered, idx);
            if (++gen == 0) { std::fill(stamp.begin(), stamp.end(), 0u); gen = 1; }
            { std::vector<uint32_t>& pos = nb.pos[slot]; uint32_t k = 0; for (uint32_t i : idx) { stamp[i] = gen; pos[i] = k++; } }
            actDirty[slot].clear();   // (fresh copies: the buffer agrees with the world)
            patchPend[slot].clear();
            if (ru) { ru->valid = true; ru->needResync = false; ru->pcx = pcx; ru->pcy = pcy; ru->base = idx.size(); }
        }
        if (forButtons) {
            for (int uid : rt.arr) {   // (uids are unique: a uid's copy is in `ordered` iff its world index is stamped)
                auto it = uidIndex.find(uid);
                if (it == uidIndex.end() || stamp[it->second] == gen) continue;
                ordered.push_back(W[it->second]); idx.push_back((uint32_t)it->second); stamp[it->second] = gen;
            }
        }
        if (p2Phase) for (GObj& o : ordered) if (o.type != 0x15) o.activated = act2.count(o.uid) != 0;   // --shadowringp2: player 2 sees its own fired set, but shared breakables
        if (nbCheck() && !p2Phase) {   // ST_NBRCHECK=1: the old scan beside the index, every call
            const std::vector<GObj> ref = neighbourhoodScan(px, py, forButtons);
            bool same = ref.size() == ordered.size();
            for (size_t k = 0; same && k < ref.size(); ++k) {
                const GObj &r = ref[k], &o = ordered[k];
                same = r.uid == o.uid && std::memcmp(&r.rx, &o.rx, sizeof(float) * 6) == 0   // rx ry rw rh cx cy
                    && std::memcmp(&r.prevCx, &o.prevCx, sizeof(float) * 5) == 0   // prevCx prevCy radius scaleX scaleY
                    && r.activated == o.activated && r.inactive == o.inactive && r.farHold == o.farHold && r.type == o.type;
            }
            if (!same) { std::fprintf(stderr, "ST_NBRCHECK: the index's neighbourhood differs from the scan at (%f, %f) forButtons %d: %zu vs %zu objects\n", px, py, (int)forButtons, ordered.size(), ref.size()); std::abort(); }
        }
    }
    static bool nbCheck() { static const bool on = std::getenv("ST_NBRCHECK") != nullptr; return on; }
    static bool nbReuseCheck() { static const bool on = std::getenv("ST_NBREUSECHECK") != nullptr; return on; }
    // the cells around (pcx, pcy) in the game's order, then the flat list: world indices into idx, fresh copies into ordered (both empty on entry)
    void fillNeighbourhood(int pcx, int pcy, std::vector<GObj>& ordered, std::vector<uint32_t>& idx) {
        const std::vector<GObj>& W = *world;
        static const std::vector<uint32_t> none;
        for (int ix = std::max(0, pcx - 1); ix <= pcx + 1; ++ix)
            for (int iy = std::max(0, pcy - 1); iy <= pcy + 1; ++iy) {
                const uint64_t k = cellKey(ix, iy);
                auto si = nb.cells.find(k);
                auto mi = nb.mcells.find(k);
                const std::vector<uint32_t>& st = si != nb.cells.end() ? si->second : none;
                const std::vector<uint32_t>& mv = mi != nb.mcells.end() ? mi->second : none;
                if (mv.empty()) { idx.insert(idx.end(), st.begin(), st.end()); continue; }
                size_t a = 0, b = 0;
                while (a < st.size() || b < mv.size()) {
                    const bool takeStatic = b >= mv.size() || (a < st.size() && W[st[a]].uid < W[mv[b]].uid);
                    idx.push_back(takeStatic ? st[a++] : mv[b++]);
                }
            }
        idx.insert(idx.end(), nb.flat.begin(), nb.flat.end());
        ordered.reserve(idx.size() + 12);
        for (uint32_t i : idx) ordered.push_back(W[i]);
    }

    // the old form: the whole level at every call (the reference ST_NBRCHECK compares with, and the path when the movers are not known)
    std::vector<GObj> neighbourhoodScan(float px, float py, bool forButtons) {
        auto cellOf = [](double v, float s) -> int { if (!(v > 0.0)) return 0; return (int)(v < 1e7 ? (double)s * v : (double)(s * 1e7f)); };
        const float sxs = tmpl::kScaleX, sys = dynHeight ? 0.01f : 0.0f;
        std::vector<GObj> ordered;
        const int pcx = px > 0.0f ? (int)(px < 1e7f ? px * sxs : sxs * 1e7f) : 0, pcy = py > 0.0f ? (int)(py < 1e7f ? py * sys : sys * 1e7f) : 0;
        std::vector<const GObj*> nearby;
        for (const GObj& o : *world) {
            if (o.extCol) continue;
            const int cx = cellOf(o.realX(), sxs), cy = cellOf(o.realY(), sys);
            if (cx >= pcx - 1 && cx <= pcx + 1 && cy >= pcy - 1 && cy <= pcy + 1) nearby.push_back(&o);
        }
        for (int ix = std::max(0, pcx - 1); ix <= pcx + 1; ++ix)
            for (int iy = std::max(0, pcy - 1); iy <= pcy + 1; ++iy) {
                std::vector<const GObj*> cell;
                for (const GObj* o : nearby) if (cellOf(o->realX(), sxs) == ix && cellOf(o->realY(), sys) == iy) cell.push_back(o);
                std::sort(cell.begin(), cell.end(), [](const GObj* a, const GObj* b) { return a->uid < b->uid; });
                for (const GObj* o : cell) ordered.push_back(*o);
            }
        std::vector<const GObj*> flat;
        for (const GObj& o : *world) if (o.extCol) flat.push_back(&o);
        std::sort(flat.begin(), flat.end(), [](const GObj* a, const GObj* b) { return a->uid < b->uid; });
        for (const GObj* o : flat) ordered.push_back(*o);
        if (forButtons) {
            for (int uid : rt.arr) {
                bool have = false;
                for (const GObj& o : ordered) if (o.uid == uid) { have = true; break; }
                if (have) continue;
                auto it = uidIndex.find(uid);
                if (it != uidIndex.end()) ordered.push_back((*world)[it->second]);
            }
        }
        return ordered;
    }
};

}  // namespace gdcol
}  // namespace dp
