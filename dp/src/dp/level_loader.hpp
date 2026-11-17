#pragma once
#include <cstdarg>
#include <limits>
#include <map>
#include <string_view>
#include "dp/triggers.hpp"

namespace dp {

// OPT-IN and unused by default. Nothing passes --obb yet -- not the driver, not
// the batch, not fidelity_diff -- so every level behaves exactly as before
// unless the flag is given by hand. See the note in hazardHit for why it is not
// on: the recorded box beats the bound but is still not the shape GD tests.
// Checked anyway: replaying the verified lv17/lv18/lv19 plans with and without
// --obb gives byte-identical traces, so those routes never graze a turned one.
//
// --obb <file>: the MOD's obb dump, `uid,id,type,cx,cy,rot,x0,y0,..,x3,y3`
// (solver.hpp writes it from GD's own getOrientedBox, for every object turned
// by something other than a multiple of 90). Four corners in order, so the two
// side lengths and the long axis fall straight out of them.
//
// The centre is NOT taken from this file: a grouped object MOVES, and the dump
// is one snapshot. objrects' cx,cy is the same point (checked on lv20 uid 424
// and 434, both exact) and the moving-geometry code already keeps it current,
// so only the SHAPE is read here and it rides along with the object.
struct ObbBox { double hw, hh, c, s; float g[8]; };
inline std::unordered_map<int, ObbBox> loadObb(const std::string& path) {
    std::unordered_map<int, ObbBox> m;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "obb: cannot open %s\n", path.c_str());
        return m;
    }
    std::string line;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string f[14];
        int n = 0;
        for (; n < 14 && std::getline(ss, f[n], ','); ++n) {}
        if (n < 14) continue;
        const int uid = std::atoi(f[0].c_str());
        double px[4], py[4];
        for (int i = 0; i < 4; ++i) {
            px[i] = std::atof(f[6 + 2 * i].c_str());
            py[i] = std::atof(f[7 + 2 * i].c_str());
        }
        // oob = m_shouldUseOuterOb. Without it GD kills on the AABB alone and
        // applying the box would make the model MISS deaths. Measured on every
        // level that has turned objects: it is 1 for all of them (lv16 598/598,
        // lv17 1084, lv18 666, lv19 718, lv20 1584, lv21 2096, lv22 1270), so
        // the column has never yet changed a verdict -- it is there so that the
        // day one of them reads 0, the model notices instead of guessing.
        if (n >= 15 && std::atoi(f[14].c_str()) == 0) continue;
        const double e1x = px[1] - px[0], e1y = py[1] - py[0];
        const double e2x = px[2] - px[1], e2y = py[2] - py[1];
        const double l1 = std::hypot(e1x, e1y), l2 = std::hypot(e2x, e2y);
        if (l1 < 1e-6 || l2 < 1e-6) continue;
        // The second axis is the first turned by a right angle either way, and
        // the test only ever uses |projection|, so the sign does not matter.
        ObbBox ob{l1 * 0.5, l2 * 0.5, e1x / l1, e1y / l1, {0, 0, 0, 0, 0, 0, 0, 0}};
        for (int i = 0; i < 4; ++i) { ob.g[2 * i] = std::strtof(f[6 + 2 * i].c_str(), nullptr); ob.g[2 * i + 1] = std::strtof(f[7 + 2 * i].c_str(), nullptr); }
        m[uid] = ob;
    }
    std::printf("obb: %zu turned boxes from %s\n", m.size(), path.c_str());
    return m;
}
inline std::unordered_map<int, ObbBox> g_obb;

// How many comma fields of an objrects row the loader reads. The dump has 49
// columns (rev is the last); the headroom is deliberate, and a column read by
// NAME past this bound is refused rather than read out of range.
inline constexpr int kObjFields = 72;

// A row's comma fields into f[0..n), exactly as a loop of
// `std::getline(std::stringstream(line), f[i], ',')` fills them -- an empty field between two
// commas is read, the loop stops at the end of the line, and a field it never reaches keeps what
// it had (the callers pass fresh, empty strings) -- without building a stream for every row. On
// a custom level's 117,356 rows the stream was most of each in-process call's level parse.
// Returns how many fields it filled, which is how many that loop's getline calls succeeded.
inline int splitCommaFields(const std::string& line, std::string* f, int n) {
    size_t pos = 0;
    int i = 0;
    for (; i < n && pos < line.size(); ++i) {
        const size_t c = line.find(',', pos);
        if (c == std::string::npos) {
            f[i].assign(line, pos, std::string::npos);
            ++i;
            break;
        }
        f[i].assign(line, pos, c - pos);
        pos = c + 1;
    }
    return i;
}

// Everything loadLevelFrom prints goes through here, byte for byte what std::printf would have
// written. With a sink set, the text is also kept: the ladder's level cache (cli.hpp) prints it
// again when it hands back a level it did not build.
inline std::string* g_loadPrinted = nullptr;
inline void loadPrintf(const char* fmt, ...) {
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    char buf[1024];
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string big;
    const char* s = buf;
    if (n >= (int)sizeof buf) {
        big.resize((size_t)n + 1);
        std::vsnprintf(big.data(), big.size(), fmt, again);
        big.resize((size_t)n);
        s = big.c_str();
    }
    va_end(again);
    if (n <= 0) return;
    std::fwrite(s, 1, (size_t)n, stdout);
    if (g_loadPrinted) g_loadPrinted->append(s, (size_t)n);
}

// Parse an objrects CSV from any stream.
//
// EVERY GLOBAL THIS WRITES IS LISTED IN LoadLevelWrites below, which the ladder's level cache
// (cli.hpp) keeps beside the Level and puts back when it hands a level out again. A global
// written here and missing there would keep, on every attempt after a ladder's first, whatever
// resetInvocationState left in it.
//
// The stream, rather than a path, is what makes the solver embeddable: the mod
// builds the very same CSV in memory out of PlayLayer's objects and feeds it in
// here, so THERE IS ONE PARSER. A second loader that walked GD's objects
// directly would be a second definition of what a level is, and the two would
// drift -- the model's fidelity is measured against this text.
// GD's anti-cheat spike: PlayLayer creates it last (the highest uid), an id-8 spike at (0,105),
// 6x12 with a 30x30 unscaled size. It sits in m_objects but never collides -- GD only hands it to
// destroyPlayer as a check -- and the mod no longer writes it (solver.hpp writeObjRects). Every
// dump written before that carries it, and read as a hazard it killed a ship or ball start on
// tick 2 (two custom levels, GD alive there). Recognised by all of those at once.
inline bool isAnticheatSpikeRow(const std::string& row, int maxUid) {
    if (row.size() < 2 || row[0] != '8' || row[1] != ',') return false;   // the first field is "8"
    std::string v[17];
    return splitCommaFields(row, v, 17) == 17 && v[0] == "8" && v[2] == "0" && v[3] == "105"
           && v[4] == "6" && v[5] == "12" && v[15] == "30" && v[16] == "30"
           && std::atoi(v[7].c_str()) == maxUid;
}
// THE GLITCH-AVOID MODE (g_glitchDeco, thread_pool.hpp): a decoration drawn where a hazard is, and
// placed somewhere without one, looks like a hazard and kills nothing. A wall edged with them has
// a hole that only a hitbox reading finds -- lv20's first wave passes the tip of a spike wall at
// x=1,168 through two edge pieces (id 719, the look of hazard 667) that have no hazard under them.
// A deco id counts as a hazard's look when at least 90% of its placements (and at least 3) sit on
// that hazard, same centre and same rotation; each ungrouped placement without one then gets a
// copy of a hazard row of that id and rotation, at its centre, under a fresh uid. Rows, not objects,
// so the copy is built by the same parse as the real one. Returns how many were added.
inline bool g_glitchDeco = false;   // --glitchdeco (the mode's other switches: thread_pool.hpp)
inline int addHazardLookalikes(std::vector<std::string>& rows, int& maxUid) {
    struct Row { std::vector<std::string> f; double cx = 0, cy = 0; int rot = 0; };
    std::vector<Row> rs;
    rs.reserve(rows.size());
    for (size_t i = 1; i < rows.size(); ++i) {
        Row r;
        std::stringstream ss(rows[i]);
        std::string f;
        while (std::getline(ss, f, ',')) r.f.push_back(f);
        while (!r.f.empty() && !r.f.back().empty()
               && (r.f.back().back() == '\r' || r.f.back().back() == '\n'))
            r.f.back().pop_back();
        if (r.f.size() < 10) { r.f.clear(); rs.push_back(r); continue; }
        r.cx = std::atof(r.f[2].c_str());
        r.cy = std::atof(r.f[3].c_str());
        r.rot = ((int)std::lround(std::atof(r.f[9].c_str())) % 360 + 360) % 360;
        rs.push_back(std::move(r));
    }
    auto key = [](double x, double y) {
        return ((long long)std::llround(x) << 32) ^ (long long)(std::llround(y) & 0xffffffff);
    };
    std::unordered_map<long long, std::vector<size_t>> haz;
    for (size_t i = 0; i < rs.size(); ++i)
        if (!rs[i].f.empty() && rs[i].f[1] == "2") haz[key(rs[i].cx, rs[i].cy)].push_back(i);
    auto onHazard = [&](const Row& d) -> const Row* {
        auto it = haz.find(key(d.cx, d.cy));
        if (it == haz.end()) return nullptr;
        for (size_t hi : it->second) {
            const Row& h = rs[hi];
            if (std::fabs(h.cx - d.cx) < 0.5 && std::fabs(h.cy - d.cy) < 0.5 && h.rot == d.rot)
                return &h;
        }
        return nullptr;
    };
    std::map<std::string, int> total;
    std::map<std::string, std::map<std::string, int>> pairedWith;
    for (const Row& d : rs) {
        if (d.f.empty() || d.f[1] != "7") continue;
        ++total[d.f[0]];
        if (const Row* h = onHazard(d)) ++pairedWith[d.f[0]][h->f[0]];
    }
    std::map<std::string, std::string> lookOf;   // deco id -> hazard id
    for (const auto& kv : pairedWith) {
        const auto best = std::max_element(kv.second.begin(), kv.second.end(),
                                           [](const auto& a, const auto& b) {
                                               return a.second < b.second;
                                           });
        if (best->second >= 3 && best->second >= 0.9 * total[kv.first])
            lookOf[kv.first] = best->first;
    }
    // One template row per (hazard id, rotation): the w/h columns are the rotated box's.
    std::map<std::pair<std::string, int>, const Row*> tmpl;
    for (const Row& h : rs)
        if (!h.f.empty() && h.f[1] == "2") tmpl.emplace(std::make_pair(h.f[0], h.rot), &h);
    int added = 0;
    std::vector<std::string> extra;
    for (const Row& d : rs) {
        if (d.f.empty() || d.f[1] != "7" || d.f[6] != "0") continue;
        const auto lk = lookOf.find(d.f[0]);
        if (lk == lookOf.end() || onHazard(d)) continue;
        const auto t = tmpl.find(std::make_pair(lk->second, d.rot));
        if (t == tmpl.end()) continue;
        std::vector<std::string> f = t->second->f;
        f[2] = d.f[2];
        f[3] = d.f[3];
        f[6] = "0";
        f[7] = std::to_string(++maxUid);
        std::string line;
        for (size_t k = 0; k < f.size(); ++k) line += (k ? "," : "") + f[k];
        extra.push_back(line);
        ++added;
    }
    for (std::string& e : extra) rows.push_back(std::move(e));
    if (added) {
        std::string ids;
        for (const auto& kv : lookOf) ids += " " + kv.first + "~" + kv.second;
        std::printf("glitchdeco: %d hazard look-alike(s) with no hazard under them are hazards "
                    "here (deco~hazard:%s)\n", added, ids.c_str());
    }
    return added;
}

// --rotfitcheck (debug, off): stage 1''s fit of a turned object's Rotate is also made by the scan it replaced, and the
// two are compared (taken or refused, and the tick). Counted per call; cli.hpp clears the counts with the flag.
inline bool g_rotFitCheck = false;
inline long long g_rotFitChecked = 0, g_rotFitMismatch = 0;

// THE TEXT PASS OF loadLevelFrom, KEPT FROM ONE CALL TO THE NEXT (--no-csvrows is the off arm).
//
// Before it reads a row of the level, loadLevelFrom reads the whole text into a vector of strings, takes each
// row's uid and place (fields 7, 2 and 3), takes the highest uid, drops GD's anti-cheat spike row and writes the
// rest out as one string again: 0.35 s of the 0.9 s a call spends on a custom level of 220,000 rows (34 MB),
// measured on a long custom level, and a pure function of the level text. The mod calls cliMain a hundred times a level
// with the same text, so what that pass makes is kept beside the text (LevelTextCache, cli.hpp: the same
// key, the bytes themselves) and each call reads its rows from it. The pass that depends on the call
// (which uids a flag wants positions of) still runs, over the kept (uid, x, y) list instead of the rows.
// With the glitch-avoid mode on, the rows are added to before the filter, so that mode builds it as before.
struct CsvRows {
    bool noDrop = true;          // no row was dropped: the text itself is what the rows are read from
    std::string filtered;        // otherwise the text without the dropped rows, every row '\n' ended
    int maxUid = -1;
    struct Place { int uid; double cx, cy; };
    std::vector<Place> places;   // the rows after the header with eight fields or more, in order
};
inline CsvRows buildCsvRows(const std::string& text) {
    CsvRows r;
    std::vector<std::string> rows;
    {
        std::istringstream in(text);
        std::string row;
        while (std::getline(in, row)) rows.push_back(std::move(row));
    }
    std::string f[8];
    for (size_t i = 1; i < rows.size(); ++i) {
        if (splitCommaFields(rows[i], f, 8) < 8) continue;
        const int uid = std::atoi(f[7].c_str());
        r.maxUid = std::max(r.maxUid, uid);
        r.places.push_back({uid, std::atof(f[2].c_str()), std::atof(f[3].c_str())});
    }
    std::string all;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i == 0 || !isAnticheatSpikeRow(rows[i], r.maxUid)) {
            all.append(rows[i]);
            all.push_back('\n');
        } else {
            r.noDrop = false;
        }
    }
    if (!r.noDrop) r.filtered = std::move(all);
    return r;
}

// The next row of `src` from `pos`, as `std::getline(stream, dst)` over the same text gives it: a '\n' ends a row
// (the '\n' is not in it, a '\r' before it is), a last row with no '\n' still counts, an empty row between two '\n'
// is one, and nothing follows the last '\n'. False at the end.
inline bool nextCsvRow(std::string_view src, size_t& pos, std::string& dst) {
    if (pos >= src.size()) return false;
    const size_t nl = src.find('\n', pos);
    if (nl == std::string_view::npos) {
        dst.assign(src.data() + pos, src.size() - pos);
        pos = src.size();
    } else {
        dst.assign(src.data() + pos, nl - pos);
        pos = nl + 1;
    }
    return true;
}

