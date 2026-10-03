#pragma once
// leveldp -- exact-representative layered reachability solver (cube + ship).
//
// First target: lv1 (Stereo Madness): cube -> ship -> cube -> ship, no pads,
// no orbs, no speed changes, nothing grouped/moving.
//
// Architecture: forward layered search over ticks. States are EXACT
// (double y, vy + mode/held/grounded); a per-layer hash keyed by quantized
// (y, vy) only DEDUPES (first representative wins) -- it never snaps state.
// Surviving paths are therefore exactly simulatable input plans; the DP is a
// candidate generator and the final proof is a plain GD replay of the witness
// (project rule).
//
// Sound under-approximations:
//   - ship: any solid contact excludes the branch (no floor/ceiling rides)
//   - cube: solids can only be stood on from above; inner-box side contact
//     kills; ceilings kill
//
// Measured constants (2026-07-30):
//   cube: gravity -0.216/tick, jump vy=11.18 (jump tick: vy set, y frozen),
//         terminal -15, rest at surface top + 15, floor clamp y=105
//   ship: ShipModel::stepVy (bit-exact), box 30x30
//   both: y += 0.225 * vy, dx = 1.29825 (speed 0.9), input latency +2 ticks
//
// usage: leveldp <objrects.csv> [--out plan.txt]
//
// This is the command-line front end as a FUNCTION. The mod compiles the same header and
// calls dp::cliMain in-process (src/mod/dp_bridge.cpp), so there is exactly one solver
// entry point: whatever the CLI does with a level, the mod does with it too, in the same
// order and with the same code. leveldp.exe is now a three-line main() around this.
#include "dp/reset.hpp"
#include "dp/clearance.hpp"
#include "dp/refwatch.hpp"
#include "dp/search_census.hpp"
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>   // _mm_getcsr: see the fpenv line at the top of cliMain
#endif

namespace dp {

// ---- per-tick input levels -> the plan's `input=press,level` edges --------------------------
//
// ONE copy of this conversion, because two callers now depend on it agreeing with itself: the
// plan writer at the end of cliMain, and the common-prefix publisher inside the layer loop
// (dp/progress.hpp). The prefix is only usable if the inputs the caller flies are the ones the
// emitted plan WOULD have carried for those ticks -- an edge placed one tick out is 2 ticks of
// dy at the seam (the measured lv20 case in the writer's own note below), and the caller would
// be testing a plan the search never proposed.
//
// It is a left-to-right scan carrying `prev`, so running it over a PREFIX of `lvl` gives
// exactly the leading edges it gives over the whole of `lvl`: every index it reads
// (`i - 1`, `i - lat0`) is smaller than `i`, so nothing behind the cut is consulted.
struct PlanEdge {
    long long press;   // the tick the button changes state on
    int level;         // ...and to what
};

// `modeAt[i]` is the mode at the END of tick t0+i+1, `prevHeld` the button state the plan
// starts from and `initMode` the anchor's mode (used when the lookback reaches before tick 0).
inline std::vector<PlanEdge> planEdges(const std::vector<uint8_t>& lvl,
                                       const std::vector<uint8_t>& modeAt, long long t0,
                                       int prevHeld, uint8_t initMode, bool oldLatency) {
    std::vector<PlanEdge> out;
    int prev = prevHeld;
    for (size_t i = 0; i < lvl.size(); ++i) {
        if (lvl[i] == prev) continue;
        const long long effect = t0 + (long long)i + 1;
        auto latOf = [](uint8_t m) { return (m == 1 || m == 3) ? 2 : 1; };
        auto modeAtTick = [&](long long j) -> uint8_t {
            if (j < 0) return initMode;
            if ((size_t)j >= modeAt.size()) return modeAt.empty() ? initMode : modeAt.back();
            return modeAt[(size_t)j];
        };
        int lat;
        if (oldLatency) {
            lat = latOf(modeAt[i]);
        } else {
            const int lat0 = latOf(modeAtTick((long long)i - 1));
            lat = latOf(modeAtTick((long long)i - lat0));
        }
        out.push_back(PlanEdge{effect - lat, (int)lvl[i]});
        prev = lvl[i];
    }
    return out;
}

// Seconds from cliMain's entry to the end of each part of the preparation a call pays before it
// searches or replays anything: 0 arguments, 1 group recordings, 2 touch/auto triggers, 3 the
// level, 4 the rest of the tables (to the --replay branch or the first layer), 5 the object
// position table inside 2. -1 = not reached. Written on every call, read by the mod after it (cfg
// dpphaseprof); nothing prints it here.
inline double g_prepMark[6] = {-1, -1, -1, -1, -1, -1};

// ---- parse caches for the in-process loop ----
// The mod calls cliMain hundreds of times a level -- the ladder's rungs, the fixup recorder's
// replays -- and every call parsed the same level text twice more (the object type and position
// tables) and every group recording again: on SubZero 4003 about 0.13-0.2 s of a 0.25-0.3 s call,
// most of a call that then searches nothing. A table is reused only when the bytes it came from are
// the same bytes, so a hit hands back exactly what parsing them would have. The CLI makes one call
// per process and never hits.
struct LevelTextCache {
    std::string text;
    bool haveTypes = false, havePos = false;
    std::unordered_map<int, int> types;
    std::unordered_map<int, std::pair<double, double>> pos;
};
inline LevelTextCache& levelTextFor(const std::string& text) {
    static LevelTextCache c;
    if (c.text.size() != text.size() || c.text != text) {
        c = LevelTextCache{};
        c.text = text;
    }
    return c;
}
// The moving-geometry timeline a call's --groups files make: each loaded, the later ones laid over
// the first (overlayGroupTimeline), and g_groupInit set -- kept as a whole, not per file. A call
// used to take its first layer as a COPY out of a per-file cache so the overlay could write into
// it, and that copy of a 1.5M-sample recording, with the character-at-a-time read before it, was
// ~1 s of every call on a custom level (lv20: ~0.6 s). The whole result depends on the files'
// bytes and the two hold settings and nothing else, so a hit on all of them hands back the
// timeline itself, and what the loads and overlays printed is printed again.
//
// A miss re-reads only the files whose bytes changed. The loop replaces the live recording
// (dp_groups.txt) whenever a replay goes deeper, which is nearly every round, while the bootstrap
// and the deep recording under it stay as they are for the whole level -- and re-parsing those was
// most of such a call: 2.4 s of 4.2 s on MOAI with an 81 MB bootstrap, 4.2 s of 5.9 s with a deep
// recording as large under it (measured 2026-09-30). Each file's parse is kept as it came off the
// file, so the first layer is a copy of it, as it was before the merged cache existed.
struct GroupFileParse {
    std::string path, bytes;
    GroupTimeline one;
    long long end = -1;
    std::unordered_map<int, uint8_t> init;
    std::string line;     // the line its load printed
};
struct GroupLayersCache {
    bool valid = false;
    std::vector<GroupFileParse> files;   // the last call's files, in its order
    bool holdDeath = false;
    int holdEnd = 0;
    GroupTimeline merged;
    std::unordered_map<int, uint8_t> init;
    std::string report;   // the lines the loads and overlays printed, in order
    long long endT = -1;  // the largest `end,<tick>` among the files (g_groupsEndT)
};
// Bumped every time groupLayersFor builds the timeline instead of handing back the one it has,
// so a table made from the timeline can tell that it is still the same one.
inline unsigned long long g_groupLayersGen = 0;
inline const GroupTimeline& groupLayersFor(const std::vector<std::string>& paths) {
    static GroupLayersCache c;
    static const GroupTimeline kNone;
    g_groupInit.clear();
    g_groupsEndT = -1;
    if (paths.empty()) return kNone;
    std::vector<std::string> bytes(paths.size());
    bool readable = true;
    for (size_t i = 0; i < paths.size() && readable; ++i)
        readable = readFileBytes(paths[i], bytes[i]);
    auto sameFiles = [&]() {
        if (c.files.size() != paths.size()) return false;
        for (size_t i = 0; i < paths.size(); ++i)
            if (c.files[i].path != paths[i] || c.files[i].bytes != bytes[i]) return false;
        return true;
    };
    if (readable && c.valid && c.holdDeath == g_groupHoldDeath && c.holdEnd == g_groupHoldEnd
        && sameFiles()) {
        std::fputs(c.report.c_str(), stdout);
        g_groupInit = c.init;
        g_groupsEndT = c.endT;
        return c.merged;
    }
    // A miss: the loop this replaces, printing as it goes -- each file's line, then its overlay's.
    ++g_groupLayersGen;
    std::vector<GroupFileParse> files(paths.size());
    GroupTimeline gt;
    std::string report;
    for (size_t gi = 0; gi < paths.size(); ++gi) {
        GroupFileParse& f = files[gi];
        GroupFileParse* kept = nullptr;
        if (readable)
            for (GroupFileParse& old : c.files)
                if (old.path == paths[gi] && old.bytes == bytes[gi]) { kept = &old; break; }
        if (kept) {   // the same bytes as last time: the same parse, and the line it printed
            f = std::move(*kept);
            kept->path.clear();   // taken; a path given twice parses its second copy anew
            std::fputs(f.line.c_str(), stdout);
        } else {
            f.path = paths[gi];
            if (readable) {
                f.bytes = std::move(bytes[gi]);
                f.one = parseGroupTimeline(f.bytes, paths[gi], &f.end, &f.init);
            } else {  // an unreadable file says so on stderr and loads as nothing
                f.one = loadGroupTimeline(paths[gi], &f.end, &f.init);
            }
            long long rows = 0;
            for (const auto& kv : f.one) rows += (long long)kv.second.size();
            char b[768];
            std::snprintf(b, sizeof b, "groups: %lld samples for %zu objects (%s)\n", rows,
                          f.one.size(), paths[gi].c_str());
            f.line = b;
        }
        report += f.line;
        if (g_groupInit.empty()) g_groupInit = f.init;
        if (gi == 0) {
            gt = f.one;
        } else {
            std::string ov;
            overlayGroupTimeline(gt, f.one, g_groupHoldDeath && gi + 1 == files.size(), f.end,
                                 &ov);
            std::fputs(ov.c_str(), stdout);
            report += ov;
        }
    }
    long long endT = -1;
    for (const GroupFileParse& f : files) endT = std::max(endT, f.end);
    g_groupsEndT = endT;
    c = GroupLayersCache{};
    c.merged = std::move(gt);
    c.endT = endT;
    if (readable) {   // an unreadable file is not a state worth remembering
        c.valid = true;
        c.files = std::move(files);
        c.holdDeath = g_groupHoldDeath;
        c.holdEnd = g_groupHoldEnd;
        c.init = g_groupInit;
        c.report = std::move(report);
    }
    return c.merged;
}

// The level, shared by the attempts of one --capladder. The ladder (cliMain) runs cliMainOnce
// again for every attempt with the same arguments but for --capmap and --gridmap, which only the
// layer loop reads (capAtX, gridAtX), so every attempt rebuilt the same level. In the panel on
// a custom level (2026-09-25, 32 minutes) that was 257 attempts for 74 searches, each preparing
// for ~1.3 s, the level ~1 s of it. An attempt after the first takes the level the first one
// built: the Level as loadLevel returned it, the globals loadLevelFrom writes (LoadLevelWrites)
// and what it printed.
//
// Only inside one ladder, where the arguments, the input files and the level text are the same
// by construction, and resetInvocationState has put every other global back where the first
// attempt found it. The argument list and the moving-geometry timeline are still compared, as a
// guard rather than as the key. The ordinary CLI empties it when the ladder ends. An in-process
// job can keep it for identical calls (including fixup passes), releasing it at the job's end.
struct LadderLevelCache {
    unsigned long long ladder = 0;   // the ladder it was built in; 0 = empty
    uint64_t job = 0;   // an in-process job may also reuse identical preparation
    std::vector<std::string> args;   // that attempt's arguments without --capmap / --gridmap
    unsigned long long groupsGen = 0;
    Level level;
    LoadLevelWrites writes;
    std::string printed;
};
inline unsigned long long g_ladder = 0;   // the ladder in progress (cliMain); 0 = none
// The gamble (PlainElsewhere::ready): the deepest tick an attempt of the ladder in progress has
// died on, for the plain search beside it to compare itself with, and the flag that stops the
// attempt in progress once that search has been taken. Both belong to one ladder: cliMain sets
// them at its start and clears them on the way out. The CLI never sets the flag.
inline std::atomic<long long> g_ladderBestT{-1};
inline std::atomic<bool> g_ladderStop{false};
inline LadderLevelCache g_ladderLevel;
inline std::vector<std::string> ladderArgs(int argc, char** argv) {
    std::vector<std::string> a;
    for (int i = 0; i < argc; ++i) {
        if (i + 1 < argc
            && (!std::strcmp(argv[i], "--capmap") || !std::strcmp(argv[i], "--gridmap"))) {
            ++i;
            continue;
        }
        a.push_back(argv[i]);
    }
    return a;
}

// One solve: everything a command line asks for. cliMain below is this, unless --capladder
// asks for a sequence of them.
inline int cliMainOnce(int argc, char** argv) {
    // --phaseprof's `prep=`: from here to the first layer -- reading the level and every input
    // file and building the tables -- which a call pays whether or not it then searches at all.
    const auto cliT0 = std::chrono::steady_clock::now();
    for (double& m : g_prepMark) m = -1;
    auto prepMark = [&](int k) {
        g_prepMark[k] = std::chrono::duration<double>(std::chrono::steady_clock::now() - cliT0)
                            .count();
    };
    // Whatever the last call concluded must not be readable as this one's answer. Cleared here
    // rather than at the search, so an early return (bad arguments, unreadable level) also
    // leaves "FAILED, nothing measured" behind instead of the previous run's verdict
    g_outcome.reset();
    // ...and neither must anything else the last call left behind. This is a process's worth of
    // state, and only the mod ever runs two solves in one process -- see dp/reset.hpp for what
    // that was costing and how it was measured.
    resetInvocationState();
    // THE FLOATING-POINT ENVIRONMENT IS NOT IN THE OBJECT FILE. The mod's dp
    // core and this CLI are built from these same headers, and on lv22 they
    // disagree -- the run's own walk dies at 1813, the CLI's at 1837, from an
    // argv that has now been checked token for token and a level whose bytes
    // dpselftest says are the same file. The candidates on the table (the
    // mod-only defines, the PCH force-include) are both compile-time stories.
    // This one is not: SSE rounding lives in MXCSR, a per-thread register, and
    // the mod runs inside a process that cocos2d, fmod, the audio backend and
    // the graphics driver have all initialised. Any of them may leave
    // flush-to-zero / denormals-are-zero set, and every arithmetic result in the
    // search would then round differently with byte-identical code. A bisect of
    // defines and PCH cannot see it, so it has to be READ rather than inferred.
    //
    // Printed unconditionally: quick_regress compares the emitted plans and
    // sends stdout to DEVNULL (py/quick_regress.py:717), so this changes no
    // acceptance. `--mxcsr <hex>` then sets it, which is what turns the reading
    // into an experiment -- without the flag nothing here alters a single bit.
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    std::printf("fpenv: mxcsr=0x%04x\n", (unsigned)_mm_getcsr());
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--mxcsr")) {
            _mm_setcsr((unsigned)std::strtoul(argv[i + 1], nullptr, 0));
            std::printf("fpenv: mxcsr set to 0x%04x\n", (unsigned)_mm_getcsr());
        }
#elif defined(__aarch64__)
    // AArch64 keeps flush-to-zero and the rounding mode in FPCR. Read only: --mxcsr is x86's.
    {
        unsigned long long fpcr = 0;
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        std::printf("fpenv: fpcr=0x%08llx\n", fpcr);
    }
#endif
    // --eval-padgate: evaluate orientedHit() -- the SHIPPED pad predicate the
    // step.hpp pad loop calls -- on cases read from stdin, and exit. No level,
    // no search.
    //
    // It exists because py/gdtas/padgate.py is a TRANSCRIPTION of that function,
    // so py/test_leaf_padgate.py's Q1 ("does the predicate return GD's answer?")
    // is green whatever the C++ says: editing object.hpp to disagree with the
    // copy leaves the test passing. `check_transcription` only notices that the
    // copied text moved, which is a staleness alarm, not a proof. Feeding the
    // same cases through here compares the copy against the real thing.
    //
    // The object's rc/rs are taken RAW rather than derived from a rotation,
    // because deriving them here would be a second transcription of the loader
    // and would put the thing under test on both sides of the comparison.
    //
    // `--eval-padgate <cases.txt>`: one case per line, 13 comma- or
    // space-separated numbers
    //   cx,cy,hw,hh,ohw,ohh,rc,rs,oriented,px,py,half,prot
    // stdout: one line per case, `hit=0` or `hit=1`, in order. A line that does
    // not parse gets `hit=?` rather than being skipped, so the caller can never
    // silently line up N inputs against fewer outputs. Blank lines are ignored.
    //
    // A FILE rather than stdin: agent harnesses commonly run with stdin on the
    // null device, and a stdin interface then reads as "the mode is broken"
    // while actually being untestable. A path is testable everywhere.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--eval-padgate")) continue;
        if (i + 1 >= argc) {
            std::printf("--eval-padgate needs a case file\n");
            return 2;
        }
        std::FILE* f = std::fopen(argv[i + 1], "r");
        if (!f) {
            std::printf("--eval-padgate: cannot read %s\n", argv[i + 1]);
            return 2;
        }
        char buf[512];
        while (std::fgets(buf, sizeof buf, f)) {
            std::string line(buf);
            bool blank = true;
            for (char& ch : line) {
                if (ch == ',') ch = ' ';
                else if (!std::isspace((unsigned char)ch)) blank = false;
            }
            if (blank) continue;
            std::istringstream is(line);
            double v[13];
            int n = 0;
            while (n < 13 && (is >> v[n])) ++n;
            if (n < 13) { std::printf("hit=?\n"); continue; }
            Obj o{};
            o.cx = v[0];  o.cy = v[1];  o.hw = v[2];  o.hh = v[3];
            o.ohw = v[4]; o.ohh = v[5]; o.rc = v[6];  o.rs = v[7];
            o.oriented = (v[8] != 0.0);
            std::printf("hit=%d\n",
                        orientedHit(o, v[9], v[10], v[11], v[12]) ? 1 : 0);
        }
        std::fclose(f);
        return 0;
    }
    if (argc < 2) {
        std::printf("usage: leveldp <objrects.csv> [--out plan.txt]\n");
        return 2;
    }
    // SWITCHES REMOVED IN A RELEASE CLEAN-UP. Each chose between a behaviour that
    // had been on by default and the one it replaced (the --no- form is the off
    // arm, the plain name the on spelling that had become a no-op). Columns: the
    // spelling, the commit that removed it, and the earlier commit that folded its
    // branch into the default. A caller that still passes one is refused by name
    // rather than ignored. A spelling ending in '=' takes its value in the same
    // token and is matched as a prefix.
    {
        static const char* const kRemoved[][3] = {
        {"--no-miniwave", "dbd106d", "5c44e5d"},
        {"--no-ringmode", "dbd106d", "5c44e5d"},
        {"--no-pressspent", "dbd106d", "5c44e5d"},
        {"--no-portalseat", "dbd106d", "5c44e5d"},
        {"--no-forceorder", "dbd106d", "5c44e5d"},
        {"--no-rot2900halve", "dbd106d", "5c44e5d"},
        {"--no-slopeland5", "dbd106d", "5c44e5d"},
        {"--no-portallatch", "dbd106d", "5c44e5d"},
        {"--no-r52gravhold", "dbd106d", "5c44e5d"},
        {"--no-ceilseat", "dbd106d", "5c44e5d"},
        {"--no-slopeveto", "dbd106d", "b43e150"},
        {"--no-slopefreshrect", "dbd106d", "5c44e5d"},
        {"--no-slopenudge", "dbd106d", "5c44e5d"},
        {"--no-mpushreach", "dbd106d", "5c44e5d"},
        {"--no-boostlatch", "dbd106d", "b43e150"},
        {"--no-ringfirsttouch", "dbd106d", "5c44e5d"},
        {"--no-padobb", "dbd106d", "5c44e5d"},
        {"--no-padspinpre", "dbd106d", "5c44e5d"},
        {"--no-dualflip", "dbd106d", "b43e150"},
        {"--nofreeside", "dbd106d", "5c44e5d"},
        {"--no-ufolandtol", "dbd106d", "5c44e5d"},
        {"--no-uforampflap", "dbd106d", "5c44e5d"},
        {"--no-ridelandlaunch", "dbd106d", "5c44e5d"},
        {"--no-ballcorng", "dbd106d", "5c44e5d"},
        {"--no-rampfirst", "dbd106d", "5c44e5d"},
        {"--nohangladder", "dbd106d", "5c44e5d"},
        {"--nocrush", "dbd106d", "5c44e5d"},
        {"--noformula", "dbd106d", "b43e150"},
        {"--ballceilpos", "4c5b7c7", "56a7a83"},
        {"--no-ballceilpos", "4c5b7c7", "56a7a83"},
        {"--ceilcont", "4c5b7c7", "56a7a83"},
        {"--no-ceilcont", "4c5b7c7", "56a7a83"},
        {"--ceillimrelease", "4c5b7c7", "56a7a83"},
        {"--no-ceillimrelease", "4c5b7c7", "56a7a83"},
        {"--ceilpinmin", "4c5b7c7", "56a7a83"},
        {"--no-ceilpinmin", "4c5b7c7", "56a7a83"},
        {"--ceilreleasemode", "4c5b7c7", "56a7a83"},
        {"--no-ceilreleasemode", "4c5b7c7", "56a7a83"},
        {"--ceilveto", "4c5b7c7", "56a7a83"},
        {"--no-ceilveto", "4c5b7c7", "56a7a83"},
        {"--csvtypes", "4c5b7c7", "ac5506d"},
        {"--no-csvtypes", "4c5b7c7", "ac5506d"},
        {"--cubeceilgrace", "4c5b7c7", "56a7a83"},
        {"--no-cubeceilgrace", "4c5b7c7", "56a7a83"},
        {"--dropnocollide", "4c5b7c7", "bf6e11d"},
        {"--dualcouple", "4c5b7c7", "bf6e11d"},
        {"--no-dualcouple", "4c5b7c7", "bf6e11d"},
        {"--no-dyninterp", "4c5b7c7", "bf6e11d"},
        {"--escrotahead", "4c5b7c7", "56a7a83"},
        {"--hangland", "4c5b7c7", "56a7a83"},
        {"--no-hangland", "4c5b7c7", "56a7a83"},
        {"--hazaftersolid", "4c5b7c7", "56a7a83"},
        {"--no-hazaftersolid", "4c5b7c7", "56a7a83"},
        {"--killslopeside", "4c5b7c7", "56a7a83"},
        {"--no-killslopeside", "4c5b7c7", "56a7a83"},
        {"--lockanchor", "4c5b7c7", "ac5506d"},
        {"--no-lockanchor", "4c5b7c7", "ac5506d"},
        {"--movetarget", "4c5b7c7", "ac5506d"},
        {"--no-movetarget", "4c5b7c7", "ac5506d"},
        {"--mpushlaunch", "4c5b7c7", "56a7a83"},
        {"--no-mpushlaunch", "4c5b7c7", "56a7a83"},
        {"--mpushsign", "4c5b7c7", "56a7a83"},
        {"--no-mpushsign", "4c5b7c7", "56a7a83"},
        {"--oriringnow", "4c5b7c7", "56a7a83"},
        {"--no-oriringnow", "4c5b7c7", "56a7a83"},
        {"--oriented", "4c5b7c7", "bf6e11d"},
        {"--no-oriented", "4c5b7c7", "bf6e11d"},
        {"--recinterp", "4c5b7c7", "bf6e11d"},
        {"--no-recinterp", "4c5b7c7", "bf6e11d"},
        {"--releaseceilpress", "4c5b7c7", "56a7a83"},
        {"--no-releaseceilpress", "4c5b7c7", "56a7a83"},
        {"--norevtoggle", "4c5b7c7", "56a7a83"},
        {"--rotpretap", "4c5b7c7", "56a7a83"},
        {"--no-rotpretap", "4c5b7c7", "56a7a83"},
        {"--shipheldflap", "4c5b7c7", "56a7a83"},
        {"--no-shipheldflap", "4c5b7c7", "56a7a83"},
        {"--shipslopekill", "4c5b7c7", "56a7a83"},
        {"--no-shipslopekill", "4c5b7c7", "56a7a83"},
        {"--slopelaw", "4c5b7c7", "56a7a83"},
        {"--no-slopelaw", "4c5b7c7", "56a7a83"},
        {"--spiderstrip", "4c5b7c7", "56a7a83"},
        {"--no-spiderstrip", "4c5b7c7", "56a7a83"},
        {"--stickground", "4c5b7c7", "56a7a83"},
        {"--no-stickground", "4c5b7c7", "56a7a83"},
        {"--stickrelease", "4c5b7c7", "56a7a83"},
        {"--no-stickrelease", "4c5b7c7", "56a7a83"},
        {"--stickseam", "4c5b7c7", "56a7a83"},
        {"--no-stickseam", "4c5b7c7", "56a7a83"},
        {"--touchprey=", "4c5b7c7", "bf6e11d"},
        {"--touchretime", "4c5b7c7", "bf6e11d"},
        {"--no-touchretime", "4c5b7c7", "bf6e11d"},
        {"--touchretimelag", "4c5b7c7", "bf6e11d"},
        {"--no-touchretimelag", "4c5b7c7", "bf6e11d"},
        {"--tpbandskip", "4c5b7c7", "56a7a83"},
        {"--no-tpbandskip", "4c5b7c7", "56a7a83"},
        {"--tpgroundvy", "4c5b7c7", "56a7a83"},
        {"--no-tpgroundvy", "4c5b7c7", "56a7a83"},
        {"--trigclosed", "4c5b7c7", "ac5506d"},
        {"--no-trigclosed", "4c5b7c7", "ac5506d"},
        {"--trigrelevant", "4c5b7c7", "ac5506d"},
        {"--no-trigrelevant", "4c5b7c7", "ac5506d"},
        {"--upceilv3", "4c5b7c7", "56a7a83"},
        {"--no-upceilv3", "4c5b7c7", "56a7a83"},
        {"--waveflipkill", "4c5b7c7", "56a7a83"},
        {"--no-waveflipkill", "4c5b7c7", "56a7a83"},
        {"--waveslopeside", "4c5b7c7", "56a7a83"},
        {"--no-waveslopeside", "4c5b7c7", "56a7a83"},
        {"--wavespentgate", "4c5b7c7", "56a7a83"},
        {"--no-wavespentgate", "4c5b7c7", "56a7a83"},
        {"--witnessframe", "4c5b7c7", "ac5506d"},
        // the 0.2.0 clean-up
        {"--automixlag", "a234c00", "edc8b6b"},
        {"--no-automixlag", "a234c00", "edc8b6b"},
        {"--trigwinworld", "a234c00", "edc8b6b"},
        {"--no-trigwinworld", "a234c00", "edc8b6b"},
        {"--bootretime", "a234c00", "edc8b6b"},
        {"--no-bootretime", "a234c00", "edc8b6b"},
        // the 0.4.0 clean-up
        {"--flyhazaftersolid", "4f75a6f", "d008ee2"},
        {"--no-flyhazaftersolid", "4f75a6f", "d008ee2"},
        {"--latgap", "4f75a6f", "d008ee2"},
        {"--no-latgap", "4f75a6f", "d008ee2"},
        {"--forceunit1x", "4f75a6f", "d008ee2"},
        {"--no-forceunit1x", "4f75a6f", "d008ee2"},
        {"--swingpushtol", "4f75a6f", "d008ee2"},
        {"--no-swingpushtol", "4f75a6f", "d008ee2"},
        {"--trigwinsel", "4f75a6f", "d008ee2"},
        {"--no-trigwinsel", "4f75a6f", "d008ee2"},
        {"--bandcarryvy", "4f75a6f", "d008ee2"},
        {"--no-bandcarryvy", "4f75a6f", "d008ee2"},
        {"--swingpushflight", "4f75a6f", "d008ee2"},
        {"--no-swingpushflight", "4f75a6f", "d008ee2"},
        {"--shrinkpress", "4f75a6f", "d008ee2"},
        {"--no-shrinkpress", "4f75a6f", "d008ee2"},
        {"--repellpolarity", "4f75a6f", "d008ee2"},
        {"--no-repellpolarity", "4f75a6f", "d008ee2"},
        {"--gravportalseat", "4f75a6f", "d008ee2"},
        {"--no-gravportalseat", "4f75a6f", "d008ee2"},
        {"--dualspeedp2", "4f75a6f", "d008ee2"},
        {"--no-dualspeedp2", "4f75a6f", "d008ee2"},
        {"--wavepadhalf", "4f75a6f", "d008ee2"},
        {"--no-wavepadhalf", "4f75a6f", "d008ee2"},
        {"--dualremirror", "4f75a6f", "d008ee2"},
        {"--no-dualremirror", "4f75a6f", "d008ee2"},
        {"--waverotactual", "4f75a6f", "d008ee2"},
        {"--no-waverotactual", "4f75a6f", "d008ee2"},
        {"--wavegrowclamp", "4f75a6f", "d008ee2"},
        {"--no-wavegrowclamp", "4f75a6f", "d008ee2"},
        {"--portalspidertap", "4f75a6f", "d008ee2"},
        {"--no-portalspidertap", "4f75a6f", "d008ee2"},
        {"--spiderairbuffer", "4f75a6f", "d008ee2"},
        {"--no-spiderairbuffer", "4f75a6f", "d008ee2"},
        {"--ceiltaponce", "4f75a6f", "d008ee2"},
        {"--no-ceiltaponce", "4f75a6f", "d008ee2"},
        {"--dualcubeband", "4f75a6f", "d008ee2"},
        {"--no-dualcubeband", "4f75a6f", "d008ee2"},
        {"--tponce", "4f75a6f", "d008ee2"},
        {"--no-tponce", "4f75a6f", "d008ee2"},
        {"--tplatch", "4f75a6f", "d008ee2"},
        {"--no-tplatch", "4f75a6f", "d008ee2"},
        {"--padentry", "4f75a6f", "d008ee2"},
        {"--no-padentry", "4f75a6f", "d008ee2"},
        {"--forceboxdir", "4f75a6f", "d008ee2"},
        {"--no-forceboxdir", "4f75a6f", "d008ee2"},
        {"--escufo", "4f75a6f", "d008ee2"},
        {"--no-escufo", "4f75a6f", "d008ee2"},
        {"--spentdyn", "4f75a6f", "d008ee2"},
        {"--no-spentdyn", "4f75a6f", "d008ee2"},
        {"--radreach", "4f75a6f", "d008ee2"},
        {"--no-radreach", "4f75a6f", "d008ee2"},
        {"--uforot", "4f75a6f", "d008ee2"},
        {"--no-uforot", "4f75a6f", "d008ee2"},
        {"--spdonce", "4f75a6f", "d008ee2"},
        {"--no-spdonce", "4f75a6f", "d008ee2"},
        {"--dualorbflip", "4f75a6f", "d008ee2"},
        {"--no-dualorbflip", "4f75a6f", "d008ee2"},
        {"--orbspawn", "4f75a6f", "d008ee2"},
        {"--no-orbspawn", "4f75a6f", "d008ee2"},
        {"--spawnringjump", "4f75a6f", "d008ee2"},
        {"--no-spawnringjump", "4f75a6f", "d008ee2"},
        {"--ballstake", "4f75a6f", "d008ee2"},
        {"--no-ballstake", "4f75a6f", "d008ee2"},
        {"--boostcarry", "4f75a6f", "d008ee2"},
        {"--no-boostcarry", "4f75a6f", "d008ee2"},
        {"--dualflyring", "4f75a6f", "d008ee2"},
        {"--no-dualflyring", "4f75a6f", "d008ee2"},
        {"--dyngravseen", "4f75a6f", "d008ee2"},
        {"--no-dyngravseen", "4f75a6f", "d008ee2"},
        {"--dropfall", "4f75a6f", "d008ee2"},
        {"--no-dropfall", "4f75a6f", "d008ee2"},
        {"--anchornoop", "4f75a6f", "d008ee2"},
        {"--no-anchornoop", "4f75a6f", "d008ee2"},
        {"--movenospawn", "4f75a6f", "d008ee2"},
        {"--no-movenospawn", "4f75a6f", "d008ee2"},
        {"--repelland", "4f75a6f", "d008ee2"},
        {"--no-repelland", "4f75a6f", "d008ee2"},
        {"--repela0c", "4f75a6f", "d008ee2"},
        {"--no-repela0c", "4f75a6f", "d008ee2"},
        {"--spentaction", "4f75a6f", "d008ee2"},
        {"--no-spentaction", "4f75a6f", "d008ee2"},
        {"--rot2", "4f75a6f", "d008ee2"},
        {"--no-rot2", "4f75a6f", "d008ee2"},
        {"--rotport", "4f75a6f", "d008ee2"},
        {"--no-rotport", "4f75a6f", "d008ee2"},
        {"--a1clatch", "4f75a6f", "d008ee2"},
        {"--no-a1clatch", "4f75a6f", "d008ee2"},
        {"--ridebox", "4f75a6f", "d008ee2"},
        {"--no-ridebox", "4f75a6f", "d008ee2"},
        {"--spiderminigd", "4f75a6f", "d008ee2"},
        {"--no-spiderminigd", "4f75a6f", "d008ee2"},
        {"--tapclosebox", "4f75a6f", "d008ee2"},
        {"--no-tapclosebox", "4f75a6f", "d008ee2"},
        {"--cubeentryspin", "4f75a6f", "9b56145"},
        {"--no-cubeentryspin", "4f75a6f", "9b56145"},
        {"--ringrotsign", "4f75a6f", "36b534d"},
        {"--no-ringrotsign", "4f75a6f", "36b534d"},
        {"--dualdash", "4f75a6f", "36b534d"},
        {"--no-dualdash", "4f75a6f", "36b534d"},
        {"--ceilcubejump", "4f75a6f", "ae8ea0c"},
        {"--no-ceilcubejump", "4f75a6f", "ae8ea0c"},
        {"--ceilcontv4", "4f75a6f", "ae8ea0c"},
        {"--no-ceilcontv4", "4f75a6f", "ae8ea0c"},
        {"--shipceiltol4", "4f75a6f", "ae8ea0c"},
        {"--no-shipceiltol4", "4f75a6f", "ae8ea0c"},
        {"--teleoldpos", "4f75a6f", "27ac5dd"},
        {"--no-teleoldpos", "4f75a6f", "27ac5dd"},
        {"--ballungroundramp", "4f75a6f", "84e607a"},
        {"--no-ballungroundramp", "4f75a6f", "84e607a"},
        {"--contseatclamp", "4f75a6f", "b42218a"},
        {"--no-contseatclamp", "4f75a6f", "b42218a"},
        {"--bandfloorunstick", "4f75a6f", "b42218a"},
        {"--no-bandfloorunstick", "4f75a6f", "b42218a"},
        {"--ceilgracerider", "4f75a6f", "18c39b9"},
        {"--no-ceilgracerider", "4f75a6f", "18c39b9"},
        {"--lawuplaunch", "4f75a6f", "3290750"},
        {"--no-lawuplaunch", "4f75a6f", "3290750"},
        {"--dualseatt", "4f75a6f", "3290750"},
        {"--no-dualseatt", "4f75a6f", "3290750"},
        {"--ceilfreshrect", "4f75a6f", "0acc458"},
        {"--no-ceilfreshrect", "4f75a6f", "0acc458"},
        {"--ceilpushreach", "4f75a6f", "7d29ad8"},
        {"--no-ceilpushreach", "4f75a6f", "7d29ad8"},
        {"--stripownacq", "4f75a6f", "44b8d97"},
        {"--no-stripownacq", "4f75a6f", "44b8d97"},
        {"--ladderback", "4f75a6f", "0bae408"},
        {"--no-ladderback", "4f75a6f", "0bae408"},
        {"--ladderenv", "4f75a6f", "0bae408"},
        {"--no-ladderenv", "4f75a6f", "0bae408"},
        {"--upceilfreshend", "4f75a6f", "b5da1ca"},
        {"--no-upceilfreshend", "4f75a6f", "b5da1ca"},
        {"--uforideflap", "4f75a6f", "f20dab1"},
        {"--no-uforideflap", "4f75a6f", "f20dab1"},
        {"--lawunderstack", "4f75a6f", "9ef38b3"},
        {"--no-lawunderstack", "4f75a6f", "9ef38b3"},
        {"--ridenoblockrest", "4f75a6f", "9ef38b3"},
        {"--no-ridenoblockrest", "4f75a6f", "9ef38b3"},
        {"--groundflushflat", "4f75a6f", "ed0535a"},
        {"--no-groundflushflat", "4f75a6f", "ed0535a"},
        {"--lawfreeclamp", "4f75a6f", "96af63d"},
        {"--no-lawfreeclamp", "4f75a6f", "96af63d"},
        {"--lawseatlaunch", "4f75a6f", "96af63d"},
        {"--no-lawseatlaunch", "4f75a6f", "96af63d"},
        {"--flipcubecrush", "4f75a6f", "e20e0b5"},
        {"--no-flipcubecrush", "4f75a6f", "e20e0b5"},
        {"--balltapstep", "4f75a6f", "520fe36"},
        {"--no-balltapstep", "4f75a6f", "520fe36"},
        {"--ballunderv3", "4f75a6f", "7f045e5"},
        {"--no-ballunderv3", "4f75a6f", "7f045e5"},
        {"--jumpafterland", "4f75a6f", "8a1bb9d"},
        {"--no-jumpafterland", "4f75a6f", "8a1bb9d"},
        {"--dualflipclock", "4f75a6f", "e6ed808"},
        {"--no-dualflipclock", "4f75a6f", "e6ed808"},
        {"--spiderbandsup", "4f75a6f", "0889198"},
        {"--no-spiderbandsup", "4f75a6f", "0889198"},
        {"--ceilrideslope", "4f75a6f", "556e131"},
        {"--no-ceilrideslope", "4f75a6f", "556e131"},
        {"--lawseatonslope", "4f75a6f", "556e131"},
        {"--no-lawseatonslope", "4f75a6f", "556e131"},
        {"--flyhazafterslope", "4f75a6f", "556e131"},
        {"--no-flyhazafterslope", "4f75a6f", "556e131"},
        {"--slopevelexact", "4f75a6f", "556e131"},
        {"--no-slopevelexact", "4f75a6f", "556e131"},
        {"--flipceilland", "4f75a6f", "556e131"},
        {"--no-flipceilland", "4f75a6f", "556e131"},
        {"--portalslopeorder", "4f75a6f", "556e131"},
        {"--no-portalslopeorder", "4f75a6f", "556e131"},
        {"--flipceilride", "4f75a6f", "556e131"},
        {"--no-flipceilride", "4f75a6f", "556e131"},
        {"--ceilreleaseball", "4f75a6f", "556e131"},
        {"--no-ceilreleaseball", "4f75a6f", "556e131"},
        {"--seattoldual", "4f75a6f", "556e131"},
        {"--no-seattoldual", "4f75a6f", "556e131"},
        {"--relkeepblock", "4f75a6f", "556e131"},
        {"--no-relkeepblock", "4f75a6f", "556e131"},
        {"--lawfrombelow", "4f75a6f", "556e131"},
        {"--no-lawfrombelow", "4f75a6f", "556e131"},
        {"--rampjumptick", "4f75a6f", "556e131"},
        {"--no-rampjumptick", "4f75a6f", "556e131"},
        {"--ufobanddir", "4f75a6f", "556e131"},
        {"--no-ufobanddir", "4f75a6f", "556e131"},
        {"--slopetopveto", "4f75a6f", "556e131"},
        {"--no-slopetopveto", "4f75a6f", "556e131"},
        {"--undernudge", "4f75a6f", "556e131"},
        {"--no-undernudge", "4f75a6f", "556e131"},
        {"--seatkeepfirst", "4f75a6f", "556e131"},
        {"--no-seatkeepfirst", "4f75a6f", "556e131"},
        {"--upceilband", "4f75a6f", "556e131"},
        {"--no-upceilband", "4f75a6f", "556e131"},
        {"--lawunderrelease", "4f75a6f", "556e131"},
        {"--no-lawunderrelease", "4f75a6f", "556e131"},
        {"--speedafterseat", "4f75a6f", "556e131"},
        {"--no-speedafterseat", "4f75a6f", "556e131"},
        {"--rideseatrepeat", "4f75a6f", "556e131"},
        {"--no-rideseatrepeat", "4f75a6f", "556e131"},
        {"--slopeinset", "4f75a6f", "556e131"},
        {"--no-slopeinset", "4f75a6f", "556e131"},
        {"--exitbeatsvy", "4f75a6f", "556e131"},
        {"--no-exitbeatsvy", "4f75a6f", "556e131"},
        {"--undersidev3", "4f75a6f", "556e131"},
        {"--no-undersidev3", "4f75a6f", "556e131"},
        {"--upceilrepeat", "4f75a6f", "556e131"},
        {"--no-upceilrepeat", "4f75a6f", "556e131"},
        {"--ballceilveto", "4f75a6f", "556e131"},
        {"--no-ballceilveto", "4f75a6f", "556e131"},
        {"--padclearsride", "4f75a6f", "556e131"},
        {"--no-padclearsride", "4f75a6f", "556e131"},
        {"--ceillimv4", "4f75a6f", "556e131"},
        {"--no-ceillimv4", "4f75a6f", "556e131"},
        {"--waveslopelowend", "4f75a6f", "556e131"},
        {"--no-waveslopelowend", "4f75a6f", "556e131"},
        {"--shipslopecap", "4f75a6f", "556e131"},
        {"--no-shipslopecap", "4f75a6f", "556e131"},
        {"--waveslopecap", "4f75a6f", "556e131"},
        {"--no-waveslopecap", "4f75a6f", "556e131"},
        {"--hangrunoff", "4f75a6f", "556e131"},
        {"--no-hangrunoff", "4f75a6f", "556e131"},
        {"--hangseatrepeat", "4f75a6f", "556e131"},
        {"--no-hangseatrepeat", "4f75a6f", "556e131"},
        {"--sloperot", "4f75a6f", "556e131"},
        {"--no-sloperot", "4f75a6f", "556e131"},
        {"--portalpressorder", "4f75a6f", "556e131"},
        {"--no-portalpressorder", "4f75a6f", "556e131"},
        {"--undercrush", "4f75a6f", "556e131"},
        {"--no-undercrush", "4f75a6f", "556e131"},
        {"--ufoslopekill", "4f75a6f", "556e131"},
        {"--no-ufoslopekill", "4f75a6f", "556e131"},
        {"--balltapexit", "4f75a6f", "556e131"},
        {"--no-balltapexit", "4f75a6f", "556e131"},
        {"--extseatrepeat", "4f75a6f", "556e131"},
        {"--no-extseatrepeat", "4f75a6f", "556e131"},
        {"--flipceilend", "4f75a6f", "556e131"},
        {"--no-flipceilend", "4f75a6f", "556e131"},
        {"--preslopegate", "4f75a6f", "556e131"},
        {"--no-preslopegate", "4f75a6f", "556e131"},
        {"--preslopesolid", "4f75a6f", "556e131"},
        {"--no-preslopesolid", "4f75a6f", "556e131"},
        {"--thinsupport", "4f75a6f", "556e131"},
        {"--no-thinsupport", "4f75a6f", "556e131"},
        {"--spikebottom", "4f75a6f", "556e131"},
        {"--no-spikebottom", "4f75a6f", "556e131"},
        {"--ufolawflap", "4f75a6f", "556e131"},
        {"--no-ufolawflap", "4f75a6f", "556e131"},
        {"--ceilwallnopush", "4f75a6f", "556e131"},
        {"--no-ceilwallnopush", "4f75a6f", "556e131"},
        {"--heldtolhead", "4f75a6f", "556e131"},
        {"--no-heldtolhead", "4f75a6f", "556e131"},
        {"--heldnodown", "4f75a6f", "556e131"},
        {"--no-heldnodown", "4f75a6f", "556e131"},
        {"--capinsetgate", "4f75a6f", "556e131"},
        {"--no-capinsetgate", "4f75a6f", "556e131"},
        {"--ballslopekill", "4f75a6f", "556e131"},
        {"--no-ballslopekill", "4f75a6f", "556e131"},
        {"--lawcontact", "4f75a6f", "556e131"},
        {"--no-lawcontact", "4f75a6f", "556e131"},
        {"--activators", "4f75a6f", "13ae1aa"},
        {"--no-activators", "4f75a6f", "13ae1aa"},
        {"--coinpick", "4f75a6f", "ae28409"},
        {"--no-coinpick", "4f75a6f", "ae28409"},
        {"--hanglandgate", "4f75a6f", "6cd733b"},
        {"--no-hanglandgate", "4f75a6f", "6cd733b"},
        {"--itemsnoblock", "4f75a6f", "9c116a0"},
        {"--no-itemsnoblock", "4f75a6f", "9c116a0"},
        {"--no-offmoves", "4f75a6f", "12e2d6e"},
        {"--no-slopeseat", "4f75a6f", "6ced25c"},
        {"--spawnroots", "4f75a6f", "65e1dad"},
        {"--no-spawnroots", "4f75a6f", "65e1dad"},
        {"--touchretimebox", "4f75a6f", "111fb0e"},
        {"--no-touchretimebox", "4f75a6f", "111fb0e"},
        {"--twinskip", "4f75a6f", "5c2a526"},
        {"--no-twinskip", "4f75a6f", "5c2a526"},
        {"--walkgates", "4f75a6f", "0597ec8"},
        {"--no-walkgates", "4f75a6f", "0597ec8"},
        {"--rotextend", "4f75a6f", "d008ee2"},
        {"--slideparts", "4f75a6f", "d008ee2"},
        };
        for (int i = 1; i < argc; ++i)
            for (const auto& r : kRemoved) {
                const size_t n = std::strlen(r[0]);
                const bool hit = r[0][n - 1] == '='
                    ? !std::strncmp(argv[i], r[0], n) : !std::strcmp(argv[i], r[0]);
                if (hit) {
                    std::printf("leveldp: %s was removed in %s; the behaviour is fixed "
                                "(since %s)\n", argv[i], r[1], r[2]);
                    std::fprintf(stderr, "leveldp: %s was removed in %s; the behaviour is "
                                 "fixed (since %s)\n", argv[i], r[1], r[2]);
                    return 2;
                }
            }
    }
    std::string outPath = "leveldp_plan.txt";
    // --replay <plan.txt>: DIAGNOSTIC ONLY. Re-simulate a fixed input plan
    // (the driver's own "input=tick,level" format) against the model and
    // report where the model dies, writing the usual .trace.csv for gd_diff.
    // Never feeds the search (cold rule): the frontier machinery is bypassed
    // entirely. Purpose: given a plan KNOWN to clear in GD, decide whether the
    // wall is model fidelity (this dies where GD lives) or search/witness
    // policy (this survives -- the route exists in the model but the search
    // never keeps it).
    std::string replayPath;
    // --start t0,x0,y,vy,mode,grounded,held : re-anchor mid-level from a GD
    // dump state. x0 matters: GD's x is NOT a pure clock -- the stair snap
    // shifts it by up to +/-threshold per stair (see StairParams), so each
    // re-anchor also re-anchors x.
    // The dump now carries m_objectSnappedTo's uid and m_snapDistance (the MOD's
    // `snapuid` / `snapdist` columns), and --start takes them as its 19th/20th
    // fields; see the note there. Anchors from an older dump pass -1 and get the
    // old behaviour (no snapObj, one missed stair).
    long long t0 = 0;
    long long startSnapUid = -1;
    double startSnapDist = 0.0;
    int startFrame = 0;   // --start's 21st field (gframe); gates the pad seeding
    double x0 = -kDx;  // so that x(1) = 0
    // --start's world x, kept before a rotated frame maps x0 into its travel
    // coordinate (see g_trigWinNear).
    double x0World = -1e18;
    int dbgLayers = 0;
    std::string snapLogPath;
    std::vector<std::string> groupsPaths;
    std::string trigPath, grpPath, obbPath, setPath, rotQPath, itemPath;
    // y, vy, mode, held, grounded, flip (everything after that is zeroed)
    State init{(float)kFloorY, 0.f, 0, 0, 1, 0};
    // value-less flags get their own loop: the one below stops at argc-1 (every
    // option there reads argv[i+1]), so a flag passed LAST would never be seen.
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--touch-from-anchor")) g_touchFromAnchor = true;
        // On by default since v0.1.4: the plain names still parse (and change
        // nothing), the --no- forms are the off arm.
        if (!std::strcmp(argv[i], "--spawnremap")) g_spawnRemap = true;
        if (!std::strcmp(argv[i], "--no-spawnremap")) g_spawnRemap = false;
        if (!std::strcmp(argv[i], "--pinminnoswing")) g_pinMinNoSwing = true;
        if (!std::strcmp(argv[i], "--hazaabb")) g_hazAabb = true;
        // --goalspare: keep a body above the level that reaches the goal first (speed.hpp)
        if (!std::strcmp(argv[i], "--goalspare")) g_goalSpare = 1;
        if (!std::strcmp(argv[i], "--no-goalspare")) g_goalSpare = 0;   // on by default
        // --upsidecoyote: an upside-down cube jumps from the og GD leaves set (speed.hpp). On by
        // default since 2026-09-26; the plain name still parses, --no-upsidecoyote is the off arm.
        if (!std::strcmp(argv[i], "--upsidecoyote")) g_upsideCoyote = 1;
        if (!std::strcmp(argv[i], "--no-upsidecoyote")) g_upsideCoyote = 0;
        // --rawvalues: the cube branch's gravity step before the 0.001 grid (speed.hpp). On by
        // default with --padtable; the positive spellings still parse and --no- turns them off.
        if (!std::strcmp(argv[i], "--rawvalues")) g_rawValues = 1;
        if (!std::strcmp(argv[i], "--no-rawvalues")) g_rawValues = 0;
        // --portalunpin: a gravity portal off a block keeps the tick's step in y (speed.hpp)
        if (!std::strcmp(argv[i], "--portalunpin")) g_portalUnpin = 1;   // on by default
        if (!std::strcmp(argv[i], "--no-portalunpin")) g_portalUnpin = 0;
        // --hazendpoint: the cube's and flying body's hazard test at the tick's end only (speed.hpp)
        if (!std::strcmp(argv[i], "--hazendpoint")) g_hazEndpoint = 1;   // on by default
        if (!std::strcmp(argv[i], "--no-hazendpoint")) g_hazEndpoint = 0;
        // --portalpress: a press on a mode portal's tick belongs to the new mode (speed.hpp)
        if (!std::strcmp(argv[i], "--portalpress")) g_portalPress = 1;   // on by default
        if (!std::strcmp(argv[i], "--no-portalpress")) g_portalPress = 0;
        // --padtable: pad launches from GD's per-mode table (speed.hpp)
        if (!std::strcmp(argv[i], "--padtable")) g_padTable = 1;
        if (!std::strcmp(argv[i], "--no-padtable")) g_padTable = 0;
        // --ringonce, --portalonce, --ringorder, --holdlatch, --ringfirst, --padfirst,
        // --portalflap, --ringpresspin, --ballbuffer, --ringbuffer, --groundcubejump,
        // --gravlandtap, --gravpadchain, --onewayceil, --padsection, --solidorder, --ballceil,
        // --ballceilflip, --snaponeway, --flyreland, --padthenland, --faceclass, --portalceil,
        // --flipgrace, --padflipland, --flyrings, --sawexact, --snaptable, --sawexactship,
        // --groundgrow, --ringeither, --growaway, --sizepress, --ballgrowlag:
        // always on since 2026-09-26 (speed.hpp). The spellings are still accepted, doing
        // nothing, so a config written before keeps working.
        if (!std::strcmp(argv[i], "--ringonce") || !std::strcmp(argv[i], "--portalonce")
            || !std::strcmp(argv[i], "--ringorder") || !std::strcmp(argv[i], "--holdlatch")
            || !std::strcmp(argv[i], "--ringfirst") || !std::strcmp(argv[i], "--padfirst")
            || !std::strcmp(argv[i], "--portalflap") || !std::strcmp(argv[i], "--ringpresspin")
            || !std::strcmp(argv[i], "--ballbuffer") || !std::strcmp(argv[i], "--ringbuffer")
            || !std::strcmp(argv[i], "--groundcubejump")
            || !std::strcmp(argv[i], "--gravlandtap") || !std::strcmp(argv[i], "--gravpadchain")
            || !std::strcmp(argv[i], "--onewayceil") || !std::strcmp(argv[i], "--padsection")
            || !std::strcmp(argv[i], "--solidorder") || !std::strcmp(argv[i], "--ballceil")
            || !std::strcmp(argv[i], "--ballceilflip") || !std::strcmp(argv[i], "--snaponeway")
            || !std::strcmp(argv[i], "--flyreland") || !std::strcmp(argv[i], "--padthenland")
            || !std::strcmp(argv[i], "--faceclass") || !std::strcmp(argv[i], "--portalceil")
            || !std::strcmp(argv[i], "--flipgrace") || !std::strcmp(argv[i], "--padflipland")
            || !std::strcmp(argv[i], "--flyrings") || !std::strcmp(argv[i], "--sawexact")
            || !std::strcmp(argv[i], "--snaptable") || !std::strcmp(argv[i], "--sawexactship")
            || !std::strcmp(argv[i], "--groundgrow") || !std::strcmp(argv[i], "--ringeither")
            || !std::strcmp(argv[i], "--growaway") || !std::strcmp(argv[i], "--sizepress")
            || !std::strcmp(argv[i], "--ballgrowlag")) continue;
        // --heldall: State::held tracks the button in every mode (speed.hpp)
        if (!std::strcmp(argv[i], "--heldall")) g_heldAll = 1;
        // --heldcellcap: under --heldall the alive cap counts cells, not button twins (speed.hpp)
        if (!std::strcmp(argv[i], "--heldcellcap")) g_heldCellCap = 1;
        // --heldcelltwins N: ...and keeps at most N states of a sampled cell (speed.hpp)
        if (i + 1 < argc && !std::strcmp(argv[i], "--heldcelltwins"))
            g_heldCellTwins = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--fgarmlive")) g_fgArmLive = true;
        if (!std::strcmp(argv[i], "--trigdump")) g_trigDump = true;
        if (!std::strcmp(argv[i], "--stopdump")) g_stopDump = true;   // print only
        if (!std::strcmp(argv[i], "--trigeffect")) g_trigEffect = true;  // print only
        // ...and here as well, because the loop that used to own it stops at argc-1:
        // passed LAST on the command line it was read by nobody, which is how the
        // turned-hazard fixture came out "still dies" for the arm that re-applies the
        // box (py/test_haz_turned_stage.py). Parsing it twice is harmless -- it only
        // ever sets the flag true.
        if (!std::strcmp(argv[i], "--obb-all")) g_obbAll = true;
        if (!std::strcmp(argv[i], "--no-obb-all")) g_obbAll = false;   // the off arm since 2026-09-21
        if (!std::strcmp(argv[i], "--memstat")) g_memStat = true;
        // --phaseprof: wall time per phase of the layer loop, printed once at the end. Print only.
        if (!std::strcmp(argv[i], "--phaseprof")) g_phaseProf = true;
        // --firebcheck: count violations of fireB's invariant (a set bit with no
        // tick, or a tick with no bit) and print the totals at the end.
        if (!std::strcmp(argv[i], "--firebcheck")) g_fireBCheck = true;
        // --groupfire: split the speed groups by the fire tick of every box still moving
        // (search_key.hpp groupFireSig).
        if (!std::strcmp(argv[i], "--groupfire")) g_groupFire = true;
        // --groupxsplit: split a speed group where its parts' object windows cannot meet
        // (search_key.hpp)
        if (!std::strcmp(argv[i], "--groupxsplit")) g_groupXSplit = true;
        // --rotqueue: consume rotations from the queue (frames.hpp) instead of
        // the pre-queue selection. Opt-in until the anchor can seed the state.
        if (!std::strcmp(argv[i], "--rotqueue")) g_rotQueue = true;
        // --startrotq <chan>,<revHex>[,<uid>...]: seed the queue's per-state
        // values at an anchor (history at g_startRotChan's declaration).
        // Applied after the queue is built, because the uids have to be looked
        // up in it.
        if (!std::strcmp(argv[i], "--startrotq")) {
            const char* p = argv[i + 1];
            g_startRotChan = std::atoi(p);
            const char* c = std::strchr(p, ',');
            if (c) {
                g_startRotRev = (unsigned)std::strtoul(c + 1, nullptr, 16);
                p = std::strchr(c + 1, ',');
                while (p) {
                    g_startRotSpent.push_back(std::atoi(p + 1));
                    p = std::strchr(p + 1, ',');
                }
            }
        }
        // --seeddump <t>: the accumulated-field line, for the seeding check.
        if (!std::strcmp(argv[i], "--seeddump")) g_seedDump = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--seedevery")) g_seedEvery = std::atoi(argv[i + 1]);
        // --p2touch: count touch boxes the second player enters (diagnostic).
        if (!std::strcmp(argv[i], "--p2touch")) g_p2Touch = true;
        // --no-spentpad: the A/B arm for --spentpad (frames.hpp). The list is
        // still parsed and still reported, so the two arms differ in the
        // seeding alone and not in what the caller passed. Valueless, so it
        // belongs in THIS loop -- the one below stops at argc-1.
        if (!std::strcmp(argv[i], "--no-spentpad")) g_spentPadSeed = false;
        // --anchor-state / --seed-partial-ok: the anchor payload (frames.hpp).
        if (!std::strcmp(argv[i], "--anchor-state")) g_anchorState = argv[i + 1];
        if (!std::strcmp(argv[i], "--touchseed") && i + 1 < argc) g_touchSeedArg = argv[i + 1];
        // --xtrack <file>: GD's x per tick behind the anchor (g_xTrack).
        if (!std::strcmp(argv[i], "--xtrack") && i + 1 < argc) {
            g_xTrack.clear();
            std::ifstream xf(argv[i + 1]);
            std::string ln;
            int ct = 0, cx = 1;
            bool first = true;
            while (std::getline(xf, ln)) {
                std::vector<std::string> f;
                size_t p = 0;
                while (true) {
                    const size_t q = ln.find(',', p);
                    f.push_back(ln.substr(p, q == std::string::npos ? std::string::npos : q - p));
                    if (q == std::string::npos) break;
                    p = q + 1;
                }
                if (first) {
                    first = false;
                    bool hdr = false;
                    for (size_t k = 0; k < f.size(); ++k) {
                        if (f[k] == "tick") { ct = (int)k; hdr = true; }
                        if (f[k] == "x") { cx = (int)k; hdr = true; }
                    }
                    if (hdr) continue;
                }
                if ((int)f.size() <= std::max(ct, cx)) continue;
                g_xTrack.push_back({std::atoi(f[ct].c_str()), std::atof(f[cx].c_str())});
            }
            std::printf("xtrack: %zu rows from %s\n", g_xTrack.size(), argv[i + 1]);
        }
        // --touchentered uid[:tick],uid[:tick],... ("-" = none): see
        // g_touchEnteredGiven and g_touchEnteredT.
        if (!std::strcmp(argv[i], "--touchentered") && i + 1 < argc) {
            g_touchEnteredGiven = true;
            g_touchEntered.clear();
            g_touchEnteredT.clear();
            const std::string v = argv[i + 1];
            size_t p = 0;
            while (p < v.size()) {
                size_t q = v.find(',', p);
                if (q == std::string::npos) q = v.size();
                const std::string tok = v.substr(p, q - p);
                const int u = std::atoi(tok.c_str());
                if (u > 0) {
                    g_touchEntered.insert(u);
                    const size_t c = tok.find(':');
                    if (c != std::string::npos)
                        g_touchEnteredT[u] = std::atoi(tok.c_str() + c + 1);
                }
                p = q + 1;
            }
        }
        if (!std::strcmp(argv[i], "--seed-partial-ok")) g_seedPartialOk = true;
        // --shiftstat: one line per moving object saying which recorded row the
        // model reads for it (dynamics.hpp). Single-threaded paths only -- the
        // "said it already" flag it keeps is not synchronised, so use it on
        // --replay, not on a threaded search.
        if (!std::strcmp(argv[i], "--shiftstat")) g_shiftStat = true;
        // Instrumentation only (dp/clearance.hpp). Reads the children after they
        // are stepped and keyed; never feeds anything back into the search.
        if (!std::strcmp(argv[i], "--clearprobe")) g_clearProbe = true;
        // --refwatch <trace.csv>: follow that trajectory through the search and
        // name the gate that drops it (refwatch.hpp).
        if (!std::strcmp(argv[i], "--refwatch") && i + 1 < argc) {
            if (!loadRefTrace(argv[i + 1]))
                std::printf("refwatch: cannot read %s\n", argv[i + 1]);
            else {
                g_refWatch = true;
                std::printf("refwatch: %zu reference rows\n", g_refRows.size());
            }
        }
        // --rejoinwatch <trace.csv> / --rejoinafter <tick>: print only (refwatch.hpp).
        if (!std::strcmp(argv[i], "--rejoinwatch") && i + 1 < argc) {
            if (!loadTraceInto(argv[i + 1], g_rjRows))
                std::printf("rejoin: cannot read %s\n", argv[i + 1]);
            else
                g_rjOn = true;
        }
        if (!std::strcmp(argv[i], "--rejoinuse")) g_rjUse = true;
        if (!std::strcmp(argv[i], "--rejoinfull")) g_rjFull = true;
        if (!std::strcmp(argv[i], "--rejoinafter") && i + 1 < argc)
            g_rjAfter = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--refeps") && i + 1 < argc)
            g_refEps = std::atof(argv[i + 1]);
        // --no-padplayerrot: a rotated pad's contact is tested with the player
        // square forced AXIS-ALIGNED (the a874728 behaviour) rather than turned
        // by the player's own rotation.
        if (!std::strcmp(argv[i], "--no-padplayerrot")) g_noPadPlayerRot = true;
        // --no-satrotraw: the four oriented-contact sites advance s.rot by one
        // spin step before the SAT (the pre-2026-09-06 behaviour) instead of
        // passing it as it stands. See g_noSatRotRaw.
        if (!std::strcmp(argv[i], "--no-satrotraw")) g_noSatRotRaw = true;
        if (!std::strcmp(argv[i], "--old-latency")) g_oldLatency = true;
        if (!std::strcmp(argv[i], "--old-slope")) g_oldSlope = true;
        if (!std::strcmp(argv[i], "--rotlast")) g_rotLast = true;
        // Value-less, so it lives in this loop (see the note just below): the
        // argc-1 loop would drop it silently whenever it is passed last.
        if (!std::strcmp(argv[i], "--rotqtoggle")) g_rotQToggle = true;
        if (!std::strcmp(argv[i], "--touchcensus")) g_touchCensus = true;   // value-less, same reason
        if (!std::strcmp(argv[i], "--lagfit")) g_lagFitDbg = true;   // print only
        // --keycensus: count, per touch box, how often it divided the dedupe
        // key. Print-only; the tally goes out at the end of the run.
        if (!std::strcmp(argv[i], "--keycensus")) g_keyCensus = true;
        // --searchcensus: what the frontier is made of (search_census.hpp), every 16th layer.
        // Print only.
        if (!std::strcmp(argv[i], "--searchcensus")) g_searchCensus = 16;
        // --twinaudit (search_census.hpp): value-less.
        if (!std::strcmp(argv[i], "--twinaudit")) g_twinAudit = true;
        if (!std::strcmp(argv[i], "--heldkeyread")) g_heldKeyRead = true;   // value-less
        if (!std::strcmp(argv[i], "--ceilpin")) g_ceilPin = true;
        // --trigraw: autonomous triggers behind the anchor trust the
        // recording's tick (history at g_trigRaw's declaration). These three sat
        // in the argc-1 loop and were never read when passed last
        // (py/test_valueless_flags.py).
        if (!std::strcmp(argv[i], "--trigraw")) g_trigRaw = true;
        // --groupholddeath: the last --groups file ended on a death; hold its
        // last row one tick (groups.hpp). The mod appends it after the live
        // recording, i.e. last. Value-less, so it lives in this loop.
        if (!std::strcmp(argv[i], "--groupholddeath")) g_groupHoldDeath = true;
        if (!std::strcmp(argv[i], "--ceilpush")) g_ceilPush = true;   // value-less, same reason
        if (!std::strcmp(argv[i], "--dualband")) g_dualBand = true;   // value-less too
        if (!std::strcmp(argv[i], "--dualslide")) g_dualSlide = true;   // value-less too
        if (!std::strcmp(argv[i], "--anchorride")) g_anchorRide = true;   // on by default
        if (!std::strcmp(argv[i], "--no-anchorride")) g_anchorRide = false;
        if (i + 1 < argc && !std::strcmp(argv[i], "--flipcldbg"))
            g_flipLandDbgLeft = std::atoi(argv[++i]);   // print only
        // --bonkarm: always on since 2026-09-26 (constants.hpp); accepted, doing nothing
        if (!std::strcmp(argv[i], "--bonkarm")) {}
        if (!std::strcmp(argv[i], "--vetophys")) g_vetoPhys = true;   // value-less, same reason
        if (!std::strcmp(argv[i], "--verdictinfo")) g_verdictInfo = true;   // value-less, same reason
        // Value-less too, and the mod's addWorldArgs can emit it LAST (nothing
        // after it when no boxes are dropped, obb.txt is missing and dpArgs is
        // empty -- the cold-restart JobFirstSolve path), where the loop below
        // never reads it. Parsed here as well so argv order cannot drop it;
        // the second parse at its old site is a harmless re-set.
        if (!std::strcmp(argv[i], "--needtrig-unseen")) g_needUnseen = true;
        if (!std::strcmp(argv[i], "--airpress")) g_airPress = true;   // value-less (g_airPress)
        if (!std::strcmp(argv[i], "--refadopt")) g_refAdopt = true;   // value-less (g_refAdopt)
        if (!std::strcmp(argv[i], "--no-airpress")) g_airPress = false;
        // --replayon (diagnostic): the replay walks past its first death (g_replayOn).
        if (!std::strcmp(argv[i], "--replayon")) g_replayOn = true;
        // [2026-09-01] The three debug switches used to sit in the argc-1 loop
        // below, i.e. in the loop this very comment says value-less flags must
        // not be in: passed LAST they did nothing, silently. Measured the hard
        // way -- `--slopedbg` alone printed nothing at all, and adding a second
        // flag after it made 216 lines appear. A diagnostic that is silently
        // off is worse than one that is missing, because the empty output reads
        // as an answer ("the model never even looks at this object").
        if (!std::strcmp(argv[i], "--banddbg")) g_bandDbg = true;
        if (!std::strcmp(argv[i], "--slopedbg")) g_slopeDbg = true;
            if (!std::strcmp(argv[i], "--slopereldbg")) g_slopeRelDbg = true;
            if (!std::strcmp(argv[i], "--vywriter") && i + 1 < argc) g_vyWatchT = std::atoll(argv[++i]);
            if (!std::strcmp(argv[i], "--vywriter2") && i + 1 < argc) g_vyWatchT2 = std::atoll(argv[++i]);
            if (!std::strcmp(argv[i], "--fxwatch") && i + 1 < argc) g_fxWatchT = std::atoll(argv[++i]);
            if (!std::strcmp(argv[i], "--rotwatch") && i + 1 < argc
                && std::sscanf(argv[++i], "%lld,%lld", &g_rotWatchLo, &g_rotWatchHi) != 2)
                g_rotWatchLo = g_rotWatchHi = -1;
            if (!std::strcmp(argv[i], "--qfoldwatch") && i + 1 < argc
                && std::sscanf(argv[++i], "%lld,%lld", &g_qfoldLo, &g_qfoldHi) != 2)
                g_qfoldLo = g_qfoldHi = -1;
        if (!std::strcmp(argv[i], "--dcydbg")) g_dcyDbg = true;
        if (!std::strcmp(argv[i], "--supdbg")) g_supDbg = true;
        // --spddbg: one line per speed-portal candidate per tick, plus the
        // window size, plus which of the three gates rejected it.
        if (!std::strcmp(argv[i], "--spddbg")) g_spdDbg = true;
        // --rotcheck: replay the computed orbit against every recorded rotated
        // object at load and print the residual. Diagnostic only -- it does not
        // place anything (see RotSpec).
        if (!std::strcmp(argv[i], "--rotcheck")) g_rotCheck = true;
        // --no-rotcompute: back to the recording (or to standing still) for
        // every turned object, for A/B against the computed orbit.
        if (!std::strcmp(argv[i], "--no-rotcompute")) g_rotCompute = false;
        // --no-rotsplit: keep the recording for turned objects instead of the
        // computed orbit (stage 1' A/B).
        if (!std::strcmp(argv[i], "--no-rotsplit")) g_rotSplit = false;
        // --stripvetoorder: a ramp's strip is vetoed only by the ramps GD's slope map holds (off by
        // default: on the model-only check set it breaks more than it fixes; g_stripVetoOrder says why).
        // --no-stripvetoorder still parses, for the A/B arms recorded while it was on.
        if (!std::strcmp(argv[i], "--stripvetoorder")) g_stripVetoOrder = true;
        if (!std::strcmp(argv[i], "--no-stripvetoorder")) g_stripVetoOrder = false;
        // --ladderx0: --ladderback's window from x 0 before the plain search (g_ladderX0).
        if (!std::strcmp(argv[i], "--ladderx0")) g_ladderX0 = true;
        // --capenvelope: a thinned cap class keeps its lowest and highest state (g_capEnvelope).
        if (!std::strcmp(argv[i], "--capenvelope")) g_capEnvelope = true;
        // --coins: collect every coin in the level as well as reaching the end.
        // Value-less, so it belongs HERE -- put in the loop below it was read
        // only when something else followed it on the command line, and the
        // first run passed it last and silently solved the level as usual.
        if (!std::strcmp(argv[i], "--coins")) g_coinRoute = true;
    }
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--threads")) g_threads = std::atoi(argv[i + 1]);
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--dodgemin"))
            g_portalDodgeMin = std::atof(argv[i + 1]);
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--speeddodge"))
            g_speedDodgeMin = std::atof(argv[i + 1]);
    // --glitchportal <px> / --glitchwave <px>: the glitch-avoid mode (thread_pool.hpp)
    for (int i = 2; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--glitchportal")) g_glitchPortal = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--glitchwave")) g_glitchWave = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--glitchembed")) g_glitchEmbed = std::atof(argv[i + 1]);
    }
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "--glitchdeco")) g_glitchDeco = true;
    // --coinwin <yBin>,<vyBin>,<len> (thread_pool.hpp). Anything but three positive numbers is off.
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--coinwin")
            && (std::sscanf(argv[i + 1], "%lf,%lf,%lf", &g_coinWinY, &g_coinWinVy, &g_coinWinLen) != 3
                || !(g_coinWinY > 0.0 && g_coinWinVy > 0.0 && g_coinWinLen > 0.0)))
            g_coinWinY = g_coinWinVy = g_coinWinLen = 0.0;
    if (g_coinWinY > 0.0)
        std::printf("coinwin: cap cells y/%.1f vy/%.2f over %.0f px before each coin still lacked\n",
                    g_coinWinY, g_coinWinVy, g_coinWinLen);
    for (int i = 2; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--out")) outPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--dbg")) dbgLayers = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--cap")) g_aliveCap = (size_t)std::atoll(argv[i + 1]);
        // --capmap x0:c0,x1:c1,... (thread_pool.hpp). Entries must be in x order.
        if (!std::strcmp(argv[i], "--capmap")) {
            g_capMap.clear();
            for (const char* p = argv[i + 1]; *p;) {
                char* e = nullptr;
                const double x = std::strtod(p, &e);
                if (e == p || *e != ':') break;
                p = e + 1;
                const long long c = std::strtoll(p, &e, 10);
                if (e == p || c <= 0) break;
                g_capMap.emplace_back(x, (size_t)c);
                p = (*e == ',') ? e + 1 : e;
            }
        }
        // --gridmap x0:k0,x1:k1,... (search_census.hpp gridAtX). Entries must be in x order;
        // k = 0 means "--inputgrid's value", 1 means no grid.
        if (!std::strcmp(argv[i], "--gridmap")) {
            g_gridMap.clear();
            for (const char* p = argv[i + 1]; *p;) {
                char* e = nullptr;
                const double x = std::strtod(p, &e);
                if (e == p || *e != ':') break;
                p = e + 1;
                const long k = std::strtol(p, &e, 10);
                if (e == p || k < 0) break;
                g_gridMap.emplace_back(x, (int)k);
                p = (*e == ',') ? e + 1 : e;
            }
        }
        // --coinmask <bits>: with --coins, the coins already collected at the
        // --start anchor (bit i = L.coins[i], x order). See the seed below.
        if (!std::strcmp(argv[i], "--coinmask")) g_coinMaskSeed = std::atoi(argv[i + 1]);
        // --coinskip <bits>: coins to leave out of the goal (same bit order).
        // Kept apart from --coinmask because the loop passes that one itself,
        // at every anchor, and the last occurrence would win.
        if (!std::strcmp(argv[i], "--coinskip")) g_coinSkip = std::atoi(argv[i + 1]);
        // --hazdbg <uid>: see g_hazDbgUid. Takes a value, so it is parsed with
        // the same argv[i+1] form as --cap and not with the valueless shape at
        // the top of this file, which is silently off when it is given one.
        if (!std::strcmp(argv[i], "--hazdbg")) g_hazDbgUid = std::atoi(argv[i + 1]);
        // --trigdbg <tick>: one line per group at that tick, naming the mask the
        // group applied. Reads --hazdbg for WHICH object to locate, so the two
        // flags are given together.
        if (!std::strcmp(argv[i], "--trigdbg")) g_trigDbgT = std::atoll(argv[i + 1]);
        // --fbforce <box>:<tick>: see g_fbForceBox (triggers.hpp).
        if (!std::strcmp(argv[i], "--fbforce")) {
            const char* v = argv[i + 1];
            g_fbForceBox = std::atoi(v);
            const char* c = std::strchr(v, ':');
            g_fbForceTick = c ? std::atoi(c + 1) : 0;
        }
        // Where the model put each coin at that tick, against the mod's own
        // `coinlive:` line for the same tick (see the declaration).
        if (!std::strcmp(argv[i], "--coindbg")) g_coinDbgT = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--gcnodes")) g_gcNodes = (size_t)std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--memlimit")) g_memLimitMiB = (size_t)std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--shipyq")) g_shipYq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--keyxq")) g_keyXq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--keyxqfly")) g_keyXqFly = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--minpulse")) g_minPulse = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--inputgrid")) g_inputGrid = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--shipvq")) g_shipVq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--snaplog")) snapLogPath = argv[i + 1];
        // [2026-08-31] --shipceil / --ceil / --flyfloor / --ufoceil ARE GONE.
        // Not the machinery -- g_shipCeil, g_playerCeils / playerCeilAt,
        // g_flyFloor and g_ufoCeil are all still read where they always were --
        // only the way to SET them from the command line. An audit found that
        // nothing passes any of the four: not addWorldArgs (the full list the
        // in-game loop hands the solver is level dumps plus what the run itself
        // produced), not cold_regress.py (one identical cfg for all 22 levels),
        // not fidelity_diff, not the lab. They are per-LEVEL knobs in a solver
        // whose whole claim is that every level gets the same one, and leaving a
        // way to set them meant the claim held only because nobody happened to.
        // Their comments still describe values "measured per level" (lv9 = 330,
        // lv14 = 420@13100:13600) as though they were in force; they are not,
        // and both levels solve without them. See the notes at the globals.
        // Dropping the flags makes that structural instead of accidental, and
        // leaves the readers as plain dead code for whoever deletes them.
        // --groups <file>: the MOD's grouptrace dump. Without it every level
        // from lv19 on is planned against geometry that is in the wrong place.
        if (!std::strcmp(argv[i], "--groups")) groupsPaths.push_back(argv[i + 1]);
        // --groupholdend <n>: the bootstrap fills in only n ticks after a live
        // recording ends (see g_groupHoldEnd in groups.hpp). Default 0.
        if (!std::strcmp(argv[i], "--groupholdend")) g_groupHoldEnd = std::atoi(argv[i + 1]);
        // --triggers / --objgroups: the MOD's trigger -> group -> uid map. Both
        // or neither; without them a door that only opens on contact reads as a
        // solid wall (see TouchTrig).
        // --supporttol X: gap tolerance for staying supported (default 0.01,
        // and the surface's own |dcy| is added on top -- see kSupportTol's
        // note)
        if (!std::strcmp(argv[i], "--supporttol"))
            kSupportTol = std::atof(argv[i + 1]);
        // --landtol X: penetration a landing allows (default 10.5, see
        // kLandTol's note)
        if (!std::strcmp(argv[i], "--landtol"))
            kLandTol = std::atof(argv[i + 1]);
        // --stickgap X: gap tolerance for continuing a ride (default 4.03, see
        // kStickGap's note)
        if (!std::strcmp(argv[i], "--stickgap"))
            kStickGap = std::atof(argv[i + 1]);
        // --rotperp X: how far off a 2900 the player may be on the perpendicular
        // axis and still fire it (default 150, see kRotPerpWin's note)
        if (!std::strcmp(argv[i], "--rotperp"))
            kRotPerpWin = std::atof(argv[i + 1]);
        // --stepdepth X: step depth that can be grabbed mid-ride (default
        // 3.15, see kStepDepth's note)
        if (!std::strcmp(argv[i], "--stepdepth"))
            kStepDepth = std::atof(argv[i + 1]);
        // --ctrlwin t0:t1,t0:t1,...: controls-disabled windows (both ends
        // inclusive). The caller builds them from the ctrlOff column of GD's
        // dump (history at g_ctrlWin's declaration).
        if (!std::strcmp(argv[i], "--ctrlwin")) {
            for (const char* p = argv[i + 1]; *p; ) {
                long long a0 = std::atoll(p);
                const char* col = std::strchr(p, ':');
                long long a1 = col ? std::atoll(col + 1) : a0;
                g_ctrlWin.push_back({a0, a1});
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        // --spentrot uid,uid,...: 2900s already fired before the anchor
        // (history at g_spentRot's declaration). Applied after loadLevel (once
        // g_rotTrig has been filled).
        if (!std::strcmp(argv[i], "--spentrot")) {
            for (const char* p = argv[i + 1]; *p; ) {
                g_spentRot.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        // --spentpad uid,uid,...: pads already fired before the anchor
        // (history at g_spentPad's declaration). Applied at the anchor's pad
        // seeding, after loadLevel.
        if (!std::strcmp(argv[i], "--spentpad")) {
            for (const char* p = argv[i + 1]; *p; ) {
                g_spentPad.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        // --solidorddbg t0:t1: print the solids the collision loops visit, in order (speed.hpp)
        if (!std::strcmp(argv[i], "--solidorddbg")) {
            g_solidOrdDbgT0 = std::atoll(argv[i + 1]);
            const char* c = std::strchr(argv[i + 1], ':');
            g_solidOrdDbgT1 = c ? std::atoll(c + 1) : g_solidOrdDbgT0;
        }
        // --spentorb uid,uid,...: rings already fired before the anchor, oldest first (speed.hpp)
        if (!std::strcmp(argv[i], "--spentorb")) {
            for (const char* p = argv[i + 1]; *p; ) {
                g_spentOrb.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        if (!std::strcmp(argv[i], "--dyndbg")) g_dynDbg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--triggers")) trigPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--objgroups")) grpPath = argv[i + 1];
        // --items <path>: the MOD's items.txt, the pickups a Count trigger
        // counts. Read only under --coins (nothing else looks at a counter).
        if (!std::strcmp(argv[i], "--items")) itemPath = argv[i + 1];
        // --itembase "<item>:<n>,...": the counters GD already holds at the
        // anchor (see g_itemBase).
        // --coinmargin <px>: how far inside a coin the plan has to be (see
        // kCoinMargin). 0 restores "the boundary counts", which is GD's own
        // rule and the fragile one to plan against.
        if (!std::strcmp(argv[i], "--coinmargin")) kCoinMargin = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--itembase")) {
            g_itemBase.clear();
            std::stringstream ss(argv[i + 1]);
            std::string ent;
            while (std::getline(ss, ent, ',')) {
                int it = 0, n = 0;
                if (std::sscanf(ent.c_str(), "%d:%d", &it, &n) == 2 && it != 0)
                    g_itemBase.push_back({it, n});
            }
        }
        // --rotgameplay <path>: the MOD's dump of the 2899/2900 queue (channel,
        // ord, the switch fields). Absent on 21 of 22 levels, which have no
        // such object at all.
        if (!std::strcmp(argv[i], "--rotgameplay")) rotQPath = argv[i + 1];
        // --levelsettings <file>: the level's LevelSettingsObject flags. Only
        // lv22 has any of them set among the 22 official levels, and only
        // fixRadiusCollision has a reader (hazardHit). Without the flag the
        // loader looks for the file beside the objrects dump, which covers
        // every caller that passes a real path; the in-process caller passes
        // the level in memory, so it has to name the file.
        if (!std::strcmp(argv[i], "--levelsettings")) setPath = argv[i + 1];
        // --obb <file>: GD's own corners for the turned objects (see loadObb).
        // Optional -- without it a turned hazard keeps the bound it has always
        // had, so a level with no obb dump cannot move.
        if (!std::strcmp(argv[i], "--obb")) obbPath = argv[i + 1];
        // --forceids <file>: force block IDs (see g_forceIdsPath)
        if (!std::strcmp(argv[i], "--forceids")) g_forceIdsPath = argv[i + 1];
        // --bandtrack <file>: the recorded band (history at g_bandTrack's
        // declaration)
        if (!std::strcmp(argv[i], "--bandtrack")) {
            std::FILE* bf = std::fopen(argv[i + 1], "r");
            if (bf) {
                // NO ROW IS DROPPED. The old reader kept only `ce > fl` and
                // said nothing about the rest, so a collapsed band reached the
                // lookup as "no row" and the lookup answered with the last
                // interval it had -- an old, wrong, non-degenerate band. Each
                // row now arrives with its kind and is counted; what a reader
                // does with a kind is the reader's decision, not the parser's.
                //
                // The version is the column count: three fields is the original
                // `t,floor,ceil`, four is `t,kind,a,b`. No header line, because
                // a reader of the older format would take one for a row.
                size_t kn[4] = {0, 0, 0, 0};
                size_t skipped = 0;
                char line[256];
                while (std::fgets(line, sizeof line, bf)) {
                    BandTrackRow r{};
                    char k = 0;
                    float a = 0, b = 0;
                    if (std::sscanf(line, "%d,%c,%f,%f", &r.t, &k, &a, &b) >= 2
                        && (k == 'I' || k == 'D' || k == 'N' || k == 'U')) {
                        r.fl = a;
                        // A D row carries the collapsed value in both columns,
                        // so this does not depend on the fourth field being
                        // present -- but the writers fill it in anyway, so a
                        // future reader may tighten the field count.
                        r.ce = (k == 'D') ? a : b;
                        r.kind = k == 'I' ? BandKind::Interval
                               : k == 'D' ? BandKind::Degenerate
                               : k == 'N' ? BandKind::NoBand
                                          : BandKind::Unknown;
                        // An interval that is not one is not silently kept:
                        // the invariant is part of the type.
                        if (r.kind == BandKind::Interval && !(r.ce > r.fl))
                            r.kind = BandKind::Unknown;
                    } else if (std::sscanf(line, "%d,%f,%f", &r.t, &r.fl,
                                           &r.ce) == 3) {
                        r.kind = r.ce > r.fl  ? BandKind::Interval
                               : r.ce == r.fl ? BandKind::Degenerate
                                              : BandKind::Unknown;
                        if (r.ce < r.fl)
                            std::fprintf(stderr, "bandtrack: t=%d has ceil %g "
                                         "below floor %g\n", r.t, (double)r.ce,
                                         (double)r.fl);
                    } else {
                        // A line that matches neither shape. It is still not
                        // dropped in silence: the count is printed with the
                        // rest, because "nothing here" and "I could not read
                        // this" are the two the whole change exists to keep
                        // apart.
                        ++skipped;
                        continue;
                    }
                    if (r.kind == BandKind::Interval)
                        g_bandTrackIntervals.push_back(g_bandTrack.size());
                    ++kn[(size_t)r.kind];
                    g_bandTrack.push_back(r);
                }
                std::fclose(bf);
                g_bandTrackCam = -1;   // the verdict belongs to THIS track
                std::printf("bandtrack: %zu rows (I %zu D %zu N %zu U %zu) "
                            "unreadable %zu (%s)\n", g_bandTrack.size(),
                            kn[0], kn[1], kn[2], kn[3], skipped, argv[i + 1]);
            } else {
                std::fprintf(stderr, "bandtrack: cannot open %s\n", argv[i + 1]);
            }
        }
        // --bandtrackend <t>: where the recording behind --bandtrack stops (bands.hpp)
        if (!std::strcmp(argv[i], "--bandtrackend")) g_bandTrackEnd = std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--startband")) {
            // Three different facts used to leave through the same line: a
            // collapsed band, a pair the wrong way round, and a string that is
            // not a pair at all. They are separated here because only the first
            // says anything about the level.
            double f = 0, c = 0;
            if (std::sscanf(argv[i + 1], "%lf,%lf", &f, &c) != 2) {
                g_startBandKind = BandKind::Unknown;
                std::fprintf(stderr, "startband: `%s` is not a readable f,c pair\n",
                             argv[i + 1]);
            } else if (c > f) {
                g_startBandKind = BandKind::Interval;
                g_startBandFloor = f;
                g_startBandCeil = c;
            } else if (c == f) {
                g_startBandKind = BandKind::Degenerate;
                g_startBandFloor = g_startBandCeil = f;
                std::fprintf(stderr, "startband: `%s` is a collapsed band\n",
                             argv[i + 1]);
            } else {
                g_startBandKind = BandKind::Unknown;
                std::fprintf(stderr, "startband: `%s` has the ceiling below the "
                             "floor\n", argv[i + 1]);
            }
        }
        // --needtrig <n>: repeatable. See g_needTrig.
        if (!std::strcmp(argv[i], "--needtrig"))
            g_needTrig |= touchBit(std::atoi(argv[i + 1]));
        // --needtrig-uid <uid>: repeatable. See g_needTrigUids.
        if (!std::strcmp(argv[i], "--needtrig-uid"))
            g_needTrigUids.push_back(std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--needtrig-unseen")) g_needUnseen = true;
        // --needtrig-skip <n>: repeatable. See g_needSkip.
        if (!std::strcmp(argv[i], "--needtrig-skip"))
            g_needSkip |= touchBit(std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--obb-all")) g_obbAll = true;
        if (!std::strcmp(argv[i], "--no-obb-all")) g_obbAll = false;   // the off arm since 2026-09-21
        if (!std::strcmp(argv[i], "--bands")) g_bandPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--replay")) replayPath = argv[i + 1];
        // --fixups <file>: divergence-driven local overrides recorded by the
        // driver DURING THIS RUN (see Fixup above). The driver clears the file
        // at run start -- records never carry across runs, so a cold solve
        // stays cold; the permanent archive lives in fixups_log_lv*.txt which
        // nothing here ever reads.
        if (!std::strcmp(argv[i], "--fixups")) {
            std::ifstream ff(argv[i + 1]);
            std::string ln;
            while (std::getline(ff, ln)) {
                if (ln.size() >= 3 && (unsigned char)ln[0] == 0xEF &&
                    (unsigned char)ln[1] == 0xBB && (unsigned char)ln[2] == 0xBF)
                    ln.erase(0, 3);
                Fixup f{};
                double x, y, vy, dy, dvy;
                int in, mode, mini, flip, g, g2, kill = 0;
                const int n = std::sscanf(
                    ln.c_str(),
                    "x=%lf,in=%d,mode=%d,mini=%d,flip=%d,g=%d,"
                    "y=%lf,vy=%lf,dy=%lf,dvy=%lf,g2=%d,kill=%d",
                    &x, &in, &mode, &mini, &flip, &g, &y, &vy, &dy, &dvy,
                    &g2, &kill);
                if (n < 11) continue;
                f.x = (float)x; f.y = (float)y; f.vy = (float)vy;
                f.dy = (float)dy; f.dvy = (float)dvy;
                f.in = (uint8_t)in; f.mode = (uint8_t)mode;
                f.mini = (uint8_t)mini; f.flip = (uint8_t)flip;
                f.g = (uint8_t)g; f.gAfter = (uint8_t)g2;
                f.kill = (uint8_t)(kill != 0);
                // The second body, if this record was measured on a pair. Read
                // from a named tail rather than more positional fields, so a
                // file written before duals were recordable parses unchanged
                // and lands on dual=0 -- which is what it always meant.
                if (const char* d = std::strstr(ln.c_str(), ",dual2=")) {
                    double y2, vy2, dy2, dvy2;
                    int g2b;
                    if (std::sscanf(d, ",dual2=%lf,%lf,%lf,%lf,%d",
                                    &y2, &vy2, &dy2, &dvy2, &g2b) == 5) {
                        f.dual = 1;
                        f.y2 = (float)y2; f.vy2 = (float)vy2;
                        f.dy2 = (float)dy2; f.dvy2 = (float)dvy2;
                        f.gAfter2 = (uint8_t)g2b;
                    }
                }
                f.ord = (int)g_fixups.size();
                g_fixups.push_back(f);
            }
            for (const Fixup& fx : g_fixups)
                (fx.kill ? g_fixupKills : g_fixupDeltas).push_back(fx);
            auto byX = [](const Fixup& a, const Fixup& b) { return a.x < b.x; };
            std::stable_sort(g_fixupKills.begin(), g_fixupKills.end(), byX);
            std::stable_sort(g_fixupDeltas.begin(), g_fixupDeltas.end(), byX);
            // CONTRADICTORY delta pairs: the NEWER record wins. Two records
            // the matcher cannot tell apart (their key windows overlap) whose
            // outcomes disagree are two worldlines' answers written on one
            // key -- the lv22 lift at x=8,185 holds dvy=+3.426 from plans
            // where it was rising and 0.000 from plans where it sank, and
            // some of these transitions (the grounded ball's tap-flip phase
            // on a mover) are decided inside GD's collision pass, below the
            // model's resolution -- no rule can carry them, only the current
            // run's own measurement. The file is append-only within a run, so
            // the larger ord is the measurement of the LATEST worldline (the
            // one the converging plan actually lives in); the stale side is
            // dropped. Records that agree (adjacent-tick drift stays under
            // 0.5) coexist, and there is no blanket dyn gate on deltas.
            // DISABLED (2026-08-26): neither the drop-both nor the
            // newest-wins form ever produced a corridor breakout, while both
            // no-filter builds did (see the note at stepBoth's dyn gate).
            // Kept compiled behind the flag for the next measurement.
            if (g_fixupConflictFilter) {
                std::vector<char> drop(g_fixupDeltas.size(), 0);
                for (size_t a = 0; a < g_fixupDeltas.size(); ++a)
                    for (size_t b = a + 1;
                         b < g_fixupDeltas.size()
                         && g_fixupDeltas[b].x <= g_fixupDeltas[a].x + 2.4f;
                         ++b) {
                        const Fixup &p = g_fixupDeltas[a], &q = g_fixupDeltas[b];
                        if (p.in != q.in || p.mode != q.mode || p.mini != q.mini
                            || p.flip != q.flip || p.g != q.g || p.dual != q.dual)
                            continue;
                        if (std::fabs(p.y - q.y) > 8.0f
                            || std::fabs(p.vy - q.vy) > 2.0f)
                            continue;
                        if (std::fabs(p.dy - q.dy) > 0.5f
                            || std::fabs(p.dvy - q.dvy) > 0.5f)
                            drop[p.ord < q.ord ? a : b] = 1;
                    }
                size_t w = 0, gone = 0;
                for (size_t a = 0; a < g_fixupDeltas.size(); ++a) {
                    if (drop[a]) { ++gone; continue; }
                    g_fixupDeltas[w++] = g_fixupDeltas[a];
                }
                g_fixupDeltas.resize(w);
                if (gone)
                    std::printf("fixups: %zu contradictory delta records "
                                "dropped\n", gone);
            }
            if (!g_fixups.empty())
                std::printf("fixups: %zu transition overrides loaded\n",
                            g_fixups.size());
        }
        if (!std::strcmp(argv[i], "--dynhazpad")) g_dynHazPad = std::atof(argv[i + 1]);
        // --histstat <file>: the fixed-length histories' fill after a replay (speed.hpp)
        if (!std::strcmp(argv[i], "--histstat")) {
            g_histStatOn = true;
            g_histStatPath = argv[i + 1];
        }
        if (!std::strcmp(argv[i], "--maxplayy")) g_maxPlayY = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--offboard") && i + 1 < argc)
            g_offBoardMargin = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--shiftdbg")) g_shiftDbgUid = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--deadband")) {
            double a0 = 0, a1 = 0, b0 = -1e18, b1 = 1e18; int md = -1;
            const int n = std::sscanf(argv[i + 1], "%lf,%lf,%d,%lf,%lf",
                                      &a0, &a1, &md, &b0, &b1);
            if (n >= 2 && a1 > a0)
                g_deadBands.push_back({a0, a1, n >= 3 ? md : -1,
                                       n >= 4 ? b0 : -1e18,
                                       n >= 5 ? b1 : 1e18});
        }
        if (!std::strcmp(argv[i], "--cubeyq")) g_cubeYq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--cubevq")) g_cubeVq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--start")) {
            // 30 fields (21st=frame, 22nd=rev, 23rd/24th=sprite rotation,
            // 25th=boost, 26th/27th=the second body's mode and size,
            // 28th=ticks since the last gravity flip, 29th=ticks since the
            // last id-1859 ceiling arm, 30th=rotStep). The mod's producer
            // (repair.hpp startArg) writes the first 27 only, so 28-30 reach
            // this parser from the offline instruments and one-off scripts
            // alone. New per-body history does not get a field here: it goes
            // through --anchor-state owns=hist (frames.hpp, kHistNames).
            // FORGET TO GROW THE SIZE AND
            // sscanf WRITES PAST THE ARRAY: when rev was added it was left at
            // 21, and it showed up as rev=0/1 not changing the result by a
            // single bit.
            // -1 in the last three = "the caller did not say", which is not the
            // same as 0 (a real mode / a flip on this very tick) -- see the
            // notes at the dual block and at State::flipT.
            // 31st-33rd (--rot2): the second body's sprite angle, spin sign and spin size;
            // -9999 = "not said" (the pair then starts from p1's, as it always did).
            double a[33] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, -1, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, 0, -9999, 0, 0};
            // 8th field = flip. It used to be absent entirely, so every
            // re-anchor taken while the player was upside down restarted the
            // solve in NORMAL gravity -- the tail was then solved for a world
            // that mirrors the real one. Levels that flip a lot (lv10) can only
            // re-anchor correctly with it. Older callers passing 7 fields still
            // work: a[7] stays 0.
            // 9th field = mini (from the dump's vsize: 1.0 normal, 0.6 mini).
            // Same class of bug as the flip field above -- re-anchoring past a
            // size portal without it restarts the tail at the wrong half extent.
            // 10th..14th = the SECOND player: dual, y2, vy2, flip2, grounded2.
            // Without them a re-anchor taken inside a dual section restarted
            // the solve as a single player, and the tail was then planned for a
            // world with half the bodies in it. lv16's dual runs from x=10,551
            // to x=15,103 and its wall at x=13,982 sits inside it, so EVERY
            // anchor the loop took there was wrong. Same class of bug as the
            // missing flip / mini / ufo fields before it.
            // 15th field = GD's OWN speed multiplier at the anchor (the dump's
            // `speed` column). The model used to re-derive the section's speed
            // by replaying every speed portal whose x is behind the anchor --
            // which is wrong exactly when the player did not TOUCH one, the case
            // the note above advanceX predicted. lv16 is that case: its id 201
            // ("0.9") portal sits at x=17,403 cy=561 (y=533..589) and the mini
            // UFO flies UNDER it at y~515, so GD keeps 1.1 for the rest of the
            // level while the model dropped to 0.9 -- a 0.3145 px/tick x error,
            // 24% slow, from x=17,403 all the way to the end. Every tail solved
            // past that point was timed for a level moving at the wrong speed,
            // which is why they came back SOLVED and then died in GD.
            // GD's measurement is exact and needs no rule, so prefer it.
            // 16th field = the ROBOT's remaining hover budget (rHover).
            // Without it every re-anchor taken mid-hover restarts with the
            // budget at 0, so the model falls at the mode's gravity through a
            // stretch where GD holds vy dead flat -- and the driver, whose fixup
            // comparison IS an anchored resim, records that as a divergence on
            // every single tick.
            // Measured on lv21 (2026-08-10): 45 of the 63 fixups at the x=18,705
            // wall were this, all `m5/mini1` with the same edy=0.0873 /
            // edvy=0.194, which reads exactly like one wrong constant. It is
            // not: anchored BEFORE the jump, so the model arms the budget
            // itself, it holds vy=4.472 and matches GD bit for bit
            // (y = 203.337 / 204.343 / 205.349 against GD's 203.336731 /
            // 204.342926 / 205.349121).
            // Worse than a wasted record: those 45 go back in through --fixups
            // and overwrite correct physics next to the wall.
            // Same family as the documented snapObj gap, and the same list still
            // has holes -- dashing / dashSlope / ringHold / usedOrb are all
            // still dropped by a re-anchor. lv21 holds one dash for 300+
            // ticks, so that one will bite next.
            // `usedPad` came off that list on 2026-09-06: the overlap rule below
            // covers a contact still in progress and --spentpad covers the pads
            // fired earlier in the attempt (g_spentPad's declaration). usedOrb
            // has no such producer -- an orb is consumed by a PRESS, so the
            // recording's x,y cannot say which ones were taken.
            // Callers passing 15 fields still work: a[15] stays 0, which is what
            // every non-robot anchor wants anyway.
            // 17th/18th = DASH state (dashing, dashSlope). Same family as the
            // 16th and the one that bites hardest: a dash is held for hundreds
            // of ticks (lv21 holds one at x=2,445 for 300+, and another at
            // x=18,105 for 69), so a re-anchor taken inside one restarts in free
            // fall through the whole thing.
            // Found the long way (2026-08-10): the hover-budget inference was
            // credited for lv21's x=18,105 stretch and the mini-ship fixups
            // collapsed 14 -> 2, which looked like a win. It was a COINCIDENCE.
            // The object at (18105,255) is a dash ring (type 37), not a hover:
            // GD sets vy := 0 and freezes y, and a hover AT vy=0 draws the same
            // line, so the wrong mechanism produced the right trajectory. It
            // only holds while the ring's rot is 0 (all of lv21's are) and while
            // the dash is shorter than the hover's 67-tick budget -- that
            // stretch is 69, so the last two ticks were already wrong.
            // 19th/20th = the STAIR SNAP state (m_objectSnappedTo's uid and
            // m_snapDistance), the "documented snapObj gap" the note above
            // names. An anchor without it restarts with snapObj = null, so the
            // first stair after the anchor cannot match a pattern (the gate
            // needs a PREVIOUS object) and the nudge is skipped -- the model
            // then runs 1 px behind GD for the whole rest of that simulation.
            // Measured 2026-08-10 with the segment harness (quick_regress's own
            // anchors, 400 ticks apart, lv1-20): 64 segments diverge with
            // dy = dvy = 0 and dx = -1.0000 exactly, all mode 0 on a block
            // surface, and every one of them sits EARLIER than the same level's
            // un-anchored replay first diverges -- i.e. they are artefacts of
            // the anchor, not of the physics. -1 = "no object" (the default).
            // 22nd = reverse (a same-frame firing of id 2900). Without it an
            // anchor taken after the reversal restarts with rev=0 and runs the
            // opposite way to GD.
            // 23rd/24th = THE PLAYER'S SPRITE ROTATION (the dump's `rot`
            // column) and its spin direction (rotNeg; the caller derives it
            // from the sign of rot(t0) - rot(t0-1)). State::rot is the only
            // angle used for "hitbox of a turned object", and an anchor
            // without it re-accumulates from rot=0, so mid-section it drifts
            // tens of degrees from GD. A different angle FLIPS THE VERDICT OF
            // THE TURNED-BOX TEST: measured on lv20 t=7,281 against the -46
            // degree teleport portal uid7021:
            //   GD:    rot=-420.303 -> 14.3 degrees off the portal axis -> no
            //          fire; fires next tick (rot=-422.033, 16.0 degrees off)
            //   model: rot re-accumulated from the anchor, effectively near 45
            //          degrees = the angle where the player's projection is
            //          largest (21.21); fires 1 tick early
            // It showed up as the 192 px warp landing 1 tick off
            // (fixcensus `m0/mini0/g0/gdg0/sp0.9/air/in0`, edy=+192.106).
            // 25th = the velocity-limit exemption (State::boost), read
            // straight off GD's byte [player+0x952]. An anchor taken during a
            // boosted stretch (a fast slope exit, a red ring/pad) without it
            // re-clamps the swing at 8 while GD keeps accelerating.
            // 26th/27th = THE SECOND BODY'S OWN MODE AND SIZE. -1 = the caller
            // did not say, and gets the old behaviour (copy the first body's);
            // see the note where they are applied. 0 is a real mode, so the
            // sentinel cannot be 0 the way the earlier optional fields' is.
            // 28th = TICKS SINCE GRAVITY LAST FLIPPED (State::flipT), which
            // decides whether the side/crush kill is still inside GD's 0.1 s
            // grace. -1 = the caller did not say, read as "no grace"; 0 is a
            // real value (flipped on the anchor tick), so it cannot be the
            // sentinel. An anchor taken in the 24 ticks after a blue pad or a
            // gravity portal without it kills states GD spares.
            // 30th = the SIZE of the spin (State::rotStep), deg/tick. Default 0
            // = "not turning", which is what every caller written before this
            // meant and is correct for a cube and for a grounded ball; it is
            // wrong only for an anchor landing mid-air on a ball, which is the
            // case the field exists for.
            std::sscanf(argv[i + 1],
                        "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,"
                        "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,"
                        "%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                        &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7],
                        &a[8], &a[9], &a[10], &a[11], &a[12], &a[13], &a[14],
                        &a[15], &a[16], &a[17], &a[18], &a[19], &a[20], &a[21],
                        &a[22], &a[23], &a[24], &a[25], &a[26], &a[27], &a[28],
                        &a[29], &a[30], &a[31], &a[32]);
            const uint8_t startRev = (uint8_t)((int)a[21] & 1);
            startFrame = (int)a[20] & 3;
            startSnapUid = (long long)a[18];
            startSnapDist = a[19];
            g_startSpeedMul = a[14];
            t0 = (long long)a[0];
            x0 = a[1];
            x0World = a[1];
            init = State{(float)a[2], (float)a[3], (uint8_t)a[4], (uint8_t)a[6],
                         (uint8_t)a[5], (uint8_t)a[7]};
            init.mini = (uint8_t)a[8];
            // Ticks since the last gravity flip; absent or negative = no grace.
            init.flipT = (a[27] < 0.0)
                             ? (uint8_t)kFlipGraceTicks
                             : (uint8_t)std::min<int>(kFlipGraceTicks,
                                                      (int)a[27]);
            // ...and since the last id-1859 touch; absent or negative = not
            // armed, which is the conservative answer (the ceiling kills).
            init.armT = (a[28] < 0.0)
                            ? (uint8_t)kArmTicks
                            : (uint8_t)std::min<int>(kArmTicks, (int)a[28]);
            // 21st field = GD's gameplay rotation (the dump's gframe, 0..3).
            // Re-anchored INSIDE a rotated section, the model had no way to
            // know which way it was running (rotation triggers already passed
            // count as fired), restarted in frame 0 and fell within 1 tick
            // (the "follow 1 tick" at lv22 t=11,340). GD's x,y are world, so
            // map them into frame coordinates before handing them over. vy's
            // sign mirrors in frame 3 only (same convention as the trace
            // writer).
            // NOTE: `init = State{...}` above REBUILDS init, so the rev
            // assignment is lost unless it comes after it (that is why frame
            // worked while rev did not: the frame assignment was always after
            // the rebuild). Reverse can occur in frame 0 too, so it sits
            // OUTSIDE the frame block below.
            init.rev = startRev;
            // (after the rebuild, same as rev -- see the NOTE above)
            init.boost = ((int)a[24] != 0) ? 1 : 0;
            if (((int)a[20] & 3) != 0) {
                const int sf = (int)a[20] & 3;
                double u, v;
                toFrame(sf, x0, a[2], u, v);
                x0 = u;
                init.y = (float)v;
                // vy too is MIRRORED WHEN f&2, for the same reason as flip
                // (the vertical v uses the world axis reversed for f=2 and
                // f=3). Only frame 3 was being mapped, so a frame-2 anchor
                // TURNED A FALL INTO A RISE: at lv22 t=20,450 GD falls at
                // 8.2px/tick while the model rose at 1.7px/tick, and 87 ticks
                // later it was passing through a spot 42px too high.
                init.vy = (float)(a[3] * ((sf & 2) ? -1.0 : 1.0));
                init.frame = (uint8_t)sf;
                // ...and FLIP TOO IS MIRRORED IN FRAME 3 (same convention as
                // gdUpOf). Forget to map it here and the state says "standing
                // right there" while gravity points the other way, and the
                // re-anchor floats upward from tick 1 (dvy=-0.432 = exactly 2
                // ticks' worth at lv22 t=11,340).
                // The mirror applies when the frame's vertical v uses the
                // world axis REVERSED: toFrame has f=0 v=Y, f=1 v=X, f=2 v=-Y,
                // f=3 v=-X. That is the two with f&2 (180 and 270 degrees).
                // frame 3 is confirmed by measurement
                // ([[gd-lv22-rotated-section]]); frame 2 follows the same
                // logic and was backed up by the t=12,878 anchor failing to
                // reproduce GD's fall (only the model stands on a ledge 130px
                // higher).
                //
                // [correction 2026-08-18] THE FLIP MIRROR IS NOT APPLIED HERE.
                // The caller (py/mkstart.py's measured table -- checked
                // against all 2,069 ticks of lv22) already does the
                // `GD 3/up1 -> model 3/flip0` conversion, and stacking it here
                // double-flips back to the original. The caller rewrites
                // gframe 2 as (frame 0, rev 1), so a 2 never arrives here and
                // the "frame 2 follows the same logic" above is currently
                // reached from nowhere.
                //
                // Measured (--start at lv22 t=17,000, robot / gframe3 / up1):
                //   with the double flip (flip=1): the model's vy goes 3.160
                //     -> 2.965 -> ..., FALLING 0.195/tick, while GD goes
                //     3.550 -> 3.745 -> ..., rising
                //   without it (flip=0): the internal vy runs -3.355 -> -3.550
                //     and the writer's mirror gives +3.550 = matches GD as is
                // This is what fixcensus's `m5/mini0/g0/gdg0/sp1.4/air`
                // (lv22 x3, edy=0.000 / edvy=+0.390 = two gravities' worth)
                // really was.
                // The vy-side mirror STAYS: the trace writer negates in frame
                // 3 only, so entry and exit are paired.
                // [note 2026-08-16] This loop is DEAD CODE: g_rotTrig is
                // filled by loadLevel (much later than this), so it is always
                // empty here. The intent (treat passed triggers as fired) was
                // taken over by --spentrot.
                for (RotTrig& r : g_rotTrig)
                    if (r.frame == sf) r.firedT = (int)t0;
            }
            init.rHover = (uint8_t)a[15];   // see the 16th-field note above
            // ...and `action` as well as `held`, from the same 7th field. They
            // are the same fact ("was the button down coming into this tick")
            // and the anchor only ever carried one of them. Everything that
            // asks whether a hold is CONTINUING reads s.action, not s.held --
            // the robot's hover, the dash, the UFO's rising edge -- so an anchor
            // with action=0 makes the very first tick look like a fresh press
            // after a release. For the robot that is fatal and silent: the hover
            // branch is skipped once, the else-branch clears the budget, and no
            // value of the 16th field can bring it back.
            //
            // Gated on the hover budget, NOT applied to every anchor. Seeding it
            // unconditionally is what the semantics say, but it costs three
            // levels in quick_regress -- lv12, lv18 and lv20 each lose a segment
            // from 400 ticks of follow to 1 (lv19 gains 114), which is the first
            // tick going a different way. The obvious suspect is the UFO's
            // rising edge (`act = input && !s.action`), where a seeded hold
            // turns a flap into nothing; that is a separate bug and a separate
            // measurement. A non-zero 16th field only ever comes from an anchor
            // the driver has already identified as mid-hover, i.e. one where the
            // button is down by construction, so this gate is exact for the case
            // it was measured on and inert everywhere else.
            init.dashing = (uint8_t)a[16];
            init.dashSlope = (float)a[17];
            // ...and a dash continues on `s.action` exactly like the hover, so
            // it needs the same seed for the same reason.
            if (a[15] > 0 || a[16] > 0) init.action = (uint8_t)a[6];
            // 23rd/24th (note above). A caller that omits them leaves 0 = the
            // old behaviour.
            init.rot = (float)a[22];
            init.rotNeg = (uint8_t)(a[23] != 0.0 ? 1 : 0);
            // 30th: the SIZE of the spin (State::rotStep). Rides in the same
            // seeding path as the two fields above rather than getting one of
            // its own, because it is the same quantity measured from the same
            // two reference rows: the caller reads `rot(t0) - rot(t0-1)`, sends
            // the sign as the 24th and the magnitude here. No factor of 240 --
            // see State::rotStep for why the name says "step" and not "rate".
            //
            // BALL ONLY, and seedcheck is why. The caller measures the turn
            // whatever mode it is in, but this field is a ball's stake: nothing
            // stakes it in any other mode, so a whole run leaves it at 0 there.
            // Seeded unconditionally it disagreed on 25 of 105 sampled ticks,
            // every one of them a CUBE holding the cube's own step (t=400:
            // whole 0.000000, seeded 1.730773 = 180/0.43333334/240). Copying an
            // observation into a field that means something narrower is how a
            // seed ends up describing a state the run can never be in.
            //
            // WHAT THE GATE CANNOT FIX, and exactly who it reaches: an anchor
            // taken in a NON-ball mode while a ball's stake is still live. The
            // stake outlives the mode -- nothing clears it -- so a run that was
            // a ball, changed mode, and comes back to ball in mid-air is still
            // spending the old step, and the reference has no column that says
            // so. Seeded from a non-ball row this reads 0 and the anchored run
            // stops turning. That is the whole population: every other anchor
            // either re-derives its spin per tick (cube) or is re-staked on its
            // first tick down (grounded ball). 21 of 105 sampled ticks on lv22,
            // and quick_regress is per-level identical to a0c4c8f, so no
            // section in the corpus is anchored inside that window -- which is
            // a fact about the corpus, not a bound on the defect.
            if ((int)a[4] == 2) init.rotStep = (float)std::fabs(a[29]);
            // --sloperot: the same turn, read for a CUBE, says whether a ramp's launch staked the
            // spin (State::rotBoost): boostPlayer's size is half the take-off's, 0.8653846 (1.125
            // mini) against 1.7307692 (2.25), and nothing else stakes that size. A grounded cube's
            // easing never lands on it within the tolerance by more than chance, and the gate
            // below only reads it off the ground.
            if ((int)a[4] == 0 && (int)a[5] == 0
                && std::fabs(std::fabs(a[29]) - (a[8] != 0.0 ? 1.125 : 0.8653846)) < 2e-3)
                init.rotBoost = 1;
            // State::hitG (--repela0c): GD's +0xa0c is not in the dump; a body on the ground at t0
            // holds it, which is all a row can say (one that walked off an edge still holds it).
            init.hitG = (uint8_t)(a[5] != 0.0 ? 1 : 0);
            init.dual = (uint8_t)a[9];
            if (init.dual) {
                init.y2 = (float)a[10];
                init.vy2 = (float)a[11];
                init.flip2 = (uint8_t)a[12];
                init.grounded2 = (uint8_t)a[13];
                init.hitG2 = init.grounded2 ? 1 : 0;
                // The second body's own mode and size (26th/27th fields).
                //
                // These used to be seeded from the first body unconditionally,
                // because the dump had one `mode` column for the pair and an
                // anchor could not tell the two apart. The justification given
                // for it -- "they differ for at most the tick between one
                // clearing a mode portal's window and the other reaching it"
                // -- was never measurable, precisely because of the thing it
                // was justifying, and it is FALSE. Measured 2026-08-28 on the
                // rig `dualmode` (py/mklevel.py), which separates the halves
                // with floor-height portals: 3,471 CONSECUTIVE ticks with the
                // modes differing (p1 cube / p2 ship, 50% of the run) and 4,676
                // with the sizes differing.
                //
                // It is true of the official corpus -- lv16's whole cold run is
                // 207,761 dual ticks and the halves never differ -- and false
                // wherever the corpus is not, which now includes levels this
                // solver clears. So an anchor that is told takes what it is
                // told; -1 (or an older caller passing 25 fields) keeps the
                // copy, which is what every official anchor resolves to anyway.
                init.mode2 = (a[25] >= 0.0) ? (uint8_t)a[25] : init.mode;
                init.mini2 = (a[26] >= 0.0) ? (uint8_t)(a[26] != 0.0)
                                            : init.mini;
                // --rot2: the second body's own angle and spin (31st-33rd). Not said, it
                // starts from p1's -- the angle the pair's second half used to read anyway.
                {
                    const bool told = a[30] > -9998.0;
                    init.rot2 = told ? (float)a[30] : init.rot;
                    init.rotNeg2 = told ? (uint8_t)(a[31] != 0.0 ? 1 : 0) : init.rotNeg;
                    init.rotStep2 = (init.mode2 == 2)
                                        ? (told ? (float)std::fabs(a[32]) : init.rotStep) : 0.f;
                }
            }
        }
    }
    // seed the per-state float accumulator with the anchor's absolute x. The
    // rounding depends on the magnitude, so this cannot be deferred (see advanceX)
    init.xAbs = (float)x0;

    // --horizon N: stop as soon as the frontier has survived N ticks past the
    // anchor and emit that prefix as the plan. The driver only ever uses the
    // part up to the next death, so solving all the way to the level end every
    // iteration is wasted work (17k ticks re-solved to use 300 of them).
    long long horizon = 0;
    long long planCut = 0;   // >0: emit only the plan up to this tick
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--horizon")) horizon = std::atoll(argv[i + 1]);

    prepMark(0);
    // Read only from here on: the level loader takes it by const pointer, and the rest look up.
    const GroupTimeline& gt = groupLayersFor(groupsPaths);   // ...and sets g_groupInit
    prepMark(1);
    if (!trigPath.empty() && !grpPath.empty()) {
        // The window (--touch-from-anchor) stays opt-in in general -- the note at
        // the cap records a measured failure with it ON at an early anchor -- but
        // there is one case where NOT windowing is strictly worse than anything:
        // an anchor so deep that the plain first-32 list lies entirely behind it.
        // There the model can see no door ahead at all, needUnseen has nothing to
        // force, and a section built on touch-doors is a dead end by construction
        // (lv22's shaft: four chained doors at x=20,081..20,141 against 155 touch
        // triggers in the level -- the roof at (20115,1215) could never open, and
        // every climb died under it while the machinery asked for capacity).
        // Auto-window exactly then: the levels below 33 triggers never trip this,
        // and early anchors keep the measured-safe plain list.
        g_touchRetimeFrom = t0;   // --touchretime re-times only boxes entered from here on
        // THE COIN SIDE FIRST, because the probe below decides the gate from a
        // list loadTouchTriggers builds, and both of these change that list: a
        // Count or Tap trigger is a root only when its chain reaches a coin
        // (g_coinUids), and a box is kept as a feeder only when its chain
        // reaches something items.txt says gives the item a coin's counter
        // reads (g_itemGiver, filled by loadCollectibles). Loaded after the
        // probe, the probe would be deciding on a population the run does not
        // use -- which is the fault --trigwinsel exists to name.
        if (g_coinRoute) {
            g_coinUids = coinUids(argv[1]);
            std::printf("coins: %zu coin uids for the trigger window\n",
                        g_coinUids.size());
        }
        if (g_coinRoute && !itemPath.empty()) {
            loadCollectibles(itemPath);
            if (!g_collect.empty())
                std::printf("items: %zu pickups\n", g_collect.size());
        }
        // The types the chains reach (the relevance filter reads them, --trigdump
        // prints them). Read here because loadLevel has not run yet and this is
        // the only place that knows the dump's path.
        // MOVED ABOVE THE GATE (2026-09-21) so the probe below can use it: see
        // g_trigWinSel.
        // In-process, argv[1] is a placeholder and the level is g_levelCsv (see
        // loadObjTypesFrom). --csvtypes reads it from there.
        // The cached tables are read in place rather than copied (117k entries each on a custom
        // level): nothing below writes to either.
        std::unordered_map<int, int> objTypesOwn;
        const std::unordered_map<int, int>* objTypesP = &objTypesOwn;
        if (!g_levelCsv.empty()) {
            LevelTextCache& lc = levelTextFor(g_levelCsv);
            if (!lc.haveTypes) {
                std::istringstream cs(g_levelCsv);
                lc.types = loadObjTypesFrom(cs);
                lc.haveTypes = true;
            }
            objTypesP = &lc.types;
        } else {
            objTypesOwn = loadObjTypes(argv[1]);
        }
        const std::unordered_map<int, int>& objTypes = *objTypesP;
        bool winTouch = g_touchFromAnchor;
        // A rotated-frame anchor is placed by its WORLD x (the boxes' cx are
        // world), and windows around it (triggers.hpp, g_trigWinNear).
        const bool rotWin = startFrame != 0 && x0World > -1e17;
        const double xGate = rotWin ? x0World : x0;
        if (!winTouch && xGate > -1e17) {
            // --trigwinsel: probe the population this call will ACTUALLY load.
            // Without it the probe runs with the selection off while g_touch
            // below runs with it on (it has been on by default since 934c21e),
            // so the gate is decided on a list the run never uses: on lv22 the
            // probe's last box is at cx 2,283 and the loaded set reaches 3,675,
            // and the gate fires ~1,400 px of anchor too early. Early windowing
            // is the direction the note at the cap records a measured failure
            // for, so this is not a safe inconsistency -- but it does change
            // which anchors window, so it is off until measured.
            const std::vector<TouchTrig> plain =
                loadTouchTriggers(trigPath, grpPath, -1e18,
                                  &objTypes);
            // THE LAST BOX, NOT THE LAST ENTRY. With --coins the list ends with
            // the roots held outside the cap (Counts, Taps, Item Compares --
            // triggers.hpp), which have no x the player passes, and lv22's tap
            // at x=15,525 sat last in every probe: no anchor ever windowed, and
            // every coin run carried the boxes of x=511..3,674 all the way to
            // the end (the loop's own `[trigwin] win=0 maxKeptX=3674`). Without
            // coins nothing is held and the max is the last entry, as before.
            double lastBox = -1e18;
            for (const TouchTrig& e : plain)
                if (e.count < 0 && !e.tap) lastBox = std::max(lastBox, e.cx);
            if ((int)plain.size() >= kTouchBits && !plain.empty()
                && lastBox < xGate - 200.0) {
                winTouch = true;
                // ...and say how many, rather than "32": the cap is kTouchBits
                // and the probe's own count is what the test just used.
                std::printf("triggers: the plain %d all lie behind the anchor "
                            "(last %.0f < %.0f) - windowing from the anchor\n",
                            (int)plain.size(), lastBox, xGate);
            }
        }
        // --movetarget: object positions, for a target-mode Move's destination.
        // In-process the level is g_levelCsv, not argv[1] (a placeholder there).
        std::unordered_map<int, std::pair<double, double>> objPosOwn;
        const std::unordered_map<int, std::pair<double, double>>* objPosP = &objPosOwn;
        if (!g_levelCsv.empty()) {
            LevelTextCache& lc = levelTextFor(g_levelCsv);
            if (!lc.havePos) {
                std::istringstream cs(g_levelCsv);
                lc.pos = loadObjPos(cs);
                lc.havePos = true;
            }
            objPosP = &lc.pos;
        } else {
            std::ifstream fs(argv[1]);
            if (fs) objPosOwn = loadObjPos(fs);
        }
        const std::unordered_map<int, std::pair<double, double>>& objPos = *objPosP;
        prepMark(5);
        std::printf("movetarget: %zu object positions from %s\n", objPos.size(),
                    g_levelCsv.empty() ? "the dump" : "the in-process level table");
        if (rotWin && winTouch) g_trigWinNear = x0World;
        g_touch = loadTouchTriggers(trigPath, grpPath, winTouch ? x0 : -1e18,
                                    &objTypes,
                                    &objPos);
        g_trigWinNear = -1e18;
        // ...and publish what that decided, because nothing else can see it:
        // the numbers are printed and the mod has no pipe. See SolveOutcome.
        g_outcome.trigWinTouch = g_touch.empty() ? -1 : (winTouch ? 1 : 0);
        g_outcome.trigTotal = (long long)g_trigTotal;
        g_outcome.trigRelevantN = (long long)g_trigRelevantN;
        g_outcome.trigKept = (long long)g_trigKept;
        g_outcome.trigDroppedRelevant = (long long)g_trigDroppedRelevant;
        g_outcome.trigDroppedBehind = (long long)g_trigDroppedBehind;
        g_outcome.trigDroppedAhead = (long long)g_trigDroppedAhead;
        g_outcome.trigMaxKeptX = g_trigMaxKeptX;
        // FNV-1a over the uids in BIT ORDER. Order is the point -- the same
        // set of boxes in a different order is a different bit->uid map and
        // must hash differently, which a set hash would hide.
        {
            unsigned long long h = 1469598103934665603ull;
            for (const TouchTrig& t : g_touch) {
                unsigned long long u = (unsigned long long)(unsigned)t.uid;
                for (int k = 0; k < 8; ++k) {
                    h ^= (u >> (k * 8)) & 0xffull;
                    h *= 1099511628211ull;
                }
            }
            g_outcome.trigMapSig = g_touch.empty() ? 0ull : h;
        }
        buildTouchMoveTicks();   // the dedupe key's "is this box still moving"
        // ...and "is the button down near a toggle block" (g_pressWin). 15 is the largest
        // half any mode has, 20 more covers a tick's travel at the fastest speed, since the
        // box reads the PARENT's button and so the held parent has to survive the tick before.
        g_pressWin.clear();
        g_actMask = TouchMask{};
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            if (g_touch[b].press)
                g_pressWin.push_back({g_touch[b].cx - g_touch[b].hw - 35.0,
                                      g_touch[b].cx + g_touch[b].hw + 15.0, (int)b});
            if (g_touch[b].activator) g_actMask |= touchBit((int)b);
        }
        g_autoTrig = loadAutoTriggers(trigPath, grpPath);
    }
    prepMark(2);
    if (!obbPath.empty()) g_obb = loadObb(obbPath);   // before loadLevel reads it
    // The flags decide the SHAPE of a circular hazard's test, so they have to
    // be in before anything asks hazardHit anything.
    if (setPath.empty()) setPath = settingsPathBeside(argv[1]);
    if (!setPath.empty() && loadLevelSettings(setPath) && g_fixRadiusCollision)
        std::printf("levelsettings: fixRadiusCollision=1 - circular hazards "
                    "use centre distance (branch A) in this level\n");
    // Also reuse identical preparations between this job's fixup passes. Fixups/replay inputs
    // are consumed after loadLevel, and every world file is immutable for the job.
    Level L;
    {
        std::vector<std::string> la;
        const uint64_t job = g_levelCsv.empty() ? 0 : g_inputFiles.job();
        if (g_ladder || job) la = ladderArgs(argc, argv);
        const bool scopeMatch = job ? g_ladderLevel.job == job
                                   : g_ladder && g_ladderLevel.ladder == g_ladder;
        if (scopeMatch
            && g_ladderLevel.groupsGen == g_groupLayersGen && g_ladderLevel.args == la) {
            if (job) ++g_inputFiles.levelHits;
            L = g_ladderLevel.level;
            g_ladderLevel.writes.put();
            std::fputs(g_ladderLevel.printed.c_str(), stdout);
        } else {
            std::string printed;
            if (g_ladder || job) g_loadPrinted = &printed;
            L = loadLevel(argv[1], groupsPaths.empty() ? nullptr : &gt,
                          g_touch.empty() ? nullptr : &g_touch,
                          g_autoTrig.empty() ? nullptr : &g_autoTrig);
            g_loadPrinted = nullptr;
            if (g_ladder || job) {
                g_ladderLevel.ladder = g_ladder;
                g_ladderLevel.job = job;
                g_ladderLevel.args = std::move(la);
                g_ladderLevel.groupsGen = g_groupLayersGen;
                g_ladderLevel.level = L;
                g_ladderLevel.writes.take();
                g_ladderLevel.printed = std::move(printed);
            }
        }
    }
    prepMark(3);
    // A level this build cannot represent: say so and return 2 -- the exit code the
    // loader's old std::exit(2) gave the CLI, without ending the host process when
    // this runs inside the game (see Level::unsupported).
    if (!L.unsupported.empty()) {
        std::printf("unsupported: %s\n", L.unsupported.c_str());
        std::fprintf(stderr, "level unsupported: %s\n", L.unsupported.c_str());
        g_outcome.unsupported = L.unsupported;
        return 2;
    }
    g_baseLv = &L;   // see levelOfFrame / applyRotation's re-tap
    g_gpHandoff = L.gpHandoff;   // shared gravity-portal bits (prelude.hpp); empty on most levels
    if (!g_gpHandoff.empty())
        std::printf("gravity portals: %zu bits shared past %d, handed on at x=%.1f..%.1f\n",
                    g_gpHandoff.size(), kGravPortalBits, g_gpHandoff.front().x,
                    g_gpHandoff.back().x);
    // TouchTrig::followIdx: a box whose own group a Move displaces follows a recorded object
    // moved by exactly the same Moves -- the same set of Move-targeted groups, so neither can
    // travel without the other -- the nearest one to the box. The trigger itself has no row in
    // any recording. Printed per box, so a run says which boxes move and after what.
    // markTouched adds the object's WHOLE displacement, which is the box's own only when
    // everything that moves either of them is a translation of whole groups (Move in any
    // mode, Follow, Follow Player Y) and both carry the same set of such groups. A Rotate,
    // a Scale, an Area effect or an Advanced Follow moves each object by an amount that
    // depends on where it is, so a box under one is left unfollowed and says so -- lv22's
    // pulse boxes 197/277/334 turn with the arms of groups 38-40, at another radius than any
    // recorded object. (An Edit Area's target may be an effect id rather than a group; the
    // test then refuses more than it needs to, never less.)
    if (!g_touch.empty() && !trigPath.empty() && !grpPath.empty() && !L.dyn.objs.empty()) {
        std::unordered_map<int, TrigRow> rows;
        if (loadTrigRows(trigPath, rows)) {
            std::unordered_set<int> moved;   // groups some Move displaces
            std::unordered_set<int> rigid;   // groups a whole-group translation acts on
            std::unordered_set<int> bent;    // groups a position-dependent transform acts on
            for (const auto& kv : rows) {
                const TrigRow& r = kv.second;
                if (r.target <= 0) continue;
                if (r.id == 901 && (r.ox != 0.0 || r.oy != 0.0)) moved.insert(r.target);
                if (r.id == 901 || r.id == 1347 || r.id == 1814) rigid.insert(r.target);
                if (r.id == 1346 || r.id == 2067 || (r.id >= 3006 && r.id <= 3008)
                    || (r.id >= 3011 && r.id <= 3013) || r.id == 3016)
                    bent.insert(r.target);
            }
            std::unordered_map<int, std::vector<int>> movedOf;   // uid -> its moved groups
            std::unordered_map<int, std::vector<int>> rigidOf;   // uid -> its translated groups
            std::unordered_set<int> bentUid;
            {
                std::ifstream in(grpPath);
                std::string line;
                std::getline(in, line);   // header
                while (std::getline(in, line)) {
                    std::stringstream ss(line);
                    int uid = 0, g = 0;
                    if (!(ss >> uid)) continue;
                    while (ss >> g) {
                        if (moved.count(g)) movedOf[uid].push_back(g);
                        if (rigid.count(g)) rigidOf[uid].push_back(g);
                        if (bent.count(g)) bentUid.insert(uid);
                    }
                }
                for (auto& kv : movedOf) std::sort(kv.second.begin(), kv.second.end());
                for (auto& kv : rigidOf) std::sort(kv.second.begin(), kv.second.end());
            }
            const auto rigidSig = [&](int uid) {
                const auto it = rigidOf.find(uid);
                return it == rigidOf.end() ? std::vector<int>{} : it->second;
            };
            int nFollow = 0, nRefused = 0;
            for (TouchTrig& T : g_touch) {
                const auto want = movedOf.find(T.uid);
                if (want == movedOf.end() || want->second.empty()) continue;
                const std::vector<int> wantRigid = rigidSig(T.uid);
                int best = -1, nSameMoves = 0;
                double bestD = 1e18;
                for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                    const int u = L.dyn.objs[i].uid;
                    const auto have = movedOf.find(u);
                    if (have == movedOf.end() || have->second != want->second) continue;
                    ++nSameMoves;
                    if (bentUid.count(T.uid) || bentUid.count(u) || rigidSig(u) != wantRigid)
                        continue;
                    const double d = std::fabs(L.dyn.objs[i].cx - T.cx)
                                     + std::fabs(L.dyn.objs[i].cy - T.cy);
                    if (d < bestD) { bestD = d; best = (int)i; }
                }
                if (best < 0) {
                    if (nSameMoves) {
                        ++nRefused;
                        std::printf("touchfollow: box uid %d NOT followed: %s\n", T.uid,
                                    bentUid.count(T.uid)
                                        ? "a Rotate/Scale/Area/Advanced Follow moves its "
                                          "group, by an amount that depends on position"
                                        : "no recorded object shares all its transforms");
                    }
                    continue;
                }
                // load-time position: seek() has not run yet
                T.followIdx = best;
                T.followX0 = L.dyn.objs[(size_t)best].cx;
                T.followY0 = L.dyn.objs[(size_t)best].cy;
                ++nFollow;
                std::printf("touchfollow: box uid %d follows dynamic uid %d (%zu moved "
                            "group(s), %.0f px away)\n", T.uid,
                            L.dyn.objs[(size_t)best].uid, want->second.size(), bestD);
            }
            if (nFollow || nRefused)
                std::printf("touchfollow: %d box(es) follow, %d refused\n", nFollow, nRefused);
            if (nFollow) g_touchFollowObjs = &L.dyn.objs;
        }
    }
    // Beside the dump when nobody said, the way levelsettings and rotgameplay are
    // (forceIdsPathBeside; it returns empty unless the file is really there).
    if (g_forceIdsPath.empty()) g_forceIdsPath = forceIdsPathBeside(argv[1]);
    if (!g_forceIdsPath.empty()) applyForceIds(g_forceIdsPath);
    // ...and build every frame the level can turn into, NOW. frameLevel() is a
    // lazy cache, and applyRotation's re-tap needs the NEW frame's geometry on
    // the tick it turns -- which for the first rotation is before anything else
    // has asked for it. Building it there would also be a data race: the search
    // calls applyRotation from the worker threads.
    for (const RotTrig& r : g_rotTrig) (void)frameLevel(L, r.frame);
    // --spentrot: burn the 2900s already fired before the anchor (history at
    // the declaration). firedT=0 means "fired at t=0" = skipped on every later
    // tick via shot != t. revT is left alone: all the driver can derive is the
    // gframe transitions = frame-changing firings; the consumption of a
    // reverse toggle does not show in the dump.
    if (!g_spentRot.empty()) {
        int hit = 0;
        for (int su : g_spentRot)
            for (RotTrig& r : g_rotTrig)
                if (r.uid == su && r.firedT < 0) { r.firedT = 0; ++hit; }
        std::printf("spentrot: %d/%zu triggers pre-fired\n",
                    hit, g_spentRot.size());
    }
    // Resolve the anchor's stair-snap object now that the level exists. Pointers
    // into L.objs are what the snap block compares against (`c.snapObj != o`),
    // so it has to be the SAME object, not a copy: only the identity matters for
    // the gate, and the pattern is measured from its cx/cy.
    // A MOVING solid lives in L.dyn.objs, not L.objs, and the player can be
    // standing on one -- both stores are stable across ticks by construction
    // (see the note on Dynamics::objs), which is what makes the raw pointer
    // legal here in the first place.
    if (startSnapUid >= 0) {
        for (const Obj& o : L.objs)
            if (o.uid == (int)startSnapUid) { init.snapObj = &o; break; }
        if (!init.snapObj)
            for (const Obj& o : L.dyn.objs)
                if (o.uid == (int)startSnapUid) { init.snapObj = &o; break; }
        if (init.snapObj) init.snapDist = (float)startSnapDist;
        else std::printf("start: snap uid %lld not found in the level\n",
                         startSnapUid);
    }
    // [2026-08-19] The anchor cannot carry CONSUMED PADS either (same family
    // of hole as rHover/dash/snapObj). A pad is consumed on contact (note in
    // stepOne's pad loop) and does not re-fire while the contact lasts. If the
    // player's box at t0 overlaps a pad's firing box, that contact began before
    // t0 and GD has already fired it -- start with it consumed.
    // Measured on lv17 t=14,601: GD had fired the yellow pad uid7254
    // (18,943,152) at t=14,577, yet the section anchor at t0=14,600 re-fired
    // it for vy=16 (census m4/orbnear/in1, edvy +10.807).
    // Orbs are CONSUMED ONLY BY A PRESS, so they cannot be seeded from
    // position alone (deferred). Anchors in a rotated frame are deferred too,
    // the coordinate system being different.
    // y overlap is checked as well: an x-only overlap (ducking past the side)
    // is unfired in GD too, so it is not seeded -- seeding there would err on
    // the side of killing a real firing.
    if (t0 > 0 && startFrame == 0) {
        const double sHalf = playerHalf(init.mode, init.mini != 0);
        int slot = 0;
        for (const Obj& o : L.pads) {
            if (slot >= 4) break;
            if (std::fabs(x0 - o.cx) >= o.hw + sHalf + kPadReach) continue;
            if (std::fabs((double)init.y - o.cy) >= o.hh + sHalf) continue;
            // The same shape the pad loop uses, or the seeder and the loop
            // would disagree about what "already in contact" means for a
            // rotated board -- see the padobb / padplayerrot note at the loop's
            // site in step.hpp. A type-8 pad's activation is judged with the
            // player's square TURNED BY ITS OWN ROTATION, so the anchor asks the
            // same question here, under the same two arms. (No one-step
            // advance of the angle. That used to be a DIFFERENCE from the loop,
            // justified by `init.rot` being a whole seeded state rather than the
            // mid-tick value the loop reconstructed from `s.rot`; since the
            // advance was deleted at the loop's site the two ask the same thing,
            // and `--no-satrotraw` reintroduces the difference along with it.)
            double sRotPad = (double)init.rot;
            if (o.type == 8) {
                if (g_noPadPlayerRot) sRotPad = 0.0;
                else if (!padPlayerRotMode(init.mode)) sRotPad = 0.0;
            }
            if (o.oriented
                && !orientedHit(o, x0, (double)init.y, sHalf, sRotPad))
                continue;
            init.usedPad[slot++] = &o;
        }
        // ...AND THE PADS THE RUN FIRED EARLIER AND STEPPED OFF (--spentpad,
        // history at g_spentPad's declaration). The block above only ever
        // catches a contact still in progress at t0, because it asks a question
        // about the CURRENT position. GD's latch is permanent for the attempt,
        // so a pad fired thirty ticks ago is just as dead -- and lv18's
        // x~25,000 zig-zag walks straight back into six of them.
        //
        // The two are kept apart rather than merged. The overlap rule needs no
        // caller and works for anyone who passes only --start -- the repair
        // loop's own anchors, an MCP probe, any hand-built section -- while the
        // list is the recording talking, and is only there when someone has
        // read a recording. Its one witness, lv17 t=14,601, is covered by both.
        //
        // THE X WINDOW IS DECIDED HERE and not by the producer, because it is
        // this side's rule: `usedPad` is released on x alone (the release loop
        // at the end of stepOne's pad section) and there are four slots for a
        // level with up to 82 pads, so "which of the fired pads can still
        // matter" has to be answered with the same expression the release uses.
        //
        // A PAD CARRIED BY MOVING GEOMETRY IS NOT SEEDED, and is counted
        // separately rather than folded into "unknown", which has to keep its
        // own meaning (a stale dump, the wrong level). Such a pad lives in
        // L.dyn.objs, where `cx` is its position at load time and not at t0 --
        // the x window below would be measured against the wrong place, and
        // the producer's own contact test has the same problem. Measured over
        // quick_regress's 1,116 sections: 406 named uids land here, all of them
        // on lv19/20/21/22, which are exactly the levels with grouped pads
        // (16/15/2/2 of them). lv1-18 name none.
        if (g_spentPadSeed && !g_spentPad.empty()) {
            int seeded = 0, gone = 0, unknown = 0, overflow = 0, carried = 0;
            for (int su : g_spentPad) {
                const Obj* pad = nullptr;
                for (const Obj& o : L.pads)
                    if (o.uid == su) { pad = &o; break; }
                if (!pad) {
                    bool moving = false;
                    for (const Obj& o : L.dyn.objs)
                        if (o.uid == su) { moving = true; break; }
                    (moving ? carried : unknown) += 1;
                    continue;
                }
                if (std::fabs(x0 - pad->cx) >= pad->hw + sHalf + kPadReach) {
                    ++gone;      // released before t0 anyway -- see the release
                    continue;
                }
                bool have = false;
                for (int i = 0; i < slot; ++i)
                    if (init.usedPad[i] == pad) { have = true; break; }
                if (have) continue;             // the overlap rule got it first
                if (slot >= 4) { ++overflow; continue; }
                init.usedPad[slot++] = pad;
                ++seeded;
            }
            // Reported, not silent: "nothing happened" is this block's failure
            // mode, and a caller whose uids name no pad of this level (a stale
            // dump, the wrong level) would otherwise look exactly like a caller
            // with nothing to seed.
            std::printf("spentpad: %d/%zu pads pre-fired (%d already outside "
                        "the x window, %d carried by moving geometry, %d not a "
                        "pad in this level)\n",
                        seeded, g_spentPad.size(), gone, carried, unknown);
            if (overflow)
                std::printf("spentpad: %d DROPPED - all four usedPad slots are "
                            "taken at t0\n", overflow);
        }
        // --spentorb (speed.hpp): the rings the attempt fired before t0, replayed through
        // noteRingFired oldest first, so usedOrb / usedOrbOld (and the --ringonce history) hold
        // the most recent ones as they would have had the run started at the head.
        if (!g_spentOrb.empty()) {
            int seeded = 0, unknown = 0, carried = 0;
            for (int su : g_spentOrb) {
                const Obj* ring = nullptr;
                for (const Obj& o : L.orbs)
                    if (o.uid == su) { ring = &o; break; }
                // --spentdyn: ...and a ring in dyn (a grouped ring is routed there and L.orbs
                // never holds it). Unlike a pad it needs no position: a ring is spent by uid
                // (usedOrbOld / the history), and usedOrb points at the same L.dyn.objs entry
                // the step collects. A custom level anchored at t0=16,064 named 57 rings GD had
                // fired and 34 of them were dropped as "not a ring"; the pink ring uid 8532, the
                // last GD fired, stayed live and the model's UFO press fired it a second time at
                // t=16,085 where GD's gave the plain jump.
                if (!ring)
                    for (size_t i = 0; i < L.dyn.objs.size(); ++i)
                        if (L.dyn.objs[i].uid == su && L.dyn.bucket[i] == Dynamics::ORB) {
                            ring = &L.dyn.objs[i];
                            ++carried;
                            break;
                        }
                if (!ring) { ++unknown; continue; }
                noteRingFired(init, ring);
                ++seeded;
            }
            std::printf("spentorb: %d/%zu rings pre-fired (%d not a ring in this level)\n",
                        seeded, g_spentOrb.size(), unknown);
            std::printf("spentorb: %d of them carried by moving geometry\n", carried);
        }
    }
    // A re-anchored solve starts mid-level, where the doors the earlier part of
    // the run already opened ARE open. That is not an assumption to make -- it
    // is in the recording, so read it from there: a controlled object that has
    // already travelled most of its offset by t0 means its trigger fired.
    // Without this the anchor is dropped inside a door GD has open and the
    // frontier dies on tick 0 (measured on lv19 --start 19900, x=27,721, which
    // sits exactly where gate uid 13395 was before it slid away).
    // ...unless GD told us directly. The recording can only answer for objects
    // it contains, which is why reading it left 163 differences on lv22; a
    // payload names the triggers GD saw fire and their ticks, so nothing has to
    // be inferred. It takes precedence over the block below and reports what it
    // set, including the boxes it named that this build has no bit for -- a
    // payload written against a wider window than this one's 32.
    std::vector<std::pair<int, int>> payloadTouch;
    std::vector<int> payloadPortal, payloadPortal2;
    std::vector<std::pair<std::string, int>> payloadHist;
    int histVersion = 1;
    bool havePayload = false;
    if (!g_anchorState.empty()) {
        std::vector<std::pair<std::string, std::string>> kv;
        std::string unknown;
        if (!parseAnchorState(g_anchorState, kv, unknown)) {
            std::printf("seed payload rejected: unknown key '%s'\n"
                        "  this exe knows:", unknown.c_str());
            for (const char* kk : kAnchorKeys) std::printf(" %s", kk);
            std::printf("\n  a payload naming something else was written by a "
                        "build that carries what this one would drop\n");
            return 2;
        }
        bool sawTouchKey = false, sawPortalKey = false, sawPortal2Key = false;
        bool sawHistKey = false;
        for (const auto& p : kv) {
            if (p.first == "owns") {
                if (p.second.find("touch") != std::string::npos)
                    g_ownsTouch = true;
                if (p.second.find("portal") != std::string::npos)
                    g_ownsPortal = true;
                if (p.second.find("hist") != std::string::npos)
                    g_ownsHist = true;
            } else if (p.first == "hist") {
                // version|count|name:value,... -- see kHistNames.
                const std::string& v = p.second;
                const size_t b1 = v.find('|');
                const size_t b2 = (b1 == std::string::npos) ? b1
                                                            : v.find('|', b1 + 1);
                if (b2 == std::string::npos
                    || (v.substr(0, b1) != "1" && v.substr(0, b1) != "2"
                        && v.substr(0, b1) != "3" && v.substr(0, b1) != "4")) {
                    std::printf("seed payload rejected: hist '%s' is not version "
                                "1, 2, 3 or 4 (version|count|name:value,...)\n", v.c_str());
                    return 2;
                }
                histVersion = std::atoi(v.substr(0, b1).c_str());
                const int want = std::atoi(v.substr(b1 + 1, b2 - b1 - 1).c_str());
                int got = 0;
                size_t i = b2 + 1;
                while (i < v.size()) {
                    size_t c = v.find(',', i);
                    if (c == std::string::npos) c = v.size();
                    const std::string nv = v.substr(i, c - i);
                    i = c + 1;
                    if (nv.empty()) continue;
                    const size_t colon = nv.find(':');
                    const std::string name = nv.substr(0, colon);
                    bool known = false;
                    for (const char* hn : histNamesFor(histVersion))
                        if (hn && name == hn) known = true;
                    if (colon == std::string::npos || !known) {
                        std::printf("seed payload rejected: hist name '%s' is "
                                    "not one version %d knows:", name.c_str(),
                                    histVersion);
                        for (const char* hn : histNamesFor(histVersion))
                            if (hn) std::printf(" %s", hn);
                        std::printf("\n");
                        return 2;
                    }
                    payloadHist.emplace_back(name,
                                             std::atoi(nv.substr(colon + 1).c_str()));
                    ++got;
                }
                if (got != want) {
                    std::printf("seed payload rejected: hist says %d values and "
                                "carries %d\n", want, got);
                    return 2;
                }
                sawHistKey = true;
            } else if (p.first == "touch") {
                payloadTouch = parseTouchPayload(p.second);
                sawTouchKey = true;
            } else if (p.first == "portal") {
                payloadPortal = parseUidList(p.second);
                sawPortalKey = true;
            } else if (p.first == "portal2") {
                payloadPortal2 = parseUidList(p.second);
                sawPortal2Key = true;
            }
        }
        havePayload = g_ownsTouch;
        // A PAYLOAD REPLACES THE RECORDING-DERIVED SEED WHOLESALE, so a key it
        // does not carry is a value nobody sets -- not a value that falls back.
        // Found by this build's own first test: a payload with `touch` and no
        // `lockOff` seeded the fire ticks perfectly and left the platform's ride
        // at zero, and the replay died 45 ticks later. That is the exact silent
        // degradation this refusal exists for, so it is not optional.
        // Refusal is per DECLARED subsystem: a payload that claims `touch` and
        // carries no `touch` key is incomplete, while one that never claimed
        // the lock is not missing anything -- the lock simply keeps its own
        // seeding.
        std::string missing;
        if (g_ownsTouch && !sawTouchKey) missing = "touch";
        // Both halves, because the mask is per player: a payload that claims
        // the subsystem and names only p1 has left p2 to a default, which is
        // the shape of degradation this refusal exists for.
        if (g_ownsPortal && !sawPortalKey)
            missing += (missing.empty() ? "" : " ") + std::string("portal");
        if (g_ownsPortal && !sawPortal2Key)
            missing += (missing.empty() ? "" : " ") + std::string("portal2");
        // ...and a claimed hist must name every value version 1 knows: an
        // omitted one would be left at its default by nobody's decision.
        if (g_ownsHist) {
            if (!sawHistKey)
                missing += (missing.empty() ? "" : " ") + std::string("hist");
            for (const char* hn : histNamesFor(histVersion)) {
                if (!hn) continue;
                bool have = false;
                for (const auto& h : payloadHist) if (h.first == hn) have = true;
                if (sawHistKey && !have)
                    missing += (missing.empty() ? "" : " ") + std::string(hn);
            }
        }
        if (!missing.empty()) {
            if (!g_seedPartialOk) {
                // Say WHY this is fatal rather than a warning: the payload
                // replaces the recording-derived seed wholesale, so a key it
                // omits is not filled from the recording -- it is filled by
                // nobody. Without this line the next reader assumes the old
                // path still covers the gap, which is what the first test of
                // this code did: three fire ticks perfect, the ride at zero,
                // dead 45 ticks later.
                std::printf("seed payload incomplete: missing %s\n"
                            "  a payload REPLACES the recording-derived seed, "
                            "so an omitted key is set by nobody\n"
                            "  this exe wants:", missing.c_str());
                for (const char* kk : kAnchorKeys) std::printf(" %s", kk);
                std::printf("\n  payload carried:");
                for (const auto& p : kv) std::printf(" %s", p.first.c_str());
                std::printf("\n  (the shorter list is the older build; pass "
                            "--seed-partial-ok to run anyway)\n");
                return 2;
            }
            // Allowed -- but the result has to carry it, because a line on
            // stderr does not survive into the place results are compared.
            g_seedPartial = missing;
        }
    }
    if (t0 > 0 && havePayload) {
        int set = 0, unmapped = 0;
        for (const auto& ut : payloadTouch) {
            int bit = -1;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
                if (g_touch[b].uid == ut.first) { bit = (int)b; break; }
            if (bit < 0) { ++unmapped; continue; }
            init.trig |= touchBit(bit);
            init.fireB[bit] = (uint16_t)std::max(0, ut.second);
            ++set;
        }
        std::printf("seed payload: %d boxes set, %d named but not in this "
                    "build's window\n", set, unmapped);
        // Provenance, in the run's own output. Which side set a value is the
        // first question when two runs disagree, and stderr does not survive
        // into the place results are compared.
        std::printf("seed: touch=payload lock=recording\n");
        // The one diagnosis worth pre-writing, because the convention breaks
        // in exactly one shape: values are the state at t0, and a payload
        // written at t0+1 lands one tick of motion further on. If the first
        // compared tick is out by about one dx (1.615 px on lv19), that is
        // what happened -- not a physics difference.
        std::printf("seed payload: values are the state at t0; a first-tick "
                    "difference near one dx means the payload was written at "
                    "t0+1\n");
    }
    // --touchseed (--rotqtoggle's anchor seed): the touch boxes the anchored
    // attempt had already entered by t0, named by uid and mapped onto this
    // build's window exactly as the payload's touch list is above. OR-ed in, and
    // ownership is not claimed, so the recording-derived seeding below still runs
    // for every other box.
    if (t0 > 0 && !g_touchSeedArg.empty()) {
        int set = 0, unmapped = 0;
        for (const auto& ut : parseTouchPayload(g_touchSeedArg)) {
            int bit = -1;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
                if (g_touch[b].uid == ut.first) { bit = (int)b; break; }
            if (bit < 0) { ++unmapped; continue; }
            init.trig |= touchBit(bit);
            init.fireB[bit] = (uint16_t)std::max(0, ut.second);
            ++set;
        }
        std::printf("seed touchseed: %d boxes set, %d named but not in this "
                    "build's window\n", set, unmapped);
    }
    // WHICH GRAVITY PORTALS THE RUN HAD ALREADY SPENT before t0. Without this a
    // state handed to --start starts with an empty mask and re-fires every
    // portal the run has passed -- the same hole State::fireB, State::lockOff
    // and the rotation queue each fell into, and the reason the queue is still
    // opt-in. A portal is spent on OVERLAP, so the producer takes the passes
    // out of gdref's x,y rather than out of the firings.
    if (t0 > 0 && g_ownsPortal) {
        int set = 0, unmapped = 0;
        for (int half = 0; half < 2; ++half) {
            const std::vector<int>& ids = half ? payloadPortal2 : payloadPortal;
            GravLatch& mask = half ? init.portalLatch2 : init.portalLatch;
            for (int uid : ids) {
                int bit = -1;
                double px = 0.0;
                for (const Obj& p : L.portals)
                    if (p.uid == uid && p.gpBit >= 0) { bit = p.gpBit; px = p.cx; break; }
                // --tplatch: a teleport in dyn holds a bit too.
                if (bit < 0)
                    for (const Obj& p : L.dyn.objs)
                        if (p.uid == uid && p.gpBit >= 0) { bit = p.gpBit; px = p.cx; break; }
                if (bit < 0) { ++unmapped; continue; }
                // a shared bit already handed on past this portal belongs to the later one
                bool handedOn = false;
                for (const GpHandoff& h : g_gpHandoff)
                    handedOn = handedOn || (h.bit == bit && px < h.x && (double)init.xAbs >= h.x);
                if (handedOn) continue;
                // (this said `mask |= touchBit(bit)` -- the touch mask's helper on
                // the portal latch, which only agreed while both were 32 wide)
                mask.set(bit);
                ++set;
            }
        }
        std::printf("seed payload: %d gravity portals spent (%s/%s), %d "
                    "named but not a gravity portal in this level\n",
                    set, gravLatchHex(init.portalLatch).c_str(),
                    gravLatchHex(init.portalLatch2).c_str(), unmapped);
        std::printf("seed: portal=payload\n");
    }
    // owns=hist: the per-body history values --start does not carry.
    if (t0 > 0 && g_ownsHist) {
        for (const auto& h : payloadHist) {
            if (h.first == "pressSpent") init.pressSpent = (uint8_t)(h.second != 0);
            else if (h.first == "pressSpent2")
                init.pressSpent2 = (uint8_t)(h.second != 0);
            else if (h.first == "freeMode" && g_freeModeCol)
                init.bandBranch = (uint8_t)((init.bandBranch & ~kBandFree)
                                            | kBandFreeKnown
                                            | (h.second != 0 ? kBandFree : 0));
            else if (h.first == "spiderJumpT" && h.second >= 0)
                init.spiderJumpT = (uint8_t)std::min(h.second, kSpiderJumpGraceTicks);
            // slopeOn .. slopeLanded (version 4) are seeded further down, under
            // --anchorride, once the geometric ride guess (rideAtAnchor) has run.
        }
        // --spentaction: a UFO's or swing's flap fires on a fresh edge (`input && !s.action`), and
        // the anchor leaves action at 0, so a press GD had already consumed before t0 looked fresh
        // on the first tick. pressSpent is 0x985 && !0x986 -- the button held and its press used --
        // so the button was down: action 1 is the truth, and the edge is not there. A custom level
        // (custom level C, a cold run's attempt 16): a dual UFO pressing on 387 takes the pink rings
        // uid 392 / 324 on GD's row 388 (vy 4.696 / -4.696); the anchor at 388 says pressSpent 1,
        // and the model flapped again on 389 (6.871) where GD decays.
        if (init.pressSpent && (init.mode == 3 || init.mode == 7))
            init.action = 1;
        std::printf("seed: hist=payload v%d pressSpent=%d pressSpent2=%d "
                    "freeMode=%s spiderJumpT=%d", histVersion, (int)init.pressSpent,
                    (int)init.pressSpent2,
                    !(init.bandBranch & kBandFreeKnown) ? "unknown"
                    : (init.bandBranch & kBandFree) ? "1" : "0",
                    (int)init.spiderJumpT);
        if (histVersion >= 4)
            for (const auto& h : payloadHist)
                if (h.first.rfind("slope", 0) == 0)
                    std::printf(" %s=%d", h.first.c_str(), h.second);
        std::printf("\n");
    }
    // ...and at the level's own start the byte is known: resetLevel leaves
    // [layer+0x311] clear and only a mode portal writes it. Known only when the
    // objrects dump says which portals are Free Mode -- without that column no
    // portal could keep it up to date, so it stays unknown and the band keeps
    // the old grid test (bandIsWall).
    if (t0 <= 0 && g_freeModeCol) init.bandBranch |= kBandFreeKnown;
    // The recording-derived seeding still runs even when a payload owns
    // `touch`, because it also seeds the LOCK -- a subsystem no payload claims
    // today. What ownership changes is only whether the trig/fireB branches
    // below are allowed to write; skipping the whole block would leave the
    // lock unseeded, which is the death this code's first test produced.
    if (t0 > 0 && !g_touch.empty()) {
        if (!havePayload)
            std::printf("seed: touch=recording lock=recording\n");
        // Counted, because "nothing happened" is the failure mode this block has
        // (an anchor that silently keeps every door shut looks exactly like a
        // physics wall, and did for a whole session on lv22).
        int nCtl = 0, nNoRec = 0, nNoOff = 0, nNotEntered = 0, nEnteredRedated = 0;
        for (size_t b = 0; b < g_touch.size(); ++b) {
            // --touchentered: the boxes the attempt's own recorded positions
            // overlapped by t0 (the mod's touchEnteredArg). Given, a box
            // missing from it gets no bit from the recording below: an object
            // moving before t0 says SOMETHING moved it, and when its group is
            // also driven by an autonomous Move that something is not this box.
            // lv22's ceiling (group 265) is moved by the autonomous Move 18311
            // and by seven boxes' chains, so an anchor at x=2,954 used to open
            // all seven, the nearest 240 px ahead of anything the player had
            // reached. Not given, every box may open as before.
            const bool mayOpen = !g_touchEnteredGiven
                                 || g_touchEntered.count(g_touch[b].uid) != 0;
            if (!mayOpen) ++nNotEntered;
            // HOW MANY BOXES CAN THIS SCAN NEVER SEED. Both branches below are
            // gated on `full > 0.5`, and `full` is the TRIGGER TABLE's offset,
            // so a box whose controllers all carry a zero offset -- move to
            // target, lock only, toggle -- gets no bit however plainly the
            // recording shows its objects moving. Counted rather than argued:
            // the fix's blast radius is exactly the boxes this reports.
            // WHY a box gets no bit, per box. Both branches below are gated on
            // `full > 0.5` and `full` is the TRIGGER TABLE's offset, so a
            // controller whose displacement does not live there -- Move-To
            // takes B-A at firing time, a lock or a toggle has no displacement
            // at all -- is invisible to them however plainly its object moved.
            // The three counts separate those cases so the fix is aimed rather
            // than loosened.
            if (g_seedDump >= 0 || g_seedEvery > 0) {
                int passes = 0, movedNoOff = 0, offNoMove = 0, noRec = 0;
                for (const TrigCtl& c : g_touch[b].ctl) {
                    const auto itc = gt.find(c.uid);
                    if (itc == gt.end() || itc->second.empty()) { ++noRec; continue; }
                    const DynSample& f = itc->second.front();
                    double mv = 0.0;
                    for (const DynSample& s : itc->second)
                        mv = std::max(mv, std::hypot((double)s.cx - (double)f.cx,
                                                     (double)s.cy - (double)f.cy));
                    const bool hasOff = std::hypot((double)c.dx, (double)c.dy) > 0.5;
                    if (hasOff && mv > 0.5) ++passes;
                    else if (!hasOff && mv > 0.5) ++movedNoOff;
                    else if (hasOff) ++offNoMove;
                }
                // ...and the key's window for the same box. It is built from
                // durTicks, and the chain walk only takes a hop's duration when
                // that hop MOVES something -- so a box whose mover is a Rotate
                // can lose the window for the same reason it loses the bit.
                const int win = (b < g_touchMoveTicks.size())
                                ? g_touchMoveTicks[b] : -1;
                std::printf("boxctl: box=%zu ctl=%zu seedable=%d "
                            "movedButNoTableOffset=%d offsetButNoMotion=%d "
                            "noRecording=%d window=%d%s\n",
                            b, g_touch[b].ctl.size(), passes, movedNoOff,
                            offNoMove, noRec, win,
                            (passes == 0 && movedNoOff > 0)
                                ? "  <- MISSED: recorded motion, no table offset"
                                : "");
            }
            for (const TrigCtl& c : g_touch[b].ctl) {
                ++nCtl;
                const auto it = gt.find(c.uid);
                const bool dbg = (g_dynDbg >= 0 && c.uid == g_dynDbg);
                if (it == gt.end() || it->second.empty()) {
                    ++nNoRec;
                    if (dbg)
                        std::printf("triggers: dyndbg uid=%d box=%zu -- no "
                                    "recording\n", c.uid, b);
                    continue;
                }
                if (std::hypot((double)c.dx, (double)c.dy) <= 0.5) ++nNoOff;
                if (dbg)
                    std::printf("triggers: dyndbg uid=%d box=%zu off=(%.1f,%.1f) "
                                "samples=%zu t[%d..%d]\n", c.uid, b,
                                (double)c.dx, (double)c.dy, it->second.size(),
                                it->second.front().t, it->second.back().t);
                const DynSample& s0 = it->second.front();
                const DynSample* at = &s0;
                for (const DynSample& s : it->second)
                    if ((long long)s.t <= t0) at = &s;
                const double moved = std::hypot((double)at->cx - (double)s0.cx,
                                                (double)at->cy - (double)s0.cy);
                const double full = std::hypot((double)c.dx, (double)c.dy);
                // WHEN the box was entered, from the recording rather than from
                // the anchor. The position at t0 cannot answer this once the
                // move has finished -- every fire tick at or before t0 - dur
                // puts the object in the same place -- but the recording holds
                // the whole history, and its first moving row is the move's
                // second tick (a box entered on F starts moving on F+1). The
                // recorder's epsilon is 0.05 px, so the first row past that is
                // the first row it wrote.
                //
                // This matters where the position does not: the formula path
                // opens its door FROM this tick, so an estimate 26 ticks late
                // (which `t0 - durTicks` is for lv19's uid14011 -- 20,530
                // against the recording's 20,504) opens the door 26 ticks late
                // and a hazard the real door had cleared kills the player.
                // A LOCK the anchor is already inside. `lockOff` is accumulated
                // tick by tick during a run, so an anchor dropped after the box
                // was punched has none of it -- the platform snaps back to its
                // base and the ride disappears, which is the same hole the fire
                // tick had. The recording answers it directly: this controller
                // moves the object nowhere (a lock-only Move carries offset
                // (0,0)), so whatever its recorded x has done since the first
                // sample IS the lock.
                if (c.lockTicks > 0.0
                    && std::hypot((double)c.dx, (double)c.dy) <= 0.5) {
                    const DynSample& f0 = it->second.front();
                    const DynSample* atL = &f0;
                    for (const DynSample& s : it->second)
                        if ((long long)s.t <= t0) atL = &s;
                    init.lockOff = (float)((double)atL->cx - (double)f0.cx);
                    if (dbg)
                        std::printf("triggers: dyndbg uid=%d box=%zu lock seed "
                                    "%.3f (recorded cx %.3f - base %.3f)\n",
                                    c.uid, b, (double)init.lockOff,
                                    (double)atL->cx, (double)f0.cx);
                }
                // ...minus how late that first row is. `- 1` was right only for
                // a move that clears 0.05 px in its first tick: the recorder
                // drops smaller changes, and an eased move spends its opening
                // ticks in the thousandths, so a SLOW controller's first row
                // can be several ticks after it started. recordLag computes
                // exactly that from the offset, the duration and the curve --
                // it is what the autonomous side already subtracts, and not
                // using it here left lv19's two slow boxes seeded 3 ticks late
                // (--seeddump: fireB[0] 19,586 against 19,589).
                const int lag = recordLag((double)c.dx, (double)c.dy,
                                          c.durTicks, c.ease, c.erate);
                // ...and the first row is looked for ALONG THIS CONTROLLER'S
                // OWN OFFSET. Taking the distance in both axes finds whichever
                // coordinate moved first, and for lv19's platforms that is the
                // LOCK -- their x tracks the player at 1.6 px/tick from the
                // moment the box is punched, so every row clears 0.05 px in x
                // while the eased y is still in the thousandths. The lag then
                // gets subtracted from the wrong row. Measured either way: the
                // door's fire tick is 20,503, which the y-only row (20,506)
                // minus its lag (3) gives and the both-axes row (20,504) does
                // not.
                const bool useX = std::fabs((double)c.dx) > 0.5;
                const bool useY = std::fabs((double)c.dy) > 0.5;
                long long recFire = -1;
                for (const DynSample& s : it->second) {
                    const double ddx = (double)s.cx - (double)s0.cx;
                    const double ddy = (double)s.cy - (double)s0.cy;
                    const bool moved =
                        (useX || useY)
                            ? ((useX && std::fabs(ddx) > 0.05)
                               || (useY && std::fabs(ddy) > 0.05))
                            : (std::hypot(ddx, ddy) > 0.05);
                    if (moved) {
                        // `- lag - 1`, not `- lag`: the move starts the tick
                        // AFTER the box is entered (the chain players read
                        // `(t - F - 1)/dur`), so the first row recordLag counts
                        // to is F + 1 + lag. Measured on all four of lv19's
                        // controllers at once -- 19,590-3-1, 19,950-3-1,
                        // 20,506-2-1 and the lock-only 20,504-0-1 give
                        // 19,586 / 19,946 / 20,503 / 20,503, which is what a
                        // whole run holds.
                        recFire = (long long)s.t - lag - 1;
                        if (g_seedDump >= 0 || g_seedEvery > 0)
                            std::printf("seedscan: box=%zu uid=%d firstRow=%d "
                                        "lag=%d recFire=%lld dx=%.2f dy=%.2f "
                                        "dur=%.3f ease=%d\n",
                                        b, c.uid, s.t, lag, recFire,
                                        (double)c.dx, (double)c.dy, c.durTicks,
                                        c.ease);
                        break;
                    }
                }
                // A payload that owns `touch` has already set these; the rest
                // of this block still runs for the lock.
                if (havePayload) continue;
                if (!mayOpen) continue;
                if (full > 0.5 && moved > full * 0.5) {
                    init.trig |= touchBit(b);
                    // far enough back that the move counts as finished
                    init.trigT = (int32_t)(t0 - (long long)std::llround(c.durTicks));
                    // ...and this box's own tick, or the anchored state would
                    // carry a set bit with no fire tick. fireB defaults to 0,
                    // which the key reads as "fired at tick 0" = long finished,
                    // so a box still sliding at the anchor would merge states
                    // that are genuinely at different points of it.
                    init.fireB[b] = (uint16_t)std::max(
                        0LL, recFire >= 0 ? recFire
                                          : t0 - (long long)std::llround(c.durTicks));
                    break;
                }
                // Started, but not half way: the anchor landed INSIDE the slide.
                // Neither "open" nor "shut" is true, so say open and let the
                // recording carry the phase (see g_recPhase).
                if (full > 0.5 && moved > 0.5) {
                    init.trig |= touchBit(b);
                    g_recPhase |= touchBit(b);
                    // The anchor landed INSIDE the slide, so this box is still
                    // moving and the key has to keep it. How far in is what the
                    // recording already says: `moved` of `full` is the fraction
                    // travelled, so the move began that much before t0. (Under
                    // an ease this is approximate, and approximate LATE is the
                    // safe side -- it holds the box in the key a little longer
                    // than the motion lasts, which costs width, where early
                    // would merge states that still differ.)
                    init.fireB[b] = (uint16_t)std::max(
                        0LL, recFire >= 0
                                 ? recFire
                                 : t0 - (long long)std::llround(
                                            (moved / full) * c.durTicks));
                    break;
                }
                // NO DISPLACEMENT IN THE TABLE, BUT THE RECORDING MOVED.
                //
                // The two branches above answer two different questions with
                // one gate. "Did this state enter the box" is the `trig` bit
                // and CANNOT depend on how far anything travelled; "how far
                // along is the move" is the phase, and only exists where there
                // is a displacement to be a fraction of. Gating both on
                // `full > 0.5` silently answered the first with the second.
                //
                // What falls through is a controller whose motion is not a
                // translation. lv22's boxes 0, 2 and 5 are driven by Rotate
                // triggers (id 1346: uid199 -> group 39, uid255 -> 38,
                // uid321 -> 40 -- exactly three, exactly the three boxes that
                // were missed), and a rotation has no ox/oy to read: the zero
                // offset is CORRECT and the gate was wrong. Measured with
                // seedcheck: 768 unexpected field differences on lv22, every
                // one of them "recorded motion, no table offset", and zero of
                // the opposite kind.
                //
                // The bit is set from the recording having moved BEFORE the
                // anchor, which is the same evidence the branches above use
                // and does not care how the movement is expressed. No phase is
                // claimed: a rotation has no "fraction travelled", so this
                // does not set g_recPhase.
                // THE TICK THIS GIVES IS EARLY, AND KNOWINGLY SO. An object can
                // be under two controllers at once -- lv22's group 39 holds
                // both the touch Rotate (uid199) and an autonomous Move
                // (uid89, oy=+120) -- so the recording's first motion says
                // SOMETHING acted, not that this box was entered. uid195 climbs
                // from t=292 under the autonomous one and is only turned from
                // t=421, while the box is entered at 417.
                //
                // The rotation is not separable from the recording: grouptrace
                // writes the object's own `rot`, which stays 0.000 for the
                // whole run because the group is what turns, and the turn shows
                // only as position drift indistinguishable in kind from the
                // Move's. Splitting them needs the recorded ANGLE, which the
                // dump does not carry per object.
                //
                // So the bit is set from the earlier evidence and the tick is
                // early. That is the safe direction, the same asymmetry as the
                // key window: an early tick holds the box in the key longer
                // than it needs (cost), a late one merges states whose geometry
                // still differs (a wrong answer). Not setting the bit at all is
                // the worst of the three -- the anchor then plans against a
                // door the run had already opened.
                if (full <= 0.5 && recFire >= 0 && recFire <= t0) {
                    init.trig |= touchBit(b);
                    // fireB from the recording's own first motion, taken on
                    // EITHER axis because a rotation has no offset axis to
                    // prefer -- and nothing fast can contaminate it here: the
                    // only co-moving component that could is a lock, and the
                    // census found locks on lv19 alone. Early is the safe
                    // side, the same asymmetry as the key window: too early
                    // holds the box in the key longer (cost), too late merges
                    // states that still differ (a wrong answer).
                    init.fireB[b] = (uint16_t)std::max(0LL, recFire);
                    break;
                }
            }
            // --touchentered uid:tick: a box the scan opened is dated by the
            // attempt's own entry into it (the first recorded row that overlaps
            // it, markTouched's test), not by the first motion of an object it
            // moves -- which the branches above take, and which for an object an
            // autonomous Move also drives is that Move's (g_touchEnteredT).
            if (!havePayload && (init.trig & touchBit(b))) {
                const auto et = g_touchEnteredT.find(g_touch[b].uid);
                if (et != g_touchEnteredT.end() && et->second > 0
                    && (long long)et->second <= t0) {
                    if (init.fireB[b] != (uint16_t)et->second) ++nEnteredRedated;
                    init.fireB[b] = (uint16_t)et->second;
                }
            }
        }
        // --tapclosebox: a box that ARMS a spawned Count or Tap (TouchTrig::armBy) is set when the
        // attempt had entered it by t0 (--touchentered uid:tick, markTouched's test on its rows).
        // The scan above opens a box only from the recorded motion of what its chain moves, and a
        // chain that ends at the roots it spawns may move nothing the recording shows: SubZero
        // 4003's box 6102 (entered at 13139) was missing from an anchor at 13212, so its Count
        // and Tap never listened there. Left to a payload that owns `touch`, as the rest.
        if (!havePayload && g_touchEnteredGiven) {
            TouchMask arms{};
            for (const TouchTrig& R : g_touch)
                if (R.armBy) arms |= R.armBy;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
                const TouchMask bit = touchBit((int)b);
                if (!(arms & bit) || (init.trig & bit)) continue;
                const auto et = g_touchEnteredT.find(g_touch[b].uid);
                if (et == g_touchEnteredT.end() || et->second <= 0
                    || (long long)et->second > t0)
                    continue;
                init.trig |= bit;
                init.fireB[b] = (uint16_t)et->second;
                init.trigT = std::max(init.trigT, (int32_t)et->second);
                std::printf("triggers: --tapclosebox: box %zu uid %d arms a spawned root and "
                            "was entered at t=%d <= t0, so the anchor starts with it set\n",
                            b, g_touch[b].uid, et->second);
            }
        }
        std::printf("triggers: anchor scan t0=%lld: %d controlled objects, "
                    "%d with no recording, %d with no offset\n",
                    t0, nCtl, nNoRec, nNoOff);
        if (g_touchEnteredGiven)
            std::printf("triggers: --touchentered names %zu boxes (%zu with an entry "
                        "tick, %d opened boxes re-dated by it); %d of %zu boxes "
                        "may not open from the recording\n",
                        g_touchEntered.size(), g_touchEnteredT.size(), nEnteredRedated,
                        nNotEntered, g_touch.size());
        if (init.trig)
            std::printf("triggers: anchor starts with mask 0x%llx (already open "
                        "in the recording)\n",
                        (unsigned long long)init.trig.word(0));
        if (g_recPhase)
            std::printf("triggers: mask 0x%llx was still MOVING at the anchor -- "
                        "replayed from the recording's own phase\n",
                        (unsigned long long)g_recPhase.word(0));
    }
    // Autonomous triggers behind the anchor fired before the sim begins; their
    // first-motion tick is already in the recording (recFire of the objects
    // they move), so resolving from it makes the re-timing a no-op for the
    // prefix. No recorded motion at all = treat the move as long finished
    // (fireT 0 saturates the analytic fallback).
    if (!g_autoTrig.empty() && x0 > 0.0) {
        // dx at the anchor, for turning "how far behind is this trigger" into
        // "how many ticks ago did we cross it". Same replay the tail does below.
        double dxA = kDxF;
        {
            size_t si = 0;
            auto gate = [&](const Obj& o) { return o.cx - o.hw - kCubeHalf; };
            while (si < L.speeds.size() && gate(L.speeds[si]) <= x0) {
                dxA = dxForSpeedId(L.speeds[si].id);
                ++si;
            }
        }
        // A level that can turn the player round (property 117 on a ring or a
        // pad, Obj::rev) breaks what est below assumes -- that x advanced
        // monotonically from the crossing to the anchor -- so est cannot vet
        // the recording there. What stays true is that a recorded tick at or
        // before the anchor lies in the prefix this anchor shares with the
        // recorded run, whichever side of x0 the trigger sits on: 4002 anchored
        // at t=20,861 with the player running back through x=31,874 had
        // uid17712 (cx=30,975) recorded at 19,999 and estimated at 20,304, and
        // the 305-tick shift held the drop of group 87 back until the model's
        // platform crushed the player.
        bool canReverse = false;
        for (const std::vector<Obj>* v : {&L.orbs, &L.pads, &L.dyn.objs})
            for (const Obj& o : *v) canReverse = canReverse || o.rev != 0;
        const auto sharedPast = [&](int rf) {
            return canReverse && rf >= 0 && (long long)rf <= (long long)t0;
        };
        int behind = 0, fromX = 0, fromRec = 0, xTracked = 0;
        for (size_t b = 0; b < g_autoTrig.size(); ++b) {
            AutoTrig& A = g_autoTrig[b];
            // recFire is the first ROW, which is 1-2 ticks after the move really
            // started (see recordLag). Everything here works in true ticks.
            int rf = -1;
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                if (L.dyn.autoAnchor[i] != (int)b || L.dyn.trigRecFire[i] < 0)
                    continue;
                // --anchornoop: its first recorded change is not this trigger's effect.
                if (i < L.dyn.anchorUnrec.size() && L.dyn.anchorUnrec[i])
                    continue;
                const int tr = L.dyn.trigRecFire[i] - L.dyn.autoLag[i];
                if (rf < 0 || tr < rf) rf = tr;
            }
            // [2026-08-22 r99] UNDER --trigraw, TRIGGERS AHEAD OF THE ANCHOR
            // ALSO TAKE THE RECORDING'S TICK FIRST. Across a rotated section
            // (2900) the world x does not advance while the camera x does not,
            // so "fire on the x crossing" is too early. Measured on lv22: the
            // model fires Toggle uid6439 (cx=15,765), which switches off group
            // 308, at t=12,627 (x crossing), but the live recording fires at
            // t=14,225 -- 1,598 ticks early. As a result the solid uid6332
            // that GD lands on at t=12,816 had vanished from the model
            // (census `m0/mini1/g0/gdg1/sp0.9/air` edvy -6.057).
            // --trigraw is only passed for section anchors of levels that have
            // a 2900 (quick_regress.rot_anchor_args), so lv1-21 are
            // bit-identical.
            if (A.cx > x0) {
                if ((g_trigRaw || sharedPast(rf)) && rf >= 0) {
                    A.fireT = rf;
                    A.fireX = A.lockLastX = A.cx;
                    if (g_dynDbg >= 0)
                        std::printf("anchortrig(ahead) uid=%d cx=%.1f rf=%d "
                                    "-> fireT=%d\n", A.uid, A.cx, rf, A.fireT);
                    continue;
                }
                // ...and `rf` only looks at the objects ANCHORED on this
                // trigger. An object driven by several controllers is anchored
                // on one of them, so a trigger that is only ever a PART of such
                // a superposition has no recorded tick at all and stays
                // unresolved -- and the superposition adds an unresolved part as
                // zero. SubZero 4002: the drop of group 87 (uid18255 at
                // x=31,899, (0,-600), duration 0) is a part of the saw uid16730,
                // whose anchor is another trigger; anchored past it in the
                // reversed section, the model kept that saw 600 px up and walked
                // through the kill GD makes at t=22,893.
                // The recording settles it: when an object's own rows show THIS
                // part's displacement already applied before the anchor, the row
                // that first shows it is the tick. Only for a single-part object
                // -- with two parts the rows are a sum and cannot be attributed.
                // ...and only for a move of DURATION 0. The row this looks for
                // is the one where the recording has reached the part's FULL
                // offset, which for a move that takes time is the tick it
                // ENDS: handing that tick to the closed form would replay the
                // whole ease from there and run the move late by its own
                // duration. An instant move begins and ends
                // on the same tick, so there the row is the answer.
                if (canReverse && A.fireT < 0) {
                    for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                        if (L.dyn.autoParts[i].size() != 1
                            || L.dyn.autoParts[i][0].trig != (int)b
                            || L.dyn.autoParts[i][0].dur > 0.0
                            || L.dyn.samples[i].size() < 2)
                            continue;
                        const Dynamics::AutoPart& p = L.dyn.autoParts[i][0];
                        const std::vector<DynSample>& sm = L.dyn.samples[i];
                        const double bx = sm[0].cx, by = sm[0].cy;
                        for (const DynSample& s : sm) {
                            if ((long long)s.t > t0) break;
                            if (std::fabs((s.cx - bx) - p.dx) > 1.0
                                || std::fabs((s.cy - by) - p.dy) > 1.0)
                                continue;
                            A.fireT = s.t;
                            A.fireX = A.lockLastX = A.cx;
                            ++fromRec;
                            // one line per resolution, always: the count alone
                            // cannot say which trigger took which object's word
                            std::printf("anchortrig: ahead trigger uid=%d cx=%.1f "
                                        "was unresolved -> fireT=%d from uid%d's "
                                        "recording (dur=%.2f d=(%.1f,%.1f))\n",
                                        A.uid, A.cx, A.fireT, L.dyn.objs[i].uid,
                                        p.dur, (double)p.dx, (double)p.dy);
                            break;
                        }
                        if (A.fireT >= 0) break;
                    }
                }
                continue;
            }
            // The crossing is a fact about THIS run's x, so derive it from x and
            // keep the recording's tick only when the two agree.
            //
            // The recording's tick is on the RECORDED run's timeline, which this
            // file already warns about for the forward case ("a bootstrap corpse
            // crossed x=30,375 at t=23,397 where the real run crosses at
            // t=21,690"). Behind the anchor it is worse, because a fireT in the
            // FUTURE reads as "not fired yet" and freezes the door at its first
            // sample. Measured 2026-08-09, lv20 uid15929 (id469, the block at
            // 28785,315 that moves -105): the live recording put its crossing at
            // t=20,928 while the run crosses x=28,545 at t=18,971 -- 1,957 ticks
            // of phase. The replay died at t=19,108 on that block sitting at its
            // STATIC cy=315, i.e. the model was flying into a door GD had opened
            // 137 ticks earlier. That is the whole x=28,867 wall.
            // [2026-08-21 r58] FLOOR, NOT ROUND. The crossing tick is "the
            // first tick at which x >= cx", so from x(t) = x0 - (t0-t)*dx:
            //   t >= t0 - (x0-cx)/dx  ->  crossing = t0 - floor((x0-cx)/dx)
            // llround steps one extra tick back for a fraction >= 0.5 and
            // fires 1 tick early. Measured on the lv20 t0=21,400 section:
            // uid18290 (cx=31,965, moves group 54 by +240) has rf=-1 (nothing
            // is anchored on this trigger) and falls back to estT.
            // (x0-cx)/dxA = 35.23/1.61426 = 21.82 -> llround 22 gives
            // estT=21,379, while the true effect tick is 21,380 (with a
            // t0=21,360 anchor it fires inside the sim and prints "crossed at
            // t=21379, effect t=21380"). floor 21 gives 21,380 and agrees.
            // The 1-tick phase is 0.74 px at EaseInOut's onset (f=0.237) --
            // the census family `m3/mini0/g0/gdg0/sp1.1/air` edy -0.743 was
            // entirely this, and r53, which pushes out onto the top face of
            // the moving solid uid18338, was pushing onto a face 0.744 px too
            // high.
            long long est = t0 - (long long)std::floor(
                (x0 - A.cx) / (dxA > 0.0 ? dxA : (double)kDxF)) + A.delay;
            // --xtrack: the crossing read off GD's own x, when the track reaches it.
            if (!g_xTrack.empty())
                for (const auto& r : g_xTrack)
                    if (r.second >= A.cx) {
                        est = (long long)r.first + A.delay;
                        ++xTracked;
                        break;
                    }
            const int estT = (int)std::max(0LL, est);
            // 60 ticks of slack: within that the recording IS this run's own
            // timeline (the live overlay comes from this plan's replays) and its
            // tick is exact, so keep the old no-op behaviour.
            // --trigraw removes this gate and always trusts the recording
            // (history at g_trigRaw's declaration: est assumes x advances
            // monotonically and lies behind a rotation maze).
            if (rf >= 0 && (g_trigRaw || sharedPast(rf)
                            || std::llabs((long long)rf - (long long)estT) <= 60)) {
                A.fireT = rf;
            } else {
                A.fireT = estT;
                if (rf >= 0) ++fromX;
            }
            // Behind the anchor the crossing x is not recorded anywhere, but it
            // is the trigger's own cx to within one tick of travel (the
            // crossing is the FIRST x at or past cx).
            A.fireX = A.lockLastX = A.cx;
            // --lockanchor [2026-09-19]: ...but for a lockToPlayerX trigger that
            // is not enough. lockLastX only moves while the replay is inside the
            // lock window, so an anchor past the window leaves fireX == lockLastX
            // and the object sits at its base: lv19 uid15908/15910 (trigger
            // uid15896, locked t=22,509..22,597) anchored at 22,600 came out at
            // cx 31,455 where GD holds 31,570.596, and the robot standing on them
            // fell. For an object moved in x by nothing but the lock, the
            // recording (this plan's own replays) holds the displacement
            // outright -- the same source the touch lock reads its lockOff from.
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                if (L.dyn.autoLockTrig[i] != (int)b
                    || L.dyn.samples[i].size() < 2)
                    continue;
                bool xFree = true;
                for (const auto& p : L.dyn.autoParts[i])
                    xFree = xFree && std::fabs(p.dx) <= 0.001f;
                if (!xFree) continue;
                const auto& sm = L.dyn.samples[i];
                const DynSample* at = &sm.front();
                for (const DynSample& s : sm)
                    if ((long long)s.t <= t0) at = &s;
                const double D = (double)at->cx - (double)sm.front().cx;
                A.fireX = x0 - D;
                A.lockLastX = x0;
                break;
            }
            if (g_dynDbg >= 0)
                std::printf("anchortrig uid=%d cx=%.1f rf=%d estT=%d -> fireT=%d "
                            "(x0=%.2f dxA=%.5f t0=%lld)\n",
                            A.uid, A.cx, rf, estT, A.fireT, x0, dxA, (long long)t0);
            ++behind;
        }
        if (behind || fromRec)
            std::printf("autotrig: %d behind the anchor (%d re-dated from x, "
                        "the recording disagreed by >60 ticks); %d ahead of it "
                        "resolved from a recording\n", behind, fromX, fromRec);
        if (!g_xTrack.empty())
            std::printf("autotrig: %d of the %d behind the anchor dated by GD's x (--xtrack, "
                        "%zu rows)\n", xTracked, behind, g_xTrack.size());
    }
    // --dyndbg <uid>: everything that decides where one moving object is placed.
    // Added 2026-08-09 chasing lv20's x=28,867 wall, where the model killed on
    // uid15929 at its STATIC cy=315 although the recording ends at cy=210.
    // Guessing between "not linked to its trigger", "fireT unresolved" and
    // "overlaid by a shorter timeline" cost more than printing all three.
    if (g_dynDbg >= 0) {
        bool found = false;
        for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
            if (L.dyn.objs[i].uid != g_dynDbg) continue;
            found = true;
            const auto& sm = L.dyn.samples[i];
            std::printf("dyndbg: uid=%d IN DYN  samples=%zu", g_dynDbg, sm.size());
            if (!sm.empty())
                std::printf("  first(t=%d cy=%.3f)  last(t=%d cy=%.3f)",
                            sm.front().t, sm.front().cy, sm.back().t, sm.back().cy);
            std::printf("\n  closed=%d  trigMask=0x%llx recFire=%d (lag %d -> "
                        "true %d; autoLag %d)  autoAnchor=%d autoD=(%.1f,%.1f) "
                        "autoDur=%.2f ease=%d/%.2f\n",
                        (int)L.dyn.autoClosed[i],
                        (unsigned long long)L.dyn.trigMask[i].word(0),
                        L.dyn.trigRecFire[i], L.dyn.recLag[i],
                        L.dyn.trigRecFire[i] >= 0
                            ? L.dyn.trigRecFire[i] - L.dyn.recLag[i] : -1,
                        L.dyn.autoLag[i],
                        L.dyn.autoAnchor[i], L.dyn.autoDx[i], L.dyn.autoDy[i],
                        L.dyn.autoDur[i], L.dyn.autoEase[i], L.dyn.autoErate[i]);
            const int aA = L.dyn.autoAnchor[i];
            if (aA >= 0)
                std::printf("  anchor trigger: uid=%d cx=%.1f fireT=%d delay=%d\n",
                            g_autoTrig[(size_t)aA].uid, g_autoTrig[(size_t)aA].cx,
                            g_autoTrig[(size_t)aA].fireT,
                            g_autoTrig[(size_t)aA].delay);
        }
        if (!found)
            std::printf("dyndbg: uid=%d is NOT in dyn (static grid)\n", g_dynDbg);
    }
    if (g_needUnseen) {
        // A box is "seen" when at least one of the objects it controls actually
        // moved in a recording. Read off Dynamics, which already joined the map
        // to the timeline.
        for (size_t b = 0; b < g_touch.size(); ++b) {
            bool seenIt = false;
            for (size_t i = 0; i < L.dyn.objs.size() && !seenIt; ++i)
                if ((L.dyn.trigMask[i] & touchBit(b)) && L.dyn.trigRecFire[i] >= 0)
                    seenIt = true;
            // ...and one the anchor already entered is settled, not unknown.
            if (!seenIt && !(init.trig & touchBit(b))) {
                // Unreachable from this anchor: the tail starts past the box.
                // Reported either way so the driver can tell a blocked anchor
                // from a physics wall; only --needtrig-skip drops it.
                const bool passed = g_touch[b].cx + g_touch[b].hw + 40.0 < x0;
                const bool skip = (g_needSkip & touchBit(b)).any();
                std::printf("needtrig: box=%zu cx=%.0f cy=%.0f hw=%.0f "
                            "passed=%d skipped=%d\n",
                            b, g_touch[b].cx, g_touch[b].cy, g_touch[b].hw,
                            passed ? 1 : 0, skip ? 1 : 0);
                if (skip) continue;
                g_needTrig |= touchBit(b);
                // Same facts, for a caller with no stdout to read (dp/progress.hpp)
                g_outcome.needTrigMask |= touchBit(b);
                if (passed) g_outcome.needTrigPassed |= touchBit(b);
            }
        }
    }
    // --needtrig-uid: the same requirement, the box named by its trigger's uid (this call's bit
    // numbering is its own window's). A box the anchor has already passed, or one outside the
    // window, is not required -- said so, never silently.
    for (const int u : g_needTrigUids) {
        int bit = -1;
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
            if (g_touch[b].uid == u) { bit = (int)b; break; }
        if (bit < 0) {
            std::printf("needtrig-uid: uid %d is not a box of this call's window - not required\n", u);
            continue;
        }
        const TouchTrig& T = g_touch[(size_t)bit];
        if (init.trig & touchBit(bit)) {
            std::printf("needtrig-uid: uid %d (box %d) is already entered at the anchor\n", u, bit);
            continue;
        }
        if (T.cx + T.hw + 40.0 < x0) {
            std::printf("needtrig-uid: uid %d (box %d, cx=%.0f) is behind the anchor x=%.0f - not "
                        "required\n", u, bit, T.cx, x0);
            continue;
        }
        g_needTrig |= touchBit(bit);
        std::printf("needtrig-uid: uid %d = box %d cx=%.0f cy=%.0f hw=%.0f - required\n", u, bit,
                    T.cx, T.cy, T.hw);
    }
    // After the level, because the queue joins to g_rotTrig by uid. Defaulted
    // from the objrects path the same way levelsettings is, so the harnesses do
    // not each have to learn a flag.
    if (rotQPath.empty()) rotQPath = rotQPathBeside(argv[1]);
    if (!rotQPath.empty() && !g_rotTrig.empty() && !loadRotQueue(rotQPath))
        std::printf("rotq: could not read %s\n", rotQPath.c_str());
    // The cursor is a 32-bit set: State::rotSpent marks a consumed entry by its
    // queue index, and both places that touch it stop at 32 -- step.hpp's
    // `if (idx < 32)` and buildRotQueue's channel mask. Past that an entry is
    // CONSUMED WITHOUT BEING RECORDED, so the popcount that picks the next one
    // points at an entry the walk already passed, silently and forever after.
    // Refuse the queue instead, and say why: this is the same rule --startrotq
    // follows a few lines down, where a uid that does not resolve is named
    // rather than dropped. lv22 -- the only level with a queue -- holds 30, so
    // this cannot fire today; it is here because 30 is two short of the limit
    // and nothing else would report crossing it.
    if (g_rotQ.size() > 32) {
        std::printf("rotq: %zu entries, but State::rotSpent is a 32-bit cursor "
                    "- entries 33+ would be consumed without being recorded. "
                    "Refusing the queue on this level.\n", g_rotQ.size());
        g_rotQueue = false;
        g_rotQ.clear();
    }
    // The queue in consumption order, handed back for a caller that derives its
    // own seed (progress.hpp rotQOrder). After the refusal above, so a refused
    // queue hands back nothing; printed nowhere, so no output changes.
    if (!g_rotQ.empty())
        for (int ch = 0; ch <= 15; ++ch)
            for (int k = g_rotQBeg[(size_t)ch]; k < g_rotQEnd[(size_t)ch]; ++k) {
                const RotQEntry& e = g_rotQ[(size_t)k];
                char b[192];
                std::snprintf(b, sizeof b, "%s%d,%d,%.4f,%.4f,%d,%d,%d,%d,%d",
                              g_outcome.rotQOrder.empty() ? "" : ";", ch, e.uid,
                              (double)e.px, (double)e.py, (int)e.swarm, (int)e.swch,
                              (int)e.chanOnly,
                              e.rotIdx >= 0 ? g_rotTrig[(size_t)e.rotIdx].gndDir : -1,
                              (int)e.id);
                g_outcome.rotQOrder += b;
            }
    // --rotqtoggle's masks (frames.hpp). For each queue entry and each pre-queue
    // trigger: the bits of the touch boxes whose Toggle names the object in its
    // chain (TouchTrig::ctl) and switches that group off (togOn 0) or on (1).
    // Built here because both tables exist by now: g_touch was loaded with the
    // level, the queue just above. Zeros with the flag off, so nothing reads
    // differently and nothing is printed.
    g_rotQOff.assign(g_rotQ.size(), 0);
    g_rotQOn.assign(g_rotQ.size(), 0);
    g_rotTrigOff.assign(g_rotTrig.size(), 0);
    g_rotTrigOn.assign(g_rotTrig.size(), 0);
    if (g_rotQToggle) {
        auto maskFor = [](int uid, int want) {
            uint32_t m = 0;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
                if (g_touch[b].togOn != want) continue;
                for (const TrigCtl& ct : g_touch[b].ctl)
                    if (ct.uid == uid) { m |= (uint32_t)touchBit((int)b).word(0); break; }
            }
            return m;
        };
        for (size_t k = 0; k < g_rotQ.size(); ++k) {
            g_rotQOff[k] = maskFor(g_rotQ[k].uid, 0);
            g_rotQOn[k] = maskFor(g_rotQ[k].uid, 1);
            if (g_rotQOff[k] | g_rotQOn[k])
                std::printf("rotqtoggle: queue uid %d off=0x%x on=0x%x\n",
                            g_rotQ[k].uid, g_rotQOff[k], g_rotQOn[k]);
        }
        for (size_t k = 0; k < g_rotTrig.size(); ++k) {
            g_rotTrigOff[k] = maskFor(g_rotTrig[k].uid, 0);
            g_rotTrigOn[k] = maskFor(g_rotTrig[k].uid, 1);
        }
    }
    // --startrotq: the queue's --spentrot. Applied HERE because the uids have
    // to be resolved against the built queue, and before init is used -- the
    // --replay path takes its copy at `State s = init` well below, and the
    // search takes its own at `cur.push_back(init)` below that.
    //
    // Every uid that does not resolve is NAMED. A seed that silently drops
    // entries is worse than no seed: it produces a state that looks anchored
    // and is not, which is the failure the flag exists to fix.
    if (g_startRotChan >= 0) {
        if (g_rotQ.empty()) {
            std::printf("startrotq: no rotation queue on this level - the seed "
                        "has nothing to bind to and is ignored\n");
            g_outcome.startRotHit = 0;
            g_outcome.startRotGiven = (int)g_startRotSpent.size();
            g_outcome.startRotMiss = "(no queue)";
        } else {
            init.rotChan = (uint8_t)g_startRotChan;
            init.rotRev = (uint16_t)g_startRotRev;
            init.rotSpent = 0;
            int hit = 0;
            std::string miss;
            for (int su : g_startRotSpent) {
                bool found = false;
                for (size_t q = 0; q < g_rotQ.size() && q < 32; ++q)
                    if (g_rotQ[q].uid == su) {
                        init.rotSpent |= (uint32_t)1 << q;
                        found = true;
                        ++hit;
                        break;
                    }
                if (!found) {
                    miss += miss.empty() ? "" : ",";
                    miss += std::to_string(su);
                }
            }
            std::printf("startrotq: chan=%d rev=0x%04x spent=%d/%zu of %zu "
                        "queued%s%s\n",
                        (int)init.rotChan, (unsigned)init.rotRev, hit,
                        g_startRotSpent.size(), g_rotQ.size(),
                        miss.empty() ? "" : "  NOT IN THE QUEUE: ",
                        miss.c_str());
            g_outcome.startRotHit = hit;
            g_outcome.startRotGiven = (int)g_startRotSpent.size();
            g_outcome.startRotMiss = miss;
        }
    }
    std::printf("level: %zu colliders, %zu portals, %zu pads, %zu orbs, "
                "%zu moving, maxX=%.0f\n",
                L.objs.size(), L.portals.size(), L.pads.size(), L.orbs.size(),
                L.dyn.size(), L.maxX);
    // GAMEPLAY ROTATION. Printed always so a level that has them says so --
    // the model's whole frame ("x is the clock") only holds inside one of these.
    if (!g_rotTrig.empty()) {
        std::printf("rotation: %zu gameplay-rotation objects (id 2900)\n",
                    g_rotTrig.size());
        for (const RotTrig& r : g_rotTrig)
            std::printf("  rot: (%.0f,%.0f) frame=%d travel=%s\n",
                        r.cx, r.cy, r.frame,
                        r.frame == 0 ? "+X" : r.frame == 1 ? "-Y"
                        : r.frame == 2 ? "-X" : "+Y");
    }
    // ...and the queue those objects sit in, if the dump was passed. Printed in
    // consumption order per channel, because the order IS the mechanism: GD
    // walks one channel from a cursor and stops at the first element the player
    // has not passed, so an element in the wrong place blocks everything behind
    // it.
    //
    // [2026-09-10] The queue IS read now -- step.hpp:361 and :426, under
    // `--rotqueue`. The line this replaces said nothing did, which was true when
    // the queue was built and stopped being true without the comment moving.
    // What the line is FOR has changed with it: it is no longer a display, it is
    // the arm's proof of life. A run that turns the queue on and does not print
    // this loaded no queue, and its result is "the queue never ran" rather than
    // "the queue changed nothing" -- the two readings a silent empty arm cannot
    // be told apart by.
    if (!g_rotQ.empty()) {
        std::printf("rotq: %zu queued objects in %d channels\n",
                    g_rotQ.size(), g_rotQChans);
        for (int ch = 0; ch <= 15; ++ch) {
            if (g_rotQEnd[(size_t)ch] <= g_rotQBeg[(size_t)ch]) continue;
            int d = 4;
            for (const RotQEntry& e : g_rotQ)
                if (e.swarm && e.swch == ch && e.rotIdx >= 0) {
                    d = rotQDirection(g_rotTrig[(size_t)e.rotIdx].rawRot,
                                      g_rotTrig[(size_t)e.rotIdx].flipX != 0);
                    break;
                }
            std::printf("  rotq ch=%d dir=%s n=%d\n", ch,
                        d == 1 ? "y+" : d == 2 ? "y-" : d == 3 ? "x-" : "x+",
                        g_rotQEnd[(size_t)ch] - g_rotQBeg[(size_t)ch]);
            for (int k = g_rotQBeg[(size_t)ch]; k < g_rotQEnd[(size_t)ch]; ++k) {
                const RotQEntry& e = g_rotQ[(size_t)k];
                std::printf("    [%d] uid=%d ord=%d (%.0f,%.0f) swarm=%d "
                            "swch=%d chanOnly=%d gnddir=%d rev=%d\n",
                            k - g_rotQBeg[(size_t)ch], e.uid, e.ord, e.px, e.py,
                            e.swarm, e.swch, e.chanOnly,
                            e.rotIdx >= 0 ? g_rotTrig[(size_t)e.rotIdx].gndDir : -1,
                            e.rotIdx >= 0
                                ? ((unsigned)(g_rotTrig[(size_t)e.rotIdx].gndDir - 2) < 2u)
                                : 0);
            }
        }
    }
    // How much of the moving geometry the closed form can actually stand in for
    // -- printed always, so a level whose doors it cannot describe says so
    // instead of quietly falling back.
    if (L.dyn.anyAuto) {
        size_t autoN = 0, closedN = 0, closedRec = 0, lockN = 0;
        for (size_t i = 0; i < L.dyn.size(); ++i) {
            if (L.dyn.autoAnchor[i] < 0) continue;
            ++autoN;
            if (L.dyn.autoLock[i] > 0.0) ++lockN;
            if (!L.dyn.autoClosed[i]) continue;
            ++closedN;
            if (L.dyn.trigRecFire[i] >= 0) ++closedRec;
        }
        std::printf("autotrig: %zu/%zu moved objects match the closed form "
                    "(%zu of them checked against a recording, %zu follow the "
                    "player)%s\n",
                    closedN, autoN, closedRec, lockN,
                    ", --trigclosed ON");
    }
    // A re-anchor taken while the player was RIDING A RAMP used to restart with
    // onSlope = 0. The ride is sticky (see stickToSlope), so without it the
    // model free-falls off every downhill ramp it re-anchors on -- measured on
    // lv16: anchored at t=6300 on a downhill the frontier fell away and died 40
    // ticks later at x=9,372, while GD rode the ramp for hundreds more.
    // The dump has no such column, so infer it from the geometry exactly the
    // way the ride does: grounded, and sitting on a ramp's resting height.
    // ...and the SECOND BODY needs exactly the same inference. It rides its own
    // ramps, it has its own onSlope2 / slopeM2 / slopeT2, and `--start` carries
    // none of them -- so an anchor taken while the dual's second half was mid-ride
    // restarted it at slopeT2 = 0 while p1 got the saturated 24, and the two halves
    // left the ramp with different launches.
    // Measured on lv16 t=7,816 (both bodies on mirrored m=+-0.5 ramps, jumping
    // together): GD gives an exact mirror, p1 +12.663 / p2 -12.663. The model had
    // p1 +12.663 to the digit and p2 -12.197 -- the slope-exit bonus scaled by
    // slopeRampFactor(14 + 1) instead of (24 + 1), because p2 had only been able to
    // count the 15 ticks since the anchor. That 0.466 is the whole of the section.
    auto rideAtAnchor = [&](uint8_t grounded, uint8_t flip, float y,
                            uint8_t& onSlope, float& slopeM, uint8_t& slopeT,
                            uint8_t& rideLanded) {
        if (!grounded) return;
        const double pH = init.mini ? kMiniHalf : kCubeHalf;
        for (const Obj& sp : L.slopes) {
            const double sx0 = sp.cx - sp.hw, sx1 = sp.cx + sp.hw;
            const double m = (sp.sy1 - sp.sy0) / (sx1 - sx0);
            if (m == 0.0) continue;
            // Which corner of the box touches the line is decided in the
            // player's OWN gravity frame, so a hanging body takes the offset on
            // the other side -- the same mirror the hang-side rules make about
            // everything else. Written as "m as this body sees it".
            // With the upright sign both ways, p2's seat test missed by exactly
            // 2*slopeXOffset: lv16 t=7,800, ceiling ramp uid3357 (m=-0.5,
            // sy0=570), GD has p2 at 538.580 and the unmirrored formula asks for
            // 542.120. Mirrored it lands on 538.580 to the digit, the same way
            // p1 lands on its own 451.420.
            const double mEff = flip ? -m : m;
            const double xr0 =
                x0 + (mEff > 0 ? slopeXOffset(m, pH) : -slopeXOffset(m, pH));
            if (xr0 < sx0 || xr0 > sx1 + 1.5) continue;
            const double top =
                sp.sy0 + m * (std::min(xr0, sx1) - sx0) + (flip ? -pH : pH);
            if (std::fabs((double)y - top) > 0.6) continue;
            onSlope = 1;
            slopeM = (float)m;
            // KNOWN GAP (same class as the missing snapObj above): the dump
            // carries no slope-ride age, so an anchor taken mid-ride guesses
            // SATURATED (24 = full exit impulse). Right whenever the real ride
            // is >= 0.1 s -- the long chained climbs where anchors actually
            // land -- and over-launches on a shorter one; the loop's own GD
            // replay catches that case.
            slopeT = 24;
            // ...and the ride counts as LANDED, for the same reason slopeT is
            // saturated here: `--start` does not carry it, and 0 is not the
            // neutral value -- it means "never landed", which suppresses the
            // exit launch and release (State::rideLanded). An anchor taken
            // mid-ride would then leave the ramp with no launch at all, and
            // every anchored section that starts on a ramp would diverge from
            // the whole-run replay of the same plan. Caught exactly that way:
            // quick_regress lv16 lost 2 sections (t=5,400 went 400 -> 1 ticks)
            // while the whole-run A/B was byte-identical on all 21 other levels.
            // 1 is the pre-change behaviour, so an unseeded anchor behaves as it
            // always did and the loop's own GD replay judges the rest.
            // ([[gd-unseeded-field-safe-when-zero-is-old-behaviour]] -- here 0
            // IS a lie, so the default has to be 1.)
            rideLanded = 1;
            break;
        }
    };
    rideAtAnchor(init.grounded, init.flip, init.y,
                 init.onSlope, init.slopeM, init.slopeT, init.rideLanded);
    if (init.dual)
        rideAtAnchor(init.grounded2, init.flip2, init.y2,
                     init.onSlope2, init.slopeM2, init.slopeT2,
                     init.rideLanded2);
    // ...and the same default for a ride that reached the anchor by any other
    // route. rideAtAnchor only reconstructs one for a GROUNDED body, but the
    // seat sets grounded only when the landing is not a flipped rider on a
    // ramp's top -- so a hanging ride arrives here with onSlope set and
    // grounded clear, and rideAtAnchor never sees it. Seeding only inside that
    // lambda left lv16 t=13,000 short (264 -> 117 ticks) with quick_regress
    // still red after the first attempt. An anchor cannot know whether the ride
    // landed, and 0 means "it did not", so every anchored ride takes the
    // pre-change behaviour and the loop's GD replay judges it.
    if (init.onSlope) init.rideLanded = 1;
    if (init.dual && init.onSlope2) init.rideLanded2 = 1;
    // --anchorride: p1's ride as GD had it (the hist payload's version 4), in place of
    // the guess above. GD keeps ONE clock per ride (+0x598, stamped on the air -> ramp
    // transition and kept across a chain, and across a floor ride handed straight to an
    // underside contact) and one bit (+0x9b0) that does not tell a floor ride from an
    // underside press. The model splits that into three counters, and exactly one of
    // them takes the age, by the side and by whether the ride has landed:
    //   underside (slopeUnder = 1)     ceilT = min(200, A+1), ceilM4 = round(|m|*4),
    //                                  ceilMode = the anchor's mode, no floor ride
    //   floor side, landed             onSlope = 1, slopeT = min(24, A), rideLanded = 1,
    //                                  slopeM = the ramp's m (as the ride stores it),
    //                                  slopeUid0 = slopeUidNow = the ramp
    //   floor side, not landed (seat)  seatT = min(24, A+1), no floor ride
    // A counts ticks from the contact tick, 0 there: the model's slopeT is 0 on that
    // tick and seatT / ceilT are 1. The payload's slopeAge is GD's clock difference,
    // which the recording point reads as 1 on the contact tick (cfg slopetrace), hence
    // kRideAgeLag.
    // slopeUid0 is GD's CURRENT ramp: the one the ride began on is not in the payload,
    // and the two differ only for an anchor past a seam of a chain.
    // Nothing is written off a ramp, for a value not said (-1), a uid that is not a
    // ramp in this level, a rotated frame (the model's line is the turned one), or a
    // floor ride on a wave (the model's wave never rides): the guess above stands.
    if (g_anchorRide && t0 > 0) {
        constexpr int kRideAgeLag = 1;
        auto histVal = [&](const char* name) {
            for (const auto& h : payloadHist) if (h.first == name) return h.second;
            return -1;
        };
        const int on = histVal("slopeOn"), under = histVal("slopeUnder");
        const int uid = histVal("slopeUid"), age = histVal("slopeAge");
        const int landed = histVal("slopeLanded");
        const Obj* ramp = nullptr;
        if (uid >= 0) {
            for (const Obj& sp : L.slopes)
                if (sp.uid == uid) { ramp = &sp; break; }
            if (!ramp)
                for (const Obj& o : L.dyn.objs)
                    if (o.slope && o.uid == uid) { ramp = &o; break; }
        }
        const double m = (ramp && ramp->hw > 0.0)
            ? (ramp->sy1 - ramp->sy0) / (2.0 * ramp->hw) : 0.0;
        const char* skip = nullptr;
        if (!g_ownsHist || histVersion < 4) skip = "no version-4 hist payload";
        else if (on != 1) skip = on == 0 ? "off a ramp (slopeOn=0)" : "slopeOn not said";
        else if (under < 0 || age < 0) skip = "slopeUnder / slopeAge not said";
        else if (!ramp) skip = "the uid is not a ramp in this level";
        else if (m == 0.0) skip = "the ramp is flat";
        else if (init.frame != 0) skip = "a rotated frame";
        else if (under == 0 && landed < 0) skip = "slopeLanded not said";
        else if (under == 0 && init.mode == 4) skip = "a floor ride on a wave";
        if (skip) {
            std::printf("anchorride: nothing seeded, %s (slopeOn=%d slopeUnder=%d "
                        "slopeUid=%d slopeAge=%d slopeLanded=%d)\n",
                        skip, on, under, uid, age, landed);
        } else {
            const int A = std::max(0, age - kRideAgeLag);
            const int wasOn = init.onSlope, wasT = init.slopeT;
            const char* regime = nullptr;
            if (under == 1) {
                regime = "underside";
                init.onSlope = 0; init.slopeT = 0; init.rideLanded = 0; init.slopeM = 0.f;
                init.seatT = 0;
                init.ceilT = (uint8_t)std::min(200, A + 1);
                init.ceilM4 = (uint8_t)std::min<long>(255, std::lround(std::fabs(m) * 4.0));
                init.ceilMode = init.mode;
            } else if (landed == 1) {
                regime = "floor-landed";
                init.onSlope = 1;
                init.slopeT = (uint8_t)std::min(24, A);
                init.rideLanded = 1;
                init.slopeM = (float)m;
                init.slopeUid0 = uid;
                init.slopeUidNow = uid;
                init.seatT = 0;
                init.ceilT = 0; init.ceilM4 = 0;
            } else {
                regime = "floor-seat";
                init.onSlope = 0; init.slopeT = 0; init.rideLanded = 0; init.slopeM = 0.f;
                init.seatT = (uint8_t)std::min(24, A + 1);
                init.ceilT = 0; init.ceilM4 = 0;
            }
            std::printf("anchorride: %s uid=%d m=%.4f ceilRamp=%d flip=%d mode=%d "
                        "slopeAge=%d A=%d slopeLanded=%d -> onSlope=%d slopeT=%d "
                        "rideLanded=%d slopeM=%.4f seatT=%d ceilT=%d ceilM4=%d ceilMode=%d "
                        "(the geometric guess had onSlope=%d slopeT=%d)\n",
                        regime, uid, m, slopeIsCeiling(ramp->slopeDir) ? 1 : 0,
                        (int)init.flip, (int)init.mode, age, A, landed,
                        (int)init.onSlope, (int)init.slopeT, (int)init.rideLanded,
                        (double)init.slopeM, (int)init.seatT, (int)init.ceilT,
                        (int)init.ceilM4, (int)init.ceilMode, wasOn, wasT);
        }
    }
    // --ridebox: an anchor INSIDE a riding arm box's ride. --start carries no 2866 arm and
    // the 1859's field 29 is usually not said, and under the ride rule (modifiers.hpp,
    // g_rideBox) a state that is not armed inside the ride stays so to its end. The loop
    // gives an anchored call the recording of the anchor's own attempt, so the box's
    // recorded place on the first tick after the anchor says whether THIS state rides with
    // it: overlapping there = the arm was fresh at the anchor. Frame 0 only (lv22's rides
    // are); a turned anchor is left unarmed and says so.
    if (t0 > 0) {
        const int t1 = (int)t0 + 1;
        const double half = playerHalf(init.mode, init.mini != 0);
        auto over = [&](double bx, double by, double hw, double hh) {
            return init.frame == 0 && std::fabs(x0World - bx) <= hw + half
                   && std::fabs((double)init.y - by) <= hh + half;
        };
        for (const FlipHeadBox& b : g_flipHeadBoxes) {
            if (rideStage(b, t1) != 1) continue;
            double bx, by;
            flipHeadAt(b, t1, bx, by);
            const bool on = over(bx, by, b.hw, b.hh);
            if (on) init.fgArm = (uint8_t)kArmTicks;
            std::printf("ridebox: anchor t=%lld inside fliphead uid %d's ride - %s "
                        "(box %.2f,%.2f, frame %d)\n", t0, b.uid,
                        on ? "riding with it, armed" : "not overlapping it, unarmed", bx, by,
                        (int)init.frame);
        }
        for (const ArmBox& b : g_armBoxes) {
            if (rideStage(b, t1) != 1) continue;
            double bx, by;
            armBoxAt(b, t1, bx, by);
            const bool on = over(bx, by, b.hw, b.hh);
            if (on && init.armT >= (uint8_t)kArmTicks) init.armT = 0;
            std::printf("ridebox: anchor t=%lld inside ceilarm uid %d's ride - %s "
                        "(box %.2f,%.2f, frame %d, armT %d)\n", t0, b.uid,
                        on ? "riding with it, armed" : "not overlapping it, unarmed", bx, by,
                        (int)init.frame, (int)init.armT);
        }
    }
    const double goalX = L.maxX + 60.0;
    g_goalX = goalX;   // --goalspare (speed.hpp)
    // ---- --coins: route through the level's coins as well as to its end -----
    //
    // Two things and no more. A coin is COLLECTED when the player's own rect
    // overlaps it (hazardHalfFor, which is GD's getObjectRect half and the one
    // the coincal rig measured: 15/9 for most modes, 13.5/8.1 spider, 5/3 wave),
    // and a state that leaves an uncollected coin BEHIND IT is dead. The second
    // half is what keeps this cheap: without it the frontier fills with lineages
    // that can no longer finish, and the collected set would have to stay in the
    // key for the whole level instead of just inside a coin's window.
    //
    // Nothing here is a heuristic about where to go. The search is not steered
    // towards a coin; the goal simply is not reached without it.
    const uint32_t coinAll = g_coinRoute && !L.coins.empty()
        ? (uint32_t)((1u << L.coins.size()) - 1u) : 0u;
    if (g_coinRoute) {
        if (L.coins.size() > 8) {
            std::printf("coins: %zu is more than the 8 the mask holds -- "
                        "routing disabled\n", L.coins.size());
        } else if (L.coins.empty()) {
            std::printf("coins: none in this level\n");
        } else {
            std::printf("coins: %zu to collect, mask 0x%x\n",
                        L.coins.size(), coinAll);
            for (size_t i = 0; i < L.coins.size(); ++i)
                std::printf("  coin %zu: (%.0f,%.0f) box %.1fx%.1f\n", i,
                            L.coins[i].cx, L.coins[i].cy,
                            L.coins[i].hw * 2, L.coins[i].hh * 2);
        }
    }
    const bool coinOn = coinAll != 0 && L.coins.size() <= 8;
    // --coinpick (g_coinPick): the state a cut or a PARTIAL emits -- the first one holding the most
    // coins. Without the flag, or without --coins, it is front() exactly as before.
    auto pickOf = [coinOn](const std::vector<State>& v) -> const State& {
        size_t best = 0;
        if (coinOn) {
            int most = -1;
            for (size_t i = 0; i < v.size(); ++i) {
                const int c = popCount32((uint32_t)v[i].coins);
                if (c > most) { most = c; best = i; }
            }
        }
        return v[best];
    };
    // ---- WHAT A COIN NEEDS COUNTED BEFORE IT CAN BE HAD --------------------
    // Four of the corpus' coins sit behind a counter, and the search had no
    // reason to feed one: the only signal was the miss prune, which fires at
    // the coin. lv21's ten pickups end at x=21,455 and the coin is at 21,813,
    // so a branch that skipped the FIRST pickup (x=19,079) still flew 2,759 px
    // -- filling the frontier under a cap of 2,000 with branches that were
    // already doomed -- before anything said so.
    //
    // Only ONE shape gives a sound prune, and it is the shape the corpus has.
    // A counter never decreases, so "can this branch still reach N" is exactly
    // `have + still ahead >= N`. That is a proof, not a heuristic. The other
    // two comparisons (n == C, n > C) cannot be made unreachable by running out
    // of feeders, so they get no prune here and are left to the miss test.
    //
    // WHICH WAY the trigger decides is read off the offset its chain gives the
    // coin, not assumed: if firing moves the coin further than the collect
    // bound, firing is what loses it, and the requirement is the negation of
    // the trigger's own test. lv21: uid 21783 is `item 1 < 10` and its chain
    // drops the coin 120 px against a bound of 25, so the coin needs ten.
    struct CoinCountReq {
        int item;      // which counter
        int need;      // ...has to reach this
        int coinIdx;   // the coin it decides (skip once that one is collected)
    };
    std::vector<CoinCountReq> coinReq;
    if (coinOn) {
        for (const TouchTrig& T : g_touch) {
            if (T.count < 0 || T.cmode != 2) continue;   // only `n < C` bites
            for (const TrigCtl& c : T.ctl) {
                int ci = -1;
                for (size_t i = 0; i < L.coins.size(); ++i)
                    if (L.coins[i].uid == c.uid) { ci = (int)i; break; }
                if (ci < 0) continue;
                const double reach = L.coins[(size_t)ci].hh + 15.0;
                if (std::fabs((double)c.dy) <= reach) continue;   // not decisive
                coinReq.push_back({T.item, T.count, ci});
                std::printf("coins: coin %d needs item %d to reach %d before it"
                            " (uid %d moves it %.0f px, reach %.0f)\n",
                            ci, T.item, T.count, c.uid, (double)c.dy, reach);
            }
        }
    }
    // ---- ...AND THE COIN ITS GROUP CAN BE SWITCHED OFF ---------------------
    // lv20's first coin is the other shape, and it has no counter at all --
    // the level holds no pickups. A Toggle at x=6,465 switches its group off on
    // the crossing, and a TOUCH-TRIGGERED one at (7,003, 405.5) switches it
    // back on; the coin is at x=15,631. So the only thing that could have saved
    // the branch is 8,628 px behind by the time the miss test speaks.
    //
    // Sound for the same reason as the counter, and gated on the same fact: x
    // only grows in frame 0, so once every switch-on box is behind the player
    // and none of them has fired, that coin can never come back. The test also
    // asks whether the coin is off RIGHT NOW (it is absent from the live rows
    // when its group is) -- without that, a coin that is on by default and
    // merely has an enabling Toggle somewhere would be pruned for never having
    // needed it.
    //
    // Only a Toggle that is the ROOT of its chain is seen here (togOn is read
    // off the root row). A switch-on nested inside a spawn chain would not be,
    // and the branch would keep the old behaviour -- late, not wrong.
    // ---- WHAT A FIRED CHAIN ADDS TO AN ITEM COUNTER ------------------------
    // The counter used to be "collectibles the player has physically touched"
    // (g_collect, which loadCollectibles fills from the rows with pickup=1).
    // On lv22's first coin that counter can never move: the gate is
    // `item 2 == 5`, and all three rows that give item 2 have pickup=0 --
    // they are Pickup triggers (1817) and a collectible (1816) that a TOUCH BOX
    // spawns, so nothing is ever touched. The gate could not fire, the coin was
    // never raised, and the model was not choosing the low lane over the high
    // one: IT HAD NO HIGH LANE. Closing the low band with --deadband made it go
    // lower still (under the coin, between the two spikes) rather than up.
    //
    // So count what the FIRED CHAINS give as well. The per-bit table is static:
    // g_itemGiver holds every row that names an item, touchable or not, and a
    // box's ctl list is what its chain reaches. No new state -- s.trig already
    // says which boxes have fired.
    //
    // MULTIPLICITY IS TAKEN FROM THE CTL LIST, and the game agrees on both
    // boxes it was asked about. A uid reached by two paths appears twice there,
    // and whether GD's counter moves once or twice for that is the game's
    // answer to give (cfg coins=1 prints `itemcnt:`). Dropped onto each box in
    // turn, lv22 says:
    //
    //   box uid 18090   weight 1   itemcnt v=1, still 1 fourteen ticks later
    //   box uid 2195    weight 2   itemcnt v=1 at the touch, v=2 at +25 ticks
    //
    // THE SECOND INCREMENT IS LATE, which a first reading missed: that box was
    // measured on a run that died nine ticks in and reported 1, and a walk of
    // the same chains written independently said 3 for the first box. Both were
    // wrong and the ctl list was right -- so the weights are measured, and the
    // number to distrust is a counter read from a run that ended too early.
    //
    // The line below prints what the model derives, so any level can be put
    // beside its `itemcnt:` the same way. On lv22 nine boxes feed item 2, 13 in
    // total, against a gate of 5. Under --itemsnoblock five boxes, 5 in total: the
    // other eight were the Collision Block uid 18154 on the ceiling every one of
    // those chains moves (g_itemsNoBlock) -- and GD's counter, taken on a pass
    // rather than a drop, agrees with the five.
    std::unordered_map<int, TouchMask> itemBits;   // item -> bits that give it
    std::vector<std::vector<std::pair<int, int>>> bitItemGive(g_touch.size());
    // ---- A TAP THAT COUNTS PRESSES -----------------------------------------
    // A fired box gives its items once, and s.trig says so. A counting TAP
    // (TouchTrig::tapCloseX) gives them on every press, which a bit cannot
    // hold: lv22's third coin opens at item 1 >= 6, six presses of the box at
    // x=15,525 before the Stop at x=15,765 (six confirmed in the game). So its
    // gift is taken out of the per-bit table and counted in State::taps
    // instead. One such tap per call -- State::taps is a single number; a
    // second one keeps the fire-once reading and says so.
    struct TapGive {
        TouchMask bit{};
        int item = 0, per = 0;
        double armX = 0.0, closeX = 1e18;
        // GD's terms for the two ends (TouchTrig::tapChan): the channel each
        // sits on and its world point.
        int armChan = 0, closeChan = 0;
        double armY = 0.0, closeY = 0.0;
        // --spawnroots: a Tap a box spawns opens when that box fires (not at armX), and one
        // with no Stop past it stays open (closeX stays at 1e18).
        TouchMask armBy{};
        // --tapclosebox: the Stop is a touch box and shuts the window on rect overlap
        // (TouchTrig::tapCloseTouch; closeHw/closeHh its half sizes, closeUid its uid).
        bool closeBox = false;
        double closeHw = 0.0, closeHh = 0.0;
        int closeUid = -1;
    };
    TapGive tapGive;   // bit 0 = none
    if (coinOn && !g_itemGiver.empty()) {
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            std::vector<std::pair<int, int>>& v = bitItemGive[b];
            for (const TrigCtl& c : g_touch[b].ctl) {
                const auto gi = g_itemGiver.find(c.uid);
                if (gi == g_itemGiver.end()) continue;
                // ...but NOT the ones the player has to run into. Those are in
                // g_collect and are counted from s.items, so counting them here
                // as well would credit a branch for a collectible it never
                // touched -- and credit it twice if it did. No corpus level
                // does this today (lv20 has no items at all and no chain on
                // lv21 reaches one of its eleven pickups, so this loop is empty
                // there), which is exactly why it would have gone unnoticed.
                bool isPickup = false;
                for (const auto& col : g_collect)
                    if (col.uid == c.uid) { isPickup = true; break; }
                if (isPickup) continue;
                bool seen = false;
                for (auto& p : v)
                    if (p.first == gi->second) { ++p.second; seen = true; break; }
                if (!seen) v.push_back({gi->second, 1});
            }
            if (g_touch[b].tap && v.size() == 1
                && (g_touch[b].tapCloseX < 1e17 || (g_touch[b].armBy))) {
                if (!tapGive.bit) {
                    tapGive.bit = touchBit((int)b);
                    tapGive.armBy = g_touch[b].armBy;
                    tapGive.item = v[0].first;
                    tapGive.per = v[0].second;
                    tapGive.armX = g_touch[b].cx;
                    tapGive.armY = g_touch[b].cy;
                    tapGive.armChan = g_touch[b].tapChan;
                    tapGive.closeX = g_touch[b].tapCloseX;
                    tapGive.closeY = g_touch[b].tapCloseY;
                    tapGive.closeChan = g_touch[b].tapCloseChan;
                    tapGive.closeBox = g_touch[b].tapCloseTouch
                                       && g_touch[b].tapCloseX < 1e17;
                    tapGive.closeHw = g_touch[b].tapCloseHw;
                    tapGive.closeHh = g_touch[b].tapCloseHh;
                    tapGive.closeUid = g_touch[b].tapCloseUid;
                    std::printf("items: tap %zu uid %d gives item %d x%d PER PRESS,"
                                " open from (%.0f,%.0f) on channel %d to the Stop at"
                                " (%.0f,%.0f) on channel %d\n", b, g_touch[b].uid,
                                tapGive.item, tapGive.per, tapGive.armX, tapGive.armY,
                                tapGive.armChan, tapGive.closeX, tapGive.closeY,
                                tapGive.closeChan);
                    if (tapGive.closeBox)
                        std::printf("items: --tapclosebox: the Stop uid %d is a touch box "
                                    "%.1fx%.1f, and shuts the window on the first tick the "
                                    "player's rect overlaps it\n", tapGive.closeUid,
                                    2.0 * tapGive.closeHw, 2.0 * tapGive.closeHh);
                    v.clear();
                    continue;
                }
                std::printf("items: tap %zu uid %d also counts presses -- only one"
                            " is held per call, so it gives once\n", b,
                            g_touch[b].uid);
            }
            for (const auto& p : v) itemBits[p.first] |= touchBit((int)b);
            if (!v.empty()) {
                std::printf("items: box %zu uid %d (x=%.0f) gives", b,
                            g_touch[b].uid, g_touch[b].cx);
                for (const auto& p : v)
                    std::printf(" item %d x%d", p.first, p.second);
                std::printf("\n");
            }
        }
    }
    // --tapclosebox: an anchor after the attempt entered the window's touch-box Stop starts with
    // the window shut (--touchentered uid:tick). Its box is behind the player by then, so no
    // overlap in the step can shut it, and a press past it would count where GD counts none.
    if (tapGive.closeBox && t0 > 0 && g_touchEnteredGiven) {
        const auto et = g_touchEnteredT.find(tapGive.closeUid);
        if (et != g_touchEnteredT.end() && et->second > 0 && (long long)et->second <= t0) {
            init.taps = 0x40;
            std::printf("items: --tapclosebox: the Stop uid %d was entered at t=%d <= t0, so "
                        "the window starts shut\n", tapGive.closeUid, et->second);
        }
    }
    // s.trig -> how much this state has fed `item`. The mask makes the common
    // case (no box for this item has fired) a single test.
    auto chainItems = [&itemBits, &bitItemGive](TouchMask trig, int item) -> int {
        const auto mi = itemBits.find(item);
        if (mi == itemBits.end() || !(trig & mi->second)) return 0;
        int add = 0;
        for (size_t b = 0; b < bitItemGive.size(); ++b) {
            if (!(trig & touchBit((int)b))) continue;
            for (const auto& p : bitItemGive[b])
                if (p.first == item) add += p.second;
        }
        return add;
    };
    // The Item Compare gates (cmode 3) that read the counting tap's item --
    // once one has fired, further presses change nothing -- and the coins each
    // gate switches on. A gated coin is live only in a state whose gate has
    // fired (coinLive below): the recording's own on/off belongs to the run
    // that recorded it, and lv22's recordings are of runs that never pressed
    // six times, so they hold the third coin off for every state. The chain
    // is read as switching the coin ON; lv22's reaches it through Toggle
    // uid17968 (activate 1), and no corpus gate reaches a coin any other way.
    TouchMask tapGateBits{};
    std::vector<TouchMask> coinEnable(L.coins.size(), TouchMask{});
    if (coinOn)
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            const TouchTrig& T = g_touch[b];
            if (T.count < 0 || T.cmode != 3) continue;
            if (tapGive.bit && T.item == tapGive.item) tapGateBits |= touchBit((int)b);
            for (const TrigCtl& c : T.ctl)
                for (size_t ci = 0; ci < L.coins.size(); ++ci)
                    if (L.coins[ci].uid == c.uid && !(coinEnable[ci] & touchBit((int)b))) {
                        coinEnable[ci] |= touchBit((int)b);
                        std::printf("coins: coin %zu is switched on by Item Compare uid %d"
                                    " (item %d at least %d)\n",
                                    ci, T.uid, T.item, T.count);
                    }
        }
    // ...and a coin a Toggle inside a touch box's chain switches on
    // (TouchTrig::togOnUids). The same shape and the same reason as the gate
    // above: without it the coin's on/off is whatever the RECORDING holds, and a
    // recording of a run that never entered the box holds it off for every
    // state -- so the model could not collect it from any branch. SubZero 4002's
    // first coin: the Spawn uid 500 at (1661,263) is touched, spawns group 679,
    // and its Toggle uid 505 switches the coin's group 678 on. Measured in the
    // game by teleporting onto each coin: the first (group off) awards nothing,
    // the second (group on) awards on contact.
    //
    // NOT for a Tap or a Count root: there the COUNTER decides, and the box's
    // own bit is set by the first press. lv22's third coin is reached by a
    // Toggle in the tap box uid 17958's chain and needs six presses, so reading
    // the Toggle there would switch it on five presses early -- the same trap
    // the walk already avoids by treating an Item Compare as a root of its own.
    // ...EXCEPT a Count a box spawns (--spawnroots, TouchTrig::armBy): its bit is set only
    // when its counter condition holds (itemGates), so the Toggle in its chain is exactly
    // when the coin comes on. SubZero 4003's third coin: box 6102 spawns the Count
    // `item 1 == 4`, whose Toggle switches the coin on.
    if (coinOn)
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
            if (g_touch[b].tap) continue;
            if (g_touch[b].count >= 0 && !(g_touch[b].armBy)) continue;
            for (const int u : g_touch[b].togOnUids)
                for (size_t ci = 0; ci < L.coins.size(); ++ci)
                    if (L.coins[ci].uid == u
                        && !(coinEnable[ci] & touchBit((int)b))) {
                        coinEnable[ci] |= touchBit((int)b);
                        std::printf("coins: coin %zu is switched on by the Toggle in"
                                    " touch box uid %d's chain\n", ci, g_touch[b].uid);
                    }
        }
    // ...and which of those coins can be collected in a TURNED frame too. The
    // collect test runs in frame 0 because a moving coin's position there is
    // read off the dyn rows; a coin nothing moves has one world position, and
    // toFrame turns it into any frame exactly as it turns the geometry.
    // lv22's third coin needs this: GD's own runs pass it at y=506 in frame 0,
    // 175 px under it, and come back up through x=16,090..16,131 in frame 3.
    // "Nothing moves it" = no touch chain gives it an offset, a duration, a
    // lock or a target-mode move; its group (537) is otherwise only toggled.
    std::vector<uint8_t> coinAnyFrame(L.coins.size(), 0);
    for (size_t ci = 0; ci < L.coins.size(); ++ci) {
        if (!coinEnable[ci]) continue;
        bool moved = false;
        for (size_t b = 0; b < g_touch.size() && !moved; ++b)
            for (const TrigCtl& c : g_touch[b].ctl)
                if (c.uid == L.coins[ci].uid
                    && (c.dx != 0.f || c.dy != 0.f || c.durTicks != 0.0
                        || c.lockTicks != 0.0 || c.lockYTicks != 0.0 || c.tmode != 0)) {
                    moved = true;
                    break;
                }
        if (moved) continue;
        coinAnyFrame[ci] = 1;
        std::printf("coins: coin %zu never moves, so it is collected in any frame\n", ci);
    }
    // Whether the counting tap is the only thing that feeds its item. Then a
    // window that shut with the gate unfired is final (the prune in the step).
    bool haveCmpGate = false;
    for (const TouchTrig& T : g_touch)
        if (T.count >= 0 && T.cmode == 3) haveCmpGate = true;
    bool tapSole = tapGive.bit.any() && !itemBits.count(tapGive.item);
    for (const auto& col : g_collect)
        if (col.item == tapGive.item) tapSole = false;
    // ---- WHEN THE COUNTING TAP'S WINDOW IS OPEN, IN GD'S TERMS -------------
    // A channel's direction is the one the queue print above names
    // (rotQDirection of the 2900 that switches TO it: 1 y+, 2 y-, 3 x-, else
    // x+), and a point is PASSED once the player's WORLD coordinate on that
    // axis has reached it in that direction -- the comparison step.hpp's queue
    // walk makes for the 2900s themselves. Passed now (the channel is active and
    // the player is there) or earlier, which the queue's own record keeps: an
    // entry of that channel lying at or beyond the point has been consumed.
    // That second reading is what lets an anchored call inside the window know
    // it is open without having seen it open.
    int chanDir[16];
    for (int ch = 0; ch < 16; ++ch) {
        chanDir[ch] = 4;
        for (const RotQEntry& e : g_rotQ)
            if (e.swarm && e.swch == ch && e.rotIdx >= 0) {
                chanDir[ch] = rotQDirection(g_rotTrig[(size_t)e.rotIdx].rawRot,
                                            g_rotTrig[(size_t)e.rotIdx].flipX != 0);
                break;
            }
    }
    const bool tapQueue = g_rotQueue && !g_rotQ.empty();
    auto reachedOn = [&chanDir](int ch, double wx, double wy, double px, double py) {
        switch (chanDir[ch & 15]) {
            case 1: return wy >= py;
            case 2: return wy <= py;
            case 3: return wx <= px;
            default: return wx >= px;
        }
    };
    // ...and WHERE SUCH A COIN IS PASSED FOR GOOD. The ordinary miss test is
    // withheld on a level that turns (coinPruneOk), and on lv22 rightly so for
    // the third coin in general: the maze brings the player back past it. But
    // the coin can only be had after its window, and past the shut point the
    // way on is the shut channel's run -- lv22's channel 10, x+, on which the
    // ship glides under the coin at y=615 and on (GD's own rows, t=14,250..
    // 14,580). So a coin lying AHEAD of the shut point in that channel's
    // direction is missed once the player is past its far edge on that same
    // channel. `gatedMask` is those coins; `tapMissX/Y` the far-edge point.
    uint8_t gatedMask = 0;
    std::vector<double> tapMissX(L.coins.size(), 0.0), tapMissY(L.coins.size(), 0.0);
    if (tapGive.bit)
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            if (!(coinEnable[ci] & tapGateBits) || ((g_coinSkip >> ci) & 1)) continue;
            const Obj& c = L.coins[ci];
            const int d = chanDir[tapGive.closeChan & 15];
            // the coin has to be ahead of the shut point on that run
            if (!reachedOn(tapGive.closeChan, c.cx, c.cy, tapGive.closeX, tapGive.closeY))
                continue;
            gatedMask |= (uint8_t)(1u << ci);
            const double m = 15.0;   // the largest half any mode has, as the mod's miss
            tapMissX[ci] = d == 3 ? c.cx - c.hw - m : (d == 1 || d == 2) ? c.cx : c.cx + c.hw + m;
            tapMissY[ci] = d == 1 ? c.cy + c.hh + m : d == 2 ? c.cy - c.hh - m : c.cy;
        }
    // ...and hand both points to the mod (Outcome::coinGates), so a flown
    // attempt that passes either -- the shut point with the count short, or the
    // coin's far edge without it -- ends there the way one that passes a missed
    // coin does. Only for a coin this run still wants.
    if (tapGive.bit && tapSole)
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            const TouchMask gates = coinEnable[ci] & tapGateBits;
            if (!gates || ((g_coinSkip >> ci) & 1)) continue;
            int need = 1 << 30;
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
                if (gates & touchBit((int)b)) need = std::min(need, g_touch[b].count);
            const bool miss = ((gatedMask >> ci) & 1) != 0;
            char e[224];
            std::snprintf(e, sizeof e, "%s%d,%d,%.3f,%.3f,%d,%d,%d,%d,%.3f,%.3f",
                          g_outcome.coinGates.empty() ? "" : ";", L.coins[ci].uid,
                          tapGive.closeChan, tapGive.closeX, tapGive.closeY,
                          chanDir[tapGive.closeChan & 15], tapGive.item, need,
                          miss ? 1 : 0, tapMissX[ci], tapMissY[ci]);
            g_outcome.coinGates += e;
        }
    auto passedOn = [&](const State& st, int frame, int ch, double wx, double wy,
                        double px, double py) {
        // No queue in this call: only channel 0 exists, and it is the plain
        // frame-0 crossing the fire-once tap already uses.
        if (!tapQueue) return ch == 0 && frame == 0 && wx > px;
        if ((st.rotChan & 15) == (ch & 15) && reachedOn(ch, wx, wy, px, py)) return true;
        const int beg = g_rotQBeg[(size_t)(ch & 15)], end = g_rotQEnd[(size_t)(ch & 15)];
        for (int k = beg; k < end && k < 32; ++k)
            if (((st.rotSpent >> k) & 1u)
                && reachedOn(ch, g_rotQ[(size_t)k].px, g_rotQ[(size_t)k].py, px, py))
                return true;
        return false;
    };
    // ---- THE ITEM GATES: one function for the search AND the walks ---------
    // The pickups, the Count triggers they and the fired chains feed, the Tap
    // triggers and the counting tap: every bit that the item counter or a press
    // sets, rather than the player's box entering one. markTouched (step.hpp)
    // skips all of them, and until this was a function only the search's step ran
    // this code, so a walk of the same plan (--replay, the witness resim) never
    // fired a Count gate. On lv22 the search planned under a ceiling `item 2 == 5`
    // had lifted while its own resim walked under the unlifted one (ledger
    // 2026-09-24 02:51). The search calls it where the code used to be; the walks
    // call it under --walkgates (g_walkGates).
    // `ks`: the state after the step and any rotation. `prevAction`: the input
    // the tick started with (a Tap fires on the press EDGE). `gframe`: the frame
    // the step started in; px/py/ph: the travel coordinates and hazard half in
    // it; `t`: the step's tick.
    auto itemGates = [&](State& ks, uint8_t prevAction, int input, const StepCtx& K,
                         int gframe, double px, double py, double ph, long long t) {
        // Frame 0 only, as everything here was before the coin collect learnt
        // the turned frames: the pickups are world rows, and the counters are
        // asked where they always were. The exception is an Item Compare gate
        // (cmode 3): it reads a count and nothing else, and lv22's third coin is
        // collected in frame 3, where an anchored call has to open it from GD's
        // own counter (--itembase) or never see the coin at all.
        if ((gframe == 0
             && (!g_collect.empty() || !itemBits.empty() || tapGive.bit))
            || (gframe != 0 && haveCmpGate)) {
            for (size_t ii = 0; gframe == 0 && ii < g_collect.size() && ii < 16; ++ii) {
                const uint16_t ibit = (uint16_t)(1u << ii);
                if (ks.items & ibit) continue;
                const Collectible& C = g_collect[ii];
                if (std::fabs(px - C.cx) <= C.hw + ph - kCoinMargin
                    && std::fabs(py - C.cy) <= C.hh + ph - kCoinMargin) {
                    // --coindbg: and the same line for the pickups. GD prints
                    // `itemcnt:` per increment, so a replay of one plan through
                    // both sides says whether the two take the SAME ones --
                    // which is what an --itembase of 1 against a model that
                    // needs ten turns on.
                    if (g_coinDbgT >= 0 && !g_itemSaid[ii]) {
                        g_itemSaid[ii] = 1;
                        std::printf("itemcollect: t=%lld item=%d uid=%d"
                                    " player=(%.3f,%.3f) obj=(%.3f,%.3f)"
                                    " |dx|=%.3f |dy|=%.3f bound=(%.3f,%.3f)"
                                    " mode=%d mini=%d\n",
                                    (long long)t, C.item, C.uid, px, py,
                                    C.cx, C.cy, std::fabs(px - C.cx),
                                    std::fabs(py - C.cy),
                                    C.hw + ph - kCoinMargin,
                                    C.hh + ph - kCoinMargin,
                                    (int)ks.mode, (int)ks.mini);
                    }
                    ks.items |= ibit;
                }
            }
            if (K.trigs)
                for (const auto& tb : *K.trigs) {
                    const TouchTrig* T = tb.first;
                    if (T->count < 0 || (ks.trig & tb.second)) continue;
                    if (T->tap) continue;   // fired below, by the press
                    // --spawnroots: a Count a box spawns listens only once that box fired.
                    if (T->armBy && !(ks.trig & T->armBy)) continue;
                    if (gframe != 0 && T->cmode != 3) continue;
                    int n = 0;
                    for (const auto& ib : g_itemBase)
                        if (ib.first == T->item) n += ib.second;
                    const int nBase = n;   // before anything this run gave
                    for (size_t ii = 0; ii < g_collect.size() && ii < 16; ++ii)
                        if ((ks.items & (uint16_t)(1u << ii))
                            && g_collect[ii].item == T->item)
                            ++n;
                    // ...and what the boxes this state has already fired gave.
                    // Without this lv22's `item 2 == 5` is unreachable: none of
                    // its givers is a pickup.
                    n += chainItems(ks.trig, T->item);
                    // ...and the counting tap's presses (tapGive).
                    if (tapGive.bit && T->item == tapGive.item)
                        n += (int)(ks.taps & 0x3f) * tapGive.per;
                    // GD's own order: 0 equals, 1 larger, 2 smaller. Measured on
                    // lv21, where the third coin's gate is a pair -- uid21783
                    // "item 1 SMALLER than 10" drops the coin out of reach,
                    // uid21781 "EQUALS 10" keeps it -- which is what "collect
                    // ten and it appears" is made of.
                    // EQUALS IS "THE COUNTER PASSED THROUGH C", not "it is C on
                    // this tick". Every increment the model counts is a single +1
                    // in the game -- a pickup, or one Pickup trigger firing -- so
                    // the game's counter visits every integer on the way up, and
                    // an equals gate fires on the visit. The model sums a box's
                    // whole weight on the tick the box fires, so it can skip:
                    // lv22's four yellow blocks weighed 2 each and the count went
                    // 2,4,6,8,9 where the game's goes 1,2,3,4,5. (The second of
                    // those two was a Collision Block's block ID -- --itemsnoblock,
                    // g_itemsNoBlock -- and the "second increment 25 ticks later"
                    // it was taken for is the next box (measured 2026-09-24).)
                    // With the old `n == C` the gate `item 2 == 5` could never
                    // fire even with every feeder forced.
                    //
                    // So with unit steps from nBase, `n == C` happens at some tick
                    // iff nBase <= C <= n. Where increments really do land one per
                    // tick (lv21's eleven pickups, collected one at a time) this
                    // fires on the same tick as before; it only differs where the
                    // old test stepped over C.
                    // cmode 3 is an Item Compare's "at least", asked on every
                    // press, so it simply reads the count.
                    const bool hit = T->cmode == 3 ? (n >= T->count)
                                   : T->cmode == 1 ? (n > T->count)
                                   : T->cmode == 2 ? (n < T->count)
                                   : (nBase <= T->count && T->count <= n);
                    // --coindbg: the gate itself, once per distinct n, so "the
                    // coin never moved" can be split into "the gate was never
                    // asked" and "it was asked and said no". Without this the
                    // only witness is the coin's live row, which cannot tell the
                    // two apart.
                    if (g_coinDbgT >= 0 && T->count >= 0 && !T->tap) {
                        const int bi = touchBitIndex(tb.second);
                        const int key = std::min(n, 60) + 1;
                        if (bi >= 0 && bi < kTouchBits
                            && g_countSaid[bi].load() < key) {
                            g_countSaid[bi].store(key);
                            std::printf("countgate: t=%lld bit=%d uid=%d id=%d "
                                        "item=%d cmode=%d need=%d n=%d base=%d "
                                        "px=%.1f hit=%d\n",
                                        (long long)t, bi, T->uid, T->id,
                                        T->item, T->cmode, T->count, n, nBase,
                                        px, hit ? 1 : 0);
                        }
                    }
                    if (!hit) continue;
                    // An INSTANT Count (1811) asks the question once, as the
                    // player crosses it; a Count (1611) is a standing listener.
                    // Without the crossing test the smaller-than ones fire at t=0
                    // for every state (0 < 10 is true from the start) and lv21's
                    // coin drops before the level has begun.
                    if (T->id == 1811 && px < T->cx) continue;
                    ks.trig |= tb.second;
                    ks.trigT = (int32_t)t;
                    const int b = touchBitIndex(tb.second);
                    if (g_touchFireT[b] < 0) g_touchFireT[b] = (int)t;
                    ks.fireB[b] = (uint16_t)t;
                }
        }
        // ---- the TAP triggers (1595) ----------------------------------------
        // Armed by crossing, fired by a press -- measured on the rig
        // calib_coingate5: a tap before the trigger's x does nothing, a tap past
        // it switches its target group. The press EDGE is what GD sees, so a
        // state already holding the button does not fire one by continuing to
        // hold.
        if (K.trigs && input && !prevAction && gframe == 0)
            for (const auto& tb : *K.trigs) {
                const TouchTrig* T = tb.first;
                if (!T->tap || (ks.trig & tb.second)) continue;
                // --spawnroots: a Tap a box spawns is armed by that box, not by its x.
                if (T->armBy ? !(ks.trig & T->armBy) : (px <= T->cx)) continue;
                ks.trig |= tb.second;
                ks.trigT = (int32_t)t;
                const int b = touchBitIndex(tb.second);
                if (g_touchFireT[b] < 0) g_touchFireT[b] = (int)t;
                ks.fireB[b] = (uint16_t)t;
            }
        // ...and the COUNTING tap (tapGive): every press edge while its window is
        // open adds one. State::taps holds the count in its low six bits, 0x80
        // once the window has opened and 0x40 once it has shut (see passedOn for
        // both ends). Read from tapGive rather than K.trigs, which drops a box
        // 40 px behind the player. The count is dropped once it cannot matter
        // (window shut, or a gate on its item already fired), so it splits the
        // key only inside the window. Any frame: lv22's window opens travelling
        // -x.
        if (tapGive.bit) {
            if (ks.trig & tapGateBits) {
                ks.taps = 0;
            } else {
                double wx = 0.0, wy = 0.0;
                fromFrame(gframe, (double)ks.xAbs, (double)ks.y, wx, wy);
                uint8_t tp = ks.taps;
                const bool armed =
                    (tp & 0x80) != 0
                    || (tapGive.armBy ? (bool)(ks.trig & tapGive.armBy)
                                      : passedOn(ks, gframe, tapGive.armChan, wx, wy,
                                                 tapGive.armX, tapGive.armY));
                // A tap a box arms (--spawnroots) closes where the player passes the Stop
                // in the frame it is travelling -- not through passedOn's rotation queue,
                // which reads a queue entry spent EARLIER in the run past the Stop's x as
                // "passed" and shut SubZero 4003's window the tick it opened.
                // --tapclosebox (g_tapCloseBox): a touch-box Stop shuts it where GD fires it,
                // on the first tick the player's rect overlaps the Stop's (edges inclusive, as
                // collisionCheckObjects tests). The player's rect and the pre-press y are
                // markTouched's: a spider that warps on this tick was tested by the collision
                // pass at the y it left (g_preBtnY, p1's only -- not read for a dual state,
                // whose second step may have written it). Frame 0 only, as the x test beside it.
                bool boxShut = false;
                if (tapGive.closeBox && armed && gframe == 0) {
                    const double half = playerHalf(ks.mode, ks.mini != 0);
                    const double preY = (!ks.dual && g_preBtnSet) ? g_preBtnY : wy;
                    boxShut = std::fabs(wx - tapGive.closeX) <= tapGive.closeHw + half
                              && (std::fabs(wy - tapGive.closeY) <= tapGive.closeHh + half
                                  || std::fabs(preY - tapGive.closeY) <= tapGive.closeHh + half);
                }
                const bool shut =
                    (tp & 0x40) != 0 || boxShut
                    || (!tapGive.closeBox && armed && tapGive.closeX < 1e17
                        && (tapGive.armBy ? (gframe == 0 && wx > tapGive.closeX)
                                          : passedOn(ks, gframe, tapGive.closeChan, wx, wy,
                                                     tapGive.closeX, tapGive.closeY)));
                if (shut) {
                    tp = 0x40;
                } else if (armed) {
                    tp |= 0x80;
                    if (input && !prevAction && (tp & 0x3f) < 0x3f) ++tp;
                }
                ks.taps = tp;
            }
        }
    };
    // ---- CAN "THE PLAYER IS PAST IT" BE FINAL FOR THIS COIN? ---------------
    // Both prunes below -- the switch-on gate and the miss test -- say "behind
    // the player, so gone". That rests on the player never coming back, which
    // used to be asked of the LEVEL: no gameplay rotation anywhere, or neither
    // prune runs. lv22 turns the frame, so on the one level whose coins are
    // still missing, nothing pruned and a branch that skipped a coin flew to the
    // end for free. The level's own log says what that costs: fourteen runs
    // completed it with 0/3.
    //
    // It is a per-COIN question, and a static one. Collecting needs frame 0 (the
    // live coin rows are built only there -- in a turned frame xAbs is a
    // different axis), and inside frame 0 x only grows. So a coin is out of
    // reach for good once every LATER return to frame 0 happens further along
    // than the coin. The returns are known before the search starts: a 2900
    // carries the frame it SETS and its world position, and it is one-shot, so
    // the chain is a fixed sequence. Take the ones at or after the point the
    // coin is passed and ask for the smallest.
    //
    // The index is taken crudely -- the first rotation in chain order sitting
    // past the coin -- which can only pull in rotations that fire EARLIER than
    // the coin is passed. That makes the minimum smaller and the verdict more
    // conservative, never less.
    //
    // On lv22 this separates the three coins, and its "no" is independently
    // right: a recorded run comes back past c3 for 2,840 ticks and never returns
    // past c1 or c2, which is exactly what the rule says.
    //
    //   c1 (3,919)   smallest later return 6,875    prunable
    //   c2 (10,513)  smallest later return 15,015   prunable
    //   c3 (16,097)  smallest later return 15,015   NOT prunable
    // ...and a ROTATION is not the only way back. A ring or a pad carrying
    // property 117 (Obj::rev) turns the player round, so a coin with one of
    // those anywhere past it can still be taken on the way back, and "passing
    // it is final" is simply false. SubZero 4002: the first coin sits at
    // x=1,787 with reversing rings later in the level, and with the prune on
    // every branch that ran past it died -- the coin run gave up at x=1,911
    // (5%) while the same level clears without coins.
    const auto revPast = [&](double cx) {
        for (const std::vector<Obj>* v : {&L.orbs, &L.pads, &L.dyn.objs})
            for (const Obj& o : *v)
                if (o.rev != 0 && o.cx > cx) return true;
        return false;
    };
    std::vector<uint8_t> coinPruneOk(L.coins.size(), 0);
    if (coinOn) {
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            const double past = L.coins[ci].cx + L.coins[ci].hw + 40.0;
            if (revPast(L.coins[ci].cx)) {
                std::printf("coins: coin %zu at x=%.0f -- something past it turns "
                            "the player round, so passing it says nothing (no "
                            "miss prune)\n", ci, L.coins[ci].cx);
                // ...and the mod is told, so its own attempt cut agrees (Outcome).
                g_outcome.coinNoPrune += (g_outcome.coinNoPrune.empty() ? "" : ";")
                                         + std::to_string(L.coins[ci].uid);
                continue;
            }
            if (g_rotTrig.empty()) { coinPruneOk[ci] = 1; continue; }
            size_t idx = g_rotTrig.size();
            for (size_t j = 0; j < g_rotTrig.size(); ++j)
                if (g_rotTrig[j].cx > L.coins[ci].cx) { idx = j; break; }
            double minRe = 1e18;
            for (size_t j = idx; j < g_rotTrig.size(); ++j)
                if (g_rotTrig[j].frame == 0)
                    minRe = std::min(minRe, g_rotTrig[j].cx);
            coinPruneOk[ci] = (minRe > past) ? (uint8_t)1 : (uint8_t)0;
            if (!coinPruneOk[ci])
                g_outcome.coinNoPrune += (g_outcome.coinNoPrune.empty() ? "" : ";")
                                         + std::to_string(L.coins[ci].uid);
            if (!g_rotTrig.empty())
                std::printf("coins: coin %zu at x=%.0f -- the frame returns to 0"
                            " next at x=%.0f, %s\n", ci, L.coins[ci].cx,
                            minRe > 1e17 ? -1.0 : minRe,
                            coinPruneOk[ci] ? "so passing it is final"
                                            : "which is BEHIND it, so passing it "
                                              "says nothing (no miss prune)");
        }
    }
    struct CoinOnGate {
        int coinIdx;
        // THE MASK'S OWN TYPE. This was uint32_t while s.trig, which it is
        // tested against, is a TouchMask: at a width of 64 the `1u << b` that
        // filled it is undefined for b >= 32 (and wraps mod 32 on x86), the
        // store truncates, and the test can only ever match the low half. The
        // gate sets dead = true, so a wrong bit KILLS BRANCHES -- it is the
        // expensive direction of that mistake, not the harmless one.
        std::vector<std::pair<TouchMask, double>> ons;  // bit, x past which it is gone
    };
    std::vector<CoinOnGate> coinGate;
    // Same gate as the miss test below, and for the same reason -- both rest on
    // x growing for ever, now asked per coin.
    if (coinOn) {
        for (size_t ci = 0; ci < L.coins.size(); ++ci) {
            if (!coinPruneOk[ci]) continue;
            CoinOnGate g{(int)ci, {}};
            for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b) {
                if (g_touch[b].togOn != 1) continue;
                bool hits = false;
                for (const TrigCtl& c : g_touch[b].ctl)
                    if (c.uid == L.coins[ci].uid) { hits = true; break; }
                if (!hits) continue;
                g.ons.push_back({touchBit((int)b),
                                 g_touch[b].cx + g_touch[b].hw + 15.0});
            }
            if (g.ons.empty()) continue;
            coinGate.push_back(std::move(g));
            std::printf("coins: coin %zu is switched on by %zu box(es), the last"
                        " at x=%.0f\n", ci, coinGate.back().ons.size(),
                        coinGate.back().ons.back().second);
        }
    }
    // ...and whether a coin the player has passed can be called missed. The
    // answer is coinPruneOk above, per coin. Say so once, with the count, so a
    // level where nothing prunes reads differently from one where the flag is
    // off: "0 of 3" is a verdict, a missing line is not.
    if (coinOn && !g_rotTrig.empty()) {
        size_t n = 0;
        for (uint8_t ok : coinPruneOk) n += ok ? 1 : 0;
        std::printf("coins: the level turns the frame -- %zu of %zu coins can "
                    "still be called missed once passed\n", n, coinPruneOk.size());
    }
    // 120 px = 4 blocks of slack above the highest surface, so a legitimate
    // arc over the top of the level is still allowed
    g_yBound = std::max(700.0, L.maxY + 120.0);
    // A turned frame's `y` is a world X (or a negated one), so the bound has to
    // cover the level's whole x extent in both signs.
    g_yBoundTurned = std::max(L.maxX, L.maxY) + 400.0;
    std::printf("  yBound=%.0f (level maxY=%.0f)\n", g_yBound, L.maxY);
    {   // suffix max of surface tops per 30 px bucket (see g_topAhead)
        double lo = 1e18, hi = -1e18;
        auto scan = [&](const std::vector<Obj>& v) {
            for (const Obj& o : v) {
                lo = std::min(lo, o.cx - o.hw);
                hi = std::max(hi, o.cx + o.hw);
            }
        };
        scan(L.objs); scan(L.pads); scan(L.orbs); scan(L.portals);
        if (lo < hi) {
            g_bucketX0 = lo;
            const size_t n = (size_t)((hi - lo) / 30.0) + 2;
            g_topAhead.assign(n, -1e18);
            auto put = [&](const std::vector<Obj>& v) {
                for (const Obj& o : v) {
                    long long i = (long long)((o.cx - lo) / 30.0);
                    if (i < 0) i = 0;
                    if (i >= (long long)n) i = (long long)n - 1;
                    g_topAhead[(size_t)i] =
                        std::max(g_topAhead[(size_t)i], o.cy + o.hh);
                }
            };
            put(L.objs); put(L.pads); put(L.orbs); put(L.portals);
            for (size_t i = n - 1; i-- > 0;)
                g_topAhead[i] = std::max(g_topAhead[i], g_topAhead[i + 1]);
        }
    }
    // The tick budget is the count "from x=0 to goalX at the base speed". A
    // LEVEL WITH SLOW SECTIONS EXCEEDS IT: lv22 has sections where x barely
    // advances (rotation / reverse), so for a t=18,600 anchor tEnd came out
    // as 18,602 and both the replay and the DP ENDED AFTER 1 TICK (SURVIVED
    // to t=18601 / maxAlive=1). The late-game wall that looked like a
    // coordinate-system problem was this. Starting from an anchor, allow at
    // least 8,000 ticks beyond it (max horizon 6,000 + slack). A run from the
    // start (t0=0) is as before.
    long long tEnd = (long long)std::ceil(goalX / kDx) + 2;
    if (t0 > 0) tEnd = std::max(tEnd, t0 + 8000);

    const gdapprox::ShipParams SP = gdapprox::ShipParams::normal();
    const gdapprox::ShipParams SPmini = gdapprox::ShipParams::mini();
    const gdapprox::UfoParams UP = gdapprox::UfoParams::normal();
    const gdapprox::UfoParams UPmini = gdapprox::UfoParams::mini();

    std::vector<State> cur, nxt;
    std::vector<Node> arena;
    arena.push_back(Node{0});
    // The mode of the state each arena node was created for, parallel to `arena` and ONLY
    // filled when the checkpoint channel has a subscriber (dp/progress.hpp). A checkpoint's
    // `input=` edges have to carry the plan writer's per-mode latency, and the writer reads
    // that off `modeAt`, which does not exist until the witness resim runs at the end of the
    // call. The lineage's own modes are the same sequence and they are available now.
    // Costs one byte per node while a subscriber is listening and nothing at all otherwise --
    // which is why it is a side vector and not three spare bits of Node: Node's parent field is
    // 31 bits wide and the arena has been measured at 147.9M nodes.
    const bool checkWanted = g_check.enabled.load(std::memory_order_relaxed);
    std::vector<uint8_t> arenaMode;
    if (checkWanted) arenaMode.push_back(0);   // the root, which no state owns
    // This call's checkpoints, not the previous call's -- and cleared again on the way OUT, so a
    // finished call leaves nothing behind that still looks live, and a judgement that arrives for
    // it afterwards is refused (dp/progress.hpp SearchCheckpoints::pass).
    struct CheckGuard {
        bool on;
        ~CheckGuard() { if (on) g_check.reset(); }
    } checkGuard{checkWanted};
    if (checkWanted) g_check.reset();
    // NOTE: init keeps being amended below (band seed, anchor dx). The root
    // state is pushed AFTER the last amendment -- it used to be pushed here,
    // which silently dropped the seeded band and the anchor dx from the
    // whole SEARCH while the witness resim (State s = init, after the
    // amendments) kept them. Every re-anchored tail searched with NO
    // invisible ceiling: found via the lineage-vs-resim fork at lv16
    // t=14,770, where the resim clamped to the band (390/690) the search
    // never had (0/1e9), and GD then died on the plan at x=23,000.

    // cell -> hi/lo representatives (indices into nxt). Keep BOTH vy extremes
    // per cell: first-wins kept the slowest lineage and strangled climbs
    // (measured: ship climb rate collapsed to ~0.02 vy/tick); a single
    // extreme-|vy| slot instead discarded dive-recovery lineages.
    struct Slots { int hi = -1; int lo = -1; };
    // Serial and sharded dedupe share full equality and explicit occupancy.
    using CellMap = SearchKeyMap<Slots>;
    CellMap seen;
    // ---- parallel dedupe (phase 2p) -------------------------------------
    // The serial dedupe was the measured ceiling of the whole DP (1 thread
    // 22.5 s -> 8 threads 8.0 s -> 16 threads 8.9 s on lv16's cap-40,000
    // layers: the stepping scales, the dedupe does not). It parallelises by
    // KEY: children of one key all land in one shard (keyed by a hash of the
    // dedupe key), each shard replays emit()'s exact hi/lo hysteresis over
    // its keys in ordinal order, and a serial merge then materialises `nxt`
    // in the SAME slot order the serial loop would have produced -- the
    // emitted plan stays bit-identical (checked against --old-slope-era runs
    // and asserted by the regression). Only the arena's node COUNT differs:
    // the serial loop pushed a node per accepted improvement, this pushes one
    // per final representative, so the arena is strictly smaller and parent
    // CHAINS (the only thing read back) are unchanged.
    struct KeyRec {
        uint32_t aOrd;          // ordinal that allocated the key's first slot
        uint32_t bOrd;          // ordinal of the first hi/lo split (kNone: none)
        uint32_t hiIdx, loIdx;  // child index of the current hi / lo rep
        float hiVy, loVy;
        uint8_t bWasHi;
    };
    struct ShardMap : SearchKeyMap<uint32_t> {
        std::vector<KeyRec> recs;
        uint32_t goalOrd = kNone;
        uint16_t goalTight = 0xffff;
        // Clear representatives together with the group's exact-key table.
        void clear() {
            SearchKeyMap<uint32_t>::clear();
            recs.clear();
            goalOrd = kNone;
            goalTight = 0xffff;
        }
    };
    std::vector<ShardMap> shards;
    std::vector<SearchKey> kidKeys;  // worker-built canonical cell identities
    std::vector<uint8_t> kidFlag;   // 0 = not expanded, 1 = dead, 2 = alive
    // per-ordinal slot events, written race-free (one shard owns each key):
    // evKind[i] = 0 none / 1+shard; evRec[i] = record index within that shard.
    // An ordinal is an 'a' event iff recs[evRec[i]].aOrd == i.
    // Cleared by the merge scan itself (every set entry is visited).
    std::vector<uint8_t> evKind;
    std::vector<uint32_t> evRec;
    // One slice set per gameplay frame (see RotTrig). Frame 0 is the level as
    // loaded; the others are built on first use. The cursors are monotone in
    // that frame's travel coordinate, so a group whose window starts BEHIND
    // where the cursor already is has to rewind -- states in one frame are no
    // longer guaranteed to advance together once a turn splits them.
    struct FrameSlices {
        Level* lv = nullptr;
        std::unique_ptr<XSlice> near, port, pad, orb, slope, speed;
        double lastLo = -1e18;
    };
    std::array<FrameSlices, 4> fsl;
    auto slicesFor = [&](int f, double wLo) -> FrameSlices& {
        FrameSlices& fs = fsl[(size_t)(f & 3)];
        if (!fs.lv) {
            fs.lv = &frameLevel(L, f);
            fs.near = std::make_unique<XSlice>(fs.lv->objs);
            fs.port = std::make_unique<XSlice>(fs.lv->portals);
            fs.pad = std::make_unique<XSlice>(fs.lv->pads);
            fs.orb = std::make_unique<XSlice>(fs.lv->orbs);
            fs.slope = std::make_unique<XSlice>(fs.lv->slopes);
            fs.speed = std::make_unique<XSlice>(fs.lv->speeds);
        } else if (wLo + 1e-6 < fs.lastLo) {
            // a group behind the cursor: rewind by binary search, do not
            // rebuild (see XSlice::seekTo)
            fs.near->seekTo(wLo - 40);
            fs.port->seekTo(wLo - 60);
            fs.pad->seekTo(wLo - 40);
            fs.orb->seekTo(wLo - 50);
            fs.slope->seekTo(wLo - 60);
            fs.speed->seekTo(wLo - 80);
        }
        fs.lastLo = wLo;
        return fs;
    };
    // One stepped child. Slot 2i is state i's no-input child, 2i+1 its pressed
    // one (empty when that state does not expand both -- see stepKid).
    struct Child {
        State s{};
        SearchKey key{};          // keyOf(s), computed on the worker thread
        uint8_t dead = 0;
        uint8_t valid = 0;
        const char* why = "";     // --dbg only
        const Obj* obj = nullptr; // --dbg only
        int rotUid = -1;          // --rotwatch only: the 2900 stepKid applied
        int8_t rotF0 = -1, rotNf = -1;
    };
    std::vector<Child> kids;      // reused every layer
    std::vector<ClearSample> clearKids;   // --clearprobe only, reused too
    std::vector<size_t> gidx;     // this speed group's indices into `cur`
    SlopeVetoIndex vetoIdx;       // rebuilt per group, storage reused
    // Below this many children the pool's wakeup costs more than the work.
    // A thin frontier is cheap anyway; the layers that matter run at the cap.
    const size_t kParallelMin = 256;
    std::unique_ptr<ThreadPool> pool;
    if (g_threads > 1) pool.reset(new ThreadPool(g_threads - 1));  // + this one
    // ...lent to the moving geometry's per-object loops (g_dynParTasks) for as
    // long as it exists. The guard is declared after the pool, so it takes the
    // loan back before the pool goes away -- including on every early return.
    struct DynParLoan {
        ~DynParLoan() { g_dynParTasks = nullptr; }
    } dynParLoan;
    if (pool)
        g_dynParTasks = [&pool](size_t n, const std::function<void(size_t)>& task) {
            pool->parallelTasks(n, task);
        };
    long long bestT = 0;
    double bestX = 0;
    State goalState{};
    bool solved = false;
    // Cap accounting for the driver's tier ladder (see `capstat:` below).
    // A PARTIAL that never TOUCHED the cap died of physics, not of capacity,
    // and re-running it at a higher tier is pure waste -- the driver reads
    // these to skip that.
    size_t maxAlive = 0;
    long long capHits = 0, capDropped = 0;
    // --heldcellcap: layers over the cap in states whose cells fit (kept whole), layers cut by
    // cells, and the button twins those cuts kept beyond the cap (printed only under the flag).
    long long cellSpared = 0, cellCuts = 0, cellTwinsKept = 0;
    SearchCensus census;   // --searchcensus only
    TwinStats twin;        // --twinskip / --twinaudit
    std::vector<uint8_t> kidMoot;   // --twinaudit: per parent of the group, predicted twin
    // Seed the anchor's band by replaying every mode portal already behind x0.
    // This is the one place the x-only approximation survives -- a re-anchored
    // run does not know which lane the player took, so it takes the last portal
    // by x. The search itself carries the band per state from here on.
    {
        FlyBand seed;
        // ...and replay the dual portals the same way the search does, or an
        // anchor taken inside a dual seeds the band from the mode portal's own
        // cy instead of the dual's (lv16's first dual: 390 instead of 330).
        double seedRefY = 0.0;
        bool seedDual = false;
        int seedMode = (int)init.mode;
        for (const Obj& p : L.portals) {
            if (p.cx > x0) break;
            if (p.type == 23 || p.type == 24) {
                seedDual = (p.type == 23);
                if (seedDual) {
                    seedRefY = p.cy;
                    seed = bandFor(seedRefY, bandHeightDual(seedMode));
                }
                continue;
            }
            const double modeH = bandHeightFor(p.type);
            const bool isModeP = modeH > 0.0 || p.type == 6 || p.type == 27;
            if (isModeP) {
                seedMode = (p.type == 5)    ? 1
                           : (p.type == 16) ? 2
                           : (p.type == 19) ? 3
                           : (p.type == 26) ? 4
                                            : 0;
            }
            if (seedDual && isModeP) {
                seed = bandFor(seedRefY, bandHeightDual(seedMode));
            } else if (modeH > 0.0) {
                seedRefY = p.cy;
                seed = bandFor(p.cy, modeH);
            }
        }
        init.bandRefY = (float)seedRefY;
        // ...unless GD was asked. `--startband f,c` carries the anchor tick's
        // own getMinPortalY / getMaxPortalY straight out of the dump, and it
        // REPLACES the x-only guess above rather than refining it.
        //
        // The guess is wrong whenever the player flew past a mode portal
        // instead of through it. Measured on lv20 (2026-08-09): the wave goes
        // UNDER the portal at (4445,303) -- its box is y[253,353] and the wave
        // is at y=114..160 -- so GD never fires it and its band stays
        // [90,390] (dump pmin/pmax read 11/309 + the layer offsets). The seed
        // took it anyway, put the band at [150,450], and clamped the wave at
        // 156 where GD was at 122. Everything after ran 34 px high, so GD died
        // where the model lived and the driver bought the difference back four
        // ticks at a time -- 5 px per iteration, which is not progress.
        if (startBandUsable()) {
            // ...unless the anchor is OUTSIDE it, in which case GD's pmin/pmax
            // are stale defaults and not a band the player is in. Measured on
            // lv22's ball corridor (2026-08-14): every anchor the ladder takes
            // there comes back as `--startband 90,386.999969` (the default
            // 90 + 270/camScale) while GD's own y is ~1,000 and climbing to
            // 1,074 -- GD plainly does not clamp there. Taking the band at face
            // value clamped the seed to 387 on the very first tick, so the
            // fixup pass reported `regenDied=t0+1` with dy=-593.95 and the
            // driver could not re-anchor anywhere in the corridor.
            const bool inside = (double)init.y >= g_startBandFloor - 1.0
                                && (double)init.y <= g_startBandCeil + 1.0;
            if (inside) {
                seed.floorY = g_startBandFloor;
                seed.ceilY = g_startBandCeil;
            } else {
                std::printf("startband: anchor y=%.2f is OUTSIDE [%.1f, %.1f]"
                            " - ignoring it (stale pmin/pmax)\n",
                            (double)init.y, g_startBandFloor, g_startBandCeil);
                seed.floorY = kGroundY;
                seed.ceilY = 1e9;
            }
        }
        if (g_shipCeilSet) seed.ceilY = g_shipCeil + kCubeHalf;
        if (g_ufoCeil > 0.0) seed.ceilY = g_ufoCeil;
        if (g_flyFloor > 0.0) seed.floorY = g_flyFloor;
        init.bandFloor = (float)seed.floorY;
        init.bandCeil = (float)seed.ceilY;
        if (startBandUsable())
            std::printf("startband: the seed will start in [%.1f, %.1f]\n",
                        seed.floorY, seed.ceilY);
        // --dualband: an anchor inside a dual takes the reference y from GD's own bands instead.
        // The replay above sees only the portals in L.portals, and a dual portal in a group is not
        // there (frames.hpp g_dualBand) -- it also re-reads every dual portal by x, where GD keeps
        // the one that turned the dual on. Every band GD placed since then is bandFor(ref, H), so
        // each on-grid row [fl, fl + H] with H a dual height puts ref in [fl + H/2, fl + H/2 + 30)
        // (a floor held at 90 bounds it from above only). The rows are intersected from the anchor
        // backwards -- the startband first, then the track -- and the walk stops at the first row
        // that would empty it, which is a band from before this reference. Off-grid rows are the
        // camera's and say nothing about it.
        if (g_dualBand && init.dual) {
            double lo = -1e18, hi = 1e18;
            int used = 0;
            auto take = [&](double fl, double ce) -> bool {
                const double H = ce - fl;
                if (std::fabs(H - kBandFly) > 0.01 && std::fabs(H - kBandOther) > 0.01)
                    return true;   // not a dual height: no information, keep walking
                if (fl <= 90.0 + 0.01) {
                    const double b = 90.0 + H * 0.5 + 30.0;
                    if (b <= lo) return false;
                    hi = std::min(hi, b);
                    ++used;
                    return true;
                }
                if (std::fabs(fl - std::round(fl / 30.0) * 30.0) > 0.01) return true;
                const double a = fl + H * 0.5, b = a + 30.0;
                if (b <= lo || a >= hi) return false;
                lo = std::max(lo, a);
                hi = std::min(hi, b);
                ++used;
                return true;
            };
            bool open = true;
            if (startBandUsable()) open = take(g_startBandFloor, g_startBandCeil);
            for (size_t k = g_bandTrackIntervals.size(); open && k-- > 0;) {
                const BandTrackRow& r = g_bandTrack[g_bandTrackIntervals[k]];
                if ((long long)r.t > t0) continue;
                open = take((double)r.fl, (double)r.ce);
            }
            if (used > 0 && lo > -1e17 && hi < 1e17) {
                // The rows only narrow the reference to a 30 px window when one height has been
                // seen, and a window's midpoint can sit on the wrong side of the other height's
                // 30 px step (lv16's anchors before t=8,459: H 270 alone gives [495,525), the
                // dual portal is at 509, the midpoint 510 puts the H 300 floor at 360 where GD
                // has 330). So an actual portal y wins: the replay's own when it fits, else the
                // last dual portal behind the anchor whose cy fits -- grouped ones included, which
                // the replay cannot see (custom level C's uid 36984, cy 465, in [465,480)).
                double pick = 0.0;
                const char* how = nullptr;
                if (seedRefY >= lo && seedRefY < hi) {
                    pick = seedRefY;
                    how = "the portal replay";
                } else {
                    double bestX = -1e18;
                    auto consider = [&](const Obj& p) {
                        if (p.type != 23 || p.cx > x0 || p.cx <= bestX) return;
                        if (p.cy >= lo && p.cy < hi) {
                            bestX = p.cx;
                            pick = p.cy;
                            how = "a dual portal";
                        }
                    };
                    for (const Obj& p : L.portals) consider(p);
                    for (size_t k = 0; k < L.dyn.objs.size() && k < L.dyn.bucket.size(); ++k)
                        if (L.dyn.bucket[k] == Dynamics::PORT) consider(L.dyn.objs[k]);
                    if (!how) {
                        pick = (lo + hi) * 0.5;
                        how = "the rows' midpoint";
                    }
                }
                init.bandRefY = (float)pick;
                std::printf("dualband: the anchor is inside a dual - reference y %.1f (%s) from %d"
                            " band row(s), in [%.1f, %.1f) (the portal replay gave %.1f)\n",
                            (double)init.bandRefY, how, used, lo, hi, seedRefY);
            } else {
                std::printf("dualband: the anchor is inside a dual but no band row bounds the"
                            " reference y - left at %.1f\n", seedRefY);
            }
        }
    }
    size_t ceilIdx = 0;
    // next arena size that triggers a GC (doubles after each compaction)
    size_t gcNext = g_gcNodes > 0 ? g_gcNodes : (size_t)-1;
    long long nBornPrev = 0, nDiedPrev = 0;   // last layer's expansion counters
    size_t prevAlive = 0;

    // (there is no shared x accumulator any more -- every state carries its own
    // xAbs and its own dx, and the layer is processed one speed group at a
    // time. See the speed-group note in the loop below.)
    // Current speed. A re-anchored solve starts mid-level, so every speed
    // portal already behind x0 has to be replayed here or the tail would run
    // the whole rest of the level at the wrong dx.
    float curDxF = kDxF;
    size_t spdIdx = 0;
    auto speedGate = [&](const Obj& o) { return o.cx - o.hw - kCubeHalf; };
    while (spdIdx < L.speeds.size() && speedGate(L.speeds[spdIdx]) <= x0) {
        curDxF = dxForSpeedId(L.speeds[spdIdx].id);
        ++spdIdx;
    }
    // The loop above positions spdIdx for the portals AHEAD, which is what it is
    // really for. What it must not do is decide the CURRENT speed by x alone: a
    // portal the player flew past without touching never fired in GD. When the
    // caller passed GD's own multiplier, that is a measurement of the answer, so
    // it wins over the replay. (lv16: the replay says 0.9 from x=17,403 on, GD
    // says 1.1 -- 0.3145 px/tick, and every tail past there was mistimed.)
    if (g_startSpeedMul > 0.0) {
        const float measured = dxForSpeedMul(g_startSpeedMul);
        if (measured != curDxF)
            std::printf("start speed: replay-by-x says %.5f, GD says %.5f "
                        "(mul %.3f) - using GD\n",
                        (double)curDxF, (double)measured, g_startSpeedMul);
        curDxF = measured;
    }
    // ...and the seed state carries it, so a re-anchored tail starts at the
    // section's real speed rather than 0.9 (see State::dx).
    init.dx = curDxF;

    // ---- --replay: fixed-input diagnostic resim (no search) ----------------
    // Mirrors the witness resim at the end of main (same stepBoth, same window
    // construction) but takes its input from a plan file instead of the arena
    // walk, and STOPS on death instead of ignoring it (the witness resim's
    // documented flaw: it writes trace rows past rdead, and the band clamp is
    // an absorbing fixed point, so a dead lineage can look alive).
    prepMark(4);
    if (!replayPath.empty()) {
        struct Edge { long long press; int v; };
        std::vector<Edge> edges;
        {
            std::ifstream pf(replayPath);
            if (!pf) {
                std::printf("REPLAY: cannot open %s\n", replayPath.c_str());
                return 2;
            }
            std::string ln;
            while (std::getline(pf, ln)) {
                // the driver splices plans through PowerShell, which stamps a
                // UTF-8 BOM -- without this the FIRST edge of every spliced
                // plan is silently dropped (found the hard way: the replayed
                // cube never took its first jump and died 40 px later)
                if (ln.size() >= 3 && (unsigned char)ln[0] == 0xEF &&
                    (unsigned char)ln[1] == 0xBB && (unsigned char)ln[2] == 0xBF)
                    ln.erase(0, 3);
                long long p; int v;
                if (std::sscanf(ln.c_str(), "input=%lld,%d", &p, &v) == 2)
                    edges.push_back(Edge{p, v});
            }
        }
        // [2026-08-19] WINDOW RE-PUSH. On w1, the tick a 2899 window lifts, GD
        // re-issues pushButton for a button that is physically still held --
        // worker98 measurement: at t=20,487 of the reference run (the end of
        // window 20110:20487) a PO_pushButton appears with NO INJECT and the
        // robot jumps at 20,488 with vy=4.568. Five variants all divide
        // cleanly by "held in the press ledger before w1 <=> fires":
        //   press20484/rel20487 -> jumps     press20486/rel20487 -> jumps
        //   press20484/rel20485 -> no jump   press20470/rel20473 -> no jump
        //   no pair -> no jump
        // (Even when the release lands on the same tick as w1, the re-push
        // runs before that tick's injected release.)
        // The model mirrors it with synthetic edges: if held(<w1), add a press
        // at press=w1 (its effect lands at w1+1 through the existing latOf
        // path, and the window's suppression ends at w1, so it passes
        // through). If already released by w1, add a release at w1+1 too,
        // closing it within 1 tick (holding across the window is unchanged =
        // the measured ship shape).
        // The search side (which has no edges) is untouched -- the
        // completeness hole that the post-window jump cannot be used as a
        // move remains, but the correctness side (census) is closed by this.
        std::stable_sort(edges.begin(), edges.end(),
                         [](const Edge& a, const Edge& b) { return a.press < b.press; });
        if (!g_ctrlWin.empty()) {
            const size_t nOrig = edges.size();
            for (const auto& w : g_ctrlWin) {
                int heldBefore = 0, heldAfter = 0;
                for (size_t k = 0; k < nOrig; ++k) {
                    if (edges[k].press < w.second) heldBefore = edges[k].v;
                    if (edges[k].press <= w.second) heldAfter = edges[k].v;
                    else break;
                }
                if (heldBefore) {
                    edges.push_back(Edge{w.second, 1});
                    if (!heldAfter) {
                        edges.push_back(Edge{w.second + 1, 0});
                        // a buffered jump fires without holding ->
                        // this jump does not hover (rePushNoHoverAt)
                        g_winRePushJump.push_back(w.second + 1);
                    }
                    std::printf("REPLAY: ctrl re-push at t=%lld (held into "
                                "window end)\n", w.second);
                }
            }
            // synthetic edges are appended and then stable-sorted again --
            // they must come AFTER the real release {w1,0} at the same press
            // (on an equal effect tick the later press wins, see the note
            // below)
            std::stable_sort(edges.begin(), edges.end(),
                             [](const Edge& a, const Edge& b) { return a.press < b.press; });
        }
        std::printf("REPLAY: %zu edges from %s\n", edges.size(), replayPath.c_str());
        g_hist = HistStat{};   // --histstat: this replay's counts only
        g_histFired.clear();
        g_histPads.clear();
        // The plan stores PRESS ticks (effect - latency, see the emitter at the
        // bottom of main). Latency depends on the mode at the END of the press
        // tick, which a forward sim knows by the time it needs it: after
        // stepping tick t, schedule every edge pressed at t for t + lat(mode).
        // The two lats are 1 and 2, so effects never cross in press order (a
        // swap would need a lat gap >= 2); equal-tick effects apply in press
        // order, later press winning, which is what GD does. Mode-portal
        // boundary ticks can still land +/-1 tick -- the known latency class
        // the driver absorbs -- so a divergence needs ~2 ticks of slack before
        // it means anything.
        auto latOf = [](uint8_t m) { return (m == 1 || m == 3) ? 2 : 1; };
        size_t eIdx = 0;
        std::vector<std::pair<long long, int>> fx;  // (effect tick, level)
        // An edge whose EFFECT tick is at or before t0 was already consumed by
        // the time the anchor's state was recorded, so it must NOT be replayed
        // as a change: it defines the level the anchor comes in HOLDING.
        // Pushing it into fx made the very first tick see `input=1` against an
        // `action` of 0, i.e. a fresh press -- and the two modes whose action
        // is the rising EDGE (`act = input && !s.action`, swing and UFO) then
        // fired one extra time. Measured on lv22 t=4,200 (swing, plan edge
        // pressed at 4,199 -> effect 4,200 = the anchor tick itself): GD flips
        // at 4,201, the model flipped at 4,202 and stayed a tick behind for
        // the rest of the segment.
        // `init.action` is normally seeded from the --start hold field only
        // when the anchor is mid-hover or mid-dash (see that gate's note: the
        // unconditional version cost lv12/18/20 a segment each). This is why
        // it did -- the hold field is `held_before(plan, t0)`, which counts by
        // PRESS tick, so for a ship/UFO (latency 2) an edge pressed at t0-1
        // reads as already held when its effect is still one tick away. The
        // level derived here is by EFFECT tick and per-mode, which is the
        // quantity `action` actually means.
        int preLevel = 0;
        bool preRise = false;   // the rising edge landed exactly ON t0
        while (eIdx < edges.size() && edges[eIdx].press <= t0) {
            const long long eff = edges[eIdx].press + latOf(init.mode);
            if (eff <= t0) {
                preRise = (eff == t0) && edges[eIdx].v != 0 && preLevel == 0;
                preLevel = edges[eIdx].v;
            } else {
                fx.push_back({eff, edges[eIdx].v});
            }
            ++eIdx;
        }
        init.action = (uint8_t)preLevel;
        // --spentaction (the hist seeding's note): ...but an edge whose press GD has already SPENT
        // before t0 -- a ring takes it on the press's own tick, a tick sooner than the flap's
        // latency 2 counts -- is not an edge any more when its counted effect tick arrives.
        if (init.pressSpent && (init.mode == 3 || init.mode == 7))
            init.action = 1;
        // ...and the SWING's pending flip is state the dump has no column for.
        // The tap sets a pending bit and the flip lands on the NEXT tick, so an
        // anchor taken on the tick the press took effect owes the flip. Same
        // lv22 t=4,200 measurement: with the level fixed above and no pending
        // bit the model never flips at all. The bit rides rHover (robot-only,
        // free in mode 7) exactly as the airborne path uses it.
        if (init.mode == 7 && preRise) init.rHover = 1;
        std::ofstream tr(outPath + ".trace.csv");
        tr.precision(10);
        // `act` = the input level the tick was stepped with. The driver's
        // fixup recorder needs it (a divergence override is gated on the
        // input), and reconstructing it from the plan would duplicate the
        // latency rules in PowerShell.
        // ...and the CONTACT STATE, so the driver can group divergences by
        // CAUSE instead of patching them point by point.
        //
        // Why: a fixup record used to carry only (x, y, vy, dy, dvy), so every
        // occurrence of ONE mishandled interaction became its own local override
        // (matched within 1.2 px of x and 4 px of y). Measured on lv20
        // 2026-08-08: 104 records to reach x=11,973 -- 115 px per record -- and
        // 62 of them fell into SIX signatures. One rule fix (the ceiling ramp)
        // had moved the same level 9,266 -> 18,214 by itself. Without saying what
        // the model was touching, the driver cannot tell "a constant is off"
        // (tight dy spread -> fit it) from "the formula is wrong" (wide spread ->
        // fix the code).
        // `dx` = this state's speed (px/tick). THE SIGNATURE NEEDS THE SPEED:
        // the acceleration switch threshold is 2 x GD's per-speed gravity
        // (models/speed.hpp), so it takes a different value in every speed band.
        // Without knowing which band a divergence shows up in, "the threshold is
        // off" cannot be told apart from some other cause. (This used to say
        // 1.3 and 1.6 were unmeasured and inherited 0.9's value -- they were,
        // and that was the row the formula corrected.)
        // `flip` and `frame` are here because the rotated sections are exactly
        // where `flip` stops meaning what GD's `upsideDown` means, and without
        // them a sign disagreement reads as a physics bug (lv22 t=5,110: the
        // model's vy is the negative of GD's for the whole frame-3 section, so
        // a type-4 portal looks like a no-op and never fires).
        tr << "tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act"
              ",onslope,slopem,slopet,bandf,bandc,mini,held,dx,nearorb,clamp"
              ",clampuid,clampcx,clampcy,flip,frame,rot,rotneg"
              // The SECOND BODY's own ride state. The dual runs the whole of
              // stepOne twice and the second half keeps its own copy of every
              // one of these, but only p1's was ever written -- so a divergence
              // that lives in p2's ride ("only p2 is off, by exactly the slope
              // exit bonus") had nothing to read. Appended at the end; existing
              // readers index the columns before this by position.
              ",grounded2,onslope2,slopem2,slopet2,mode2,ceilt,ceilt2,mini2"
              // `fxblk`: the moving object that kept a MATCHING fixup from
              // firing on the step into this row, -1 if none (fixup.hpp,
              // g_fxBlockedUid). The recorder's third answer.
              ",fxblk\n";
        // Make --snaplog usable in replay too (it used to exist only on the
        // SOLVE side, so a known plan's stair snaps could never be checked
        // against GD's snaptrace).
        std::ofstream sn;
        if (!snapLogPath.empty()) {
            sn.open(snapLogPath);
            g_snapOut = &sn;
        }
        State s = init;
        // Did --seeddump's tick actually occur in this walk? A tick the walk
        // never reached must SAY SO rather than print nothing: silence reads
        // the same as "nothing was consumed there", and a producer would take
        // the absence for a clean seed. Same discipline as NO VERSION and
        // resimdie=? -- a value that is missing has to be distinguishable from
        // a value that is zero.
        bool sawSeedTick = false;
        // Slices live in the CURRENT frame and restart when it turns (their
        // cursor is monotone in that frame's u).
        // ...and when the ANCHOR itself starts inside a turned frame
        // (--start's 21st field), bind the world in that orientation from the
        // start. This was fixed at &L, so the state was in frame 3 while the
        // geometry was in frame 0, and the replay lost its footing within 2
        // ticks (the "follow 2 ticks" at lv22 t=11,340).
        Level* Lf = &frameLevel(L, (int)init.frame);
        std::unique_ptr<XSlice> sl, pl, dl, ol, sls, vl;
        auto rebind = [&](Level& lv) {
            sl = std::make_unique<XSlice>(lv.objs);
            pl = std::make_unique<XSlice>(lv.portals);
            dl = std::make_unique<XSlice>(lv.pads);
            ol = std::make_unique<XSlice>(lv.orbs);
            sls = std::make_unique<XSlice>(lv.slopes);
            vl = std::make_unique<XSlice>(lv.speeds);
        };
        rebind(*Lf);
        // Turn the frame ONE TICK AFTER the crossing (measured: the boxes
        // overlap for ~15 ticks and nothing happens; the change lands the tick
        // after the player's forward coordinate passes the object's).
        int pendingFrame = -1;
        const float rDxF = curDxF;
        int curIn = preLevel;   // (see the pre-anchor edge split above)
        size_t fxIdx = 0;
        long long diedT = -1;
        double diedX = 0;
        long long lastT = t0;
        for (long long t = t0 + 1; t <= tEnd; ++t) {
            lastT = t;
            while (fxIdx < fx.size() && fx[fxIdx].first <= t)
                curIn = fx[fxIdx++].second;
            // Under reverse (State::rev) x decreases. Apply the same sign
            // here as the DP side's group windows (if the replay / resim do
            // not match GD, every fixup comparison becomes a lie).
            const float rDxUsed = ((s.dx > 0.f) ? s.dx : rDxF)
                                  * (s.rev ? -1.f : 1.f);
            const double xPrevR = (double)s.xAbs;
            const double x = (double)advanceX(s.xAbs, rDxUsed);
            if (g_dynDbg == -2 && t == t0 + 1)
                std::printf("revdbg init.rev=%d (at replay start)\n", (int)init.rev);
            if (g_dynDbg == -2 && t <= t0 + 3)
                std::printf("revdbg t=%lld s.rev=%d dx=%.4f xPrev=%.3f x=%.3f\n",
                            t, (int)s.rev, rDxUsed, xPrevR, x);
            // autonomous fires, from this single trajectory's own crossing
            // (x is the position the player reaches AT tick t, so a first
            // x >= cx means the crossing is t and the move starts at t + delay)
            for (AutoTrig& A : g_autoTrig) {
                if (A.fireT >= 0) continue;
                if (x < A.cx) break;   // sorted by cx
                A.fireT = (int)t + A.delay;
                A.fireX = A.lockLastX = x;
                std::printf("autotrig: uid %d x=%.0f crossed at t=%lld, "
                            "effect t=%d\n", A.uid, A.cx, t, A.fireT);
            }
            // GAMEPLAY ROTATION: did this tick's advance pass a rotation
            // object, measured along the CURRENT frame's travel axis?
            // ...those pointing at the same frame are reverse toggles, so
            // they are NOT excluded (applyRotation handles them with a
            // proximity test). The crossing direction follows the direction
            // of travel.
            // [2026-08-24] THE SAME GATE AS applyRotation, or the two paths live
            // in different worlds: this copy still held the falsified 30px window
            // after the gate itself moved to kRotPerpWin + nearest-|dv|-wins, so
            // the DP turned at t=16,428 (|dv|=115, inside the measured firing at
            // 116.7) while the replay of the very same plan sailed past in frame 0
            // and rode the underside of the death block out of the level. Every
            // constant here follows the measurement at applyRotation's gate.
            {
                RotTrig* rBest = nullptr;
                bool bestSame = false;
                double bestDvR = 1e18;
                for (RotTrig& r : g_rotTrig) {
                    // `same` MUST HAVE THE SAME DEFINITION as applyRotation
                    // (4941). Read from frame equality alone, a reverse setting
                    // that has a gnddir (setRev=1, frame stays 0 and only the
                    // direction changes) turns into a "pure toggle" and falls
                    // into the else branch below. [2026-08-18] lv22 uid16659
                    // (22575,1755) was that: only the one-shot (revT) got burnt,
                    // applyRotation was never called, the model kept moving
                    // forward and GD's floor vanished from the near slice (the
                    // missed landing at t=18,513).
                    const bool same = (r.setRev < 0)
                        ? (r.frame == (int)s.frame)
                        : (r.frame == (int)s.frame
                           && r.setRev == (int)s.rev);
                    if ((same ? r.revT : r.firedT) >= 0) continue;  // one shot
                    const double ru = frameU((int)s.frame, r.cx, r.cy);
                    const bool cross = s.rev ? (xPrevR > ru && x <= ru)
                                             : (xPrevR < ru && x >= ru);
                    // ...and it must overlap on the perpendicular axis too. This
                    // CONSUMES the one-shot, so it is narrowed by the same
                    // condition as applyRotation's gate -- merely passing far
                    // away must not burn the trigger. [2026-08-16] Measured
                    // (lv22): uid6308 (15,399,615) was consumed at t~11,2xx,
                    // when the world y crossed 615 in frame 3 -- the player was
                    // at world x 16,125 then, 726px AWAY on the perpendicular
                    // axis. By t=12,134, when GD actually turns, it was already
                    // spent and only the model failed to turn.
                    const double dvR =
                        std::fabs((double)s.y
                                  - frameV((int)s.frame, r.cx, r.cy));
                    if (!cross || dvR > kRotPerpWin) continue;
                    if (dvR < bestDvR) {
                        bestDvR = dvR; rBest = &r; bestSame = same;
                    }
                }
                if (rBest) {
                    // a same-frame firing (reverse toggle / absolute rev
                    // setting) is ALWAYS handed to applyRotation too. Burning
                    // only the one-shot without raising pendingFrame leaves
                    // rev unchanged forever
                    pendingFrame = rBest->frame;
                    if (!bestSame) rBest->firedT = (int)t;
                    else rBest->revT = (int)t;
                }
            }
            std::vector<const Obj*> rn, rp, rd, ro, rs;
            Lf->dyn.seek((int)t);
            seekOtherFrames(Lf, (int)t);   // --rotpretap only
            // + this tick's advance (xPrevR is tick t-1's x): see the group
            // call's note on why a lock needs the CURRENT tick.
            // --fbforce <box>:<tick> (diagnostic): place this tick's geometry as if
            // that box had fired at <tick>, when this walk has entered it. Asks
            // whether a box's fire tick still moves an object after the box's key
            // window (g_touchMoveTicks) has run out -- the window assumes it does
            // not.
            uint16_t fbForced[kTouchBits];
            const uint16_t* fbUse = s.fireB;
            if (g_fbForceBox >= 0 && g_fbForceBox < kTouchBits && s.trig.test(g_fbForceBox)) {
                std::copy(s.fireB, s.fireB + kTouchBits, fbForced);
                fbForced[g_fbForceBox] = (uint16_t)g_fbForceTick;
                fbUse = fbForced;
            }
            Lf->dyn.applyTriggers(s.trig, (int)s.trigT, fbUse,
                                  s.lockOff + (float)(x - xPrevR), (int)t, x);
            std::vector<std::pair<const TouchTrig*, TouchMask>> rt;
            const std::vector<TouchTrig>& tfr = touchFor((int)s.frame);
            for (size_t b = 0; b < tfr.size(); ++b) {
                if (s.trig & touchBit(b)) continue;
                const TouchTrig& T = tfr[b];
                // --walkgates: a standing Count and an Item Compare stay in the list
                // the way the search's window keeps them (its note at `standing`).
                const bool standing = (T.id == 1611 || T.id == 3620)
                                      && T.count >= 0 && !T.tap;
                const bool anywhere = T.id == 3620 && T.count >= 0;
                if (!anywhere && !standing && T.cx + T.hw < x - 40) continue;
                if (!anywhere && T.cx - T.hw > x + 40) continue;
                rt.push_back({&T, touchBit(b)});
            }
            if (g_slopeDbg && s.frame != 0) {
                std::printf("trigcand t=%lld frame=%d x=%.3f y=%.3f "
                            "boxes=%zu cand=%zu:",
                            (long long)t, (int)s.frame, x, (double)s.y,
                            tfr.size(), rt.size());
                for (const auto& tb : rt)
                    std::printf(" (%.1f,%.1f %.0fx%.0f du=%.2f dv=%.2f)",
                                tb.first->cx, tb.first->cy,
                                2 * tb.first->hw, 2 * tb.first->hh,
                                std::fabs(x - tb.first->cx),
                                std::fabs((double)s.y - tb.first->cy));
                std::printf("\n");
            }
            sl->forRange(x - 40, x + 40, [&](const Obj& o) { rn.push_back(&o); });
            Lf->dyn.collect(Dynamics::NEAR, x - 40, x + 40, rn);
            pl->forRange(x - 60, x + 60, [&](const Obj& o) { rp.push_back(&o); });
            Lf->dyn.collect(Dynamics::PORT, x - 60, x + 60, rp);
            dl->forRange(x - 40, x + 40, [&](const Obj& o) { rd.push_back(&o); });
            Lf->dyn.collect(Dynamics::PAD, x - 40, x + 40, rd);
            ol->forRange(x - 50, x + 50, [&](const Obj& o) { ro.push_back(&o); });
            Lf->dyn.collect(Dynamics::ORB, x - 50, x + 50, ro);
            sls->forRange(x - 60, x + 60, [&](const Obj& o) { rs.push_back(&o); });
            Lf->dyn.collect(Dynamics::SLOPE, x - 60, x + 60, rs);
            std::vector<const Obj*> rv;
            vl->forRange(x - 80, x + 80, [&](const Obj& o) { rv.push_back(&o); });
            Lf->dyn.collect(Dynamics::SPEED, x - 80, x + 80, rv);
            std::vector<const Obj*> rnSolid;   // --solidorder: rn in GD's solid order, once per tick
            solidOrderInto(rn, rnSolid);
            const StepCtx K{x, xPrevR, rDxUsed, t, &rn, &rp, &rd, &ro, &rs, &rv,
                            &SP, &SPmini, &UP, &UPmini, &rt, nullptr, nullptr,
                            &rnSolid};
            bool rdead = false;
            g_nearOrb = 0;
            g_dashVySet = 0;
            g_clampWhy = "";
            g_clampUid = -1;
            // ...and the COORDINATES beside them, which this block cleared the
            // "why" and the uid of but not the numbers. seqcall found it: two
            // identical whole-run replays in one process differ in clampcx /
            // clampcy on 539 of lv20's rows and 448 of lv21's, starting at
            // t=1 with the LAST clamp of the previous call. Nothing else
            // differs, so the trajectory was never at risk -- but the comment
            // at CLAMP0O says these exist to catch stale geometry, and they
            // were themselves stale across calls.
            g_clampCx = g_clampCy = 0.f;
            g_deadWhy = "";
            g_deadObj = nullptr;
            // g_deadCx/Cy are NOT diagnostics: the resim block publishes them
            // as g_resimObjX / g_resimObjY. An object-free death (deadband,
            // maxplayy, offboard -- and the two-tick latch GD kills with) sets
            // g_deadObj to null and leaves the coordinates alone, so the
            // outcome carried the PREVIOUS killer's position with uid = -1.
            g_deadCx = g_deadCy = 0.f;
            const bool sPrevGrounded = (s.grounded != 0);   // applyRotation's re-tap
            const double sPrevY = (double)s.y;              // ...and its pre-tap y
            const int wgFrame = (int)s.frame;               // --walkgates (below)
            const uint8_t wgPrev = s.action;
            State c = stepBoth(s, (uint8_t)curIn, K, rdead);
            c.action = (uint8_t)curIn;
            s = c;
            // --seeddump <t>: the state's ACCUMULATED fields at one tick.
            //
            // These are the fields an anchor has to seed, and the check they
            // exist for is one comparison: run from t=0 with --seeddump T, run
            // with --start at T-1 and --seeddump T, and the two lines must
            // match. Three separate defects today were all "the anchor scan
            // does not seed this" (State::fireB, State::lockOff, and the
            // queue's rotSpent / rotChan / rotRev), each found only after it
            // changed an answer. WHOEVER ADDS A PER-STATE ACCUMULATED FIELD
            // ADDS IT HERE -- a field missing from this line is a field the
            // check cannot see.
            if ((g_seedDump >= 0 && t == (long long)g_seedDump)
                || (g_seedEvery > 0 && t % (long long)g_seedEvery == 0)) {
                // pressSpent is printed but NOT seeded: gdref has no column for
                // GD's +0x986, so an anchor starts it at 0 ("this press has not
                // been spent"). It joins ringHold in the hole the list at the top
                // of this file names. The bias is one-sided and small -- a state
                // anchored mid-hold can fire one ring GD would have refused --
                // and it needs the button held ACROSS the anchor tick to bite.
                // groundUid (--stickseam) is the same kind of hole: printed, not
                // seeded; an anchor mid-ride re-ties to the lowest-uid support.
                // usedOrbOld too (the ring fired before usedOrb): an orb is spent by
                // a press, and the recording cannot say which were taken -- the
                // hole usedOrb itself has (the list at the top of this file).
                std::printf("seed: t=%lld sizeof=%zu trig=0x%llx trigT=%d "
                            "lockOff=%.4f rotSpent=0x%x rotChan=%d "
                            "rotRev=0x%x rotStep=%.6f rotNeg=%d rotBoost=%d "
                            "ringHold=%d pressSpent=%d portalLatch=%s/%s "
                            "groundUid=%d usedOrbOld=%d usedOrbHist=%d|%d|%d portSeen=%d|%d|%d "
                            "touchRing=%d|%d|%d|%d/%x taps=%d fireB=",
                            t, sizeof(State), (unsigned long long)s.trig.word(0),
                            (int)s.trigT,
                            (double)s.lockOff, s.rotSpent, (int)s.rotChan,
                            (unsigned)s.rotRev, (double)s.rotStep,
                            (int)s.rotNeg, (int)s.rotBoost, (int)s.ringHold,
                            (int)s.pressSpent, gravLatchHex(s.portalLatch).c_str(),
                            gravLatchHex(s.portalLatch2).c_str(),
                            (int)s.groundUid, (int)s.usedOrbOld, (int)s.usedOrbHist[0],
                            (int)s.usedOrbHist[1], (int)s.usedOrbHist[2], (int)s.portSeen[0],
                            (int)s.portSeen[1], (int)s.portSeen[2], (int)s.touchRing[0],
                            (int)s.touchRing[1], (int)s.touchRing[2], (int)s.touchRing[3],
                            (unsigned)s.touchRingT, (int)s.taps);
                for (int b = 0; b < kTouchBits; ++b)
                    if (s.fireB[b]) std::printf("%d:%u,", b, s.fireB[b]);
                std::printf("\n");
                // The speed portals' memory (State::spdFired), on its own line so the one above
                // keeps its shape for the parsers that read it.
                std::printf("seed: t=%lld spdFired=%d|%d spdFired2=%d|%d\n", t,
                                (int)s.spdFired[0], (int)s.spdFired[1],
                                (int)s.spdFired2[0], (int)s.spdFired2[1]);
                // +0xa1c (State::a1cLatch, bit 1 = the second body): printed, not seeded -- the
                // dump has no column for it, so an anchor starts it at 0
                std::printf("seed: t=%lld a1cLatch=%d\n", t, (int)s.a1cLatch);
                // ...and the same state as a READY-MADE --startrotq argument.
                //
                // rotSpent is a mask over g_rotQ, and the bit order is
                // buildRotQueue's, not rotgameplay.txt's -- seeding uid 1215
                // and uid 1343, the file's first two rows, gives bits 0 and 8.
                // So the inversion back to uids belongs HERE, beside the queue
                // it indexes. A caller doing it would have to hold a copy of
                // that ordering, which is the proxy-for-identity shape this
                // campaign spent 2026-09-10 learning to distrust.
                //
                // SCOPE: these values come from a walk of the MODEL, not from
                // the game. frames.hpp:165-168 is what licenses that -- the
                // queue is right from t=0, measured at 191 of 192 transitions
                // agreeing on lv22 -- so a seed read off a t=0 walk carries at
                // most that 1-in-192 of the model's own error, and is not the
                // circular "assume the equality to prove it".
                //
                // Built ONCE, into a string, and then both printed and
                // published. The in-process caller (the repair loop) cannot
                // read stdout, so it needs the value rather than the line; two
                // formatters for one argument would be two things to keep in
                // step, and the bit->uid inversion above is precisely the part
                // that must not be duplicated.
                if (!g_rotQ.empty()) {
                    char head[64];
                    std::snprintf(head, sizeof head, "%d,%04x",
                                  (int)s.rotChan, (unsigned)s.rotRev);
                    std::string arg(head);
                    for (size_t q = 0; q < g_rotQ.size() && q < 32; ++q)
                        if ((s.rotSpent >> q) & 1u)
                            arg += "," + std::to_string(g_rotQ[q].uid);
                    std::printf("seedrotq: t=%lld --startrotq %s\n",
                                t, arg.c_str());
                    g_outcome.seedRotQ = arg;
                }
                sawSeedTick = true;
            }
            // ...and now turn, if the tick that just ran crossed one. The world
            // point is what carries over; (u,v) is re-read in the new frame.
            // The world VELOCITY carries too: the forward speed becomes the new
            // perpendicular one (measured on lv22 uid 6286 -- the cube keeps
            // moving +X at 1.298 px/tick while its travel axis is now +Y, which
            // reads as vy = 1.298 / kYScale = 5.193 on the tick it turns), and
            // the old perpendicular velocity is dropped because the forward
            // speed is the section's, not a free variable.
            // A `pendingFrame != s.frame` gate STRUCTURALLY DROPS SAME-FRAME
            // REVERSE (a firing that goes frame 0 -> 0 and changes only the
            // direction cannot pass). The DP side (stepKid) calls
            // applyRotation unconditionally and can solve reverse, yet this
            // replay path alone could not keep up -- the real cause of "the
            // DP says SOLVED, the replay keeps going forward". [2026-08-18]
            if (pendingFrame >= 0) {
                double wx0, wy0;
                fromFrame((int)s.frame, (double)s.xAbs, (double)s.y, wx0, wy0);
                const int rev0dbg = (int)s.rev;
                const int f0dbg = (int)s.frame;
                const int nf = applyRotation(s, xPrevR, (double)rDxUsed, t,
                                             curIn, sPrevGrounded,
                                             sPrevY, true);
                if ((nf >= 0 && nf != f0dbg) || (int)s.rev != rev0dbg) {
                    if (nf >= 0 && nf != f0dbg) {
                        Lf = &frameLevel(L, nf);
                        rebind(*Lf);
                    }
                    std::printf("rotation: frame -> %d at t=%lld "
                                "world=(%.1f,%.1f) u=%.3f v=%.3f vy=%.3f "
                                "f0=%d rev0=%d -> rev=%d flip=%d\n",
                                (nf >= 0 ? nf : f0dbg), t, wx0, wy0,
                                (double)s.xAbs, (double)s.y,
                                (double)s.vy, f0dbg, rev0dbg,
                                (int)s.rev, (int)s.flip);
                }
                pendingFrame = -1;
            }
            // --walkgates: the item gates the search's own step fires (itemGates),
            // after the turn and only on a tick that did not turn -- the search's
            // gate. Without it this walk never fires a Count or a Tap trigger.
            if (coinOn && !rdead && (int)s.frame == wgFrame)
                itemGates(s, wgPrev, curIn, K, wgFrame, (double)s.xAbs, (double)s.y,
                          hazardHalfFor(s.mode, s.mini != 0), t);
            // The trace is always WORLD coordinates -- that is what GD's dump
            // is, and a turned frame would otherwise read as a huge divergence.
            double wX, wY;
            fromFrame((int)s.frame, (double)s.xAbs, (double)s.y, wX, wY);
            // ...and so is vy. GD's dump reports the player's own vertical
            // velocity in the CURRENT gameplay frame, and in frame 3 the
            // model's vertical axis points the other way -- the same statement
            // `gdUpOf` makes about `flip`. Leaving the raw value here made
            // every tick of a frame-3 section look like a divergence with
            // dy=0.000 and dvy = -2*gd_vy, which is what the driver's fixup
            // recorder then wrote down: measured on lv22 (2026-08-14) the pass
            // anchored at t=4,643 reported "first divergence t=4,666 dy=-0.0000
            // dvy=-0.4240" while x and y agreed to three decimals for the whole
            // rotated section, and the loop spent 18 iterations at x=6,930-7,101
            // chasing 41 phantom fixups.
            // Only on ticks where a dash ring engaged, emit the same vy as
            // GD's dump (history and measurements at g_dashVy's declaration).
            // The state stays 0, so the trajectory is unchanged.
            // Fly-rings (speed.hpp): GD's row already carries the ring a ship's or UFO's press fires from this
            // state (the flag's note in speed.hpp); the step applies it on the next tick, so only
            // the display moves. The same gate as stepOne's, with the next tick's input.
            double rowVy = (double)s.vy;
            int rowFlip = (int)s.flip;
            if ((s.mode == 1 || s.mode == 3) && !s.dual && !s.jumpBuf
                && !s.pressSpent) {
                int nextIn = curIn;
                for (size_t j = fxIdx; j < fx.size() && fx[j].first <= t + 1; ++j)
                    nextIn = fx[j].second;
                if (!g_ctrlWin.empty() && ctrlOffAt(t + 1)) nextIn = 0;
                const float ringDx = (s.dx > 0.f) ? s.dx : K.dxF;
                const Obj* ring = nextIn
                    ? flyEarlyRing(s, K, (double)ringDx * timeWarpAt((double)s.xAbs)
                                             * (s.rev ? -1.0 : 1.0))
                    : nullptr;
                if (ring) {
                    State d = s;
                    const bool hs = g_histStatOn;   // a look ahead for the row, not a fire
                    g_histStatOn = false;
                    applyFlyEarlyRing(d, ring, ringDx);
                    g_histStatOn = hs;
                    rowVy = (double)d.vy;
                    rowFlip = (int)d.flip;
                }
            }
            const double vyGd = (g_dashVySet ? g_dashVy : rowVy)
                                * (s.frame == 3 ? -1.0 : 1.0);
            // ...and which line of step.hpp wrote this tick's vy, when asked.
            // Reported HERE rather than inside stepOne: this is the replay's own
            // per-tick loop, so it fires once per tick and cannot be confused
            // with the search running the same tick for many states.
            //
            // `writes` matters as much as `writer`. One write and the site is
            // the answer; several and the last one is only the last -- reading
            // it as "the site that decided the value" is the mistake the count
            // is here to prevent. Zero means vy was not written this step at all
            // and the value came from somewhere else entirely.
            // The vpNew trail is printed oldest first; `vpwrites` above eight
            // means the head of the sequence was overwritten.
            if ((g_vyWatchT >= 0 && t == g_vyWatchT)
                || (g_vyWatchT2 >= 0 && t == g_vyWatchT2)) {
                char vpt[96] = "-";
                const int nvp = g_vpWrites < 8 ? g_vpWrites : 8;
                for (int k = 0, o = 0; k < nvp; ++k)
                    o += std::snprintf(vpt + o, sizeof vpt - o, k ? ",%d" : "%d",
                                       g_vpTrail[(g_vpWrites - nvp + k) & 7]);
                char impt[96] = "-";
                const int nimp = g_impCount < 8 ? g_impCount : 8;
                for (int k = 0, o = 0; k < nimp; ++k)
                    o += std::snprintf(impt + o, sizeof impt - o, k ? ",%d" : "%d",
                                       g_impTrail[(g_impCount - nimp + k) & 7]);
                std::printf("vywriter: t=%lld writes=%d lastline=%d ywrites=%d ylastline=%d vyIn=%.6f vy=%.6f yIn=%.4f y=%.4f seatgate=%d impulsedOff=%d tappedOff=%d sOnSlope=%d took=%d impsite=%d "
                            "impcount=%d imptrail=%s vpwrites=%d vptrail=%s traceVy=%.6f frame=%d mode=%d\n",
                            (long long)t, g_vyWrites, g_vyWriter, g_yWrites, g_yWriter, (double)g_vyIn, (double)s.vy, (double)g_yIn, (double)s.y, g_seatGateSeen, g_seatImpulsedOff, g_seatTappedOff, g_seatOnSlope, g_seatTook, g_impulseSite,
                            g_impCount, impt, g_vpWrites, vpt, vyGd, (int)s.frame, (int)s.mode);
            }
            // --fxwatch: the fixup gate's own account of this step (fixup.hpp
            // fxDescribe). Kt is the step's tick as stepBoth saw it; it has to
            // equal t or the line describes some other step.
            if (g_fxWatchT >= 0 && t == g_fxWatchT)
                std::printf("fxwhy: t=%lld %s\n", (long long)t, g_fxWhy);
            tr << t << ',' << wX << ',' << wY << ',' << vyGd << ','
               << (int)s.mode << ',' << (int)s.grounded << ',' << (int)s.dual
               << ',' << s.y2 << ',' << s.vy2 << ',' << (int)s.flip2 << ','
               << curIn
               << ',' << (int)s.onSlope << ',' << s.slopeM << ','
               << (int)s.slopeT << ',' << s.bandFloor << ',' << s.bandCeil
               << ',' << (int)s.mini << ',' << (int)s.held << ',' << s.dx
               << ',' << (int)g_nearOrb
               << ',' << (*g_clampWhy ? g_clampWhy : "-")
               << ',' << g_clampUid << ',' << g_clampCx << ',' << g_clampCy
               << ',' << rowFlip << ',' << (int)s.frame
               // The player's sprite rotation. THE HITBOX TEST OF A TURNED
               // OBJECT IS DECIDED BY THIS, yet until now it was not in the
               // trace, so there was no way to measure "how far is the
               // model's rot from GD's" (measured on lv20: 1,158 degrees off
               // over a 281-tick anchor). Columns are appended at the end
               // (existing readers index by position).
               << ',' << s.rot << ',' << (int)s.rotNeg
               << ',' << (int)s.grounded2 << ',' << (int)s.onSlope2
               << ',' << s.slopeM2 << ',' << (int)s.slopeT2
               << ',' << (int)s.mode2 << ',' << (int)s.ceilT
               << ',' << (int)s.ceilT2 << ',' << (int)s.mini2
               << ',' << g_fxBlockedUid
               << "\n";
            if (rdead) {
                // --replayon (diagnostic): note the first death and walk on, the
                // way the witness resim does, so geometry after it can be read.
                if (g_replayOn) {
                    if (diedT < 0) { diedT = t; diedX = (double)s.xAbs; }
                } else {
                    diedT = t;
                    diedX = (double)s.xAbs;
                    break;
                }
            }
            if ((double)s.xAbs >= goalX) break;
            while (eIdx < edges.size() && edges[eIdx].press == t) {
                fx.push_back({t + latOf(s.mode), edges[eIdx].v});
                ++eIdx;
            }
        }
        g_outcome.replayDiedT = diedT;
        if (g_histStatOn) {
            if (FILE* hf = std::fopen(g_histStatPath.c_str(), "a")) {
                std::fprintf(hf,
                             "histstat out=%s x=%.1f died=%lld touchMax=%d touchDrop=%d "
                             "portMax=%d portFull=%d padMax=%d padFull=%d orbFired=%d "
                             "orbDrop=%d orbRefire=%d padRefire=%d\n",
                             outPath.c_str(), (double)s.xAbs, diedT, g_hist.touchMax,
                             g_hist.touchDrop, g_hist.portMax, g_hist.portFull, g_hist.padMax,
                             g_hist.padFull, g_hist.orbFired, g_hist.orbDrop, g_hist.orbRefire,
                             g_hist.padRefire);
                std::fclose(hf);
            }
        }
        if (diedT >= 0) {
            std::printf("REPLAY: model DIED at t=%lld x=%.1f (y=%.2f vy=%.3f "
                        "mode=%d grounded=%d dual=%d y2=%.2f vy2=%.3f held=%d)\n",
                        diedT, diedX, (double)s.y, (double)s.vy, (int)s.mode,
                        (int)s.grounded, (int)s.dual, (double)s.y2,
                        (double)s.vy2, (int)curIn);
            // The reason was already being recorded -- only --dbg (inside the
            // DP) ever read it, so a replay that died reported the state and
            // left "what killed it" to be guessed. A false death is exactly
            // the case where the object matters more than the state: measured
            // on lv20 t=786, the model and GD agree to 0.3 px for 786 ticks
            // and then only the model dies.
            if (*g_deadWhy) {
                std::printf("REPLAY: cause=%s", g_deadWhy);
                if (g_deadObj)
                    std::printf(" obj=uid%d id%d %gx%g @%.2f,%.2f (now %.2f,%.2f)",
                                g_deadObj->uid, g_deadObj->id,
                                2 * g_deadObj->hw, 2 * g_deadObj->hh,
                                (double)g_deadCx, (double)g_deadCy,
                                g_deadObj->cx, g_deadObj->cy);
                // WHICH copy of the level the killer came from. A moving object
                // exists once per gameplay frame that has been built, and only
                // the CURRENT frame's copy is seeked -- so a stale one reads as
                // a hazard frozen at its t=1 position.
                if (g_deadObj) {
                    const char* home = "?";
                    auto in = [&](const std::vector<Obj>& v) {
                        return !v.empty() && g_deadObj >= v.data()
                               && g_deadObj < v.data() + v.size();
                    };
                    if (in(L.objs)) home = "L.objs";
                    else if (in(L.dyn.objs)) home = "L.dyn";
                    else if (in(L.slopes)) home = "L.slopes";
                    else for (int fi = 1; fi < 4; ++fi)
                        if (g_frameLv[fi]) {
                            if (in(g_frameLv[fi]->objs)) { home = "frame.objs"; break; }
                            if (in(g_frameLv[fi]->dyn.objs)) { home = "frame.dyn"; break; }
                            if (in(g_frameLv[fi]->slopes)) { home = "frame.slopes"; break; }
                        }
                    std::printf(" from=%s", home);
                }
                std::printf("\n");
            }
        }
        else
            std::printf("REPLAY: SURVIVED to t=%lld x=%.1f (goal %.1f)\n",
                        lastT, (double)s.xAbs, goalX);
        // A --seeddump tick this walk never reached is reported as such. The
        // caller asked for the state at a tick; "the walk stopped first" is an
        // answer and silence is not.
        if (g_seedDump >= 0 && !sawSeedTick)
            std::printf("seedrotq: t=%d NOT REACHED - this walk ended at "
                        "t=%lld, so there is no state to seed from\n",
                        g_seedDump, lastT);
        std::printf("trace -> %s.trace.csv\n", outPath.c_str());
        g_snapOut = nullptr;
        return 0;
    }
    // --coinmask: the coins GD had already credited at the anchor (the repair
    // loop's re-anchor, repair.hpp). An anchored tail otherwise starts with an
    // empty set, so every coin behind x0 reads as missed and the miss prune kills
    // the seed on its first step. Bits for coins the level does not have drop.
    if (coinOn) {
        init.coins = (uint8_t)((uint32_t)g_coinMaskSeed & coinAll);
        if (init.coins)
            std::printf("coins: starting with 0x%x already collected\n",
                        (unsigned)init.coins);
        // --coinskip: every coin test skips a collected coin, so seeding the
        // bit takes that coin out of the goal and out of both miss prunes.
        const uint8_t skip = (uint8_t)((uint32_t)g_coinSkip & coinAll);
        if (skip) {
            init.coins |= skip;
            std::printf("coins: 0x%x not a goal this run (--coinskip)\n",
                        (unsigned)skip);
        }
    }
    cur.push_back(init);   // root state, AFTER every init amendment (see above)
    // Announce the search to anything watching from another thread (dp/progress.hpp). The end
    // is announced from the guard below, so that every way out of the loop clears it.
    //
    // THE LAST LAYER THIS LOOP CAN PROCESS, which is not `t0 + horizon`. The horizon bounds the
    // PLAN, not the search: the loop runs on to t0 + 2*horizon and only then takes a survivor
    // (the survivor guarantee below), and it can never run past tEnd either way. Announcing
    // t0 + horizon gave the mod's search bar a denominator it is wrong on both sides of --
    // under the short horizon (3,000) the tick runs PAST it, and under the full one (level
    // length + 2,000) an anchored tail at t0=18,000 announced 44,000-odd while the loop stops
    // at tEnd around 21,000, so the bar could never fill. Nothing the CLI prints reads this
    // field (it is written here and read only through dpbridge::progress), so the printed
    // output is unchanged.
    g_progress.begin(t0, horizon > 0 ? std::min(tEnd, t0 + 2 * horizon) : tEnd);
    struct ProgressGuard { ~ProgressGuard() { g_progress.end(); } } progressGuard;
    // --phaseprof buckets: 0 group setup, 1 step, 2 dedupe/merge, 3 cap, 4 gc, 5 mem+checkpoints,
    // 6 the rest of the layer (progress, prints, next layer's top).
    double pp[7] = {0, 0, 0, 0, 0, 0, 0};
    auto ppLast = std::chrono::steady_clock::now();
    auto ppMark = [&](int b) {
        if (!g_phaseProf) return;
        const auto n = std::chrono::steady_clock::now();
        pp[b] += std::chrono::duration<double>(n - ppLast).count();
        ppLast = n;
    };
    const auto ppStart = std::chrono::steady_clock::now();
    for (long long t = t0 + 1; t <= tEnd && !solved; ++t) {
        ppMark(6);
        nxt.clear();
        // ---- reference watch, part 1: where is it now (--refwatch) ---------
        // Locate the reference's PARENT in the frontier before the layer runs.
        // On the first layer it is looked up in `cur` directly; afterwards the
        // previous layer's check has already confirmed it is there.
        if (g_refWatch && g_refLostAt < 0) {
            if (t == t0 + 1) {
                // The trace's first row is t0+1: the state AT the anchor tick
                // is the anchor itself, which is what the frontier was seeded
                // with, so there is nothing to match. After this layer the
                // watch follows the state it is carrying, not the trace.
                g_refParent = cur.empty() ? -1 : 0;
            }
            if (g_refParent < 0 && t == t0 + 1) {
                // The anchor itself is not in the frontier. That is a fault in
                // the harness (wrong trace, wrong anchor tick), not a finding
                // about the model, and it must not read as one.
                std::printf("refwatch: the reference is not in the FIRST "
                            "frontier (t=%lld) -- wrong trace or wrong anchor, "
                            "not a search result\n", t - 1);
                g_refWatch = false;
            }
            if (t == t0 + 1)
                std::printf("refwatch: armed rows=%zu t0=%lld parent=%d "
                            "frontier=%zu\n",
                            g_refRows.size(), t0, g_refParent, cur.size());
            for (int a = 0; a < 2; ++a) {
                g_refKidFate[a] = -1;
                g_refKidWhy[a] = "";
                g_refKidKey[a] = {};
            }
        }

        // Resolve autonomous fires from the frontier's leading edge. `cur`
        // holds states AT tick t-1, so the first layer whose lead x reaches
        // the trigger's x means the crossing happened at t-1 and the move
        // starts at t-1 + delay (measured). States behind the
        // lead see the move at most a tick or two early -- under 1 px at the
        // slide rates in play, where the recording-timeline error this
        // replaces was measured at 1,707 ticks (see AutoTrig).
        if (!g_autoTrig.empty()) {
            double xLead = -1e18;
            bool haveLead = false;
            for (AutoTrig& A : g_autoTrig) {
                if (A.fireT >= 0) continue;
                if (!haveLead) {
                    for (const State& s : cur)
                        if ((double)s.xAbs > xLead) xLead = (double)s.xAbs;
                    haveLead = true;
                }
                // sorted by cx: the first unfired one that has not been
                // crossed ends the scan (two triggers CAN share an x --
                // lv19 has pairs at 5,955 and 29,265 -- so no early break
                // on a fire)
                if (xLead < A.cx) break;
                // `cur` holds states at t-1, so the crossing is t-1 and the
                // effect lands `delay` ticks later.
                A.fireT = (int)(t - 1) + A.delay;
                A.fireX = A.lockLastX = xLead;
                std::printf("autotrig: uid %d x=%.0f crossed at t=%lld, "
                            "effect t=%d\n",
                            A.uid, A.cx, t - 1, A.fireT);
            }
        }

        // ---- speed groups -------------------------------------------------
        // The frontier is no longer a single x. A state that took a speed
        // portal and one that flew past it are at the same TICK but at
        // different x, and the gap only grows (0.9 vs 1.1 is 0.316 px/tick;
        // on lv18 it reached 1,700 px). So the layer is processed once per
        // distinct speed: each group gets its own object windows, built from
        // that group's own x span, and its own dedupe map.
        // Within a group the x values differ only by stair snaps (sub-pixel to
        // a couple of px), so the windows stay as tight as the shared one was.
        // The previous scheme -- one shared accumulator, switched when ANY
        // state's y overlapped the portal -- is what put the search on
        // displaced geometry.
        // A separate `seen` per group also keeps two speeds from ever merging
        // into one cell, without putting dx in keyOf (which would repartition
        // every single-speed layer in the suite for nothing).
        // ...and by the touch-trigger mask for the same reason (see TouchTrig).
        // A state that flew through the box and one that flew under it disagree
        // about where a door IS, so they cannot share object windows and must
        // not merge in the dedupe. Levels without touch triggers keep exactly
        // one mask (0) and are partitioned exactly as before.
        // ...and by the gameplay FRAME (see RotTrig): two states that turned at
        // different ticks are in different worlds, so they cannot share object
        // windows any more than two speeds can.
        // ...and by REVERSE (id 2899) for the same reason again: a reversed
        // state's x runs the other way, so it cannot share a window with a
        // forward one. frame and rev are separate axes, so both go in the key.
        // NOT by the spent-gravity-portal mask, though it is carried per state
        // the same way `trig` is. What puts `trig` here is that it moves the
        // GEOMETRY: two masks need two sets of object windows. A spent portal
        // moves nothing -- both states see the same box, they only disagree
        // about whether it still fires -- so the mask has no business in a key
        // whose job is to choose windows, and an axis here would be paid for in
        // parent steps: gidx deliberately hands every parent to every group
        // (see the rebind note), so k masks in a layer means stepping each
        // parent k times for k identical children. It goes in keyOf instead,
        // where the separation is exactly as absolute and costs one xor.
        struct GKey {
            // TouchMask, not uint32_t: this key GROUPS states, so a narrower
            // field would merge two states whose only difference is a box above
            // bit 31 -- silently, and only once kTouchBits passes 32.
            // fsig: --groupfire's fire-tick signature (search_key.hpp), 0 without the flag.
            // xc: --groupxsplit's part, 0 without the flag (search_key.hpp).
            float dx; TouchMask trig; uint8_t frame; uint8_t rev; uint64_t fsig; uint16_t xc;
            bool operator==(const GKey& o) const {
                return dx == o.dx && trig == o.trig && frame == o.frame
                       && rev == o.rev && fsig == o.fsig && xc == o.xc;
            }
        };
        std::vector<uint64_t> curSig(g_groupFire ? cur.size() : 0);
        for (size_t i = 0; i < curSig.size(); ++i) curSig[i] = groupFireSig(cur[i], t);
        auto sigOf = [&](size_t i) -> uint64_t { return curSig.empty() ? 0 : curSig[i]; };
        // --groupxsplit: the members of each group in x order, a new part wherever the gap to the
        // previous member is wider than the threshold. Part 0 is the lowest x.
        std::vector<uint16_t> curXc;
        // ...only in a layer where every state is in frame 0 and runs forward. gidx hands a parent
        // of another frame to every group of this one (the note above); splitting changed that
        // (such a parent went to part 0 alone), and on lv22 -- whose layers carry turned frames --
        // the flag took the loop off d008ee2's route at the second iteration, the search's
        // chosen plan dying in its own resim in frame 1 (`resimdie=511@1758/spider/no-target
        // ... f1`). Where a turned or reversed frame is live the layer is processed as before.
        bool xsplitHere = g_groupXSplit && cur.size() > 1;
        for (size_t i = 0; xsplitHere && i < cur.size(); ++i)
            if (cur[i].frame != 0 || cur[i].rev != 0) xsplitHere = false;
        if (xsplitHere) {
            curXc.assign(cur.size(), 0);
            std::vector<GKey> bases;
            std::vector<std::vector<size_t>> members;
            for (size_t i = 0; i < cur.size(); ++i) {
                const State& s = cur[i];
                const GKey b{(s.dx > 0.f) ? s.dx : curDxF, s.trig, s.frame, s.rev, sigOf(i), 0};
                const size_t j = (size_t)(std::find(bases.begin(), bases.end(), b) - bases.begin());
                if (j == bases.size()) {
                    bases.push_back(b);
                    members.emplace_back();
                }
                members[j].push_back(i);
            }
            bool split = false;
            for (size_t j = 0; j < members.size(); ++j) {
                std::vector<size_t>& m = members[j];
                const double gap = 2.0 * kGroupWinMargin + std::fabs((double)bases[j].dx);
                std::sort(m.begin(), m.end(), [&](size_t a, size_t b) {
                    return cur[a].xAbs < cur[b].xAbs || (cur[a].xAbs == cur[b].xAbs && a < b);
                });
                uint16_t c = 0;
                for (size_t k = 1; k < m.size(); ++k) {
                    if ((double)cur[m[k]].xAbs - (double)cur[m[k - 1]].xAbs > gap
                        && c < 0xFFFF)
                        ++c;
                    curXc[m[k]] = c;
                }
                if (c > 0) split = true;
                if ((int)c + 1 > g_groupXSplitMax) g_groupXSplitMax = (int)c + 1;
            }
            if (split) ++g_groupXSplitLayers;
        }
        auto xcOf = [&](size_t i) -> uint16_t { return curXc.empty() ? 0 : curXc[i]; };
        std::vector<GKey> groupDx;
        for (size_t i = 0; i < cur.size(); ++i) {
            const State& s = cur[i];
            const GKey k{(s.dx > 0.f) ? s.dx : curDxF, s.trig, s.frame, s.rev, sigOf(i), xcOf(i)};
            if (std::find(groupDx.begin(), groupDx.end(), k) == groupDx.end())
                groupDx.push_back(k);
        }

        auto emit = [&](State s, const State& from, uint8_t action, const SearchKey& k) {
            auto finish = [&](State& stored) {
                arena.push_back(Node{from.parent | ((uint32_t)action << 31)});
                if (checkWanted) arenaMode.push_back(s.mode);
                s.parent = (uint32_t)(arena.size() - 1);
                s.action = action;
                stored = s;
                // the goal is reached by a STATE, at its own x -- not by the
                // layer's nominal timeline. Only in frame 0: `goalX` is a world
                // x, and in a turned frame xAbs is a different axis entirely
                // (lv22 ends un-rotated, so nothing is lost).
                // The goal is reached by a STATE, and by MORE THAN ONE of them:
                // the layer runs to the end (the loop tests `!solved` on the
                // NEXT iteration), so every other state that got past goalX is
                // emitted too and only loses to this latch. Measured on lv16:
                // 2,000 of them, whose lineages stay distinct for 3,345 ticks.
                // Keep the roomiest instead of the first; with the score off
                // they all read 0 and the incumbent always wins, which is the
                // old latch exactly.
                // --coins: the end of the level is not the goal on its own.
                // A lineage that reached it without the full set is not a
                // solution, and saying so HERE rather than filtering afterwards
                // is what makes the search look for the other one instead of
                // reporting the first thing that got to the end.
                if (s.frame == 0 && (double)s.xAbs >= goalX
                    && (!coinOn || s.coins == (uint8_t)coinAll)
                    && (!solved || roomierRoute(s.tight, goalState.tight))) {
                    solved = true;
                    goalState = s;
                }
            };
            Slots& sl = seen.at(k);
            if (sl.hi < 0) {
                nxt.push_back(State{});
                sl.hi = sl.lo = (int)nxt.size() - 1;
                finish(nxt.back());
            } else if (s.vy > nxt[(size_t)sl.hi].vy + 1e-9) {
                if (sl.hi == sl.lo) {
                    nxt.push_back(State{});
                    sl.hi = (int)nxt.size() - 1;
                }
                finish(nxt[(size_t)sl.hi]);
            } else if (s.vy < nxt[(size_t)sl.lo].vy - 1e-9) {
                if (sl.hi == sl.lo) {
                    nxt.push_back(State{});
                    sl.lo = (int)nxt.size() - 1;
                }
                finish(nxt[(size_t)sl.lo]);
            }
        };

        // "alive dropped" does not say whether the children DIED or were merged
        // away by the dedupe; those need opposite fixes, so count both.
        long long nBorn = 0, nDied = 0;
        // --qfoldwatch: which rotation-queue states this group's dedupe merged
        // away. keyOf has no rotSpent/rotChan/rotRev, so children that differ
        // only in the queue share a cell. For each key, the queue states among
        // the group's live children minus those among what the dedupe kept (the
        // survivors are copies of children, so they key the same). Print only.
        long long qfLost = 0, qfKeys = 0;
        auto qfoldAccount = [&](size_t before) {
            auto qs = [](const State& s) {
                return ((uint64_t)s.rotSpent << 32) | ((uint64_t)s.rotChan << 16)
                       | (uint64_t)s.rotRev;
            };
            auto add = [](std::vector<uint64_t>& v, uint64_t q) {
                if (std::find(v.begin(), v.end(), q) == v.end()) v.push_back(q);
            };
            std::unordered_map<SearchKey, std::vector<uint64_t>, SearchKeyHash> alive, kept;
            for (size_t i = 0; i < kids.size(); ++i)
                if (kidFlag[i] == 2) add(alive[kidKeys[i]], qs(kids[i].s));
            for (size_t j = before; j < nxt.size(); ++j)
                add(kept[keyOf(nxt[j], (long long)t)], qs(nxt[j]));
            for (const auto& [k, a] : alive) {
                const auto it = kept.find(k);
                long long lost = 0;
                for (uint64_t q : a)
                    if (it == kept.end()
                        || std::find(it->second.begin(), it->second.end(), q)
                               == it->second.end())
                        ++lost;
                if (lost) { qfLost += lost; ++qfKeys; }
            }
        };
        const bool qfHere = (g_qfoldLo >= 0 && t >= g_qfoldLo && t <= g_qfoldHi);
        for (const auto& gkey : groupDx) {
        const float gdx = gkey.dx;
        const TouchMask gtrig = gkey.trig;
        const int gframe = (int)gkey.frame;
        const int grev = (int)gkey.rev;
        // this group's own x span, and the windows that cover it for the whole
        // tick (from where its slowest member starts to where it ends up)
        double gLo = 1e18, gHi = -1e18;
        int gFire = -1;   // latest tick any of them fired a touch trigger
        int gFireMin = 0x7fffffff;   // ...and the earliest, for the pass count only
        // ...and the same thing per box, for the formula path. The group key
        // holds `trig`, so every member has entered the SAME set of boxes --
        // what differs is when, and this keeps the latest for each one
        // separately instead of collapsing them onto a single scalar.
        uint16_t gFireB[kTouchBits] = {};
        // ...and how far the least-advanced member has carried a locked
        // platform. Same conservative direction as `gFire`, which takes the
        // LATEST firing tick so the group is shown a door that is still
        // opening: the smallest offset is the one that has ridden least.
        float gLockOff = 1e30f;
        for (size_t ci = 0; ci < cur.size(); ++ci) {
            const State& s = cur[ci];
            if (((s.dx > 0.f) ? s.dx : curDxF) != gdx || s.trig != gtrig
                || (int)s.frame != gframe || (int)s.rev != grev
                || sigOf(ci) != gkey.fsig || xcOf(ci) != gkey.xc) continue;
            gLo = std::min(gLo, (double)s.xAbs);
            gHi = std::max(gHi, (double)s.xAbs);
            gFire = std::max(gFire, (int)s.trigT);
            gFireMin = std::min(gFireMin, (int)s.trigT);
            // gFireB is uint16_t[kTouchBits] -- fold all of it, not the first 32.
            for (int b = 0; b < kTouchBits; ++b)
                if (s.fireB[b] > gFireB[b]) gFireB[b] = s.fireB[b];
            if (s.lockOff < gLockOff) gLockOff = s.lockOff;
        }
        if (gLockOff > 1e29f) gLockOff = 0.f;   // no member (group is empty)
        // The gate's pass count: this group is APPROXIMATED only if its members
        // disagree about when the trigger fired. Counted here, before any of the
        // reasons below can `continue` past it, so "placed" means placed.
        if (gFire >= 0) {
            ++g_gfireGroups;
            if (gFireMin < gFire) {
                ++g_gfireSpread;
                const int d = gFire - gFireMin;
                g_gfireSum += d;
                if (d > g_gfireMax) g_gfireMax = d;
            }
        }
        // Drop a whole group once it is past a box it was required to enter.
        // Killing it here rather than at the end keeps the frontier spent on
        // states that can still satisfy the requirement.
        if (g_needTrig) {
            bool missed = false;
            for (size_t b = 0; b < g_touch.size() && !missed; ++b)
                if ((g_needTrig & ~gtrig) & touchBit(b))
                    missed = gLo > g_touch[b].cx + g_touch[b].hw + 40.0;
            if (missed) continue;
        }
        const double xPrev = gHi;                    // nominal, reports only
        // A reverse (id 2899) group's x decreases. The windows too are laid
        // out along the direction of travel.
        const double sdx = grev ? -(double)gdx : (double)gdx;
        const double x = (grev ? gLo : gHi) + sdx;   // ditto
        const float dxUsed = (float)sdx;
        const double wLo = grev ? (gLo + sdx) : gLo;
        const double wHi = grev ? gHi : (gHi + sdx);
        // GAMEPLAY ROTATION, one shot (see RotTrig::firedT). Spend a trigger the
        // moment this group's advance can reach it. Done here, single-threaded,
        // because applyRotation itself runs on the worker threads and must stay
        // a pure read. All states in a group share dx and start from one x, so
        // "some state crosses this tick" is "every state crosses this tick".
        // Those pointing at the same frame (= reverse toggles) are consumed
        // here too. Excluded, applyRotation reads firedT and can never fire.
        for (RotTrig& r : g_rotTrig) {
            const bool same = (r.frame == gframe);
            if ((same ? r.revT : r.firedT) >= 0) continue;
            const double ru = frameU(gframe, r.cx, r.cy);
            if (grev ? (wLo <= ru && ru < gHi) : (gLo < ru && wHi >= ru)) {
                if (same) r.revT = (int)t; else r.firedT = (int)t;
            }
        }
        // Moving geometry is placed for THIS tick before any window is built.
        // seek() mutates the rects in place, so the pointers the search already
        // holds (snapObj / usedOrb / usedPad) keep pointing at the same object.
        FrameSlices& FS = slicesFor(gframe, wLo);
        Level& LG = *FS.lv;
        LG.dyn.seek((int)t);
        seekOtherFrames(&LG, (int)t);   // --rotpretap only; serial here, before the workers
        // ...and then the doors this group has (or has not) opened. Must come
        // after seek and before any window is built. `gFire` is the LATEST
        // firing tick in the group, so a state that touched the box early is
        // shown a door that is still opening -- conservative, never the other
        // way round.
        // ...plus THIS tick's advance. A lock is a per-tick copy of where the
        // player is NOW, so the geometry placed for tick t needs the player's
        // x at t -- while `gLockOff` comes from states that have only stepped
        // to t-1. Every member shares dx, so the advance is `sdx` for all of
        // them. Measured without it: lv19's platform sits exactly one dx
        // (1.614 px) behind its recording, at every tick.
        LG.dyn.applyTriggers(gtrig, gFire, gFireB,
                             gLockOff + (float)sdx, (int)t, x);
        std::vector<const Obj*> near;
        std::vector<CoinLive> coinLive;   // --coins, filled below the windows
        FS.near->forRange(wLo - 40, wHi + 40, [&](const Obj& o) { near.push_back(&o); });
        LG.dyn.collect(Dynamics::NEAR, wLo - 40, wHi + 40, near);
        // --trigdbg <tick>: the mask this GROUP applied, and where it left the
        // object named by --hazdbg. The resim prints the same pair from the
        // state's own mask (`resimwho:`), so the two lines can be put side by
        // side -- which is the only way to say "at this tick the two masks
        // differ" rather than "they could differ".
        if (g_trigDbgT >= 0 && (long long)t == g_trigDbgT) {
            const Obj* seen = nullptr;
            if (g_hazDbgUid >= 0)
                for (const Obj* o : near)
                    if (o->uid == g_hazDbgUid) { seen = o; break; }
            // gFireB is an ARRAY: this line used to print `(unsigned)gFireB`, the low
            // bits of its address. It prints the group's per-box ticks now (the
            // latest over the members, which is what applyTriggers is handed).
            std::printf("trigdbg t=%lld gtrig=0x%08x gFire=%d lockOff=%.3f "
                        "win=[%.1f,%.1f] n=%zu uid=%d %s gFireB=",
                        (long long)t, (unsigned)gtrig.word(0), gFire,
                        (double)(gLockOff + (float)sdx), wLo, wHi, near.size(),
                        g_hazDbgUid,
                        seen ? "" : "NOT-IN-WINDOW");
            for (int b = 0; b < kTouchBits; ++b)
                if (gtrig.test(b)) std::printf("%d:%u,", b, (unsigned)gFireB[b]);
            std::printf("\n");
            if (seen)
                std::printf("trigdbg t=%lld uid=%d at (%.3f,%.3f) hw=%.3f hh=%.3f\n",
                            (long long)t, seen->uid, seen->cx, seen->cy,
                            seen->hw, seen->hh);
        }
        std::vector<const Obj*> ports;
        FS.port->forRange(wLo - 60, wHi + 60, [&](const Obj& o) { ports.push_back(&o); });
        LG.dyn.collect(Dynamics::PORT, wLo - 60, wHi + 60, ports);
        std::vector<const Obj*> pads;
        FS.pad->forRange(wLo - 40, wHi + 40, [&](const Obj& o) { pads.push_back(&o); });
        LG.dyn.collect(Dynamics::PAD, wLo - 40, wHi + 40, pads);
        std::vector<const Obj*> orbs;
        FS.orb->forRange(wLo - 50, wHi + 50, [&](const Obj& o) { orbs.push_back(&o); });
        LG.dyn.collect(Dynamics::ORB, wLo - 50, wHi + 50, orbs);
        std::vector<const Obj*> slps;
        FS.slope->forRange(wLo - 60, wHi + 60, [&](const Obj& o) { slps.push_back(&o); });
        LG.dyn.collect(Dynamics::SLOPE, wLo - 60, wHi + 60, slps);
        std::vector<const Obj*> spds;
        FS.speed->forRange(wLo - 80, wHi + 80, [&](const Obj& o) { spds.push_back(&o); });
        LG.dyn.collect(Dynamics::SPEED, wLo - 80, wHi + 80, spds);
        // touch-trigger boxes this group could reach this tick, with their bits
        std::vector<std::pair<const TouchTrig*, TouchMask>> trigs;
        // ...in THIS group's frame (see touchFor): a world-coordinate box is
        // unreachable once the frame turns, which silently shut every door in
        // lv22's rotated sections.
        const std::vector<TouchTrig>& tfg = touchFor(gframe);
        for (size_t b = 0; b < tfg.size(); ++b) {
            const TouchTrig& T = tfg[b];
            if (gtrig & touchBit(b)) continue;   // already fired
            // A STANDING COUNT DOES NOT STOP LISTENING WHEN IT SCROLLS OFF.
            // Count (1611) is the standing form -- the file already says so at
            // the test that makes 1811 ask once -- but the window dropped it
            // the moment the player was 40 px past, so it was only ever asked
            // while it was nearby. On lv22's first coin that is fatal to the
            // whole mechanism: the gate `item 2 == 5` sits at x=2,835 and every
            // box that feeds item 2 is at 3,195..3,674, so the player crosses
            // the gate with a count of 0, and by the time the count rises the
            // gate is no longer in this list. The coin could never be raised,
            // which is why every plan collects it at its load position.
            //
            // Only the BEHIND half of the window goes. A gate the player has
            // not reached yet is still skipped, so this only ever adds
            // occasions to ask a question whose answer was already false.
            // An Item Compare root (3620) is asked from anywhere for the same
            // reason: what fires it is a press, not the player's x.
            const bool standing =
                (T.id == 1611 || T.id == 3620) && T.count >= 0 && !T.tap;
            // ...and an Item Compare is not ahead either: nothing about it is a
            // place, and in a turned frame its x is not even on this axis.
            const bool anywhere = T.id == 3620 && T.count >= 0;
            if (!anywhere && !standing && T.cx + T.hw < wLo - 40) continue;
            if (!anywhere && T.cx - T.hw > wHi + 40) continue;
            trigs.push_back({&T, touchBit(b)});
        }
        seen.ensure(cur.size() * 2 + 64);
        seen.clear();   // per GROUP, not per layer (see the note above)
        (void)x; (void)xPrev;
        // --coins: this group's coins where THEY are now. A controlled coin is
        // read out of the dyn rows the applyTriggers above has just placed (and
        // is absent while its group is switched off, which is what GD does with
        // it); an uncontrolled one is its load-time row. Frame 0 only, like the
        // collect in the step: in a turned frame xAbs is a different axis.
        // ...and in a TURNED frame, only the coins that never move
        // (coinAnyFrame), turned into this frame the way the geometry is.
        if (coinOn && gframe != 0)
            for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                if (!coinAnyFrame[ci] || !(gtrig & coinEnable[ci])) continue;
                const Obj& c = L.coins[ci];
                double u = 0.0, v = 0.0;
                toFrame(gframe, c.cx, c.cy, u, v);
                const bool odd = (gframe & 1) != 0;
                coinLive.push_back({u, v, odd ? c.hh : c.hw, odd ? c.hw : c.hh, (int)ci});
            }
        if (coinOn && gframe == 0) {
            for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                const int di = ci < L.coinDyn.size() ? L.coinDyn[ci] : -1;
                // A coin an Item Compare switches on is live exactly when this
                // group's gate has fired (see coinEnable), wherever it sits.
                if (coinEnable[ci]) {
                    if (gtrig & coinEnable[ci]) {
                        const Obj& c = di < 0 ? L.coins[ci] : LG.dyn.objs[(size_t)di];
                        coinLive.push_back({c.cx, c.cy, c.hw, c.hh, (int)ci});
                    }
                    continue;
                }
                if (di < 0) {
                    const Obj& c = L.coins[ci];
                    coinLive.push_back({c.cx, c.cy, c.hw, c.hh, (int)ci});
                } else if (LG.dyn.on[(size_t)di]) {
                    const Obj& c = LG.dyn.objs[(size_t)di];
                    coinLive.push_back({c.cx, c.cy, c.hw, c.hh, (int)ci});
                }
            }
            // --coindbg <tick>: where THIS group put each coin, and which ones
            // its mask left switched off. The mod prints the same thing from
            // GD (`coinlive:`), so the two can be put side by side -- which is
            // the only way to say "at this tick the model and GD disagree about
            // where the coin is" rather than "they could".
            if (g_coinDbgT >= 0 && (long long)t == g_coinDbgT) {
                for (const CoinLive& C : coinLive)
                    std::printf("coindbg: t=%lld gtrig=0x%08llx coin=%d live=(%.3f,%.3f)"
                                " box %.1fx%.1f load=(%.3f,%.3f)\n",
                                (long long)t, (unsigned long long)gtrig.word(0),
                                C.bit, C.cx, C.cy,
                                C.hw * 2, C.hh * 2,
                                L.coins[(size_t)C.bit].cx, L.coins[(size_t)C.bit].cy);
                for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                    bool live = false;
                    for (const CoinLive& C : coinLive)
                        if ((size_t)C.bit == ci) live = true;
                    if (!live)
                        std::printf("coindbg: t=%lld gtrig=0x%08llx coin=%zu OFF"
                                    " (its group is switched off)\n",
                                    (long long)t, (unsigned long long)gtrig.word(0), ci);
                }
            }
        }
        // Which ramps can veto which solid (SlopeVetoIndex): the same for every
        // state of the group, so worked out here once.
        vetoIdx.build(near, slps);
        std::vector<const Obj*> nearSolid;   // --solidorder: near in GD's solid order, once per group
        solidOrderInto(near, nearSolid);
        const StepCtx K{x, xPrev, dxUsed, t, &near, &ports, &pads, &orbs, &slps, &spds, &SP, &SPmini, &UP, &UPmini, &trigs, &coinLive, &vetoIdx,
                        &nearSolid};
        ppMark(0);
        // ---- phase 1: STEP every state of this group (parallel) -------------
        // Stepping is pure -- it reads the shared windows and writes only its
        // own child -- so it parallelises exactly. The dedupe, the arena and
        // `nxt` stay on the main thread in phase 2, walked in the SAME order as
        // the serial loop was, which keeps the emitted plan bit-identical.
        gidx.clear();
        for (size_t i = 0; i < cur.size(); ++i)
            if (((cur[i].dx > 0.f) ? cur[i].dx : curDxF) == gdx
                && cur[i].trig == gtrig && sigOf(i) == gkey.fsig
                // --groupxsplit: a member of this frame and direction goes to its own part only.
                // A parent from another frame (gidx hands those to every group, see above) keeps
                // being stepped once per frame group, in its part 0.
                && (curXc.empty()
                    || (((int)cur[i].frame == gframe && (int)cur[i].rev == grev)
                            ? curXc[i] == gkey.xc
                            : gkey.xc == 0)))
                gidx.push_back(i);
        // Airborne cube input is normally moot, so only one child is expanded --
        // but NOT next to an orb, where a press in mid-air is the whole point.
        // Leaving this unconditional silently threw away every orb activation
        // (lv3: 0 boosts in 35k expansions). Decided per state up front so that
        // slot 2i+1 either holds that state's second child or nothing.
        // ...and NOT while DASHING, where the input is the only thing holding the
        // player up. A dash ends the tick after the button comes off (the
        // `s.dashing && s.action` rule in the step), so pruning the pressed child
        // means the frontier can never keep a dash alive further than 50 px past
        // the last orb in the window -- the dash is forcibly released the moment
        // the ring leaves the window, whatever the level needs.
        // Measured on lv21 (2026-08-10): the dash ring at (2445,229) is the only
        // way across the pit at x=2550..2700 (no floor at all, spikes at y<=181).
        // At t=1966 the layer expanded 1,402 pressed children; at t=1967 it
        // expanded ZERO, so every dashing state was released at t=1968 (x=2,557)
        // and fell into the pit. GD with the same press held to t=2200 reaches
        // x=2,996. The frontier collapse the previous session blamed on
        // granularity was this: the winning branch was never enumerated.
        kids.assign(gidx.size() * 2, Child{});
        kidKeys.assign(gidx.size() * 2, SearchKey{});
        kidFlag.assign(gidx.size() * 2, 0);
        const bool dbgHere = (dbgLayers > 0 && t - t0 <= dbgLayers);
        const bool orbsEmpty = orbs.empty();
        // --twinskip / --twinaudit (search_census.hpp): is this parent's pressed child a twin
        // of its released one? Only the airborne ground modes, whose button does nothing in
        // the air except through the things excluded here: something the press fires (a
        // ring or portal close enough to matter this tick, a toggle block, a tap or
        // the counting tap), a hold the release would change (ringHold / pressSpent /
        // holdDead, the robot's hover, a dash), and GD's mid-air coyote jump (ogLinger).
        // Off wherever an instrument reads the pressed child itself.
        bool groupPressTrig = false;
        for (const auto& tb : trigs)
            if (tb.first->tap || tb.first->press) { groupPressTrig = true; break; }
        // --heldall writes the button into `held` in every mode, and keyOf keys `held` in every
        // mode, so under it the two children part in the key and the pressed one is no longer
        // deduped away behind its released twin: it is kept, and it has to be the child stepping
        // would have made. The fill below sets what the button wrote (held, the jump buffer) and
        // takes the key again; --twinaudit checks that against the stepped child.
        const bool twinOn = !dbgHere && !g_refWatch
                            && !g_clearProbe && !qfHere && g_coinDbgT < 0 && !g_airPress
                            && false   // --latgap was always on, and the twin skip never ran with it
                            && !(g_rotWatchLo >= 0 && t >= g_rotWatchLo && t <= g_rotWatchHi);
        kidMoot.assign(twinOn ? gidx.size() : 0, 0);
        auto pressMoot = [&](const State& s) -> bool {
            if (s.dual || s.grounded || s.onSlope) return false;
            if (s.mode != 0 && s.mode != 2 && s.mode != 5 && s.mode != 6) return false;
            if ((int)s.frame != gframe || (int)s.rev != grev) return false;
            if (s.ringHold || s.pressSpent || s.rHover || s.dashing || s.ogLinger || s.holdDead)
                return false;
            // Inside the flip grace a head-side contact seats the body (step.hpp, flipGraceSeat),
            // and whether it does reads the press: a ball's tap after the collision pass keeps the
            // window open, so the pair can part there. No corpus state has shown it on this path;
            // the gravity pad below is the same window opened on the tick itself, and it has.
            if ((int)s.flipT + 1 < kFlipGraceTicks) return false;
            if (nearPressBox(s)) return false;
            if (tapGive.bit && !(s.trig & tapGateBits) && !(s.taps & 0x40)) return false;
            // A fixup record is matched per INPUT (fixup.hpp fixupMatches), so one landing on
            // either child splits the pair -- measured in a loop call of a custom level at
            // t=4,325, an upside-down ball whose released child took a +2.83 vy record.
            // Any record within a few px of the parent's x rules the pair out.
            for (const std::vector<Fixup>* fv : {&g_fixupKills, &g_fixupDeltas}) {
                const float lo = s.xAbs - 8.f;
                const auto it = std::lower_bound(
                    fv->begin(), fv->end(), lo,
                    [](const Fixup& f, float xx) { return f.x < xx; });
                if (it != fv->end() && it->x <= s.xAbs + 8.f) return false;
            }
            const double px = (double)s.xAbs, py = (double)s.y;
            const double rx = 60.0 + 2.0 * std::fabs((double)K.dxF);
            const double ry = 60.0 + std::fabs((double)s.vy);
            auto within = [&](const std::vector<const Obj*>& v) {
                for (const Obj* o : v)
                    if (std::fabs(o->cx - px) <= o->hw + rx && std::fabs(o->cy - py) <= o->hh + ry)
                        return true;
                return false;
            };
            // Rings: the contact test reads this tick's and the previous tick's position, and a
            // rotated ring reaches 18 + 15*sqrt(2) = 39.2 px along its axis; the margin covers
            // both. Portals: a mode portal can hand the press to the new mode on its tick
            // (--portalpress). Pads read no input in the air, so they are not a reason.
            // Ramps: a contact made on this tick goes through slopeNudge, which reads the
            // input (a custom level t=15,834, an airborne cube at vy 0: released -2.000, pressed
            // -0.216).
            // A gravity pad (type 10) flips the body in the collision pass, which opens the flip
            // grace above from any flipT (flipGraceSeat's age 0), so one in reach rules the pair out
            // too. Gravity portals are among `ports` already. The twinaudit witnesses are this
            // path: lv11 t=8,968 and lv12 t=18,838, a ball whose pressed child is seated at vy 0
            // while the released one leaves at -3.072.
            for (const Obj* o : pads)
                if (o->type == 10 && std::fabs(o->cx - px) <= o->hw + rx
                    && std::fabs(o->cy - py) <= o->hh + ry)
                    return false;
            return !within(orbs) && !within(ports) && !within(slps);
        };
        // --inputgrid / --gridmap: this layer's grid, from the leader x of the layer before it
        // (the x --capmap reads, so a ladder window lifts both over the same stretch).
        const int gridNow = gridAtX(bestX);
        auto stepKidAt = [&](size_t i, bool mayTwin) {
            const State& s = cur[gidx[i >> 1]];
            const int input = (int)(i & 1);
            // --minpulse / --inputgrid (search_census.hpp): may the button change on this tick?
            const bool edgeLocked =
                (g_minPulse > 1 && (int)s.edgeAge + 1 < g_minPulse)
                || (gridNow > 1 && (K.t % gridNow) != 0);
            if (edgeLocked && input != (int)s.action) return;
            // ...and NOT next to a toggle block it has not fired (nearPressBox), which the
            // button fires: SubZero 4003's uid 4001 is passed by an airborne cube, so with
            // the pressed child pruned no state ever entered it (2026-09-24).
            // ...and NOT under --airpress (g_airPress): a press held from the air into a
            // landing is a different jump from one pressed on the landing tick.
            // ...and not when the input rule has just forbidden the release: then the held child
            // is the only one left.
            if (input == 1 && s.mode == 0 && !s.grounded && orbsEmpty
                && !s.dashing && !(s.dual && s.dashing2)   // --dualdash
                && !nearPressBox(s) && !g_airPress
                && !(edgeLocked && s.action == 1))
                return;
            // --latgap: no edge on a tick GD cannot put one (frames.hpp g_latGap)
            if (s.latLock && input != (int)s.action)
                return;
            // --twinskip: decided after the step pass, from the released sibling (slot i-1):
            // copied in when that one stayed in the air, stepped for real otherwise.
            if (input == 1 && mayTwin && twinOn && pressMoot(s)) {
                if (!g_twinAudit) {
                    kidFlag[i] = 3;
                    return;
                }
                kidMoot[i >> 1] = 1;
            }
            Child& kid = kids[i];
            bool dead = false;
            kid.s = stepBoth(s, input, K, dead);
            // GAMEPLAY ROTATION: turn the child if this tick crossed one. Must
            // happen BEFORE keyOf below -- the turn rewrites x, y, vy and the
            // frame, and a key taken in the old frame would put two different
            // worlds in one cell.
            if (!dead && !g_rotTrig.empty()) {
                const bool rw = (g_rotWatchLo >= 0 && K.t >= g_rotWatchLo && K.t <= g_rotWatchHi);
                if (rw) g_rotWatchUid = -1;
                const int f0R = (int)kid.s.frame;
                const int nfR = applyRotation(kid.s, (double)s.xAbs, (double)K.dxF, K.t,
                                              input, s.grounded != 0, (double)s.y, true);
                if (rw && nfR >= 0) {   // --rotwatch: print only, read in the serial pass below
                    kid.rotUid = g_rotWatchUid;
                    kid.rotF0 = (int8_t)f0R;
                    kid.rotNf = (int8_t)nfR;
                }
            }
            // The child's own action, set HERE and not only in emit()'s
            // finish(): keyOf reads it (for the robot's hover) and the key is
            // computed on this thread, before phase 2 runs. Without this the
            // key saw the PARENT's action and the held and released children
            // hashed to the same cell -- which is the whole bug the rHover
            // note in keyOf describes.
            kid.s.action = (uint8_t)input;
            if (g_minPulse > 1)
                kid.s.edgeAge = (input != (int)s.action)
                                    ? 0 : (uint8_t)std::min(254, (int)s.edgeAge + 1);
            {
                const auto lat2 = [](uint8_t m) { return m == 1 || m == 3; };
                kid.s.latLock = (lat2(kid.s.mode) && !lat2(s.mode) && !kid.s.dual) ? 1 : 0;
            }
            // ---- --coins: collect, then kill whoever left one behind --------
            // After the step and after any rotation, so the position tested is
            // the one the tick ended at, in the frame the coin's world
            // coordinates mean something. In the frame coinLive was built in
            // (the group's): frame 0 for every coin, a turned frame only for
            // the coins that never move (coinAnyFrame), whose rows are turned
            // into it. A tick that turns the player skips the test, as before.
            if (coinOn && !dead && kid.s.frame == gframe) {
                const double ph =
                    hazardHalfFor(kid.s.mode, kid.s.mini != 0);
                const double px = (double)kid.s.xAbs, py = (double)kid.s.y;
                // COLLECTING reads the live rows (coinLive): a coin its group
                // has moved is tested where it is now, and one the group has
                // switched off is not in the list at all -- GD credits neither
                // a moved coin at its old place nor a disabled one.
                for (const CoinLive& C : coinLive) {
                    const uint8_t bit = (uint8_t)(1u << C.bit);
                    if (kid.s.coins & bit) continue;
                    // --coindbg: how close anything got. Recorded BEFORE the
                    // collect test and only inside the x bound, so a coin that
                    // is never collected still says which of the three reasons
                    // it was (see g_coinNearDy).
                    // (frame 0 only: the probe's x is a world x)
                    if (g_coinDbgT >= 0 && gframe == 0 && px > g_coinProbeMaxX)
                        g_coinProbeMaxX = px;
                    if (g_coinDbgT >= 0 && gframe == 0
                        && std::fabs(px - C.cx) <= C.hw + ph) {
                        ++g_coinSeen[C.bit];
                        const double dy = std::fabs(py - C.cy);
                        double& best = g_coinNearDy[C.bit];
                        if (best < 0.0 || dy < best) {
                            best = dy;
                            g_coinNearX[C.bit] = px;
                            g_coinNearY[C.bit] = py;
                            g_coinNearT[C.bit] = (long long)t;
                        }
                    }
                    // Touching counts (measured: exactly on the boundary is
                    // credited), hence <= and not <.
                    // The margin is an allowance for THIS model's own error, so
                    // it must never eat the window it is protecting: a wave's
                    // coin bound is 25 px against a cube's 35, and the learnt 8
                    // takes a third of it -- measured on lv21, where the search
                    // then had no route left it could call a collection. A
                    // quarter of the bound is the cap.
                    const double mx = std::min(kCoinMargin, (C.hw + ph) * 0.25);
                    const double my = std::min(kCoinMargin, (C.hh + ph) * 0.25);
                    if (std::fabs(px - C.cx) <= C.hw + ph - mx
                        && std::fabs(py - C.cy) <= C.hh + ph - my) {
                        // --coindbg: WHERE the model decided it had one, once
                        // per coin. "The game refused a coin the plan claimed"
                        // names the coin and nothing else, so without this the
                        // claim's own position is only inferable -- and the two
                        // sides were compared at different ticks twice before.
                        if (g_coinDbgT >= 0 && !g_coinSaid[C.bit]) {
                            g_coinSaid[C.bit] = 1;
                            // ...with vy and the mode, because the next question
                            // this line gets asked is always "does the game
                            // survive from there", and that is an injection
                            // which needs the whole state. Without vy the
                            // probe has to guess it, and a cube dropped at
                            // this coin with vy=0 is inside the spikes three
                            // ticks later -- which says nothing about the plan.
                            std::printf("coincollect: t=%lld coin=%d player=(%.3f,%.3f)"
                                        " vy=%.4f mode=%d mini=%d frame=%d"
                                        " live=(%.3f,%.3f) |dx|=%.3f |dy|=%.3f"
                                        " bound=(%.3f,%.3f)\n",
                                        (long long)t, C.bit, px, py,
                                        (double)kid.s.vy, (int)kid.s.mode,
                                        (int)kid.s.mini, gframe, C.cx, C.cy,
                                        std::fabs(px - C.cx), std::fabs(py - C.cy),
                                        C.hw + ph - mx, C.hh + ph - my);
                        }
                        kid.s.coins |= bit;
                    }
                }
                // MISSING is judged on the load-time x instead, for the two
                // reasons the live row cannot serve: a switched-off coin has no
                // live row at all (and must still be counted as passed), and
                // the bound has to be the same one the game side uses to end an
                // attempt (hooks_gamelayer.cpp). Every coin this corpus moves is
                // moved in y only, so the two agree; a coin carried in x would
                // need its own bound and does not exist here.
                //
                // ...AND ONLY WHERE THE PLAYER CANNOT COME BACK. The whole
                // verdict rests on x growing for ever, which a level with
                // rotated gameplay breaks outright: lv22 travels -x through its
                // maze and its third coin sits at x=16,097, inside it. There the
                // prune would kill a lineage that has merely not turned round
                // yet. Without it the search only loses the early exit -- the
                // goal still is not reached without every coin.
                // `dead` and no early return: the bookkeeping below (flags,
                // key, the dbg reason) is the same for this death as for a
                // physical one, and duplicating it is how the two drift.
                // ---- the PICKUP ITEMS, and the Count triggers they fire -----
                // Same test as a coin's (the player's own box against the
                // object), and state for the same reason: lv21's third coin
                // only comes into reach once ten of item 1 have been taken.
                // The trigger they fire takes a bit in the SAME mask a touch
                // box does, so the chain, the fire tick and the dedupe key are
                // machinery that already exists.
                //
                // AND THE COUNT LOOP BELOW LIVES INSIDE THIS BLOCK, which was
                // opened only when there are pickups. It was written for lv21,
                // whose gate is fed by collectibles; on a level whose counter is
                // fed by CHAINS instead there are no pickups at all, the block
                // is skipped, and no Count trigger is ever asked anything. That
                // is lv22's first coin exactly: its three item-2 rows all have
                // pickup=0, g_collect is empty, and `item 2 == 5` never fired
                // even with every feeder forced -- a --coindbg print inside the
                // loop produced zero lines for any Count on that level. Opened
                // for either source now; the pickup loop just below is a no-op
                // when g_collect is empty, so a level with neither is unchanged
                // and a level with pickups takes the same path as before.
                // Frame 0 only, as everything in this block was before the coin
                // collect above learnt the turned frames: the pickups are world
                // rows, and the counters are asked where they always were.
                // The exception is an Item Compare gate (cmode 3): it reads a
                // count and nothing else, and lv22's third coin is collected in
                // frame 3, where an anchored call has to open it from GD's own
                // counter (--itembase) or never see the coin at all.
                itemGates(kid.s, s.action, input, K, gframe, px, py, ph, t);
                // ...and a gated coin passed for good (gatedMask, tapMissX/Y):
                // once its window has resolved, past the coin's far edge on the
                // shut channel without it. Same reading of "passed" as the
                // window's own ends.
                if (!dead && gatedMask
                    && ((kid.s.trig & tapGateBits) || (kid.s.taps & 0x40))) {
                    double wx = 0.0, wy = 0.0;
                    fromFrame(gframe, (double)kid.s.xAbs, (double)kid.s.y, wx, wy);
                    for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                        if (!((gatedMask >> ci) & 1) || (kid.s.coins & (uint8_t)(1u << ci)))
                            continue;
                        if (passedOn(kid.s, gframe, tapGive.closeChan, wx, wy,
                                     tapMissX[ci], tapMissY[ci])) {
                            dead = true;
                            break;
                        }
                    }
                }
                // ...a coin whose group is OFF and whose every switch-on box is
                // behind the player (coinGate, built at load). "Off right now"
                // is read from the live rows: a switched-off coin is not in
                // them. Nothing to learn from flying the rest of the level.
                // Frame 0 only, and for the same reason as the miss test: `px`
                // is a world Y in a turned frame, and coinLive is empty there,
                // so every coin would read as "off, and the switch-ons are all
                // behind" and be killed on a comparison between two axes.
                for (size_t gi = 0; gframe == 0 && gi < coinGate.size(); ++gi) {
                    const CoinOnGate& G = coinGate[gi];
                    if (kid.s.coins & (uint8_t)(1u << G.coinIdx)) continue;
                    bool liveNow = false;
                    for (const CoinLive& C : coinLive)
                        if (C.bit == G.coinIdx) { liveNow = true; break; }
                    if (liveNow) continue;
                    bool canStill = false;
                    for (const auto& on : G.ons)
                        if ((kid.s.trig & on.first) || px <= on.second) {
                            canStill = true;
                            break;
                        }
                    if (!canStill) { dead = true; break; }
                }
                // ...and a coin that only an Item Compare on the counting tap's
                // item switches on, once that tap's window has shut with the
                // gate unfired: the Stop at its end halts the tap itself (lv22's
                // uid17958 sits in the Stop's group 535), nothing else feeds the
                // item (tapSole), and the gate reads only the count -- so the
                // coin stays off for good. Without this the branches that
                // pressed too few times fly on to the coin and only the goal
                // test tells them apart, a whole frontier later.
                if (!dead && tapSole && (kid.s.taps & 0x40)
                    && !(kid.s.trig & tapGateBits))
                    for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                        if (!coinEnable[ci] || (coinEnable[ci] & ~tapGateBits)) continue;
                        if (kid.s.coins & (uint8_t)(1u << ci)) continue;
                        dead = true;
                        break;
                    }
                // ...and the COUNTER a coin needs (coinReq, built at load).
                // A counter never decreases, so once `have + ahead < need` the
                // coin can never come into reach again and the branch is done.
                // This is the prune the miss test could not be: it fires at the
                // last feeder the branch could still have taken, not at the
                // coin, which on lv21 is 2,759 px and a whole cap's worth of
                // frontier earlier.
                for (size_t ri = 0; gframe == 0 && ri < coinReq.size(); ++ri) {
                    const CoinCountReq& R = coinReq[ri];
                    if (kid.s.coins & (uint8_t)(1u << R.coinIdx)) continue;
                    int have = 0, ahead = 0;
                    for (const auto& ib : g_itemBase)
                        if (ib.first == R.item) have += ib.second;
                    for (size_t ii = 0; ii < g_collect.size() && ii < 16; ++ii) {
                        if (g_collect[ii].item != R.item) continue;
                        if (kid.s.items & (uint16_t)(1u << ii)) ++have;
                        else if (g_collect[ii].cx > px) ++ahead;
                    }
                    if (have + ahead < R.need) { dead = true; break; }
                }
                // ONLY IN FRAME 0, and that is not a detail: px is the frame's
                // travel coordinate, so in a turned frame it is a world Y and
                // comparing it with the coin's world x kills branches for a
                // reason that does not exist. The old form could skip the guard
                // because it only ran on levels that never turn.
                for (size_t ci = 0; !dead && gframe == 0 && ci < L.coins.size(); ++ci) {
                    if (!coinPruneOk[ci]) continue;
                    if (kid.s.coins & (uint8_t)(1u << ci)) continue;
                    const Obj& C = L.coins[ci];
                    // x only ever grows in frame 0, and coinPruneOk says every
                    // later return to frame 0 is past this coin, so it is final.
                    if (px > C.cx + C.hw + ph) { dead = true; break; }
                }
            }
            // Route tightness rides along with the child (State c = s copies it
            // already, so this only adds this tick's own contribution). Read by
            // the goal pick below; nothing else looks at it.
            if (!dead && kid.s.tight < 0xffff
                && tightHere(near, (double)kid.s.xAbs, (double)kid.s.y,
                             playerHalf(kid.s.mode, kid.s.mini != 0), kTightPx))
                ++kid.s.tight;
            kid.dead = dead ? 1 : 0;
            kid.valid = 1;
            // the dedupe key is a pure function of the child, so it belongs on
            // this thread rather than in the serial phase
            if (!dead) kid.key = keyOf(kid.s, (long long)t);
            // fireB's invariant, checked where every surviving child passes.
            // (i) is invisible to every replay harness -- dedupe is not on the
            // replay path -- so "quick_regress is byte-identical" says the
            // change was not SEEN, not that it is right. This is the direct
            // witness that the array is written where it should be: a tick
            // present for a box the state never entered, or a box entered with
            // no tick, is an implementation defect and nothing else would
            // catch it.
            if (!dead && g_fireBCheck) {
                for (int b = 0; b < kTouchBits; ++b) {
                    const bool bit = kid.s.trig.test(b);
                    const bool has = kid.s.fireB[b] != 0;
                    if (bit && !has) ++g_fireBNoTick;
                    if (!bit && has) ++g_fireBNoBit;
                    // ...and against an INDEPENDENT tick. g_touchFireT[b] is
                    // written in a different scope (level-wide, first entry
                    // only), so "no state entered box b before the first time
                    // any state entered box b" is a real check on the index and
                    // the value rather than a restatement of the line that
                    // wrote them. An anchored run seeds fireB from --start, so
                    // those may legitimately predate it and are skipped.
                    if (bit && has && g_touchFireT[b] >= 0
                        && (int)kid.s.fireB[b] < g_touchFireT[b]
                        && !init.trig.test(b))
                        ++g_fireBTooEarly;
                }
            }
            // Side arrays let phase 2p scan keys without loading the full Child.
            kidKeys[i] = kid.key;
            kidFlag[i] = dead ? 1 : 2;
            if (dbgHere || g_refWatch) { kid.why = g_deadWhy; kid.obj = g_deadObj; }
        };
        auto stepKid = [&](size_t i) { stepKidAt(i, true); };
        // --dbg stays serial: it prints per child, in order, and the reason
        // globals are per thread.
        if (pool && !dbgHere && kids.size() >= kParallelMin)
            pool->parallelFor(kids.size(), stepKid);
        else
            for (size_t i = 0; i < kids.size(); ++i) stepKid(i);
        // --twinskip: fill each skipped pressed slot with its released sibling, which is what
        // stepping it would have produced in everything the dedupe and the counts read.
        // --twinaudit: both were stepped; check that claim instead.
        // The claim needs the released sibling to have STAYED in the air, alive: a body that
        // lands on this tick jumps on this tick if the button is down (measured on lv22 t=372:
        // released vy 0.000, pressed 11.180, same y), and a death is not worth reasoning about.
        if (twinOn) {
            auto airborne = [&](size_t a) {
                const Child& k = kids[a];
                return kidFlag[a] == 2 && !k.s.grounded && !k.s.onSlope;
            };
            std::vector<size_t> redo;   // pending pressed slots whose sibling did not qualify
            for (size_t j = 0; j < gidx.size(); ++j) {
                const size_t a = 2 * j, b = a + 1;
                const State& p = cur[gidx[j]];
                const int m = p.mode < 8 ? p.mode : 0;
                // The pressed child as the fill makes it from the released one. Without --heldall
                // the two share a key and the pressed one never survives the dedupe, so only the
                // key is carried; under it the button's own writes are set too and the key taken
                // again (the note above twinOn).
                auto pressedFrom = [&](const Child& rel) {
                    Child k = rel;
                    k.s.action = 1;
                    if (g_minPulse > 1)
                        k.s.edgeAge = (p.action != 1)
                                          ? 0 : (uint8_t)std::min(254, (int)p.edgeAge + 1);
                    if (g_heldAll) {
                        k.s.held = 1;
                        k.s.jumpBuf = 1;
                        k.key = keyOf(k.s, (long long)t);
                    }
                    return k;
                };
                if (g_twinAudit) {
                    if (!kidMoot[j] || !airborne(a)) continue;
                    ++twin.predicted;
                    ++twin.byMode[m];
                    const int fa = kidFlag[a], fb = kidFlag[b];
                    bool ok = fa == fb;
                    if (ok && fa == 2) {
                        if (g_heldAll) {
                            const Child pk = pressedFrom(kids[a]);
                            ok = pk.key == kidKeys[b] && pk.s.vy == kids[b].s.vy
                                 && pk.s.y == kids[b].s.y && pk.s.trig == kids[b].s.trig
                                 && pk.s.held == kids[b].s.held
                                 && pk.s.jumpBuf == kids[b].s.jumpBuf;
                        } else {
                            ok = kidKeys[a] == kidKeys[b] && kids[a].s.vy == kids[b].s.vy
                                 && kids[a].s.trig == kids[b].s.trig;
                        }
                    }
                    if (!ok)
                        twin.violation(t, p, fa, fb, kids[a].s, kids[b].s, kidKeys[a],
                                       kidKeys[b]);
                } else if (kidFlag[b] == 3) {
                    if (!airborne(a)) {
                        kidFlag[b] = 0;
                        redo.push_back(b);
                        continue;
                    }
                    kids[b] = pressedFrom(kids[a]);
                    kidKeys[b] = g_heldAll ? kids[b].key : kidKeys[a];
                    kidFlag[b] = kidFlag[a];
                    ++twin.skipped;
                    ++twin.byMode[m];
                }
            }
            if (!redo.empty()) {
                auto stepRedo = [&](size_t k) { stepKidAt(redo[k], false); };
                if (pool && redo.size() >= kParallelMin)
                    pool->parallelFor(redo.size(), stepRedo);
                else
                    for (size_t k = 0; k < redo.size(); ++k) stepRedo(k);
            }
        }
        ppMark(1);
        // --searchcensus: did the two inputs of each parent lead anywhere different? Print only.
        if (g_searchCensus > 0)
            for (size_t j = 0; j < gidx.size(); ++j)
                census.siblings(cur[gidx[j]], kidFlag[2 * j], kidFlag[2 * j + 1], kids[2 * j].s,
                                kids[2 * j + 1].s, kidKeys[2 * j], kidKeys[2 * j + 1],
                                (long long)t);
        // --rotwatch: the rotations this group's children actually took, counted
        // serially after the parallel step. Print only.
        if (g_rotWatchLo >= 0 && t >= g_rotWatchLo && t <= g_rotWatchHi) {
            struct RW { int uid, f0, nf; size_t n; };
            std::vector<RW> rws;
            size_t nk = 0;
            for (const Child& k : kids) {
                if (!k.valid) continue;
                ++nk;
                if (k.rotNf < 0) continue;
                bool found = false;
                for (RW& r : rws)
                    if (r.uid == k.rotUid && r.f0 == k.rotF0 && r.nf == k.rotNf) { ++r.n; found = true; break; }
                if (!found) rws.push_back(RW{k.rotUid, (int)k.rotF0, (int)k.rotNf, 1});
            }
            for (const RW& r : rws)
                std::printf("rotwatch: t=%lld gdx=%.4f uid=%d f0=%d nf=%d kids=%zu/%zu\n",
                            (long long)t, (double)gdx, r.uid, r.f0, r.nf, r.n, nk);
        }

        // ---- reference watch, part 2: what happened to its child ------------
        // Every child has been stepped and keyed, and nothing has been merged
        // or capped yet, so this is where the reference's own continuation can
        // still be told apart from the ones that replaced it. Read-only, in the
        // same position and for the same reason as the clearance probe below.
        //
        // The parent may be stepped in more than one group when a level has
        // several live frames (gidx filters on dx and trig only), so an ALIVE
        // fate wins over a dead one rather than the last one written.
        if (g_refWatch && g_refParent >= 0) {
            for (size_t i = 0; i < kids.size(); ++i) {
                if ((int)gidx[i >> 1] != g_refParent) continue;
                const int a = (int)(i & 1);
                if (g_refKidFate[a] == 2) continue;   // already alive somewhere
                g_refKidFate[a] = (int)kidFlag[i];
                g_refKidWhy[a] = kids[i].why ? kids[i].why : "";
                g_refKidKey[a] = kidKeys[i];
                g_refKidState[a] = kids[i].s;
            }
        }

        // ---- clearance probe (instrumentation, --clearprobe) ----------------
        // Every child has been stepped and keyed by now, and this only READS
        // them -- no slot, no ordinal and no key is touched -- so the emitted
        // plan is the same with the flag on or off. That equality is the
        // acceptance test (py/quick_regress.py, bit-identical).
        // Serial on purpose: the stepping above is what needed the threads, and
        // a probe that took a lock per child would be measuring the lock.
        if (g_clearProbe) {
            clearKids.clear();
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kidFlag[i] != 2) continue;   // 1 = died, 0 = never expanded
                const State& c = kids[i].s;
                // A turned frame has "above" and "below" on a different axis;
                // measuring it here would mix two coordinate systems in one
                // histogram. Counted instead, so the omission is visible.
                if (c.frame != 0) { ++g_clear.framesSkipped; continue; }
                const double px = (double)c.xAbs, py = (double)c.y;
                ClearSample cs;
                cs.key = kidKeys[i];
                cs.vy = c.vy;
                cs.solid = solidClearance(near, px, py,
                                          playerHalf(c.mode, c.mini != 0),
                                          &g_clear.slopesSkipped);
                cs.haz = hazardClearance(near, px, py,
                                         hazardHalfFor(c.mode, c.mini),
                                         c.mode, c.mini);
                cs.mode = c.mode;
                cs.goal = (uint8_t)(c.frame == 0 && px >= goalX ? 1 : 0);
                clearKids.push_back(cs);
            }
            g_clear.addLayer(clearKids);
        }

        // ---- phase 2p: dedupe in parallel shards (big layers) ---------------
        // Same result as the serial loop below, built in three steps:
        //   1. each shard walks the ordinals ascending and, for ITS keys only,
        //      replays emit()'s hi/lo hysteresis (a per-key sequential scan --
        //      exactness needs order only WITHIN a key, and a key lives in one
        //      shard). Slot-allocation events (first sight of a key, first
        //      hi/lo split) are marked per ordinal -- race-free, one owner.
        //   2. a serial scan over the ordinals materialises the slots in the
        //      same order the serial emit() would have pushed them.
        //   3. the goal is the smallest ACCEPTED ordinal whose x reached
        //      goalX, which is the same state the serial loop would have
        //      grabbed first.
        const size_t qfBefore = nxt.size();   // --qfoldwatch: this group's survivors start here
        const bool parDedupe = pool && !dbgHere && g_threads > 1
                               && kids.size() >= 4096;
        if (parDedupe) {
            const size_t nsh = (size_t)g_threads;
            if (shards.size() < nsh) shards.resize(nsh);
            if (evKind.size() < kids.size()) {
                evKind.resize(kids.size(), 0);
                evRec.resize(kids.size(), 0);
            }
            pool->parallelTasks(nsh, [&](size_t si) {
                ShardMap& sm = shards[si];
                sm.clear();
                sm.ensure(kids.size() / nsh + 64);
                for (uint32_t i = 0; i < (uint32_t)kids.size(); ++i) {
                    if (kidFlag[i] != 2) continue;
                    const SearchKey& k = kidKeys[i];
                    if ((SearchKeyHash{}(k) >> 32) % nsh != si) continue;
                    const float vy = kids[i].s.vy;
                    uint32_t& ri = sm.at(k, kNone);
                    bool accepted = false;
                    if (ri == kNone) {
                        ri = (uint32_t)sm.recs.size();
                        sm.recs.push_back(KeyRec{i, kNone, i, i, vy, vy, 0});
                        evKind[i] = (uint8_t)(1 + si);
                        evRec[i] = ri;
                        accepted = true;
                    } else {
                        KeyRec& r = sm.recs[ri];
                        // the comparisons mirror emit() exactly, including the
                        // float->double promotion of `vy + 1e-9`
                        if ((double)vy > (double)r.hiVy + 1e-9) {
                            if (r.bOrd == kNone) {
                                r.bOrd = i;
                                r.bWasHi = 1;
                                evKind[i] = (uint8_t)(1 + si);
                                evRec[i] = ri;
                            }
                            r.hiIdx = i;
                            r.hiVy = vy;
                            accepted = true;
                        } else if ((double)vy < (double)r.loVy - 1e-9) {
                            if (r.bOrd == kNone) {
                                r.bOrd = i;
                                r.bWasHi = 0;
                                evKind[i] = (uint8_t)(1 + si);
                                evRec[i] = ri;
                            }
                            r.loIdx = i;
                            r.loVy = vy;
                            accepted = true;
                        }
                    }
                    // Same change as the serial path: keep the roomiest goal
                    // state this shard saw rather than its first. `goalTight`
                    // starts at 0xffff, so with the score off the first one wins
                    // and nothing after it can tie-break past it -- the old
                    // "smallest accepted ordinal" rule, unchanged.
                    if (accepted && !solved && (double)kids[i].s.xAbs >= goalX
                        && (sm.goalOrd == kNone
                            || roomierRoute(kids[i].s.tight, sm.goalTight))) {
                        sm.goalOrd = i;
                        sm.goalTight = kids[i].s.tight;
                    }
                }
            });
            // counters (compact array, trivial serial pass)
            for (size_t i = 0; i < kids.size(); ++i) {
                if (!kidFlag[i]) continue;
                ++nBorn;
                if (kidFlag[i] == 1) ++nDied;
            }
            // materialise nxt in the serial loop's slot order
            for (uint32_t i = 0; i < (uint32_t)kids.size(); ++i) {
                if (!evKind[i]) continue;
                ShardMap& sm = shards[(size_t)evKind[i] - 1];
                const KeyRec& r = sm.recs[evRec[i]];
                evKind[i] = 0;   // leave the array clean for the next group
                uint32_t src;
                if (r.bOrd == kNone) src = r.hiIdx;   // never split: hi == lo
                else if (i == r.aOrd) src = r.bWasHi ? r.loIdx : r.hiIdx;
                else src = r.bWasHi ? r.hiIdx : r.loIdx;
                const State& from = cur[gidx[src >> 1]];
                const uint8_t action = (uint8_t)(src & 1);
                arena.push_back(Node{from.parent | ((uint32_t)action << 31)});
                if (checkWanted) arenaMode.push_back(kids[src].s.mode);
                nxt.push_back(kids[src].s);
                nxt.back().parent = (uint32_t)(arena.size() - 1);
                nxt.back().action = action;
            }
            if (!solved) {
                // Across shards, the same rule as within one: roomiest first,
                // the smaller ordinal breaking ties. With the score off every
                // tight is 0, no candidate is ever "roomier", and this reduces
                // to min(goalOrd) -- the rule it replaced, bit for bit.
                uint32_t g = kNone;
                uint16_t gt = 0xffff;
                for (size_t si = 0; si < nsh; ++si) {
                    const uint32_t o = shards[si].goalOrd;
                    if (o == kNone) continue;
                    const uint16_t ot = shards[si].goalTight;
                    if (g == kNone || ot < gt || (ot == gt && o < g)) {
                        g = o;
                        gt = ot;
                    }
                }
                if (g != kNone) {
                    const State& from = cur[gidx[g >> 1]];
                    const uint8_t action = (uint8_t)(g & 1);
                    arena.push_back(
                        Node{from.parent | ((uint32_t)action << 31)});
                    if (checkWanted) arenaMode.push_back(kids[g].s.mode);
                    goalState = kids[g].s;
                    goalState.parent = (uint32_t)(arena.size() - 1);
                    goalState.action = action;
                    solved = true;
                }
            }
            if (qfHere) qfoldAccount(qfBefore);
            continue;   // next speed group -- skips the serial phase 2
        }
        // ---- phase 2: dedupe and record (serial, in order) ------------------
        for (size_t i = 0; i < kids.size(); ++i) {
            const Child& kid = kids[i];
            if (!kid.valid) continue;
            const State& s = cur[gidx[i >> 1]];
            const int input = (int)(i & 1);
            const State& c = kid.s;
            const bool dead = kid.dead != 0;
            ++nBorn;
            if (dead) ++nDied;
            {
                if (dbgHere) {
                    // name the killer. "dead=1" alone never says whether the
                    // model is over-killing or under-killing (see g_deadWhy).
                    char why[128] = "";
                    if (dead) {
                        if (kid.obj)
                            std::snprintf(why, sizeof why,
                                          " %s id=%d type=%d (%.0f,%.0f) %gx%g",
                                          kid.why, kid.obj->id,
                                          (int)kid.obj->type, kid.obj->cx,
                                          kid.obj->cy, kid.obj->hw * 2,
                                          kid.obj->hh * 2);
                        else
                            std::snprintf(why, sizeof why, " %s", kid.why);
                    }
                    // The second body, when there is one. Without it a dual
                    // layer prints two parents with the SAME (y, vy) and no way
                    // to tell what separated them -- which is how lv20's
                    // t=17,115 layer reads: four lines, two distinct children,
                    // nothing on the line that explains the split.
                    // `held` is on every line for the same reason: GD runs
                    // buttons AFTER the update, so a wave's direction on this
                    // tick follows the PREVIOUS tick's button, and two parents
                    // differing only in `held` step apart under the same input.
                    char two[160] = "";
                    if (s.dual)
                        std::snprintf(two, sizeof two,
                                      " | p2 (%.3f,%.3f)->(%.3f,%.3f) flip2=%d "
                                      "g2=%d free=%d",
                                      (double)s.y2, (double)s.vy2,
                                      (double)c.y2, (double)c.vy2,
                                      (int)c.flip2, (int)c.grounded2,
                                      (int)c.freeHalf);
                    std::printf("dbg t=%lld in=%d: (%.3f,%.3f)->(%.3f,%.3f) "
                                "x=%.2f mode=%d flip=%d g=%d held=%d dead=%d%s%s\n",
                                t, input, (double)s.y, (double)s.vy, (double)c.y,
                                (double)c.vy, (double)c.xAbs, (int)c.mode,
                                (int)c.flip, (int)c.grounded, (int)s.held,
                                (int)dead, why, two);
                }
                if (!dead) emit(c, s, (uint8_t)input, kid.key);
            }
        }
        if (qfHere) qfoldAccount(qfBefore);
        }   // speed group
        // ---- SAFE BANDS ------------------------------------------------------
        // How wide the frontier is at this tick, written out for the driver.
        //
        // The driver's anchor policy is "just before the death, then back off
        // geometrically", and that is what breaks on a deep wall: it can only
        // crawl backwards from the wall, so when a stretch of the route becomes
        // invalid it never gets far enough back to redo it. Measured on lv16
        // (60 iterations, oriented rule): the loop reached x=19,174 once and
        // then fell to 13,310 and stayed there; on lv19 it bounced 29,024 <->
        // 27,319 three times. Both are the same shape.
        // A tick where the frontier is WIDE is a tick where many different
        // states survive -- i.e. a place where the level does not care much how
        // you arrived. Those are the places to cut a level into segments, and
        // the DP already knows them; it just never told anyone.
        // Cheap: two words per layer, flushed once at the end.
        if (!g_bandPath.empty()) {
            float ylo = 1e9f, yhi = -1e9f;
            double xr = 0;
            // semantic classes: mode/mini/dual/speed. Small in practice (a
            // handful per layer), so a flat vector beats a map.
            struct Cls { uint32_t k; int n; float lo, hi; };
            std::vector<Cls> cs;
            for (const State& s : nxt) {
                ylo = std::min(ylo, s.y);
                yhi = std::max(yhi, s.y);
                xr = std::max(xr, (double)s.xAbs);
                const uint32_t k = ((uint32_t)s.mode << 24)
                                   ^ ((uint32_t)s.mini << 20)
                                   ^ ((uint32_t)s.dual << 16)
                                   ^ (uint32_t)std::lround(s.dx * 1000.0);
                size_t j = 0;
                for (; j < cs.size(); ++j) if (cs[j].k == k) break;
                if (j == cs.size()) cs.push_back({k, 0, 1e9f, -1e9f});
                cs[j].n++;
                cs[j].lo = std::min(cs[j].lo, s.y);
                cs[j].hi = std::max(cs[j].hi, s.y);
            }
            if (!nxt.empty()) {
                BandRow b;
                b.t = t; b.alive = (int)nxt.size();
                b.ylo = ylo; b.yhi = yhi; b.x = (float)xr;
                // merged this layer = children born - children that died -
                // survivors (what the dedupe folded away)
                b.merged = (int)std::max<long long>(
                    0, nBorn - nDied - (long long)nxt.size());
                char buf[64];
                for (const Cls& c : cs) {
                    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%d:%.0f:%.0f",
                                  c.k >> 24, (c.k >> 20) & 0xf,
                                  (c.k >> 16) & 0xf, c.k & 0xffff, c.n,
                                  (double)c.lo, (double)c.hi);
                    if (!b.cls.empty()) b.cls += ';';
                    b.cls += buf;
                }
                g_bands.push_back(std::move(b));
            }
        }
        // Say it the FIRST time a touch trigger fires anywhere in the frontier.
        // Without this the only symptom of "the search never found the box" is
        // that the wall does not move, which is exactly the symptom of a dozen
        // other things (see the three sessions spent on x=28,075).
        if (!g_touch.empty()) {
            TouchMask m{};
            for (const State& s : nxt) m |= s.trig;
            if (m & ~g_trigReported) {
                for (size_t b = 0; b < g_touch.size(); ++b)
                    if ((m & ~g_trigReported) & touchBit(b))
                        std::printf("triggers: box (%.0f,%.0f) entered at "
                                    "t=%lld\n", g_touch[b].cx, g_touch[b].cy, t);
                g_trigReported |= m;
            }
        }
        ppMark(2);
        // ---- stride cap, PER CLASS ------------------------------------------
        // A "class" is what makes two states play a different game: mode, size,
        // gravity, dual, speed. The cap used to be one stride over the whole
        // layer, which quietly starves any class that expands less per tick --
        // and one does: an AIRBORNE CUBE gets one child (its input is moot)
        // while a ship gets two. So a cube branch's share halves every layer
        // once the cap binds, no matter how healthy it is.
        // Measured on lv18 from t=14,200: the branch that takes the RegularSize
        // portal at x=20,386 (the one route that survives -- it reaches
        // x=27,356 while the mini one dies at 24,716) is 416 states at
        // t=15,000 and ZERO at t=15,500 with cap 2,000, while the same run at
        // cap 40,000 still has 2,945 of them. It was not being out-competed on
        // merit; it was being out-multiplied.
        // Max-min fair instead: a small class keeps everything it has, and the
        // big ones split what is left. Kept states stay in their original order
        // so the layer is otherwise unchanged.
        // ---- reference watch, part 3: is it still there, and if not, why ----
        // Checked twice, on purpose: once here, with the layer complete and the
        // cap not yet applied, and once after the cap. Only the pair can tell
        // "the cap evicted it" from "it never got into the layer", and those
        // two have nothing to do with each other.
        bool refPreCap = false;
        State refCarried{};     // the tracked state, captured BEFORE the cap
        if (g_refWatch && g_refLostAt < 0) {
            const char* gate = nullptr;
            char detail[256];
            detail[0] = '\0';
            const auto it = g_refRows.find(t);
            // Which child IS the reference's next state: the one that matches
            // the trace. Both are examined, so a dead one can still be named.
            int want = -1;
            if (it != g_refRows.end() && it->second.act >= 0
                && it->second.act < 2 && g_refKidFate[it->second.act] >= 1) {
                // The reference recorded which input stepped this tick. Read it
                // rather than infer it: on a swing's press tick the two children
                // are identical in everything the trace shows.
                want = it->second.act;
            } else {
                for (int a = 0; a < 2 && it != g_refRows.end(); ++a)
                    if (g_refKidFate[a] >= 1 && refMatches(g_refKidState[a],
                                                           it->second))
                        want = a;
            }
            if (g_refParent < 0) {
                gate = "parent-gone";
            } else if (it == g_refRows.end()) {
                gate = "trace-ended";
            } else if (want < 0) {
                // Neither child is the reference's next state. The search's
                // carrier and the replay have parted company -- a difference in
                // stepping or in context, not a pruning decision, and it must
                // not be reported as one.
                gate = "cannot-reproduce";
                snprintf(detail, sizeof(detail),
                         " ref y=%.4f vy=%.4f | kid0 y=%.4f vy=%.4f f=%d"
                         " | kid1 y=%.4f vy=%.4f f=%d",
                         it->second.y, it->second.vy,
                         (double)g_refKidState[0].y, (double)g_refKidState[0].vy,
                         g_refKidFate[0],
                         (double)g_refKidState[1].y, (double)g_refKidState[1].vy,
                         g_refKidFate[1]);
            } else if (g_refKidFate[want] == 2
                       && !refMatches(g_refKidState[want], it->second)) {
                // The reference says this input stepped the tick, and the search
                // stepping the same state with the same input produced something
                // else. That is a transition difference, not a pruning decision.
                gate = "cannot-reproduce";
                snprintf(detail, sizeof(detail),
                         " act=%d ref y=%.4f vy=%.4f flip=%d fr=%d | kid y=%.4f "
                         "vy=%.4f flip=%d fr=%d", want, it->second.y,
                         it->second.vy, it->second.flip, it->second.frame,
                         (double)g_refKidState[want].y,
                         (double)g_refKidState[want].vy,
                         (int)g_refKidState[want].flip,
                         (int)g_refKidState[want].frame);
            } else if (g_refKidFate[want] == 1) {
                gate = "kill";
                snprintf(detail, sizeof(detail), " rule=%s action=%d",
                         *g_refKidWhy[want] ? g_refKidWhy[want] : "(unnamed)",
                         want);
            } else {
                const int idx = refFindExact(nxt, g_refKidState[want]);
                if (idx >= 0) {
                    g_refParent = idx;      // carried, untouched
                    refPreCap = true;
                } else {
                    // Merged away. The watch stops here rather than adopting the
                    // survivor and carrying on: past a merge the carrier is no
                    // longer on the reference's trajectory, so the next layer's
                    // children cannot be matched against the trace and every
                    // line after it would be about a different path.
                    //
                    // What IS recorded is how far the kept state is, because
                    // that is the whole question about a merge: 0.19 px/tick of
                    // vy is a state the cell may fairly stand in for, and a
                    // larger gap is a passage being represented by something
                    // that cannot make it.
                    gate = "dedupe";
                    const int alt = refNearestInCell(nxt, g_refKidKey[want],
                                                     g_refKidState[want],
                                                     [t](const State& st) {
                                                         return keyOf(st, (long long)t);
                                                     });
                    int n = 0;
                    for (const State& s : nxt)
                        if (keyOf(s, (long long)t) == g_refKidKey[want]) ++n;
                    // A merge whose survivor differs from the reference only in WHEN
                    // boxes fired (fireB / trigT) -- a box whose chain moves nothing,
                    // entered a tick apart -- is followed on the survivor instead of
                    // ending the watch. Printed, so the reader knows the carrier is no
                    // longer the reference to the bit (SubZero 4003's toggle block:
                    // the held and the released entry merge, and the watch stopped
                    // 400 ticks before the question it was asked).
                    bool adopted = false;
                    // Every survivor in the cell, not just the nearest: the nearest in (y, vy)
                    // can be one from another group (a different trigger mask), which is never
                    // the reference's merge partner.
                    for (size_t si = 0; si < nxt.size() && !adopted; ++si) {
                        if (keyOf(nxt[si], (long long)t) != g_refKidKey[want]) continue;
                        State a = nxt[si], b = g_refKidState[want];
                        // Which of the overlooked fields actually differ, by name.
                        std::string diff;
                        if (a.action != b.action) diff += " action";
                        if (std::memcmp(a.fireB, b.fireB, sizeof(a.fireB)) != 0) diff += " fireB";
                        if (a.trigT != b.trigT) diff += " trigT";
                        if (a.tight != b.tight) diff += " tight";
                        if (a.rot != b.rot || a.rotNeg != b.rotNeg || a.rotStep != b.rotStep
                            || a.rotBoost != b.rotBoost)
                            diff += " rot";
                        if (a.groundUid != b.groundUid) diff += " groundUid";
                        a.parent = b.parent = 0;
                        a.action = b.action = 0;
                        std::memcpy(a.fireB, b.fireB, sizeof(a.fireB));
                        a.trigT = b.trigT;
                        a.tight = b.tight;
                        // ...and the sprite angle (rot / rotNeg / rotStep), which the key
                        // leaves out too; named when it is the difference.
                        const bool rotDiff = a.rot != b.rot || a.rotNeg != b.rotNeg
                                             || a.rotStep != b.rotStep || a.rotBoost != b.rotBoost;
                        a.rot = b.rot; a.rotNeg = b.rotNeg; a.rotStep = b.rotStep;
                        a.rotBoost = b.rotBoost;
                        // ...and --stickseam's tie (groundUid), which the key leaves out.
                        const bool tieDiff = a.groundUid != b.groundUid;
                        a.groundUid = b.groundUid;
                        (void)rotDiff; (void)tieDiff;
                        if (std::memcmp(&a, &b, sizeof(State)) == 0 && !g_refAdopt) {
                            std::printf("refwatch: merge t=%lld the survivor differs only in:%s "
                                        "(--refadopt would carry on with it)\n", t, diff.c_str());
                        } else if (std::memcmp(&a, &b, sizeof(State)) == 0) {
                            std::printf("refwatch: ADOPT t=%lld a SUBSTITUTE from here, differing in:%s\n",
                                        t, diff.c_str());
                            if (g_refAdoptT < 0) g_refAdoptT = t;
                            for (const char* fld : {"action", "fireB", "trigT", "tight", "rot", "groundUid"})
                                if (diff.find(std::string(" ") + fld) != std::string::npos
                                    && g_refAdoptFields.find(fld) == std::string::npos)
                                    g_refAdoptFields += std::string(" ") + fld;
                            g_refParent = (int)si;
                            refPreCap = true;
                            gate = nullptr;
                            adopted = true;
                        } else {
                            const unsigned char* pa = reinterpret_cast<const unsigned char*>(&a);
                            const unsigned char* pb = reinterpret_cast<const unsigned char*>(&b);
                            std::printf("refwatch: cell-mate %zu trig 0x%llx/0x%llx differs at:", si,
                                        (unsigned long long)nxt[si].trig.word(0),
                                        (unsigned long long)g_refKidState[want].trig.word(0));
                            int nd = 0;
                            for (size_t o = 0; o < sizeof(State) && nd < 16; ++o)
                                if (pa[o] != pb[o]) { std::printf(" %zu", o); ++nd; }
                            std::printf("\n");
                        }
                    }
                    if (adopted) {
                    } else if (alt < 0) {
                        snprintf(detail, sizeof(detail), " cell-gone");
                    } else {
                        g_refDriftY = std::fabs((double)nxt[(size_t)alt].y
                                                - (double)g_refKidState[want].y);
                        g_refDriftVy = std::fabs((double)nxt[(size_t)alt].vy
                                                 - (double)g_refKidState[want].vy);
                        snprintf(detail, sizeof(detail),
                                 " cell=%d action=%d ref y=%.4f vy=%.4f "
                                 "kept y=%.4f vy=%.4f", n, want,
                                 (double)g_refKidState[want].y,
                                 (double)g_refKidState[want].vy,
                                 (double)nxt[(size_t)alt].y,
                                 (double)nxt[(size_t)alt].vy);
                    }
                }
            }
            if (refPreCap) refCarried = nxt[(size_t)g_refParent];
            if (gate) {
                std::printf("refwatch: LOST t=%lld gate=%s%s "
                            "dy=%.3f dvy=%.3f alive=%zu\n",
                            t, gate, detail, g_refDriftY,
                            g_refDriftVy, nxt.size());
                g_refLostAt = t;
            }
        }
        maxAlive = std::max(maxAlive, nxt.size());
        const bool censusHere = g_searchCensus > 0 && ((t - t0) % g_searchCensus) == 0;
        if (g_searchCensus > 0) census.layer(nxt, (long long)t, curDxF, nBorn, nDied, censusHere);
        // --capmap: this layer's cap, from the leader x of the layer before it.
        const size_t capNow = capAtX(bestX);
        g_outcome.workStates += (long long)std::min(nxt.size(), capNow);
        const size_t qfPre = nxt.size();   // --qfoldwatch: the frontier before the alive cap
        // --heldcellcap: the layer's cells, keyed without the button (held, jumpBuf), each named by
        // its first state in layer order -- the population the water-fill samples -- and the cell
        // of every state. Built only for a layer over the cap; a layer whose CELLS fit is kept whole.
        std::vector<uint32_t> cellPool;
        std::vector<uint32_t> cellOf;
        bool overCap = nxt.size() > capNow;
        if (overCap && g_heldCellCap && g_heldAll) {
            // The keys on the pool's threads (the layer is thousands of states on every capped
            // layer, and keyOf is the costly part), then (key, index) sorted: a cell's first
            // state is its lowest index.
            const size_t n = nxt.size();
            std::vector<std::pair<SearchKey, uint32_t>> ck(n);
            auto keyAt = [&](size_t i) {
                State s = nxt[i];
                s.held = 0;
                s.jumpBuf = 0;
                ck[i] = {keyOf(s, (long long)t), (uint32_t)i};
            };
            if (pool && n >= 512) pool->parallelFor(n, keyAt);
            else for (size_t i = 0; i < n; ++i) keyAt(i);
            std::sort(ck.begin(), ck.end());
            cellOf.resize(n);
            for (size_t j = 0; j < n; ++j) {
                const uint32_t rep = (j > 0 && ck[j].first == ck[j - 1].first)
                                         ? cellOf[ck[j - 1].second] : ck[j].second;
                cellOf[ck[j].second] = rep;
                if (rep == ck[j].second) cellPool.push_back(rep);
            }
            std::sort(cellPool.begin(), cellPool.end());
            overCap = cellPool.size() > capNow;
            if (overCap) ++cellCuts;
            else ++cellSpared;
        }
        const bool cellCap = !cellOf.empty();
        if (overCap) {
            ++capHits;
            if (!cellCap) capDropped += (long long)(nxt.size() - capNow);
            if (!g_bandPath.empty() && !g_bands.empty() && g_bands.back().t == t)
                g_bands.back().capdrop = (int)((cellCap ? cellPool.size() : nxt.size()) - capNow);
            // --coins only. Measured 2026-08-31: with the band class on for
            // every run, 20 of the 21 levels the cold suite reached kept their
            // baseline iteration count exactly -- and lv20 went 37 -> 50. That
            // is the level where the band really does branch, and +13 cold
            // iterations is not a price the PLAIN solver has any reason to pay:
            // it needs the deepest route, which is the majority the water-fill
            // already keeps. Preserving a minority branch is a coin-routing
            // problem, so the class is a coin-routing class.
            const bool bandClass = coinOn;
            // --coinwin (thread_pool.hpp): the (y, vy) cell of a state inside the approach window
            // of a coin it still lacks, and 0 everywhere else -- so with the flag off, or outside
            // every window, the partition below is the one it was.
            auto coinWinCell = [&](const State& s) -> uint64_t {
                if (!coinOn || g_coinWinY <= 0.0 || s.frame != 0) return 0;
                for (size_t ci = 0; ci < L.coins.size() && ci < 8; ++ci) {
                    if (!coinPruneOk[ci] || ((s.coins >> ci) & 1u)) continue;
                    const double cx = L.coins[ci].cx;
                    if ((double)s.xAbs < cx - g_coinWinLen || (double)s.xAbs > cx + L.coins[ci].hw)
                        continue;
                    const int64_t yb = (int64_t)std::floor((double)s.y / g_coinWinY);
                    const int64_t vb = (int64_t)std::floor((double)s.vy / g_coinWinVy);
                    return (((uint64_t)(ci + 1) << 40) ^ ((uint64_t)(yb & 0xfffff) << 16)
                            ^ (uint64_t)(vb & 0xffff)) * 0xBF58476D1CE4E5B9ull;
                }
                return 0;
            };
            auto classOf = [bandClass, &coinWinCell](const State& s) {
                return coinWinCell(s) ^ ((uint64_t)s.mode << 32) ^ ((uint64_t)s.mini << 24)
                       ^ ((uint64_t)s.flip << 16) ^ ((uint64_t)s.dual << 8)
                       // --coins: the collected set is a class of its own, so
                       // the water-fill below -- which serves the SMALLEST
                       // class first -- protects the few lineages that took a
                       // coin instead of thinning them at the same stride as
                       // the many that are merely inside its window. 0 without
                       // the flag, so every existing partition is unchanged.
                       ^ ((uint64_t)s.coins << 40)
                       // ...and the FLIGHT BAND. Every ship state is otherwise
                       // one class, so the water-fill samples the whole layer at
                       // one stride -- and a route that has to thread many
                       // saturated layers as a minority survives that with
                       // probability (kept/born)^n. lv1's coin corridor is
                       // exactly such a route, and it is what this is for: at
                       // cap 16,000 the frontier at t=19,000 is y=[113,367]
                       // while the reference route is at 457, so the search
                       // reports the coin unreachable even though the model
                       // replays the human's own route through it to 0.00 px.
                       // At cap 200,000 the frontier is y=[435,495] and it
                       // solves -- so it was never physics, only thinning.
                       // The band is the right axis rather than a y grid (tried
                       // first, floor(y/60)): it is what a BRANCH actually is
                       // here. lv1's corridor is entered through a portal at
                       // cy=405 that a low route never touches, and firing it is
                       // what rewrites bandCeil; keyOf already treats two bands
                       // as two worlds. The y grid recovered lv1 too but cost
                       // capHits on levels that have no branch at all (lv19
                       // +30%, lv14 +13%, lv18 +10%), while the band left
                       // lv14/17/18/19 identical to the digit.
                       // (Only where the band clamps -- search_key.hpp bandClamps.)
                       ^ ((bandClass && bandClamps(s)) ? ((uint64_t)(uint32_t)(int32_t)
                              std::lround((double)s.bandCeil) << 48) : 0)
                       // ...and which ACTIVATORS have fired (g_actMask, empty
                       // without --activators). SubZero 4003: the one lineage that
                       // pressed the toggle block, and so has the ring that leads
                       // to the second coin, was evicted by this cap at t=8,288
                       // (refwatch on a human route the model replays to 0 px).
                       ^ (bandClass ? (s.trig & g_actMask).word(0) * 0x94D049BB133111EBull
                                    : 0)
                       ^ (uint64_t)(uint32_t)std::lround(s.dx * 1000.0);
            };
            std::vector<uint64_t> cls;
            std::vector<std::vector<uint32_t>> byCls;
            const size_t popN = cellCap ? cellPool.size() : nxt.size();
            for (size_t p = 0; p < popN; ++p) {
                const uint32_t i = cellCap ? cellPool[p] : (uint32_t)p;
                const uint64_t k = classOf(nxt[i]);
                size_t j = 0;
                for (; j < cls.size(); ++j) if (cls[j] == k) break;
                if (j == cls.size()) { cls.push_back(k); byCls.push_back({}); }
                byCls[j].push_back(i);
            }
            // water-fill: smallest class first, each takes at most an equal
            // share of what is still free
            std::vector<size_t> ord(byCls.size());
            for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
                return byCls[a].size() < byCls[b].size();
            });
            std::vector<uint32_t> keepIdx;
            keepIdx.reserve(capNow);
            size_t left = capNow;
            for (size_t n = 0; n < ord.size(); ++n) {
                const std::vector<uint32_t>& v = byCls[ord[n]];
                const size_t fair = left / (ord.size() - n);
                const size_t take = std::min(v.size(), fair);
                if (take == v.size()) {
                    keepIdx.insert(keepIdx.end(), v.begin(), v.end());
                } else if (g_capEnvelope && take >= 3) {
                    // --capenvelope: the class's two extremes, then the stride over the rest.
                    auto lower = [&](uint32_t a, uint32_t b) {
                        if (nxt[a].y != nxt[b].y) return nxt[a].y < nxt[b].y;
                        if (nxt[a].vy != nxt[b].vy) return nxt[a].vy < nxt[b].vy;
                        return a < b;
                    };
                    size_t lo = 0, hi = 0;
                    for (size_t k = 1; k < v.size(); ++k) {
                        if (lower(v[k], v[lo])) lo = k;
                        if (lower(v[hi], v[k])) hi = k;
                    }
                    keepIdx.push_back(v[lo]);
                    keepIdx.push_back(v[hi]);
                    std::vector<uint32_t> rest;
                    rest.reserve(v.size() - 2);
                    for (size_t k = 0; k < v.size(); ++k)
                        if (k != lo && k != hi) rest.push_back(v[k]);
                    const double stride = (double)rest.size() / (double)(take - 2);
                    for (double i = 0; (size_t)i < rest.size(); i += stride)
                        keepIdx.push_back(rest[(size_t)i]);
                } else if (take > 0) {
                    const double stride = (double)v.size() / (double)take;
                    for (double i = 0; (size_t)i < v.size() && take > 0; i += stride)
                        keepIdx.push_back(v[(size_t)i]);
                }
                left -= std::min(left, take);
            }
            // --heldcellcap: every state of a sampled cell comes back with it, in layer order.
            auto withTwins = [&](std::vector<uint32_t>& idx) {
                if (!cellCap) return;
                std::vector<uint8_t> pick(nxt.size(), 0);
                for (uint32_t i : idx) pick[i] = 1;
                idx.clear();
                if (g_heldCellTwins <= 0) {
                    for (uint32_t i = 0; i < (uint32_t)nxt.size(); ++i)
                        if (pick[cellOf[i]]) idx.push_back(i);
                    return;
                }
                // --heldcelltwins N: the cell's first state, then the states whose held differs
                // from it, then the rest, until the cell has N.
                // TRIED AND REMOVED (2026-09-26): a bound on the layer's total as well
                // (--heldcelltotal P, at most (100 + P)% of the cap, the twins taken at an even
                // stride). With N = 3 it did not bring lv16's seconds under 2x (P=50 2.63x, P=25
                // 2.48x, against 2.6x without it) and it broke the ladder on lv14 (kids 4.2x at
                // P=50, 39x at P=25) and lv11 (13x, not solved, against 0.55x and SOLVED at N = 3
                // alone): official 22 from t=0, the loop's base argv.
                std::vector<uint8_t> kept(nxt.size(), 0);
                std::vector<int> nOf(nxt.size(), 0);
                for (int pass = 0; pass < 3; ++pass)
                    for (uint32_t i = 0; i < (uint32_t)nxt.size(); ++i) {
                        const uint32_t r = cellOf[i];
                        if (!pick[r] || kept[i] || nOf[r] >= g_heldCellTwins) continue;
                        if (pass == 0 && i != r) continue;
                        if (pass == 1 && nxt[i].held == nxt[r].held) continue;
                        kept[i] = 1;
                        ++nOf[r];
                        idx.push_back(i);
                    }
                std::sort(idx.begin(), idx.end());
            };
            const size_t cellsKept = keepIdx.size();
            withTwins(keepIdx);
            if (cellCap) {
                capDropped += (long long)(nxt.size() - keepIdx.size());
                cellTwinsKept += (long long)(keepIdx.size() - cellsKept);
            }
            std::sort(keepIdx.begin(), keepIdx.end());
            // --searchcensus: the same water-fill, with each class's stride taken over the
            // class sorted by (y, vy) instead of in layer order. Counted, never kept.
            if (g_searchCensus > 0) census.cap(nxt.size(), capNow, nDied);
            if (censusHere) {
                std::vector<uint32_t> altIdx;
                size_t leftA = capNow;
                for (size_t n = 0; n < ord.size(); ++n) {
                    std::vector<uint32_t> v = byCls[ord[n]];
                    std::sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) {
                        if (nxt[a].y != nxt[b].y) return nxt[a].y < nxt[b].y;
                        if (nxt[a].vy != nxt[b].vy) return nxt[a].vy < nxt[b].vy;
                        return a < b;
                    });
                    const size_t fair = leftA / (ord.size() - n);
                    const size_t take = std::min(v.size(), fair);
                    if (take == v.size()) {
                        altIdx.insert(altIdx.end(), v.begin(), v.end());
                    } else if (take > 0) {
                        const double stride = (double)v.size() / (double)take;
                        for (double i = 0; (size_t)i < v.size() && take > 0; i += stride)
                            altIdx.push_back(v[(size_t)i]);
                    }
                    leftA -= std::min(leftA, take);
                }
                withTwins(altIdx);
                census.coverage(nxt, keepIdx, altIdx, curDxF);
            }
            std::vector<State> kept;
            kept.reserve(keepIdx.size());
            for (uint32_t i : keepIdx) kept.push_back(nxt[i]);
            nxt.swap(kept);
            // ...and the second half of the pair: it was in the layer before
            // the cap and is not in it now.
            if (g_refWatch && g_refLostAt < 0 && refPreCap) {
                const int idx = refFindExact(nxt, refCarried);
                if (idx < 0) {
                    std::printf("refwatch: LOST t=%lld gate=cap alive=%zu "
                                "cap=%zu\n",
                                t, nxt.size(), capNow);
                    g_refLostAt = t;
                } else {
                    g_refParent = idx;    // the cap reordered the layer
                }
            }
        }
        // --heldcellcap keeps more states than the cap counts: charge what the layer really keeps.
        if (cellCap) g_outcome.workStates += (long long)nxt.size() - (long long)std::min(qfPre, capNow);
        if (g_searchCensus > 0) census.kept(nxt);
        // How many complete routes the final pick is choosing between, and how
        // far back they are actually different (--clearprobe only; reads the
        // arena, changes nothing).
        if (g_clearProbe && solved) measureGoalDiversity(nxt, arena, goalX);
        if (g_rjOn) {
            if ((long long)t == g_rjAfter) {
                const auto rr = g_rjRows.find((long long)t);
                const int oi = rr == g_rjRows.end() ? -1 : refFind(nxt, rr->second);
                g_rjOldNode = oi >= 0 ? nxt[(size_t)oi].parent : 0xffffffffu;
                std::printf("rejoin: the old path is %s the frontier at its death tick %lld\n",
                            oi >= 0 ? "in" : "NOT in", (long long)t);
            }
            const long long tt = (long long)t;
            const int ji = rejoinLayer(nxt, tt, [&](const State& s) {
                if (g_rjOldNode == 0xffffffffu) return false;
                uint32_t n = s.parent;
                for (long long k = tt; k > g_rjAfter; --k) n = arena[n].parent();
                return n == g_rjOldNode;
            }, [&](const State& s, const RefRow& r) {
                return !g_rjFull || (r.haveKey && keyOf(s, tt) == r.key);
            });
            // ...and only onto an old plan that goes on: a trace that ends a tick past the join
            // (the old plan was a PARTIAL that died there) would turn a dead end into a SOLVED.
            const bool rjGoesOn = !g_rjRows.empty() && g_rjRows.rbegin()->first - tt >= kRjMinTail;
            // ...and, with --coins, not from INSIDE an open counting-tap window
            // whose gate has not fired. The old plan's tail is flown as it is,
            // and a join is a SOLVED: joined there, the presses still owed are
            // never planned. Anywhere else a join stays what it was -- a tail
            // that then passes the window short is ended by the mod at the shut
            // point (Outcome::coinGates), and the loop plans the window again
            // from an anchor inside it. (Refusing every join short of all coins
            // was tried first: 0 joins against 34, and the run stalled at
            // x=3.6-4.9k, 1,000 px before any coin.)
            // ...nor after the gate has fired while a coin it opens is still to
            // be taken: the old tail was flown with the coin off, and on lv22 it
            // glides under it.
            const State* rjS = ji >= 0 ? &nxt[(size_t)ji] : nullptr;
            const bool rjWindow = rjS && (rjS->taps & 0x80) && !(rjS->taps & 0x40)
                                  && !(rjS->trig & tapGateBits);
            const bool rjOwed = rjS && gatedMask && (rjS->trig & tapGateBits)
                                && (rjS->coins & gatedMask) != gatedMask;
            const bool rjCoins = !coinOn || !tapGive.bit || !(rjWindow || rjOwed);
            if (g_rjUse && ji >= 0 && !solved && rjGoesOn && rjCoins) {
                solved = true;
                goalState = nxt[(size_t)ji];
                planCut = 0;
                g_rjJoinT = tt;
                g_outcome.rejoinT = tt;
                bestT = tt;
                std::printf("rejoin: JOINED the old plan at t=%lld (%lld ticks past its death), "
                            "stopping the search here\n", tt, tt - g_rjAfter);
                break;
            }
        }
        if (qfHere)
            std::printf("qfold: t=%lld frontier=%zu kept=%zu lostq=%lld keys=%lld\n",
                        (long long)t, qfPre, nxt.size(), qfLost, qfKeys);
        prevAlive = cur.size();
        cur.swap(nxt);
        nBornPrev = nBorn;
        nDiedPrev = nDied;
        // The layer no longer HAS one x (see the speed groups above), so the
        // number everything downstream reports is the frontier's own leader.
        double x = 0.0;
        for (const State& s : cur) x = std::max(x, (double)s.xAbs);
        if (!cur.empty()) { bestT = t; bestX = x; }
        if (cur.empty()) break;
        // horizon reached: take the surviving state with the most clearance
        // (highest |vy| is a bad proxy; just take the first survivor -- they
        // are all alive, and the driver re-anchors from GD anyway)
        // The horizon bounds the PLAN, not the SEARCH. It used to stop the
        // search dead at t0+horizon and emit `cur.front()`, an arbitrary
        // survivor -- and a survivor at the horizon is not the same thing as a
        // survivor that goes anywhere. On lv11 that prefix was a dead end: GD
        // replayed it, died 37 ticks after the plan ran out, and every anchor
        // the driver backed off to (six of them, down to t=1537) was doomed,
        // because they all sat on that one doomed branch. The full search
        // reached x=6,067 on a different branch the whole time.
        // So: keep searching another `horizon` ticks past the cut, and only
        // then take a survivor. The emitted plan is truncated back to the cut
        // below, so what the driver replays is a prefix that is KNOWN to still
        // be alive `horizon` ticks later. Cost is at most 2x the search.
        if (horizon > 0 && !solved && t - t0 >= horizon * 2) {
            solved = true;
            g_outcome.horizonCut = true;   // survived, did not reach the end (SearchOutcome)
            goalState = pickOf(cur);
            planCut = t0 + horizon;
            break;
        }
        ppMark(3);
        // ---- mark-compact GC ----
        // The arena is append-only and keeps every dead lineage's whole
        // ancestor chain; on lv16 cap=40000 it reached 147.9M nodes (564 MiB)
        // and the process died growing it. The frontier's chains coalesce a
        // few hundred ticks back, so the REACHABLE set is a tiny fraction
        // (measure it with --memstat). Compact whenever the arena doubles
        // past the threshold: amortized O(1) per node. Indices are internal,
        // so the emitted plan is bit-identical with GC on or off.
        if (g_gcNodes > 0 && !solved && arena.size() >= gcNext) {
            std::vector<bool> mark(arena.size(), false);
            mark[0] = true;
            auto markFrom = [&](const std::vector<State>& v) {
                for (const State& s : v)
                    for (uint32_t i = s.parent; i != 0 && !mark[i];
                         i = arena[i].parent())
                        mark[i] = true;
            };
            markFrom(cur);
            markFrom(nxt);   // the PARTIAL path reads nxt after the loop
            size_t keep = 0;
            for (size_t i = 0; i < arena.size(); ++i) keep += mark[i];
            std::vector<uint32_t> remap(arena.size(), 0);
            std::vector<Node> na;
            na.reserve(keep);
            // The mode side vector is indexed by the same numbers, so it is compacted in the
            // same pass or it would describe the nodes the arena used to hold. Nothing that has
            // ALREADY been published can be reached from here -- a published prefix is ticks
            // and levels, never indices (dp/progress.hpp) -- so this only has to keep the live
            // array consistent for the next LCA walk.
            std::vector<uint8_t> nm;
            if (checkWanted) nm.reserve(keep);
            for (size_t i = 0; i < arena.size(); ++i) {
                if (!mark[i]) continue;
                remap[i] = (uint32_t)na.size();
                // a parent always precedes its children in the arena, so
                // remap[parent()] is already final here
                na.push_back(Node{remap[arena[i].parent()]
                                  | ((uint32_t)arena[i].action() << 31)});
                if (checkWanted) nm.push_back(arenaMode[i]);
            }
            const size_t before = arena.size();
            arena.swap(na);
            na = std::vector<Node>();   // release the old allocation NOW
            if (checkWanted) {
                arenaMode.swap(nm);
                nm = std::vector<uint8_t>();
            }
            for (State& s : cur) s.parent = remap[s.parent];
            for (State& s : nxt) s.parent = remap[s.parent];
            if (g_rjOldNode != 0xffffffffu)
                g_rjOldNode = mark[g_rjOldNode] ? remap[g_rjOldNode] : 0xffffffffu;
            gcNext = std::max(g_gcNodes, arena.size() * 2);
            std::printf("gc t=%lld arena %zu -> %zu nodes (%.1f%% live)\n", t,
                        before, arena.size(),
                        100.0 * (double)arena.size() / (double)before);
            std::fflush(stdout);
        }
        ppMark(4);
        // ---- memory budget ----
        // Estimate the search's own structures and stop GROWING before the OS
        // stops us (the lv16 run died as exit 255 with zero diagnostics). The
        // emitted plan keeps the horizon guarantee: a prefix that is known to
        // still be alive (t-t0)/2 ticks past its end, i.e. the same contract
        // as the horizon cut, just with a smaller effective horizon.
        if (g_memLimitMiB > 0 && !solved && (t & 127) == 0 && t - t0 > 2) {
            // Include worker/shard key storage now that a cell holds complete fields.
            size_t shardBytes = 0;
            for (const auto& sh : shards)
                shardBytes += sh.storageBytes() + sh.recs.capacity() * sizeof(KeyRec);
            const size_t est = arena.capacity() * sizeof(Node) + arenaMode.capacity()
                               + (cur.capacity() + nxt.capacity()) * sizeof(State)
                               + seen.storageBytes() + shardBytes
                               + kids.capacity() * sizeof(Child)
                               + kidKeys.capacity() * sizeof(SearchKey);
            if (est > g_memLimitMiB * (size_t)1048576) {
                solved = true;
                goalState = pickOf(cur);
                planCut = t0 + (t - t0) / 2;
                std::printf("MEMORY_LIMIT: est=%zuMiB arena=%zu alive=%zu "
                            "t=%lld x=%.0f -> plan cut at t=%lld "
                            "(half the searched depth)\n",
                            est >> 20, arena.size(), cur.size(), t, x,
                            planCut);
                break;
            }
        }
        // ---- checkpoints (dp/progress.hpp) ----
        //
        // At a checkpoint layer, publish the lineage of the frontier's FIRST state, cut at this
        // layer, for the caller to fly. The first state because the frontier's order is a
        // function of the search alone, so the same call publishes the same lineage every time
        // -- which state is flown matters less than that the choice never depends on the clock.
        // Every state in `cur` is alive in the model through tick t, so the game killing this
        // lineage at or before t is a disagreement with the model by construction.
        if (checkWanted && !solved && !cur.empty() && isCheckpointLayer(t - t0)) {
            std::vector<uint8_t> plvl, pmode;
            plvl.reserve((size_t)(t - t0));
            pmode.reserve((size_t)(t - t0));
            for (uint32_t i = cur.front().parent; i != 0; i = arena[i].parent()) {
                plvl.push_back(arena[i].action());
                pmode.push_back(arenaMode[i]);
            }
            std::reverse(plvl.begin(), plvl.end());
            std::reverse(pmode.begin(), pmode.end());
            SearchCheckpoints::Point p;
            p.t0 = t0;
            p.tick = t0 + (long long)plvl.size();
            for (const PlanEdge& e :
                 planEdges(plvl, pmode, t0, (int)init.held, init.mode, g_oldLatency))
                p.edges.emplace_back(e.press, e.level);
            g_check.publish(std::move(p));
        }
        // ---- the caller has stopped caring ----
        // Read every layer: once the game has refuted a checkpoint, every layer after it is paid
        // for and thrown away. One relaxed load -- and read with no subscriber too, because the
        // mod also cancels when its session ends (the player left the level). Read only under
        // `dpcheck`, which is off by default, that cancel did nothing: the search ran on to its
        // end on its detached thread, and the next level's Solve sat queued behind it with the
        // old search's numbers on its HUD (lv16 left mid-search went on for 22 s at 2,000 states
        // while lv1 waited). The CLI never sets the flag, so its output is unchanged.
        if (g_check.cancel.load(std::memory_order_relaxed)) {
            std::printf("CANCELLED: at t=%lld (search abandoned by the caller)\n", t);
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = t;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;    // distinct from FAILED (1) and from --replay's unreadable plan (2)
        }
        // ...and the ladder that has taken the plain search beside it (the gamble, cliMain):
        // this attempt's answer can no longer be used. Never set outside a ladder.
        if (g_ladderStop.load(std::memory_order_relaxed)) {
            std::printf("STOPPED: at t=%lld (the ladder took the plain search beside it)\n", t);
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = t;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;
        }
        ppMark(5);
        // Publish where the loop has got to. Every layer, not every 500th: this is what a UI
        // watching from another thread samples, and the printed line below is far too coarse
        // to look alive on screen (see dp/progress.hpp)
        g_progress.layer(t, x, cur.size());
        // "alive=1" tells you the frontier collapsed but not WHERE the survivors
        // are; the y span and mode mix is what says whether the search is stuck
        // on one ledge or spread out. Report more often once it gets thin.
        // also report the tick the frontier actually collapses on -- a periodic
        // sample tells you it happened somewhere in the last 500 ticks, which is
        // not enough to find the cause
        const bool crashed = prevAlive >= 8 && cur.size() * 4 <= prevAlive;
        if ((t % 500) == 0 || crashed
            || (cur.size() <= 64 && (t % 10) == 0)) {
            double ylo = 1e9, yhi = -1e9;
            // cube / ship / ball / ufo / wave / robot / spider / swing.
            // ALL of them: the print used to stop at 4, so a mode portal that
            // never fired was invisible here -- which is exactly how lv19's
            // robot section was planned as a cube for a whole session.
            int nm[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            // ...and how many are MINI. A size portal that the frontier is
            // missing shows up here and nowhere else: on lv18 every state
            // stayed mini past x=20,386 and the level is unplayable that way.
            int nMini = 0;
            for (const State& s : cur) {
                ylo = std::min(ylo, (double)s.y);
                yhi = std::max(yhi, (double)s.y);
                if (s.mode < 8) ++nm[s.mode];
                if (s.mini) ++nMini;
            }
            // ...and the SPEED spread. Since speed is per state the frontier can
            // hold branches that took a speed portal and branches that flew past
            // it, and those are at different x at the same tick -- which is
            // exactly the freedom the old shared timeline could not represent.
            // "x" above is the leader; xlo says how far back the slowest is.
            double xlo = 1e18;
            std::vector<float> spds;
            for (const State& s : cur) {
                xlo = std::min(xlo, (double)s.xAbs);
                const float d = (s.dx > 0.f) ? s.dx : curDxF;
                if (std::find(spds.begin(), spds.end(), d) == spds.end())
                    spds.push_back(d);
            }
            std::sort(spds.begin(), spds.end());
            char sp[64] = "";
            for (size_t i = 0, o = 0; i < spds.size() && o + 8 < sizeof sp; ++i)
                o += (size_t)std::snprintf(sp + o, sizeof sp - o, "%s%.2f",
                                           i ? "/" : "", (double)spds[i]);
            std::printf("t=%lld x=%.0f alive=%zu arena=%zu y=[%.0f,%.0f] "
                        "cbswrpg=%d/%d/%d/%d/%d/%d/%d/%d mini=%d dx=%s xlo=%.0f "
                        "born=%lld died=%lld merged=%lld\n",
                        t, x, cur.size(), arena.size(),
                        cur.empty() ? 0.0 : ylo, cur.empty() ? 0.0 : yhi,
                        nm[0], nm[1], nm[2], nm[3], nm[4], nm[5], nm[6], nm[7],
                        nMini, sp,
                        cur.empty() ? 0.0 : xlo, nBornPrev, nDiedPrev,
                        nBornPrev - nDiedPrev - (long long)cur.size());
            if (g_memStat) {
                // 1) nodes still reachable from the live frontier. This is the
                // exact size a mark-compact GC would shrink the arena to, so
                // this one number decides whether a GC is worth building.
                std::vector<bool> mark(arena.size(), false);
                size_t reach = 0;
                for (const State& s : cur)
                    for (uint32_t i = s.parent; i != 0 && !mark[i];
                         i = arena[i].parent()) {
                        mark[i] = true;
                        ++reach;
                    }
                // 2) dual census. The mirror-compression proposal assumes the
                // frontier in a mirror region is a 2-body PRODUCT of the two
                // state sets; but while the pair IS a mirror, p2 is a function
                // of (p1, split axis) and the dedupe key already collapses it.
                // If dual ~= p1cells * axes there is no product to compress;
                // if dual >> p1cells * axes, the product is real.
                size_t nDual = 0, nMirror = 0;
                std::unordered_set<uint64_t> p1All, p1Mir, axes;
                for (const State& s : cur) {
                    if (!s.dual) continue;
                    ++nDual;
                    const bool flying = (s.mode == 1 || s.mode == 3);
                    const double ys = flying ? g_shipYq : g_cubeYq;
                    const double vs = flying ? g_shipVq : g_cubeVq;
                    const uint64_t c1 =
                        ((uint64_t)(uint32_t)(int32_t)std::lround(s.y * ys) << 32)
                        ^ (uint32_t)(int32_t)std::lround(s.vy * vs);
                    p1All.insert(c1);
                    // exact mirror: opposite gravity, opposite vy, both airborne
                    if (s.flip2 != s.flip && !s.grounded && !s.grounded2
                        && std::fabs((double)s.vy + (double)s.vy2) < 0.05) {
                        ++nMirror;
                        p1Mir.insert(c1);
                        // the split axis, 0.5 px bins: y2 = 2*axis - y
                        axes.insert((uint64_t)(int64_t)std::llround(
                            ((double)s.y + (double)s.y2)));
                    }
                }
                const double MiB = 1024.0 * 1024.0;
                std::printf(
                    "memstat t=%lld arena=%zu (%.0f MiB) reach=%zu (%.1f%%) "
                    "state=%zuB cur=%zu/%zu nxtcap=%zu seen=%zu/%zu\n",
                    t, arena.size(),
                    (double)(arena.size() * sizeof(Node)) / MiB, reach,
                    arena.empty() ? 0.0 : 100.0 * (double)reach / (double)arena.size(),
                    sizeof(State), cur.size(), cur.capacity(), nxt.capacity(),
                    seen.size(), seen.bucket_count());
                if (nDual)
                    std::printf(
                        "memstat-dual t=%lld dual=%zu p1cells=%zu product=%.1f "
                        "mirror=%zu (%.0f%%) p1mir=%zu axes=%zu\n",
                        t, nDual, p1All.size(),
                        (double)nDual / (double)p1All.size(), nMirror,
                        100.0 * (double)nMirror / (double)nDual, p1Mir.size(),
                        axes.size());
            }
            std::fflush(stdout);
        }
    }
    // Cap accounting, one line, parsed by the driver's tier ladder: a PARTIAL
    // with capHits=0 died of physics at full enumeration, so a bigger cap
    // cannot change the answer and the next tier is skipped.
    if (g_fireBCheck)
        std::printf("firebcheck: bit-without-tick=%llu tick-without-bit=%llu too-early=%llu"
                    " (both must be 0)\n", g_fireBNoTick, g_fireBNoBit, g_fireBTooEarly);
    std::printf("capstat: maxAlive=%zu capHits=%lld dropped=%lld cap=%zu\n",
                maxAlive, capHits, capDropped, g_aliveCap);
    // --groupxsplit: this search's own counts (zeroed here, so the next search in the call
    // starts from 0).
    if (g_groupXSplit) {
        std::printf("groupxsplit: gap>2*%.0f+|dx| layers-split=%lld max-parts=%d\n",
                    kGroupWinMargin, g_groupXSplitLayers, g_groupXSplitMax);
        g_groupXSplitLayers = 0;
        g_groupXSplitMax = 0;
    }
    if (g_heldCellCap && g_heldAll)
        std::printf("cellcap: spared=%lld cut=%lld twinsKept=%lld (layers over the cap in states "
                    "whose cells fit / layers cut by cells / twins kept past the cap)\n",
                    cellSpared, cellCuts, cellTwinsKept);
    if (g_searchCensus > 0) census.print();
    twin.print();
    // --keycensus: the per-box tally, with the uid so it can be joined against
    // objrects. Boxes that never divided the key are printed as 0 rather than
    // omitted -- "absent" and "zero" are the distinction this project keeps
    // getting wrong, and a box that cost nothing is the interesting case here.
    // --coindbg: the closest approach per coin, whether or not it was taken.
    if (g_coinDbgT >= 0 && coinOn)
        for (size_t i = 0; i < L.coins.size() && i < 8; ++i) {
            // The denominator is on the line, always: "never entered" reads the
            // same whether no state entered the bound or the search never got
            // that far, and this project has mis-read that 0 six times in one
            // night. `seen` is how many states were examined inside the bound,
            // `reachedX` is the deepest x the coin test itself ever saw.
            if (g_coinNearDy[i] < 0.0) {
                std::printf("coinnear: coin=%zu at (%.0f,%.0f) NEVER ENTERED "
                            "(seen=%lld, the coin test reached x=%.0f, this "
                            "coin's x bound starts at %.0f)%s\n",
                            i, L.coins[i].cx, L.coins[i].cy, g_coinSeen[i],
                            g_coinProbeMaxX, L.coins[i].cx - L.coins[i].hw - 15.0,
                            (g_coinProbeMaxX < L.coins[i].cx - L.coins[i].hw - 15.0)
                                ? "  <-- NOT YET: the search never got this far"
                                : "  <-- a real zero: the search passed it and no "
                                  "state was inside the x bound");
                continue;
            }
            std::printf("coinnear: coin=%zu at (%.0f,%.0f) closest |dy|=%.3f at "
                        "t=%lld player=(%.3f,%.3f)  yBound=%.1f  seen=%lld%s\n",
                        i, L.coins[i].cx, L.coins[i].cy, g_coinNearDy[i],
                        g_coinNearT[i], g_coinNearX[i], g_coinNearY[i],
                        L.coins[i].hh + 15.0, g_coinSeen[i],
                        (g_coinNearDy[i] <= L.coins[i].hh + 5.0) ? "  <-- inside" : "");
        }
    if (g_keyCensus) {
        long long tot = 0;
        for (int b = 0; b < kTouchBits; ++b) tot += g_keyCount[b].load();
        std::printf("keycensus: total=%lld over %zu boxes\n", tot, g_touch.size());
        for (size_t b = 0; b < g_touch.size() && b < (size_t)kTouchBits; ++b)
            std::printf("keycensus: box=%zu uid=%d cx=%.0f moveTicks=%lld splits=%lld\n",
                        b, g_touch[b].uid, g_touch[b].cx,
                        (long long)(b < g_touchMoveTicks.size() ? g_touchMoveTicks[b] : -1),
                        (long long)g_keyCount[b].load());
    }
    clearReport();
    g_outcome.capHits = capHits;
    if (!g_fixups.empty()) {
        std::printf("fixups: %lld transitions overridden this call\n",
                    g_fixupHits);
        // ...and in which frame the overridden state was. A record's dy is
        // GD's world dy; a rotated state's y is not on that axis. Printed
        // whether or not any rotated hit occurred, because "the path exists
        // and never fired" is the answer this was built to be able to give.
        // CALLS first, then HITS, on one line each and never merged: a hit count
        // without its call count cannot be read (0 hits means "never matched"
        // or "never asked", and those are different findings).
        // (frame, rev) for every STEP and for every fixup LOOKUP, side by side.
        // The mod rewrites GD's frame 2 as (frame 0, rev 1), so frame 2 can
        // arrive under either name and a frame-only count cannot say which.
        for (int f = 0; f < 4; ++f)
            for (int r = 0; r < 2; ++r)
                if (frameRevReach(f, r) || g_frameRevCall[f][r] || g_frameRevHit[f][r])
                    std::printf("framerev: f%d rev%d steps=%lld lookups=%lld hits=%lld\n",
                                f, r, frameRevReach(f, r), g_frameRevCall[f][r],
                                g_frameRevHit[f][r]);
        std::printf("fixupcall:  f0=%lld f1=%lld f2=%lld f3=%lld rotated=%d",
                    g_fixupCallFrame[0], g_fixupCallFrame[1],
                    g_fixupCallFrame[2], g_fixupCallFrame[3],
                    g_fixupCallRotSeen);
        for (int i = 0; i < g_fixupCallRotSeen && i < 8; ++i)
            std::printf(" x=%.1f", g_fixupCallRotX[i]);
        std::printf("\n");
        std::printf("fixupframe: f0=%lld f1=%lld f2=%lld f3=%lld rotated=%d",
                    g_fixupHitFrame[0], g_fixupHitFrame[1],
                    g_fixupHitFrame[2], g_fixupHitFrame[3], g_fixupRotSeen);
        for (int i = 0; i < g_fixupRotSeen && i < 8; ++i)
            std::printf(" x=%.1f", g_fixupRotX[i]);
        std::printf("\n");
    }
    // The reference watch's verdict. Printed whether or not it was lost: a run
    // that carried the reference all the way is the negative control, and it has
    // to be as visible as a loss or the instrument only ever speaks when it has
    // something to blame.
    if (g_refWatch)
        std::printf("refwatch: %s%s%s%s\n",
                    g_refLostAt < 0 ? "CARRIED to the end"
                                    : "lost (see the line above)",
                    // Past an ADOPT the carrier is a substitute, not the reference.
                    g_refAdoptT >= 0 ? " -- a SUBSTITUTE lineage since t=" : "",
                    g_refAdoptT >= 0 ? std::to_string(g_refAdoptT).c_str() : "",
                    g_refAdoptT >= 0 ? (", overlooked:" + g_refAdoptFields).c_str() : "");
    if (g_phaseProf) {
        ppMark(6);
        const double tot = std::chrono::duration<double>(std::chrono::steady_clock::now() - ppStart).count();
        std::printf("phaseprof: layers=%lld total=%.2fs setup=%.2f step=%.2f dedupe=%.2f cap=%.2f "
                    "gc=%.2f mem=%.2f rest=%.2f prep=%.2f\n", bestT - t0, tot, pp[0], pp[1], pp[2],
                    pp[3], pp[4], pp[5], pp[6],
                    std::chrono::duration<double>(ppStart - cliT0).count());
    }
    if (g_rjOn)
        std::printf("rejoin: t0=%lld after=%lld reached=%lld layers=%lld exact=%lld(m%d,n%lld) "
                    "near2=%lld(m%d,n%lld) near8=%lld(m%d,n%lld)\n",
                    t0, g_rjAfter, bestT, g_rjLayers,
                    g_rjFirst[0], g_rjMode[0], g_rjLayersNear[0],
                    g_rjFirst[1], g_rjMode[1], g_rjLayersNear[1],
                    g_rjFirst[2], g_rjMode[2], g_rjLayersNear[2]);
    // ---- no answer before every checkpoint is judged (dp/progress.hpp) ----
    // The search is over, but the caller may still be flying checkpoints it published. Wait for
    // every one of them: if the game refutes one, the caller throws this whole call away, and an
    // answer handed back first would make the outcome depend on which of the two finished first.
    // The flights are the game replaying a few thousand ticks; the search is not blocked while
    // they run, only its answer is. `enabled` going off (the session ended) releases the wait.
    if (checkWanted) {
        while (g_check.enabled.load(std::memory_order_acquire)
               && !g_check.cancel.load(std::memory_order_acquire)
               && g_check.judged.load(std::memory_order_acquire) < g_check.published())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (g_check.cancel.load(std::memory_order_acquire)) {
            std::printf("CANCELLED: after the search, waiting on a checkpoint (search abandoned "
                        "by the caller)\n");
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = bestT;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;
        }
    }
    // The frontier died before the cut. That is still useful to the driver: the
    // deepest surviving branch is the best guess at how to get near the wall,
    // and replaying it in GD is how the wall gets localized at all. Emit it
    // (truncated to the cut) instead of nothing -- but only in horizon mode,
    // because a tail solve that returns a doomed plan would look like success
    // to the driver's `Test-Path $tail` check and the loop would never move.
    if (!solved && horizon > 0 && !nxt.empty()) {
        solved = true;
        goalState = pickOf(nxt);
        planCut = 0;   // no truncation: the driver wants to reach the wall
        std::printf("PARTIAL: frontier died at t=%lld x=%.1f, emitting the "
                    "deepest branch\n", bestT, bestX);
        // PARTIAL is announced BEFORE `solved` is forced true, so the SOLVED line below is
        // printed for it as well. The driver resolves that by letting PARTIAL win; the same
        // precedence is kept here (the SOLVED site does not overwrite a PARTIAL verdict)
        g_outcome.verdict = VerdictPartial;
        g_outcome.deepT = bestT;
        g_outcome.deepX = bestX;
    }
    // ...and whether it died at a coin's miss prune: the deepest x at the far edge of a coin whose
    // passing is final, where every state still without it is cut (the prune above: past cx + hw
    // by the player's half, at most 15, plus one tick's travel).
    if (g_outcome.deepX >= 0.0 && coinOn)
        for (size_t ci = 0; ci < L.coins.size() && ci < coinPruneOk.size(); ++ci) {
            if (!coinPruneOk[ci]) continue;
            const double edge = (double)L.coins[ci].cx + (double)L.coins[ci].hw;
            if (bestX >= edge - 5.0 && bestX <= edge + 40.0) {
                g_outcome.coinWall = (int)ci;
                std::printf("coinwall: the frontier died at coin %zu's miss prune (x %.1f, far "
                            "edge %.1f)\n", ci, bestX, edge);
                break;
            }
        }
    if (!solved) {
        std::printf("FAILED: frontier died at t=%lld x=%.1f\n", bestT, bestX);
        g_outcome.verdict = VerdictFailed;
        g_outcome.deepT = bestT;
        g_outcome.deepX = bestX;
        // after the final swap the previous layer's states sit in nxt
        for (size_t i = 0; i < nxt.size() && i < 12; ++i)
            std::printf("  prev[%zu]: y=%.2f vy=%.2f mode=%d g=%d held=%d\n", i,
                        (double)nxt[i].y, (double)nxt[i].vy, (int)nxt[i].mode,
                        (int)nxt[i].grounded, (int)nxt[i].held);
        return 1;
    }
    std::printf("SOLVED at x=%.0f, reconstructing plan\n", (double)goalState.y * 0 + goalX);
    // Everything past this line is the reconstruction, so DIE can tell the two
    // apart (see g_inRecon in constants.hpp). One aggregate line at the end.
    g_inRecon = true;
    if (g_outcome.verdict != VerdictPartial) g_outcome.verdict = VerdictSolved;

    // walk the arena back to get the per-tick input level
    std::vector<uint8_t> lvl;
    for (uint32_t i = goalState.parent; i != 0; i = arena[i].parent())
        lvl.push_back(arena[i].action());
    std::reverse(lvl.begin(), lvl.end());
    // truncate to the horizon cut (see planCut). lvl[i] is the input at tick
    // t0+i+1, so keeping (planCut - t0) entries ends the plan exactly at planCut.
    if (planCut > 0 && (long long)lvl.size() > planCut - t0)
        lvl.resize((size_t)std::max<long long>(0, planCut - t0));
    // --rejoinuse: the search stopped on the old plan's trajectory at g_rjJoinT; the rest is the
    // old plan's own input levels, read off its trace (act at tick k steps into tick k, which is
    // lvl[k - t0 - 1]). The witness walk below covers the joined plan whole.
    if (g_rjJoinT >= 0 && (long long)lvl.size() == g_rjJoinT - t0) {
        size_t added = 0;
        for (long long k = g_rjJoinT + 1;; ++k) {
            const auto it = g_rjRows.find(k);
            if (it == g_rjRows.end() || it->second.act < 0) break;
            lvl.push_back((uint8_t)it->second.act);
            ++added;
        }
        std::printf("rejoin: the plan carries on with %zu ticks of the old plan's inputs\n", added);
    } else if (g_rjJoinT >= 0) {
        std::printf("rejoin: the prefix is %zu ticks, not %lld - not joined\n", lvl.size(),
                    g_rjJoinT - t0);
    }
    std::vector<uint8_t> modeAt(lvl.size(), 0);  // filled by the resim below

    // re-simulate the witness and write its per-tick trace (for diffing
    // against a GD dump to localize model divergences)
    {
        std::ofstream tr(outPath + ".trace.csv");
        // 6 significant digits hid the thing we most often need to see: whether
        // x is a hair over or under a contact boundary. Print enough to compare
        // against the MOD's snaptrace, which gives 4 decimals.
        tr.precision(10);
        // dual/y2/vy2/flip2: the second body's track. lv16's late wall sat
        // undiagnosed for a whole session because the trace carried p1 only --
        // gd_diff said "full match" while GD was killing PLAYER 2 (the death
        // looked position-independent: same tick for every injected p1 y/x).
        // act: the input level of the tick (the fixup recorder gates on it).
        // ...and `flip` and `frame`, appended. --refwatch looks up its reference
        // columns by NAME (refwatch.hpp:108 asks for "flip" and "frame") and this
        // trace carried neither, so feeding a resim trace back to the search
        // reported `cannot-reproduce` with dy=0.000 dvy=0.000 -- the trajectory
        // matched to seven figures and the match was refused on two fields that
        // were not in the file. refwatch.hpp:42-46 already records that the frame
        // mismatch "accounted for most of the first sweep's cannot-reproduce
        // count"; this is the other half of that, on the writing side.
        //
        // Appended rather than inserted: every reader found is name-based
        // (mcp/gdmcp/data.py builds a csv.DictReader and validates the columns it
        // wants against r.fieldnames), and the positional reads in the tree index
        // into diff rows, regex groups and triggers.txt -- not this file.
        // ...and the ROTATION QUEUE's three per-state values. The anchor scan
        // does not seed them (frames.hpp:158-174), which is why --rotqueue is
        // opt-in and why --startrotq exists. The producer of a --startrotq seed
        // needs their value at the tick the next anchor will use, and this walk
        // passes every such tick: recording them here means the seed is READ
        // OFF a walk rather than DERIVED from something adjacent. Deriving is
        // what --spentrot has to do from the dump's frame transitions, and it
        // cannot see the entries that change no frame (2899 and chanOnly).
        // `dead`, appended last for the same reason as the columns above: the walk's own kill on
        // the tick, so a later reader (--rejoinuse) can tell the walk's deaths from its corpse
        // rows -- the walk runs on through a kill, and y/vy alone do not say where it fired.
        tr << "tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act,flip,frame"
              ",rotspent,rotchan,rotrev,dead,key,key_fields\n";
        std::ofstream sn;
        if (!snapLogPath.empty()) {
            sn.open(snapLogPath);
            g_snapOut = &sn;
        }
        State s = init;
        // GAMEPLAY ROTATION: the witness resim did not turn. `applyRotation` had
        // exactly two callers -- the search's kid (:3108) and --replay (:2654) --
        // and this walk, which is the one that writes the trace everything is
        // diffed against, was not one of them. On lv22 the search is in the
        // rotated frame from t=6,000 (its layer line reads `x=0 y=[8272,8272]`,
        // which is the turned coordinate system) while this loop kept walking
        // +X. So the trace was not the plan's trajectory, and every death
        // counted here after a turn was the walk's, not the plan's.
        //
        // The slices have to be re-bound with the frame for the same reason
        // --replay re-binds (:2440): after a turn the geometry that matters is
        // the NEW frame's, and reading the old frame's is worse than not
        // turning at all. XSlice holds a reference, so it cannot be assigned --
        // hence the pointers, which is also how --replay carries them.
        // --witnessframe (default off): bind the frame the call STARTS in, as
        // --replay does at its own Lf. Fixed at &L, a call anchored in a turned
        // frame walks frame 0's geometry: lv22 plan 466 (t0=6127, frame 1) fell
        // through uid 5845 at t=6162 where --replay lands, missed the turn at
        // 6299 and reported escapee-prune at 6505. The walk also fills modeAt,
        // which sets the emitted edges' latency, so this is not print-only.
        Level* rLf = &frameLevel(L, (int)init.frame);
        std::unique_ptr<XSlice> sl, pl, dl, ol, sls, vl;
        auto rrebind = [&](Level& lv) {
            sl = std::make_unique<XSlice>(lv.objs);
            pl = std::make_unique<XSlice>(lv.portals);
            dl = std::make_unique<XSlice>(lv.pads);
            ol = std::make_unique<XSlice>(lv.orbs);
            sls = std::make_unique<XSlice>(lv.slopes);
            vl = std::make_unique<XSlice>(lv.speeds);
        };
        rrebind(*rLf);
        FlyBand rBand;
        size_t rIdx = 0;
        // Fallback speed only: replayed by x for an anchor that carries no
        // measured multiplier, and used solely to seed `init.dx` (the state's
        // own speed is what the resim actually runs on).
        float rDxF = kDxF;
        size_t rSpd = 0;
        auto rGate = [&](const Obj& o) { return o.cx - o.hw - kCubeHalf; };
        while (rSpd < L.speeds.size() && rGate(L.speeds[rSpd]) <= x0) {
            rDxF = dxForSpeedId(L.speeds[rSpd].id);
            ++rSpd;
        }
        // GD's measured speed at the anchor beats replay-by-x here too, same
        // as the search's curDxF above. Without it, any re-anchor whose
        // replay-by-x speed differs from the measured one would run the
        // resim's LAYER x (collision windows) at the wrong rate while every
        // state's own xAbs ran at the right one -- an invisible desync (the
        // trace's x column prints s.xAbs). The lv16 anchors checked so far
        // happened to agree by luck; this closes the class.
        if (g_startSpeedMul > 0.0) rDxF = dxForSpeedMul(g_startSpeedMul);
        for (size_t i = 0; i < lvl.size(); ++i) {
            const long long t = t0 + (long long)i + 1;
            // The witness's windows come from ITS OWN x, at ITS OWN speed --
            // the same numbers stepOne will compute one line later. The resim
            // used to keep a second accumulator (rxF) switched by a replay of
            // the speed portals; the moment the two disagreed, the trace was
            // simulated against geometry the state was not actually at. Now
            // there is only one x to disagree about, and the speed portals are
            // fired by stepOne on contact (see State::dx).
            // Under reverse (State::rev) x decreases. Apply the same sign
            // here as the DP side's group windows (if the replay / resim do
            // not match GD, every fixup comparison becomes a lie).
            const float rDxUsed = ((s.dx > 0.f) ? s.dx : rDxF)
                                  * (s.rev ? -1.f : 1.f);
            const double xPrevR = (double)s.xAbs;
            const double x = (double)advanceX(s.xAbs, rDxUsed);
            // (band is per state; the resim inherits it from `init`)
            std::vector<const Obj*> rn, rp, rd, ro, rs;
            // ...and the witness resim has to see the SAME moving geometry the
            // search planned against, or its trace disagrees with its own plan
            rLf->dyn.seek((int)t);
            seekOtherFrames(rLf, (int)t);   // --rotpretap only
            // ...including the doors IT opened. The witness carries its own mask
            // (stepBoth sets it below), so this is the same call the search made
            // for the group this lineage belonged to.
            // + this tick's advance: see the group call's note. `x` is tick t's
            // x and `s.xAbs` is tick t-1's, so their difference is it.
            rLf->dyn.applyTriggers(s.trig, (int)s.trigT, s.fireB,
                                   s.lockOff + (float)(x - (double)s.xAbs),
                                   (int)t, x);
            std::vector<std::pair<const TouchTrig*, TouchMask>> rt;
            // --witnessframe: the boxes in THIS frame's coordinates, as --replay
            // (touchFor above) and the search do. g_touch is frame 0's, so a walk
            // that turns part-way could never touch a box inside the turned
            // section: lv22 box 30 drops the spike row at 213.25 at t=1,758 in
            // frame 1, and a witness that missed it teleported onto the spikes.
            const std::vector<TouchTrig>& tw = touchFor((int)s.frame);
            for (size_t b = 0; b < tw.size(); ++b) {
                if (s.trig & touchBit(b)) continue;
                const TouchTrig& T = tw[b];
                // --walkgates: the search's window keeps these (see `standing` there).
                const bool standing = (T.id == 1611 || T.id == 3620)
                                      && T.count >= 0 && !T.tap;
                const bool anywhere = T.id == 3620 && T.count >= 0;
                if (!anywhere && !standing && T.cx + T.hw < x - 40) continue;
                if (!anywhere && T.cx - T.hw > x + 40) continue;
                rt.push_back({&T, touchBit(b)});
            }
            sl->forRange(x - 40, x + 40, [&](const Obj& o) { rn.push_back(&o); });
            rLf->dyn.collect(Dynamics::NEAR, x - 40, x + 40, rn);
            pl->forRange(x - 60, x + 60, [&](const Obj& o) { rp.push_back(&o); });
            rLf->dyn.collect(Dynamics::PORT, x - 60, x + 60, rp);
            dl->forRange(x - 40, x + 40, [&](const Obj& o) { rd.push_back(&o); });
            rLf->dyn.collect(Dynamics::PAD, x - 40, x + 40, rd);
            ol->forRange(x - 50, x + 50, [&](const Obj& o) { ro.push_back(&o); });
            rLf->dyn.collect(Dynamics::ORB, x - 50, x + 50, ro);
            sls->forRange(x - 60, x + 60, [&](const Obj& o) { rs.push_back(&o); });
            rLf->dyn.collect(Dynamics::SLOPE, x - 60, x + 60, rs);
            std::vector<const Obj*> rv;
            vl->forRange(x - 80, x + 80, [&](const Obj& o) { rv.push_back(&o); });
            rLf->dyn.collect(Dynamics::SPEED, x - 80, x + 80, rv);
            std::vector<const Obj*> rnSolid;   // --solidorder: rn in GD's solid order, once per tick
            solidOrderInto(rn, rnSolid);
            const StepCtx K{x, xPrevR, rDxUsed, t, &rn, &rp, &rd, &ro, &rs, &rv, &SP, &SPmini, &UP, &UPmini, &rt,
                            nullptr, nullptr, &rnSolid};
            bool rdead = false;
            const bool rPrevGrounded = (s.grounded != 0);
            const double rPrevY = (double)s.y;
            const int rFrame0 = (int)s.frame;
            State c = stepBoth(s, lvl[i], K, rdead);
            // ...and turn, the way the search's kid does at :3108. Same gate
            // (`!dead && !g_rotTrig.empty()`), same arguments, and it has to run
            // BEFORE the row is written or the trace records a position in a
            // coordinate system the plan had already left.
            if (!rdead && !g_rotTrig.empty()) {
                applyRotation(c, xPrevR, (double)rDxUsed, t, lvl[i],
                              rPrevGrounded, rPrevY, true);
                if ((int)c.frame != rFrame0) {
                    rLf = &frameLevel(L, (int)c.frame);
                    rrebind(*rLf);
                }
            }
            // --walkgates: the item gates the search's own step fires (itemGates),
            // so the witness walks the world the search planned in. Same gate as
            // the search's: alive, and not on a tick that turned the player.
            if (coinOn && !rdead && (int)c.frame == rFrame0)
                itemGates(c, s.action, (int)lvl[i], K, rFrame0, (double)c.xAbs,
                          (double)c.y, hazardHalfFor(c.mode, c.mini != 0), t);
            // ...and rdead is not read. It was declared, passed, and dropped:
            // the witness resim walks the whole plan whether or not the player
            // survived it, so a plan that dies at tick 40 of 1,200 still writes
            // 1,160 rows of a corpse and the run still prints SOLVED (which was
            // printed before this loop, by the search). Count it here rather
            // than at DIE: this is the ONE walk of the FINAL plan, so a death
            // counted here is the plan's, with none of the search's prunes in
            // it. First and last are recorded too, because "one corpse dying
            // every tick" and "many separate deaths" produce the same total and
            // are told apart only by whether the ticks are contiguous.
            if (rdead) {
                if (g_resimDead == 0) {
                    g_resimFirst = (int)t;
                    g_resimWhy = g_deadWhy;
                    // ...and where the player was, in world x, for --vetophys's deepX
                    // (the loop's PARTIAL gate compares deepX with the verified x).
                    {
                        double wxR, wyR;
                        fromFrame((int)c.frame, (double)c.xAbs, (double)c.y, wxR, wyR);
                        g_resimPX = (float)wxR;
                    }
                    // ...and the object, from the same DIE that set the cause.
                    // g_deadObj is the killer; its uid is the level's, not this
                    // build's ordinal, so it can be looked up in the dump.
                    g_resimUid = g_deadObj ? g_deadObj->uid : -1;
                    g_resimObjX = g_deadCx;
                    g_resimObjY = g_deadCy;
                    g_resimTrig = s.trig;
                    // ...and the frame those coordinates are expressed in.
                    g_resimFrame = (int)c.frame;
                }
                g_resimLast = (int)t;
                ++g_resimDead;
            }
            ++g_resimTicks;
            c.action = (uint8_t)lvl[i];
            s = c;
            modeAt[i] = s.mode;
            const SearchKey traceKey = keyOf(s, (long long)t);
            tr << t << ',' << (double)s.xAbs << ',' << s.y << ',' << s.vy << ','
               << (int)s.mode << ',' << (int)s.grounded << ',' << (int)s.dual
               << ',' << s.y2 << ',' << s.vy2 << ',' << (int)s.flip2 << ','
               << (int)lvl[i] << ',' << (int)s.flip << ',' << (int)s.frame
               << ',' << s.rotSpent << ',' << (int)s.rotChan
               << ',' << (unsigned)s.rotRev << ',' << (rdead ? 1 : 0)
               << ',' << (unsigned long long)SearchKeyHash{}(traceKey)
               << ',' << keyText(traceKey) << "\n";
            // --rejoinuse: past the join the walk must retrace the old plan's walk -- the same
            // fields on every tick the old trace has, and no kill it did not have. The first tick
            // it does not is the join failing its own premise (repair.hpp refuses the join).
            if (g_rjJoinT >= 0 && (long long)t > g_rjJoinT && g_outcome.rejoinBadT < 0) {
                const auto ot = g_rjRows.find((long long)t);
                if (ot != g_rjRows.end()) {
                    const RefRow& o = ot->second;
                    const char* why = nullptr;
                    if ((o.mode >= 0 && (int)s.mode != o.mode) || (o.flip >= 0 && (int)s.flip != o.flip)
                        || (o.frame >= 0 && (int)s.frame != o.frame))
                        why = "mode/flip/frame";
                    else if (std::fabs((double)s.y - o.y) > g_refEps
                             || std::fabs((double)s.vy - o.vy) > g_refEps)
                        why = "y/vy";
                    else if (rdead && o.dead == 0)
                        why = "a kill the old walk did not have";
                    if (why) {
                        g_outcome.rejoinBadT = (long long)t;
                        g_outcome.rejoinBadWhy = why;
                        std::printf("rejoin: the joined walk leaves the old one at t=%lld (%s)\n",
                                    (long long)t, why);
                    }
                }
            }
        }
        g_snapOut = nullptr;
    }

    if (!g_bandPath.empty()) {
        std::ofstream bo(g_bandPath);
        bo << "tick,alive,ylo,yhi,x,capdrop,merged,classes\n";
        for (const BandRow& b : g_bands)
            bo << b.t << ',' << b.alive << ',' << b.ylo << ',' << b.yhi << ','
               << b.x << ',' << b.capdrop << ',' << b.merged << ',' << b.cls
               << "\n";
        std::printf("bands: %zu layers -> %s\n", g_bands.size(),
                    g_bandPath.c_str());
    }
    // emit input edges. Latency is PER MODE (measured on lv1): cube presses
    // act on the next tick (+1), ship/UFO two ticks later (+2).
    std::ofstream out(outPath);
    g_outcome.planWritten = true;   // ...and the trace above with it (cliMain's step 5 keeps both)
    // prev is the button state the plan STARTS from, and on a re-anchor that is
    // `--start`'s held field, not 0. Hard-coding 0 dropped the very first edge
    // whenever a tail was solved from a HELD anchor: the tail's own model
    // released on its first tick, and the edge that release needs was never
    // written, so the emitted plan could not reproduce the trace the same run
    // had just printed.
    //
    // Measured 2026-08-09, lv20 cold, anchor t0=717 (mini wave, held from
    // t=698). The tail's trace turns the dart down at t=718 and needs
    // `input=717,0` (wave latency 1). Nothing was emitted; the driver's splice
    // guessed `input={t0-1},0` = 716 instead, one tick early, which is 2 ticks
    // of the dart's dy = 5.193 px of permanent offset. Replaying the SAME tail
    // in GD with only that seam moved:
    //
    //   input=716,0  ->  death t=723 x=938.64   (16 ticks short)
    //   input=717,0  ->  death t=739 x=959.42   = the tail's own horizon, and
    //                    GD matches the tail's y to 4 decimals at every tick
    //
    // That one tick was the whole "grind": the tail was right, the plan that
    // carried it was not, GD died early, and the driver logged the miss as a
    // `kill` fixup -- 11 of them inside a 44 px band in 13 iterations, none of
    // which was a physics gap. Emitting the edge here (instead of letting the
    // caller guess) also puts the latency where the latency logic already
    // lives: `latOf` below is per-mode, and the driver's fixed t0-1 is only
    // ever right for ship/UFO (lat 2).
    // The scan itself now lives in planEdges (top of this file), because the common-prefix
    // publisher inside the layer loop has to produce the same edges for the same ticks. The
    // note below is about the RULE, and is kept here where the plan is written.
    int edges = 0;
    {
        // ...and the UFO is mode 3, which this test was missing: it got the
        // cube's +1 and every flap in the plan came out one tick late in GD.
        // Measured on lv12: pressed at t=170, GD flapped at t=172.
        // The mode that decides the latency is the one the input is applied
        // UNDER, i.e. the mode at the START of that tick -- modeAt[i] is
        // the mode at its END, which on a mode-portal tick is already the
        // NEW one. lv19 t=19,525 is exactly that tick: the input is a
        // WAVE's (lat 1) but the state ends as a UFO (lat 2), the edge was
        // emitted one tick early, and GD turned the wave down one tick
        // before the model did. The model then floated 1.3 px higher,
        // fired the UFO portal at (27,315,333) that GD does not reach, and
        // every cold run oscillated between x=27,297 and x=27,328.
        // The mode that decides the latency is the one GD is in when the
        // BUTTON is pressed, which is `lat` ticks before the effect -- not
        // modeAt[i], the mode the tick ENDS in. On a mode-portal tick those
        // differ, and lv19 t=19,525 is exactly that tick: the input is a
        // WAVE's (lat 1) but the state ends as a UFO (lat 2), so the edge
        // went out one tick early, GD turned the wave down a tick before
        // the model did, the model floated 1.3 px higher and fired the UFO
        // portal at (27,315,333) that GD never reaches. Every cold run
        // oscillated between x=27,297 and x=27,328.
        // Resolved in two passes because `lat` picks its own lookback:
        // start from the mode at the START of the effect tick, then take
        // the mode at the tick that guess points at.
        const std::vector<PlanEdge> pe =
            planEdges(lvl, modeAt, t0, (int)init.held, init.mode, g_oldLatency);
        for (const PlanEdge& e : pe) {
            out << "input=" << e.press << ',' << e.level << "\n";
            ++edges;
        }
    }
    // AFTER the reconstruction, not with `capstat:`. A non-zero count means the
    // plan this run just reported does not survive its own replay: the search
    // reached the goal, and rebuilding that path then died.
    // [2026-09-08] This print started life beside `capstat:` at :3898, which is
    // FIFTY LINES BEFORE the reconstruction begins, so it read the counter
    // before anything could increment it and reported n=0 for every case --
    // including the one where the reconstruction death had already been
    // observed directly. The `inRecon` field is what caught it: it printed 0
    // where it had to be 1, which is why a probe should report the state it
    // depends on and not only its answer (the same reason `pogate` prints its
    // seven conjuncts).
    std::printf("recondie: n=%lld inRecon=%d last=%s\n",
                g_dieRecon, g_inRecon ? 1 : 0,
                (g_dieRecon && g_deadWhy) ? g_deadWhy : "-");
    // ...and the split, because the total is not a number about physics: the
    // `out-of-play` bound is a search prune. Every cause is listed, so the
    // reader is not left inferring the population from the last entry.
    for (const auto& e : g_dieReconWhy)
        std::printf("recondiewhy: %s=%lld\n", e.first ? e.first : "?", e.second);
    // ...and the walk itself. `contig` is the test, not a decoration: if the
    // dead ticks are the tail of the walk, one plan died once and the rest is
    // its corpse; if they are not, the total was never one number about one
    // event. `of` is here so a zero can be told from a walk that never ran.
    // READ THE COUNTERS ONCE. The printf and the publish below used to read the
    // globals separately, twenty lines apart, and that is enough to make the two
    // channels disagree about the same call: nothing serialises cliMain (there
    // is no lock in src/mod/dp_bridge.cpp, three call sites in repair.hpp, and
    // this project has already met a solver thread that outlived its session --
    // the orphaned-worker case), so a second walk mutating g_resim* between the
    // two reads publishes a number no printf ever emitted.
    //
    // That is not hypothetical arithmetic. In the 2026-09-08 cold run
    // result.txt carries `resimdie=10@19554` and BOTH logs -- the suite's stdout
    // capture and Geode's own -- hold 423 resimdie lines each, agree on 23 with
    // dead>0, and contain no `dead=10` at all. The cause was never pinned down;
    // this removes the one mechanism that could produce it from here.
    const long long rDead = g_resimDead, rOf = g_resimTicks;
    const int rFirst = g_resimFirst, rLast = g_resimLast;
    const char* rWhy = g_resimWhy;
    // --vetophys (default off). A SOLVED whose witness walk dies of a physical
    // cause is the model agreeing that the route dies, which is what the repair
    // loop means by PARTIAL (checkPhantom: "a PARTIAL tail is the model agreeing
    // that the route dies, which is not a phantom"). So publish it as the PARTIAL
    // at that death, and let the loop's own PARTIAL gate decide whether to play
    // it. The witness is bound to its own frame's geometry (--witnessframe,
    // always on since the flag clean-up), and only physical causes count -- a search prune
    // (escapee-prune, deadband, maxplayy, out-of-play) is not the model's
    // physics. The emitted tail is not cut, as for any PARTIAL.
    if (g_vetoPhys && rDead > 0 && g_outcome.verdict == VerdictSolved && rWhy) {
        static const char* const kPhys[] = {"cube/hazard", "wave/hazard", "fly/hazard",
                                            "spider/tp-hazard", "cube/solid-side",
                                            "fly/solid-side", "crush"};
        bool phys = false;
        for (const char* p : kPhys)
            if (!std::strcmp(rWhy, p)) phys = true;
        if (phys) {
            g_outcome.verdict = VerdictPartial;
            g_outcome.deepT = rFirst;
            g_outcome.deepX = g_resimPX;
            std::printf("vetophys: SOLVED -> PARTIAL at t=%d x=%.1f (witness %s, start frame %d)\n",
                        rFirst, (double)g_resimPX, rWhy, (int)init.frame);
        }
    }
    std::printf("gfirestat: groups=%lld spread=%lld sum=%lld max=%d\n",
                g_gfireGroups, g_gfireSpread, g_gfireSum, g_gfireMax);
    std::printf("resimdie: dead=%lld of=%lld first=%d last=%d contig=%d why=%s\n",
                rDead, rOf, rFirst, rLast,
                (rDead > 0 && (long long)(rLast - rFirst + 1) == rDead) ? 1 : 0,
                rWhy ? rWhy : "-");
    if (g_resimDead > 0)
        std::printf("resimwho: uid=%d obj=(%.1f,%.1f) frame=%d trig=0x%08llx\n",
                    g_resimUid, g_resimObjX, g_resimObjY, g_resimFrame,
                    (unsigned long long)g_resimTrig.word(0));
    // --verdictinfo (default off): one line per SOLVED call, carrying that call's
    // verdict beside its OWN witness walk's death. Print only -- the verdict is not
    // touched here and the plan is not withheld. The direction is "send the plan,
    // attach the known death as information" (audit 05:02), which is why this sits
    // after the verdict is settled and changes nothing about it.
    //
    // Emitted for EVERY SOLVED call, dead=0 included, so the line carries its own
    // denominator. "How many SOLVED plans had a dying witness" cannot be read off
    // lines that exist only when one died -- the same discipline as the resimdie=?
    // note above: a value that is missing must be distinguishable from a zero.
    //
    // Reads the SNAPSHOT (rDead/rOf/rFirst/rLast/rWhy) rather than g_resim*. Nothing
    // serialises cliMain, and re-reading those globals here is exactly the mechanism
    // the READ THE COUNTERS ONCE note removed after the 2026-09-08 cold run published
    // a resimdie=10 that no printf ever emitted. uid/frame are read once, right here,
    // and only when there was a death to describe.
    if (g_verdictInfo && g_outcome.verdict == VerdictSolved) {
        const int vUid   = (rDead > 0) ? g_resimUid   : -1;
        const int vFrame = (rDead > 0) ? g_resimFrame : -1;
        std::printf("vinfo: verdict=SOLVED dead=%lld of=%lld first=%d last=%d "
                    "why=%s uid=%d objframe=%d startframe=%d\n",
                    rDead, rOf, rFirst, rLast,
                    (rDead > 0 && rWhy) ? rWhy : "-",
                    vUid, vFrame, (int)init.frame);
    }
    // Push this call's tail out NOW. stdout is a pipe here (Geode captures it), so it is
    // block-buffered: everything printed after the search loop -- capstat: at the top of
    // this block through resimdie:/resimwho:/vinfo: -- sits in the buffer until the NEXT
    // call's search output happens to fill it. Every call but one is rescued by its
    // successor; the last call of a run has none, so its whole tail dies with the process
    // and a verdict that result.txt records is simply absent from the log. Measured on
    // 2026-09-12: one SOLVED call's block missing, which made a healthy instrument look
    // like a broken one and cost that run its (1).
    //
    // The two existing flushes (the gc line and memstat) are both INSIDE the search loop,
    // so neither covers this block, and nothing in the tree calls setvbuf.
    //
    // Outside the --verdictinfo guard on purpose: the loss is a property of the block, not
    // of that flag. Inside it, a run with the flag off would keep losing its tail while the
    // flagged case looked fixed -- and the run that found this had the flag on.
    //
    // Not behind a flag of its own: this changes no output byte, only when the bytes leave
    // the buffer. A default-off flag would leave the instrument broken by default, which is
    // the opposite of what defaulting off protects.
    std::fflush(stdout);
    // ...and out through the struct, because the repair loop -- the one consumer
    // that ACTS on these plans -- reads dp::g_outcome, not stdout. A walk that
    // never ran stays -1 and must not be counted as a clean plan.
    //
    // The printf is not lost in the mod, though 98c36ed's message says it is:
    // Geode captures stdout into its own log, and the whole first cold run of
    // this counter was read out of there. What dp_bridge.hpp:57 says is that
    // there is no pipe to the PYTHON DRIVER, which is a narrower claim than the
    // one that got written down. The value of this field is correlation -- it
    // sits on the iteration's own line beside capHits and the verdict -- not
    // that the number would otherwise be unobtainable.
    //
    // Note the gate below is one-way, and the asymmetry is load-bearing when the
    // two channels disagree: the printf above is unconditional, so "printed but
    // not published" happens (that is exactly what `resimdie=?` means) and
    // "published but not printed" cannot come from this path.
    if (rOf > 0) {
        g_outcome.resimDead = rDead;
        g_outcome.resimFirst = rFirst;
        g_outcome.resimWhy = rWhy;   // a string literal: static, and dp is
                                     // linked into the mod, so it outlives
                                     // the call the way the others do not
        // The killer travels with the cause. Read from the same locals-once
        // discipline as the counters above, and left at their reset values when
        // the walk found nothing -- `uid=-1` is "no death here", not "unknown".
        g_outcome.resimUid = g_resimUid;
        g_outcome.resimObjX = g_resimObjX;
        g_outcome.resimObjY = g_resimObjY;
        g_outcome.resimTrig = g_resimTrig;
        g_outcome.resimFrame = g_resimFrame;
    }
    std::printf("plan: %d edges, %zu ticks -> %s\n", edges, lvl.size(),
                outPath.c_str());
    return 0;
}

// --capladder <c0>: solve with a small alive cap first, and raise it only where that search runs
// out of states.
//
// Measured 2026-09-25 on the official lv1-15 from t=0 (one search each, the loop's cap 2000 as
// the reference): cap 125 reaches the end on 13 of 15 levels with 0.068-0.099 of the stepped
// children, and where it does not (lv11, lv12) the frontier dies at one or two narrow passages.
// Raising the cap over a window starting some distance BEFORE the passage gets through it
// (lv12: 1000 from >= 300 px before, not 100; lv11: 250 from >= 600 px), because the passage
// needs states that have to be kept on the way to it, not at it. Which cap a passage needs also
// depends on what the search kept earlier (lv11's second passage fails with 250 from x=12,111 to
// the end, and passes with 250 from 3,000 px before it), so each attempt is a whole search again
// rather than a resumed one.
//
// The attempts:
//   1. --cap c0 everywhere.
//   2. The frontier died at x_w: add a window [x_w - 600, x_w + 300] at 4 * c0, search again.
//   3. Died inside a window again: that window's cap x4 (at most --cap) and its lead x2. Died
//      outside every window: a new window at the new wall. Windows are kept, since each attempt
//      starts from t0.
//   4. Died inside a window that already has the full --cap, or more than kLadderMaxAttempts
//      attempts: the plain search with the caller's own arguments, i.e. exactly what runs
//      without the flag. Also straight after an attempt whose alive cap never bound (capHits
//      0): the attempts after it only raise caps, and a cap no layer reaches cuts nothing (the
//      layer loop truncates only a layer over it), so each would be that search again. In the
//      official levels with coins, 311 of the ladders' 764 attempts came after one.
//   5. The plain search's frontier died too, on an EARLIER tick than the deepest attempt's did:
//      that attempt is searched again (the first attempt to reach that tick, with its own map)
//      and its result is the ladder's. The alive cap is not monotone -- measured 2026-09-26 with
//      --heldall, the plain cap-2000 search died at lv21 t=1,961 and lv19 t=2,317 where the
//      ladder's attempts had reached t=13,930 and t=11,350 -- and without this step the ladder
//      handed back the shallower plan. "Deeper" is the tick the frontier died on (deepT), not x,
//      which runs backwards in a reversed section; on a tie the plain search's result stands.
//      A death is still a death either way (PARTIAL/FAILED): this picks which one, not whether.
// Anything that is not a frontier death (SOLVED, a horizon cut, CANCELLED, an early return)
// ends the ladder with that attempt's result. Every attempt writes the same output files, so
// the last search's are the ones left behind -- which is why step 5 searches again rather than
// keeping copies: the plan, its trace and g_outcome all come from that one search.
//
// A search restriction only: every attempt steps the model exactly as a plain search does.
inline constexpr int kLadderMaxAttempts = 8;
inline constexpr double kLadderLead = 600.0;
inline constexpr double kLadderPast = 300.0;

// The plain search that ends a ladder, run somewhere else and started with the ladder rather
// than after it. Its arguments are the caller's own plus --gridmap -1e9:1 -- nothing the attempts
// find out goes into them -- so it can start before the first attempt, and the ladder takes its
// result exactly where it would have searched it itself (step 4); a ladder that ends without it
// throws it away. What the ladder hands back is the same either way; only the time differs. The
// mod supplies the second search from a second copy of this core (src/mod/dp_bridge2.cpp): the
// solver's state is all in globals, so two searches cannot share one copy. Its printed lines are
// held and come out where this copy's plain search would have printed them.
struct PlainElsewhere {
    // Start the plain search with these arguments (argv[0] included). False = not available for
    // this call, and the ladder searches it itself.
    bool (*start)(const std::vector<std::string>& argv) = nullptr;
    // The ladder has come to it: wait for it, put its output files where this call's own go and
    // its outcome into g_outcome, and return its exit code.
    int (*take)() = nullptr;
    // The ladder ended without it: stop it and throw it away.
    void (*drop)() = nullptr;
    // Set: the instrument that checks the claim above. The ladder searches the plain search
    // itself as it does without any of this, then hands its exit code here to be compared with
    // the one run elsewhere, which is thrown away; take() is not called.
    void (*check)(int rc) = nullptr;
    // Set: THE GAMBLE, which gives up the claim above for time. Asked after every attempt with
    // the deepest tick an attempt has died on so far (bestT; -1 while none has): true when the
    // plain search has finished and either solved or died no earlier than that, and the ladder
    // then takes it at once instead of searching on -- stopping an attempt in progress through
    // g_ladderStop, which the other side raises when the same holds as it finishes. An attempt
    // still to come could have got further or solved; which of the two finishes first depends on
    // the machine, so the answer does too.
    bool (*ready)(long long bestT) = nullptr;
};
inline PlainElsewhere g_plainElsewhere;

inline int cliMain(int argc, char** argv) {
    long long c0 = 0;
    std::vector<std::string> base;
    for (int i = 0; i < argc; ++i) {
        if (i + 1 < argc && !std::strcmp(argv[i], "--capladder")) {
            c0 = std::atoll(argv[++i]);
            continue;
        }
        // the ladder writes its own maps; a caller's would be overwritten by them anyway
        if (i + 1 < argc && !std::strcmp(argv[i], "--capmap")) { ++i; continue; }
        if (i + 1 < argc && !std::strcmp(argv[i], "--gridmap")) { ++i; continue; }
        base.push_back(argv[i]);
    }
    if (c0 <= 0) return cliMainOnce(argc, argv);
    // The attempts share one level (LadderLevelCache), for this ladder only.
    static unsigned long long s_ladders = 0;
    g_ladder = ++s_ladders;
    g_ladderBestT.store(-1);
    g_ladderStop.store(false);
    struct EndLadder {
        ~EndLadder() {
            g_ladder = 0;
            if (!g_inputFiles.job()) g_ladderLevel = LadderLevelCache{};
            g_ladderBestT.store(-1);
            g_ladderStop.store(false);
        }
    } endLadder;
    // --inputgrid under the ladder: a wall the grid makes (a press it cannot place) looks to the
    // ladder exactly like a wall the cap makes, so the plain search that ends the ladder runs
    // without the grid (--gridmap -1e9:1). The windows keep it.
    // TRIED AND REVERTED (2026-09-25): lifting the grid inside every window as well (a --gridmap
    // over the stretches whose cap was raised). On lv3/11/12/16/22 and two custom levels at
    // --capladder 125 --inputgrid 2 it cost 199M stepped children against 147M with the grid kept
    // (lv11 3 -> 5 attempts, the custom level 2 -> 4): the lifted window searched finer and took other
    // routes, and none of the seven had needed it -- all were SOLVED with the grid kept.
    // The full cap is whatever the caller's --cap (or the default) leaves g_aliveCap at, which
    // is known once an attempt has parsed the arguments; the first attempt always runs.
    long long fullCap = -1;
    const std::string noGrid = "-1e9:1";
    // The plain search, started elsewhere now if the caller can (PlainElsewhere); dropped on every
    // way out of the ladder that does not take it.
    struct Elsewhere {
        bool on = false, taken = false;
        ~Elsewhere() {
            if (on && !taken && g_plainElsewhere.drop) g_plainElsewhere.drop();
        }
    } elsewhere;
    if (g_plainElsewhere.start && g_plainElsewhere.take && g_plainElsewhere.drop) {
        std::vector<std::string> a = base;
        a.push_back("--gridmap");
        a.push_back(noGrid);
        elsewhere.on = g_plainElsewhere.start(a);
    }

    // --ladderback's envelope step: the attempts run with --capenvelope while this is set.
    bool envOn = false;
    // ...and its coin-wall trigger (--ladderenv): the coins it has fired for, whether the attempt
    // in flight is its trial, and the death tick that trial has to beat.
    uint32_t envCoinTried = 0;
    bool envCoinPending = false;
    long long envCoinT = -1;
    auto run = [&](const std::string* map, const std::string* grid) {
        std::vector<std::string> a = base;
        if (envOn) a.push_back("--capenvelope");
        if (map) {
            a.push_back("--capmap");
            a.push_back(*map);
        }
        if (grid) {
            a.push_back("--gridmap");
            a.push_back(*grid);
        }
        std::vector<char*> p;
        p.reserve(a.size());
        for (std::string& s : a) p.push_back(s.data());
        return cliMainOnce((int)p.size(), p.data());
    };

    // stepped / lastBack / lastWx / backFailed / capFailed: --ladderback's account of the window's
    // last change (a lead step or a cap step), where the frontier died after it, and which kinds
    // of step have left the death where it was.
    struct Window {
        double wall, lead;
        long long cap;
        bool stepped = false, lastBack = false;
        double lastWx = 0.0;
        long long lastT = -1;   // the tick the frontier died on after the window's last change
        bool backFailed = false, capFailed = false;
    };
    std::vector<Window> wins;
    // The map is a step function (thread_pool.hpp capAtX), so overlapping windows are resolved
    // here: each stretch between two window edges takes the largest cap that covers it.
    auto mapOf = [&]() {
        std::vector<double> edges;
        for (const Window& w : wins) {
            edges.push_back(w.wall - w.lead);
            edges.push_back(w.wall + kLadderPast);
        }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        char buf[64];
        std::snprintf(buf, sizeof(buf), "-1e9:%lld", c0);
        std::string m = buf;
        for (double e : edges) {
            long long c = c0;
            for (const Window& w : wins)
                if (e >= w.wall - w.lead && e < w.wall + kLadderPast) c = std::max(c, w.cap);
            std::snprintf(buf, sizeof(buf), ",%.1f:%lld", e, c);
            m += buf;
        }
        return m;
    };

    auto frontierDied = [] {
        const int v = g_outcome.verdict;
        return (v == VerdictPartial || v == VerdictFailed) && g_outcome.deepX >= 0.0
               && g_outcome.capHits >= 0;
    };
    // The ladder's account (SearchOutcome::ladder), written on every way out -- after the answer
    // is in place, so the chosen hash is the plan the caller will read.
    std::string outPath;
    for (size_t i = 0; i + 1 < base.size(); ++i)
        if (base[i] == "--out") outPath = base[i + 1];
    auto planHash = [&]() -> std::string {
        std::string bytes;
        if (outPath.empty() || !readFileBytes(outPath, bytes)) return "none";
        uint64_t h = 1469598103934665603ULL;
        for (char c : bytes) {
            h ^= (uint8_t)c;
            h *= 1099511628211ULL;
        }
        char b[24];
        std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
        return b;
    };
    int nAttempts = 0, nIdle = 0;
    std::string plainHash = "-";
    const char* via = "attempt";
    struct Account {
        std::function<void()> f;
        ~Account() { f(); }
    } account{[&] {
        char b[160];
        const std::string chosen = planHash();
        std::snprintf(b, sizeof b, "attempts=%d idle=%d plain=%s chosen=%s via=%s", nAttempts,
                      nIdle, plainHash.c_str(), chosen.c_str(), via);
        g_outcome.ladder = b;
        std::printf("capladder: result %s\n", b);
        std::fflush(stdout);
    }};
    // Step 5: the first attempt whose frontier died deepest, and the map it ran with.
    long long bestT = -1;
    int bestAttempt = 0;
    std::string bestMap;
    bool bestEnv = false;   // ...and whether it ran with --ladderback's envelope step
    // ...and what it left, kept as it finished: its plan, its trace and its outcome, which is what
    // searching it again would write. Step 5 puts them back instead of searching it again -- the
    // second search of the same map is the same search. Not under the checkpoint channel, where
    // searching it again also publishes checkpoints for the game to fly, and not for an attempt
    // that wrote no plan (then the files would still hold whatever the plain search left).
    const bool keep = !outPath.empty() && !g_check.enabled.load(std::memory_order_acquire);
    const std::string keptPlan = outPath + ".best";
    const std::string keptTrace = outPath + ".best.trace.csv";
    bool kept = false;
    int keptRc = 0;
    SearchOutcome keptOutcome;
    auto copyFile = [](const std::string& from, const std::string& to) {
        std::string bytes;
        if (!readFileBytes(from, bytes)) return false;
        std::ofstream o(to, std::ios::binary | std::ios::trunc);
        o.write(bytes.data(), (std::streamsize)bytes.size());
        return (bool)o;
    };
    struct DropKept {
        std::string a, b;
        ~DropKept() {
            std::remove(a.c_str());
            std::remove(b.c_str());
        }
    } dropKept{keptPlan, keptTrace};
    // The gamble (PlainElsewhere::ready): the plain search beside the ladder, taken now.
    auto gambleTakes = [&](int attempt) {
        if (!elsewhere.on || !g_plainElsewhere.ready || !g_plainElsewhere.ready(bestT))
            return false;
        std::printf("capladder: attempt=%d took the plain search beside it (the gamble; the "
                    "ladder had died at t=%lld at best)\n", attempt, bestT);
        std::fflush(stdout);
        elsewhere.taken = true;
        return true;
    };
    for (int attempt = 1;; ++attempt) {
        const std::string m = mapOf();
        const int rc = run(&m, nullptr);
        const int v = g_outcome.verdict;
        const double wx = g_outcome.deepX;
        std::printf("capladder: attempt=%d map=%s verdict=%d deepX=%.1f rc=%d\n", attempt,
                    m.c_str(), v, wx, rc);
        std::fflush(stdout);
        ++nAttempts;
        // The gamble's taking of the plain search (below): its plan is the answer.
        auto takeGamble = [&] {
            const int grc = g_plainElsewhere.take();
            plainHash = planHash();
            via = "gamble";
            return grc;
        };
        // Stopped in progress: the other side raised g_ladderStop because the gamble held when
        // its search finished. This attempt's answer is gone either way, so it is taken.
        if (g_ladderStop.load()) {
            std::printf("capladder: attempt=%d stopped; took the plain search beside it (the "
                        "gamble; the ladder had died at t=%lld at best)\n", attempt, bestT);
            std::fflush(stdout);
            elsewhere.taken = true;
            return takeGamble();
        }
        if (g_outcome.capHits == 0) ++nIdle;
        if (fullCap < 0) fullCap = (long long)g_aliveCap;
        const bool died = frontierDied();
        if (!died || c0 >= fullCap) return rc;
        if (g_outcome.deepT > bestT) {
            bestT = g_outcome.deepT;
            bestAttempt = attempt;
            bestMap = m;
            bestEnv = envOn;
            kept = keep && g_outcome.planWritten && copyFile(outPath, keptPlan)
                   && copyFile(outPath + ".trace.csv", keptTrace);
            if (kept) {
                keptRc = rc;
                keptOutcome = g_outcome;
            }
            g_ladderBestT.store(bestT);
        }
        if (gambleTakes(attempt)) return takeGamble();
        Window* hit = nullptr;
        for (Window& w : wins)
            if (wx >= w.wall - w.lead && wx < w.wall + kLadderPast) hit = &w;
        // Step 4: the plain search -- at once if this attempt's cap never bound, since every later
        // attempt would only raise caps it never reached
        bool plain = attempt >= kLadderMaxAttempts || g_outcome.capHits == 0;
        // --ladderenv at a coin's miss prune: the frontier died where the states without that coin
        // are cut, so the route that takes it is a minority the stride lost on the way -- the
        // envelope's case. The next attempt runs with it at once, the windows left as they are,
        // once per coin; a death no later than this one drops it again and the windows step as
        // usual. Official lv1 with coins: its last coin's prune killed every attempt at x 26,359
        // that lead and cap steps made, and the envelope at cap 125 takes it on the first try.
        if (!plain) {
            if (envCoinPending) {
                envCoinPending = false;
                if (g_outcome.deepT <= envCoinT) envOn = false;
            } else if (!envOn && g_outcome.coinWall >= 0 && g_outcome.coinWall < 32
                       && !((envCoinTried >> g_outcome.coinWall) & 1u)) {
                envCoinTried |= 1u << g_outcome.coinWall;
                envOn = true;
                envCoinPending = true;
                envCoinT = g_outcome.deepT;
                continue;
            }
        }
        if (hit) {
            // --ladderback: one kind of step at a time. The first repeat reaches further back
            // (lead x2, cap kept); after that, a step that moved the death is repeated in kind and
            // one that did not is swapped for the other kind (cap x4, lead kept). A kind that is
            // spent (the window runs from x 0, or has the full cap) gives way to the other.
            const bool canBack = hit->wall - hit->lead > 0.0;
            const bool canCap = hit->cap < fullCap;
            // A wall that neither kind of step has moved, or with both kinds spent, goes to the
            // plain search, as the default ladder's wall does: more steps of the same kinds would
            // be the same death again.
            bool back = true;
            if (hit->stepped) {
                // "Moved" is a later death, not a different x: on RC9 lv20 a lead step left the
                // death 2 px BEHIND where it was (x 6,577.4 -> 6,575.2), and read as a move it
                // sent the ladder on through four more lead steps before the plain search.
                const bool moved = g_outcome.deepT > hit->lastT;
                if (moved) {
                    hit->backFailed = hit->capFailed = false;
                } else if (hit->lastBack) {
                    hit->backFailed = true;
                } else {
                    hit->capFailed = true;
                }
                back = moved ? hit->lastBack : !hit->lastBack;
            }
            if (back && !canBack) back = false;
            if (!back && !canCap) back = canBack;
            if ((!canBack && !canCap) || (hit->backFailed && hit->capFailed)) {
                // TRIED AND REMOVED (2026-10-01, --ladderenv): one attempt with --capenvelope
                // here, at every wall neither kind moved. At a wall no search crosses it was one
                // more search for nothing: RC9 lv20 without coins 26 -> 41 s offline. The envelope
                // now runs only at a coin's miss prune (above), which is where it was ever needed.
                // TRIED AND REMOVED (2026-10-01): one window from x 0 at the full cap, input grid
                // kept, before the plain search. It crossed lv16's wall at x 11,371, which moved
                // for nothing until a window from x 1,771 at 2000 -- but at a wall no search
                // crosses it was a second plain-sized search on top of the plain one, and those
                // walls are the common case: SubZero 4002 with coins went 36 -> 49 rounds and
                // 251 -> 853 s of dp, seven calls over 20 s against one, every one of them 5-8
                // attempts. Without it the ladder never costs more at a wall than the default
                // one does, bar one attempt at the window's cap. --ladderx0 keeps it for measuring.
                if (g_ladderX0 && (canBack || canCap)) {
                    hit->lead = hit->wall + 1e9;
                    hit->cap = fullCap;
                } else {
                    plain = true;
                }
            } else if (back) {
                hit->lead *= 2;
            } else {
                hit->cap = std::min(hit->cap * 4, fullCap);
            }
            hit->stepped = true;
            hit->lastBack = back;
            hit->lastWx = wx;
            hit->lastT = g_outcome.deepT;
        } else if (hit && hit->cap >= fullCap) {
            plain = true;
        } else if (hit) {
            hit->cap = std::min(hit->cap * 4, fullCap);
            hit->lead *= 2;
        } else {
            wins.push_back({wx, kLadderLead, std::min(c0 * 4, fullCap), false, false, 0.0, -1, false,
                            false});
        }
        if (plain) {
            envOn = false;   // the plain search is the caller's own arguments
            std::printf("capladder: attempt=%d plain search at --cap %lld%s\n", attempt + 1,
                        fullCap, elsewhere.on ? " (started with the ladder)" : "");
            std::fflush(stdout);
            int prc;
            if (elsewhere.on && !g_plainElsewhere.check) {
                elsewhere.taken = true;
                prc = g_plainElsewhere.take();
            } else {
                prc = run(nullptr, &noGrid);
                if (elsewhere.on) {
                    elsewhere.taken = true;
                    g_plainElsewhere.check(prc);
                }
            }
            plainHash = planHash();
            via = "plain";
            // The other side may have raised it as it finished; step 5's search must not see it.
            g_ladderStop.store(false);
            const long long plainT = g_outcome.deepT;
            if (!frontierDied() || plainT >= bestT) return prc;
            via = "again";
            if (kept) {
                std::remove(outPath.c_str());
                std::remove((outPath + ".trace.csv").c_str());
                if (copyFile(keptPlan, outPath) && copyFile(keptTrace, outPath + ".trace.csv")) {
                    g_outcome = keptOutcome;
                    std::printf("capladder: the plain search died at t=%lld, before attempt %d's"
                                " t=%lld - attempt %d's own result is the ladder's (kept)\n",
                                plainT, bestAttempt, bestT, bestAttempt);
                    std::fflush(stdout);
                    return keptRc;
                }
            }
            std::printf("capladder: the plain search died at t=%lld, before attempt %d's t=%lld"
                        " - searching attempt %d again for the result\n",
                        plainT, bestAttempt, bestT, bestAttempt);
            std::fflush(stdout);
            envOn = bestEnv;
            const int brc = run(&bestMap, nullptr);
            std::printf("capladder: attempt=%d again map=%s verdict=%d deepT=%lld (was %lld)"
                        " deepX=%.1f rc=%d\n", bestAttempt, bestMap.c_str(), g_outcome.verdict,
                        g_outcome.deepT, bestT, g_outcome.deepX, brc);
            std::fflush(stdout);
            return brc;
        }
    }
}


}  // namespace dp
