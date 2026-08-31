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
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace grouptrace {

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
    bool emitted = false;
};

inline std::vector<Tracked> g_objs;
// The layer g_objs was built against. build() is only safe to skip (falling through to
// restart(), which does not re-walk the object list) when the caller is the SAME layer that
// populated it -- otherwise the vector is still non-empty but every GameObject* in it belongs to
// a level that has since been torn down. `g_objs.empty()` used to be the gate at the call site,
// which is true only "once per process", not "once per level": the first level entered after a
// session that had grouptrace on left its Tracked entries behind for the next level to inherit.
// Measured (2026-08-24): solving lv16 then replaying lv20 dereferenced a stale lv16 GameObject*
// in tick(), crashing on getObjectRect() with a freed (once) or zeroed (once) vtable slot.
inline GJBaseGameLayer* g_owner = nullptr;
inline std::ofstream g_out;
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
constexpr float kEps = 0.05f;

inline void reset() {
    g_objs.clear();
    g_owner = nullptr;
    g_rows = 0;
    g_lastTick = -1;
    // ...and the published verdict, which is this session's answer and no other's. See the note
    // at g_lastRoll: left standing it is a recording of the PREVIOUS LEVEL offered to this one.
    g_lastRoll = Roll{};
    if (g_out.is_open()) g_out.close();
}

// Whether g_objs already belongs to this layer, i.e. restart() is enough and a full build() would
// just redo the same walk. False for a fresh/different layer, INCLUDING when g_objs is still
// non-empty from a layer that is gone -- that is exactly the case a rebuild must not skip.
inline bool owns(GJBaseGameLayer* l) { return l && g_owner == l; }

// At level entry. Track only objects that belong to a group (for the rest the
// static dump is correct).
inline void build(GJBaseGameLayer* l) {
    reset();
    if (!g_on || !l || !l->m_objects) return;
    g_owner = l;
    for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
        if (!obj) continue;
        const int t7 = (int)obj->m_objectType;
        // Triggers never collide; even the measuring mode has no use for them.
        if (t7 == 22 || t7 == 31) continue;
        if (!g_all) {
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
        if (t7 == 20) {
            const int oid = (int)obj->m_objectID;
            const bool wanted = oid == 200 || oid == 201 || oid == 202
                                || oid == 203 || oid == 1334 || oid == 2900;
            if (!wanted) continue;
        }
        Tracked t;
        t.obj = obj;
        t.uid = obj->m_uniqueID;
        g_objs.push_back(t);
    }
    g_out.open(std::string(DATA_DIR) + "/grouptrace.txt", std::ios::trunc);
    if (g_out.is_open()) g_out << "tick,uid,cx,cy,w,h,on,rot\n";
    log::info("grouptrace: tracking {} grouped objects", g_objs.size());
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
inline Roll roll() {
    Roll r;
    // "Nothing was recorded" is an answer, and it is THIS attempt's answer -- publish it rather
    // than leaving the previous one standing for the reader to mistake for it (see g_lastRoll).
    if (!g_on || g_objs.empty()) { g_lastRoll = r; return r; }
    for (auto& t : g_objs) t.emitted = false;
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
            {
                std::ifstream src(cur, std::ios::binary);
                std::ofstream dst(tmp, std::ios::binary | std::ios::trunc);
                dst << src.rdbuf();
                dst.flush();
            }
            std::error_code ec;
            std::filesystem::rename(tmp, last, ec);
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
        if (g_out.is_open()) g_out << "tick,uid,cx,cy,w,h,on,rot\n";
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
    for (auto& t : g_objs) t.emitted = false;
    if (g_out.is_open()) {
        g_out.close();
        g_out.open(std::string(DATA_DIR) + "/grouptrace.txt", std::ios::trunc);
        if (g_out.is_open()) g_out << "tick,uid,cx,cy,w,h,on,rot\n";
    }
    g_rows = 0;
    g_lastTick = -1;
}

inline void tick(long long t) {
    if (!g_on || g_objs.empty() || !g_out.is_open()) return;
    if (g_stride > 1 && (t % g_stride) != 0) return;
    for (auto& tr : g_objs) {
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
        if (tr.emitted && on == tr.on && std::fabs(cx - tr.cx) < kEps
            && std::fabs(cy - tr.cy) < kEps && std::fabs(w - tr.w) < kEps
            && std::fabs(h - tr.h) < kEps && std::fabs(rot - tr.rot) < kEps)
            continue;
        tr.cx = cx; tr.cy = cy; tr.w = w; tr.h = h; tr.rot = rot; tr.on = on;
        tr.emitted = true;
        char b[160];
        snprintf(b, sizeof(b), "%lld,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f\n", t, tr.uid,
                 cx, cy, w, h, on, rot);
        g_out << b;
        ++g_rows;
        g_lastTick = t;
    }
    // Always flush: workers are force-killed with Stop-Process, so whatever is
    // left in the buffer is lost (on levels with little motion the file would
    // stay at 0 bytes and invite misreading)
    if ((t % 600) == 0) g_out.flush();
    // grouptrace_last.txt is not touched here. The contract with the reader is
    // that ONLY finished runs go in; mixing in the attempt in progress would make
    // the harvest depend on polling timing, and runs with the same input would
    // stop reproducing. The commit happens in one place only: roll().
}

} // namespace grouptrace
