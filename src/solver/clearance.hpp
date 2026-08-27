#pragma once
// ============================================================
// Derivation of the corridor clearance table (cfg `clearance=1`)
//
// Port of the upstream GD-approx-physics tools/extract_geometry.py (the
// Python stays upstream for offline cross-checking). The derivation has 2 stages
// (identical to upstream):
//  1. Contacts give hy as an equality: the distance at the moment GD pressed the
//     player against a surface and stopped it is the dimension
//     (contacts with the same id give exactly the same value)
//  2. Surviving positions give hx: hx = min{ |dx| : survivors with |dy| < hy }
//     = the soundness condition "never block a position GD has shown to be safe"
//
// A lethal position left unblocked costs one GD verification; a safe position
// that got blocked loses the solution. So always err towards under-blocking.
//
// The table is per mode: the blocked region is "object size + player size",
// which differs per mode. Mixing modes collapses hx.
//
// Implementation note: hx is monotonically decreasing in hy, so finalising hx
// incrementally drifts towards over-blocking when hy grows later. Keep min dx
// per dy bucket and take it after hy is finalised. Buckets err towards
// "including the boundary" to stay on the under-blocking side.
// ============================================================
namespace clearance {

inline bool g_on = false;
inline uint8_t g_mode = 1;          // target mode (default=ship). cfg `clearancemode=`
constexpr double RADIUS = 70.0;     // Chebyshev radius for collecting survivor samples
constexpr double DX_MAX = 22.0;     // contact test: max horizontal offset to count as above/below
constexpr double D_MAX = 60.0;      // same, max vertical distance
constexpr double DY_Q = 0.1;        // dy bucket width
constexpr int DY_BUCKETS = (int)(RADIUS / DY_Q) + 2;
constexpr double CAP = 30.0;        // upper bound on hx (same as upstream)

struct Entry {
    int id = 0;
    int type = 0;                   // 0=solid 2=hazard
    double bestCheb = 1e9;          // isotropic minimum distance (hy when there is no contact)
    long long samples = 0;
    std::vector<float> minDxForDy;  // dy bucket -> min|dx| within that bucket
    std::map<int, int> contactHist;  // round(d*100) -> count (take the mode of hy)
};
inline std::map<std::pair<int,int>, Entry> g_table;

// Index bucketing x in 60px units (same as upstream). Without it, every tick
// would do a distance computation against every object
struct Bucketed {
    std::map<int, std::vector<const solver::Obj*>> byX;
    int type = 0;
    bool built = false;
};
inline Bucketed g_solidIdx{{}, 0, false};
inline Bucketed g_hazardIdx{{}, 2, false};

inline void buildIndex(Bucketed& b, const std::vector<solver::Obj>& v, int type) {
    if (b.built) return;
    b.byX.clear(); b.type = type;
    for (auto& o : v) b.byX[(int)std::floor(o.x / 60.0)].push_back(&o);
    b.built = true;
}

inline void reset() {
    g_table.clear();
    g_solidIdx.byX.clear(); g_solidIdx.built = false;
    g_hazardIdx.byX.clear(); g_hazardIdx.built = false;
}

inline Entry& entryFor(int id, int type) {
    auto& e = g_table[{id, type}];
    if (e.minDxForDy.empty()) {
        e.id = id; e.type = type;
        e.minDxForDy.assign(DY_BUCKETS, 1e9f);
    }
    return e;
}

// Exclusion width (ticks) so that the moments just before death are not counted
// as "surviving positions". If the pre-death part of a dying attempt (where GD's
// collision resolution has put the player inside the box) leaks into the survivor
// samples, hx collapses and nothing gets blocked. Larger values err towards
// over-blocking, so keep it to the necessary minimum
inline int g_deathTail = 60;

// Collect samples from the trajectory of one run.
// D = death tick (for a cleared run pass the end of the log). died=false treats
// the tail as surviving too
inline void observe(const std::vector<solver::TickInfo>& log, long long D, bool died = true) {
    if (!g_on || log.empty()) return;
    long long last = std::min<long long>(D, (long long)log.size()) - 1;
    if (died) last -= g_deathTail;
    if (last < 1) return;
    buildIndex(g_solidIdx, solver::g_solids, 0);
    buildIndex(g_hazardIdx, solver::g_hazards, 2);
    // Look only at the sections in the target mode
    for (long long t = 1; t <= last; ++t) {
        const auto& r = log[t];
        if (r.mode != g_mode) continue;
        if (r.x < 30.f) continue;
        // --- contact: the moment vy pins to 0. It must have been moving just before ---
        const auto& p = log[t - 1];
        if (std::abs(r.yvel) < 1e-9f && std::abs(p.yvel) > 0.5f) {
            bool wantAbove = p.yvel < 0;   // stopped while falling = obstacle is below
            double bestD = 1e9; int bestId = -1, bestType = 0;
            auto scan = [&](Bucketed& idx) {
                int k = (int)std::floor(r.x / 60.0);
                for (int kk = k - 1; kk <= k + 1; ++kk) {
                    auto it = idx.byX.find(kk);
                    if (it == idx.byX.end()) continue;
                    for (auto* o : it->second) {
                        if (std::abs(o->x - r.x) > DX_MAX) continue;
                        double d = wantAbove ? (r.y - o->y) : (o->y - r.y);
                        if (d > 0 && d < D_MAX && d < bestD) {
                            bestD = d; bestId = o->id; bestType = idx.type;
                        }
                    }
                }
            };
            scan(g_solidIdx);
            scan(g_hazardIdx);
            if (bestId >= 0)
                ++entryFor(bestId, bestType).contactHist[(int)std::lround(bestD * 100.0)];
        }
        // --- survival: record (dx, dy) against every nearby obstacle ---
        auto collect = [&](Bucketed& idx) {
            int k = (int)std::floor(r.x / 60.0);
            for (int kk = k - 2; kk <= k + 2; ++kk) {
                auto it = idx.byX.find(kk);
                if (it == idx.byX.end()) continue;
                for (auto* o : it->second) {
                    double dx = std::abs(o->x - r.x), dy = std::abs(o->y - r.y);
                    if (dx > RADIUS || dy > RADIUS) continue;
                    auto& e = entryFor(o->id, idx.type);
                    ++e.samples;
                    double cheb = std::max(dx, dy);
                    if (cheb < e.bestCheb) e.bestCheb = cheb;
                    int b = (int)(dy / DY_Q);
                    if (b >= 0 && b < DY_BUCKETS && dx < e.minDxForDy[b])
                        e.minDxForDy[b] = (float)dx;
                }
            }
        };
        collect(g_solidIdx);
        collect(g_hazardIdx);
    }
}

inline const char* typeName(int t) { return t == 0 ? "solid" : (t == 2 ? "hazard" : "other"); }

// Emit in the same format as the upstream fixtures (so they can be cross-checked)
inline void write(int levelId) {
    if (!g_on || g_table.empty()) return;
    const char* mn = (g_mode == 0) ? "cube" : (g_mode == 1) ? "ship"
                   : (g_mode == 2) ? "ball" : (g_mode == 3) ? "ufo" : "other";
    char name[192];
    snprintf(name, sizeof(name), "%s/clearance_lv%d_%s.csv", DATA_DIR, levelId, mn);
    std::ofstream f(name, std::ios::trunc);
    if (!f) return;
    f << "# half_extent = smallest Chebyshev distance from a SURVIVING player\n"
      << "# centre to an object of this ID. Blocking a square of this size\n"
      << "# around each object cannot block any position GD has shown safe.\n"
      << "# It UNDER-blocks: some lethal positions stay unblocked, which only\n"
      << "# costs extra GD verifications.\n"
      << "# derived in-mod (clearance=1), mode=" << mn << "\n"
      << "object_id,type,type_name,half_extent,half_x,half_y,samples\n";
    // Output order follows upstream: most samples first
    std::vector<const Entry*> rows;
    for (auto& kv : g_table) if (kv.second.samples > 0) rows.push_back(&kv.second);
    std::sort(rows.begin(), rows.end(),
              [](const Entry* a, const Entry* b) { return a->samples > b->samples; });
    for (auto* e : rows) {
        // hy: the mode of the contacts if there are any, otherwise the isotropic lower bound
        double hy = e->bestCheb;
        if (!e->contactHist.empty()) {
            int bestCount = -1, bestKey = 0;
            for (auto& c : e->contactHist)
                if (c.second > bestCount) { bestCount = c.second; bestKey = c.first; }
            hy = bestKey / 100.0;
        }
        // hx = min{ dx : dy < hy }. Buckets err towards including the boundary
        // (= under-blocking = safe)
        double hx = CAP;
        int upto = (int)(hy / DY_Q);
        for (int b = 0; b <= upto && b < DY_BUCKETS; ++b)
            if (e->minDxForDy[b] < hx) hx = e->minDxForDy[b];
        if (hx < 0) hx = 0;
        char line[256];
        snprintf(line, sizeof(line), "%d,%d,%s,%.4f,%.4f,%.4f,%lld\n",
                 e->id, e->type, typeName(e->type),
                 e->bestCheb, hx, hy, e->samples);
        f << line;
    }
    f.close();
    writeResult(std::string("clearance: wrote ") + name + " ("
                + std::to_string(rows.size()) + " object ids, mode=" + mn + ")");
}

} // namespace clearance
