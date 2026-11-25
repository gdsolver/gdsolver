#pragma once
// ============================================================
// Timeline of moving geometry (cfg `grouptrace=1`)
//
// The static dump only holds the coordinates at level entry, so on levels with
// moving objects we measure "the rect of this uid at this tick" directly. For
// every object that belongs to a group, the hitbox rect is written out every
// tick, but only when it changed. Rotation and toggles also show up as rect
// changes, so the model side never has to implement any trigger semantics.
//
// Why the rect itself is emitted: the group offset (dx,dy) is not enough.
// One object can belong to several groups, and a rotate trigger changes the
// shape rather than the position. getObjectRect() is the rect GD itself uses
// for collision.
//
// [Relation to the plan — absolute rule] This recording is not external
// ground-truth data. The driver takes it every iteration from the replay of the
// plan the model itself produced, so it is self-contained inside the cold loop
// (does not conflict with the spec ('solving is always done from scratch')).
// ============================================================
#include <charconv>
#include <chrono>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <xmmintrin.h>
#endif
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace grouptrace {

// One row of the recording, `<t>,<uid>,<cx>,<cy>,<w>,<h>,<on>,<rot>` (and `,1` for an envelope row), newline
// last: the bytes snprintf("%lld,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f\n") writes, put together with std::to_chars,
// which costs a third (1,474 -> 464 ns a row, measured on 4 M rows; a custom level's bootstrap recording is 21.6 M
// rows and the flight that wrote them spent 36 s of its 42 in this function). to_chars(float, fixed, 3) writes
// the same text as "%.3f" of the float taken as a double: checked on 159.5 M floats -- random bit patterns of every
// exponent, rounding ties and the grid a rect lies on, the non-finite ones too -- with no difference. A row that
// does not fit goes the way it always went: snprintf, which cuts it at 159 characters.
inline size_t formatRow(char (&b)[160], long long t, int uid, float cx, float cy, float w, float h, int on,
                        float rot, bool env) {
#ifdef _MSC_VER
    // (Windows only: the Android toolchain's library may not have the floating-point to_chars, and its rows
    // stay snprintf's.)
    {
        char* p = b;
        char* const last = b + sizeof(b) - 1;   // snprintf keeps the last place for the NUL
        bool ok = true;
        auto num = [&](auto v) {
            const auto r = std::to_chars(p, last, v);
            if (r.ec == std::errc()) p = r.ptr; else ok = false;
        };
        auto fix = [&](float v) {
            const auto r = std::to_chars(p, last, v, std::chars_format::fixed, 3);
            if (r.ec == std::errc()) p = r.ptr; else ok = false;
        };
        auto put = [&](char c) {
            if (p < last) *p++ = c; else ok = false;
        };
        num(t); put(',');
        num(uid); put(',');
        fix(cx); put(',');
        fix(cy); put(',');
        fix(w); put(',');
        fix(h); put(',');
        num(on); put(',');
        fix(rot);
        if (env) { put(','); put('1'); }
        put('\n');
        if (ok) return (size_t)(p - b);
    }
#endif
    if (env)
        snprintf(b, sizeof(b), "%lld,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f,1\n", t, uid, cx, cy, w, h, on, rot);
    else
        snprintf(b, sizeof(b), "%lld,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f\n", t, uid, cx, cy, w, h, on, rot);
    return std::strlen(b);
}

// Wall time spent inside tick() and the number of calls, since the counters were last taken (cfg profsample
// prints them with each roll: `gttime:`). Two clock reads a tick.
inline long long g_tickNanos = 0, g_tickCalls = 0;
// ...and the CPU cycles the thread spent in it (QueryThreadCycleTime): a wall clock moves with the machine's other
// work, the cycles of the thread do not, so two builds can be compared while other runs are going.
inline unsigned long long g_tickCycles = 0;
inline unsigned long long threadCycles() {
#ifdef GEODE_IS_WINDOWS
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return (unsigned long long)c;
#else
    return 0;
#endif
}

// cfg gtrollcheck: FNV-1a of a whole file (0 when it cannot be read).
inline unsigned long long fileFnv(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    unsigned long long h = 1469598103934665603ULL;
    std::vector<char> buf(1 << 20);
    while (f.read(buf.data(), (std::streamsize)buf.size()) || f.gcount()) {
        const std::streamsize got = f.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            h ^= (unsigned char)buf[(size_t)i];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

inline bool g_on = false;      // cfg `grouptrace=1`
inline long long g_stride = 1; // cfg `groupstride=<n>` (subsampling)
// cfg `grouptraceall=1`: track every object, not only the grouped non-decorations.
// A MEASURING mode, not a solving one: build()'s normal filters encode the premise
// "whatever has no groups (or is a type-7 decoration) sits where the static dump
// says". lv22's chained platforms broke both halves of that premise at once --
// uid17103 (id 4300, type 7, one group, moved by the 2.2 keyframe system rather
// than by any move trigger) carries GD's ship up 26px at t=16,409-16,428 while
// the recording had no row for it, so the model flew through the place GD was
// standing. The dedupe keeps the cost sane: a static object emits exactly one row
// per attempt. This is how "what actually moves here" is measured before deciding
// what the permanent tracking predicate should be.
inline bool g_all = false;
// cfg `grouptracksmart=1` (off): also track what the normal filter drops although it MOVES, found by measuring with grouptraceall on 20 levels (notes/mover-survey-1030.md): (a) [mode 2] a gameplay object
// with no group (the robot portal of a custom level drifted 20 px under something that is not a group nor m_hasAreaParent, while the recording held the dump position, so the portal acted a step early),
// (b) a decoration in the target group of a Teleport trigger / orb (an Advanced Follow carries such an exit with the player: the teleport lands at the exit's position NOW), (c) a trigger object with
// Touch Triggered set (its touch rect moves with its group). Writes smart_tracked.txt (uid, rule) so the rules can be held against grouptraceall.
// modes: 0 off, 1 rules (b) and (c) below, 2 rules (a), (b), (c) with (a) = every gameplay object that has no group; 3 = 2, but (a) also in grouptrace.txt; 4 = 3, but a row for (a) only from the first time it moves.
// ON (4) by default since 2026-10-30 (user ruling, after the 6-level fidelity run: 68 -> 67 diverged sections, none worse); grouptracksmart=0 is the old behaviour.
inline int g_smart = 4;
// cfg `bootstrapgate` (ON by default; bootstrapgate=0 is the old behaviour): the bootstrap record (repair.hpp startBootstrapRecord, one
// run of the whole level with no inputs and no dying) only on a level where something can move. Since g_smart=4 every ungrouped gameplay
// object is tracked as smartQuiet (a row only once it moves), so every level had tracked objects and every level paid that run -- on a
// level where nothing moves it recorded 0 samples and was discarded (lv5: 12 s -> 7 s a level, 2026-11-13). The gate runs it when a
// tracked object is not smartQuiet, or when the level has an Area trigger that can move objects with no group (g_areaMovers).
inline bool g_bootGate = true;
inline int g_nonQuiet = 0;       // build(): tracked objects that are written to grouptrace.txt from their first reading (not smartQuiet, not smartOnly)
inline int g_areaMovers = 0;     // build(): Area / Edit Area Move, Rotate, Scale triggers (3006-3008, 3011-3013)
inline int g_rotGameplay = 0;    // build(): Rotate Gameplay triggers (2900)
// WHEN THE SMARTQUIET OBJECTS ARE SCANNED. A smartQuiet object (rule (a): a gameplay object with no group) writes a row only once it
// moves, and on 21 of the 22 official levels none does (a cold with grouptracksmart=0 and one with 4 write the same rows and the same
// [fp], 2026-11-19), yet scanning them costs 18 ns an object a tick: 1,000-5,800 objects on every level, 17-50% of a verification
// flight. What moves an object that no group holds (read from the 22091 binary, 2026-11-19: the only code that stores a game object's
// position is the group actions -- moveObjects, rotateObjects, the follow actions, the loaded move actions --, the Area effects
// (processAreaEffects -> moveAreaObject / rotate / transform, which take the target group's objects or an enter channel's), the
// rotate-gameplay pair that turns an object around the player for one collision test and back (handleRotatedCollisionInternal), and
// the start / reset / editor code): so a level with no Area trigger (3006-3008; 3011-3013 only edit a running one) and no Rotate
// Gameplay trigger (2900) cannot move them, and after the baseline tick they are not read again. Every observation that wants every
// object every tick (wtrace, wscale, gtscaleoff, grouptraceall) keeps scanning, and so does cfg grouptracksmartaudit, which also says
// when one of them moves on a level where the premise says none can (`gt_premise:`).
inline bool g_quietScan = true;      // build(): the smartQuiet objects are scanned every tick
inline bool g_quietPremise = false;  // build(): no trigger on the level that can move an object without a group
inline bool g_quietBase = false;     // this attempt's baseline of the smartQuiet objects has been taken (tick() reads them once)
inline std::vector<uint32_t> g_activeIdx;   // build(): the indices (ascending, so the rows keep their order) of the objects that are not smartQuiet
inline int g_premiseBreaks = 0;
struct Tracked {
    GameObject* obj = nullptr;
    int uid = -1;
    // The last rect written out. Reference for writing only changes above the threshold
    float cx = 0, cy = 0, w = 0, h = 0;
    // getRotation(). w,h alone are NOT enough: this rect is getObjectRect() =
    // the axis-aligned bounding box, so a rotated object looks as if "its size
    // has grown", and the reader would use that as the hitbox as-is.
    // Measured (lv22 uid 434, spider portal, base 34x86): 37.6x87.4 at t=768,
    // 80.7x88.1 at t=810 — fits exactly the bounding box of 34x86 rotated from
    // 3° to 38°. The model collides with this bounding box, so the mode changes
    // 3 ticks earlier than in GD, and that was the primary cause of the walls at
    // x=2,200 / 3,956 in lv22.
    // Recovering the angle from the bounding box only yields |cos|,|sin| and the
    // SIGN IS UNDETERMINED (θ and -θ give mirror-image boxes), so asking GD is
    // the reliable way.
    float rot = 0;
    int on = -1;   // negation of GameObject::m_isGroupDisabled (toggle)
    // 1 = the row is the box GD's random numbers can put the object in, not its rect
    // (areaenv.hpp). Written as a ninth column, only on such rows, so a level without them
    // records the same bytes as before the column existed.
    int env = 0;
    bool emitted = false;
    // cfg grouptracksmart: tracked only for wtrace.csv (the shadow's input). grouptrace.txt, which leveldp reads, is left byte-for-byte as the normal filter writes it: reading the extra rows changed the model's
    // section divergences (6 levels: 68 -> 76 of 319 with mode 2, a custom level 20 -> 29/30), so the model does not take the new objects yet.
    bool smartOnly = false;
    // mode 4: an ungrouped gameplay object is written to grouptrace.txt only once it has MOVED: its first reading of an attempt is the baseline, not a row (the reader holds the dump position until a row says otherwise)
    bool smartQuiet = false;
    // cfg wtrace: the last exact reading written to wtrace.csv (position and rotation as GD holds them, the toggle byte), to write a row on any change at all
    float wx = 0, wy = 0, wrot = 0, ww = 0, wh = 0;
    int won = -1;
    bool wValid = false;
    // cfg wtlast: the last reading of the object's "last position" (+0x4d0, +0x4d4: GJBaseGameLayer::moveObjects sets it) and its stamp (+0x4dc: the layer counter at that move)
    float wlx = 0, wly = 0;
    int wstamp = 0;
    // cfg wscale / gtscaleoff: the last readings written to wscale.csv (the node scale and skew) and scaleoff.csv (the Area Scale offsets)
    float wsx = 0, wsy = 0, wrx = 0, wry = 0, sox = 0, soy = 0;
    bool wsValid = false, soValid = false;
    // cfg gtrestrow: the previous tick's values as read (not as written), to see the object come to rest
    float pcx = 0, pcy = 0, pw = 0, ph = 0, prot = 0;
    bool pvalid = false;
};

inline std::vector<Tracked> g_objs;
// cfg `wtrace=1` (with grouptrace=1; off by default): wtrace.csv, the tracked objects' state as GD's live objects hold it, at full precision and with no threshold -- a row (tick, uid, x, y, rotation, on, w, h: the rect size, which a Scale trigger changes)
// whenever any of them differs from the last row written for that object, and the first reading of every object at the first tick of an attempt. An observation that the trigger shadow
// (dp/src/dp/trigshadow.hpp) is compared with; grouptrace.txt's rows are a rect centre kept to 0.05 and cannot be.
inline bool g_wtOn = false;
// cfg `wtlast=1` (with wtrace=1; off by default): three more columns (lx, ly, stamp) on every wtrace.csv row -- the object's +0x4d0 / +0x4d4 / +0x4dc as GD's live object holds them -- and a row also when one of them changes.
// Those are what the game's collision passes compare an object's position with ("has it moved since the last tick"); moveObjects writes them only for an object a Move action touches and only when the counter differs.
inline bool g_wtLast = false;
// The layer g_objs was built against. build() is only safe to skip (falling through to
// restart(), which does not re-walk the object list) when the caller is the SAME layer that
// populated it -- otherwise the vector is still non-empty but every GameObject* in it belongs to
// a level that has since been torn down. `g_objs.empty()` used to be the gate at the call site,
// which is true only "once per process", not "once per level": the first level entered after a
// session that had grouptrace on left its Tracked entries behind for the next level to inherit.
// Measured (2026-08-24): solving lv16 then replaying lv20 dereferenced a stale lv16 GameObject*
// in tick(), crashing on getObjectRect() with a freed (once) or zeroed (once) vtable slot.
inline GJBaseGameLayer* g_owner = nullptr;

// The line end of the recording's rows. The file is written in binary mode, so the end of a line is written
// by hand: the text-mode stream that wrote it before put a CR in front of every LF on Windows (and nothing on
// the other platforms), and the file's bytes -- not only its rows -- are what the loop's signatures hash.
#ifdef GEODE_IS_WINDOWS
inline constexpr const char* kHeader = "tick,uid,cx,cy,w,h,on,rot\r\n";
#else
inline constexpr const char* kHeader = "tick,uid,cx,cy,w,h,on,rot\n";
#endif
// b[0..n) is a row from formatRow, ending in a newline unless it was cut: the newline as the file has it.
inline size_t endRow(char (&b)[160], size_t n) {
#ifdef GEODE_IS_WINDOWS
    if (n > 0 && n < sizeof(b) && b[n - 1] == '\n') { b[n - 1] = '\r'; b[n] = '\n'; ++n; }
#endif
    return n;
}

// The recording's file: binary mode, and the rows put together in a buffer of our own that goes to the file
// a megabyte at a time. A row of a tracked object is written by the million on a heavy level (21.6 M rows in the
// bootstrap recording of a 167,383-object custom level), and what the stream cost per row -- a locked call into the
// CRT, its 4 KB buffer, the CR it inserts in text mode -- was a third of the time of tick(): 614 -> 342 ns a row
// measured on 4 M rows, the same bytes. The stream's own calls stay what cp_flight.hpp uses (is_open, open, close,
// flush, tellp, seekp); tellp and seekp hand the buffer to the file first, so a position is the file's.
// Rows reach the file at least every 600 ticks (tick() flushes), as they did: a worker that is killed loses no more.
class RowFile {
public:
    RowFile() : buf_(kBuf) {}
    ~RowFile() { close(); }
    RowFile(const RowFile&) = delete;
    RowFile& operator=(const RowFile&) = delete;
    bool is_open() const { return f_.is_open(); }
    void open(const std::string& path, std::ios::openmode mode) {
        f_.open(path, mode | std::ios::binary);
        n_ = 0;
    }
    void close() {
        if (f_.is_open()) { drain(); f_.close(); }
        n_ = 0;
    }
    void flush() {
        if (f_.is_open()) { drain(); f_.flush(); }
    }
    long long tellp() {
        drain();
        return (long long)f_.tellp();
    }
    void seekp(std::streamoff off, std::ios::seekdir dir) {
        drain();
        f_.seekp(off, dir);
    }
    void write(const char* p, size_t n) {
        if (n == 0 || !f_.is_open()) return;
        if (n_ + n > buf_.size()) drain();
        if (n >= buf_.size()) { f_.write(p, (std::streamsize)n); return; }
        std::memcpy(buf_.data() + n_, p, n);
        n_ += n;
    }
    void write(const std::string& s) { write(s.data(), s.size()); }
    void write(const char* s) { write(s, std::strlen(s)); }

private:
    void drain() {
        if (n_) { f_.write(buf_.data(), (std::streamsize)n_); n_ = 0; }
    }
    static constexpr size_t kBuf = 1u << 20;
    std::ofstream f_;
    std::vector<char> buf_;
    size_t n_ = 0;
};
inline RowFile g_out;
inline RowFile g_wt;   // wtrace.csv (cfg wtrace)
// cfg `wscale=1` (with grouptrace=1; off by default): wscale.csv, the tracked objects' own scale and skew as the node holds them -- rows (tick, uid, scaleX, scaleY, rotationX, rotationY) written only when one of them differs from the object's last row,
// and the first reading of every object at the first tick of an attempt. wtrace.csv has x, y, rot only: a Transform trigger that scales or skews an object (and rebuilds its rotation from the matrix) shows only here. Plain member reads
// (GameObject::m_scaleX / m_scaleY, CCNode::getRotationX / getRotationY): nothing is called that can draw a random number or move the run. A copy is kept as wscale_last.csv like wtrace_last.csv.
inline bool g_wsOn = false;
inline RowFile g_ws;
// cfg `gtscaleoff=1` (with grouptrace=1; off by default): scaleoff.csv, the objects' Area Scale offsets (GameObject::m_scaleXOffset / m_scaleYOffset) -- rows (tick, uid, offx, offy) written when either differs from the object's last row, and the first reading of every
// object at the first tick of an attempt. A saw's collision radius is m_objectRadius * max(m_scaleX + offset, ...) and an Area Scale trigger (3008) moves the offset every tick; grouptrace.txt's columns are not extended (their bytes are hashed
// into the loop's signatures), so the offsets get a file of their own. A copy is kept as scaleoff_last.csv.
inline bool g_soOn = false;
inline RowFile g_so;
// The flights whose recording nothing reads (a check flight that is expected to clear, the showing of a
// solution) set this at their reset (hooks_playlayer.cpp): tick() writes nothing, the roll publishes rows=0, and
// grouptrace_last.txt stays what the last flight that did record left. See level_slice.hpp flightUnrecorded.
inline bool g_hold = false;
inline long long g_rows = 0;
inline long long g_lastTick = -1;   // last tick a row was written at (= depth of the recording)

// What roll() wrote to grouptrace_last.txt. rows==0 means "nothing written", and
// in that case _last keeps its previous content (the reader should skip harvesting).
struct Roll {
    long long rows = 0;
    long long depth = -1;
};

// What the last roll() committed. The external driver learns this from the `gt_last:` line in
// result.txt; the in-process loop reads it directly, and needs the depth to decide whether this
// run's recording is deeper than the one it already has.
//
// IT IS A PUBLISHED RESULT, so it has to be published on every path -- including "there was
// nothing to record". It was not: roll() returned early on `g_objs.empty()` without touching
// this, so a level with no grouped objects never overwrote the answer left by the level before
// it, and harvestGroups() -- which reads THIS, not the returned Roll -- adopted the previous
// level's grouptrace_last.txt as this one's `--groups`.
//
// The two halves, from the cold logs of 2026-08-31 (one process each, so neither run was
// itself damaged):
//   lv19  gt_last: attempt=2 rows=158608 depth=24495   <- what g_lastRoll still holds at the end
//   lv18  gt_last: attempt=2 rows=0 depth=-1           <- every attempt; it never writes here
// and what that costs, measured on the CLI by handing lv18's own first-solve arguments lv19's
// recording as one extra `--groups` (leveldp, 2026-08-31):
//   without it   SOLVED at x=29,151, 468-edge plan over the whole level
//   with it      `groups: 158608 samples for 847 objects`, then PARTIAL: frontier died at
//                t=4872 x=6,549 -- a fifth of the way in, on a level that clears cold in eight
//                rounds. The samples are keyed by uid, so lv19's movers land on whichever lv18
//                objects share their number.
//
// The launcher already had to work around this from the outside (py/gdtas/worker.py deletes
// grouptrace_last.txt between runs, with the same finding). That covers a fresh process per
// level and nothing else, which is why solving two levels in ONE game was the case left broken.
inline Roll g_lastRoll;

// Jitter below 0.05px is not written. This is a threshold, not quantisation
// (when writing, the measured value is written as-is, so no error accumulates)
constexpr float kEps = 0.05f;   // (the default of cfg gteps, config.hpp)
// A move this small is not written: under the threshold, or -- with gteps 0 -- not a move at all.
inline bool smallMove(float d) { const float eps = g_cfg.gtEps; return eps > 0.0f ? d < eps : d == 0.0f; }

inline void reset() {
    g_objs.clear();
    g_nonQuiet = 0; g_areaMovers = 0; g_rotGameplay = 0;   // (per level, like g_objs)
    g_quietScan = true; g_quietPremise = false; g_quietBase = false; g_activeIdx.clear(); g_premiseBreaks = 0;
    areaenv::clear();   // keyed by GameObject*, which die with the level
    g_owner = nullptr;
    g_rows = 0;
    g_lastTick = -1;
    // ...and the published verdict, which is this session's answer and no other's. See the note
    // at g_lastRoll: left standing it is a recording of the PREVIOUS LEVEL offered to this one.
    g_lastRoll = Roll{};
    if (g_out.is_open()) g_out.close();
    if (g_wt.is_open()) g_wt.close();
    if (g_ws.is_open()) g_ws.close();
    if (g_so.is_open()) g_so.close();
}

// Whether g_objs already belongs to this layer, i.e. restart() is enough and a full build() would
// just redo the same walk. False for a fresh/different layer, INCLUDING when g_objs is still
// non-empty from a layer that is gone -- that is exactly the case a rebuild must not skip.
inline bool owns(GJBaseGameLayer* l) { return l && g_owner == l; }

// At level entry. Track only objects that belong to a group (for the rest the
// static dump is correct).
inline void build(GJBaseGameLayer* l) {
    reset();
    areaenv::resetTallies();
    if (!g_on || !l || !l->m_objects) return;
    g_owner = l;
    std::set<int> exitGroups;   // cfg grouptracksmart: the target groups of the Teleport triggers (3022) and orbs (3027) and the 2902 portals
    if (g_smart) {
        for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
            if (!obj) continue;
            const int oid = (int)obj->m_objectID;
            if (oid != 3022 && oid != 3027 && oid != 2902) continue;
            auto* e = static_cast<EffectGameObject*>(obj);
            if (e->m_targetGroupID > 0) exitGroups.insert(e->m_targetGroupID);
        }
    }
    std::ofstream smartOut;
    if (g_smart) { smartOut.open(std::string(DATA_DIR) + "/smart_tracked.txt", std::ios::trunc); smartOut << "uid\trule\n"; }
    for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
        if (!obj) continue;
        { const int oid = (int)obj->m_objectID; if ((oid >= 3006 && oid <= 3008) || (oid >= 3011 && oid <= 3013)) ++g_areaMovers; else if (oid == 2900) ++g_rotGameplay; }   // cfg bootstrapgate; g_quietScan
        const int t7 = (int)obj->m_objectType;
        // [2026-09-20] TYPES 22 AND 31 ARE THE COINS, not triggers -- secret and
        // user -- and they were skipped here under the heading "triggers never
        // collide". They do collide, with the one body that matters for a coin
        // route, and they MOVE: lv21's third coin sits at a node position of
        // (21813,191) while its collision rect is at y=71 until ten pickups
        // bring it up. Nothing in the model could see that, because the
        // recording this filter writes is where the model reads a moving
        // object's timeline from. A level holds at most three, so tracking them
        // costs nothing measurable.
        if ((t7 == 22 || t7 == 31) && !g_cfg.coinMode && !g_cfg.coinRoute)
            continue;
        int smartRule = 0;   // 1 area-moved gameplay object without a group, 2 a Teleport exit decoration, 3 a Touch Triggered trigger object
        if (g_smart && !g_all) {
            if (g_smart >= 2 && t7 != 7 && t7 != 20 && obj->m_groupCount <= 0) smartRule = 1;   // (m_hasAreaParent was tried as the test and is 0 on every object of the two levels that have such movers)
            else if (t7 == 7 && obj->m_groupCount > 0 && obj->m_groups) { for (int gi = 0; gi < obj->m_groupCount && gi < 10; ++gi) if (exitGroups.count((int)(*obj->m_groups)[gi])) { smartRule = 2; break; } }
            else if (t7 == 20 && static_cast<EffectGameObject*>(obj)->m_isTouchTriggered) smartRule = 3;
        }
        if (!g_all && smartRule == 0) {
            if (obj->m_groupCount <= 0) continue;
            // Decorations (7) are not tracked in the normal mode (the model only
            // reads things that collide; tracking them only inflates the row
            // count). CAUTION, measured 2026-08-24: 2.2 gameplay objects live in
            // type 7 too -- lv22's chained platform uid17103 both collides and
            // moves -- so when a rider is carried by a surface the model does not
            // have, re-measure with grouptraceall=1 before trusting this filter.
            if (t7 == 7) continue;
        }
        // [2026-08-16] type 20 is "modifier" and mostly triggers, BUT the speed
        // portals (200/201/202/203/1334) and gameplay rotation (2900) live here,
        // and the model reads both. Excluding the whole type hid a toggle:
        // lv22 group 309 is the ship + speed-portal pair at (15,765,645); GD
        // turns it OFF at t=9,896, yet only the ship side (type 5) was in the
        // recording. The model kept firing the speed portal and ran at 1.3x from
        // then on (at t=12,402 dx splits 1.298 vs 1.950).
        // The model's toggles are designed to take the `on` column from the
        // recording (the definition only gives the phase), so a toggle of an
        // object missing from the recording is unknowable in principle.
        if (t7 == 20 && smartRule != 3 && !g_all) {   // (grouptraceall tracks the modifier / trigger objects too: they were filtered here before 2026-10-30, so the survey saw none of them)
            const int oid = (int)obj->m_objectID;
            const bool wanted = oid == 200 || oid == 201 || oid == 202
                                || oid == 203 || oid == 1334 || oid == 2900;
            if (!wanted) continue;
        }
        if (smartRule && smartOut.is_open()) smartOut << obj->m_uniqueID << "\t" << smartRule << "\n";
        Tracked t;
        t.obj = obj;
        if (smartRule) {
            const bool wanted20 = t7 == 20 && ((int)obj->m_objectID == 200 || (int)obj->m_objectID == 201 || (int)obj->m_objectID == 202 || (int)obj->m_objectID == 203 || (int)obj->m_objectID == 1334 || (int)obj->m_objectID == 2900);
            const bool normal = obj->m_groupCount > 0 && t7 != 7 && (t7 != 20 || wanted20);
            t.smartOnly = !normal && !(g_smart >= 3 && smartRule == 1);   // mode 4 as 3, and see smartQuiet
            t.smartQuiet = !normal && g_smart == 4 && smartRule == 1;   // mode 3: the ungrouped gameplay objects (a) are written to grouptrace.txt like any other tracked object, (b) and (c) to wtrace.csv only
        }
        t.uid = obj->m_uniqueID;
        if (!t.smartQuiet && !t.smartOnly) ++g_nonQuiet;   // (a smartOnly object never reaches grouptrace.txt, so it cannot make the bootstrap record worth a run: lv1 / 12 / 15 / 16 / 18 counted 2 / 16 / 2 / 5 / 12 of them, ran the whole level, and discarded "only 0 samples")
        g_objs.push_back(t);
    }
    g_out.open(std::string(DATA_DIR) + "/grouptrace.txt", std::ios::trunc);
    g_out.write(kHeader);
    g_quietPremise = g_areaMovers == 0 && g_rotGameplay == 0;
    g_quietScan = !g_quietPremise || g_wtOn || g_wsOn || g_soOn || g_all || g_cfg.gtQuietAudit;
    for (size_t oi = 0; oi < g_objs.size(); ++oi) if (!g_objs[oi].smartQuiet) g_activeIdx.push_back((uint32_t)oi);
    log::info("grouptrace: tracking {} grouped objects ({} not smartQuiet, {} area movers, {} rotate gameplay; the smartQuiet ones are {})", g_objs.size(), g_nonQuiet, g_areaMovers, g_rotGameplay,
              g_quietScan ? "scanned every tick" : "read once, at the baseline tick");
}

// Commit the current recording to grouptrace_last.txt and empty grouptrace.txt.
// A plain truncate would wipe the previous recording as soon as the next attempt
// starts, and the reader (leveldp --groups) would grab an empty file. The reader
// looks at this *_last.txt.
//
// [The call site is the contract] Call this at the end of the run
// (destroyPlayer / levelComplete), BEFORE the `death:` / `complete:` line that
// reports the result to result.txt. GD calls resetLevel on its own ~1 second
// after death, so if the commit were left to the start of the attempt, whether
// the reader grabs "the attempt that just died" or "the one before" would depend
// on the polling timing.
// `endTick` is the tick the RUN reached, which is not the last tick a row was
// written at: the recording stops writing as soon as nothing moves. The reader
// merges recordings by "the newer one wins for as far as it reaches", and with
// the last row as the reach, an older recording's rows leak into the ticks the
// newer run covered without moving anything. Measured on SubZero 4002:
// the bootstrap's t=22,677 row lifted a platform the live
// recording had dropped at t=20,711 back to where it started, and the model let
// the player fall through a floor GD stands on. Written as a trailer line so an
// older recording (no trailer) still loads.
// cfg wtrace: the attempt that is over is committed to wtrace_last.csv (a copy through a temp file and a rename; wtrace.csv itself stays, as a capture reads it). GD resets the level about a second
// after a death and restart() then truncates wtrace.csv for the new attempt: a worker stopped in that second -- the capture of a run that ends in a death -- was left with the header of an attempt
// that never ran, 0 bytes, and the record of the attempt that died (the one a divergence is looked for in) gone. The reader of a dead attempt takes wtrace_last.csv, like grouptrace_last.txt.
inline void commitWtrace() {
    if (!g_wtOn || !g_wt.is_open()) return;
    g_wt.flush();
    const std::string cur = std::string(DATA_DIR) + "/wtrace.csv";
    const std::string last = std::string(DATA_DIR) + "/wtrace_last.csv";
    const std::string tmp = last + ".tmp";
    {
        std::ifstream src(cur, std::ios::binary);
        std::ofstream dst(tmp, std::ios::binary | std::ios::trunc);
        if (!src || !dst) { std::error_code ec0; std::filesystem::remove(tmp, ec0); return; }
        dst << src.rdbuf();
        dst.flush();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, last, ec);
    if (ec) { std::filesystem::remove(tmp, ec); log::warn("grouptrace: wtrace_last.csv could not be written ({})", ec.message()); }
}
// cfg wscale / gtscaleoff: the attempt that is over is kept as wscale_last.csv / scaleoff_last.csv (the same copy-and-rename as wtrace_last.csv).
inline void commitSide(RowFile& rf, const char* cur, const char* last) {
    if (!rf.is_open()) return;
    rf.flush();
    const std::string c = std::string(DATA_DIR) + "/" + cur, l = std::string(DATA_DIR) + "/" + last, tmp = l + ".tmp";
    {
        std::ifstream src(c, std::ios::binary);
        std::ofstream dst(tmp, std::ios::binary | std::ios::trunc);
        if (!src || !dst) { std::error_code ec0; std::filesystem::remove(tmp, ec0); return; }
        dst << src.rdbuf();
        dst.flush();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, l, ec);
    if (ec) { std::filesystem::remove(tmp, ec); log::warn("grouptrace: {} could not be written ({})", last, ec.message()); }
}
inline Roll roll(long long endTick = -1) {
    Roll r;
    // "Nothing was recorded" is an answer, and it is THIS attempt's answer -- publish it rather
    // than leaving the previous one standing for the reader to mistake for it (see g_lastRoll).
    if (!g_on || g_objs.empty()) { g_lastRoll = r; return r; }
    for (auto& t : g_objs) { t.emitted = false; t.pvalid = false; }
    g_quietBase = false;
    commitWtrace();   // cfg wtrace: the attempt is over; the rows since the last 600-tick flush are written, and the file is kept as wtrace_last.csv
    if (g_wsOn) commitSide(g_ws, "wscale.csv", "wscale_last.csv");
    if (g_soOn) commitSide(g_so, "scaleoff.csv", "scaleoff_last.csv");
    if (g_out.is_open()) {
        g_out.close();
        const std::string cur = std::string(DATA_DIR) + "/grouptrace.txt";
        if (g_rows > 0) {
            // Write to a temp file, then rename. Streaming straight into
            // _last.txt lets the reader (the driver's copy_held_file) grab a
            // half-written file. Measured (lv22 runR39/40, 2026-08-14): the MOD
            // reported `gt_last: rows=1810`, yet the driver grabbed 781 rows,
            // and re-reading 3 times with 0.4 s gaps gave the same. rename is
            // atomic on the same volume, so the reader only ever sees "the
            // previous complete recording" or "this run's complete recording".
            const std::string last =
                std::string(DATA_DIR) + "/grouptrace_last.txt";
            const std::string tmp = last + ".tmp";
            // The trailer goes onto the recording itself and the recording is RENAMED onto _last
            // (replacing it, atomically on one volume): the same bytes the copy below wrote, without
            // copying them -- 9 s of a 300 s run on a custom level whose recordings reach 293 MB.
            // A rename that fails (a reader holding _last open: a dp read, a kit copy, a scan)
            // falls back to the copy and says so once.
            if (endTick >= 0) {
                std::ofstream app(cur, std::ios::binary | std::ios::app);
                app << "end," << endTick << "\n";
            }
            // cfg gtrollcheck (debug): the copy's bytes, hashed before the rename, against _last after
            unsigned long long checkSig = 0;
            if (g_cfg.gtRollCheck) checkSig = fileFnv(cur);
            std::error_code ec;
            std::filesystem::rename(cur, last, ec);
            if (ec) {
                static bool s_said = false;
                if (!s_said) {
                    s_said = true;
                    log::warn("grouptrace: renaming the recording onto grouptrace_last.txt failed ({}) "
                              "- copying it instead from now on when that happens", ec.message());
                }
                // The copy, from the recording as it now stands (trailer included).
                {
                    std::ifstream src(cur, std::ios::binary);
                    std::ofstream dst(tmp, std::ios::binary | std::ios::trunc);
                    dst << src.rdbuf();
                    dst.flush();
                }
                ec.clear();
                std::filesystem::rename(tmp, last, ec);
            }
            if (g_cfg.gtRollCheck && !ec) {
                const unsigned long long got = fileFnv(last);
                log::info("gtrollcheck: {} (copy {:016x} last {:016x})",
                          got == checkSig ? "same bytes" : "MISMATCH", checkSig, got);
            }
            if (ec) {
                // if rename fails, keep the previous recording (do not throw)
                std::filesystem::remove(tmp, ec);
                log::warn("grouptrace: rename failed, keeping the previous "
                          "grouptrace_last.txt");
            } else {
                r.rows = g_rows;
                r.depth = g_lastTick;
            }
        }
        g_out.open(cur, std::ios::trunc);
        g_out.write(kHeader);
    }
    g_rows = 0;
    g_lastTick = -1;
    g_lastRoll = r;
    return r;
}

// Start of every attempt. Across attempts the coordinates go back to the
// entry-time values, so rewrite them each time. If the end-of-run roll() has
// already happened, rows==0 and _last is left alone.
inline void restart() {
    // Do NOT publish here. The end-of-run roll() has already committed _last,
    // and the few ticks until the attempt switches over flow in here, so calling
    // roll() would satisfy `g_rows > 0` and CLOBBER _last with those few ticks.
    // Measured (lv22 runR41, 2026-08-14): result.txt reported `gt_last: rows=2361
    // depth=662`, yet grouptrace_last.txt had 784 rows, maxtick 664 — the
    // recording supposedly committed at 662 had been overwritten by the few rows
    // of 663/664. The driver's .groups.snap.txt takes that in as-is, so the
    // anchor started from a nearly empty world of "one row per uid".
    if (!g_on || g_objs.empty()) return;
    for (auto& t : g_objs) { t.emitted = false; t.pvalid = false; t.wValid = false; t.wsValid = false; t.soValid = false; }
    g_quietBase = false;
    if (g_ws.is_open()) {   // cfg wscale: one wscale.csv per attempt, like wtrace.csv
        g_ws.close();
        g_ws.open(std::string(DATA_DIR) + "/wscale.csv", std::ios::trunc);
        g_ws.write("tick,uid,scaleX,scaleY,rotationX,rotationY\n");
    }
    if (g_so.is_open()) {   // cfg gtscaleoff: one scaleoff.csv per attempt
        g_so.close();
        g_so.open(std::string(DATA_DIR) + "/scaleoff.csv", std::ios::trunc);
        g_so.write("tick,uid,offx,offy\n");
    }
    if (g_out.is_open()) {
        g_out.close();
        g_out.open(std::string(DATA_DIR) + "/grouptrace.txt", std::ios::trunc);
        g_out.write(kHeader);
    }
    if (g_wtOn && g_wt.is_open()) {   // one wtrace.csv per attempt, like grouptrace.txt
        g_wt.close();
        g_wt.open(std::string(DATA_DIR) + "/wtrace.csv", std::ios::trunc);
        g_wt.write(g_wtLast ? "tick,uid,x,y,rot,on,w,h,lx,ly,stamp\n" : "tick,uid,x,y,rot,on,w,h\n");
    }
    g_rows = 0;
    g_lastTick = -1;
}

// The on column of every tracked object at the start of an attempt: after GD's own reset and
// before the first update, the phase of the model's t=0. An object's on/off in the model is that
// value with the switches applied in time order (dp's OnEvent), so it has to be read at exactly
// this point -- the first row the recording writes is AFTER the first update, by which time a
// trigger behind the start (SubZero 4002's Toggle uid 23 at x=-29) may already have fired.
// Deriving it backwards from the recorded transitions is not unique.
// Written as `init,<uid>,<on>` lines after the header; the recording's reader skips any line
// whose tick is not a number, as it skips the `end,` trailer, so a reader that does not know
// them loads the file unchanged. Called right after PlayLayer::resetLevel, not from restart():
// that runs before the reset, when every object still holds the previous attempt's state.
inline void snapshotInit() {
    if (!g_on || g_hold || g_objs.empty() || !g_out.is_open()) return;
    for (const auto& tr : g_objs) {
        if (!tr.obj || tr.smartOnly || tr.smartQuiet) continue;
        const int on = (tr.obj->m_isGroupDisabled || tr.obj->m_isGroupDisabledTemp) ? 0 : 1;
        char b[160];
        size_t n = (size_t)snprintf(b, sizeof(b), "init,%d,%d\n", tr.uid, on);
        n = endRow(b, n);
        g_out.write(b, n);
    }
}

// The scan below reads five cache lines of each tracked object (the vtable and the node fields at the head, the
// toggle bytes at +0x28e, the cached rect and its dirty byte at +0x358, the size at +0x394 -- getObjectRect,
// getObjectRect2) and the objects lie all over the heap: on a heavy level, 9,158 of them a tick, most of the scan's
// time is the wait for those lines. The next objects' lines are asked for a few iterations ahead. A prefetch of an
// address that is not there does nothing, and nothing it fetches changes a value.
inline constexpr size_t kPrefetchAhead = 6;
inline void prefetchObject(const void* o) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    if (!o) return;
    const char* p = static_cast<const char*>(o);
    _mm_prefetch(p, _MM_HINT_T0);
    _mm_prefetch(p + 0x40, _MM_HINT_T0);
    _mm_prefetch(p + 0x28e, _MM_HINT_T0);
    _mm_prefetch(p + 0x358, _MM_HINT_T0);
    _mm_prefetch(p + 0x394, _MM_HINT_T0);
#else
    (void)o;
#endif
}