// `rows` and `csvText` together: the kept pass of that text (cli.hpp hands both in, only when the glitch-avoid
// mode is off); then `inRaw` is not read.
inline Level loadLevelFrom(std::istream& inRaw, const GroupTimeline* gt = nullptr,
                const std::vector<TouchTrig>* tt = nullptr,
                const std::vector<AutoTrig>* at = nullptr,
                const CsvRows* kept = nullptr, const std::string* csvText = nullptr) {
    const bool fast = kept && csvText && !g_glitchDeco;
    std::string_view src;   // fast: the text the rows are read from, and where the next one starts
    size_t srcPos = 0;
    std::istringstream filtered;
    // --rottail: where each Rotate's centre object stands, from its own row (a decoration is not
    // loaded into any list, and a centre usually is one).
    std::unordered_map<int, std::pair<double, double>> rotCentrePos;
    // --tpexitrot (modifiers.hpp): where each object a Rotate turns stands at load, to find a teleport's exit
    std::vector<std::pair<int, std::pair<double, double>>> rotObjPos;
    // --tpexitmove (modifiers.hpp): ...and each object an x-crossing Move carries
    std::vector<std::pair<int, std::pair<double, double>>> movedObjPos;
    // --dynseamlen: the level's length in x (the furthest base x of any row), for the seam rule below
    double lvMaxBaseCx = 0.0;
    if (fast) {
        for (const CsvRows::Place& pl : kept->places) lvMaxBaseCx = std::max(lvMaxBaseCx, pl.cx);
        // The same readers as the pass below, over the kept (uid, x, y) list.
        std::unordered_set<int> centres, moved;
        const bool wantCentres = !g_rotTailParts.empty();
        if (wantCentres)
            for (const auto& kv : g_rotTailParts)
                for (const RotTailPart& p : kv.second) centres.insert(p.centreUid);
        const bool wantRotObj = g_tpExitRot && g_tpPush && !g_rotTailParts.empty();
        if (g_tpExitMove && !g_autoTrig.empty())
            for (const AutoTrig& A : g_autoTrig)
                for (const TrigCtl& tc : A.ctl)
                    if (tc.tmode == 0 && (tc.dx != 0.f || tc.dy != 0.f)) moved.insert(tc.uid);
        if (wantCentres || wantRotObj || !moved.empty()) {
            for (const CsvRows::Place& pl : kept->places) {
                const int uid = pl.uid;
                const bool isCentre = wantCentres && centres.count(uid);
                const bool isRot = wantRotObj && g_rotTailParts.count(uid);
                const bool isMoved = !moved.empty() && moved.count(uid);
                if (!isCentre && !isRot && !isMoved) continue;
                if (isCentre) rotCentrePos[uid] = {pl.cx, pl.cy};
                if (isRot) rotObjPos.push_back({uid, {pl.cx, pl.cy}});
                if (isMoved) movedObjPos.push_back({uid, {pl.cx, pl.cy}});
            }
        }
        src = kept->noDrop ? std::string_view(*csvText) : std::string_view(kept->filtered);
    } else {
        std::string all, row;
        std::vector<std::string> rows;
        while (std::getline(inRaw, row)) rows.push_back(row);
        int maxUid = -1;
        // One pass over the rows serves four readers: the highest uid, --rottail's rotation centres,
        // --tpexitrot's turned objects and --tpexitmove's moved ones. Each of them used to walk the
        // rows with a stream of its own and took a row's uid (field 7) and place (fields 2 and 3) only
        // when the row had eight fields, as here; a row with fewer gives none of them anything.
        std::unordered_set<int> centres, moved;
        const bool wantCentres = !g_rotTailParts.empty();
        if (wantCentres)
            for (const auto& kv : g_rotTailParts)
                for (const RotTailPart& p : kv.second) centres.insert(p.centreUid);
        const bool wantRotObj = g_tpExitRot && g_tpPush && !g_rotTailParts.empty();
        if (g_tpExitMove && !g_autoTrig.empty())
            for (const AutoTrig& A : g_autoTrig)
                for (const TrigCtl& tc : A.ctl)
                    if (tc.tmode == 0 && (tc.dx != 0.f || tc.dy != 0.f)) moved.insert(tc.uid);
        {
            std::string f[8];
            for (size_t i = 1; i < rows.size(); ++i) {
                if (splitCommaFields(rows[i], f, 8) < 8) continue;
                const int uid = std::atoi(f[7].c_str());
                maxUid = std::max(maxUid, uid);
                lvMaxBaseCx = std::max(lvMaxBaseCx, std::atof(f[2].c_str()));
                const bool isCentre = wantCentres && centres.count(uid);
                const bool isRot = wantRotObj && g_rotTailParts.count(uid);
                const bool isMoved = !moved.empty() && moved.count(uid);
                if (!isCentre && !isRot && !isMoved) continue;
                const double rcx = std::atof(f[2].c_str()), rcy = std::atof(f[3].c_str());
                if (isCentre) rotCentrePos[uid] = {rcx, rcy};
                if (isRot) rotObjPos.push_back({uid, {rcx, rcy}});
                if (isMoved) movedObjPos.push_back({uid, {rcx, rcy}});
            }
        }
        // The glitch-avoid mode's hazard look-alikes, before the anticheat filter reads maxUid
        // (the copies' uids are above every real one, so that filter never matches them).
        const int realMaxUid = maxUid;
        if (g_glitchDeco && !rows.empty()) addHazardLookalikes(rows, maxUid);
        maxUid = realMaxUid;
        for (size_t i = 0; i < rows.size(); ++i)
            if (i == 0 || !isAnticheatSpikeRow(rows[i], maxUid)) {
                all.append(rows[i]);
                all.push_back('\n');
            }
        filtered.str(std::move(all));
    }
    std::istream& in = filtered;
    // The next row of the text, as `std::getline(in, dst)` gives it (a '\n' ends a row, a last row without one
    // still counts, nothing follows the last '\n'); on the kept pass the rows are cut out of `src` instead.
    auto nextRow = [&](std::string& dst) -> bool {
        if (!fast) return static_cast<bool>(std::getline(in, dst));
        return nextCsvRow(src, srcPos, dst);
    };
    Level L;
    // Rows the MOD marked `env` (DynSample::env): an Area Move with a variance puts the object
    // somewhere in that box, and where depends on GD's own random seeds, which differ from one
    // game to the next and between the recording and any replay. No position inside it is safe
    // to plan against, so the whole box is deadly while the object is enabled: the object gets a
    // hazard twin that follows the box, and does not collide itself on those ticks. (User
    // ruling 2026-09-19: everything the seeds can reach counts as a death, and a level that
    // cannot be passed that way has no solution.) Solids and hazards only -- see the NEAR
    // routing below; anything else keeps its rows as recorded and is counted.
    GroupTimeline envSplit;
    std::unordered_map<int, int> envTwin;   // uid -> its twin's uid
    const GroupTimeline* const gtAsRecorded = gt;
    if (gt) {
        std::vector<int> withEnv;
        for (const auto& kv : *gt)
            for (const DynSample& s : kv.second)
                if (s.env) { withEnv.push_back(kv.first); break; }
        if (!withEnv.empty()) {
            envSplit = *gt;
            for (int uid : withEnv) {
                std::vector<DynSample> twin;
                {
                    std::vector<DynSample>& own = envSplit[uid];
                    twin.reserve(own.size());
                    for (DynSample& s : own) {
                        DynSample h = s;
                        h.on = s.env ? s.on : 0;
                        h.rot = 0.f;   // the box is axis-aligned already
                        h.env = 0;
                        twin.push_back(h);
                        if (s.env) s.on = 0;
                    }
                }
                envSplit[envTwinUid(uid)] = std::move(twin);   // after `own` is done with
                envTwin[uid] = envTwinUid(uid);
            }
        }
    }
    int envTwins = 0, envOther = 0;
    g_forceFields.clear();
    g_forceCircles = 0;
    g_pressRings.clear();
    g_forceBoxes.clear();
    g_flipHeadBoxes.clear();
    g_dashStopBoxes.clear();
    g_ctrlTouchBoxes.clear();
    g_pcTrigs.clear();
    g_pcTouchBoxes.clear();
    g_revTrigs.clear();
    g_timeWarps.clear();
    g_tpTrigs.clear();
    g_tpSpawned.clear();
    g_gravTrigs.clear();
    g_gravSpawned.clear();
    g_gravTouchBoxes.clear();
    g_tpTouchBoxes.clear();
    g_camFrees.clear();
    g_zoomTrigs.clear();
    g_staticCams.clear();
    g_unlinkTrigs.clear();
    g_ctrlTrigs.clear();
    std::string line;
    // header: id,type,cx,cy,w,h,groups,uid,radius,rot[,sy0,sy1,shz]
    nextRow(line);
    // ...and these three are located BY NAME rather than by position. Every
    // other column here is positional, which is this parser's idiom, but those
    // arrive with a MOD change that lands on its own schedule -- and the one
    // failure mode this file already records (mvdir/gnddir dead from the day
    // they were written, because the field bound was not raised with them) is
    // exactly what a position guessed in advance produces. Read by name, a
    // dump whose column order differs reads as "column absent" and keeps the
    // old behaviour instead of silently reading a neighbour.
    int colFree = -1, colTouch = -1, colSpawn = -1, colChan = -1;
    int colAxis = -1, colExStat = -1, colRev = -1, colNoCol = -1, colMultiAct = -1;
    int colGMod = -1, colGPl = -1, colDontSlide = -1;
    int colTpFm = -1, colTpF = -1, colTpAng = -1, colTpRMod = -1, colTpRMin = -1, colTpRMax = -1;
    int colOptUdg = -1;
    int colExtCol = -1;   // --bigsection: Extended Collision (Obj::extCol)
    int colTpSave = -1, colTpXOff = -1, colTpRY = -1;   // --tpsaveoff (Obj::tpSave, tpXOff, tpRY)
    int colPcCtl = -1;   // --stopjump: the Player Control trigger's components and targets
    {
        std::stringstream hs(line);
        std::string name;
        for (int i = 0; std::getline(hs, name, ','); ++i) {
            while (!name.empty()
                   && (name.back() == '\r' || name.back() == '\n'))
                name.pop_back();
            if (name == "free") colFree = i;
            else if (name == "touch") colTouch = i;
            else if (name == "spawn") colSpawn = i;
            else if (name == "chan") colChan = i;
            else if (name == "axis") colAxis = i;
            else if (name == "exstat") colExStat = i;
            else if (name == "rev") colRev = i;
            else if (name == "nocol") colNoCol = i;
            else if (name == "multiact") colMultiAct = i;
            else if (name == "gmod") colGMod = i;
            else if (name == "gpl") colGPl = i;
            else if (name == "dontslide") colDontSlide = i;
            else if (name == "tpfm") colTpFm = i;
            else if (name == "tpf") colTpF = i;
            else if (name == "tpang") colTpAng = i;
            else if (name == "tprmod") colTpRMod = i;
            else if (name == "tprmin") colTpRMin = i;
            else if (name == "tprmax") colTpRMax = i;
            else if (name == "optudg") colOptUdg = i;
            else if (name == "extcol") colExtCol = i;
            else if (name == "tpsave") colTpSave = i;
            else if (name == "tprx") colTpXOff = i;
            else if (name == "tpry") colTpRY = i;
            else if (name == "pcctl") colPcCtl = i;
        }
    }
    if (colPcCtl >= kObjFields) colPcCtl = -1;
    if (g_stopJump && colPcCtl < 0)
        loadPrintf("objrects: --stopjump asked, but the dump predates the pcctl column -- no Player Control "
                   "trigger is read (re-dump to read it)\n");
    if (colTpSave >= kObjFields || colTpXOff >= kObjFields || colTpRY >= kObjFields)
        colTpSave = colTpXOff = colTpRY = -1;
    if (colOptUdg >= kObjFields) colOptUdg = -1;
    if (colExtCol >= kObjFields) colExtCol = -1;
    if (colExtCol < 0)
        loadPrintf("objrects: --bigsection asked, but the dump predates the extcol column -- an object with "
                   "Extended Collision cannot be told from one GD files in a section cell (re-dump to read it)\n");
    if (colOptUdg < 0)
        loadPrintf("objrects: --dualunlink asked, but the dump predates the optudg column -- no "
                   "Options trigger is read as unlinking the dual's gravity (re-dump to read it)\n");
    const bool tpForceCols = colTpFm >= 0 && colTpFm < kObjFields && colTpF >= 0
                             && colTpF < kObjFields && colTpAng >= 0 && colTpAng < kObjFields
                             && colTpRMod >= 0 && colTpRMod < kObjFields && colTpRMin >= 0
                             && colTpRMin < kObjFields && colTpRMax >= 0 && colTpRMax < kObjFields;
    // --tppush: a teleport's velocity, from the tp* force columns of its row (modifiers.hpp TpForce)
    auto tpForceOf = [&](const std::string* f) {
        TpForce t;
        if (!g_tpPush || !tpForceCols) return t;
        auto num = [&](int c) { return f[c].empty() ? 0.f : (float)std::atof(f[c].c_str()); };
        t.fm = (uint8_t)(f[colTpFm].empty() ? 0 : std::atoi(f[colTpFm].c_str()));
        t.f = num(colTpF);
        t.ang = num(colTpAng);
        t.rmod = num(colTpRMod);
        t.rmin = num(colTpRMin);
        t.rmax = num(colTpRMax);
        return t;
    };
    // --tpexitrot (modifiers.hpp): the exit a force's direction was read off -- a turned object standing at the
    // teleport's target -- and its Rotates, not lockrot ones, with the g_autoTrig entry that dates each
    auto tpExitOf = [&](TpForce& t, double ex, double ey) {
        if (!g_tpExitRot || t.fm == 0) return;
        for (const auto& kv : rotObjPos) {
            if (std::fabs(kv.second.first - ex) > 0.5 || std::fabs(kv.second.second - ey) > 0.5) continue;
            const auto it = g_rotTailParts.find(kv.first);
            if (it == g_rotTailParts.end()) continue;
            std::vector<TpExitRot> rs;
            for (const RotTailPart& p : it->second) {
                if (p.lock) continue;
                for (size_t k = 0; k < g_autoTrig.size(); ++k)
                    if (g_autoTrig[k].uid == p.trigUid) {
                        rs.push_back({(int)k, p.total, p.durT, p.erate, p.ease});
                        break;
                    }
            }
            if (rs.empty()) continue;
            t.exitUid = kv.first;
            g_tpExitRots[kv.first] = rs;
            loadPrintf("tpexitrot: exit uid %d at (%.0f,%.0f), %zu Rotate(s) turn it\n", kv.first, ex, ey, rs.size());
            return;
        }
    };
    auto tpForceAt = [&](const std::string* f, double ex, double ey) {
        TpForce t = tpForceOf(f);
        tpExitOf(t, ex, ey);
        return t;
    };
    // --tpexitmove (modifiers.hpp): the Moves that carry the exit of teleport `tpUid` standing at (ex, ey)
    auto tpExitMovesOf = [&](int tpUid, double ex, double ey) {
        if (!g_tpExitMove || (ex == 0.0 && ey == 0.0)) return;
        for (const auto& kv : movedObjPos) {
            if (std::fabs(kv.second.first - ex) > 0.5 || std::fabs(kv.second.second - ey) > 0.5) continue;
            std::vector<TpExitMove> ms;
            for (size_t k = 0; k < g_autoTrig.size(); ++k)
                for (const TrigCtl& tc : g_autoTrig[k].ctl)
                    if (tc.uid == kv.first && tc.tmode == 0 && (tc.dx != 0.f || tc.dy != 0.f))
                        ms.push_back({(int)k, tc.dx, tc.dy, tc.durTicks, tc.ease, tc.erate});
            if (ms.empty()) continue;
            g_tpExitMoves[tpUid] = ms;
            loadPrintf("tpexitmove: teleport uid %d exits at uid %d (%.0f,%.0f), %zu Move(s) carry it\n", tpUid,
                       kv.first, ex, ey, ms.size());
            return;
        }
    };
    g_tpForceCol = tpForceCols;
    // The portals whose force or kept height the model reads, and whether the level also turns
    // its gameplay (2900) or goes dual (type 23) -- said at the end of the load (see there).
    std::vector<int> tpActUids;
    bool tpLevelTurns = false, tpLevelDual = false;
    if (g_tpPush && !tpForceCols)
        loadPrintf("objrects: --tppush asked, but the dump predates the tpfm columns -- no "
                   "teleport's force is read (re-dump to read them)\n");
    // Counted per load, not per invocation: loadLevelFrom runs again for each
    // rotated frame, and a count that accumulated across those would report
    // the same objects several times.
    g_formulaDriven = 0;
    g_recLeadsN = 0;
    g_touchRecLeadsN = 0;
    g_freeModeCol = (colFree >= 0);
    g_trigGateCol = (colTouch >= 0 && colSpawn >= 0);
    g_staticCamCol = (colAxis >= 0 && colExStat >= 0);
    // AN ABSENT COLUMN TURNS A FEATURE OFF; IT MUST NOT DO SO IN SILENCE.
    // Reading by name means an old dump keeps working, which is the point --
    // but it also means a dump written before a column existed disables
    // whatever depends on it, and nothing says so. Measured 2026-09-04: the
    // stored objrects dumps were six columns behind the mod (free, touch,
    // spawn, chan, axis, exstat), so the corpus had been running with both of
    // these gates off, and the only way anyone found out was diffing a
    // re-dump. Nothing was riding on them -- quick_regress was identical
    // across all 22 once refreshed -- but the next omission may not be free.
    if (!g_freeModeCol || !g_trigGateCol || !g_staticCamCol)
        loadPrintf("objrects: dump predates a column this build reads --"
                   "%s%s%s (re-dump to enable them)\n",
                   g_freeModeCol ? "" : " free",
                   g_trigGateCol ? "" : " touch/spawn",
                   g_staticCamCol ? "" : " axis/exstat");
    if (colMultiAct < 0)
        loadPrintf("objrects: --multiorb asked, but the dump predates the multiact column -- "
                   "no ring is read as multi-activate (re-dump to enable it)\n");
    if (g_rotBoost && colDontSlide < 0)
        loadPrintf("objrects: --rotboost asked, but the dump predates the dontslide column -- "
                   "every 2900 is read as Dont Slide off (re-dump to read it)\n");
    // uid -> which triggers move it, and where to. Built once so the
    // routing below can ask in O(1). Touch and autonomous controls share the
    // map; an object under both keeps the touch mask (per-state truth beats
    // the level-wide approximation, and no level has the overlap anyway).
    // The easing follows the longest hop, the same rule the chain walks use:
    // when two controllers stack on one object the long one is the one still
    // moving when the short one is done, so its curve is what the tail looks
    // like. (No level has a real conflict here -- lv20's stacks are all one
    // move deep -- but the rule has to be written down somewhere.)
    struct TrigOf {
        // THE MASK'S OWN WIDTH. These are touch BOX INDICES, so the type has to
        // follow kTouchBits: at a width of 64 `(uint32_t)1 << b` is undefined
        // for b >= 32, and on x86 the shift count wraps mod 32, so boxes 32..63
        // silently answered for boxes 0..31. The consumer (dynamics.hpp's
        // dyn.trigMask) has always been TouchMask -- only the producer here
        // truncated, which is the shape that hides: widening a short value into
        // a wide vector compiles and says nothing.
        TouchMask mask{}; float dx = 0, dy = 0; double dur = 0;
        int ease = 0; double erate = 2.0;
        int aAnchor = -1; float adx = 0, ady = 0; double adur = 0;
        int aease = 0; double aerate = 2.0;
        // The lockToPlayer window and WHICH controller opened it. The lock is
        // not tied to the anchor: lv20's structure is anchored on a toggle at
        // x=22,455 while the lock comes from a Move at x=24,285, so the window
        // has to carry its own trigger (fireT / fireX live there).
        double alock = 0, alockY = 0; int alockTrig = -1; int alockN = 0;
        // --multilock: the locks after the first: {autonomous trigger index, window in ticks, mover uid, start x (NaN: the trigger's fireX)}
        std::vector<std::array<double, 4>> alockExtra;
        // ...and the same thing arriving down a TOUCH chain, kept apart. The
        // two indices mean different things -- `alockTrig` is an index into the
        // autonomous trigger list (which is what `b` is in that loop, and what
        // dynamics.hpp reads it as), while a touch lock's is a BOX -- so they
        // cannot share a field without the reader having to know which kind of
        // object it is holding.
        //
        // The touch side was collected by the walk (triggers.hpp accumulates it
        // hop by hop) and then dropped: the loop below that reads `lockTicks`
        // is the autonomous one, and the touch loop never looked. Measured on
        // lv19: 26 objects arrive through box 2 with lockTicks=284.1, of which
        // 7 collide -- 4 platforms and 3 yellow pads, which are exactly the
        // seven the formula path already computes the y of.
        double tlock = 0; int tlockBox = -1; int tlockN = 0;
        // The ANCHOR controller's own contribution, kept apart from the sum.
        // recFire is that ONE trigger's first effect, so the recorder's
        // threshold lag has to be computed from ITS move, not from every
        // controller's offset added together. Measured on lv20 uid 13276, which
        // five triggers move: the offsets (+24, -30, +120, -120, ...) cancel to
        // -6 px over 208 ticks, which takes 14 ticks to clear 0.05 px, so the
        // recording was replayed 14 ticks late. Its real anchor is a toggle
        // that moves nothing at all -- lag 0.
        float andx = 0, andy = 0; double andur = 0;
        int aAnchorMove = -1;   // --anchormove: the earliest-crossed controller that moves it
        double aMinCx = 1e18;   // --recleads: the smallest cx over its autonomous controllers
        int anease = 0; double anerate = 2.0;
        // --touchretimelag: the same threshold lag for a TOUCH-only object,
        // from the single controller (one TrigCtl, not the per-box sum, which
        // cancels a +9/-9 pair to nothing) whose move clears 0.05 px soonest.
        int tlag = -1;
        float tldx = 0, tldy = 0; double tldur = 0;
        int tlease = 0; double tlerate = 2.0;
        std::vector<Dynamics::AutoPart> parts;   // one per moving controller
        std::vector<Dynamics::AutoPart> tparts;  // touch, one per BOX (trig=bit)
    };
    std::unordered_map<int, TrigOf> trigOf;
    // --trigeffect: how much of a box's chain survives the fold below. The
    // selection test and buildTouchMoveTicks() read the individual TrigCtl
    // entries ("does any one of them move, and for how long"), while
    // applyTriggers reads the per-(box, uid) SUM built here -- so a chain that
    // adds +12 and -12 down two paths keeps its mask bit and its move window
    // while moving nothing. These counts put both sides on one line.
    std::vector<long long> effReach(tt ? tt->size() : 0, 0);
    if (tt) {
        for (size_t b = 0; b < tt->size(); ++b)
            for (const TrigCtl& c : (*tt)[b].ctl) {
                ++effReach[b];
                TrigOf& e = trigOf[c.uid];
                // The lock the walk carried down this chain. Same shape as the
                // autonomous block below, and the reason it has to exist here
                // too: a Move with offset (0,0) that only locks contributes
                // nothing to dx/dy, so every other line in this loop ignores it.
                if (c.lockTicks > 0.0) {
                    ++e.tlockN;
                    if (e.tlockBox < 0) { e.tlockBox = (int)b; e.tlock = c.lockTicks; }
                }
                e.mask |= touchBit((int)b);
                if (std::hypot((double)c.dx, (double)c.dy) > 0.5
                    && c.durTicks > 0.0) {
                    const int lg = recordLag(c.dx, c.dy, c.durTicks, c.ease,
                                             c.erate);
                    if (e.tlag < 0 || lg < e.tlag) {
                        e.tlag = lg;
                        e.tldx = c.dx; e.tldy = c.dy; e.tldur = c.durTicks;
                        e.tlease = c.ease; e.tlerate = c.erate;
                    }
                }
                e.dx += c.dx;
                e.dy += c.dy;
                if (c.durTicks >= e.dur) { e.ease = c.ease; e.erate = c.erate; }
                e.dur = std::max(e.dur, c.durTicks);
                // ...and the same effect kept per box. One box can reach the
                // uid down several chain paths; those are one controller, so
                // they merge into one part per (box, uid).
                // --movetarget: a target-mode entry is its own part (its offset
                // is decided when it fires, so it cannot be folded into a sum),
                // and it is kept although its ox/oy are zero.
                if (c.tmode) {
                    Dynamics::AutoPart tp{(int)b, 0.f, 0.f, c.durTicks, c.ease,
                                          c.erate};
                    tp.mover = c.mover;
                    tp.tmode = c.tmode;
                    tp.tdx = c.tdx;
                    tp.tdy = c.tdy;
                    tp.lag = c.spawnLag;   // --spawnlag
                    e.tparts.push_back(tp);
                } else if (std::fabs(c.dx) > 0.001f || std::fabs(c.dy) > 0.001f) {
                    Dynamics::AutoPart* found = nullptr;
                    // --movetarget: parts from different Moves stay apart, so a
                    // Stop can freeze one without the other. Off, the fold is
                    // per box as before.
                    for (auto& p : e.tparts)
                        if (p.trig == (int)b && !p.tmode
                            && p.mover == c.mover) {
                            found = &p; break;
                        }
                    if (found) {
                        found->dx += c.dx;
                        found->dy += c.dy;
                        if (c.durTicks >= found->dur) {
                            found->ease = c.ease; found->erate = c.erate;
                        }
                        found->dur = std::max(found->dur, c.durTicks);
                        // --spawnlag: the paths of one (box, Move) are one controller; its
                        // motion starts with the earliest of them
                        found->lag = std::min(found->lag, c.spawnLag);
                    } else {
                        e.tparts.push_back({(int)b, c.dx, c.dy, c.durTicks,
                                            c.ease, c.erate});
                        e.tparts.back().mover = c.mover;
                        e.tparts.back().lag = c.spawnLag;
                    }
                }
            }
    }
    // ...and report it, per box, once the fold is complete. `moved` counts the
    // uids whose SUMMED offset is non-zero -- what applyTriggers will actually
    // displace. A box with reaches > 0, uids > 0 and moved == 0 costs a dedupe
    // bit and a move window and moves nothing; lv22's ball section is fourteen
    // of them. Printing raw and folded side by side is the whole point: either
    // number alone reads as "this box does something".
    //
    // MOVE-NOOP is only that: nothing reached through this box is DISPLACED.
    // It is not "the box has no effect", and the rest of the line says what
    // else a box can do that the motion columns cannot see:
    //   tmodeUids  target-mode Moves (--movetarget) -- no offset to sum, the
    //              destination is decided when they fire, so they count as
    //              motion without appearing in movedUids
    //   lockUids   lockToPlayer windows carried down this box's chain; a Move
    //              with offset (0,0) that only locks is still a consumer
    //              (touchLock / g_lockBox)
    //   togOn      the box is a Toggle (0 off / 1 on / -1 not one), which the
    //              rotation queue reads under --rotqtoggle
    //   stops      Moves a Stop in this chain halts (--movetarget)
    //   recGate    uids this box's bit keeps `controlled` -- the same predicate
    //              as emit() below -- AND whose recording moves or switches
    //              them (more than one row), so the recording waits on the box
    //              even when the summed offset is zero. Controlled alone would
    //              count every uid the box reaches; one that the recording
    //              never changes has nothing for the gate to hold back
    // live = any of them. Selection must go by `live`, never by MOVE-NOOP.
    if (g_trigEffect && tt) {
        long long nBoxes = 0, nMoveNoop = 0, nNoopLive = 0;
        for (size_t b = 0; b < tt->size(); ++b) {
            long long uids = 0, moved = 0, tmodeUids = 0, recGate = 0;
            double maxNet = 0.0, maxDur = 0.0;
            for (const auto& kv : trigOf) {
                const TrigOf& e = kv.second;
                if (!(e.mask & touchBit((int)b))) continue;
                ++uids;
                bool tm = false;
                for (const auto& p : e.tparts) {
                    if (p.trig != (int)b) continue;
                    if (p.tmode) tm = true;
                    const double n = std::max(std::fabs((double)p.dx),
                                              std::fabs((double)p.dy));
                    if (n > 0.001) { ++moved; maxNet = std::max(maxNet, n); }
                    maxDur = std::max(maxDur, (double)p.dur);
                }
                if (tm) ++tmodeUids;
                const bool autoMoves = e.adx != 0.f || e.ady != 0.f;
                const bool noopTouch = e.dx == 0.f && e.dy == 0.f && e.dur == 0.0
                                       && e.aAnchor >= 0 && autoMoves;
                const auto rit = gt ? gt->find(kv.first) : GroupTimeline::const_iterator();
                const bool recChanges = gt && rit != gt->end() && rit->second.size() > 1;
                if (!noopTouch && recChanges) ++recGate;
            }
            std::unordered_set<int> lockSet;
            double maxLock = 0.0;
            for (const TrigCtl& c : (*tt)[b].ctl)
                if (c.lockTicks > 0.0) {
                    lockSet.insert(c.uid);
                    maxLock = std::max(maxLock, c.lockTicks);
                }
            const long long lockUids = (long long)lockSet.size();
            const int togOn = (*tt)[b].togOn;
            const size_t stops = (*tt)[b].stops.size();
            const bool moveNoop = uids && !moved && !tmodeUids;
            const bool live = moved || tmodeUids || lockUids || togOn >= 0
                              || stops || recGate;
            ++nBoxes;
            if (moveNoop) { ++nMoveNoop; if (live) ++nNoopLive; }
            loadPrintf("trigeffect: box %zu uid %d reaches=%lld uids=%lld "
                       "movedUids=%lld maxNet=%.3f maxDur=%.1f tmodeUids=%lld "
                       "lockUids=%lld maxLock=%.1f togOn=%d stops=%zu "
                       "recGate=%lld live=%d%s\n",
                       b, (*tt)[b].uid, effReach[b], uids, moved, maxNet,
                       maxDur, tmodeUids, lockUids, maxLock, togOn, stops,
                       recGate, live ? 1 : 0, moveNoop ? "  MOVE-NOOP" : "");
        }
        loadPrintf("trigeffect: %lld boxes, MOVE-NOOP %lld, of which live %lld\n",
                   nBoxes, nMoveNoop, nNoopLive);
        std::fflush(stdout);
    }
    if (at) {
        for (size_t b = 0; b < at->size(); ++b)
            for (const TrigCtl& c : (*at)[b].ctl) {
                TrigOf& e = trigOf[c.uid];
                if (g_dynDbg == c.uid)
                    loadPrintf("autoctl: uid=%d trig=%zu cx=%.1f root=%d dx=%.2f dy=%.2f dur=%.1f "
                               "lock=%.1f lockY=%.1f spawnLag=%d\n", c.uid, b, (*at)[b].cx,
                               (*at)[b].uid, (double)c.dx, (double)c.dy, c.durTicks, c.lockTicks,
                               c.lockYTicks, c.spawnLag);
                // anchor on the earliest-crossed controller (see Dynamics)
                if (e.aAnchor < 0 || (*at)[b].cx < (*at)[(size_t)e.aAnchor].cx)
                    e.aAnchor = (int)b;
                e.aMinCx = std::min(e.aMinCx, (*at)[b].cx);
                if ((std::fabs(c.dx) > 0.001f || std::fabs(c.dy) > 0.001f || c.tmode != 0
                     || c.lockTicks > 0.0 || c.lockYTicks > 0.0 || c.orbit || (*at)[b].toggleRoot)
                    && (e.aAnchorMove < 0 || (*at)[b].cx < (*at)[(size_t)e.aAnchorMove].cx))
                    e.aAnchorMove = (int)b;
                // --locknodx: a Move locked to the player's x moves it by the player's own
                // displacement IN PLACE of its Move X (prepareMoveActions takes the player's
                // delta times the move's mod as the step, modifiers.hpp), so its x offset is
                // not the object's. A custom level's uid 949 (Move 360,-180 over 300 ticks,
                // locked for 300): GD's recorded x runs exactly the player's 1.298 a tick; the
                // closed form added the 360 too and put it 224 px ahead by t=640.
                const float cdx = (c.lockTicks > 0.0) ? 0.f : c.dx;
                e.adx += cdx;
                e.ady += c.dy;
                if (c.durTicks >= e.adur) { e.aease = c.ease; e.aerate = c.erate; }
                e.adur = std::max(e.adur, c.durTicks);
                e.alockY = std::max(e.alockY, c.lockYTicks);
                if (c.lockTicks > 0.0) {
                    ++e.alockN;              // two locks on one object: unmodelled
                    if (e.alockTrig < 0) { e.alockTrig = (int)b; e.alock = c.lockTicks; }
                    else e.alockExtra.push_back({(double)b, c.lockTicks, (double)c.mover, std::nan("")});
                }
                if (std::fabs(cdx) > 0.001f || std::fabs(c.dy) > 0.001f)
                    e.parts.push_back({(int)b, cdx, c.dy, c.durTicks,
                                       c.ease, c.erate});
            }
        // --anchormove: anchor on the earliest controller whose effect the recording can SEE at its
        // crossing: an offset, a move-to or a lock (position), a Rotate about a pivot (an orbit moves
        // the object), or a Toggle root (on/off). The earliest-crossed one can be none of these -- a
        // Rotate with no pivot turns objects about their own position, a Stop, a Move with no offset
        // -- and its crossing then pairs with a first recorded change that belongs to a later
        // controller, so the recording is read that many ticks ahead (a custom level's uid 20017: a
        // pivotless Rotate at x 19,995 ahead of the +18 Move at 20,107 that the recording starts with,
        // shift -80). A Rotate with a pivot and a Toggle stay anchors: on a 32-level census their
        // objects mostly read shift 0 already, and moving their anchor broke them.
        for (auto& kv : trigOf) {
            TrigOf& e = kv.second;
            if (e.aAnchor < 0 || e.aAnchorMove < 0 || e.aAnchorMove == e.aAnchor) continue;
            if (g_shiftStat)
                loadPrintf("anchormove: uid=%d anchor=%d (x %.1f) moving=%d (x %.1f) applied=%d\n", kv.first,
                           e.aAnchor, (*at)[(size_t)e.aAnchor].cx, e.aAnchorMove,
                           (*at)[(size_t)e.aAnchorMove].cx, g_anchorMove ? 1 : 0);
            // --anchorrot (speed.hpp): the recording's first change is a turn in place, the earliest controller's
            if (g_anchorMove && gt) {
                const auto rit = gt->find(kv.first);
                if (rit != gt->end() && rit->second.size() > 1) {
                    const auto& rows = rit->second;
                    bool turnFirst = false;
                    for (size_t k = 1; k < rows.size(); ++k) {
                        if (std::fabs((double)rows[k].cx - (double)rows[0].cx) > 0.001
                            || std::fabs((double)rows[k].cy - (double)rows[0].cy) > 0.001) break;
                        if (std::fabs((double)rows[k].rot - (double)rows[0].rot) > 0.01
                            || std::fabs((double)rows[k].hw - (double)rows[0].hw) > 0.01
                            || std::fabs((double)rows[k].hh - (double)rows[0].hh) > 0.01) { turnFirst = true; break; }
                    }
                    if (turnFirst) continue;
                }
            }
            if (g_anchorMove) e.aAnchor = e.aAnchorMove;
        }
        // Second pass for the anchor's own move: the anchor is only known once
        // every controller has been seen. This accumulates because a group can
        // legitimately be moved by more than one controller -- what it must NOT
        // absorb is the same controller twice, which is what an untyped walk
        // produced (a Toggle's target group was followed as though it spawned
        // the triggers inside it, so lv19's uid14066 arrived down two chains
        // and its -75 was counted as -150). The edges are typed at the walk
        // now; this stays a sum for the real multi-controller case.
        // Each uid reads its anchor controller's entries for that uid. Walking the controller's whole
        // list for every uid costs uids x entries when one Move carries thousands of objects (a custom
        // level's 10,371-object group: more than a second of every load), so a controller's entries
        // are indexed by uid the first time it is an anchor, in list order, and each uid takes its own
        // in that order -- the sums come out in the order they were added before.
        std::vector<std::unordered_map<int, std::vector<uint32_t>>> ctlByUid(at->size());
        std::vector<char> ctlIndexed(at->size(), 0);
        for (auto& kv : trigOf) {
            TrigOf& e = kv.second;
            if (e.aAnchor < 0) continue;
            const size_t a = (size_t)e.aAnchor;
            if (!ctlIndexed[a]) {
                ctlIndexed[a] = 1;
                const std::vector<TrigCtl>& cl = (*at)[a].ctl;
                for (size_t k = 0; k < cl.size(); ++k) ctlByUid[a][cl[k].uid].push_back((uint32_t)k);
            }
            const auto mine = ctlByUid[a].find(kv.first);
            if (mine == ctlByUid[a].end()) continue;
            for (const uint32_t k : mine->second) {
                const TrigCtl& c = (*at)[a].ctl[k];
                e.andx += c.dx;
                e.andy += c.dy;
                if (c.durTicks >= e.andur) {
                    e.anease = c.ease; e.anerate = c.erate;
                }
                e.andur = std::max(e.andur, c.durTicks);
            }
        }
    }
    // --ridebox: does this arm box ride the player, and over which ticks did the recording
    // see it ride (modifiers.hpp, g_rideBox)? Riding = an autonomous Move reaches it locked
    // to the player on both axes (the level data). The window = its first recorded row that
    // leaves the load-time place, to the last row at which it still moved; open (INT_MAX)
    // when that last move is on the recording's own last tick, i.e. the flight stopped
    // inside the ride.
    auto rideWindow = [&](const Obj& ob, const std::vector<ModRow>& live, bool& rides,
                          int& t0r, int& t1r, const char* kind) {
        rides = false;
        t0r = t1r = -1;
        const auto tit = (ob.uid >= 0) ? trigOf.find(ob.uid) : trigOf.end();
        if (tit == trigOf.end() || tit->second.alockTrig < 0) return;
        const TrigOf& e = tit->second;
        if (e.alock <= 0.0 || e.alockY <= 0.0) {
            loadPrintf("ridebox: %s uid %d is locked to the player on one axis only "
                       "(x %.1f, y %.1f ticks) - read as before\n",
                       kind, ob.uid, e.alock, e.alockY);
            return;
        }
        rides = true;
        float px = (float)ob.cx, py = (float)ob.cy;
        for (const ModRow& r : live) {
            const bool moved = std::fabs(r.cx - px) > 1e-3f || std::fabs(r.cy - py) > 1e-3f;
            if (moved && r.t > 1) {
                if (t0r < 0) t0r = r.t;
                t1r = r.t;
            }
            px = r.cx;
            py = r.cy;
        }
        const bool open = t1r >= 0 && g_groupsEndT >= 0 && t1r >= g_groupsEndT - 1;
        if (open) t1r = std::numeric_limits<int>::max();
        if (t0r < 0)
            loadPrintf("ridebox: %s uid %d rides the player (lock %.1f ticks) - the "
                       "recording has no ride, read at its load-time place\n",
                       kind, ob.uid, e.alock);
        else
            loadPrintf("ridebox: %s uid %d rides the player (lock %.1f ticks) - window "
                       "t=%d..%s from the recording (end %lld)\n",
                       kind, ob.uid, e.alock, t0r,
                       open ? "open" : std::to_string(t1r).c_str(), g_groupsEndT);
    };
    // Route one object either into its static bucket or into the dynamic set.
    // An object is dynamic when the MOD recorded a timeline for its uid; a
    // grouped object that never actually moves has no rows and stays in the
    // sorted index, which is where it belongs (it is cheaper there).
    // ...UNLESS a touch trigger controls it. Those are exactly the objects that
    // never move in any recording -- that is what makes them doors -- so they
    // have to be forced into the dynamic set or the mask would have nothing to
    // act on. lv19's two door blocks have one sample each and would otherwise
    // sit in the static grid as a permanently closed wall.
    int lockXN = 0;   // --movelockx: objects whose x offsets it dropped
    auto emit = [&](uint8_t bucket, const Obj& o) {
        const auto tit = (o.uid >= 0) ? trigOf.find(o.uid) : trigOf.end();
        // --movelockx (constants.hpp): GD adds no Move's x to this object, so neither may the walk's
        // offsets -- before anything below reads them. The x locks to the player go too (they
        // reach the object through the same moveObjects). A target-mode part keeps only its y
        // (both axes -> y only, x only -> nothing), and a part left moving nothing is dropped, as
        // the walk never keeps one.
        if (!g_platformerMode && tit != trigOf.end() && objLocksX(o.id)) {
            TrigOf& e = tit->second;
            bool had = e.dx != 0.f || e.adx != 0.f || e.alock > 0.0 || e.tlock > 0.0;
            e.dx = e.adx = e.andx = e.tldx = 0.f;
            e.alock = e.tlock = 0.0;
            e.alockTrig = e.tlockBox = -1;
            auto strip = [&had](std::vector<Dynamics::AutoPart>& v) {
                for (auto& p : v) {
                    if (p.tmode == 1 || p.tmode == 2) had = true;
                    p.dx = 0.f;
                    p.tdx = 0.f;
                    if (p.tmode == 1) p.tmode = 3;
                }
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [](const Dynamics::AutoPart& p) {
                                           return p.tmode == 2 || (!p.tmode && p.dy == 0.f);
                                       }),
                        v.end());
            };
            strip(e.parts);
            strip(e.tparts);
            if (had) {
                ++lockXN;
                if (g_dynDbg == o.uid)
                    loadPrintf("movelockx: uid=%d id=%d moves only in y\n", o.uid, o.id);
            }
        }
        // A touch trigger's group walk is TRANSITIVE, so an object whose own
        // mover is autonomous can be claimed by an unrelated touch trigger --
        // and `controlled` used to win outright, throwing the autonomous move
        // away and leaving the object frozen at its static position.
        //
        // Measured on lv22 (2026-08-11), the level's first wall:
        //   uid195 (525,57) is in group 39, and group 39 has TWO controllers:
        //     uid89  id901  Move cx=375 touch=0 dur=0.5 oy=+120  <- the lifter
        //     uid199 id1346 Rot  cx=511 touch=1 dur=0.5 ox=0 oy=0
        //   GD lifts it 57 -> 177 from t=292 (x=379, right after the x=375
        //   crossing) and the player lands on its top: 192 + half 15 = y=207.000
        //   exactly, with the dump's snapuid=195. The model kept it at 57, fell
        //   through the gap and died on the spike carpet at x=598.5.
        // A touch entry that moves the object NOWHERE and takes NO time cannot
        // be what opens it, so it must not veto a controller that does. Zero
        // offsets are still honoured when they carry a duration: lv19's third
        // touch trigger is GD's "move to target" mode, which legitimately
        // records no offset (see loadTouchTriggers).
        //
        // The autonomous side has to MOVE something too. A Rotate-derived ctl
        // entry carries a zero offset and exists only to give the uid an
        // aAnchor (see the g_rotated walk), so without this the swap fires on
        // objects whose autonomous "controller" places nothing -- and the door
        // then loses its mask, which is what --needtrig-unseen steers by.
        // Measured 2026-08-11: the un-narrowed form left lv20 stuck at x=8,457
        // for 28 iterations (baseline: CLEARED in 42).
        // --autolockmoves (speed.hpp): a lockToPlayer Move moves the object too, with no offset of its own
        const bool autoMoves = (tit != trigOf.end())
                               && (tit->second.adx != 0.f || tit->second.ady != 0.f
                                   || (g_autoLockMoves && (tit->second.alock > 0.0 || tit->second.alockY > 0.0)));
        // --autopartsmove: for the no-op test only, any one of the Moves having an offset counts (a -90 then a +90 sum to nothing, and the object moved by them was
        // read as moved by nothing but its no-op touch box). Everything else keeps the net sum: a block with a target-mode touch (a garbage dy) and cancelling
        // auto Moves fell into the formula path and vanished (measured on two custom levels).
        const bool autoMovesAny = autoMoves
                                  || (tit != trigOf.end() && !tit->second.parts.empty());
        const bool noopTouch = (tit != trigOf.end()) && tit->second.mask.any()
                               && tit->second.dx == 0.f && tit->second.dy == 0.f
                               && tit->second.dur == 0.0
                               && tit->second.aAnchor >= 0 && autoMovesAny;
        const bool controlled = (tit != trigOf.end()) && tit->second.mask.any()
                                && !noopTouch;
        // --recleads: the recording moves before the player has reached any
        // autonomous controller the walk found for this object, so something the walk does not see
        // moves it first -- the object is read as its recording (the `timed` route below) instead
        // of being held at its first row until the anchor fires. The x is the recording run's,
        // from --xtrack at the recording's first change of more than 0.5 px; with no track that far
        // nothing is decided.
        bool recLeads = false;
        if (tit != trigOf.end() && !controlled && tit->second.aAnchor >= 0
            && !g_xTrack.empty() && gt && o.uid >= 0) {
            const auto ri = gt->find(o.uid);
            if (ri != gt->end() && ri->second.size() > 1) {
                const std::vector<DynSample>& sm = ri->second;
                int first = -1;
                for (const DynSample& s : sm)
                    if (std::fabs(s.cx - sm[0].cx) > 0.5f || std::fabs(s.cy - sm[0].cy) > 0.5f) {
                        first = s.t;
                        break;
                    }
                if (first >= g_xTrack.front().first && first <= g_xTrack.back().first) {
                    double xAt = g_xTrack.front().second;
                    for (const auto& r : g_xTrack) {
                        if (r.first > first) break;
                        xAt = r.second;
                    }
                    recLeads = xAt < tit->second.aMinCx;
                    if (recLeads) {
                        ++g_recLeadsN;
                        if (g_dynDbg == o.uid)
                            loadPrintf("recleads: uid=%d first change t=%d at x=%.1f, before the "
                                       "nearest controller x=%.1f\n", o.uid, first, xAt,
                                       tit->second.aMinCx);
                    }
                }
            }
        }
        const bool autoCtl = (tit != trigOf.end()) && !controlled
                             && tit->second.aAnchor >= 0 && !recLeads;
        // FORMULA-DRIVEN: both kinds of controller reach this object AND both
        // actually move it. GD applies both moves; a recording holds only one
        // attempt's combination, so neither "replay the recording" nor "ignore
        // it and use the mask" is that object's world.
        //
        // Both must MOVE, for the same reason `noopTouch` and `autoMoves`
        // exist above. Reaching alone catches lv20's six (uid10997, 9056,
        // 9057, 9126, 9127, 9197), whose touch AND autonomous controllers both
        // carry offset 0 and duration 0 -- for them base IS the recording, so
        // classifying them would move them onto a path that computes the same
        // answer more slowly and gives the recording up for nothing. With the
        // movement test the corpus-wide set is lv19's seven doors and nothing
        // else (oneoff/py/touchmovers.py; lv22's group 265 arrives in 3b, when
        // the switch band's boxes become visible).
        const bool touchMoves = (tit != trigOf.end())
                                && (tit->second.dx != 0.f || tit->second.dy != 0.f);
        const bool formulaDriven = controlled && autoMoves && touchMoves
                                   && tit->second.aAnchor >= 0;
        if (formulaDriven) ++g_formulaDriven;
        // --dyndbg: what the trigger walk decided for this object. The walk is
        // the one place where a wrong edge turns into a wrong offset, a wrong
        // duration and a wrong anchor at once, and reading those back out of a
        // trajectory is guesswork -- lv19's group 96 took a reconstruction in
        // Python that could not reproduce the loader's own answer before this
        // line existed. Anyone touching the walk should diff this across the
        // corpus before and after.
        // `>= 0`, not truthiness: the flag's OFF value is -1, which is true.
        // As written this printed a line per triggered object on every run that
        // passed --triggers, and went unseen because quick_regress sends each
        // section's stdout to DEVNULL.
        if (g_dynDbg >= 0 && tit != trigOf.end())
            loadPrintf("ctl: uid=%d mask=0x%llx dx=%.2f dy=%.2f dur=%.3f "
                       "aAnchor=%d adx=%.2f ady=%.2f adur=%.3f "
                       "autoMoves=%d noopTouch=%d controlled=%d autoCtl=%d\n",
                       o.uid, (unsigned long long)tit->second.mask.word(0),
                       tit->second.dx, tit->second.dy,
                       tit->second.dur, tit->second.aAnchor, tit->second.adx,
                       tit->second.ady, tit->second.adur, autoMoves ? 1 : 0,
                       noopTouch ? 1 : 0, controlled ? 1 : 0, autoCtl ? 1 : 0);
        const auto it = (gt && o.uid >= 0) ? gt->find(o.uid) : GroupTimeline::const_iterator();
        bool timed = gt && o.uid >= 0 && it != gt->end() && !it->second.empty();
        // --bootauto: an object only an x-crossing trigger moves (autoCtl), which the layered record holds at rest (one row, or none), is read from the
        // recording the loop left out for being on another clock (--groupsauto). The autonomous re-timing puts it where the model's own crossing of the
        // trigger says, so the other clock does not matter for it; what does is that nothing else moves it (a touch, a lock, a Follow), which autoCtl
        // is: the objects the live record never saw move are the ones the model otherwise holds still for the rest of the level.
        // ...and not an object whose Moves describe it (autoMoves: a net offset the closed form places): its recording is read as the recording arm, whose re-timing
        // put a spike 24 px early on a custom level's plan the game flies (the closed form was right there), so the closed form keeps it.
        static const std::vector<DynSample> kNoRows;
        const std::vector<DynSample>* bootRows = nullptr;
        if (g_bootAuto && autoCtl && !autoMoves && o.type == 2 && o.uid >= 0 && (!timed || it->second.size() <= 1)) {
            for (const GroupTimeline* a : g_groupAutoTL) {
                const auto ai = a->find(o.uid);
                if (ai != a->end() && ai->second.size() > 1) bootRows = &ai->second;
            }
            if (bootRows) {
                timed = true;
                if (g_dynDbg == o.uid)
                    loadPrintf("bootauto: uid=%d read from the left-out recording (%zu rows, first change t=%d)\n", o.uid, bootRows->size(),
                               bootRows->size() > 1 ? (*bootRows)[1].t : -1);
            }
        }
        const std::vector<DynSample>& trows = bootRows ? *bootRows : (timed ? it->second : kNoRows);
        // A turned object with a computable orbit belongs in dyn even with no
        // recording of its own -- that is the whole point: 806 of lv21's and
        // 264 of lv22's have none, and static is the one thing they are not.
        // Only when its CENTRE is recorded, though: without the centre's motion
        // the orbit cannot be evaluated, and an object routed here that then
        // gets no timeline would be worse off than static (dynObj=1 changes the
        // fixup gating and the collision path). Those wait for stage 2.
        bool rotComputable = false;
        if (g_rotCompute && o.uid >= 0) {
            const auto rs = g_rotSpec.find(o.uid);
            if (rs != g_rotSpec.end()) {
                ++g_rotSeen;
                if (gt) {
                    const auto ct = gt->find(rs->second.centreUid);
                    rotComputable = (ct != gt->end() && !ct->second.empty());
                }
                if (rotComputable) ++g_rotRouted;
            }
        }
        if (!controlled && !autoCtl && !timed && !rotComputable) {
            switch (bucket) {
                case Dynamics::NEAR:  L.objs.push_back(o); break;
                case Dynamics::PORT:  L.portals.push_back(o); break;
                case Dynamics::PAD:   L.pads.push_back(o); break;
                case Dynamics::ORB:   L.orbs.push_back(o); break;
                case Dynamics::SPEED: L.speeds.push_back(o); break;
                // A coin nothing controls is already in L.coins (the row loop
                // puts every coin there for the mask's numbering), so this is
                // where an UNCONTROLLED coin stops -- it needs no dyn entry.
                case Dynamics::COIN:  break;
                default:              L.slopes.push_back(o); break;
            }
            return;
        }
        L.dyn.objs.push_back(o);
        // fixups are gated off near moving geometry (fixup.hpp); set on the
        // stored copy -- `o` is const here
        L.dyn.objs.back().dynObj = 1;
        // --dynhazpad: inflate moving hazards' boxes at the sample stage (default
        // 0). Measured on lv22 x=3,495: the bottom edge of a descending spike
        // (id392, recorded box 2.6x4.8) is at 258.19 at t=2776 while the (mini)
        // player's head is at 258.00 -- 0.19 px short. The model missed it, GD
        // killed. GD's effective box is slightly larger than the recording.
        // Provisional until the real size is measured by a sweep; only affects
        // runs that pass the flag.
        {
            std::vector<DynSample> sm =
                timed ? trows
                      : std::vector<DynSample>{{0, (float)o.cx, (float)o.cy,
                                                (float)o.hw, (float)o.hh, 1}};
            // --portalrot: a portal or speed portal with no recording rows is placed by this one row, and
            // the row's angle was 0 -- so the first redraw (turnedBox) turned its box back to upright. A
            // turned portal a trigger group acts on stood as a vertical strip, wherever GD had it slanted.
            if (g_portalRot && !timed && (bucket == Dynamics::PORT || bucket == Dynamics::SPEED))
                sm[0].rot = (float)o.rot;
            // A recording from ANOTHER attempt can land in the same uid's row as
            // a continuation (the MOD's tick keeps counting across attempts).
            // Within one run the recording is dense tick by tick, so a jump of
            // MORE THAN 20,000 TICKS can only be a seam (even a whole level does
            // not reach 20,000 ticks).
            //
            // Left alone, the autonomous-trigger re-timing grabs that chunk:
            // lv22's uid6270 (an actual block) is at rest with on=1 at t=1 in
            // the live recording, yet the chunk at t=81,515 (another run, where
            // on=0) was picked and the block DISAPPEARS. GD lands on this block
            // at t=11,430 and the model fell straight through -- the wall at
            // x=16,165 (67% of the level) was this one thing.
            // The threshold is 30,000: even the longest level, lv19, runs
            // 20,600 ticks, so no gap that large occurs inside one run. At
            // 20,000 it cut a legitimate lv19 recording and the regression
            // failed (follow 232 -> 107).
            // --dynseamlen: a run cannot be longer than the level at its slowest speed (0.7x, about 1.01 px a tick), so a gap
            // inside one run is at most that; only a longer one is a seam. The fixed 30000 stays as the floor.
            const double seamGap = g_dynSeamLen ? std::max((double)g_dynSeamGap, std::ceil(lvMaxBaseCx / kSeamMinPxPerTick))
                                                 : (double)g_dynSeamGap;
            for (size_t k = 1; k < sm.size(); ++k)
                if ((double)(sm[k].t - sm[k - 1].t) > seamGap) { sm.resize(k); ++g_dynSeamCut; break; }
            if (o.type == 2 && g_dynHazPad > 0.0) {
                for (DynSample& ds : sm) {
                    ds.hw += (float)g_dynHazPad;
                    ds.hh += (float)g_dynHazPad;
                }
                // hazardHit, when obbOk, tests against the STATIC OBB (bhw/bhh)
                // and radius, not the sample's hw/hh. Unless those are inflated
                // too, this padding passes straight through with no effect
                // (measured: even with pad 1.0 the kill fixup remained)
                Obj& dob = L.dyn.objs.back();
                dob.bhw += g_dynHazPad;
                dob.bhh += g_dynHazPad;
                if (dob.radius > 0.0) dob.radius += g_dynHazPad;
            }
            L.dyn.objs.back().recStatic = (timed && sm.size() == 1) ? 1 : 0;
            L.dyn.samples.push_back(std::move(sm));
        }
        L.dyn.cur.push_back(0);
        L.dyn.bucket.push_back(bucket);
        L.dyn.baseCy.push_back((float)o.cy);
        L.dyn.baseTpY.push_back(o.tpY);
        L.dyn.baseSy0.push_back((float)o.sy0);
        L.dyn.baseSy1.push_back((float)o.sy1);
        L.dyn.on.push_back(1);
        L.dyn.prevCy.push_back((float)o.cy);
        L.dyn.trigMask.push_back(controlled ? tit->second.mask : TouchMask{});
        L.dyn.trigDx.push_back(controlled ? tit->second.dx : 0.f);
        L.dyn.trigDy.push_back(controlled ? tit->second.dy : 0.f);
        L.dyn.trigDur.push_back(controlled ? tit->second.dur : 0.0);
        L.dyn.trigEase.push_back(controlled ? tit->second.ease : 0);
        L.dyn.trigErate.push_back(controlled ? tit->second.erate : 2.0);
        // --boxparts: ...and a touch-only object whose box chain moves it in two or more parts. The
        // summed offset is one move under one clock, and its recording ends where its attempt died, so
        // an object whose motion starts past that death is held at rest -- lv22's block row uid 5961
        // (group 508, box uid17771's spawn of group 493 remapped 100 -> 508): the Ordered spawn dips it
        // 12 and lifts it 12 (Moves uid17744 / 17745), the sum is 0, and with the record ending at
        // t=7,080 the row the ball rides stayed at 345.54 while GD's sank. Each part from its box's fire
        // tick and spawn lag (Dynamics::applyTriggers, plainAt) answers past the record as before it.
        bool boxParts = false;
        // One box only: an object several boxes drive (lv22's switch band, yellow +30 and red -840
        // on the same spikes) is the recAuto class below, whose recording carries a descent no box
        // times, and the formula would drop it.
        if (controlled && !formulaDriven && !autoMoves) {
            int moving = 0, box = -1;
            bool oneBox = true;
            for (const auto& p : tit->second.tparts) {
                if (p.tmode || (p.dx == 0.f && p.dy == 0.f)) continue;
                ++moving;
                const int tb = p.trig & (kTouchBits - 1);
                if (box < 0) box = tb;
                else if (tb != box) oneBox = false;
            }
            boxParts = moving >= 2 && oneBox;
        }
        if (boxParts && g_dynDbg == o.uid)
            loadPrintf("boxparts: uid=%d takes the per-part formula (%zu parts)\n", o.uid,
                       tit->second.tparts.size());
        L.dyn.formula.push_back(formulaDriven ? 1 : boxParts ? 2 : 0);
        // --formulanolock: a formula-driven object its autonomous chain also locks to the player
        L.dyn.formulaLock.push_back((formulaDriven && (tit->second.alock > 0.0 || tit->second.alockY > 0.0)) ? 1 : 0);
        // The autonomous fields exist for `autoCtl` objects, which by
        // definition are NOT touch-controlled -- so a formula-driven object,
        // which is both, needs them filled too or its autonomous half is zero
        // and the formula silently computes only the touch move.
        // The autonomous fields exist for `autoCtl` objects, which by
        // definition are NOT touch-controlled -- so a formula-driven object,
        // which is both, needs them filled too or its autonomous half is zero.
        // Only these six: the LOCK fields stay `autoCtl`-only, because the
        // formula does not implement the lockToPlayer term and filling them
        // would hand the recording-driven path a case it never sees today.
        L.dyn.autoAnchor.push_back((autoCtl || formulaDriven) ? tit->second.aAnchor : -1);
        // Controlled (so trigMask != 0), its touch entry moves nothing, and it
        // has a live autonomous mover with a resolved anchor. `controlled` is
        // tested first because it already implies `tit != trigOf.end()` (:302),
        // so the dereference is guarded. Nine objects across the 22 levels meet
        // all four, every one of them in lv22; the fire gate that reads this
        // carries the measurement.
        L.dyn.recSelfFire.push_back((controlled && !touchMoves && autoMoves
                                     && tit->second.aAnchor >= 0) ? 1u : 0u);
        // Reached by ANY autonomous controller as well (a moving one, a resolved
        // anchor, or an autonomous Rotate). --touchretime leaves these on the
        // recording's clock: their recorded first motion can be the autonomous
        // one, and re-timing that against a box entry shifts it by the gap --
        // lv22 uid195/254/320 (group 39/38/40: touch Rotate plus autonomous Move)
        // first move at t=292/315/339 and the boxes are entered at 417/507/576,
        // so the flag slid them 127/194/239 ticks and fixcensus grew a lv22 t=425
        // divergence (edvy +12.22) that nothing else produced.
        L.dyn.autoReach.push_back(((tit != trigOf.end()
                                    && (tit->second.aAnchor >= 0 || autoMoves))
                                   || g_rotated.count(o.uid)) ? 1u : 0u);
        L.dyn.autoDx.push_back((autoCtl || formulaDriven) ? tit->second.adx : 0.f);
        L.dyn.autoDy.push_back((autoCtl || formulaDriven) ? tit->second.ady : 0.f);
        L.dyn.autoDur.push_back((autoCtl || formulaDriven) ? tit->second.adur : 0.0);
        L.dyn.autoEase.push_back((autoCtl || formulaDriven) ? tit->second.aease : 0);
        L.dyn.autoErate.push_back((autoCtl || formulaDriven) ? tit->second.aerate : 2.0);
        L.dyn.autoLock.push_back(autoCtl ? tit->second.alock : 0.0);
        L.dyn.autoLockTrig.push_back(autoCtl ? tit->second.alockTrig : -1);
        L.dyn.autoLockExtra.push_back(autoCtl ? tit->second.alockExtra : std::vector<std::array<double, 4>>{});
        // The touch lock is NOT gated on `controlled`: an object can be reached
        // by a lock-only Move (offset 0,0) down a chain that moves it nowhere
        // else, and 19 of lv19's 26 are exactly that. What gates it is the box
        // having been punched, which is per-state and lives in `trig`.
        L.dyn.touchLock.push_back(tit != trigOf.end() ? tit->second.tlock : 0.0);
        if (tit != trigOf.end() && tit->second.tlockBox >= 0) {
            if (g_lockBox < 0) {
                g_lockBox = tit->second.tlockBox;
                g_lockTicks = tit->second.tlock;
            } else if (g_lockBox != tit->second.tlockBox) {
                // Two boxes would need two accumulators. Say so rather than
                // serve one of them silently.
                loadPrintf("triggers: WARNING second touch lock box %d (using "
                           "%d) -- objects on it will not follow the player\n",
                           tit->second.tlockBox, g_lockBox);
            }
        }
        L.dyn.autoParts.push_back(autoCtl ? tit->second.parts
                                          : std::vector<Dynamics::AutoPart>{});
        // --moverextend: every Move the object's groups listen to, controlled or not (the tail rule matches it to the rows).
        L.dyn.partsAny.push_back(tit != trigOf.end() ? tit->second.parts : std::vector<Dynamics::AutoPart>{});
        L.dyn.touchParts.push_back(controlled ? tit->second.tparts
                                              : std::vector<Dynamics::AutoPart>{});
        L.dyn.rotSplit.push_back(0);   // set by the g_rotSplit stage below
        L.dyn.bootRows.push_back(bootRows ? 1 : 0);
        // When did this object first move in the recording? That tick is what
        // the replayed trajectory is re-timed against (see applyTriggers).
        int recFire = -1;
        if (controlled || autoCtl) {
            const auto& sm = L.dyn.samples.back();
            for (size_t k = 1; k < sm.size(); ++k)
                if (std::fabs(sm[k].cx - sm[0].cx) > 0.05f
                    || std::fabs(sm[k].cy - sm[0].cy) > 0.05f
                    || std::fabs(sm[k].hw - sm[0].hw) > 0.05f
                    || std::fabs(sm[k].hh - sm[0].hh) > 0.05f
                    || sm[k].on != sm[0].on) { recFire = sm[k].t; break; }
        }
        L.dyn.trigRecFire.push_back(recFire);
        // --touchrecleads (Dynamics::touchLeads): touch-controlled, the box chains move it nowhere, no autonomous
        // controller, and the recording's first change of position comes before the recorded attempt entered any
        // of its boxes (--rectouch; a box it never entered counts as later).
        {
            uint8_t lead = 0;
            if (g_touchRecLeads && g_recTouchGiven && controlled && !formulaDriven && !autoMoves
                && tit->second.aAnchor < 0 && tit->second.tlock <= 0.0 && recFire >= 0) {
                bool still = true;
                for (const auto& p : tit->second.tparts)
                    if (p.tmode || p.dx != 0.f || p.dy != 0.f) { still = false; break; }
                const auto& sm = L.dyn.samples.back();
                int firstMove = -1;
                for (size_t k = 1; still && k < sm.size(); ++k)
                    if (std::fabs(sm[k].cx - sm[0].cx) > 0.05f || std::fabs(sm[k].cy - sm[0].cy) > 0.05f) {
                        firstMove = sm[k].t;
                        break;
                    }
                if (still && firstMove >= 0) {
                    int entry = -1;
                    for (int b = 0; b < kTouchBits && (size_t)b < g_touch.size(); ++b) {
                        if (!tit->second.mask.test(b)) continue;
                        const auto rt = g_recTouch.find(g_touch[(size_t)b].uid);
                        if (rt != g_recTouch.end() && rt->second > 0
                            && (entry < 0 || rt->second < entry))
                            entry = rt->second;
                    }
                    lead = (entry < 0 || firstMove < entry) ? 1 : 0;
                }
            }
            if (lead) {
                ++g_touchRecLeadsN;
                if (g_dynDbg == o.uid)
                    loadPrintf("touchrecleads: uid=%d recording moves it first at t=%d, before any of its boxes\n",
                               o.uid, recFire);
            }
            L.dyn.touchLeads.push_back(lead);
        }
        // --recanchorx (Dynamics::recAnchorX): the recording run's crossing of the anchor, from --xtrack,
        // when the recording changed before it.
        {
            int rx = -1;
            if (autoCtl && recFire >= 0 && at && tit->second.aAnchor >= 0
                && (size_t)tit->second.aAnchor < at->size() && !g_xTrack.empty()) {
                const AutoTrig& A = (*at)[(size_t)tit->second.aAnchor];
                int cross = -1;
                for (const auto& r : g_xTrack)
                    if (r.second >= A.cx) { cross = r.first; break; }
                if (cross >= 0 && recFire < cross) {
                    rx = cross + A.delay;
                    ++g_recAnchorXN;
                }
            }
            L.dyn.recAnchorX.push_back(rx);
        }
        // --anchornoop (Dynamics::anchorUnrec): the anchor is a Toggle reaching this object that
        // moves it nowhere, and the first row already reads the state it switches to.
        {
            uint8_t un = 0;
            if (autoCtl && recFire >= 0 && at && tit->second.aAnchor >= 0
                && (size_t)tit->second.aAnchor < at->size()
                && tit->second.andx == 0.f && tit->second.andy == 0.f
                && tit->second.andur == 0.0 && !L.dyn.samples.back().empty()) {
                for (const AutoTrig::Tog& g : (*at)[(size_t)tit->second.aAnchor].tog)
                    if (g.uid == o.uid) {
                        un = ((int)L.dyn.samples.back()[0].on == (int)g.on) ? 1 : 0;
                        break;
                    }
            }
            L.dyn.anchorUnrec.push_back(un);
        }
        // recAuto: is this object's recorded motion a WORLDLINE fact rather
        // than the state's own touch? Undecidable from the summed (mask, dx,
        // dy) -- an earlier direction test was tried and reverted (2026-08-26):
        // yellow (+30 up) and red (-840 down) boxes drive the same spikes and
        // the sum points down like the recorded descent. Per box it IS
        // decidable: opposing dy signs across boxes mean no single box's chain
        // can be "the" recording (lv22's switch band, whose descent starter is
        // a collision trigger the dump cannot even time), while a door -- one
        // box, one direction -- keeps the mask-gated path (lv19/20 unchanged).
        {
            uint8_t ra = 0;
            if (controlled) {
                // --touchretime: ACROSS boxes, as the note above says. The pooled
                // test (the --no-touchretime arm, gone since the flag clean-up)
                // folded every part together, so one box whose own chain goes down
                // and back up -- lv22's uid17771 spawns a -12 and a +12 Move on
                // the block row it sinks -- was classed as a worldline fact,
                // forced fired, and had its +12 counted as a punch delay.
                // Same width rule as TrigOf::mask above: p.trig is a touch BIT
                // INDEX, so `& 31` folded boxes 32..63 onto 0..31 and let two
                // different boxes answer for each other in this test.
                TouchMask upB{}, downB{};
                for (const auto& p : tit->second.tparts) {
                    const int tb = p.trig & (kTouchBits - 1);
                    if (p.dy > 0.01f) upB |= touchBit(tb);
                    if (p.dy < -0.01f) downB |= touchBit(tb);
                }
                ra = ((upB & ~downB) && (downB & ~upB)) ? 1 : 0;
            }
            L.dyn.recAuto.push_back(ra);
        }
        // ...and how late that first ROW is against the true start. The 0.05
        // above is not a choice made here: it is grouptrace's own threshold
        // (grouptrace.hpp kEps), so the lag is computable from the curve.
        // --touchretimelag: a touch-only object's first row is late by the
        // same threshold, and --touchretime compares the box entry + latency
        // (the MOTION's start) against that ROW. lv22 uid2748 (+9 px over 48
        // ticks, ease 1): GD moves it from t=2,749 (k^2/128 px), the recording's
        // first row is 2,751 at 0.070 px, and the model played the recording
        // two ticks early.
        const bool touchLag = controlled && !autoCtl
            && tit != trigOf.end() && tit->second.tlag > 0;
        int lag = autoCtl
            ? recordLag(tit->second.andx, tit->second.andy, tit->second.andur,
                        tit->second.anease, tit->second.anerate)
            : (touchLag ? tit->second.tlag : 0);
        // --rotlag (dynamics.hpp): an anchor that is a Rotate turning this object's facing has
        // the rotation's lag, not the toggle's 0 the translation formula gives it. An eased-in
        // rotation whose angle crosses 0.05 degrees after several ticks is inverted against the
        // first row's recorded turn, as the translation is below, so a row the bounding box
        // tripped earlier keeps its earlier tick.
        if (autoCtl && lag == 0 && at && tit->second.aAnchor >= 0
            && (size_t)tit->second.aAnchor < at->size()) {
            const AutoTrig& A = (*at)[(size_t)tit->second.aAnchor];
            if (A.rotDurT > 0.0 && A.rotAngleOk && !A.rotLock) {
                int rl = rotRecordLag(A.rotTotal, A.rotDurT, A.rotEase, A.rotErate);
                const auto& sm = L.dyn.samples.back();
                if (rl > 1 && recFire >= 0 && !sm.empty()) {
                    double turned = -1.0;
                    for (const DynSample& q : sm)
                        if (q.t == recFire) {
                            turned = std::fmod(std::fabs((double)q.rot - (double)sm[0].rot), 360.0);
                            turned = std::min(turned, 360.0 - turned);   // 359.8 -> 0.4 is 0.6
                            break;
                        }
                    if (turned >= 0.0) {
                        int bestN = rl;
                        double bestErr = 1e18;
                        for (int n = 1; n <= rl; ++n) {
                            const double err = std::fabs(
                                std::fabs(A.rotTotal * gdEase(A.rotEase, A.rotErate, (double)n / A.rotDurT))
                                - turned);
                            if (err < bestErr) { bestErr = err; bestN = n; }
                        }
                        rl = bestN;
                    }
                }
                if (rl > 0) {
                    lag = rl;
                    ++g_rotLagN;
                }
            }
        }
        // ...but that formula only knows about the TRANSLATION, while the
        // recorder writes a row when ANY channel moves -- cx, cy, w, h (a turned
        // or scaled object) or the toggle. An object that is being rotated trips
        // the threshold on its bounding box long before its 15 px slide does, so
        // its first row is EARLIER than the translation predicts and the lag
        // comes out too large. Measured on lv22's spider portal uid 434
        // (autoD=(-15,0), dur 120, ease 1): the formula says 5, the true lag is
        // 3, and applyTriggers' shift then reads the recording TWO TICKS IN THE
        // PAST -- 3.4 degrees of rotation, which is fatal for an OBB test.
        //
        // The recording itself says what the lag really is: at its first row the
        // object has travelled `moved`, and the analytic curve reaches that same
        // displacement after `n` ticks. Invert it. Only ever SHRINKS the lag
        // (the row cannot be later than the translation predicts), so an object
        // whose bound is not being touched keeps the old value exactly.
        if ((autoCtl || touchLag) && lag > 1 && recFire >= 0) {
            const auto& sm = L.dyn.samples.back();
            const double full = autoCtl
                ? std::hypot((double)tit->second.andx, (double)tit->second.andy)
                : std::hypot((double)tit->second.tldx, (double)tit->second.tldy);
            const double dur = autoCtl ? tit->second.andur : tit->second.tldur;
            const int easeL = autoCtl ? tit->second.anease : tit->second.tlease;
            const double erateL =
                autoCtl ? tit->second.anerate : tit->second.tlerate;
            if (full > 0.5 && dur > 0.0) {
                double moved = 0.0;
                for (const DynSample& q : sm)
                    if (q.t == recFire) {
                        moved = std::hypot((double)q.cx - (double)sm[0].cx,
                                           (double)q.cy - (double)sm[0].cy);
                        break;
                    }
                // CLOSEST n, not the first one that reaches `moved`. The
                // threshold form loses to the recording's own precision: cx is
                // written with three decimals, so `moved` carries up to 5e-4 of
                // rounding while the epsilon allowed 1e-6, and one rounded digit
                // pushes the answer a whole tick out.
                //
                // uid 434 (lv22's spider portal, autoD=(-15,0), dur 120, ease 1
                // at rate 2) is the case that showed it. Its recorded
                // displacement at the first row is 0.019; the curve gives
                // 0.01875 at n=3 and 0.03333 at n=4, so the threshold rejected
                // the right answer by 0.00025 px -- a quarter of the recording's
                // own last digit -- and returned 4. The comment above this block
                // already said the true lag was 3.
                // Confirmed against the WHOLE ramp rather than its first row:
                // fitting the analytic curve to all 118 recorded samples, the
                // start at recFire-3 matches to 0.0003 px worst while every
                // neighbouring tick is out by 0.248 px or more. That start is
                // t=754, which is also the tick the model's own trigger says it
                // fired -- so the object's shift becomes 0 and it stops reading
                // the recording a tick in the past.
                int bestN = lag;
                double bestErr = 1e18;
                for (int n = 1; n <= lag; ++n) {
                    const double e = gdEase(easeL, erateL, (double)n / dur);
                    const double err = std::fabs(recLagCurve(full, e) - moved);   // --reclagabs
                    if (err < bestErr) { bestErr = err; bestN = n; }
                }
                lag = recLagUpN(bestN, lag, full, moved, dur, easeL, erateL);   // --reclagup
            }
        }
        L.dyn.recLag.push_back(lag);
        // Autonomous lag: a formula-driven object's recLag is its TOUCH move's (the
        // branch above), but its autonomous trigger is dated from the same first
        // row (cli.hpp, the behind-the-anchor loop). When that row is the
        // autonomous move's, the lag has to come from the autonomous curve -- the
        // anchor controller's own move (andx..anerate), inverted against the
        // row's recorded displacement exactly as above, so it only ever shrinks
        // from the threshold count.
        int aLag = lag;
        if (formulaDriven && recFire >= 0) {
            const TrigOf& e = tit->second;
            aLag = recordLag(e.andx, e.andy, e.andur, e.anease, e.anerate);
            const double full = std::hypot((double)e.andx, (double)e.andy);
            if (aLag > 1 && full > 0.5 && e.andur > 0.0) {
                const auto& sm = L.dyn.samples.back();
                double moved = 0.0;
                for (const DynSample& q : sm)
                    if (q.t == recFire) {
                        moved = std::hypot((double)q.cx - (double)sm[0].cx,
                                           (double)q.cy - (double)sm[0].cy);
                        break;
                    }
                int bestN = aLag;
                double bestErr = 1e18;
                for (int n = 1; n <= aLag; ++n) {
                    const double ev = gdEase(e.anease, e.anerate, (double)n / e.andur);
                    const double err = std::fabs(recLagCurve(full, ev) - moved);   // --reclagabs
                    if (err < bestErr) { bestErr = err; bestN = n; }
                }
                aLag = recLagUpN(bestN, aLag, full, moved, e.andur, e.anease,
                                 e.anerate);   // --reclagup
            }
            // --lagfit (print only): does the first motion belong to
            // the autonomous curve at all? The first row of a two-controller object is
            // whichever fired first; the lag above is only right if that is the
            // autonomous one. Residual of the first three moving rows against each
            // curve, started where its own lag puts it.
            if (g_lagFitDbg && aLag != lag) {
                const auto& sm = L.dyn.samples.back();
                auto resid = [&](float fx, float fy, double dur, int ease, double rate,
                                 int start) {
                    double worst = 0.0;
                    int seen = 0;
                    for (size_t k = 1; k < sm.size() && seen < 3; ++k) {
                        if (sm[k].t < recFire) continue;
                        const double u = dur > 0.0
                            ? std::min(1.0, std::max(0.0, (double)(sm[k].t - start) / dur))
                            : 1.0;
                        const double ev = gdEase(ease, rate, u);
                        const double ex = (double)sm[k].cx - (double)sm[0].cx - fx * ev;
                        const double ey = (double)sm[k].cy - (double)sm[0].cy - fy * ev;
                        worst = std::max(worst, std::hypot(ex, ey));
                        ++seen;
                    }
                    return worst;
                };
                const double ra = resid(e.andx, e.andy, e.andur, e.anease, e.anerate,
                                        recFire - aLag);
                const double rt = resid(e.tldx, e.tldy, e.tldur, e.tlease, e.tlerate,
                                        recFire - lag);
                loadPrintf("lagfit: uid=%d autoAnchor=%d recFire=%d recLag=%d autoLag=%d "
                           "resid auto=%.4f touch=%.4f -> %s\n",
                           o.uid, e.aAnchor, recFire, lag, aLag, ra, rt,
                           ra < rt ? "auto" : "touch");
            }
        }
        L.dyn.autoLag.push_back(aLag);
        // Can the formula stand in for this object's recording? Replay it over
        // every recorded sample and see. An object that is really moved by
        // something else -- a rotation about a centre group, a lockToPlayer
        // move, a move-to-target with no offset -- fails here without needing
        // to be recognised by name. With no recording there is nothing to
        // check against, and the formula is what the analytic path uses anyway.
        // A LOCKED object is the exception that has to skip the check rather
        // than fail it: its recording followed the recorded run's player, so
        // comparing the formula to it would only measure how differently that
        // run moved. For those the formula wins by construction -- there is no
        // other source that can be right for this plan.
        uint8_t closed = 0;
        if (autoCtl && tit->second.alockN == 1 && tit->second.alockY <= 0.0) {
            closed = 1;
        // The gate is "does it have a move at all", NOT "does the sum of its
        // moves come to something". An OSCILLATING group sums to zero -- lv20's
        // group 54 is -240 +240 -240 +240 -- and testing the sum threw every one
        // of them back onto the recording. That was the wall at x=32,349: a
        // 4.8x2.6 spike (uid 18425) 26 px below where the model had it.
        } else if (autoCtl && tit->second.alockN == 0
            && tit->second.alockY <= 0.0
            && !tit->second.parts.empty()) {
            closed = 1;
            // With ONE controller the recording can be checked right here: its
            // origin is recFire minus the recorder's lag and nothing else
            // enters. With several, each one starts at its own crossing and the
            // crossings are not resolved yet at load time (they come from the
            // solve's own x), so there is nothing to check against -- those
            // stand on the offline measurement instead (all 384 multi-
            // controller objects in lv19+lv20 match to 0.002 px) plus the
            // structural exclusion of rotated objects above.
            if (recFire >= 0 && tit->second.parts.size() <= 1) {
                const auto& sm = L.dyn.samples.back();
                const double bx = sm[0].cx, by = sm[0].cy;
                const int t0r = recFire - lag;
                for (const DynSample& s : sm) {
                    const double e = gdEase(tit->second.aease, tit->second.aerate,
                                            (double)(s.t - t0r) / tit->second.adur);
                    if (std::fabs((double)s.cx - (bx + tit->second.adx * e)) > 0.5
                        || std::fabs((double)s.cy - (by + tit->second.ady * e)) > 0.5) {
                        closed = 0;
                        break;
                    }
                }
            }
        }
        // A rotation moves its group about a centre, which no offset describes.
        // Those objects keep their recording (the loader has always left 1346
        // roots out; this is the same rule seen from the object's side).
        if (closed && g_rotated.count(o.uid)) closed = 0;
        L.dyn.autoClosed.push_back(closed);
        if (controlled) L.dyn.anyTrig = true;
        if (autoCtl) L.dyn.anyAuto = true;
        // the level's extent has to cover where the object GOES, not where it
        // was parked at load -- lv22's intro blocks start below the floor
        if (timed && !bootRows)   // --bootauto: the level's extent stays what the layered record says
            for (const DynSample& s : trows) {
                L.maxX = std::max(L.maxX, (double)s.cx);
                if (bucket == Dynamics::NEAR)
                    L.maxY = std::max(L.maxY, (double)s.cy + (double)s.hh);
            }
        if (controlled && bucket == Dynamics::NEAR)
            L.maxY = std::max(L.maxY,
                              (double)o.cy + (double)tit->second.dy + (double)o.hh);
        if (autoCtl && bucket == Dynamics::NEAR)
            L.maxY = std::max(L.maxY,
                              (double)o.cy + (double)tit->second.ady + (double)o.hh);
    };
    while (nextRow(line)) {
        // 21 fields now: ...,sup,w0,h0,tpy,tpg,tpix,tpiy (see the objrects
        // header in solver.hpp). An older dump simply leaves the extra ones
        // empty, which reads as "not known" and falls back to the previous
        // behaviour -- a level with no teleport is unaffected either way.
        // 26 = ...,tpiy,tw,zoom,zdur,zease,zrate (added 2026-08-14).
        // 26 -> 28 (mvdir, gnddir added: id 2900's travel / gravity direction)
        // 28 -> 34 (optp1, optp2, flipx, flipy, nofx, notouch). flipy (f[31]) is
        // the blue pad's gate (objFacingDown), flipx (f[30]) a dash ring's line
        // (--dashclamp).
        // 34 -> 36 (tpex, tpey: the real position of the teleport exit half.
        //           2026-08-18)
        // 36 -> 41 (after dis=36: editvel, vmodx, vmody, ovrvel -- id 2900's
        //           velocity change. 2026-08-19. dis itself is still unread by
        //           leveldp)
        // 41 -> 42 (force: ForceBlockGameObject::m_force. 2026-08-30)
        // 42 -> 45 (free / touch / spawn -- a mode portal's Free Mode and a
        //           trigger's touch/spawn admission, 2026-09-02). Those three
        //           are found by name above, so the bound here only has to be
        //           big enough to REACH them; the headroom is deliberate.
        // 48 -> 49 (rev: property 117, 2026-09-22). It is the 49th column, one
        //           past the old bound of 48, which is why the bound moved.
        std::string f[kObjFields];
        // 41, not 28. The bound was left at 26 when mvdir/gnddir were added
        // (2026-08-15), so f[26]/f[27] were never filled and BOTH of them read
        // as "column absent" -- the whole id-2900 direction change was dead
        // code from the day it was written, silently falling back to the old
        // rot/90 guess. Found by the gravity after lv22's second rotation
        // stepping the wrong way with mvdir sitting right there in the file.
        // WHEN A COLUMN IS ADDED, RAISE THIS BOUND TOO. Forgetting it throws no
        // exception and only ever shows up as "that rule was dead from the
        // start".
        splitCommaFields(line, f, kObjFields);
        if (f[5].empty()) continue;
        const int type = std::atoi(f[1].c_str());
        Obj o{std::atof(f[2].c_str()), std::atof(f[3].c_str()),
              std::atof(f[4].c_str()) / 2, std::atof(f[5].c_str()) / 2,
              (uint8_t)type, std::atoi(f[0].c_str()),
              f[7].empty() ? -1 : std::atoi(f[7].c_str()),
              f[8].empty() ? 0.0 : std::atof(f[8].c_str())};
        if (colExtCol >= 0 && f[colExtCol] == "1") o.extCol = 1;   // --bigsection (Obj::extCol)
        // A saw's rect is NOT its bounding circle: id=88 is 44x85 with radius
        // 32.3, so the rect is 10 px narrower than the circle on x. Every x
        // window in this file (the near-slice, the prefilter) is built from
        // hw/hh, and a window narrower than the hitbox silently MISSES
        // collisions -- which is the one error class that produces plans GD
        // kills. Normalising the box to the circle here makes all of them
        // correct at once, and hazardHit still does the round test.
        // ...and m_objectRadius is the UNSCALED radius. An object placed at a
        // scale other than 1 keeps that member but its hitbox grows with it,
        // and objrects' w,h (getObjectRect) already carries the scale, so the
        // factor comes back as w / m_width. lv20 has both cases of the SAME
        // object id: uid 650 sits at scale 1 (48 / 48) and uid 1842 at 1.2
        // (57.6 / 48), and using the raw 24 for the second one is lv20's wall
        // at x=2,703 -- model and GD agreed to the last digit for all 2,084
        // ticks and only the verdict differed.
        //
        // Measured by stepping ONE tick from an injected state and reading the
        // position back out of the trace, so the contact point is known rather
        // than assumed (2026-08-05). Offsets from each hazard's centre, wave:
        //   uid 1842  dx -35.0 lived, -33.7 lived, -33.4 DIED, -32.7 DIED
        //             dy +34.05 lived, +33.05 DIED
        //             dx -25 / dy +25 (diagonal) DIED
        //   uid 650   dx -27.42 / dy +17.35 lived
        // All eight agree with a CIRCLE of 24 x scale and disagree with the raw
        // 24 (which clears three of the deaths by 4-10 px). A rect of the same
        // w,h was tried too and is wrong in the other direction: it kills uid
        // 650's case, where GD lives.
        // The player's half is not the wobble -- GD reports the wave's own rect
        // as exactly 10x10 (cfg hitboxtrace=1, `pbox:` lines), i.e. the 5.0 the
        // model already uses.
        // Unscaled objects are untouched, which is every saw in lv11-19.
        if (o.radius > 0.0) {
            // GD scales a circle by `sx == 1 && sy == 1 ? r : max(sx, sy) * r`
            // -- playerCircleCollision 0x211df0, the same expression on both
            // sides of the kA39 branch. The dump gives the BOUND, and a
            // quarter turn swaps its width and height, so the unscaled pair
            // has to be swapped with it before dividing. `w / w0` alone makes
            // every turned blade too small: lv22's uid710 (rot -90, bound
            // 23x31 over an unscaled 31x23) came out at 2.968 where GD keeps
            // the dumped 4, and the 2026-08-26 in-situ dome is the
            // measurement that says 4 -- 17.5 = 13.5 + 4 against the spider.
            // 45 objects in the corpus are that case (lv16 1, lv21 44 by
            // count, lv22 3 by count); 11 more are genuinely scaled, where
            // max(sx, sy) differs from w/w0 on the disassembly's word alone.
            // A rotation that is NOT a multiple of 90 keeps the old form: the
            // bound of a diagonally turned object is bigger than its shape, so
            // neither expression is its scale, and this is not the change that
            // settles it.
            const double w0 = f[15].empty() ? 0.0 : std::atof(f[15].c_str());
            const double h0 = f[16].empty() ? 0.0 : std::atof(f[16].c_str());
            const double rotHere = f[9].empty() ? 0.0 : std::atof(f[9].c_str());
            const long q = std::lround(rotHere / 90.0);
            const bool quarter = std::fabs(rotHere - 90.0 * (double)q) < 0.01;
            if (!quarter) {
                if (w0 > 1.0) o.radius *= (std::atof(f[4].c_str()) / w0);
            } else {
                const bool turned = (((q % 4) + 4) % 4) & 1;
                const double uw = turned ? h0 : w0;
                const double uh = turned ? w0 : h0;
                const double sx = uw > 1.0
                                      ? std::atof(f[4].c_str()) / uw : 1.0;
                const double sy = uh > 1.0
                                      ? std::atof(f[5].c_str()) / uh : 1.0;
                if (sx != 1.0 || sy != 1.0)
                    o.radius *= (sx > sy ? sx : sy);
            }
            o.hw = o.hh = o.radius;
        }
        o.rot = f[9].empty() ? 0.0 : std::atof(f[9].c_str());
        o.flipY = (uint8_t)(f[31] == "1" ? 1 : 0);
        o.flipX = (uint8_t)(f[30] == "1" ? 1 : 0);
        o.freeMode = (uint8_t)((colFree >= 0 && f[colFree] == "1") ? 1 : 0);
        // atoi, not `== "1"`: rev is the LAST column, and a dump written to a file
        // on Windows ends its lines in CRLF, so the field reads "1\r" there while
        // the in-process copy (an ostringstream) has no CR.
        o.rev = (uint8_t)((colRev >= 0 && colRev < kObjFields
                           && std::atoi(f[colRev].c_str()) == 1) ? 1 : 0);
        o.multiAct = (uint8_t)((colMultiAct >= 0 && colMultiAct < kObjFields
                                && std::atoi(f[colMultiAct].c_str()) == 1) ? 1 : 0);
        // Recover the real box from the bound when the object is turned by
        // something other than a multiple of 90 (see Obj::oriented). Multiples
        // of 90 are left alone: there the bound IS the shape, so nothing that
        // already works can move.
        // ALWAYS ON -- the `--no-oriented` arm is gone since the flag clean-up.
        // [2026-09-06] This comment said "OFF BY DEFAULT (--oriented turns it
        // on)" long after the default had been flipped, and on 2026-09-06 it
        // was read as current and became the premise of a brief: the model was
        // said to be testing pads against the circumscribed bound, when in fact
        // it was already using the real rotated box on BOTH sides.
        // [2026-09-06, later the same day] What that re-reading then concluded
        // -- that the defect was passing a rotation to the PLAYER's side, and
        // the fix `pRotPad = 0.0` for type 8 -- is itself RETRACTED. GD's
        // activation runs two conjuncts and only the first one is axis-aligned;
        // `prect` is that first conjunct, and an instrument printing it cannot
        // see the angle the second one turns the square by. See
        // g_noPadPlayerRot in constants.hpp for the disassembly and the
        // witness. Only the default was wrong in the paragraph above; the
        // history below is kept because it is still the reason the rule is
        // called incomplete.
        // WHEN A COMMENT NAMES A DEFAULT, THE DECLARATION IS THE SOURCE.
        //
        // The measurement behind it is right -- lv18's 51-degree portal really
        // is 34x86 inside an 88x80 bound, and GD really does fire it 48 px
        // later than the bound says (two injected points, see orientedHit) --
        // but switching it on cost lv16, which was CLEARED and then died at
        // x=11,944 (and took 93 min instead of 13). Something in lv16 NEEDED
        // the wide bound, so the rule was incomplete rather than merely
        // unpolished. lv18 moved too (27,713 -> 27,388), so it was not a fix
        // for that level on its own either.
        // w0,h0 = GD's own m_width / m_height, i.e. the size BEFORE rotation.
        // Added to objrects 2026-08-04 for exactly this. Deriving it back out of
        // the bound and the angle was tried first and gave the SAME numbers
        // (34x86 / 51x56 / 31x90 on lv18's stack, 25x75 on lv16's) -- the
        // inversion was right, the mistake was not believing it and filtering on
        // "looks like 34x86". These really are three differently sized portals
        // stacked at one x, and lv16's really are scaled to 25x75.
        const double w0 = f[15].empty() ? 0.0 : std::atof(f[15].c_str());
        const double h0 = f[16].empty() ? 0.0 : std::atof(f[16].c_str());
        if (type == 28) {
            o.tpY = f[17].empty() ? 0.0 : std::atof(f[17].c_str());
            o.tpGrav = f[18].empty() ? 0 : (uint8_t)std::atoi(f[18].c_str());
            o.tpEx = f[34].empty() ? 0.0 : std::atof(f[34].c_str());
            o.tpEy = f[35].empty() ? 0.0 : std::atof(f[35].c_str());
            const bool igx = (!f[19].empty() && std::atoi(f[19].c_str()));
            const bool igy = (!f[20].empty() && std::atoi(f[20].c_str()));
            // m_ignoreX keeps the PLAYER's x -- which is what this model does
            // for every teleport anyway (x is the DP's clock), so it IS
            // modelled now. The two remaining inexpressible shapes stay loud:
            // an x-moving exit (non-747, exit x away from the portal, ignoreX
            // off) and m_ignoreY (keep player y).
            // --tpportalx (constants.hpp): an exit ahead moves x too (Obj::tpKeep bit 4)
            bool movesX = false;
            if (o.id != 747 && (o.tpEx != 0.0 || o.tpEy != 0.0)
                && !igx && std::fabs(o.tpEx - o.cx) > 0.5) {
                // --tpback (modifiers.hpp): ...and one behind, x and all (Obj::tpKeep bit 8 marks it)
                movesX = o.tpEx > o.cx || g_tpBack;
                if (movesX)
                    loadPrintf("teleport: uid %d at (%.0f,%.0f) moves x to %.0f\n", o.uid, o.cx, o.cy,
                               o.tpEx);
                else
                    loadPrintf("teleport: uid %d at (%.0f,%.0f) moves x to %.0f "
                               "- NOT modelled\n", o.uid, o.cx, o.cy, o.tpEx);
            }
            // --tpkeepy: the portal pass reads the keep (Obj::tpKeep)
            if (igy && g_tpKeepY) {
                o.tpKeep = (uint8_t)((igx ? 1 : 0) | 2);
                loadPrintf("teleport: uid %d at (%.0f,%.0f) keeps y\n", o.uid, o.cx, o.cy);
            } else if (igy) {
                loadPrintf("teleport: uid %d at (%.0f,%.0f) uses ignoreY "
                           "- NOT modelled\n", o.uid, o.cx, o.cy);
            }
            if (movesX) o.tpKeep = (uint8_t)(o.tpKeep | 4 | (o.tpEx < o.cx ? 8 : 0));
            // --tpsaveoff: m_saveOffset keeps the body's height above the portal and moves x back
            if (g_tpSaveOff && o.id == 747 && colTpSave >= 0 && colTpXOff >= 0 && colTpRY >= 0
                && f[colTpSave] == "1") {
                o.tpSave = 1;
                o.tpRY = std::atof(f[colTpRY].c_str());
                o.tpRX = std::atof(f[colTpXOff].c_str());
                // teleportPlayer (0x20fdb0) sets an id-747 portal's target to (the body's own x, the portal's real y +
                // its y offset) before the saved offset is applied, so x' = x + (x - the entry's real x), not the
                // exit's x plus it: a custom level t=3,193 (entry real x 3,357, exit 3,347, body 3,342.3026) leaves
                // the body at 3,327.605 = 2 * 3,342.3026 - 3,357, where the exit's x gave 3,332.30
                o.tpSaveIgx = 1;
                o.tpXOff = -1.0;   // only its sign is read: the body goes back (--tpback)
                o.tpKeep = (uint8_t)(o.tpKeep | 8);
                loadPrintf("teleport: uid %d at (%.0f,%.0f) saves the offset: y' = %.1f + (y - %.1f), x' = 2 x - %.2f\n",
                           o.uid, o.cx, o.cy, o.tpY, o.tpRY, o.tpRX);
            }
            o.tpForce = tpForceOf(f);
            tpExitOf(o.tpForce, o.tpEx, o.tpEy);   // --tpexitrot
            if (o.id != 747) tpExitMovesOf(o.uid, o.tpEx, o.tpEy);   // --tpexitmove
            if (o.tpForce.fm || (o.tpKeep & 2)) tpActUids.push_back(o.uid);
            if (o.tpForce.fm)
                loadPrintf("teleport: uid %d at (%.0f,%.0f) %s %.3g along %.3g deg%s\n", o.uid,
                           o.cx, o.cy, (o.tpForce.fm & 4) ? "redirects, x" : "pushes",
                           (o.tpForce.fm & 4) ? o.tpForce.rmod : o.tpForce.f, o.tpForce.ang,
                           (o.tpForce.fm & 4) ? "" : (o.tpForce.fm & 2) ? " (adds)" : "");
            // A non-747 teleport resolves its target from the linked exit half
            // (m_orangePortal), NOT the tpy closed formula -- an old dump
            // without the tpex/tpey columns plans against a target measured
            // wrong on lv22 (705 saved vs 1905 real). Refresh objrects.
            if (o.id != 747 && o.tpEx == 0.0 && o.tpEy == 0.0)
                loadPrintf("teleport: uid %d id %d at (%.0f,%.0f) has NO exit "
                           "columns (old objrects dump) - target unreliable, "
                           "refresh objrects\n", o.uid, o.id, o.cx, o.cy);
            if (o.tpGrav)
                loadPrintf("teleport: uid %d at (%.0f,%.0f) sets gravity mode %d\n",
                           o.uid, o.cx, o.cy, (int)o.tpGrav);
        }
        // --teleportorb: the orb's target and its keeps, from the same columns (solver.hpp writes
        // them for a type 46 that is a TeleportPortalObject).
        if (type == 46) {
            o.tpGrav = f[18].empty() ? 0 : (uint8_t)std::atoi(f[18].c_str());
            o.tpEx = f[34].empty() ? 0.0 : std::atof(f[34].c_str());
            o.tpEy = f[35].empty() ? 0.0 : std::atof(f[35].c_str());
            o.tpKeep = (uint8_t)(((!f[19].empty() && std::atoi(f[19].c_str())) ? 1 : 0)
                                 | ((!f[20].empty() && std::atoi(f[20].c_str())) ? 2 : 0));
            // The orb's force is not measured (calib_teleportorb has none): it stays out.
            if (tpForceOf(f).fm)
                loadPrintf("teleportorb: uid %d at (%.0f,%.0f) sets a velocity - NOT modelled\n",
                           o.uid, o.cx, o.cy);
            loadPrintf("teleportorb: uid %d at (%.0f,%.0f) -> (%.1f, %.1f)%s%s grav %d%s\n", o.uid,
                       o.cx, o.cy, o.tpEx, o.tpEy, (o.tpKeep & 1) ? " keepX" : "",
                       (o.tpKeep & 2) ? " keepY" : "", (int)o.tpGrav,
                       (o.tpEx == 0.0 && o.tpEy == 0.0) ? " - NO target in the dump" : "");
        }
        if (o.id == 2900) tpLevelTurns = true;
        if (type == 23) tpLevelDual = true;
        // --ctrltouch (modifiers.hpp): a touch-triggered Options trigger that switches player 1's
        // controls
        // --dualunlink (frames.hpp): an Options trigger that raises or lowers Unlink Dual Gravity, on the
        // x-crossing queue
        if (o.id == 2899 && colOptUdg >= 0 && !f[colOptUdg].empty()) {
            const int udg = std::atoi(f[colOptUdg].c_str());
            if (udg != 0) {
                const bool queued = !(g_trigGateCol && (f[colTouch] == "1" || f[colSpawn] == "1"));
                if (queued) g_unlinkTrigs.push_back({o.cx, (int8_t)(udg == 1 ? 1 : -1)});
                loadPrintf("dualunlink: uid %d at x=%.0f %s the dual's gravity%s\n", o.uid, o.cx,
                           udg == 1 ? "unlinks" : "links",
                           queued ? "" : " - touch/spawn-triggered, IGNORED");
            }
        }
        // --ctrlwintrig (frames.hpp): every optp1 2899, the gated ones as such
        if (o.id == 2899 && !f[28].empty()) {
            const int p1 = std::atoi(f[28].c_str());
            if (p1 != 0)
                g_ctrlTrigs.push_back({o.cx, (int8_t)(p1 == 1 ? 1 : -1),
                                       (uint8_t)(g_trigGateCol && (f[colTouch] == "1" || f[colSpawn] == "1"))});
        }
        if (o.id == 2899 && g_trigGateCol && f[colTouch] == "1" && !f[28].empty()) {
            const int p1 = std::atoi(f[28].c_str());
            if (p1 != 0) {
                g_ctrlTouchBoxes.push_back({o.cx, o.cy, o.hw, o.hh, o.uid, (uint8_t)(p1 == 1 ? 1 : 0)});
                loadPrintf("ctrltouch: uid %d at (%.0f,%.0f) %.1fx%.1f %s player 1's controls\n", o.uid,
                           o.cx, o.cy, o.hw * 2.0, o.hh * 2.0, p1 == 1 ? "disables" : "enables");
            }
        }
        // --stopjump (speed.hpp): Player Control (1932) with Stop Jump, aimed at player 1 or at both. The `pcctl`
        // column: bit 0 Stop Jump, 1 Stop Move, 2 Stop Rotation, 3 Stop Slide, 4 player 1, 5 player 2 (neither
        // target bit = both, as the game reads it).
        if (o.id == 1932 && g_stopJump && colPcCtl >= 0 && !f[colPcCtl].empty()) {
            const int pc = std::atoi(f[colPcCtl].c_str());
            if (pc & 1) {
                const bool p1 = (pc & 16) != 0 || (pc & 32) == 0;
                const bool touch = g_trigGateCol && f[colTouch] == "1";
                const bool spawn = g_trigGateCol && f[colSpawn] == "1";
                const bool chan = colChan >= 0 && !f[colChan].empty() && std::atoi(f[colChan].c_str()) != 0;
                if (!p1)
                    loadPrintf("stopjump: uid %d at x=%.0f is for player 2 only - NOT modelled\n", o.uid, o.cx);
                else if (spawn && !touch)
                    loadPrintf("stopjump: uid %d at x=%.0f is spawn-triggered - NOT modelled\n", o.uid, o.cx);
                else if (chan && !touch)
                    loadPrintf("stopjump: uid %d at x=%.0f is on a rotate channel - NOT modelled\n", o.uid, o.cx);
                else if (touch) {
                    g_pcTouchBoxes.push_back({o.cx, o.cy, o.hw, o.hh, o.uid});
                    loadPrintf("stopjump: uid %d at (%.0f,%.0f) %.1fx%.1f releases the button on touch\n", o.uid, o.cx,
                               o.cy, o.hw * 2.0, o.hh * 2.0);
                } else {
                    g_pcTrigs.push_back({o.cx, o.uid});
                    loadPrintf("stopjump: uid %d at x=%.0f releases the button when x is crossed\n", o.uid, o.cx);
                }
            }
        }
        // --reversetrig (speed.hpp): a Reverse trigger (1917) on the x-crossing queue
        if (o.id == 1917 && g_reverseTrig) {
            const bool touch = g_trigGateCol && f[colTouch] == "1";
            const bool spawn = g_trigGateCol && f[colSpawn] == "1";
            const bool chan = colChan >= 0 && !f[colChan].empty() && std::atoi(f[colChan].c_str()) != 0;
            if (touch || spawn || chan) {
                loadPrintf("reversetrig: uid %d at x=%.0f is %s - NOT modelled\n", o.uid, o.cx,
                           touch ? "touch-triggered" : spawn ? "spawn-triggered" : "on a rotate channel");
            } else {
                g_revTrigs.push_back({o.cx, o.uid});
                loadPrintf("reversetrig: uid %d at x=%.0f turns the player round when x is crossed\n", o.uid, o.cx);
            }
        }
        if (o.radius == 0.0 && !o.slope && w0 > 1.0 && h0 > 1.0) {
            const double m = std::fabs(std::fmod(o.rot, 90.0));
            if (m > 0.5 && m < 89.5) {
                const double th = o.rot * 3.14159265358979 / 180.0;
                {
                    // [2026-08-19] w0/h0 are the raw sprite dimensions, and for
                    // a scaled object they are not the real box. The factor to
                    // the real size is recovered from the bound (w,h) -- the
                    // ratio of o.hw to the predicted half-bound
                    // (w0/2)|c| + (h0/2)|s|. Same trap and same formula the
                    // rings (the ORB branch below) hit first.
                    // Measured on lv21 uid24194 (id111, rot=43, raw 34x86,
                    // bound 167x172): k = 2.0 exactly. The 17x43 real box built
                    // from the raw size did not reach GD's firing point
                    // (t=18,087, x54.07 from the centre).
                    // Within 1% is dump rounding = snapped to 1 (only a scaled
                    // object is allowed to move).
                    const double ccL = std::fabs(std::cos(th));
                    const double ssL = std::fabs(std::sin(th));
                    const double denomL = (w0 * 0.5) * ccL + (h0 * 0.5) * ssL;
                    double kSc = (denomL > 1.0) ? o.hw / denomL : 1.0;
                    if (std::fabs(kSc - 1.0) < 0.01) kSc = 1.0;
                    double W = w0 * kSc, H = h0 * kSc;
                    // --padinv (speed.hpp): a turned pad stretched along one axis only
                    if (type == 8 || type == 9 || type == 10 || type == 34 || type == 44) {
                        const double detP = ccL * ccL - ssL * ssL;
                        const double predHh = (W * 0.5) * ssL + (H * 0.5) * ccL;
                        if (std::fabs(detP) > 0.1 && std::fabs(predHh - o.hh) > 0.02 * o.hh + 0.5) {
                            const double Wi = 2.0 * (o.hw * ccL - o.hh * ssL) / detP;
                            const double Hi = 2.0 * (o.hh * ccL - o.hw * ssL) / detP;
                            if (Wi > 1.0 && Hi > 0.5) { W = Wi; H = Hi; }
                        }
                    }
                    // The old rule here refused to believe any inversion that
                    // did not land on 34x86 -- see git for the three stacked
                    // lv18 portals (k = 1) and lv16's (12,122.9,556.7), bound
                    // 75.37x64.31 rot -54, which now scales by k = 0.84. That
                    // lv16 portal shrinking is exactly what once killed a
                    // CLEARED lv16 at x=11,944, so this change is condition-
                    // class: the 4th layer (lv16/22 cold) must pass, a green
                    // census is not enough.
                    if (W > 1.0 && H > 1.0) {
                        o.oriented = 1;
                        o.ohw = W * 0.5; o.ohh = H * 0.5;
                        // --portalobb: GD's own box, when the export has it (oob=1), replaces the one
                        // rebuilt from the bound; rc/rs below are cos/sin of the first edge's angle
                        const auto obbIt = (g_portalObb && o.uid >= 0 && (type == 4 || type == 5 || type == 17 || type == 18
                                                           || (g_portalObbMode && (type == 6 || type == 16 || type == 19 || type == 26 || type == 27
                                                                                   || type == 33 || type == 41))))
                                               ? g_obb.find(o.uid) : g_obb.end();
                        const bool useObb = obbIt != g_obb.end();
                        if (useObb) {
                            loadPrintf("portalobb: uid %d id %d rebuilt %.2fx%.2f -> GD's %.2fx%.2f\n", o.uid, o.id,
                                       W, H, obbIt->second.hw * 2.0, obbIt->second.hh * 2.0);
                            o.ohw = obbIt->second.hw; o.ohh = obbIt->second.hh;
                            for (int q = 0; q < 8; ++q) o.gc[q] = obbIt->second.g[q];
                            o.hasGc = 1; o.gcx = o.cx; o.gcy = o.cy;   // --obbfloat (object.hpp)
                        }
                        // cocos2d's getRotation() is CLOCKWISE-positive, so the
                        // world->local transform takes -rot. Getting this sign
                        // wrong is what made the box test still fire early even
                        // after the shape was right -- and it looks correct on a
                        // point near the object's centre, which is why it
                        // survived three rounds of "fix the shape".
                        // Checked against the three GD measurements on lv18's
                        // portal (offsets from its centre, wave half 5,
                        // |local_x| vs the 17+7.03 threshold):
                        //   dx=+2.0 dy=27.5 -> 20.1  fires      (GD: fires)
                        //   dx=-2.7 dy=33.8 -> 27.97 does not   (GD: does not)
                        //   dx=-46  dy=13.4 -> 39.3  does not   (GD: does not)
                        // With +rot the same three come out 22.6 / 24.6 / 18.5,
                        // i.e. "fires" for all three. That last one is the tick
                        // the model turned into a cube 48 px early.
                        const double thc = -th;
                        o.rc = std::cos(thc); o.rs = std::sin(thc);
                        if (useObb) { o.rc = obbIt->second.c; o.rs = obbIt->second.s; }
                    }
                    // MEASURED, and the 34x86 filter is too tight to fix lv18.
                    // The three portals stacked at x=27,756 all sit at rot 51
                    // and all bound differently, so they invert differently:
                    //   id=12  type 6  bound 88.23x80.54 -> 33.99x86.01  USED
                    //   id=99  type 17 bound 89.45x80.73 -> 31.01x89.99  rejected
                    //   id=202 type 20 bound 75.61x74.88 -> 51.01x55.99  rejected
                    // Fixing only the cube portal is not enough: the size and
                    // speed portals still fire 48 px early on their bounds, and
                    // the DP still returns SOLVED on a plan GD kills.
                    // Widening the filter to a plausible RANGE does not separate
                    // the cases either -- lv16's (25x75) sits inside any range
                    // that admits these three. **Do not guess another filter.**
                    // Measure lv16's portal at (12,122.9,556.7) in GD the way
                    // lv18's was measured (hold the player at a fixed y through
                    // it and find the x where it fires) and let the rule follow
                    // from the two data points.
                }
            }
        }
        L.maxX = std::max(L.maxX, o.cx);
        // 21 is a ONE-WAY PLATFORM (see Obj::oneway). The loader dropped it
        // entirely, so the four-block column at lv13 x=1665 -- the only footing
        // in a 450 px spike field -- did not exist for the model, and the search
        // could not get past x=1724 no matter how it flew. Everything downstream
        // splits objs by `type == 0` (solid) vs anything else (hazard), so it is
        // stored AS 0 with the oneway flag set; left as 21 it would have killed
        // on contact instead of holding the player up. Only lv13 (32) and lv18
        // (8) have any, both id 143, so lv1-12 cannot be affected by this.
        if (type == 21) { o.type = 0; o.oneway = 1; }
        // COINS (22 = secret, 31 = user). They fall through every branch below
        // -- neither type has ever had one -- so collecting them here is purely
        // additive: nothing else in the level changes, and a run without
        // --coins never looks at the list.
        //
        // The box is the object's own, taken as dumped. That is right for every
        // official coin (64 of the 66 are a plain 40x40 at rot 0 and the other
        // two are scaled with rot 0), and WRONG FOR A TURNED ONE, where w,h is
        // the bounding box and GD tests the oriented shape -- measured on the
        // coincal rig at rot=45, where the bounding box is out by 28 px at the
        // far station. No official level has one; a custom level can, and the
        // fix is the obb path the turned hazards already use.
        // `continue`, not `return`: this is the row loop, not emit()'s lambda.
        //
        // Every coin goes in L.coins -- that list is the mask's numbering -- and
        // is ALSO offered to emit(), which keeps it if a trigger controls it
        // (lv20's first coin is toggled in and out with the platform under it;
        // lv21's third and lv22's third are moved into reach). emit's COIN case
        // drops an uncontrolled one, so a level whose coins nothing touches ends
        // up exactly as it was before this.
        // ...and loadCoinUids below reads the SAME two columns off the SAME
        // file, because the touch window is built before this loop runs. If the
        // test here ever changes, change it there too -- they are neighbours so
        // that a drift is visible rather than silent.
        if (type == 22 || type == 31) {
            L.coins.push_back(o);
            if (g_coinRoute) emit(Dynamics::COIN, o);
            continue;
        }
        // HAZARDS ONLY, for now. A turned SOLID has the same bound problem, but
        // the solid branches also LAND on and ride their objects, and a landing
        // resolved against a slanted face is a different piece of work; the
        // hazard branch only ever asks "touching?", which the box answers on
        // its own. One variable per regression.
        if ((type == 2 || type == 47) && o.radius == 0.0 && o.uid >= 0) {
            const auto it = g_obb.find(o.uid);
            if (it != g_obb.end()) {
                o.obbOk = 1;
                o.bhw = it->second.hw; o.bhh = it->second.hh;
                o.bc = it->second.c;   o.bs = it->second.s;
                for (int q = 0; q < 8; ++q) o.gc[q] = it->second.g[q];
                o.hasGc = 1; o.gcx = o.cx; o.gcy = o.cy;   // --obbfloat (object.hpp)
            } else if (g_rotated.count(o.uid)) {
                // --rotobb (dynamics.hpp): its own box at its load angle, a right angle (the export
                // holds every other one), so the box is the rect it was loaded with
                o.obbOk = 1;
                o.bhw = o.hw; o.bhh = o.hh;
                o.bc = 1.0;   o.bs = 0.0;
            }
        }
        // --ringpresseat (speed.hpp): every toggle ring, with its recorded rows; the flag decides at run time
        if (o.id == 1594) {
            PressRing pr{o.cx, o.cy, o.hw, o.hh, o.uid, o.multiAct, {}, {}, {}, {}};
            if (gt && o.uid >= 0) {
                const auto fit = gt->find(o.uid);
                if (fit != gt->end())
                    for (const auto& r : fit->second) {
                        pr.liveT.push_back(r.t);
                        pr.liveX.push_back(r.cx);
                        pr.liveY.push_back(r.cy);
                        pr.liveOn.push_back(r.on);
                    }
            }
            g_pressRings.push_back(std::move(pr));
        }
        if (o.type == 0 || type == 2 || type == 47) {
            L.maxY = std::max(L.maxY, o.cy + o.hh);
            // --dropnocollide: keep id-1910 out of the collidable set. GD sets
            // [obj+0x515] on it and never collides with it at any depth (run D), while
            // the model kills on it at lv22 t=4,889. See constants.hpp for the
            // measurement and for why the ID is the discriminator rather than the flag.
            //
            // Placed AFTER the maxY fold on purpose: maxY feeds the band/ceiling logic,
            // and skipping before it would make the flag change the reachable height as
            // well as the collision -- a wider change than the defect, and one that
            // would not show up at t=4,889.
            //
            // Placed here rather than inside emit() for the same reason: emit also feeds
            // L.dyn.objs, so a skip in there would drop the object from the dynamic path
            // too if it were ever grouped. This object has groups=0, so the static path
            // is the whole of it.
            // `continue`, NOT `return`: this branch is inside the row loop that
            // starts at the `while (std::getline(in, line))` above, so a return here
            // would abandon every remaining object in the level after the first
            // id-1910 row. Both compile. The wrong one was invisible to the regression
            // suite while this was a flag that ran OFF there, the line never reached --
            // and would surface only as "the 4,889 death is gone", which is exactly
            // what a truncated level looks like too.
            //
            // ...and the ID was only ever a STAND-IN for the byte, which the dump
            // now carries (`nocol`). It held while the corpus had exactly one
            // id-1910 object; SubZero 4002's uid 11433 is the second, is NOT
            // flagged, and the game crushes the player against it -- the model
            // walked through and the run ended 12,000 px later on a plan GD
            // refuses. So: with the column, the byte decides; without it (a dump
            // written before this), the old ID rule stands, because dropping it
            // there would put lv22's t=4,889 death back.
            bool noCollide =
                (colNoCol >= 0 && colNoCol < kObjFields && !f[colNoCol].empty())
                    ? std::atoi(f[colNoCol].c_str()) != 0
                    : o.id == kNoCollideId;
            // --nocolone: a solid with the byte is a one-way top, not a hole. GD reads [obj+0x515]
            // in about eleven branches of collidedWithObjectInternal (no early return), and the cube
            // stands on such a block at cy + 30 (a custom level's uid 12500, id 1, snap=12500, which the model
            // lost by dropping the row). Side and underside stay open, as lv22 uid 17425 needs.
            // Only a MOVING one (a recorded timeline): the static two in lv22 (uid 4705, 17425) are the
            // ones GD walks through, and the quick suite moved when they were kept.
            if (noCollide && g_noColOneWay && o.type == 0 && gt && o.uid >= 0 && gt->find(o.uid) != gt->end())
                { o.oneway = 1; noCollide = false; }
            // --nocolstatic: ...and a static one too, as oneway 2 so frameLevel can leave it out of a turned
            // frame, where GD walks through it (constants.hpp)
            if (noCollide && g_noColStatic && o.type == 0) { o.oneway = 2; noCollide = false; }
            if (noCollide) continue;
            const auto tw = envTwin.empty() ? envTwin.end() : envTwin.find(o.uid);
            if (tw == envTwin.end()) {
                emit(Dynamics::NEAR, o);
            } else {
                // Placed by GD's random numbers (envSplit above): the object without its box
                // ticks, then its hazard twin on them. emit reads `gt`, so both go through the
                // split timeline and nothing else does.
                gt = &envSplit;
                emit(Dynamics::NEAR, o);
                Obj h = o;
                h.type = 2;
                h.uid = tw->second;
                h.radius = 0.0;
                h.oneway = 0;
                h.obbOk = 0;
                h.oriented = 0;
                h.slope = 0;
                h.slopeHazard = 0;
                emit(Dynamics::NEAR, h);
                gt = gtAsRecorded;
                ++envTwins;
            }
        }
        // type 3 = InverseGravityPortal. It was MISSING here, so every blue
        // gravity portal in the game was invisible to the model: 109 of them
        // across lv4..lv21, including the one at lv4 x=8775 that produced the
        // "semantics gap" wall at x=9,326. The old code expected both gravity
        // portals to be type 4 and told them apart by objectID (10 vs 11), but
        // GD reports the inverse one as its own GameObjectType, so the
        // `id == 11` branch below was dead code that never once ran.
        // 17 = RegularSize, 18 = MiniSize. Same box-overlap rule as the rest;
        // they only change the player's half extent (see kMiniHalf).
        else if (type == 3 || type == 4 || type == 5 || type == 6 || type == 16
        // 19 = UFO portal. Measured on lv12 (portal box x[6928,6962]): GD
        // switches at x = 6914.28 and not at 6912.98, i.e. left edge - 15 --
        // the same box-overlap rule as every other portal. vy halves too:
        // -2.160 becomes -1.188 = (-2.160 - 0.216) / 2, i.e. the tick's cube
        // gravity applied first and then halved. Nothing special is needed.
        // 23 = dual portal (splits into two players), 24 = solo (back to one)
                 || type == 17 || type == 18 || type == 19
        // 26 = wave portal. lv17 (2), lv18 (3), lv19 (2), lv20 (3), lv21 (2)
        // have them; lv1-16 have none, so nothing already cleared can move.
                 || type == 26
        // 27 = ROBOT portal. It was missing here for a whole session while
        // `wantMode` above already knew about it, so the model happily planned
        // lv19's robot section as a CUBE: measured against GD at t=1,470, the
        // model stepped vy by -0.216 per tick where GD stepped -0.194 (= 0.216
        // x 0.9, the robot's gravity scale), and by t=1,514 it was 6 px low and
        // landing where GD was still falling.
        // The lesson is the same one the ball, the UFO and the wave each taught
        // once: a new mode has to be added in BOTH places, and the frontier
        // print (`cube/ship/ball/ufo=`) does not show the new one, so nothing
        // says out loud that it never fired.
                 || type == 27
        // 33 = SPIDER portal (lv21 x=10,695 and lv22 x=975 are the first two).
                 || type == 33
        // 41 = SWING portal (lv22 x=4,641 / x=9,315; nowhere else in lv1-21,
        // checked with the census -- adding it cannot move a cleared level).
                 || type == 41
        // 28 = TELEPORT portal (lv20 x20, lv21 x2, lv22 x1; none before). It
        // rides in this bucket because the firing rule is the same box overlap
        // every other portal uses -- measured on a full nodeath pass of lv20:
        // all four portals the player's box entered fired, the sixteen it
        // missed did not, nearest miss 3.96 px. What it does is not a mode
        // change though; see Obj::tpY and the teleport block in stepOne.
                 || type == 28
        // 42 = GRAVITY TOGGLE portal (2.2, id 2926), --gravtoggle: reverses gravity whichever way
        // it points. Measured on a custom level (uid 3526): a cube put at rest in it at t=60 rises
        // (vy +2.59 at t=75, +10.15 at t=110) where the same cube on the floor stays. Every surveyed
        // level holding one is uncleared; the model never saw them.
                 || (g_gravToggle && type == 42)
                 || type == 23 || type == 24) {
            // Say which portals will not write the band, and say it with the
            // portal's own numbers next to it -- a silent gate is how a rule
            // ends up "dead from the day it was written".
            if (o.freeMode && bandHeightFor(o.type) > 0.0)
                loadPrintf("freemode: portal uid %d id %d at (%.0f,%.0f) "
                           "carries Free Mode - band stays as it was "
                           "(would have written H=%.0f)\n",
                           o.uid, o.id, o.cx, o.cy, bandHeightFor(o.type));
            emit(Dynamics::PORT, o);   // 16 = ball
        }
        // 9 = pink pad, 12 = pink orb (lv12 onward)
        // 34 = RED pad (see kPadRed). Only lv22 has one, so adding it cannot
        // move a level that already clears.
        else if (type == 8 || type == 9 || type == 10 || type == 34)
            emit(Dynamics::PAD, o);
        // 44 = SPIDER pad (id 3005, 2.2; --spiderpad). collisionCheckObjects sends it to the
        // same block as 8 / 9 / 34 (jump table 0x215fe0 -> 0x2158e6, bumpPlayer at 1.0), so it
        // rides the pad pass; what it does is the type 44 branch there. None in lv1-22.
        else if (type == 44)
            emit(Dynamics::PAD, o);
        // 29 = green ring, 35 = red ring. Neither exists anywhere in lv1-18
        // (checked with scripts/mechanic_census.ps1), so adding them here
        // cannot move a level that already clears. lv22 needs the red one at
        // x=375: it is the only way over the spike carpet that runs from
        // x=435 to x=945, and without it the cold DP dies at x=458.
        // 32 = drop ring (lv21 only, 4 of them)
        // 37 = dash ring, 38 = gravity dash ring (lv21 has 7+2, lv22 3)
        // 43 = SPIDER ORB (GameObjectType::SpiderOrb, id 3004). lv22 has the
        // only two in lv1-22 (x=13,965 and x=14,325, both cy=975), so adding it
        // cannot move a level that already clears. It fires like every other
        // orb (tap while the box overlaps) and teleports like the spider --
        // see the type 43 branch in stepOne.
        // 46 = TELEPORT ORB (id 3027, --teleportorb): fires as a ring, moves the body instead of
        // pushing it (the type 46 branch in applyOrb; modifiers via Obj::tpEx / tpEy / tpKeep).
        else if (type == 11 || type == 12 || type == 13 || type == 29
                 || type == 32 || type == 35 || type == 37 || type == 38
                 || type == 43 || (type == 46 && (o.tpEx != 0.0 || o.tpEy != 0.0))) {
            // Firing box of a rotated ring. The 45-degree dash ring at lv22
            // x=13,333 (1704, 1.2x) is the only non-multiple-of-90 one in the
            // level. A press-tick sweep pins the boundary to one tick
            // (press<=9984 fires / >=9985 does not): neither the AABB nor a
            // fitted circle but the ROTATED BOX -- the local-axis SAT threshold
            // 18+15*sqrt(2)=39.21 is bracketed exactly by the firing side
            // v=36.77 and the non-firing side v=39.88 (measured 2026-08-13).
            // The scale is recovered from the bound: w0/h0 are the raw sprite
            // size, which for this ring is 15 and would drop the firing side
            // (the real size is 18 at 1.2x). The portal's oriented path (above)
            // is a separate thing with its own tuning history, so it is left
            // alone. A rotation by a multiple of 90 does not set oriented and
            // behaves as before.
            const double om = std::fabs(std::fmod(o.rot, 90.0));
            if (om > 0.5 && om < 89.5 && w0 > 1.0 && h0 > 1.0) {
                const double th = o.rot * 3.14159265358979 / 180.0;
                const double cc = std::fabs(std::cos(th));
                const double ssn = std::fabs(std::sin(th));
                const double denom = (w0 * 0.5) * cc + (h0 * 0.5) * ssn;
                if (denom > 1.0) {
                    const double k = o.hw / denom;
                    o.oriented = 1;
                    o.ohw = k * w0 * 0.5;
                    o.ohh = k * h0 * 0.5;
                    const double thc = -th;
                    o.rc = std::cos(thc);
                    o.rs = std::sin(thc);
                }
            }
            emit(Dynamics::ORB, o);  // 13 = gravity
        }
        // Speed portals live under the generic "modifier" type 20 (which is
        // otherwise all triggers), so they are picked out by object id.
        else if (type == 20 && isSpeedId(o.id)) emit(Dynamics::SPEED, o);
        // GAMEPLAY ROTATION (id 2900). Also a "modifier", also picked by id.
        // Not emitted into any collider bucket -- it turns the frame, it does
        // not exist for the player to touch (see RotTrig).
        else if (type == 20 && o.id == 2900) {
            // [2026-08-15 revised] 2900 is not a "rotation": it SETS THE
            // DIRECTION AS AN ABSOLUTE VALUE. gnddir = the new travel direction
            // (1=up 2=down 3=left 4=right). rot alone cannot tell a +X-travel
            // one from a -X-travel one (= reverse) at the same orot=0, and the
            // code relied on a one-sample guess of "same frame means toggle".
            // Confirmed to agree on all 20 firings in lv22.
            //   4 -> frame 0 / rev 0     3 -> frame 0 / rev 1
            //   2 -> frame 1             1 -> frame 3
            const int gd = f[27].empty() ? 0 : std::atoi(f[27].c_str());
            int fr = ((int)std::lround(o.rot / 90.0)) & 3;
            int rv = -1;   // -1 = column absent (old export); follow rot
            switch (gd) {
                case 4: fr = 0; rv = 0; break;
                case 3: fr = 0; rv = 1; break;
                case 2: fr = 1; rv = 0; break;
                case 1: fr = 3; rv = 0; break;
                default: break;
            }
            // mvdir = the new GRAVITY DIRECTION (1=up 2=down 3=left 4=right).
            // It has been in the export since 2026-08-15 but was never read.
            // The model's flip means "local -v is down", so it is 1 only when
            // mvdir points along that frame's local +v. From fromFrame:
            //   frame 0: +v = +Y (up=1)     frame 1: +v = +X (right=4)
            //   frame 2: +v = -Y (down=2)   frame 3: +v = -X (left=3)
            static const int kUpFor[4] = {1, 4, 2, 3};
            const int mv = f[26].empty() ? 0 : std::atoi(f[26].c_str());
            RotTrig rt{o.cx, o.cy, fr};
            rt.setRev = rv;
            if (mv >= 1 && mv <= 4) rt.setFlip = (mv == kUpFor[fr & 3]) ? 1 : 0;
            rt.uid = o.uid;
            // Velocity change (editvel=169 / vmody=583 / ovrvel=584)
            // [2026-08-19]. The MOD dumps the member values after parsing, so
            // the default 0.0 when 583 is absent is also GD's own value as is.
            // The effective multiplier is folded in here (the applyRotation
            // side only multiplies by vmodY).
            const int ev = f[37].empty() ? 0 : std::atoi(f[37].c_str());
            if (ev)
                rt.vmodY = f[39].empty() ? 0.0f : (float)std::atof(f[39].c_str());
            rt.ovrVel = f[40].empty() ? 0 : (uint8_t)std::atoi(f[40].c_str());
            rt.dontSlide = (colDontSlide >= 0 && colDontSlide < kObjFields
                            && !f[colDontSlide].empty())
                               ? (uint8_t)std::atoi(f[colDontSlide].c_str()) : 0;
            // The raw inputs the QUEUE's sort order needs. `fr` above has
            // already been overridden from gnddir, and the sort reads the
            // rotation, so the raw values have to be carried separately.
            rt.rawRot = o.rot;
            rt.flipX = (uint8_t)(f[30].empty() ? 0 : std::atoi(f[30].c_str()));
            rt.gndDir = gd;
            rt.touch = (uint8_t)(colTouch >= 0 && colTouch < kObjFields && f[colTouch] == "1");
            rt.hw = o.hw;
            rt.hh = o.hh;
            g_rotTrig.push_back(rt);
        }
        // [correction 2026-08-18] id 2899 is NOT REVERSE -- it is an Options
        // trigger. It is GD's GameOptionsTrigger, and all 10 in lv22 touch
        // m_disableP1Controls (objrects' optp1: On=1 / Off=-1 / unchanged=0).
        // Reverse itself is handled by a SAME-FRAME 2900 (RotTrig::setRev), and
        // the g_revTrig that used to be pushed here was never read (= wrong,
        // but never visible in behaviour). For the effect and the window see
        // g_ctrlWin / ctrlOffAt. The firing gate is unresolved, so nothing is
        // pushed here.
        else if (type == 25 && !f[10].empty()) {
            o.slope = 1;
            o.sy0 = std::atof(f[10].c_str());
            o.sy1 = std::atof(f[11].c_str());
            o.slopeHazard = f[12].empty() ? 0 : (uint8_t)std::atoi(f[12].c_str());
            o.slopeDir = f[13].empty() ? 0 : (uint8_t)std::atoi(f[13].c_str());
            // --nanslope (constants.hpp): a ramp with no line (a scale-0 object: w = h = 0 and the mod's sy0 / sy1
            // read "nan") is not a ramp; left in, its m is 0/0 and every bound test against it passes.
            if (!std::isfinite(o.sy0) || !std::isfinite(o.sy1) || !(o.hw > 0.0)) {
                ++g_nanSlopeDropped;
                continue;
            }
            emit(Dynamics::SLOPE, o);
        }
        // FLIP-ON-HEAD-HIT (id 2866): a modifier, picked by id like 2900 above.
        // Not a collider, so it is not emitted into any bucket.
        // CAMERA ZOOM (id 1913): the invisible ceiling is 270 / zoom.
        else if (o.id == 1913) {
            const double zm = f[22].empty() ? 0.0 : std::atof(f[22].c_str());
            // ...but only if GD would ever fire it on an x crossing. Exactly
            // two of lv22's twenty never do, and they are the whole reason the
            // model's camera scale read 1.0 where GD held 0.6 through the ship
            // and wave sections:
            //
            //   x=16,065  spawn-triggered (property 62). PlayLayer::addObject
            //             admits a trigger to the crossing queue only when it
            //             is neither touch- nor spawn-triggered; the third arm
            //             (isSpecialSpawnObject) is `xor al,al; ret` in all 37
            //             trigger vtables, so nothing rescues one.
            //   x=21,795  rotate channel 15 (property 170). It IS admitted,
            //             but the queue is bucketed by channel and it is only
            //             consumed while that channel is up.
            //
            // The channel one is an approximation and it is worth being exact
            // about which way it errs. Whether a channel is raised is a fact
            // about the route, not about the level, and this model has no
            // channel state at all -- so the only two representable answers
            // are "always" and "never". Every measurement in hand says never
            // (GD's camscale holds 0.6 across the whole section in the 21,140
            // tick reference run). The two errors are also not symmetric: not
            // firing leaves the band TALLER than GD's would be, which lets the
            // model plan through a ceiling GD enforces -- and the repair loop
            // catches exactly that, by replaying and re-anchoring. Firing it
            // leaves the band SHORTER, which hides routes GD allows, and an
            // over-kill of that kind is invisible to the loop.
            const char* dropped = nullptr;
            if (g_trigGateCol && (f[colTouch] == "1" || f[colSpawn] == "1"))
                dropped = "touch/spawn-triggered";
            else if (colChan >= 0 && !f[colChan].empty()
                     && std::atoi(f[colChan].c_str()) != 0)
                dropped = "on a rotate channel";
            if (dropped)
                loadPrintf("zoom: uid %d at x=%.0f is %s - never fires on an "
                           "x crossing, IGNORED\n", o.uid, o.cx, dropped);
            if (zm > 0.0 && !dropped) {
                g_zoomTrigs.push_back({o.cx, zm,
                                       (f[23].empty() ? 0.0
                                          : std::atof(f[23].c_str())) * 240.0,
                                       f[25].empty() ? 2.0
                                          : std::atof(f[25].c_str()),
                                       f[24].empty() ? 0
                                          : std::atoi(f[24].c_str())});
                loadPrintf("zoom: uid %d at x=%.0f -> %.6f over %.2fs\n",
                           o.uid, o.cx, zm,
                           f[23].empty() ? 0.0 : std::atof(f[23].c_str()));
            }
        }
        // STATIC CAMERA (id 1914): the one thing that opens branch A of
        // getMin/MaxPortalY. Only the axes that include Y matter here
        // (property 101: 0 both / 1 X only / 2 Y only), and property 110 turns
        // it back off. Same admission gate as any other x-crossing trigger.
        else if (o.id == 1914 && g_staticCamCol) {
            const int axis = f[colAxis].empty() ? 0
                                                : std::atoi(f[colAxis].c_str());
            const uint8_t ex = (uint8_t)(f[colExStat] == "1" ? 1 : 0);
            // Touch- and spawn-triggered rows are not on the crossing queue at
            // all, so they never fire on an x crossing whatever else is true.
            bool queued = !(g_trigGateCol
                            && (f[colTouch] == "1" || f[colSpawn] == "1"));
            // The CHANNEL gate goes on the EXIT rows only, and the reason is
            // the direction each mistake errs in -- which for a Static Camera
            // is the OPPOSITE of the zoom trigger above.
            //
            // Firing an `on` row opens branch A, where the numerator is
            // 322 - (322 - H) * p and therefore lies in [H, 322]: it can never
            // come out BELOW the H that branch B would have used. So firing
            // one the game did not can only leave the band the same or taller
            // -- the loose direction, which the repair loop sees by replaying.
            // NOT firing one the game did fires leaves the band shorter, and
            // an over-kill of that kind is invisible to the loop
            // (gd-overkill-is-invisible-to-the-loop). An `exit` row is the
            // mirror: firing it returns the level to branch B, the shorter and
            // therefore the unseen side, so the gate stays there.
            //
            // This is chosen on the direction of the error and NOT on evidence
            // that GD fires a channelled trigger: lv22's own run does not
            // settle it (the band does drop out of branch A around x=20,115
            // with nothing in the model to explain it -- logged as a separate
            // hole). If evidence later says GD never fires them, this is one
            // line to put back.
            if (ex && colChan >= 0 && !f[colChan].empty()
                && std::atoi(f[colChan].c_str()) != 0)
                queued = false;
            if (axis != 1 && queued) {
                g_staticCams.push_back({o.cx, ex});
                loadPrintf("staticcam: uid %d at x=%.0f axis=%d %s\n",
                           o.uid, o.cx, axis, ex ? "EXIT" : "on");
            }
        }
        // TIME WARP (id 1935): the `tw` column carries m_timeWarpTimeMod.
        else if (o.id == 1935) {
            const double tw = f[21].empty() ? 0.0 : std::atof(f[21].c_str());
            if (tw <= 0.0) {
                loadPrintf("timewarp: uid %d at x=%.0f has NO tw column - "
                           "objrects is older than the 2026-08-14 dump, "
                           "IGNORED\n", o.uid, o.cx);
            } else if (g_trigGateCol
                       && (f[colTouch] == "1" || f[colSpawn] == "1")) {
                // --warpqueue (modifiers.hpp)
                loadPrintf("timewarp: uid %d at x=%.0f is touch/spawn-triggered, not on "
                           "the crossing queue - NOT modelled\n", o.uid, o.cx);
            } else {
                g_timeWarps.push_back({o.cx, tw});
                loadPrintf("timewarp: uid %d at x=%.0f mod=%.4f\n",
                           o.uid, o.cx, tw);
            }
        }
        // TELEPORT TRIGGER (id 3022, --teleporttrig; modifiers.hpp TpTrig). Its target is in the
        // teleport columns (tpex/tpey, tpg, tpix/tpiy), written for it as for a type 28 portal.
        // Only what the model can say is loaded: a trigger on the x-crossing queue, whose target
        // the dump names, and which moves x by no more than a tick's overshoot (a target straight
        // above or below, or x kept) -- x is the search's clock, so a sideways jump stays out
        // (--tpsideways lets one ahead in through: constants.hpp).
        else if (o.id == 3022 && g_teleportTrig) {
            const double tx = f[34].empty() ? 0.0 : std::atof(f[34].c_str());
            const double ty = f[35].empty() ? 0.0 : std::atof(f[35].c_str());
            const uint8_t igx = (uint8_t)(!f[19].empty() && std::atoi(f[19].c_str()) ? 1 : 0);
            const uint8_t igy = (uint8_t)(!f[20].empty() && std::atoi(f[20].c_str()) ? 1 : 0);
            const uint8_t grav = (uint8_t)(f[18].empty() ? 0 : std::atoi(f[18].c_str()));
            const char* dropped = nullptr;
            if (g_trigGateCol && (f[colTouch] == "1" || f[colSpawn] == "1"))
                dropped = "touch/spawn-triggered, not on the crossing queue";
            else if (tx == 0.0 && ty == 0.0)
                dropped = "without a target in the dump (no tpex/tpey)";
            else if (!igx && std::fabs(tx - o.cx) > 0.5 && !(tx > o.cx)
                     && !(g_tpBack && tx < o.cx))   // --tpback (modifiers.hpp)
                dropped = "moving x backwards";
            tpExitMovesOf(o.uid, tx, ty);   // --tpexitmove
            if (dropped) {
                // --tptrigspawn (modifiers.hpp): a spawned one with a target goes to cli.hpp's chain walk
                const bool spawned = g_trigGateCol && f[colSpawn] == "1" && f[colTouch] != "1"
                                     && (tx != 0.0 || ty != 0.0);
                if (spawned) g_tpSpawned.push_back({o.cx, tx, ty, igx, igy, grav, o.uid, tpForceAt(f, tx, ty)});
                // --tptouch (modifiers.hpp): a touched one moves the state whose box overlaps it, unless its
                // target is behind the box
                const bool touched = g_trigGateCol && f[colTouch] == "1" && (tx != 0.0 || ty != 0.0)
                                     && (igx || g_tpBack || tx >= o.cx - o.hw - 0.5);
                if (touched)
                    g_tpTouchBoxes.push_back({o.cx, o.cy, o.hw, o.hh,
                                              TpTrig{o.cx, tx, ty, igx, igy, grav, o.uid, tpForceAt(f, tx, ty)}, o.uid});
                loadPrintf("teleporttrig: uid %d at x=%.0f is %s - NOT modelled%s\n",
                           o.uid, o.cx, dropped,
                           spawned ? " (a crossing chain may fire it: --tptrigspawn)"
                           : touched ? " (a state that touches it is moved: --tptouch)" : "");
            } else {
                g_tpTrigs.push_back({o.cx, tx, ty, igx, igy, grav, o.uid, tpForceAt(f, tx, ty)});
                loadPrintf("teleporttrig: uid %d at x=%.0f -> (%.1f, %.1f)%s%s grav %d\n",
                           o.uid, o.cx, tx, ty, igx ? " keepX" : "", igy ? " keepY" : "",
                           (int)grav);
            }
        }
        // GRAVITY TRIGGER (id 2066, --gravtrig; modifiers.hpp GravTrig). The value and the
        // players it writes come in the objrects `gmod` / `gpl` columns, by name. Loaded: a
        // trigger on the x-crossing queue that writes player 1.
        else if (o.id == 2066) {
            const char* dropped = nullptr;
            const double gm = (colGMod >= 0 && !f[colGMod].empty())
                                  ? std::atof(f[colGMod].c_str()) : 0.0;
            const int gpl = (colGPl >= 0 && !f[colGPl].empty())
                                ? std::atoi(f[colGPl].c_str()) : 0;
            if (colGMod < 0)
                dropped = "in a dump without the gmod column";
            else if (g_trigGateCol && (f[colTouch] == "1" || f[colSpawn] == "1"))
                dropped = "touch/spawn-triggered, not on the crossing queue";
            else if (gpl & 2)
                dropped = "for player 2 only";
            if (dropped) {
                // --gravspawn (modifiers.hpp): a spawned one goes to cli.hpp's Spawn lookup
                const bool spawned = g_trigGateCol && colGMod >= 0 && f[colSpawn] == "1"
                                     && f[colTouch] != "1" && !(gpl & 2);
                if (spawned) g_gravSpawned.push_back({gm, o.uid, o.cx});
                // --gravtouch (modifiers.hpp): a touched one is fired by the state whose box overlaps it
                const bool touched = g_trigGateCol && colGMod >= 0 && f[colTouch] == "1" && !(gpl & 2);
                if (touched) g_gravTouchBoxes.push_back({o.cx, o.cy, o.hw, o.hh, gm, o.uid});
                loadPrintf("gravtrig: uid %d at x=%.0f is %s - NOT modelled%s\n",
                           o.uid, o.cx, dropped,
                           spawned ? " (a crossing Spawn may fire it: --gravspawn)"
                           : touched ? " (a state that touches it fires it: --gravtouch)" : "");
            } else {
                g_gravTrigs.push_back({o.cx, gm, o.uid});
                loadPrintf("gravtrig: uid %d at x=%.0f gravity x%.3f%s\n", o.uid, o.cx, gm,
                           (gpl & 4) ? " (the player that crosses it)" : "");
            }
        }
        // CAMERA MODE (id 2925, --camfree; bands.hpp): its Free Mode on the crossing queue
        else if (o.id == 2925 && g_camFree && g_freeModeCol) {
            if (g_trigGateCol && (f[colTouch] == "1" || f[colSpawn] == "1"))
                loadPrintf("camfree: uid %d at x=%.0f is touch/spawn-triggered - NOT modelled\n", o.uid, o.cx);
            else {
                g_camFrees.push_back({o.cx, o.freeMode, o.uid});
                loadPrintf("camfree: uid %d at x=%.0f free mode %d\n", o.uid, o.cx, (int)o.freeMode);
            }
        }
        else if (o.id == 2866) {
            // ...and the box's own motion, for --fgarmlive. Taken here because
            // this is where the timeline is in scope; the flag decides at run
            // time whether the arm reads it or the parked position above.
            FlipHeadBox fh{o.cx, o.cy, o.hw, o.hh, o.uid, {}};
            if (gt && o.uid >= 0) {
                const auto fit = gt->find(o.uid);
                if (fit != gt->end()) {
                    fh.live.reserve(fit->second.size());
                    for (const auto& r : fit->second)
                        fh.live.push_back({r.t, r.cx, r.cy});
                }
            }
            const size_t nrows = fh.live.size();
            rideWindow(o, fh.live, fh.rides, fh.rideT0, fh.rideT1, "fliphead");
            g_flipHeadBoxes.push_back(std::move(fh));
            loadPrintf("fliphead: uid %d at (%.0f,%.0f) %.1fx%.1f "
                       "(%zu recorded rows)\n",
                       o.uid, o.cx, o.cy, o.hw * 2.0, o.hh * 2.0, nrows);
        }
        else if (o.id == 1829) {
            g_dashStopBoxes.push_back({o.cx, o.cy, o.hw, o.hh});
            loadPrintf("dashstop: uid %d at (%.0f,%.0f) %.1fx%.1f\n",
                       o.uid, o.cx, o.cy, o.hw * 2.0, o.hh * 2.0);
        }
        // CEILING ARM (id 1859): what lets a cube family player answer a
        // ceiling with a bonk instead of dying on it (see armBoxTouch).
        else if (o.id == 1859) {
            // ...and its recorded motion, as the 2866 above: an 1859 can ride
            // the player (lv22's switch band; see armBoxTouch).
            ArmBox ab{o.cx, o.cy, o.hw, o.hh, o.uid, {}};
            if (gt && o.uid >= 0) {
                const auto fit = gt->find(o.uid);
                if (fit != gt->end()) {
                    ab.live.reserve(fit->second.size());
                    for (const auto& r : fit->second)
                        ab.live.push_back({r.t, r.cx, r.cy});
                }
            }
            const size_t nrows = ab.live.size();
            rideWindow(o, ab.live, ab.rides, ab.rideT0, ab.rideT1, "ceilarm");
            g_armBoxes.push_back(std::move(ab));
            loadPrintf("ceilarm: uid %d at (%.0f,%.0f) %.1fx%.1f "
                       "(%zu recorded rows)\n",
                       o.uid, o.cx, o.cy, o.hw * 2.0, o.hh * 2.0, nrows);
        }
        // DART SLIDE ARM (id 1755): what lets a WAVE stand on a solid at all
        // (see slideBoxTouch). The box is the scaled one, which the dump
        // already carries in w,h -- the raw 30x30 does not reach the player on
        // the arming tick.
        else if (o.id == 1755) {
            // ...with its recorded motion, for --slidelive (taken here, where the timeline is in
            // scope; the flag decides at run time, as --fgarmlive does for the 2866).
            SlideBox sb{o.cx, o.cy, o.hw, o.hh, o.uid, {}, {}};
            if (gt && o.uid >= 0) {
                const auto fit = gt->find(o.uid);
                if (fit != gt->end()) {
                    sb.live.reserve(fit->second.size());
                    for (const auto& r : fit->second) {
                        sb.live.push_back({r.t, r.cx, r.cy});
                        sb.liveOn.push_back(r.on);   // --slideon
                    }
                }
            }
            loadPrintf("dartslide: uid %d at (%.0f,%.0f) %.1fx%.1f (%zu recorded rows)\n",
                       o.uid, o.cx, o.cy, o.hw * 2.0, o.hh * 2.0, sb.live.size());
            g_slideBoxes.push_back(std::move(sb));
        }
        // FORCE FIELD (id 3645): a circular pusher, not a collider -- see
        // forceFieldAcc at the top. Not stored in L; stepOne reads the global.
        // The radius block above has already scaled o.radius by w/w0.
        else if (o.id == 3645) {
            double rm = std::fmod(std::fabs(o.rot), 360.0);
            if (rm > 180.0) rm = 360.0 - rm;
            if (std::fabs(rm - 90.0) < 45.0)
                loadPrintf("forcefield: uid %d at (%.0f,%.0f) rot %.0f is "
                           "SIDEWAYS - not measured, object IGNORED\n",
                           o.uid, o.cx, o.cy, o.rot);
            else {
                ForceField ff{o.cx, o.cy, o.radius, (rm >= 90.0) ? -1 : +1};
                ff.uid = o.uid;
                ff.force = f[41].empty() ? 0.0 : std::atof(f[41].c_str());   // --forcecircle
                ff.dirY = (o.flipY ? -1.0 : 1.0) * std::cos(o.rot * 3.14159265358979 / 180.0);
                // --forcelive: its recorded motion, taken here where the timeline is in scope (as the
                // 1755's for --slidelive); the flag decides at run time
                if (gt && o.uid >= 0) {
                    const auto fit = gt->find(o.uid);
                    if (fit != gt->end()) {
                        for (const auto& r : fit->second) {
                            ff.liveT.push_back((int)r.t);
                            ff.liveX.push_back((float)r.cx);
                            ff.liveY.push_back((float)r.cy);
                            ff.liveOn.push_back(r.on);
                        }
                    }
                }
                g_forceFields.push_back(ff);
                loadPrintf("forcefield: uid %d at (%.0f,%.0f) R=%.1f push %s\n",
                           o.uid, o.cx, o.cy, o.radius,
                           (rm >= 90.0) ? "down" : "up (UNMEASURED mirror)");
            }
        }
        // FORCE BOX (id 2069): the AABB version of 3645, a flat push (measured
        // at kFF2069's declaration). All 13 in lv22 are rot=0 (upward), so the
        // orientation is not read. The strength is per-instance (the table at
        // kFF2069's declaration).
        else if (o.id == 2069) {
            // [2026-08-30] THE TABLE BELOW IS GONE. It is one line now:
            //     dvy = m_force * |g(mode)| / kForceGDiv
            // m_force is the strength the level's author set on the instance
            // (ForceBlockGameObject, dumped into objrects' `force` column); the
            // rest is GD applying the same dt and mode gravity scale to the
            // force that it applies to gravity, so kForceGDiv = 0.9581990 is
            // the constant already at kRobotHoverTicks, not a fitted one.
            //
            // The note at kFF2069 PROPOSED THIS LAW AND REJECTED IT, because
            // dividing the measurements by the mode's gravity gave coefficients
            // over 1.25..1.69. That division left out m_force, which nothing
            // could read until the exporter emitted it. With it:
            //     11412  0.283 / (1.4  * 0.194) = 1.042
            //     14472  0.304 / (1.5  * 0.194) = 1.045
            //     17701  0.330 / (1.63 * 0.194) = 1.043
            //     carpet 0.108 / (1.2  * 0.086) = 1.047      = 1/0.9581990
            // The 1.25..1.69 scatter WAS m_force.
            //
            // Independently measured on calib_forcedrop_<mode> (all 8 modes,
            // the player walks into a scale-10 box, gravity separated in the
            // same run): the law predicts every mode to the 0.001 grid, and
            // wave -- no gravity, so the law says no response -- reads exactly
            // zero inside the box as well as outside.
            //
            // TWO OUTLIERS, both in modes whose gravity is velocity-dependent:
            //   - uid17701's SHIP reading (0.172) wants 1.53, not 1.043. Its own
            //     note records a "dvy switch at vy~+2.1" for the ship, so the
            //     bare g it was decomposed against (-0.069) is not necessarily
            //     the g that was acting; that is the same mis-decomposition the
            //     0.433 entry was withdrawn for. Not resolved -- the law is
            //     applied and the regression is what says whether it was right.
            //   - the UFO rig reads 0.087 against a predicted 0.0898.
            // OPEN, unchanged from before: mini and speed both move the mode's
            // gravity (kSwingGMini) and gMagForMode does not carry either.
            // An objrects dump written before the column exists leaves this
            // empty, and a force block silently pushing with 0 is worse than a
            // wrong constant. Say so and fall back to the old default.
            double force = 0.0;
            if (f[41].empty()) {
                force = kFF2069 * kForceGDiv / std::fabs(kCubeG);
                loadPrintf("forcebox: uid %d has NO force column (old objrects "
                           "dump) - refresh objrects\n", o.uid);
            } else {
                force = std::atof(f[41].c_str());
            }
            g_forceBoxes.push_back({o.cx, o.cy, o.hw, o.hh, force, o.uid, 0});
            // --forceboxdir (default off): the push points where the box does.
            // ForceBlockGameObject::calculateForceToTarget (0x4c1ec0), outside the
            // target mode (+0x74c): angle = (isFlipY ? 180 : 0) + 90 - getRotation()
            // in degrees, and the force is ccpForAngle(angle) times the strength --
            // so a box rotated 180 pushes DOWN. Only the y share is modelled; a box
            // turned off the vertical also pushes in x, which is said, not modelled.
            // A custom level has one at x=3,315 under a UFO, rot -180, m_force 0.1,
            // that the model pushed up.
            {
                const double dirY = (o.flipY ? -1.0 : 1.0)
                                    * std::cos(o.rot * 3.14159265358979 / 180.0);
                g_forceBoxes.back().dirY = dirY;
                if (std::fabs(dirY) < 0.999)
                    loadPrintf("forcebox: uid %d is turned %.1f deg - only the y share %.3f "
                               "of its push is modelled\n", o.uid, o.rot, dirY);
                else if (dirY < 0.0)
                    loadPrintf("forcebox: uid %d points down (rot %.1f, flipY %d)\n",
                               o.uid, o.rot, (int)o.flipY);
            }
            // The push this box gives a full-size cube / robot / swing at 1x.
            // THROUGH forceUnitFor, not through a second copy of the arithmetic:
            // the first cut of this line used the quantised gravities (0.194 for
            // the robot) and printed 0.303 where the model runs 0.304, which is
            // a diagnostic that disagrees with the thing it is describing.
            loadPrintf("forcebox: uid %d at (%.0f,%.0f) %gx%g m_force=%.3f "
                       "(cube %.3f / robot %.3f / swing %.3f)\n",
                       o.uid, o.cx, o.cy, 2 * o.hw, 2 * o.hh, force,
                       qVy(force * forceUnitFor(0, 1.29825f).v),
                       qVy(force * forceUnitFor(5, 1.29825f).v),
                       qVy(force * forceUnitFor(7, 1.29825f).v));
        }
    }
    if (!envTwin.empty()) {
        envOther = (int)envTwin.size() - envTwins;
        loadPrintf("groups: %d objects placed by GD's random numbers -> hazard twins%s\n",
                   envTwins,
                   envOther ? " (and some that are not solids or hazards: rows kept as "
                               "recorded, NOT treated as deadly)" : "");
    }
    auto byX = [](const Obj& a, const Obj& b) { return a.cx < b.cx; };
    std::sort(L.objs.begin(), L.objs.end(), byX);
    std::sort(L.portals.begin(), L.portals.end(), byX);
    // ...and number the gravity portals, AFTER the sort so the ordinal is a
    // property of the dump rather than of the load order. See Obj::gpBit for
    // what the bit means and how it was measured.
    {
        // BOTH gravity portals. type 3 is InverseGravityPortal (the blue one),
        // type 4 the normal one, and GD's hasBeenActivated / ...ByPlayer are
        // GameObject's own flags -- nothing about them is per portal kind. The
        // corpus has no witnessed type 3 double pass, though: lv16 uid 3450
        // looked like one and is two HALVES of a dual, one pass each (GD's own
        // record: p2 at t=8,014, p1 at t=8,061). See the note at the latch.
        // The budget is now tight. Counting both types, lv20 holds exactly 32
        // (12 + 20) -- the cap with nothing to spare, so the next level over the
        // line stops here instead of running.
        // ...AND THIS LOOP DOES NOT SEE EVERY GRAVITY PORTAL. StepCtx::ports is
        // built from two sources -- the frame's static index, which comes from
        // L.portals, and Dynamics::PORT -- while the numbering walks L.portals
        // alone. So a gravity portal carried by moving geometry gets gpBit -1
        // and can never be spent. Measured on lv22 uid 1158 (id 10, type 4,
        // cx 2085, rotating: prot 84.808 -> 93.462): GD activates it at
        // t=1,595, dp's own portgate prints it as type=4 from t=1,586, and it
        // holds no bit. Seven of that level's eight are numbered, and those
        // seven latch on the tick GD activates them.
        // The fix is not local. The ordinal is "this level's gravity portals in
        // cx order", which the DUMP can answer at load time even for the ones
        // the dynamic set will own -- but rebuilding the numbering around the
        // dump rather than around L.portals is a change to make when
        // State::portalLatch is widened, not before, because the two share the
        // seeding discipline that a size change re-opens.
        // [2026-09-22] 32 -> kGravPortalBits (128), and past it the level is
        // reported unsupported instead of std::exit(2). In the mod that exit ran on
        // the solver's detached thread and took the game down with it (0xC0000409
        // in abort, three custom levels with 43, 68 and 72, and a synthetic level
        // with 33 against one with 32), leaving no line anywhere to say why.
        int nGrav = 0, nGravAll = 0;
        for (Obj& p : L.portals)
            if (p.type == 3 || p.type == 4 || (g_gravToggle && p.type == 42)) {
                ++nGravAll;
                if (nGrav < kGravPortalBits) p.gpBit = (int8_t)nGrav++;
            }
        // Past the width, share bits (prelude.hpp, g_gpHandoff; the list is L.gpHandoff): portal
        // i + kGravPortalBits takes portal i's bit, handed over halfway between them. Only on a
        // level where x never runs
        // back (no object carries a reversal), and only when every pair is far enough apart that
        // no body can overlap both sides of the hand-over point (kGpShareGap); otherwise refuse.
        if (nGravAll > kGravPortalBits) {
            constexpr double kGpShareGap = 120.0;
            bool anyRev = false;
            for (const Obj& o : L.objs) anyRev = anyRev || o.rev;
            for (const Obj& o : L.portals) anyRev = anyRev || o.rev;
            for (const Obj& o : L.orbs) anyRev = anyRev || o.rev;
            for (const Obj& o : L.pads) anyRev = anyRev || o.rev;
            std::vector<Obj*> grav;
            for (Obj& p : L.portals)
                if (p.type == 3 || p.type == 4 || (g_gravToggle && p.type == 42)) grav.push_back(&p);
            bool ok = !anyRev;
            for (size_t j = kGravPortalBits; ok && j < grav.size(); ++j) {
                const Obj& a = *grav[j - kGravPortalBits];
                const Obj& b = *grav[j];
                const double lo = a.cx + a.hw, hi = b.cx - b.hw;
                if (hi - lo < kGpShareGap) ok = false;
            }
            if (ok) {
                for (size_t j = kGravPortalBits; j < grav.size(); ++j) {
                    const Obj& a = *grav[j - kGravPortalBits];
                    Obj& b = *grav[j];
                    b.gpBit = a.gpBit;
                    L.gpHandoff.push_back(GpHandoff{((a.cx + a.hw) + (b.cx - b.hw)) / 2.0,
                                                    (int)a.gpBit});
                }
            } else {
                L.unsupported = "gravity portals: " + std::to_string(nGravAll)
                                + " (limit " + std::to_string(kGravPortalBits)
                                + (anyRev ? ", reversal" : ", too close to share bits") + ")";
            }
        }
        // --tplatch: the teleport portals (type 28) take the bits after the static gravity
        // portals', static ones first and then the ones in dyn (a grouped teleport is routed there
        // and L.portals never holds it). Not on a level already sharing bits, and none past the width.
        if (nGravAll <= kGravPortalBits) {
            int nTp = 0, nTpAll = 0;
            const int first = nGrav;
            for (Obj& p : L.portals)
                if (p.type == 28) {
                    ++nTpAll;
                    if (nGrav < kGravPortalBits) { p.gpBit = (int8_t)nGrav++; ++nTp; }
                }
            for (size_t i = 0; i < L.dyn.objs.size(); ++i)
                if (L.dyn.bucket[i] == Dynamics::PORT && L.dyn.objs[i].type == 28) {
                    ++nTpAll;
                    if (nGrav < kGravPortalBits) { L.dyn.objs[i].gpBit = (int8_t)nGrav++; ++nTp; }
                }
            loadPrintf("tplatch: %d of %d teleport portals latched (bits %d..%d)\n", nTp, nTpAll,
                       first, nGrav - 1);
        }
        // --dyngplatch (constants.hpp): ...and the gravity portals moving geometry carries, with
        // the bits left after the teleports'. (Numbered before them, they took all 128 on a custom
        // level with 68 static and many grouped gravity portals, and its 25 teleports fell back to
        // the position stand-in -- 25 of 25 latched without the flag, 0 of 25 with it.) Not counted
        // in nGravAll, so the shared-bit hand-over above, which pairs static portals by cx, is not
        // offered them.
        if (nGravAll <= kGravPortalBits) {
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                Obj& p = L.dyn.objs[i];
                if (L.dyn.bucket[i] == Dynamics::PORT
                    && (p.type == 3 || p.type == 4 || (g_gravToggle && p.type == 42))
                    && nGrav < kGravPortalBits) {
                    p.gpBit = (int8_t)nGrav++;
                    if (g_slopeDbg)
                        loadPrintf("gpbit bit=%d uid=%d type=%d cx=%.1f (dyn)\n",
                                   (int)p.gpBit, p.uid, (int)p.type, p.cx);
                }
            }
        }
        // --slopedbg: the uid -> bit map. Nothing else can report it, and
        // without it a portalLatch mask is unreadable from outside: rebuilding
        // the order by hand from the dump's type 3/4 rows sorted by cx gave a
        // mapping that was off by one against GD's own activation ticks, and
        // the mask then looked like a detection bug that was not there.
        // The payload (--anchor-state portal=) is written in uids for the same
        // reason -- the ordinal is this build's, the uid is the level's.
        if (g_slopeDbg)
            for (const Obj& p : L.portals)
                if (p.gpBit >= 0)
                    loadPrintf("gpbit bit=%d uid=%d type=%d cx=%.1f\n",
                               (int)p.gpBit, p.uid, (int)p.type, p.cx);
    }
    std::sort(L.pads.begin(), L.pads.end(), byX);
    std::sort(L.orbs.begin(), L.orbs.end(), byX);
    std::sort(L.speeds.begin(), L.speeds.end(), byX);
    std::sort(L.slopes.begin(), L.slopes.end(), byX);
    // x, then y: --coinmask is indexed by this order and the mod builds its mask
    // from its own coin list sorted the same way (solver.hpp buildPois).
    std::sort(L.coins.begin(), L.coins.end(), [](const Obj& a, const Obj& b) {
        return a.cx < b.cx || (a.cx == b.cx && a.cy < b.cy);
    });
    // ...and, now that the numbering is fixed, which dyn row each coin is (by
    // uid -- dyn is never sorted, so the index stays valid, and the rotated
    // frames' copies are built from this one position for position).
    L.coinDyn.assign(L.coins.size(), -1);
    for (size_t ci = 0; ci < L.coins.size(); ++ci)
        for (size_t di = 0; di < L.dyn.objs.size(); ++di)
            if (L.dyn.objs[di].uid == L.coins[ci].uid && L.coins[ci].uid >= 0) {
                L.coinDyn[ci] = (int)di;
                break;
            }
    std::sort(g_timeWarps.begin(), g_timeWarps.end(),
              [](const TimeWarp& a, const TimeWarp& b) { return a.cx < b.cx; });
    std::sort(g_tpTrigs.begin(), g_tpTrigs.end(),
              [](const TpTrig& a, const TpTrig& b) { return a.cx < b.cx; });
    // --stopjump: State::pcTouch is a byte (index + 1); the crossing list is walked in x order
    std::sort(g_pcTrigs.begin(), g_pcTrigs.end(),
              [](const PcTrig& a, const PcTrig& b) { return a.cx < b.cx || (a.cx == b.cx && a.uid < b.uid); });
    if (g_pcTouchBoxes.size() > 254) {
        loadPrintf("stopjump: %zu touch-started, the last %zu (from x=%.0f) are NOT modelled\n", g_pcTouchBoxes.size(),
                   g_pcTouchBoxes.size() - 254, g_pcTouchBoxes[254].cx);
        g_pcTouchBoxes.resize(254);
    }
    std::sort(g_revTrigs.begin(), g_revTrigs.end(),
              [](const RevTrig& a, const RevTrig& b) { return a.cx < b.cx || (a.cx == b.cx && a.uid < b.uid); });
    // --gravtouch: State::gravTouch is a byte (index + 1), and one more is kept for an anchor's seed
    std::sort(g_gravTouchBoxes.begin(), g_gravTouchBoxes.end(),
              [](const GravTouchBox& a, const GravTouchBox& b) { return a.cx < b.cx; });
    if (g_gravTouchBoxes.size() > 253) {
        loadPrintf("gravtrig: %zu touch-triggered, the last %zu (from x=%.0f) are NOT modelled\n",
                   g_gravTouchBoxes.size(), g_gravTouchBoxes.size() - 253, g_gravTouchBoxes[253].cx);
        g_gravTouchBoxes.resize(253);
    }
    // --tptouch: State::tpTouch is a byte too
    std::sort(g_tpTouchBoxes.begin(), g_tpTouchBoxes.end(),
              [](const TpTouchBox& a, const TpTouchBox& b) { return a.cx < b.cx; });
    if (g_tpTouchBoxes.size() > 254) {
        loadPrintf("teleporttrig: %zu touch-triggered, the last %zu (from x=%.0f) are NOT modelled\n",
                   g_tpTouchBoxes.size(), g_tpTouchBoxes.size() - 254, g_tpTouchBoxes[254].cx);
        g_tpTouchBoxes.resize(254);
    }
    // A portal's force and kept height are measured with one body in a plain frame
    // (calib_tpportal). In a turned frame the portal pass leaves the force out (the game swaps
    // the axes first, +0x9c3), and a dual's second body takes it unmeasured: a level that has
    // either says so here.
    if (!tpActUids.empty() && (tpLevelTurns || tpLevelDual))
        loadPrintf("teleport: %zu portal(s) push or keep y (uid %d first) in a level that %s%s%s - "
                   "NOT modelled in a turned frame, not measured for a dual's second body\n",
                   tpActUids.size(), tpActUids.front(), tpLevelTurns ? "turns its gameplay" : "",
                   (tpLevelTurns && tpLevelDual) ? " and " : "", tpLevelDual ? "goes dual" : "");
    // The teleport trigger's move is taken for one body in a plain frame moving right (step.hpp,
    // the end of stepOne): elsewhere it is skipped.
    if (!g_tpTrigs.empty() && (tpLevelTurns || tpLevelDual))
        loadPrintf("teleporttrig: %zu trigger(s) (uid %d first) in a level that %s%s%s - NOT "
                   "modelled in a dual, a turned frame or moving backwards\n",
                   g_tpTrigs.size(), g_tpTrigs.front().uid, tpLevelTurns ? "turns its gameplay" : "",
                   (tpLevelTurns && tpLevelDual) ? " and " : "", tpLevelDual ? "goes dual" : "");
    std::sort(g_gravTrigs.begin(), g_gravTrigs.end(),
              [](const GravTrig& a, const GravTrig& b) { return a.cx < b.cx; });
    std::stable_sort(g_camFrees.begin(), g_camFrees.end(),
                     [](const CamFree& a, const CamFree& b) { return a.cx < b.cx; });
    std::sort(g_staticCams.begin(), g_staticCams.end(),
              [](const StaticCam& a, const StaticCam& b) { return a.cx < b.cx; });
    std::stable_sort(g_unlinkTrigs.begin(), g_unlinkTrigs.end(),
                     [](const UnlinkTrig& a, const UnlinkTrig& b) { return a.cx < b.cx; });
    std::stable_sort(g_ctrlTrigs.begin(), g_ctrlTrigs.end(),
                     [](const CtrlTrig& a, const CtrlTrig& b) { return a.cx < b.cx; });
    std::sort(g_zoomTrigs.begin(), g_zoomTrigs.end(),
              [](const ZoomTrig& a, const ZoomTrig& b) { return a.cx < b.cx; });
    zoomCrossReset(g_zoomTrigs.size());   // --zoomchain: no crossing seen yet
    // Which moving objects does a trigger ROTATE? Read off the recording rather
    // than derived from the trigger chain: the recording is what this run
    // actually observed, and turnedBox needs the answer before the angle has
    // grown away from zero (see its axis-aligned branch).
    //
    // "Rotated" means a recorded angle that leaves the angle the object was
    // PLACED at (objrects column 9, o.rot -- nothing has seeked yet), not any
    // angle other than 0. Compared against 0, a portal placed at 90 degrees
    // records 90 on every row and was counted as trigger-rotated, so the portal
    // test turned the player's box against it.
    // On custom level C the gravity portal uid 28482 (id 11, placed at
    // rot 90, never moved: one recorded row, 37.5 x 12.5) flips p2 at t=3,910 in
    // GD. p2 was a spinning cube at (5,075.05, 905.15), and the recorded rows
    // bracket GD's rule. As a plain AABB the contact is -0.84 px at 3,909 and
    // +1.10 at 3,910: GD fires at 3,910. Turned by the model's angle it is
    // -1.01 at 3,910, so the model flipped p2 one tick late. Turned by GD's own
    // angle it is already +1.6 at 3,909. From there p2 reached the ceiling at
    // 4,008, after the release at 4,007, instead of GD's 4,006. So it sat
    // where GD jumped, and every plan through there died at t~4,024 with the
    // model alive.
    L.dyn.everRot.assign(L.dyn.size(), 0);
    for (size_t i = 0; i < L.dyn.size(); ++i) {
        const double placed = L.dyn.objs[i].rot;
        for (const DynSample& s : L.dyn.samples[i]) {
            double d = std::fmod(std::fabs((double)s.rot - placed), 360.0);
            if (d > 180.0) d = 360.0 - d;
            if (d > 0.001) { L.dyn.everRot[i] = 1; break; }
        }
        // --rotobb: a hazard a Rotate turns is turned for the whole run, the rows' angle or not (its
        // recording may end before the Rotate fires; --rottail turns it on from there)
        if (L.dyn.objs[i].obbOk && g_rotated.count(L.dyn.objs[i].uid)
            && g_obb.find(L.dyn.objs[i].uid) == g_obb.end()) {
            L.dyn.everRot[i] = 1;
            // --rotobblatch (dynamics.hpp): ...but its box is the hazard's only from the first tick a rotation turned
            // it (Dynamics::obbLatch); until then the plain rects, as GD tests an unlatched hazard
            {
                if (L.dyn.obbLatchT.size() != L.dyn.size()) L.dyn.obbLatchT.assign(L.dyn.size(), -1);
                int first = std::numeric_limits<int>::max();
                for (const DynSample& s : L.dyn.samples[i]) {
                    double d = std::fmod(std::fabs((double)s.rot - placed), 360.0);
                    if (d > 180.0) d = 360.0 - d;
                    if (d > 0.001) { first = s.t; break; }
                }
                L.dyn.obbLatchT[i] = first;
                L.dyn.objs[i].obbOk = 0;   // the first seek sets it from the tick
            }
        }
    }
    {
        size_t n = 0;
        for (uint8_t v : L.dyn.everRot) n += (v != 0);
        if (n) loadPrintf("dynamics: %zu of %zu moving objects are rotated by "
                          "a trigger\n", n, L.dyn.size());
        size_t nl = 0, never = 0;
        for (const int v : L.dyn.obbLatchT) {
            nl += (v >= 0);
            never += (v == std::numeric_limits<int>::max());
        }
        if (nl) loadPrintf("rotobblatch: %zu hazard(s) take their turned box only once turned (%zu not turned in the "
                           "recording)\n", nl, never);
    }
    // Resolve each orbit against the loaded level: which g_autoTrig entry gives
    // the Rotate its fire tick, where the centre ended up in dyn, and the
    // entry-relative vector the rotation turns. A spec that cannot be resolved
    // keeps anchor/centreIdx at -1 and places nothing, so the object stays
    // exactly as it was.
    {
        std::unordered_map<int, size_t> idx;
        for (size_t i = 0; i < L.dyn.size(); ++i) idx[L.dyn.objs[i].uid] = i;
        size_t armed = 0;
        for (auto& kv : g_rotSpec) {
            RotSpec& R = kv.second;
            const auto io = idx.find(kv.first), ic = idx.find(R.centreUid);
            if (io == idx.end() || ic == idx.end()) continue;
            if (L.dyn.samples[ic->second].empty()) continue;
            for (size_t a = 0; a < g_autoTrig.size(); ++a)
                if (g_autoTrig[a].uid == R.trigUid) { R.anchor = (int)a; break; }
            if (R.anchor < 0) continue;          // touch-fired: stage 2
            R.centreIdx = (int)ic->second;
            R.relX = L.dyn.samples[io->second][0].cx
                   - L.dyn.samples[ic->second][0].cx;
            R.relY = L.dyn.samples[io->second][0].cy
                   - L.dyn.samples[ic->second][0].cy;
            ++armed;
        }
        if (g_rotCompute && !g_rotSpec.empty())
            loadPrintf("rotplace: %zu of %zu orbits armed (autonomous rotate "
                       "+ recorded centre); %zu spec'd objects reached the "
                       "router, %zu were routed into dyn\n",
                       armed, g_rotSpec.size(), g_rotSeen, g_rotRouted);
    }
    // --rottail: each turned object's Rotates against this level -- the
    // g_autoTrig entry that dates each one, where its centre stands -- and the object's own box. An object
    // that something else also moves (Move parts) unless every Rotate turns it about itself (--rottailmove),
    // whose Rotate is not autonomous, or whose centre is recorded moving or cannot be found, is left out whole.
    g_rotTailN = 0;
    if (!g_rotTailParts.empty()) {
        std::unordered_map<int, size_t> idx;
        for (size_t i = 0; i < L.dyn.size(); ++i) idx[L.dyn.objs[i].uid] = i;
        L.dyn.rotTail.assign(L.dyn.size(), {});
        L.dyn.rotTailMove.assign(L.dyn.size(), 0);
        L.dyn.rotBoxW.assign(L.dyn.size(), 0.f);
        L.dyn.rotBoxH.assign(L.dyn.size(), 0.f);
        L.dyn.rotTailMaxT = -1;
        for (const auto& sm : L.dyn.samples)
            if (!sm.empty()) L.dyn.rotTailMaxT = std::max(L.dyn.rotTailMaxT, sm.back().t);
        for (size_t i = 0; i < L.dyn.size(); ++i) {
            const auto it = g_rotTailParts.find(L.dyn.objs[i].uid);
            if (it == g_rotTailParts.end()) continue;
            // --rottailmove (dynamics.hpp): an object a Move also carries stays in when every Rotate turns
            // it about itself (checked below)
            const bool moveParts = i < L.dyn.autoParts.size() && !L.dyn.autoParts[i].empty();
            if (moveParts && !g_rotTailMove) continue;
            const auto& sm = L.dyn.samples[i];
            if (sm.empty()) continue;
            std::vector<Dynamics::RotTail> parts;
            bool ok = true;
            for (const RotTailPart& p : it->second) {
                int a = -1;
                for (size_t k = 0; k < g_autoTrig.size(); ++k)
                    if (g_autoTrig[k].uid == p.trigUid) { a = (int)k; break; }
                double ccx = 0.0, ccy = 0.0;
                const auto ic = idx.find(p.centreUid);
                if (p.centreUid == 0) {
                    // turns about its own position: nothing to find
                } else if (ic != idx.end()) {
                    const auto& cs = L.dyn.samples[ic->second];
                    if (cs.size() > 1) { ok = false; break; }
                    ccx = cs.empty() ? (double)L.dyn.objs[ic->second].cx : (double)cs[0].cx;
                    ccy = cs.empty() ? (double)L.dyn.objs[ic->second].cy : (double)cs[0].cy;
                } else {
                    const auto rp = rotCentrePos.find(p.centreUid);
                    if (rp == rotCentrePos.end()) { ok = false; break; }
                    ccx = rp->second.first;
                    ccy = rp->second.second;
                }
                if (a < 0) { ok = false; break; }
                parts.push_back({a, ccx, ccy, p.total, p.durT, p.erate, p.ease, p.lock,
                             (uint8_t)(p.centreUid == 0 ? 1 : 0)});
            }
            if (!ok || parts.empty()) continue;
            if (moveParts) {
                bool allOwn = true;
                for (const Dynamics::RotTail& r : parts)
                    if (!r.own) allOwn = false;
                if (!allOwn) continue;
                L.dyn.rotTailMove[i] = 1;
            }
            // The object's own box, from whichever of its first and last rows inverts best: a box w x h
            // turned by r has the bounding box (w|c| + h|s|, w|s| + h|c|), which is singular at 45 degrees.
            double bestDet = 0.3;
            for (const DynSample* q : {&sm.front(), &sm.back()}) {
                const double r = (double)q->rot * (3.14159265358979 / 180.0);
                const double c = std::fabs(std::cos(r)), s = std::fabs(std::sin(r));
                const double det = c * c - s * s;
                if (std::fabs(det) <= bestDet) continue;
                const double W = 2.0 * q->hw, H = 2.0 * q->hh;
                const double w = (W * c - H * s) / det, h = (H * c - W * s) / det;
                if (w <= 0.0 || h <= 0.0) continue;
                bestDet = std::fabs(det);
                L.dyn.rotBoxW[i] = (float)w;
                L.dyn.rotBoxH[i] = (float)h;
            }
            L.dyn.rotTail[i] = std::move(parts);
            ++g_rotTailN;
        }
    }
    // ---- stage 1': give the turned objects a COMPUTED orbit in samples[] ----
    //
    // Not by adding a rotation to the placement path -- that turns a recorded
    // object twice, which deathref caught (see g_rotCompute). By decomposing:
    //
    //   pos(t) = [ entry_C + R(theta(t - t0)) * rel ]  +  [ C(t) - entry_C ]
    //              ^^^^^^^^^ into samples[] ^^^^^^^^^      ^^ autoParts ^^
    //
    // samples[] then carries the orbit about a STATIONARY centre, whose only
    // controller is the Rotate -- so the single-shift re-timing that plays it
    // is the right operation for it, while the translation stays analytic with
    // every controller on its own fire tick. That is the whole 1.65-2.18 px:
    // today one shift has to carry a rotate and a move that cross at different
    // x, and no single shift can.
    //
    // The decomposition needs the object and its centre to share their move
    // controllers. Measured on lv21: all 66 collidable turned objects do (0
    // differ). An object that does not is refused and keeps its recording.
    if (g_rotSplit && !g_rotSpec.empty()) {
        size_t done = 0, refusedFit = 0, refusedNoAuto = 0;
        double worstFit = 0.0;
        std::unordered_map<int, size_t> idx;
        for (size_t i = 0; i < L.dyn.size(); ++i) idx[L.dyn.objs[i].uid] = i;
        for (const auto& kv : g_rotSpec) {
            const RotSpec& R = kv.second;
            if (R.anchor < 0 || R.centreIdx < 0) continue;
            const auto io = idx.find(kv.first);
            if (io == idx.end()) continue;
            const size_t i = io->second, ci = (size_t)R.centreIdx;
            if (L.dyn.samples[i].size() < 2 || L.dyn.samples[ci].empty()) continue;
            // the translation has to have somewhere analytic to come from
            if (L.dyn.autoParts[i].empty()) { ++refusedNoAuto; continue; }
            const double ex = L.dyn.samples[ci][0].cx, ey = L.dyn.samples[ci][0].cy;
            // The ROTATE's own fire tick on the recorded timeline, fitted.
            // trigRecFire is the object's FIRST motion, which for an object
            // that also moves is whichever trigger fired first -- taking it as
            // the rotation's origin refused all twenty of lv21's rotate+move
            // objects on the fit. The rotate has its own crossing, and the
            // recording is the only place it is written down, so it is read out
            // of the recording the same way trigRecFire is.
            const int lo = L.dyn.samples[i][0].t - (int)R.durT - 5;
            const int hi = L.dyn.samples[i].back().t + 5;
            int t0 = L.dyn.samples[i][0].t;
            {
                // The orbit point depends on (sample tick - candidate) alone, so it is computed
                // once per distinct difference d -- with the same operations, so the same doubles --
                // instead of once per candidate and sample (ease, cos and sin were the whole cost:
                // lv21's 20 rotating objects took ~2.3 s of every load). The centre's held row per
                // sample does not depend on the candidate either. Candidates, their order, the
                // early break and the strict `<` are as before, so t0 is the same tick.
                const std::vector<DynSample>& smp = L.dyn.samples[i];
                const std::vector<DynSample>& csm = L.dyn.samples[ci];
                const int dMin = smp.front().t - hi, dMax = smp.back().t - lo;
                std::vector<double> gxT((size_t)(dMax - dMin + 1)), gyT(gxT.size());
                for (int d = dMin; d <= dMax; ++d) {
                    const double th = -R.total
                        * gdEase(R.ease, R.erate, (double)d / R.durT)
                        * 3.14159265358979 / 180.0;
                    const double c = std::cos(th), sn = std::sin(th);
                    gxT[(size_t)(d - dMin)] = ex + c * R.relX - sn * R.relY;
                    gyT[(size_t)(d - dMin)] = ey + sn * R.relX + c * R.relY;
                }
                std::vector<size_t> cAt(smp.size());
                {
                    size_t ck = 0;
                    for (size_t k = 0; k < smp.size(); ++k) {
                        while (ck + 1 < csm.size() && csm[ck + 1].t <= smp[k].t) ++ck;
                        cAt[k] = ck;
                    }
                }
                double bestE = 1e18;
                for (int cand = lo; cand <= hi; ++cand) {
                    double w = 0.0;
                    for (size_t k = 0; k < smp.size(); ++k) {
                        const DynSample& s = smp[k];
                        const size_t di = (size_t)(s.t - cand - dMin);
                        const double gx = gxT[di], gy = gyT[di];
                        const size_t ck = cAt[k];
                        w = std::max(w, std::hypot(
                            (double)s.cx - (gx + csm[ck].cx - ex),
                            (double)s.cy - (gy + csm[ck].cy - ey)));
                        if (w >= bestE) break;
                    }
                    if (w < bestE) { bestE = w; t0 = cand; }
                }
            }
            // centre position by tick, held between recorded rows
            size_t cj = 0;
            auto centreAt = [&](int t) {
                while (cj + 1 < L.dyn.samples[ci].size()
                       && L.dyn.samples[ci][cj + 1].t <= t) ++cj;
                while (cj > 0 && L.dyn.samples[ci][cj].t > t) --cj;
                return std::pair<double, double>(L.dyn.samples[ci][cj].cx,
                                                 L.dyn.samples[ci][cj].cy);
            };
            std::vector<DynSample> gen = L.dyn.samples[i];
            double worst = 0.0;
            cj = 0;
            for (DynSample& s : gen) {
                const double th = -R.total
                    * gdEase(R.ease, R.erate, (double)(s.t - t0) / R.durT)
                    * 3.14159265358979 / 180.0;
                const double c = std::cos(th), sn = std::sin(th);
                const double ox = (double)R.relX, oy = (double)R.relY;
                const double gx = ex + c * ox - sn * oy;
                const double gy = ey + sn * ox + c * oy;
                // recorded = orbit + the centre's own translation, so the check
                // is (recorded - centre translation) against the orbit
                const auto cp = centreAt(s.t);
                worst = std::max(worst,
                    std::hypot((double)s.cx - (gx + cp.first - ex),
                               (double)s.cy - (gy + cp.second - ey)));
                s.cx = (float)gx;
                s.cy = (float)gy;
            }
            if (worst > 0.1) { ++refusedFit; continue; }
            worstFit = std::max(worstFit, worst);
            L.dyn.samples[i] = std::move(gen);
            if (i < L.dyn.rotSplit.size()) L.dyn.rotSplit[i] = 1;
            ++done;
        }
        if (done || refusedFit || refusedNoAuto)
            loadPrintf("rotsplit: %zu objects moved onto a computed orbit "
                       "(worst fit %.4f px), %zu refused on fit, %zu with no "
                       "analytic translation\n",
                       done, worstFit, refusedFit, refusedNoAuto);
    }
    // ---- stage 1'': carry a turned object past the end of its recording ----
    //
    // Unconditional since the 0.4.0 clean-up (it was --rotextend, on in every loop call that
    // read a recording since d008ee2). The Rotate says where the object is for the whole turn; the
    // recording says it only as far as the attempt that wrote it lived, and past its last row the
    // object holds that row. For an armed orbit that the recorded rows fit (stage 1''s fit, the
    // same 0.1 px), the rows from the last one to the end of the turn are appended from the
    // formula, one a tick. The centre has to stand still: an object on stage 1' carries the orbit
    // alone (its translation is analytic), so it does by construction; any other object's rows are
    // orbit plus centre, and where the centre moved over the recording nothing says where it goes
    // next, so the object keeps its recording as it was.
    if (!g_rotSpec.empty()) {
        size_t done = 0, rows = 0, refusedFit = 0, refusedCentre = 0;
        std::unordered_map<int, size_t> idx;
        for (size_t i = 0; i < L.dyn.size(); ++i) idx[L.dyn.objs[i].uid] = i;
        const double kPi = 3.14159265358979;
        for (const auto& kv : g_rotSpec) {
            const RotSpec& R = kv.second;
            if (R.anchor < 0 || R.centreIdx < 0 || R.durT <= 0.0) continue;
            const auto io = idx.find(kv.first);
            if (io == idx.end()) continue;
            const size_t i = io->second, ci = (size_t)R.centreIdx;
            std::vector<DynSample>& sm = L.dyn.samples[i];
            if (sm.size() < 2 || L.dyn.samples[ci].empty()) continue;
            const double ex = L.dyn.samples[ci][0].cx, ey = L.dyn.samples[ci][0].cy;
            const bool split = i < L.dyn.rotSplit.size() && L.dyn.rotSplit[i];
            if (!split) {
                bool still = true;
                for (const DynSample& c : L.dyn.samples[ci])
                    if (std::fabs((double)c.cx - ex) >= kRecEps
                        || std::fabs((double)c.cy - ey) >= kRecEps) {
                        still = false;
                        break;
                    }
                if (!still) { ++refusedCentre; continue; }
            }
            auto orbit = [&](double tick, double t0, double& gx, double& gy) {
                const double th = -R.total * gdEase(R.ease, R.erate, (tick - t0) / R.durT)
                                  * kPi / 180.0;
                const double c = std::cos(th), sn = std::sin(th);
                gx = ex + c * R.relX - sn * R.relY;
                gy = ey + sn * R.relX + c * R.relY;
            };
            // The first recorded motion. The Rotate fired no later than that and its turn is over
            // by that tick plus the duration, so a recording that reaches past it holds the whole
            // turn already and is not fitted at all -- the fit below is the loader's cost, every
            // call, and on lv21 fitting every armed orbit over its whole recording took 8.5 s of an
            // 11.5 s replay. An object whose position never changes turns in place: no orbit.
            size_t k1 = 1;
            while (k1 < sm.size() && std::fabs((double)sm[k1].cx - (double)sm[0].cx) < kRecEps
                   && std::fabs((double)sm[k1].cy - (double)sm[0].cy) < kRecEps)
                ++k1;
            if (k1 == sm.size()) continue;
            const int fm = sm[k1].t;
            if (sm.back().t >= fm + (int)std::ceil(R.durT) + 5) continue;
            // The Rotate's fire tick on the recorded timeline, fitted as stage 1' fits it, over the
            // ticks it can be: from a whole turn before the first motion to the tick after it. The
            // choice is the plain scan's: the lowest tick of the least error, refused when that error
            // is over 0.1 (kFitTol). A candidate's rows are walked only while its error stays within
            // min(the least error so far, kFitTol): one that passes that can neither be taken nor beat
            // the best. The bound used to be the error of the tick before the first motion, which for an
            // orbit that does not fit at all is tens of pixels, so every candidate of a refused object was
            // walked deep into its recording: on a level with 128 long Rotates that was 8e8 row
            // evaluations and 24 s of the first load, to refuse all 128.
            constexpr double kFitTol = 0.1;
            auto fitErr = [&](int cand, double bound) {
                double w = 0.0;
                for (const DynSample& s : sm) {
                    double gx, gy;
                    orbit((double)s.t, (double)cand, gx, gy);
                    w = std::max(w, std::hypot((double)s.cx - gx, (double)s.cy - gy));
                    if (w > bound) break;
                }
                return w;
            };
            const int lo = fm - (int)R.durT - 5, hi = fm + 1;
            int t0 = sm[0].t;
            double bestE = 1e18;
            bool fitted = false;
            for (int cand = lo; cand <= hi; ++cand) {
                const double w = fitErr(cand, std::min(bestE, kFitTol));
                // a walk that stopped early returns an error past its bound, which is neither <= kFitTol
                // nor < bestE
                if (w <= kFitTol && w < bestE) { bestE = w; t0 = cand; fitted = true; }
            }
            if (g_rotFitCheck) {
                // --rotfitcheck (debug): the scan this replaced, the error of the tick before the first motion
                // as the bound, and whether it agrees on taking the fit and on the tick
                const double seedE = fitErr(fm - 1, 1e18);
                int refT0 = sm[0].t;
                double refBest = 1e18;
                for (int cand = lo; cand <= hi; ++cand) {
                    const double w = fitErr(cand, std::min(refBest, seedE));
                    if (w < refBest) { refBest = w; refT0 = cand; }
                }
                const bool refFitted = refBest <= kFitTol;
                ++g_rotFitChecked;
                if (refFitted != fitted || (fitted && refT0 != t0)) {
                    ++g_rotFitMismatch;
                    loadPrintf("rotfitcheck: MISMATCH uid %d: new %s t0 %d, scan %s t0 %d\n", kv.first,
                               fitted ? "fitted" : "refused", t0, refFitted ? "fitted" : "refused", refT0);
                }
            }
            if (!fitted) { ++refusedFit; continue; }
            const int tEnd = t0 + (int)std::ceil(R.durT);
            const DynSample last = sm.back();
            if (last.t >= tEnd) continue;
            // The unturned half sizes, from the entry row when it stands on a right angle; the
            // appended rows' box is then the turned rectangle's bound, as GD's getObjectRect gives
            // it (lv21 uid 15024 at -334.502: 27|cos| + 26|sin| = 35.564, the recorded w). Any other
            // entry angle keeps the last row's box.
            double uhw = -1.0, uhh = -1.0;
            {
                const double r0 = std::fmod(std::fabs((double)sm[0].rot), 180.0);
                if (r0 < 0.001 || r0 > 179.999) { uhw = sm[0].hw; uhh = sm[0].hh; }
                else if (std::fabs(r0 - 90.0) < 0.001) { uhw = sm[0].hh; uhh = sm[0].hw; }
            }
            const double eLast = gdEase(R.ease, R.erate, ((double)last.t - t0) / R.durT);
            for (int t = last.t + 1; t <= tEnd; ++t) {
                DynSample s = last;
                s.t = t;
                double gx, gy;
                orbit((double)t, (double)t0, gx, gy);
                s.cx = (float)gx;
                s.cy = (float)gy;
                if (!R.lockrot) {
                    const double e = gdEase(R.ease, R.erate, ((double)t - t0) / R.durT);
                    s.rot = (float)((double)last.rot + R.total * (e - eLast));
                }
                if (uhw > 0.0 && uhh > 0.0) {
                    const double a = (double)s.rot * kPi / 180.0;
                    const double c = std::fabs(std::cos(a)), sn = std::fabs(std::sin(a));
                    s.hw = (float)(uhw * c + uhh * sn);
                    s.hh = (float)(uhw * sn + uhh * c);
                }
                sm.push_back(s);
                ++rows;
            }
            ++done;
        }
        loadPrintf("rotextend: %zu turned objects carried past their recordings (%zu rows), %zu "
                   "refused on fit, %zu whose centre moved\n",
                   done, rows, refusedFit, refusedCentre);
        if (g_rotFitCheck)
            loadPrintf("rotfitcheck: %lld fits checked against the plain scan, %lld disagree\n",
                       g_rotFitChecked, g_rotFitMismatch);
    }
    // --rotcheck: does the COMPUTED orbit reproduce what GD recorded?
    //
    // The corpus-wide form of the harness that measured lv21 uid15367 to
    // 0.0060 px (see RotSpec). It places nothing -- it only answers, per
    // object, "if the model computed this orbit instead of replaying the
    // recording, how far off would it be?", which is the number the switch-on
    // has to be argued from.
    //
    // The centre's own motion is taken from ITS recording rather than
    // re-derived, so the check isolates the rotation: an object whose centre is
    // not recorded is reported as unchecked instead of being scored against a
    // guess. The fire tick likewise comes from the object's own first recorded
    // motion, because the phase (crossing + 1) is separately measured and is
    // not what this check is about.
    if (g_rotCheck && !g_rotSpec.empty()) {
        std::unordered_map<int, size_t> idx;
        for (size_t i = 0; i < L.dyn.size(); ++i) idx[L.dyn.objs[i].uid] = i;
        size_t checked = 0, noCentre = 0, noRec = 0, pass = 0;
        double worstAll = 0.0;
        int worstUid = 0;
        for (const auto& kv : g_rotSpec) {
            const auto io = idx.find(kv.first);
            const auto ic = idx.find(kv.second.centreUid);
            if (io == idx.end() || L.dyn.samples[io->second].size() < 2) {
                ++noRec;
                continue;
            }
            if (ic == idx.end() || L.dyn.samples[ic->second].empty()) {
                ++noCentre;
                continue;
            }
            const auto& so = L.dyn.samples[io->second];
            const auto& sc = L.dyn.samples[ic->second];
            // centre position by tick, held between recorded rows (the recorder
            // skips a row when the move since the last one is under its epsilon)
            auto centreAt = [&](int t) {
                const DynSample* best = &sc.front();
                for (const DynSample& s : sc) {
                    if (s.t > t) break;
                    best = &s;
                }
                return std::pair<double, double>((double)best->cx, (double)best->cy);
            };
            // first motion of the object = its fire tick, to within recordLag
            int t0 = so.front().t;
            for (size_t k = 1; k < so.size(); ++k)
                if (std::fabs((double)so[k].cx - (double)so[0].cx) > 1e-4
                    || std::fabs((double)so[k].cy - (double)so[0].cy) > 1e-4) {
                    t0 = so[k].t - 1;
                    break;
                }
            const RotSpec& R = kv.second;
            float x = so.front().cx, y = so.front().cy;
            double worst = 0.0;
            size_t si = 0;
            for (int t = t0 + 1; t <= so.back().t; ++t) {
                const auto cp = centreAt(t - 1), cn = centreAt(t);
                const double th0 = -R.total * gdEase(R.ease, R.erate,
                                                     (double)(t - 1 - t0) / R.durT);
                const double th1 = -R.total * gdEase(R.ease, R.erate,
                                                     (double)(t - t0) / R.durT);
                rotStep(x, y, cp.first, cp.second, cn.first, cn.second,
                        (th1 - th0) * 3.14159265358979 / 180.0);
                while (si < so.size() && so[si].t < t) ++si;
                if (si < so.size() && so[si].t == t) {
                    const double d = std::hypot((double)x - (double)so[si].cx,
                                                (double)y - (double)so[si].cy);
                    worst = std::max(worst, d);
                }
            }
            ++checked;
            if (worst <= 0.1) ++pass;
            if (worst > worstAll) { worstAll = worst; worstUid = kv.first; }
        }
        loadPrintf("rotcheck: %zu checked, %zu within 0.1px, worst %.4f px "
                   "(uid %d); %zu unrecorded, %zu centre not recorded\n",
                   checked, pass, worstAll, worstUid, noRec, noCentre);
    }
    // Say which objects came off the recording. Silent when there are none, so
    // the 21 levels this does not touch print exactly what they printed before.
    if (g_formulaDriven) {
        // ...and how many of them the autonomous lag re-dates (dynamics.hpp): the
        // recorded ones whose autonomous curve takes longer, or shorter, to clear
        // the recorder's 0.05 px than their touch move does.
        int nLagDiff = 0, lagLo = 0, lagHi = 0;
        for (size_t i = 0; i < L.dyn.formula.size(); ++i) {
            if (!L.dyn.formula[i] || L.dyn.trigRecFire[i] < 0) continue;
            const int d = L.dyn.autoLag[i] - L.dyn.recLag[i];
            if (d == 0) continue;
            if (nLagDiff == 0 || d < lagLo) lagLo = d;
            if (nLagDiff == 0 || d > lagHi) lagHi = d;
            ++nLagDiff;
        }
        loadPrintf("dynamics: %d formula-driven (a touch AND an autonomous "
                   "controller both reach them and both move); %d recorded "
                   "with an autonomous lag other than the touch lag (%+d..%+d)\n",
                   g_formulaDriven, nLagDiff, lagLo, lagHi);
    }
    if (g_recLeadsN)
        loadPrintf("recleads: %d autonomous objects read as their recording (it moves before the "
                   "player reaches any controller the walk found)\n", g_recLeadsN);
    if (g_touchRecLeadsN)
        loadPrintf("touchrecleads: %d touch objects no box moves read as their recording (it moves before "
                   "the recorded attempt entered any of their boxes)\n", g_touchRecLeadsN);
    if (lockXN)
        loadPrintf("movelockx: %d objects a Move reaches in x are locked in x (speed portals and "
                   "triggers)\n", lockXN);
    // --touchretimebox (dynamics.hpp): the recording's entry into each box, from the objects it
    // re-times that no other box reaches -- an object two boxes move first moved for whichever
    // came first, which dates neither. The same objects the re-timing branch serves: an
    // autonomous controller (autoAnchor / recAuto / autoReach) leaves the recording's clock alone.
    if (!L.dyn.objs.empty()) {
        L.dyn.boxRecEntry.assign((size_t)kTouchBits, -1);
        std::vector<int> latest((size_t)kTouchBits, -1), n((size_t)kTouchBits, 0);
        std::vector<int> first((size_t)kTouchBits, -1);   // uid giving the entry
        for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
            if (L.dyn.trigRecFire[i] < 0 || L.dyn.autoAnchor[i] >= 0 || L.dyn.recAuto[i]
                || L.dyn.autoReach[i] || L.dyn.touchLeads[i])   // --touchrecleads: no box moved it
                continue;
            int box = -1, bits = 0;
            for (int b = 0; b < kTouchBits; ++b)
                if (L.dyn.trigMask[i].test(b)) { box = b; ++bits; }
            if (bits != 1) continue;
            const int e = L.dyn.trigRecFire[i] - kTouchRetimeLat
                          - std::max(0, L.dyn.recLag[i] - 1);
            int& cur = L.dyn.boxRecEntry[(size_t)box];
            if (cur < 0 || e < cur) { cur = e; first[(size_t)box] = L.dyn.objs[i].uid; }
            latest[(size_t)box] = std::max(latest[(size_t)box], e);
            ++n[(size_t)box];
        }
        // Named where it changes something: a box whose objects do not all start together.
        for (int b = 0; b < kTouchBits; ++b)
            if (n[(size_t)b] > 0 && latest[(size_t)b] > L.dyn.boxRecEntry[(size_t)b])
                loadPrintf("touchretimebox: box %d entry t=%d (uid %d), %d object(s), the "
                           "last starting %d ticks after\n", b,
                           L.dyn.boxRecEntry[(size_t)b], first[(size_t)b], n[(size_t)b],
                           latest[(size_t)b] - L.dyn.boxRecEntry[(size_t)b]);
    }
    // --activators: the objects an activator switches take their on/off from their switches
    // (Dynamics::onEv), starting from the recording's reset snapshot. Only those objects: an
    // x-crossing Toggle alone reaches thousands of objects in a SubZero level, and moving all of
    // them off the recording is a different change from this one.
    if (tt && !L.dyn.objs.empty()) {
        std::unordered_set<int> switched;   // uids some activator switches
        std::unordered_map<int, std::vector<uint8_t>> offBy;   // --boxoff: uid -> off boxes
        for (size_t b = 0; b < tt->size() && b < (size_t)kTouchBits; ++b) {
            if (!(*tt)[b].activator && !(*tt)[b].countAct) continue;   // --countactgrp
            for (const TouchTrig::TogEv& e : (*tt)[b].togEv) {
                if ((*tt)[b].offSwitch) offBy[e.uid].push_back((uint8_t)b);
                else switched.insert(e.uid);
            }
        }
        if (!offBy.empty()) {
            L.dyn.offBox.assign(L.dyn.objs.size(), {});
            size_t n = 0;
            for (size_t i = 0; i < L.dyn.objs.size(); ++i)
                if (const auto o = offBy.find(L.dyn.objs[i].uid); o != offBy.end()) {
                    L.dyn.offBox[i] = o->second;
                    ++n;
                }
            loadPrintf("boxoff: %zu object(s) keep their recording and go out when their box "
                       "fires\n", n);
        }
        if (!switched.empty()) {
            const auto boxEv = boxOnEvents(*tt);
            const auto autoEv = at ? autoOnEvents(*at)
                                   : std::unordered_map<int, std::vector<OnEvent>>{};
            L.dyn.onEv.assign(L.dyn.objs.size(), {});
            L.dyn.onInit.assign(L.dyn.objs.size(), 1);
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                const int u = L.dyn.objs[i].uid;
                if (!switched.count(u)) continue;
                // NO SNAPSHOT, NO SWITCHES. The recording's first row is what a recorded run
                // left there, which is exactly what this replaces -- a run that took the key
                // records the platforms on -- so falling back to it would rebuild the fault it
                // removes. The object stays on its recording and says so.
                const auto s0 = g_groupInit.find(u);
                if (s0 == g_groupInit.end()) {
                    loadPrintf("activators: uid %d stays on its recording - no reset snapshot "
                               "for it (a recording made before the snapshot existed)\n", u);
                    continue;
                }
                std::vector<OnEvent>& ev = L.dyn.onEv[i];
                if (const auto a = autoEv.find(u); a != autoEv.end())
                    ev.insert(ev.end(), a->second.begin(), a->second.end());
                if (const auto b = boxEv.find(u); b != boxEv.end())
                    ev.insert(ev.end(), b->second.begin(), b->second.end());
                L.dyn.onInit[i] = s0->second;
                loadPrintf("activators: uid %d gets %zu switch(es), on=%d at t=0 from the reset "
                           "snapshot\n", u, ev.size(), (int)L.dyn.onInit[i]);
                // --pressfresh: a teleport a toggle block switches on is live on the press's own
                // step (Obj::pendBox; the first such box when there are several).
                if (L.dyn.objs[i].type == 28)
                    for (const OnEvent& e : ev)
                        if (e.box >= 0 && e.on && e.delay == 0.0 && (size_t)e.box < tt->size()
                            && (*tt)[(size_t)e.box].press && !(*tt)[(size_t)e.box].offSwitch) {
                            L.dyn.objs[i].pendBox = (int16_t)e.box;
                            loadPrintf("pressfresh: teleport uid %d is live on the press that "
                                       "fires box %d\n", u, e.box);
                            break;
                        }
            }
        }
    }
    // --tapdisarm: the objects an x-crossing trigger switches that a box can switch off (g_autoDisarm) get every
    // switch that reaches them (Dynamics::disarmEv), from the recording's reset snapshot, for the states where
    // it has.
    if (g_tapDisarm && at && !L.dyn.objs.empty() && g_autoDisarm.size() == at->size()) {
        std::unordered_set<int> hit;
        for (size_t k = 0; k < at->size(); ++k)
            if (!g_autoDisarm[k].empty())
                for (const AutoTrig::Tog& g : (*at)[k].tog) hit.insert(g.uid);
        if (!hit.empty()) {
            const auto autoEv = autoOnEvents(*at);
            // Every box's switches, the Taps' and Counts' too (boxOnEvents leaves those out), with their groups.
            std::unordered_map<int, std::vector<OnEvent>> boxEv;
            if (tt)
                for (size_t b = 0; b < tt->size() && b < (size_t)kTouchBits; ++b)
                    for (const TouchTrig::TogEv& e : (*tt)[b].togEv) {
                        OnEvent o;
                        o.box = (int)b;
                        o.on = e.on;
                        o.delay = e.delay;
                        o.grp = e.grp;
                        boxEv[e.uid].push_back(o);
                    }
            L.dyn.disarmEv.assign(L.dyn.objs.size(), {});
            L.dyn.disarmInit.assign(L.dyn.objs.size(), 1);
            size_t n = 0, noSnap = 0;
            for (size_t i = 0; i < L.dyn.objs.size(); ++i) {
                const int u = L.dyn.objs[i].uid;
                if (!hit.count(u)) continue;
                const auto s0 = g_groupInit.find(u);
                if (s0 == g_groupInit.end()) {   // no snapshot: stays on its recording (see --activators)
                    ++noSnap;
                    continue;
                }
                std::vector<OnEvent>& ev = L.dyn.disarmEv[i];
                if (const auto a = autoEv.find(u); a != autoEv.end())
                    ev.insert(ev.end(), a->second.begin(), a->second.end());
                if (const auto b = boxEv.find(u); b != boxEv.end())
                    ev.insert(ev.end(), b->second.begin(), b->second.end());
                L.dyn.disarmInit[i] = s0->second;
                ++n;
                if (g_dynDbg == u || g_dynDbg == -6) {
                    std::string s;
                    for (const OnEvent& o : ev) {
                        char b[96];
                        if (o.box >= 0)
                            std::snprintf(b, sizeof b, " box%d:g%d:%d+%.0f", o.box, o.grp, (int)o.on, o.delay);
                        else std::snprintf(b, sizeof b, " x%.0f:g%d:%d+%.0f", (*at)[(size_t)o.autoIdx].cx, o.grp,
                                           (int)o.on, o.delay);
                        s += b;
                    }
                    loadPrintf("tapdisarm: uid %d on=%d at the reset, switches%s\n", u, (int)L.dyn.disarmInit[i],
                               s.c_str());
                }
            }
            loadPrintf("tapdisarm: %zu object(s) take their switches where a box has switched one off (%zu "
                       "without a reset snapshot stay on their recording)\n", n, noSnap);
        }
    }
    // --recendquiet (dynamics.hpp): the tick the recording's run reached, for the tails past the last row
    L.dyn.groupsEnd = (int)g_groupsEndT;
    // --recendtail: each recorded object's switches past its last row, and the rigs --
    // the objects an x-crossing lock or a Rotate about a centre reaches -- simulated from the reset the way GD
    // steps them, each armed where the simulation reproduces its own last rows.
    g_recEndTailN = g_recEndTailTried = g_recEndTailOnN = 0;
    if (!L.dyn.objs.empty()) {
        Dynamics& D = L.dyn;
        const size_t N = D.size();
        D.rigTail.assign(N, {});
        D.tailOnEv.assign(N, {});
        D.tailOnBox.assign(N, 0);
        D.tailRecEnd = -1;
        for (size_t si = 0; si < D.samples.size(); ++si)   // --bootauto: not the left-out recording's rows (its clock)
            if (!D.samples[si].empty() && !D.fromBootRows(si))
                D.tailRecEnd = std::max(D.tailRecEnd, D.samples[si].back().t);
        {
            auto ev = at ? autoOnEvents(*at) : std::unordered_map<int, std::vector<OnEvent>>{};
            if (tt)
                for (auto& kv : boxOnEvents(*tt))
                    ev[kv.first].insert(ev[kv.first].end(), kv.second.begin(), kv.second.end());
            for (size_t i = 0; i < N; ++i) {
                if (D.samples[i].empty()) continue;
                const auto e = ev.find(D.objs[i].uid);
                if (e == ev.end() || e->second.empty()) continue;
                D.tailOnEv[i] = e->second;
                for (const OnEvent& o : e->second) D.tailOnBox[i] |= (o.box >= 0) ? 1 : 0;
                ++g_recEndTailOnN;
                // --dyndbg <uid> (or -6, every one): the switches it takes past the recording's end
                if (g_dynDbg == D.objs[i].uid || g_dynDbg == -6) {
                    std::string s;
                    for (const OnEvent& o : e->second) {
                        char b[96];
                        if (o.box >= 0) std::snprintf(b, sizeof b, " box%d:%d+%.0f", o.box, (int)o.on, o.delay);
                        else if (o.autoIdx >= 0 && at && (size_t)o.autoIdx < at->size())
                            std::snprintf(b, sizeof b, " x%.0f:%d+%.0f", (*at)[(size_t)o.autoIdx].cx, (int)o.on,
                                          o.delay);
                        else std::snprintf(b, sizeof b, " t%d:%d+%.0f", o.at, (int)o.on, o.delay);
                        s += b;
                    }
                    loadPrintf("recendtail: uid %d at x %.0f, last row t=%d on=%d, recording to t=%d, switches%s\n",
                               D.objs[i].uid, (double)D.samples[i][0].cx, D.samples[i].back().t,
                               (int)D.samples[i].back().on, D.tailRecEnd, s.c_str());
                }
            }
        }
        if (at && !g_xTrack.empty()) {
            const std::vector<AutoTrig>& A = *at;
            std::unordered_map<int, size_t> dynIdx;
            for (size_t i = 0; i < N; ++i) dynIdx[D.objs[i].uid] = i;
            // Each uid's controls on the track's clock: Moves (origin crossing + delay, eased, halted by a Stop's
            // crossing), lock windows (crossing .. + lockTicks), and the Rotates from g_rotTailParts.
            struct Mv { int s; int t0; double dur, dx, dy, erate; int ease; int stopT; };
            struct Lk { int s; int t0, t1; };
            std::unordered_map<int, std::vector<Mv>> mvOf;
            std::unordered_map<int, std::vector<Lk>> lkOf;
            std::unordered_set<int> bad;   // a target Move reaches it: not simulated
            std::unordered_map<int, size_t> trigIdx;
            for (size_t k = 0; k < A.size(); ++k) {
                trigIdx[A[k].uid] = k;
                const int cross = xRefCross(A[k].cx);
                if (cross < 0) continue;
                int stopT = 1 << 30;
                if (g_autoStop && k < g_autoStopBy.size())
                    for (const int s : g_autoStopBy[k])
                        if (s >= 0 && (size_t)s < A.size()) {
                            const int c2 = xRefCross(A[(size_t)s].cx);
                            if (c2 >= 0) stopT = std::min(stopT, c2);
                        }
                for (const TrigCtl& c : A[k].ctl) {
                    if (c.orbit) continue;
                    if (c.tmode) { bad.insert(c.uid); continue; }
                    if (c.lockTicks > 0.0)
                        lkOf[c.uid].push_back({-1, cross + c.spawnLag,
                                               std::min(cross + c.spawnLag + (int)c.lockTicks, stopT)});
                    if (c.dx != 0.f || c.dy != 0.f)
                        mvOf[c.uid].push_back({-1, cross + A[k].delay + c.spawnLag, c.durTicks, (double)c.dx,
                                               (double)c.dy, c.erate, c.ease, stopT});
                }
            }
            // The candidates, and the uids their orbits need (the centres, and theirs).
            std::vector<int> S;
            std::unordered_map<int, int> sIdx;
            auto addS = [&](int u) {
                if (sIdx.count(u)) return;
                sIdx[u] = (int)S.size();
                S.push_back(u);
            };
            std::vector<size_t> cand;
            for (size_t i = 0; i < N; ++i) {
                const int u = D.objs[i].uid;
                if (g_dynDbg == u)   // --dyndbg: why it is or is not a rig candidate
                    loadPrintf("recendtail: uid %d rows=%zu bad=%d rotSplit=%d recAuto=%d formula=%d reached=%d "
                               "lock=%d rot=%d\n", u, D.samples[i].size(), (int)bad.count(u),
                               (int)(i < D.rotSplit.size() && D.rotSplit[i]),
                               (int)(i < D.recAuto.size() && D.recAuto[i]),
                               (int)(i < D.formula.size() && D.formula[i]),
                               (int)((i < D.trigMask.size() && D.trigMask[i].any())
                                     || (i < D.autoAnchor.size() && D.autoAnchor[i] >= 0)),
                               (int)lkOf.count(u), (int)g_rotTailParts.count(u));
                if (D.samples[i].empty() || u < 0 || bad.count(u)) continue;
                if (i < D.rotSplit.size() && D.rotSplit[i]) continue;
                // (a recorded auto-moving object is a candidate too: formerly --recendauto)
                if (i < D.formula.size() && D.formula[i]) continue;
                const bool reached = (i < D.trigMask.size() && D.trigMask[i].any())
                                     || (i < D.autoAnchor.size() && D.autoAnchor[i] >= 0);
                if (!reached) continue;
                if (!lkOf.count(u) && !g_rotTailParts.count(u)) continue;
                cand.push_back(i);
                addS(u);
            }
            g_recEndTailTried = (int)cand.size();
            for (size_t q = 0; q < S.size(); ++q) {
                const auto rp = g_rotTailParts.find(S[q]);
                if (rp == g_rotTailParts.end()) continue;
                for (const RotTailPart& p : rp->second)
                    if (p.centreUid != 0) addS(p.centreUid);
            }
            // reset positions: a recorded object's first row, a centre's own row in the dump
            std::vector<double> px0(S.size(), 0.0), py0(S.size(), 0.0), pr0(S.size(), 0.0);
            std::vector<uint8_t> known(S.size(), 0);
            for (size_t q = 0; q < S.size(); ++q) {
                if (bad.count(S[q])) continue;
                const auto di = dynIdx.find(S[q]);
                if (di != dynIdx.end() && !D.samples[di->second].empty()) {
                    const DynSample& s0 = D.samples[di->second][0];
                    px0[q] = s0.cx; py0[q] = s0.cy; pr0[q] = s0.rot;
                    known[q] = 1;
                } else if (const auto rc = rotCentrePos.find(S[q]); rc != rotCentrePos.end()) {
                    px0[q] = rc->second.first; py0[q] = rc->second.second;
                    known[q] = 1;
                }
            }
            // A uid is simulated when it and every centre it turns about (and theirs) are.
            std::vector<int8_t> ok(S.size(), -1);
            std::function<bool(int, int)> okOf = [&](int q, int depth) -> bool {
                if (ok[(size_t)q] >= 0) return ok[(size_t)q] != 0;
                if (depth > 8 || !known[(size_t)q]) { ok[(size_t)q] = 0; return false; }
                bool r = true;
                if (const auto rp = g_rotTailParts.find(S[(size_t)q]); rp != g_rotTailParts.end())
                    for (const RotTailPart& p : rp->second) {
                        if (!trigIdx.count(p.trigUid)) { r = false; break; }
                        if (p.centreUid != 0 && !okOf(sIdx[p.centreUid], depth + 1)) { r = false; break; }
                    }
                ok[(size_t)q] = r ? 1 : 0;
                return r;
            };
            for (size_t q = 0; q < S.size(); ++q) okOf((int)q, 0);
            // the acts, over the simulated uids
            std::vector<Mv> mvs;
            std::vector<Lk> lks;
            struct Rt { int t0; double dur, total, erate; int ease; int c; double cx; int trigUid;
                        std::vector<std::pair<int, uint8_t>> mem; };
            std::unordered_map<int, Rt> rts;   // by Rotate uid
            for (size_t q = 0; q < S.size(); ++q) {
                if (ok[q] != 1) continue;
                if (const auto m = mvOf.find(S[q]); m != mvOf.end())
                    for (Mv v : m->second) { v.s = (int)q; mvs.push_back(v); }
                if (const auto l = lkOf.find(S[q]); l != lkOf.end())
                    for (Lk v : l->second) { v.s = (int)q; lks.push_back(v); }
                if (const auto rp = g_rotTailParts.find(S[q]); rp != g_rotTailParts.end())
                    for (const RotTailPart& p : rp->second) {
                        const AutoTrig& T = A[trigIdx[p.trigUid]];
                        const int cross = xRefCross(T.cx);
                        if (cross < 0) continue;
                        auto it = rts.find(p.trigUid);
                        if (it == rts.end())
                            it = rts.emplace(p.trigUid, Rt{cross + T.delay, p.durT, p.total, p.erate, p.ease,
                                                           p.centreUid ? sIdx[p.centreUid] : -1, p.trigCx,
                                                           p.trigUid, {}}).first;
                        it->second.mem.push_back({(int)q, p.lock});
                    }
            }
            std::vector<Rt> rot;
            for (auto& kv : rts) rot.push_back(std::move(kv.second));
            std::sort(rot.begin(), rot.end(), [](const Rt& a, const Rt& b) {
                return a.cx != b.cx ? a.cx < b.cx : a.trigUid < b.trigUid;
            });
            // what each candidate is held against: its rows over the last kRecEndTailCheck ticks
            const int globalEnd = D.tailRecEnd;
            struct Chk { size_t i; int q; int tEnd; size_t row; double offX = 0, offY = 0, offR = 0;
                         double worst = 0, worstR = 0, gap = 0; double ex = 0, ey = 0, er = 0; bool have = false; };
            std::vector<Chk> chk;
            int tMax = 0;
            for (const size_t i : cand) {
                const int q = sIdx[D.objs[i].uid];
                if (ok[(size_t)q] != 1) continue;
                const auto& sm = D.samples[i];
                Chk c{i, q, sm.back().t, 0};
                while (c.row < sm.size() && sm[c.row].t < c.tEnd - kRecEndTailCheck) ++c.row;
                chk.push_back(c);
                D.rigTail[i].t0 = c.tEnd;
                D.rigTail[i].x.reserve(kRecEndTailTicks + 1);
                tMax = std::max(tMax, c.tEnd + kRecEndTailTicks);
                for (const Lk& l : lks)
                    if (l.s == q) {
                        D.rigTail[i].lockA = std::min(D.rigTail[i].lockA, l.t0);
                        D.rigTail[i].lockB = std::max(D.rigTail[i].lockB, l.t1);
                    }
            }
            std::vector<double> P(S.size() * 2), R(S.size());
            for (size_t q = 0; q < S.size(); ++q) { P[2 * q] = px0[q]; P[2 * q + 1] = py0[q]; R[q] = pr0[q]; }
            // --recendauto: a slope's own angle is not turned (it still orbits): GD's recording of the ramp uid
            // 14699 holds 0.000 in all 9,392 rows while its group's Rotates (lockrot 0) turn the rest +-4 degrees.
            std::vector<uint8_t> noTurn(S.size(), 0);
            for (size_t q = 0; q < S.size(); ++q) {
                const auto di = dynIdx.find(S[q]);
                if (di != dynIdx.end() && di->second < D.bucket.size()
                    && D.bucket[di->second] == Dynamics::SLOPE)
                    noTurn[q] = 1;
            }
            auto eAt = [](int tk, int t0, double dur, int ease, double erate) {
                if (tk < t0) return 0.0;
                return dur > 0.0 ? gdEase(ease, erate, std::min(1.0, (double)(tk - t0) / dur)) : 1.0;
            };
            // the acts by start, swept into an active list
            std::sort(mvs.begin(), mvs.end(), [](const Mv& a, const Mv& b) { return a.t0 < b.t0; });
            std::sort(lks.begin(), lks.end(), [](const Lk& a, const Lk& b) { return a.t0 < b.t0; });
            std::vector<size_t> actMv, actLk;
            size_t nextMv = 0, nextLk = 0;
            double xPrev = xRefAt(1);
            for (int t = 2; t <= tMax && !chk.empty(); ++t) {
                const double xNow = xRefAt(t);
                while (nextMv < mvs.size() && mvs[nextMv].t0 <= t) actMv.push_back(nextMv++);
                while (nextLk < lks.size() && lks[nextLk].t0 < t) actLk.push_back(nextLk++);
                for (size_t a = 0; a < actMv.size();) {
                    const Mv& v = mvs[actMv[a]];
                    const int te = std::min(t, v.stopT), tp = std::min(t - 1, v.stopT);
                    const double de = eAt(te, v.t0, v.dur, v.ease, v.erate) - eAt(tp, v.t0, v.dur, v.ease, v.erate);
                    P[2 * (size_t)v.s] += v.dx * de;
                    P[2 * (size_t)v.s + 1] += v.dy * de;
                    if (t >= v.stopT || (double)(t - v.t0) >= v.dur) { actMv[a] = actMv.back(); actMv.pop_back(); }
                    else ++a;
                }
                for (size_t a = 0; a < actLk.size();) {
                    const Lk& l = lks[actLk[a]];
                    if (t <= l.t1) P[2 * (size_t)l.s] += xNow - xPrev;
                    if (t >= l.t1) { actLk[a] = actLk.back(); actLk.pop_back(); }
                    else ++a;
                }
                for (const Rt& r : rot) {
                    if (t < r.t0 || (r.dur > 0.0 ? (double)(t - 1 - r.t0) >= r.dur : t != r.t0)) continue;
                    const double d = r.total * (eAt(t, r.t0, r.dur, r.ease, r.erate)
                                                - eAt(t - 1, r.t0, r.dur, r.ease, r.erate));
                    if (d == 0.0) continue;
                    const double th = -d * (3.14159265358979 / 180.0);
                    const double cs = std::cos(th), sn = std::sin(th);
                    for (const auto& m : r.mem) {
                        const size_t q = (size_t)m.first;
                        if (r.c >= 0 && (size_t)r.c != q) {
                            const double ccx = P[2 * (size_t)r.c], ccy = P[2 * (size_t)r.c + 1];
                            const double rx = P[2 * q] - ccx, ry = P[2 * q + 1] - ccy;
                            P[2 * q] = ccx + cs * rx - sn * ry;
                            P[2 * q + 1] = ccy + sn * rx + cs * ry;
                        }
                        if (!m.second && !noTurn[q]) R[q] += d;
                    }
                }
                xPrev = xNow;
                for (Chk& c : chk) {
                    const auto& sm = D.samples[c.i];
                    const double sx = P[2 * (size_t)c.q], sy = P[2 * (size_t)c.q + 1], sr = R[(size_t)c.q];
                    while (c.row < sm.size() && sm[c.row].t < t) ++c.row;
                    if (c.row < sm.size() && sm[c.row].t == t) {
                        const double ex = sm[c.row].cx - sx, ey = sm[c.row].cy - sy, er = sm[c.row].rot - sr;
                        if (!c.have) { c.ex = ex; c.ey = ey; c.er = er; c.have = true; }
                        c.worst = std::max({c.worst, std::fabs(ex - c.ex), std::fabs(ey - c.ey)});
                        c.worstR = std::max(c.worstR, std::fabs(er - c.er));
                        if (t == c.tEnd) { c.offX = ex; c.offY = ey; c.offR = er; }
                    }
                    if (t >= c.tEnd) {
                        Dynamics::RigTail& T = D.rigTail[c.i];
                        if (T.x.size() <= (size_t)kRecEndTailTicks) {
                            T.x.push_back((float)(sx + c.offX));
                            T.y.push_back((float)(sy + c.offY));
                            T.r.push_back((float)(sr + c.offR));
                        }
                        if (t <= globalEnd && !T.x.empty())
                            c.gap = std::max({c.gap, std::fabs((double)T.x.back() - (double)T.x.front()),
                                              std::fabs((double)T.y.back() - (double)T.y.front())});
                    }
                }
            }
            // Armed where the rows agree with ONE offset (the spread against the first checked row's is the
            // spread about the last one's too, within twice the tolerance -- the test reads the first), the
            // offset is small, and the simulation stood still where the recording did.
            for (const Chk& c : chk) {
                Dynamics::RigTail& T = D.rigTail[c.i];
                // --recendheldrot: every recorded row at one angle -- GD did not turn it, whatever the rig says
                bool heldRot = false;
                if (g_recEndHeldRot && c.worstR > kRecEndTailRotTol) {
                    const auto& sm = D.samples[c.i];
                    heldRot = std::all_of(sm.begin(), sm.end(), [&](const DynSample& s) {
                        return std::fabs((double)s.rot - (double)sm.back().rot) < 0.001;
                    });
                }
                const bool armed = c.have && T.x.size() > 1 && c.worst <= kRecEndTailTol
                                   && (c.worstR <= kRecEndTailRotTol || heldRot)
                                   && std::hypot(c.offX, c.offY) <= kRecEndTailOff
                                   && c.gap <= kRecEndTailTol;
                if (armed && heldRot) std::fill(T.r.begin(), T.r.end(), D.samples[c.i].back().rot);
                if (g_dynDbg == D.objs[c.i].uid)
                    loadPrintf("recendtail: uid %d last row t=%d off=(%.3f,%.3f,%.3f) spread=%.3f/%.3fdeg "
                               "gap=%.3f lock=[%d,%d] -> %s\n", D.objs[c.i].uid, c.tEnd, c.offX, c.offY, c.offR,
                               c.worst, c.worstR, c.gap, T.lockA, T.lockB, armed ? "armed" : "not armed");
                if (!armed) { T = Dynamics::RigTail{}; continue; }
                ++g_recEndTailN;
            }
        }
    }
    // --tapdisarm: the objects whose on/off asks a box whether it switched a trigger off (Dynamics::disarmDep). The
    // same tick's second group would otherwise be handed the first group's answer for them (isPure).
    if (g_tapDisarm && !L.dyn.objs.empty()) {
        Dynamics& D = L.dyn;
        auto disarmable = [](const OnEvent& o) {
            return o.autoIdx >= 0 && (size_t)o.autoIdx < g_autoDisarm.size()
                   && !g_autoDisarm[(size_t)o.autoIdx].empty();
        };
        D.disarmDep.assign(D.size(), 0);
        size_t n = 0, noBox = 0;   // noBox: no box reaches them, so they were placed once per tick before
        for (size_t i = 0; i < D.size(); ++i) {
            bool dep = i < D.disarmEv.size() && !D.disarmEv[i].empty();
            if (!dep && i < D.tailOnEv.size())
                dep = std::any_of(D.tailOnEv[i].begin(), D.tailOnEv[i].end(), disarmable);
            D.disarmDep[i] = dep ? 1 : 0;
            n += dep ? 1 : 0;
            if (dep && !(i < D.trigMask.size() && D.trigMask[i].any())) {
                ++noBox;
                if (g_dynDbg == -6) loadPrintf("tapdisarm: uid %d is reached by no box\n", D.objs[i].uid);
            }
        }
        if (n)
            loadPrintf("tapdisarm: %zu object(s) are placed for every group (their on/off reads a box), %zu of them "
                       "reached by no box\n", n, noBox);
    }
    return L;
}

