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
// Measured constants (docs/findings.md 2026-07-30):
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

namespace dp {

inline int cliMain(int argc, char** argv) {
    // Whatever the last call concluded must not be readable as this one's answer. Cleared here
    // rather than at the search, so an early return (bad arguments, unreadable level) also
    // leaves "FAILED, nothing measured" behind instead of the previous run's verdict
    g_outcome.reset();
    // ...and neither must anything else the last call left behind. This is a process's worth of
    // state, and only the mod ever runs two solves in one process -- see dp/reset.hpp for what
    // that was costing and how it was measured.
    resetInvocationState();
    if (argc < 2) {
        std::printf("usage: leveldp <objrects.csv> [--out plan.txt]\n");
        return 2;
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
    int dbgLayers = 0;
    std::string snapLogPath;
    std::vector<std::string> groupsPaths;
    std::string trigPath, grpPath, obbPath;
    // y, vy, mode, held, grounded, flip (everything after that is zeroed)
    State init{(float)kFloorY, 0.f, 0, 0, 1, 0};
    // value-less flags get their own loop: the one below stops at argc-1 (every
    // option there reads argv[i+1]), so a flag passed LAST would never be seen.
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--touch-from-anchor")) g_touchFromAnchor = true;
        if (!std::strcmp(argv[i], "--memstat")) g_memStat = true;
        if (!std::strcmp(argv[i], "--no-miniwave")) g_noMiniWave = true;
        if (!std::strcmp(argv[i], "--old-latency")) g_oldLatency = true;
        if (!std::strcmp(argv[i], "--old-slope")) g_oldSlope = true;
        if (!std::strcmp(argv[i], "--rotport")) g_rotPort = true;
        if (!std::strcmp(argv[i], "--rotlast")) g_rotLast = true;
        // The inverted-cube ground kill, both ways (step.hpp g_invCubeFloorKill). Value-less,
        // and the mod appends cfg `dparg` LAST of all, which is exactly the position the loop
        // below cannot see -- put here, a `dparg=--invcubefloor` finally reaches the search.
        // It did not before, and two A/B rounds were read off a switch that never moved.
        if (!std::strcmp(argv[i], "--invcubefloor")) g_invCubeFloorKill = true;
        if (!std::strcmp(argv[i], "--no-invcubefloor")) g_invCubeFloorKill = false;
        // Value-less too, and the mod's addWorldArgs can emit it LAST (nothing
        // after it when no boxes are dropped, obb.txt is missing and dpArgs is
        // empty -- the cold-restart JobFirstSolve path), where the loop below
        // never reads it. Parsed here as well so argv order cannot drop it;
        // the second parse at its old site is a harmless re-set.
        if (!std::strcmp(argv[i], "--needtrig-unseen")) g_needUnseen = true;
    }
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--threads")) g_threads = std::atoi(argv[i + 1]);
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--dodgemin"))
            g_portalDodgeMin = std::atof(argv[i + 1]);
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--speeddodge"))
            g_speedDodgeMin = std::atof(argv[i + 1]);
    for (int i = 2; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--out")) outPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--banddbg")) g_bandDbg = true;
        if (!std::strcmp(argv[i], "--slopedbg")) g_slopeDbg = true;
        if (!std::strcmp(argv[i], "--dbg")) dbgLayers = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--cap")) g_aliveCap = (size_t)std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--gcnodes")) g_gcNodes = (size_t)std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--memlimit")) g_memLimitMiB = (size_t)std::atoll(argv[i + 1]);
        if (!std::strcmp(argv[i], "--shipyq")) g_shipYq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--shipvq")) g_shipVq = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--snaplog")) snapLogPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--shipceil")) {
            g_shipCeil = std::atof(argv[i + 1]);
            g_shipCeilSet = true;
        }
        if (!std::strcmp(argv[i], "--flyfloor")) g_flyFloor = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--ufoceil")) g_ufoCeil = std::atof(argv[i + 1]);
        // --ceil <y>            everywhere (lv9)
        // --ceil <y>@<x0>:<x1>  only inside that x window (lv14), repeatable
        if (!std::strcmp(argv[i], "--ceil")) {
            CeilBand b{0.0, -1e18, 1e18};
            const char* a = argv[i + 1];
            const char* at = std::strchr(a, '@');
            b.y = std::atof(a);
            if (at) std::sscanf(at + 1, "%lf:%lf", &b.x0, &b.x1);
            if (b.y > 0.0) g_playerCeils.push_back(b);
        }
        // --groups <file>: the MOD's grouptrace dump. Without it every level
        // from lv19 on is planned against geometry that is in the wrong place.
        if (!std::strcmp(argv[i], "--groups")) groupsPaths.push_back(argv[i + 1]);
        // --triggers / --objgroups: the MOD's trigger -> group -> uid map. Both
        // or neither; without them a door that only opens on contact reads as a
        // solid wall (see TouchTrig).
        if (!std::strcmp(argv[i], "--norevtoggle")) g_revToggle = false;
        // --supporttol X: gap tolerance for staying supported (default 0.6,
        // see kSupportTol's note)
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
        // --nohangladder: turn off the hanging ride's ship ladder (r88)
        // (diagnostic A/B)
        if (!std::strcmp(argv[i], "--nohangladder")) g_noHangLadder = true;
        if (!std::strcmp(argv[i], "--nocrush")) g_noCrush = true;
        // --no-dyninterp: turn off interpolation between recording rows (the
        // r104 A/B)
        if (!std::strcmp(argv[i], "--no-dyninterp")) g_dynInterp = false;
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
        if (!std::strcmp(argv[i], "--ceilpin")) g_ceilPin = true;
        // --trigraw: autonomous triggers behind the anchor trust the
        // recording's tick (history at g_trigRaw's declaration). The argument
        // loop runs to i+1<argc, so PASSED LAST IT IS NEVER READ.
        if (!std::strcmp(argv[i], "--trigraw")) g_trigRaw = true;
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
        if (!std::strcmp(argv[i], "--dyndbg")) g_dynDbg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--triggers")) trigPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--objgroups")) grpPath = argv[i + 1];
        // --obb <file>: GD's own corners for the turned objects (see loadObb).
        // Optional -- without it a turned hazard keeps the bound it has always
        // had, so a level with no obb dump cannot move.
        if (!std::strcmp(argv[i], "--obb")) obbPath = argv[i + 1];
        // --bandtrack <file>: the recorded band (history at g_bandTrack's
        // declaration)
        if (!std::strcmp(argv[i], "--bandtrack")) {
            std::FILE* bf = std::fopen(argv[i + 1], "r");
            if (bf) {
                BandTrackRow r{};
                while (std::fscanf(bf, "%d,%f,%f", &r.t, &r.fl, &r.ce) == 3)
                    if (r.ce > r.fl) g_bandTrack.push_back(r);
                std::fclose(bf);
                g_bandTrackCam = -1;   // the verdict belongs to THIS track
                std::printf("bandtrack: %zu rows (%s)\n", g_bandTrack.size(),
                            argv[i + 1]);
            } else {
                std::fprintf(stderr, "bandtrack: cannot open %s\n", argv[i + 1]);
            }
        }
        if (!std::strcmp(argv[i], "--startband")) {
            double f = 0, c = 0;
            if (std::sscanf(argv[i + 1], "%lf,%lf", &f, &c) == 2 && c > f) {
                g_startBandSet = true;
                g_startBandFloor = f;
                g_startBandCeil = c;
            } else {
                std::fprintf(stderr, "startband: `%s` is not a readable f,c pair\n",
                             argv[i + 1]);
            }
        }
        // --needtrig <n>: repeatable. See g_needTrig.
        if (!std::strcmp(argv[i], "--needtrig"))
            g_needTrig |= (uint32_t)1 << std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--needtrig-unseen")) g_needUnseen = true;
        // --needtrig-skip <n>: repeatable. See g_needSkip.
        if (!std::strcmp(argv[i], "--needtrig-skip"))
            g_needSkip |= (uint32_t)1 << std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--oriented")) g_oriented = true;
        if (!std::strcmp(argv[i], "--no-oriented")) g_oriented = false;
        if (!std::strcmp(argv[i], "--obb-all")) g_obbAll = true;
        if (!std::strcmp(argv[i], "--invcubefloor")) g_invCubeFloorKill = true;
        if (!std::strcmp(argv[i], "--no-invcubefloor")) g_invCubeFloorKill = false;
        if (!std::strcmp(argv[i], "--trigclosed")) g_trigClosed = true;
        if (!std::strcmp(argv[i], "--no-trigclosed")) g_trigClosed = false;
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
        if (!std::strcmp(argv[i], "--maxplayy")) g_maxPlayY = std::atof(argv[i + 1]);
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
            // 25 fields (21st=frame, 22nd=rev, 23rd/24th=sprite rotation,
            // 25th=boost). FORGET TO GROW THE SIZE AND
            // sscanf WRITES PAST THE ARRAY: when rev was added it was left at
            // 21, and it showed up as rev=0/1 not changing the result by a
            // single bit.
            double a[25] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0};
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
            // has holes -- dashing / dashSlope / ringHold / usedPad / usedOrb
            // are all still dropped by a re-anchor. lv21 holds one dash for 300+
            // ticks, so that one will bite next.
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
            std::sscanf(argv[i + 1],
                        "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,"
                        "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                        &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7],
                        &a[8], &a[9], &a[10], &a[11], &a[12], &a[13], &a[14],
                        &a[15], &a[16], &a[17], &a[18], &a[19], &a[20], &a[21],
                        &a[22], &a[23], &a[24]);
            const uint8_t startRev = (uint8_t)((int)a[21] & 1);
            startFrame = (int)a[20] & 3;
            startSnapUid = (long long)a[18];
            startSnapDist = a[19];
            g_startSpeedMul = a[14];
            t0 = (long long)a[0];
            x0 = a[1];
            init = State{(float)a[2], (float)a[3], (uint8_t)a[4], (uint8_t)a[6],
                         (uint8_t)a[5], (uint8_t)a[7]};
            init.mini = (uint8_t)a[8];
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
            init.dual = (uint8_t)a[9];
            if (init.dual) {
                init.y2 = (float)a[10];
                init.vy2 = (float)a[11];
                init.flip2 = (uint8_t)a[12];
                init.grounded2 = (uint8_t)a[13];
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

    GroupTimeline gt;
    for (size_t gi = 0; gi < groupsPaths.size(); ++gi) {
        GroupTimeline one = loadGroupTimeline(groupsPaths[gi]);
        if (gi == 0) gt = std::move(one);
        else overlayGroupTimeline(gt, one);
    }
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
        bool winTouch = g_touchFromAnchor;
        if (!winTouch && x0 > -1e17) {
            const std::vector<TouchTrig> plain =
                loadTouchTriggers(trigPath, grpPath, -1e18);
            if (plain.size() >= 32 && !plain.empty()
                && plain.back().cx < x0 - 200.0) {
                winTouch = true;
                std::printf("triggers: the plain 32 all lie behind the anchor "
                            "(last %.0f < %.0f) - windowing from the anchor\n",
                            plain.back().cx, x0);
            }
        }
        g_touch = loadTouchTriggers(trigPath, grpPath, winTouch ? x0 : -1e18);
        g_autoTrig = loadAutoTriggers(trigPath, grpPath);
    }
    if (!obbPath.empty()) g_obb = loadObb(obbPath);   // before loadLevel reads it
    Level L = loadLevel(argv[1], groupsPaths.empty() ? nullptr : &gt,
                        g_touch.empty() ? nullptr : &g_touch,
                        g_autoTrig.empty() ? nullptr : &g_autoTrig);
    g_baseLv = &L;   // see levelOfFrame / applyRotation's re-tap
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
    // of hole as rHover/dash/snapObj). A pad is consumed unconditionally on
    // contact (note in stepOne's pad loop) and does not re-fire while the
    // contact lasts. If the player's box at t0 overlaps a pad's firing box,
    // that contact began before t0 and GD has already fired it -- start with
    // it consumed.
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
            if (o.oriented
                && !orientedHit(o, x0, (double)init.y, sHalf, (double)init.rot))
                continue;
            init.usedPad[slot++] = &o;
        }
    }
    // A re-anchored solve starts mid-level, where the doors the earlier part of
    // the run already opened ARE open. That is not an assumption to make -- it
    // is in the recording, so read it from there: a controlled object that has
    // already travelled most of its offset by t0 means its trigger fired.
    // Without this the anchor is dropped inside a door GD has open and the
    // frontier dies on tick 0 (measured on lv19 --start 19900, x=27,721, which
    // sits exactly where gate uid 13395 was before it slid away).
    if (t0 > 0 && !g_touch.empty()) {
        // Counted, because "nothing happened" is the failure mode this block has
        // (an anchor that silently keeps every door shut looks exactly like a
        // physics wall, and did for a whole session on lv22).
        int nCtl = 0, nNoRec = 0, nNoOff = 0;
        for (size_t b = 0; b < g_touch.size(); ++b) {
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
                if (full > 0.5 && moved > full * 0.5) {
                    init.trig |= (uint32_t)1 << b;
                    // far enough back that the move counts as finished
                    init.trigT = (int32_t)(t0 - (long long)std::llround(c.durTicks));
                    break;
                }
                // Started, but not half way: the anchor landed INSIDE the slide.
                // Neither "open" nor "shut" is true, so say open and let the
                // recording carry the phase (see g_recPhase).
                if (full > 0.5 && moved > 0.5) {
                    init.trig |= (uint32_t)1 << b;
                    g_recPhase |= (uint32_t)1 << b;
                    break;
                }
            }
        }
        std::printf("triggers: anchor scan t0=%lld: %d controlled objects, "
                    "%d with no recording, %d with no offset\n",
                    t0, nCtl, nNoRec, nNoOff);
        if (init.trig)
            std::printf("triggers: anchor starts with mask 0x%x (already open in "
                        "the recording)\n", init.trig);
        if (g_recPhase)
            std::printf("triggers: mask 0x%x was still MOVING at the anchor -- "
                        "replayed from the recording's own phase\n", g_recPhase);
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
        int behind = 0, fromX = 0;
        for (size_t b = 0; b < g_autoTrig.size(); ++b) {
            AutoTrig& A = g_autoTrig[b];
            // recFire is the first ROW, which is 1-2 ticks after the move really
            // started (see recordLag). Everything here works in true ticks.
            int rf = -1;
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                if (L.dyn.autoAnchor[i] != (int)b || L.dyn.trigRecFire[i] < 0)
                    continue;
                const int tr = L.dyn.trigRecFire[i] - L.dyn.recLag[i];
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
                if (g_trigRaw && rf >= 0) {
                    A.fireT = rf;
                    A.fireX = A.lockLastX = A.cx;
                    if (g_dynDbg >= 0)
                        std::printf("anchortrig(ahead) uid=%d cx=%.1f rf=%d "
                                    "-> fireT=%d\n", A.uid, A.cx, rf, A.fireT);
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
            const long long est = t0 - (long long)std::floor(
                (x0 - A.cx) / (dxA > 0.0 ? dxA : (double)kDxF)) + A.delay;
            const int estT = (int)std::max(0LL, est);
            // 60 ticks of slack: within that the recording IS this run's own
            // timeline (the live overlay comes from this plan's replays) and its
            // tick is exact, so keep the old no-op behaviour.
            // --trigraw removes this gate and always trusts the recording
            // (history at g_trigRaw's declaration: est assumes x advances
            // monotonically and lies behind a rotation maze).
            if (rf >= 0 && (g_trigRaw
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
            if (g_dynDbg >= 0)
                std::printf("anchortrig uid=%d cx=%.1f rf=%d estT=%d -> fireT=%d "
                            "(x0=%.2f dxA=%.5f t0=%lld)\n",
                            A.uid, A.cx, rf, estT, A.fireT, x0, dxA, (long long)t0);
            ++behind;
        }
        if (behind)
            std::printf("autotrig: %d behind the anchor (%d re-dated from x, "
                        "the recording disagreed by >60 ticks)\n", behind, fromX);
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
            std::printf("\n  closed=%d  trigMask=0x%x recFire=%d (lag %d -> "
                        "true %d)  autoAnchor=%d autoD=(%.1f,%.1f) autoDur=%.2f "
                        "ease=%d/%.2f\n",
                        (int)L.dyn.autoClosed[i],
                        L.dyn.trigMask[i], L.dyn.trigRecFire[i], L.dyn.recLag[i],
                        L.dyn.trigRecFire[i] >= 0
                            ? L.dyn.trigRecFire[i] - L.dyn.recLag[i] : -1,
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
                if ((L.dyn.trigMask[i] & ((uint32_t)1 << b)) && L.dyn.trigRecFire[i] >= 0)
                    seenIt = true;
            // ...and one the anchor already entered is settled, not unknown.
            if (!seenIt && !(init.trig & ((uint32_t)1 << b))) {
                // Unreachable from this anchor: the tail starts past the box.
                // Reported either way so the driver can tell a blocked anchor
                // from a physics wall; only --needtrig-skip drops it.
                const bool passed = g_touch[b].cx + g_touch[b].hw + 40.0 < x0;
                const bool skip = (g_needSkip & ((uint32_t)1 << b)) != 0;
                std::printf("needtrig: box=%zu cx=%.0f cy=%.0f hw=%.0f "
                            "passed=%d skipped=%d\n",
                            b, g_touch[b].cx, g_touch[b].cy, g_touch[b].hw,
                            passed ? 1 : 0, skip ? 1 : 0);
                if (skip) continue;
                g_needTrig |= (uint32_t)1 << b;
                // Same facts, for a caller with no stdout to read (dp/progress.hpp)
                g_outcome.needTrigMask |= (unsigned)1 << b;
                if (passed) g_outcome.needTrigPassed |= (unsigned)1 << b;
            }
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
                    g_trigClosed ? ", --trigclosed ON" : "");
    }
    // A re-anchor taken while the player was RIDING A RAMP used to restart with
    // onSlope = 0. The ride is sticky (see stickToSlope), so without it the
    // model free-falls off every downhill ramp it re-anchors on -- measured on
    // lv16: anchored at t=6300 on a downhill the frontier fell away and died 40
    // ticks later at x=9,372, while GD rode the ramp for hundreds more.
    // The dump has no such column, so infer it from the geometry exactly the
    // way the ride does: grounded, and sitting on a ramp's resting height.
    if (init.grounded) {
        const double pH = init.mini ? kMiniHalf : kCubeHalf;
        for (const Obj& sp : L.slopes) {
            const double sx0 = sp.cx - sp.hw, sx1 = sp.cx + sp.hw;
            const double m = (sp.sy1 - sp.sy0) / (sx1 - sx0);
            if (m == 0.0) continue;
            const double xr0 =
                x0 + (m > 0 ? slopeXOffset(m, pH) : -slopeXOffset(m, pH));
            if (xr0 < sx0 || xr0 > sx1 + 1.5) continue;
            const double top =
                sp.sy0 + m * (std::min(xr0, sx1) - sx0) + (init.flip ? -pH : pH);
            if (std::fabs((double)init.y - top) > 0.6) continue;
            init.onSlope = 1;
            init.slopeM = (float)m;
            // KNOWN GAP (same class as the missing snapObj above): the dump
            // carries no slope-ride age, so an anchor taken mid-ride guesses
            // SATURATED (24 = full exit impulse). Right whenever the real ride
            // is >= 0.1 s -- the long chained climbs where anchors actually
            // land -- and over-launches on a shorter one; the loop's own GD
            // replay catches that case.
            init.slopeT = 24;
            break;
        }
    }
    const double goalX = L.maxX + 60.0;
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
    // Open addressing, linear probing, cleared in O(used) via `touched`.
    // std::unordered_map was the whole serial phase once stepping went parallel
    // (a node allocation and a pointer chase per child, ~80k children a layer).
    // Semantics are identical -- key -> Slots, lookup and insert only, never
    // iterated -- so `nxt` still fills in the same order and the plan is
    // bit-identical. Key 0 doubles as "empty": keyOf is a scrambled XOR, so a
    // real key of exactly 0 is a 2^-64 event, and if it ever happened it would
    // merge two cells, which is what a hash collision already does.
    struct CellMap {
        std::vector<uint64_t> keys;
        std::vector<Slots> vals;
        std::vector<uint32_t> touched;
        size_t mask = 0;
        void ensure(size_t want) {
            size_t n = 1024;
            while (n < want * 4) n <<= 1;   // keep the load factor under 1/4
            if (n <= keys.size()) return;
            keys.assign(n, 0);
            vals.assign(n, Slots{});
            mask = n - 1;
        }
        void clear() {
            for (uint32_t i : touched) keys[i] = 0;
            touched.clear();
        }
        // same names the memory report used when this was an unordered_map
        size_t size() const { return touched.size(); }
        size_t bucket_count() const { return keys.size(); }
        Slots& at(uint64_t k) {
            size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull) & mask;
            for (;;) {
                if (keys[i] == k) return vals[i];
                if (keys[i] == 0) {
                    keys[i] = k;
                    vals[i] = Slots{};
                    touched.push_back((uint32_t)i);
                    return vals[i];
                }
                i = (i + 1) & mask;
            }
        }
    };
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
        uint64_t key;
        uint32_t aOrd;          // ordinal that allocated the key's first slot
        uint32_t bOrd;          // ordinal of the first hi/lo split (kNone: none)
        uint32_t hiIdx, loIdx;  // child index of the current hi / lo rep
        float hiVy, loVy;
        uint8_t bWasHi;
    };
    struct ShardMap {           // CellMap's open addressing, key -> rec index
        std::vector<uint64_t> keys;
        std::vector<uint32_t> vals;
        std::vector<uint32_t> touched;
        std::vector<KeyRec> recs;
        uint32_t goalOrd = kNone;
        size_t mask = 0;
        void ensure(size_t want) {
            size_t n = 1024;
            while (n < want * 4) n <<= 1;
            if (n <= keys.size()) return;
            keys.assign(n, 0);
            vals.assign(n, kNone);
            mask = n - 1;
        }
        void clear() {
            for (uint32_t i : touched) keys[i] = 0;
            touched.clear();
            recs.clear();
            goalOrd = kNone;
        }
        // ensure() sizes for the EXPECTED keys per shard; a lopsided hash
        // could still overfill one shard's table, and a full open-addressing
        // table is an infinite probe loop, not a slowdown. Grow keeps the
        // guarantee unconditional.
        void grow() {
            const size_t n = keys.size() ? keys.size() * 2 : 1024;
            std::vector<uint64_t> ok;
            ok.swap(keys);
            std::vector<uint32_t> ov;
            ov.swap(vals);
            keys.assign(n, 0);
            vals.assign(n, (uint32_t)kNone);
            mask = n - 1;
            touched.clear();
            for (size_t i = 0; i < ok.size(); ++i) {
                if (!ok[i]) continue;
                size_t j = (size_t)(ok[i] * 0x9E3779B97F4A7C15ull) & mask;
                while (keys[j]) j = (j + 1) & mask;
                keys[j] = ok[i];
                vals[j] = ov[i];
                touched.push_back((uint32_t)j);
            }
        }
        uint32_t& at(uint64_t k) {
            if ((touched.size() + 1) * 4 > keys.size()) grow();
            size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull) & mask;
            for (;;) {
                if (keys[i] == k) return vals[i];
                if (keys[i] == 0) {
                    keys[i] = k;
                    vals[i] = kNone;
                    touched.push_back((uint32_t)i);
                    return vals[i];
                }
                i = (i + 1) & mask;
            }
        }
    };
    std::vector<ShardMap> shards;
    std::vector<uint64_t> kidKeys;  // compact copy of kids[i].key (8 B strides
                                    // instead of pulling the whole Child in)
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
        uint64_t key = 0;         // keyOf(s), computed on the worker thread
        uint8_t dead = 0;
        uint8_t valid = 0;
        const char* why = "";     // --dbg only
        const Obj* obj = nullptr; // --dbg only
    };
    std::vector<Child> kids;      // reused every layer
    std::vector<size_t> gidx;     // this speed group's indices into `cur`
    // Below this many children the pool's wakeup costs more than the work.
    // A thin frontier is cheap anyway; the layers that matter run at the cap.
    const size_t kParallelMin = 256;
    std::unique_ptr<ThreadPool> pool;
    if (g_threads > 1) pool.reset(new ThreadPool(g_threads - 1));  // + this one
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
        if (g_startBandSet) {
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
        if (g_startBandSet)
            std::printf("startband: the seed will start in [%.1f, %.1f]\n",
                        seed.floorY, seed.ceilY);
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
        // the ship's acceleration switch threshold accelSwitchVy is a
        // per-speed measured value, and 1.3 and 1.6 are still UNMEASURED,
        // using 1.9165 (ship_params.hpp). Without knowing which speed band a
        // divergence shows up in, "the threshold is off" cannot be told apart
        // from some other cause.
        // `flip` and `frame` are here because the rotated sections are exactly
        // where `flip` stops meaning what GD's `upsideDown` means, and without
        // them a sign disagreement reads as a physics bug (lv22 t=5,110: the
        // model's vy is the negative of GD's for the whole frame-3 section, so
        // a type-4 portal looks like a no-op and never fires).
        tr << "tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act"
              ",onslope,slopem,slopet,bandf,bandc,mini,held,dx,nearorb,clamp"
              ",clampuid,clampcx,clampcy,flip,frame,rot,rotneg\n";
        // Make --snaplog usable in replay too (it used to exist only on the
        // SOLVE side, so a known plan's stair snaps could never be checked
        // against GD's snaptrace).
        std::ofstream sn;
        if (!snapLogPath.empty()) {
            sn.open(snapLogPath);
            g_snapOut = &sn;
        }
        State s = init;
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
            Lf->dyn.applyTriggers(s.trig, (int)s.trigT, (int)t, x);
            std::vector<std::pair<const TouchTrig*, uint32_t>> rt;
            const std::vector<TouchTrig>& tfr = touchFor((int)s.frame);
            for (size_t b = 0; b < tfr.size(); ++b) {
                if (s.trig & ((uint32_t)1 << b)) continue;
                const TouchTrig& T = tfr[b];
                if (T.cx + T.hw < x - 40 || T.cx - T.hw > x + 40) continue;
                rt.push_back({&T, (uint32_t)1 << b});
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
            const StepCtx K{x, xPrevR, rDxUsed, t, &rn, &rp, &rd, &ro, &rs, &rv,
                            &SP, &SPmini, &UP, &UPmini, &rt};
            bool rdead = false;
            g_nearOrb = 0;
            g_dashVySet = 0;
            g_clampWhy = "";
            g_clampUid = -1;
            g_deadWhy = "";
            g_deadObj = nullptr;
            const bool sPrevGrounded = (s.grounded != 0);   // applyRotation's re-tap
            const double sPrevY = (double)s.y;              // ...and its pre-tap y
            State c = stepBoth(s, (uint8_t)curIn, K, rdead);
            c.action = (uint8_t)curIn;
            s = c;
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
            const double vyGd = (g_dashVySet ? g_dashVy : (double)s.vy)
                                * (s.frame == 3 ? -1.0 : 1.0);
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
               << ',' << (int)s.flip << ',' << (int)s.frame
               // The player's sprite rotation. THE HITBOX TEST OF A TURNED
               // OBJECT IS DECIDED BY THIS, yet until now it was not in the
               // trace, so there was no way to measure "how far is the
               // model's rot from GD's" (measured on lv20: 1,158 degrees off
               // over a 281-tick anchor). Columns are appended at the end
               // (existing readers index by position).
               << ',' << s.rot << ',' << (int)s.rotNeg
               << "\n";
            if (rdead) {
                diedT = t;
                diedX = (double)s.xAbs;
                break;
            }
            if ((double)s.xAbs >= goalX) break;
            while (eIdx < edges.size() && edges[eIdx].press == t) {
                fx.push_back({t + latOf(s.mode), edges[eIdx].v});
                ++eIdx;
            }
        }
        g_outcome.replayDiedT = diedT;
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
        std::printf("trace -> %s.trace.csv\n", outPath.c_str());
        g_snapOut = nullptr;
        return 0;
    }
    cur.push_back(init);   // root state, AFTER every init amendment (see above)
    // Announce the search to anything watching from another thread (dp/progress.hpp). The end
    // is announced from the guard below, so that every way out of the loop clears it
    g_progress.begin(t0, horizon > 0 ? t0 + horizon : tEnd);
    struct ProgressGuard { ~ProgressGuard() { g_progress.end(); } } progressGuard;
    for (long long t = t0 + 1; t <= tEnd && !solved; ++t) {
        nxt.clear();

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
        // displaced geometry (docs/findings.md).
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
        struct GKey {
            float dx; uint32_t trig; uint8_t frame; uint8_t rev;
            bool operator==(const GKey& o) const {
                return dx == o.dx && trig == o.trig && frame == o.frame
                       && rev == o.rev;
            }
        };
        std::vector<GKey> groupDx;
        for (const State& s : cur) {
            const GKey k{(s.dx > 0.f) ? s.dx : curDxF, s.trig, s.frame, s.rev};
            if (std::find(groupDx.begin(), groupDx.end(), k) == groupDx.end())
                groupDx.push_back(k);
        }

        auto emit = [&](State s, const State& from, uint8_t action, uint64_t k) {
            auto finish = [&](State& stored) {
                arena.push_back(Node{from.parent | ((uint32_t)action << 31)});
                s.parent = (uint32_t)(arena.size() - 1);
                s.action = action;
                stored = s;
                // the goal is reached by a STATE, at its own x -- not by the
                // layer's nominal timeline. Only in frame 0: `goalX` is a world
                // x, and in a turned frame xAbs is a different axis entirely
                // (lv22 ends un-rotated, so nothing is lost).
                if (!solved && s.frame == 0 && (double)s.xAbs >= goalX) {
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
        for (const auto& gkey : groupDx) {
        const float gdx = gkey.dx;
        const uint32_t gtrig = gkey.trig;
        const int gframe = (int)gkey.frame;
        const int grev = (int)gkey.rev;
        // this group's own x span, and the windows that cover it for the whole
        // tick (from where its slowest member starts to where it ends up)
        double gLo = 1e18, gHi = -1e18;
        int gFire = -1;   // latest tick any of them fired a touch trigger
        for (const State& s : cur) {
            if (((s.dx > 0.f) ? s.dx : curDxF) != gdx || s.trig != gtrig
                || (int)s.frame != gframe || (int)s.rev != grev) continue;
            gLo = std::min(gLo, (double)s.xAbs);
            gHi = std::max(gHi, (double)s.xAbs);
            gFire = std::max(gFire, (int)s.trigT);
        }
        // Drop a whole group once it is past a box it was required to enter.
        // Killing it here rather than at the end keeps the frontier spent on
        // states that can still satisfy the requirement.
        if (g_needTrig) {
            bool missed = false;
            for (size_t b = 0; b < g_touch.size() && !missed; ++b)
                if ((g_needTrig & ~gtrig) & ((uint32_t)1 << b))
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
        // ...and then the doors this group has (or has not) opened. Must come
        // after seek and before any window is built. `gFire` is the LATEST
        // firing tick in the group, so a state that touched the box early is
        // shown a door that is still opening -- conservative, never the other
        // way round.
        LG.dyn.applyTriggers(gtrig, gFire, (int)t, x);
        std::vector<const Obj*> near;
        FS.near->forRange(wLo - 40, wHi + 40, [&](const Obj& o) { near.push_back(&o); });
        LG.dyn.collect(Dynamics::NEAR, wLo - 40, wHi + 40, near);
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
        std::vector<std::pair<const TouchTrig*, uint32_t>> trigs;
        // ...in THIS group's frame (see touchFor): a world-coordinate box is
        // unreachable once the frame turns, which silently shut every door in
        // lv22's rotated sections.
        const std::vector<TouchTrig>& tfg = touchFor(gframe);
        for (size_t b = 0; b < tfg.size(); ++b) {
            const TouchTrig& T = tfg[b];
            if (gtrig & ((uint32_t)1 << b)) continue;   // already fired
            if (T.cx + T.hw < wLo - 40 || T.cx - T.hw > wHi + 40) continue;
            trigs.push_back({&T, (uint32_t)1 << b});
        }
        seen.ensure(cur.size() * 2 + 64);
        seen.clear();   // per GROUP, not per layer (see the note above)
        (void)x; (void)xPrev;
        const StepCtx K{x, xPrev, dxUsed, t, &near, &ports, &pads, &orbs, &slps, &spds, &SP, &SPmini, &UP, &UPmini, &trigs};
        // ---- phase 1: STEP every state of this group (parallel) -------------
        // Stepping is pure -- it reads the shared windows and writes only its
        // own child -- so it parallelises exactly. The dedupe, the arena and
        // `nxt` stay on the main thread in phase 2, walked in the SAME order as
        // the serial loop was, which keeps the emitted plan bit-identical.
        gidx.clear();
        for (size_t i = 0; i < cur.size(); ++i)
            if (((cur[i].dx > 0.f) ? cur[i].dx : curDxF) == gdx
                && cur[i].trig == gtrig) gidx.push_back(i);
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
        kidKeys.assign(gidx.size() * 2, 0);
        kidFlag.assign(gidx.size() * 2, 0);
        const bool dbgHere = (dbgLayers > 0 && t - t0 <= dbgLayers);
        const bool orbsEmpty = orbs.empty();
        auto stepKid = [&](size_t i) {
            const State& s = cur[gidx[i >> 1]];
            const int input = (int)(i & 1);
            if (input == 1 && s.mode == 0 && !s.grounded && orbsEmpty
                && !s.dashing)
                return;
            Child& kid = kids[i];
            bool dead = false;
            kid.s = stepBoth(s, input, K, dead);
            // GAMEPLAY ROTATION: turn the child if this tick crossed one. Must
            // happen BEFORE keyOf below -- the turn rewrites x, y, vy and the
            // frame, and a key taken in the old frame would put two different
            // worlds in one cell.
            if (!dead && !g_rotTrig.empty())
                applyRotation(kid.s, (double)s.xAbs, (double)K.dxF, K.t,
                              input, s.grounded != 0, (double)s.y, true);
            // The child's own action, set HERE and not only in emit()'s
            // finish(): keyOf reads it (for the robot's hover) and the key is
            // computed on this thread, before phase 2 runs. Without this the
            // key saw the PARENT's action and the held and released children
            // hashed to the same cell -- which is the whole bug the rHover
            // note in keyOf describes.
            kid.s.action = (uint8_t)input;
            kid.dead = dead ? 1 : 0;
            kid.valid = 1;
            // the dedupe key is a pure function of the child, so it belongs on
            // this thread rather than in the serial phase
            if (!dead) kid.key = keyOf(kid.s);
            // compact side arrays for phase 2p: the shard scans read 9 B per
            // child instead of dragging the whole Child through the cache
            kidKeys[i] = kid.key;
            kidFlag[i] = dead ? 1 : 2;
            if (dbgHere) { kid.why = g_deadWhy; kid.obj = g_deadObj; }
        };
        // --dbg stays serial: it prints per child, in order, and the reason
        // globals are per thread.
        if (pool && !dbgHere && kids.size() >= kParallelMin)
            pool->parallelFor(kids.size(), stepKid);
        else
            for (size_t i = 0; i < kids.size(); ++i) stepKid(i);

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
                    const uint64_t k = kidKeys[i];
                    if ((size_t)((k * 0x9E3779B97F4A7C15ull) >> 32) % nsh != si)
                        continue;
                    const float vy = kids[i].s.vy;
                    uint32_t& ri = sm.at(k);
                    bool accepted = false;
                    if (ri == kNone) {
                        ri = (uint32_t)sm.recs.size();
                        sm.recs.push_back(KeyRec{k, i, kNone, i, i, vy, vy, 0});
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
                    if (accepted && !solved && sm.goalOrd == kNone
                        && (double)kids[i].s.xAbs >= goalX)
                        sm.goalOrd = i;
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
                nxt.push_back(kids[src].s);
                nxt.back().parent = (uint32_t)(arena.size() - 1);
                nxt.back().action = action;
            }
            if (!solved) {
                uint32_t g = kNone;
                for (size_t si = 0; si < nsh; ++si)
                    g = std::min(g, shards[si].goalOrd);
                if (g != kNone) {
                    const State& from = cur[gidx[g >> 1]];
                    const uint8_t action = (uint8_t)(g & 1);
                    arena.push_back(
                        Node{from.parent | ((uint32_t)action << 31)});
                    goalState = kids[g].s;
                    goalState.parent = (uint32_t)(arena.size() - 1);
                    goalState.action = action;
                    solved = true;
                }
            }
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
                    std::printf("dbg t=%lld in=%d: (%.3f,%.3f)->(%.3f,%.3f) "
                                "x=%.2f mode=%d flip=%d g=%d dead=%d%s\n",
                                t, input, (double)s.y, (double)s.vy, (double)c.y,
                                (double)c.vy, (double)c.xAbs, (int)c.mode,
                                (int)c.flip, (int)c.grounded, (int)dead, why);
                }
                if (!dead) emit(c, s, (uint8_t)input, kid.key);
            }
        }
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
            uint32_t m = 0;
            for (const State& s : nxt) m |= s.trig;
            if (m & ~g_trigReported) {
                for (size_t b = 0; b < g_touch.size(); ++b)
                    if ((m & ~g_trigReported) & ((uint32_t)1 << b))
                        std::printf("triggers: box (%.0f,%.0f) entered at "
                                    "t=%lld\n", g_touch[b].cx, g_touch[b].cy, t);
                g_trigReported |= m;
            }
        }
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
        maxAlive = std::max(maxAlive, nxt.size());
        if (nxt.size() > g_aliveCap) {
            ++capHits;
            capDropped += (long long)(nxt.size() - g_aliveCap);
            if (!g_bandPath.empty() && !g_bands.empty() && g_bands.back().t == t)
                g_bands.back().capdrop = (int)(nxt.size() - g_aliveCap);
            auto classOf = [](const State& s) {
                return ((uint64_t)s.mode << 32) ^ ((uint64_t)s.mini << 24)
                       ^ ((uint64_t)s.flip << 16) ^ ((uint64_t)s.dual << 8)
                       ^ (uint64_t)(uint32_t)std::lround(s.dx * 1000.0);
            };
            std::vector<uint64_t> cls;
            std::vector<std::vector<uint32_t>> byCls;
            for (uint32_t i = 0; i < (uint32_t)nxt.size(); ++i) {
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
            keepIdx.reserve(g_aliveCap);
            size_t left = g_aliveCap;
            for (size_t n = 0; n < ord.size(); ++n) {
                const std::vector<uint32_t>& v = byCls[ord[n]];
                const size_t fair = left / (ord.size() - n);
                const size_t take = std::min(v.size(), fair);
                if (take == v.size()) {
                    keepIdx.insert(keepIdx.end(), v.begin(), v.end());
                } else if (take > 0) {
                    const double stride = (double)v.size() / (double)take;
                    for (double i = 0; (size_t)i < v.size() && take > 0; i += stride)
                        keepIdx.push_back(v[(size_t)i]);
                }
                left -= std::min(left, take);
            }
            std::sort(keepIdx.begin(), keepIdx.end());
            std::vector<State> kept;
            kept.reserve(keepIdx.size());
            for (uint32_t i : keepIdx) kept.push_back(nxt[i]);
            nxt.swap(kept);
        }
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
            goalState = cur.front();
            planCut = t0 + horizon;
            break;
        }
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
            for (size_t i = 0; i < arena.size(); ++i) {
                if (!mark[i]) continue;
                remap[i] = (uint32_t)na.size();
                // a parent always precedes its children in the arena, so
                // remap[parent()] is already final here
                na.push_back(Node{remap[arena[i].parent()]
                                  | ((uint32_t)arena[i].action() << 31)});
            }
            const size_t before = arena.size();
            arena.swap(na);
            na = std::vector<Node>();   // release the old allocation NOW
            for (State& s : cur) s.parent = remap[s.parent];
            for (State& s : nxt) s.parent = remap[s.parent];
            gcNext = std::max(g_gcNodes, arena.size() * 2);
            std::printf("gc t=%lld arena %zu -> %zu nodes (%.1f%% live)\n", t,
                        before, arena.size(),
                        100.0 * (double)arena.size() / (double)before);
            std::fflush(stdout);
        }
        // ---- memory budget ----
        // Estimate the search's own structures and stop GROWING before the OS
        // stops us (the lv16 run died as exit 255 with zero diagnostics). The
        // emitted plan keeps the horizon guarantee: a prefix that is known to
        // still be alive (t-t0)/2 ticks past its end, i.e. the same contract
        // as the horizon cut, just with a smaller effective horizon.
        if (g_memLimitMiB > 0 && !solved && (t & 127) == 0 && t - t0 > 2) {
            const size_t est = arena.capacity() * sizeof(Node)
                               + (cur.capacity() + nxt.capacity()) * sizeof(State)
                               + seen.bucket_count() * 8 + seen.size() * 48;
            if (est > g_memLimitMiB * (size_t)1048576) {
                solved = true;
                goalState = cur.front();
                planCut = t0 + (t - t0) / 2;
                std::printf("MEMORY_LIMIT: est=%zuMiB arena=%zu alive=%zu "
                            "t=%lld x=%.0f -> plan cut at t=%lld "
                            "(half the searched depth)\n",
                            est >> 20, arena.size(), cur.size(), t, x,
                            planCut);
                break;
            }
        }
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
    std::printf("capstat: maxAlive=%zu capHits=%lld dropped=%lld cap=%zu\n",
                maxAlive, capHits, capDropped, g_aliveCap);
    g_outcome.capHits = capHits;
    if (!g_fixups.empty())
        std::printf("fixups: %lld transitions overridden this call\n",
                    g_fixupHits);
    // The frontier died before the cut. That is still useful to the driver: the
    // deepest surviving branch is the best guess at how to get near the wall,
    // and replaying it in GD is how the wall gets localized at all. Emit it
    // (truncated to the cut) instead of nothing -- but only in horizon mode,
    // because a tail solve that returns a doomed plan would look like success
    // to the driver's `Test-Path $tail` check and the loop would never move.
    if (!solved && horizon > 0 && !nxt.empty()) {
        solved = true;
        goalState = nxt.front();
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
        tr << "tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act\n";
        std::ofstream sn;
        if (!snapLogPath.empty()) {
            sn.open(snapLogPath);
            g_snapOut = &sn;
        }
        State s = init;
        XSlice sl(L.objs), pl(L.portals), dl(L.pads), ol(L.orbs), sls(L.slopes),
            vl(L.speeds);
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
            L.dyn.seek((int)t);
            // ...including the doors IT opened. The witness carries its own mask
            // (stepBoth sets it below), so this is the same call the search made
            // for the group this lineage belonged to.
            L.dyn.applyTriggers(s.trig, (int)s.trigT, (int)t, x);
            std::vector<std::pair<const TouchTrig*, uint32_t>> rt;
            for (size_t b = 0; b < g_touch.size(); ++b) {
                if (s.trig & ((uint32_t)1 << b)) continue;
                const TouchTrig& T = g_touch[b];
                if (T.cx + T.hw < x - 40 || T.cx - T.hw > x + 40) continue;
                rt.push_back({&T, (uint32_t)1 << b});
            }
            sl.forRange(x - 40, x + 40, [&](const Obj& o) { rn.push_back(&o); });
            L.dyn.collect(Dynamics::NEAR, x - 40, x + 40, rn);
            pl.forRange(x - 60, x + 60, [&](const Obj& o) { rp.push_back(&o); });
            L.dyn.collect(Dynamics::PORT, x - 60, x + 60, rp);
            dl.forRange(x - 40, x + 40, [&](const Obj& o) { rd.push_back(&o); });
            L.dyn.collect(Dynamics::PAD, x - 40, x + 40, rd);
            ol.forRange(x - 50, x + 50, [&](const Obj& o) { ro.push_back(&o); });
            L.dyn.collect(Dynamics::ORB, x - 50, x + 50, ro);
            sls.forRange(x - 60, x + 60, [&](const Obj& o) { rs.push_back(&o); });
            L.dyn.collect(Dynamics::SLOPE, x - 60, x + 60, rs);
            std::vector<const Obj*> rv;
            vl.forRange(x - 80, x + 80, [&](const Obj& o) { rv.push_back(&o); });
            L.dyn.collect(Dynamics::SPEED, x - 80, x + 80, rv);
            const StepCtx K{x, xPrevR, rDxUsed, t, &rn, &rp, &rd, &ro, &rs, &rv, &SP, &SPmini, &UP, &UPmini, &rt};
            bool rdead = false;
            State c = stepBoth(s, lvl[i], K, rdead);
            c.action = (uint8_t)lvl[i];
            s = c;
            modeAt[i] = s.mode;
            tr << t << ',' << (double)s.xAbs << ',' << s.y << ',' << s.vy << ','
               << (int)s.mode << ',' << (int)s.grounded << ',' << (int)s.dual
               << ',' << s.y2 << ',' << s.vy2 << ',' << (int)s.flip2 << ','
               << (int)lvl[i] << "\n";
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
    int prev = (int)init.held, edges = 0;
    for (size_t i = 0; i < lvl.size(); ++i) {
        if (lvl[i] != prev) {
            const long long effect = t0 + (long long)i + 1;
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
            auto latOf = [](uint8_t m) { return (m == 1 || m == 3) ? 2 : 1; };
            auto modeAtTick = [&](long long j) -> uint8_t {
                if (j < 0) return init.mode;
                if ((size_t)j >= modeAt.size()) return modeAt.back();
                return modeAt[(size_t)j];
            };
            int lat;
            if (g_oldLatency) {
                lat = latOf(modeAt[i]);
            } else {
                const int lat0 = latOf(modeAtTick((long long)i - 1));
                lat = latOf(modeAtTick((long long)i - lat0));
            }
            out << "input=" << (effect - lat) << ',' << (int)lvl[i] << "\n";
            prev = lvl[i];
            ++edges;
        }
    }
    std::printf("plan: %d edges, %zu ticks -> %s\n", edges, lvl.size(),
                outPath.c_str());
    return 0;
}


}  // namespace dp