inline void tick(long long t) {
    if (!g_on || g_hold || g_objs.empty() || !g_out.is_open()) return;
    if (g_stride > 1 && (t % g_stride) != 0) return;
    struct TickTimer {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        unsigned long long c0 = threadCycles();
        ~TickTimer() {
            g_tickNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
            g_tickCycles += threadCycles() - c0;
            ++g_tickCalls;
        }
    } timer;
    // The smartQuiet objects are read on every tick, or (g_quietScan false: nothing on the level can move them) once per attempt, at the baseline.
    const bool scanAll = g_quietScan || !g_quietBase;
    if (!g_quietScan) g_quietBase = true;
    const size_t nObjs = scanAll ? g_objs.size() : g_activeIdx.size();
    for (size_t ki = 0; ki < nObjs; ++ki) {
        const size_t oi = scanAll ? ki : (size_t)g_activeIdx[ki];
        auto& tr = g_objs[oi];
        if (ki + kPrefetchAhead < nObjs) prefetchObject(g_objs[scanAll ? ki + kPrefetchAhead : (size_t)g_activeIdx[ki + kPrefetchAhead]].obj);
        if (!tr.obj) continue;
        auto r = tr.obj->getObjectRect();
        const float cx = r.origin.x + r.size.width * 0.5f;
        const float cy = r.origin.y + r.size.height * 0.5f;
        const float w = r.size.width, h = r.size.height;
        const float rot = tr.obj->getRotation();
        // A toggle does not change the position: the object stops colliding at
        // the same coordinates, so looking at the rect alone it looks like "a
        // wall that does not move". The on column is its effective state
        const int on = (tr.obj->m_isGroupDisabled
                        || tr.obj->m_isGroupDisabledTemp) ? 0 : 1;
        if (g_wtOn) {   // cfg wtrace: any change at all, at full precision
            const float wx = tr.obj->m_positionX, wy = tr.obj->m_positionY;
            float wlx = 0, wly = 0;
            int wstamp = 0;
            if (g_wtLast) {   // raw reads: the offsets are the 2.2091 layout (moveObjects 0x3369ab writes them)
                const char* base = reinterpret_cast<const char*>(tr.obj);
                wlx = *reinterpret_cast<const float*>(base + gdoff::kObjLastPosition);
                wly = *reinterpret_cast<const float*>(base + gdoff::kObjLastPosition + sizeof(float));
                wstamp = *reinterpret_cast<const int*>(base + gdoff::kObjLastFrameStamp);
            }
            if (!tr.wValid || wx != tr.wx || wy != tr.wy || rot != tr.wrot || on != tr.won || w != tr.ww || h != tr.wh
                || (g_wtLast && (wlx != tr.wlx || wly != tr.wly || wstamp != tr.wstamp))) {
                if (!g_wt.is_open()) {
                    g_wt.open(std::string(DATA_DIR) + "/wtrace.csv", std::ios::trunc);
                    g_wt.write(g_wtLast ? "tick,uid,x,y,rot,on,w,h,lx,ly,stamp\n" : "tick,uid,x,y,rot,on,w,h\n");
                }
                char wb[220];
                int wn = snprintf(wb, sizeof(wb), "%lld,%d,%.9g,%.9g,%.9g,%d,%.9g,%.9g", t, tr.uid, (double)wx, (double)wy, (double)rot, on, (double)w, (double)h);
                if (g_wtLast) wn += snprintf(wb + wn, sizeof(wb) - (size_t)wn, ",%.9g,%.9g,%d", (double)wlx, (double)wly, wstamp);
                wb[wn++] = '\n';
                g_wt.write(wb, (size_t)wn);
                tr.wx = wx; tr.wy = wy; tr.wrot = rot; tr.won = on; tr.ww = w; tr.wh = h; tr.wValid = true;
                tr.wlx = wlx; tr.wly = wly; tr.wstamp = wstamp;
            }
        }
        if (g_wsOn) {   // cfg wscale: the node scale and skew, any change at all, at full precision
            const float sx = tr.obj->m_scaleX, sy = tr.obj->m_scaleY, rx = tr.obj->getRotationX(), ry = tr.obj->getRotationY();
            if (!tr.wsValid || sx != tr.wsx || sy != tr.wsy || rx != tr.wrx || ry != tr.wry) {
                if (!g_ws.is_open()) {
                    g_ws.open(std::string(DATA_DIR) + "/wscale.csv", std::ios::trunc);
                    g_ws.write("tick,uid,scaleX,scaleY,rotationX,rotationY\n");
                }
                char sb[200];
                const int sn = snprintf(sb, sizeof(sb), "%lld,%d,%.9g,%.9g,%.9g,%.9g\n", t, tr.uid, (double)sx, (double)sy, (double)rx, (double)ry);
                g_ws.write(sb, (size_t)sn);
                tr.wsx = sx; tr.wsy = sy; tr.wrx = rx; tr.wry = ry; tr.wsValid = true;
            }
        }
        if (g_soOn) {   // cfg gtscaleoff: the Area Scale offsets, any change at all
            const float ox = tr.obj->m_scaleXOffset, oy = tr.obj->m_scaleYOffset;
            if (!tr.soValid || ox != tr.sox || oy != tr.soy) {
                if (!g_so.is_open()) {
                    g_so.open(std::string(DATA_DIR) + "/scaleoff.csv", std::ios::trunc);
                    g_so.write("tick,uid,offx,offy\n");
                }
                char ob[120];
                const int on2 = snprintf(ob, sizeof(ob), "%lld,%d,%.9g,%.9g\n", t, tr.uid, (double)ox, (double)oy);
                g_so.write(ob, (size_t)on2);
                tr.sox = ox; tr.soy = oy; tr.soValid = true;
            }
        }
        if (tr.smartOnly) continue;   // (cfg grouptracksmart) wtrace.csv only
        // Placed by an Area Move with a variance: the rect depends on GD's random numbers, so
        // write the box of every place it can be instead (areaenv.hpp). The rect itself must
        // not reach the file -- it is the one thing that differs between two games.
        float ex = cx, ey = cy, ew = w, eh = h;
        const int env = areaenv::envelope(tr.obj, ex, ey, ew, eh) ? 1 : 0;
        // cfg gtrestrow: an object that has come to rest (the same values as the tick before) where the last row
        // does not have it -- an ease's last stretch moves less than kEps a tick, so it ends up to kEps short of
        // where it settles, and the reader holds the short value (lv22 coin run: a block 0.044 px below where it
        // stopped, and a ship GD kills on its corner flew on two ticks in the model)
        const bool rest = tr.pvalid && ex == tr.pcx && ey == tr.pcy && ew == tr.pw && eh == tr.ph && rot == tr.prot;
        tr.pcx = ex; tr.pcy = ey; tr.pw = ew; tr.ph = eh; tr.prot = rot; tr.pvalid = true;
        const bool restRow = g_cfg.gtRestRow && rest && tr.emitted
            && (std::fabs(ex - tr.cx) >= 0.0005f || std::fabs(ey - tr.cy) >= 0.0005f
                || std::fabs(ew - tr.w) >= 0.0005f || std::fabs(eh - tr.h) >= 0.0005f
                || std::fabs(rot - tr.rot) >= 0.0005f);
        if (tr.smartQuiet && !tr.emitted) { tr.cx = ex; tr.cy = ey; tr.w = ew; tr.h = eh; tr.rot = rot; tr.on = on; tr.env = env; tr.emitted = true; continue; }   // mode 4: the baseline
        if (!restRow && tr.emitted && on == tr.on && env == tr.env && smallMove(std::fabs(ex - tr.cx))
            && smallMove(std::fabs(ey - tr.cy)) && smallMove(std::fabs(ew - tr.w))
            && smallMove(std::fabs(eh - tr.h)) && smallMove(std::fabs(rot - tr.rot)))
            continue;
        if (g_cfg.gtQuietAudit && tr.smartQuiet && g_quietPremise) {   // the premise says this cannot happen (see g_quietScan)
            if (++g_premiseBreaks <= 20) {
                log::warn("grouptrace: smartQuiet object uid {} moved at tick {} on a level with no Area / Rotate Gameplay trigger", tr.uid, (long long)t);
                writeResult("gt_premise: uid=" + std::to_string(tr.uid) + " t=" + std::to_string((long long)t));
            }
        }
        tr.cx = ex; tr.cy = ey; tr.w = ew; tr.h = eh; tr.rot = rot; tr.on = on; tr.env = env;
        tr.emitted = true;
        char b[160];
        size_t n;
        if (env) {
            n = formatRow(b, t, tr.uid, ex, ey, ew, eh, on, rot, true);
            ++areaenv::g_rows;
        } else {
            n = formatRow(b, t, tr.uid, cx, cy, w, h, on, rot, false);
        }
        n = endRow(b, n);
        g_out.write(b, n);
        ++g_rows;
        g_lastTick = t;
    }
    // Always flush: workers are force-killed with Stop-Process, so whatever is
    // left in the buffer is lost (on levels with little motion the file would
    // stay at 0 bytes and invite misreading)
    if ((t % 600) == 0) { g_out.flush(); if (g_wt.is_open()) g_wt.flush(); if (g_ws.is_open()) g_ws.flush(); if (g_so.is_open()) g_so.flush(); }
    // grouptrace_last.txt is not touched here. The contract with the reader is
    // that ONLY finished runs go in; mixing in the attempt in progress would make
    // the harvest depend on polling timing, and runs with the same input would
    // stop reproducing. The commit happens in one place only: roll().
}