// When this is not empty, the level is parsed FROM IT and no file is opened. The mod fills it
// with the table it built out of PlayLayer, so an in-process solve needs nothing on disk. The
// CLI never touches it, which is why its behaviour is unchanged (equiv suite).
inline std::string g_levelCsv;

// ---- the level's own compatibility flags --------------------------------
// `levelsettings.txt`, written beside objrects by the mod (src/solver/
// solver.hpp). Absent file = every flag 0, which is what 21 of the 22 official
// levels actually have, so nothing changes for a caller that never passes one.
//
// The file is `key=value` per line. Unknown keys are ignored rather than
// rejected: the dump grows a key whenever the survey finds another flag worth
// carrying, and an older solver should not stop reading because of it.
inline bool loadLevelSettings(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string ln;
    while (std::getline(in, ln)) {
        const size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = ln.substr(0, eq);
        const int v = std::atoi(ln.c_str() + eq + 1);
        if (k == "fixRadiusCollision")      g_fixRadiusCollision = v;
        else if (k == "fixGravityBug")      g_fixGravityBug = v;
        else if (k == "fixNegativeScale")   g_fixNegativeScale = v;
        else if (k == "fixRobotJump")       g_fixRobotJump = v;
        else if (k == "dynamicLevelHeight") g_dynamicLevelHeight = v;
        else if (k == "platformerMode")     g_platformerMode = v;
        else if (k == "endPortalX")         g_endPortalX = std::atof(ln.c_str() + eq + 1);   // --goalgd
        else if (k == "endPortalFlipX")     g_endPortalFlipX = v;
        else if (k == "reverseSync")        g_reverseSync = v;   // revReflect (step.hpp)
        else if (k == "twoPlayer")          g_twoPlayer = v;   // frames.hpp: a second button, no gravity link
    }
    return true;
}

// Where the settings live when nobody said: beside the objrects dump, with the
// same suffix. `objrects.txt` -> `levelsettings.txt` for the mod's data dir,
// `objrects_lv22.txt` -> `levelsettings_lv22.txt` for the lab's. Returns an
// empty string when the argument is not a path at all -- the in-process caller
// passes the level in memory and argv[1] is a placeholder, so THAT caller has
// to pass --levelsettings explicitly.
inline std::string settingsPathBeside(const std::string& objrectsPath) {
    const size_t at = objrectsPath.rfind("objrects");
    if (at == std::string::npos) return std::string();
    return objrectsPath.substr(0, at) + "levelsettings"
           + objrectsPath.substr(at + 8);
}
// ...and the same for the trigger queue, so every caller gets it without being
// taught a new flag. The mod writes rotgameplay_lvN.txt beside objrects_lvN.txt
// and 21 of the 22 files are a bare header.
inline std::string rotQPathBeside(const std::string& objrectsPath) {
    const size_t at = objrectsPath.rfind("objrects");
    if (at == std::string::npos) return std::string();
    return objrectsPath.substr(0, at) + "rotgameplay"
           + objrectsPath.substr(at + 8);
}
// ...and the same for the force blocks' IDs, for a reason the other two did not
// have to state. The mod hands the loop --forceids; until this existed the CLI
// did not take it unless told, so quick_regress, fixcensus, refaudit and
// deathref were all measuring a level the loop no longer played -- lv22's
// t=20,961 family sat in the census as a model defect when the loop had already
// lost it. An input one side passes and the other does not is an input the
// instruments stop measuring. Returns empty when the file is not there, so a
// level without one behaves exactly as before rather than logging a failure:
// 21 of the 22 official files are a bare header.
inline std::string forceIdsPathBeside(const std::string& objrectsPath) {
    const size_t at = objrectsPath.rfind("objrects");
    if (at == std::string::npos) return std::string();
    const std::string p = objrectsPath.substr(0, at) + "forceblocks"
                          + objrectsPath.substr(at + 8);
    std::ifstream probe(p);
    return probe ? p : std::string();
}