// cfg gtdeathrow: the rows tick(t) would write, as text, with the recorder's state left as it is. Taken
// at the top of destroyPlayer, BEFORE GD's own call: the death itself changes objects (an On Death
// trigger switches its group on inside that call; SubZero 4001 has two), and a row taken after it put
// those switches into the dying tick. Written by commitRows only if the call killed.
inline std::string g_deathRows;      // the capture, held from the top of destroyPlayer to the roll
inline long long g_deathRowsT = -1;  // its tick (-1: none held)

inline std::string captureRows(long long t) {
    std::string out;
    if (!g_on || g_hold || g_objs.empty() || !g_out.is_open()) return out;
    for (const auto& tr : g_objs) {
        if (!tr.obj || tr.smartOnly || (tr.smartQuiet && !tr.emitted)) continue;
        auto r = tr.obj->getObjectRect();
        const float cx = r.origin.x + r.size.width * 0.5f;
        const float cy = r.origin.y + r.size.height * 0.5f;
        const float w = r.size.width, h = r.size.height;
        const float rot = tr.obj->getRotation();
        const int on = (tr.obj->m_isGroupDisabled || tr.obj->m_isGroupDisabledTemp) ? 0 : 1;
        float ex = cx, ey = cy, ew = w, eh = h;
        const int env = areaenv::envelope(tr.obj, ex, ey, ew, eh) ? 1 : 0;
        if (tr.emitted && on == tr.on && env == tr.env && smallMove(std::fabs(ex - tr.cx))
            && smallMove(std::fabs(ey - tr.cy)) && smallMove(std::fabs(ew - tr.w))
            && smallMove(std::fabs(eh - tr.h)) && smallMove(std::fabs(rot - tr.rot)))
            continue;
        char b[160];
        size_t n = env ? formatRow(b, t, tr.uid, ex, ey, ew, eh, on, rot, true)
                       : formatRow(b, t, tr.uid, cx, cy, w, h, on, rot, false);
        n = endRow(b, n);
        out.append(b, n);
    }
    return out;
}

inline void commitRows(const std::string& rows, long long t) {
    if (rows.empty() || !g_out.is_open()) return;
    g_out.write(rows);
    for (const char c : rows) if (c == '\n') ++g_rows;
    g_lastTick = t;
}

} // namespace grouptrace