// The globals loadLevelFrom writes, as it left them: taken after one load and put back in place
// of another from the same inputs (the ladder's level cache, cli.hpp). Found by reading the
// function for assignments, container writes, `++`, sorts and non-const references to g_ names
// (2026-09-26); its callees (bandHeightFor, envTwinUid, forceUnitFor, gdEase, isSpeedId,
// recordLag, rotStep, touchBit) write none. g_rotSpec is here because the orbit stage arms its
// entries in place.
struct LoadLevelWrites {
    decltype(g_forceFields) forceFields;
    decltype(g_forceBoxes) forceBoxes;
    decltype(g_flipHeadBoxes) flipHeadBoxes;
    decltype(g_dashStopBoxes) dashStopBoxes;
    decltype(g_ctrlTouchBoxes) ctrlTouchBoxes;
    decltype(g_pcTrigs) pcTrigs;
    decltype(g_pcTouchBoxes) pcTouchBoxes;
    decltype(g_revTrigs) revTrigs;
    decltype(g_timeWarps) timeWarps;
    decltype(g_tpTrigs) tpTrigs;
    decltype(g_tpSpawned) tpSpawned;
    decltype(g_gravTrigs) gravTrigs;
    decltype(g_gravSpawned) gravSpawned;
    decltype(g_gravTouchBoxes) gravTouchBoxes;
    decltype(g_tpTouchBoxes) tpTouchBoxes;
    decltype(g_camFrees) camFrees;
    decltype(g_zoomTrigs) zoomTrigs;
    decltype(g_staticCams) staticCams;
    decltype(g_unlinkTrigs) unlinkTrigs;
    decltype(g_ctrlTrigs) ctrlTrigs;
    decltype(g_armBoxes) armBoxes;
    decltype(g_slideBoxes) slideBoxes;
    decltype(g_rotTrig) rotTrig;
    decltype(g_rotSpec) rotSpec;
    decltype(g_formulaDriven) formulaDriven{};
    decltype(g_freeModeCol) freeModeCol{};
    decltype(g_trigGateCol) trigGateCol{};
    decltype(g_staticCamCol) staticCamCol{};
    decltype(g_lockBox) lockBox{};
    decltype(g_lockTicks) lockTicks{};
    decltype(g_rotSeen) rotSeen{};
    decltype(g_rotRouted) rotRouted{};
    void take() {
        forceFields = g_forceFields;
        forceBoxes = g_forceBoxes;
        flipHeadBoxes = g_flipHeadBoxes;
        dashStopBoxes = g_dashStopBoxes;
        ctrlTouchBoxes = g_ctrlTouchBoxes;
        pcTrigs = g_pcTrigs;
        pcTouchBoxes = g_pcTouchBoxes;
        revTrigs = g_revTrigs;
        timeWarps = g_timeWarps;
        tpTrigs = g_tpTrigs;
        tpSpawned = g_tpSpawned;
        gravTrigs = g_gravTrigs;
        gravSpawned = g_gravSpawned;
        gravTouchBoxes = g_gravTouchBoxes;
        tpTouchBoxes = g_tpTouchBoxes;
        camFrees = g_camFrees;
        zoomTrigs = g_zoomTrigs;
        staticCams = g_staticCams;
        unlinkTrigs = g_unlinkTrigs;
        ctrlTrigs = g_ctrlTrigs;
        armBoxes = g_armBoxes;
        slideBoxes = g_slideBoxes;
        rotTrig = g_rotTrig;
        rotSpec = g_rotSpec;
        formulaDriven = g_formulaDriven;
        freeModeCol = g_freeModeCol;
        trigGateCol = g_trigGateCol;
        staticCamCol = g_staticCamCol;
        lockBox = g_lockBox;
        lockTicks = g_lockTicks;
        rotSeen = g_rotSeen;
        rotRouted = g_rotRouted;
    }
    void put() const {
        g_forceFields = forceFields;
        g_forceBoxes = forceBoxes;
        g_flipHeadBoxes = flipHeadBoxes;
        g_dashStopBoxes = dashStopBoxes;
        g_ctrlTouchBoxes = ctrlTouchBoxes;
        g_pcTrigs = pcTrigs;
        g_pcTouchBoxes = pcTouchBoxes;
        g_revTrigs = revTrigs;
        g_timeWarps = timeWarps;
        g_tpTrigs = tpTrigs;
        g_tpSpawned = tpSpawned;
        g_gravTrigs = gravTrigs;
        g_gravSpawned = gravSpawned;
        g_gravTouchBoxes = gravTouchBoxes;
        g_tpTouchBoxes = tpTouchBoxes;
        g_camFrees = camFrees;
        g_zoomTrigs = zoomTrigs;
        g_staticCams = staticCams;
        g_unlinkTrigs = unlinkTrigs;
        g_ctrlTrigs = ctrlTrigs;
        g_armBoxes = armBoxes;
        g_slideBoxes = slideBoxes;
        g_rotTrig = rotTrig;
        g_rotSpec = rotSpec;
        g_formulaDriven = formulaDriven;
        g_freeModeCol = freeModeCol;
        g_trigGateCol = trigGateCol;
        g_staticCamCol = staticCamCol;
        g_lockBox = lockBox;
        g_lockTicks = lockTicks;
        g_rotSeen = rotSeen;
        g_rotRouted = rotRouted;
    }
};

// The CLI's way in: the same parse, reading the dump the mod wrote to disk.
inline Level loadLevel(const std::string& path, const GroupTimeline* gt = nullptr,
                const std::vector<TouchTrig>* tt = nullptr,
                const std::vector<AutoTrig>* at = nullptr,
                const CsvRows* kept = nullptr) {
    if (!g_levelCsv.empty()) {
        if (kept && !g_glitchDeco) {   // the kept text pass of g_levelCsv (CsvRows): no stream, no copy of the text
            std::istringstream none;
            return loadLevelFrom(none, gt, tt, at, kept, &g_levelCsv);
        }
        std::istringstream in(g_levelCsv);
        return loadLevelFrom(in, gt, tt, at);
    }
    std::ifstream in(path);
    return loadLevelFrom(in, gt, tt, at);
}

// The coin uids alone, for the ONE caller that needs them before the level is
// parsed: the touch window is built first (cli.hpp), and it has to know which
// Count triggers reach a coin before it decides what to drop. Same two columns
// and the same test as the row loop above -- see the note beside it.
//
// Why it matters that this is cheap rather than a second loadLevel: a Count
// root moves what its chain names, and on lv21 the ten Counts the level uses
// for its 'n of 10' readout moved some 350 objects the model otherwise never
// touches. The same plan that GD flies to x=21,840 died at x=14,245 -- with
// --coins on, and only with it. A flag for bookkeeping must not move the level.
inline std::unordered_set<int> coinUidsFrom(std::istream& in) {
    std::unordered_set<int> out;
    std::string line;
    if (!std::getline(in, line)) return out;   // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        // COLUMN 7 IS THE UID, column 0 is the object ID -- the header reads
        // `id,type,cx,cy,w,h,groups,uid,...`. Read as f[0] this returned the
        // IDs, and since all 66 official coins share id 142 the set collapsed
        // to one element: "coins: 1 coin uids" on a level with three. The count
        // is printed for exactly that reason -- it is the cheap check that the
        // column is the one the header names.
        std::string f[8];
        for (int i = 0; i < 8 && std::getline(ss, f[i], ','); ++i) {}
        if (f[1].empty() || f[7].empty()) continue;
        const int type = std::atoi(f[1].c_str());
        if (type == 22 || type == 31) out.insert(std::atoi(f[7].c_str()));
    }
    return out;
}

inline std::unordered_set<int> coinUids(const std::string& path) {
    if (!g_levelCsv.empty()) {
        std::istringstream in(g_levelCsv);
        return coinUidsFrom(in);
    }
    std::ifstream in(path);
    return coinUidsFrom(in);
}

}  // namespace dp
