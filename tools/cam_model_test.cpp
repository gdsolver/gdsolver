// cam_model_test <flight dir> <level.lvl>: dp/src/dp/cam_model.hpp (camera X) along the flight's own x track against the recorded camtrace.csv: bit-exact fractions of the zoom and of camPX, and the tick ranges it is off.
#include "dp/cam_model.hpp"
#include <cstring>
#include <map>

static std::vector<std::string> split(const std::string& s) { std::vector<std::string> o; size_t p = 0; while (p <= s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); o.push_back(s.substr(p, q - p)); p = q + 1; } return o; }

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: cam_model_test <flight dir> <level.lvl>\n"); return 2; }
    const std::string fl = argv[1];
    dp::gdcam::CamModel M;
    for (int i = 3; i < argc; ++i) if (!std::strcmp(argv[i], "--nounknown")) M.useUnknown = false;
    if (!M.load(argv[2])) { std::printf("cannot read %s\n", argv[2]); return 2; }
    std::map<long long, double> X;
    {
        std::ifstream f(fl + "/dump.csv"); std::string l; std::getline(f, l);
        const auto h = split(l); int cT = -1, cX = -1;
        for (size_t i = 0; i < h.size(); ++i) { if (h[i] == "tick") cT = (int)i; if (h[i] == "x") cX = (int)i; }
        while (std::getline(f, l)) { const auto c = split(l); if ((int)c.size() > std::max(cT, cX)) X[std::atoll(c[cT].c_str())] = std::atof(c[cX].c_str()); }
    }
    struct Rec { float px, zoom; std::string line; };
    std::map<long long, Rec> R;
    {
        std::ifstream f(fl + "/camtrace.csv"); std::string l; std::getline(f, l);
        while (std::getline(f, l)) { const auto c = split(l); if (c.size() >= 7) R[std::atoll(c[1].c_str())] = {(float)std::atof(c[2].c_str()), (float)std::atof(c[4].c_str()), l}; }
    }
    long long atA = -1, atB = -1, atS = 1;
    for (int i = 3; i + 2 < argc; ++i) if (!std::strcmp(argv[i], "--at")) { atA = std::atoll(argv[i + 1]); atB = std::atoll(argv[i + 2]); atS = i + 3 < argc ? std::atoll(argv[i + 3]) : 1; }
    std::string emitPath; for (int i = 3; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--emit")) emitPath = argv[i + 1];
    std::ofstream emit; if (!emitPath.empty()) { emit.open(emitPath); emit << "attempt,tick,camPX,camPY,zoom,layerX,layerY\n"; }
    // --live <csv> <group>: the rows (tick,x,y; a row when the object moved) of the first object of a follow Static's target group, from the same flight's grouptrace -- what the shadow hands to the model in the dp
    std::map<long long, std::pair<float, float>> LV; int liveG = 0;
    for (int i = 3; i + 2 < argc; ++i) if (!std::strcmp(argv[i], "--live")) {
        std::ifstream f(argv[i + 1]); std::string l; std::getline(f, l); liveG = std::atoi(argv[i + 2]);
        while (std::getline(f, l)) { const auto c = split(l); if (c.size() >= 3) LV[std::atoll(c[0].c_str())] = {(float)std::atof(c[1].c_str()), (float)std::atof(c[2].c_str()) - 90.0f}; }
    }
    dp::gdcam::CamState s;
    long long n = 0, zb = 0, pb = 0, p3 = 0, p2 = 0, p05 = 0, unc = 0, nc = 0, pbc = 0, p3c = 0;
    std::vector<std::pair<long long, long long>> bad;   // ranges where certain-camera ticks are off by >= 0.5
    long long rs = -1, re = -1, offZoomOk = 0, offZoomBad = 0;
    for (const auto& kv : X) {
        const long long t = kv.first;
        const auto r = R.find(t);
        float z = 1.0f;
        dp::gdcam::CamLive lv;
        if (liveG > 0 && !LV.empty()) { auto it = LV.upper_bound(t); if (it != LV.begin()) { --it; lv.okPos = true; lv.pg = liveG; lv.px = it->second.first; lv.py = it->second.second; } }
        float px = M.step(s, kv.second, &z);
        if (s.liveNeed || s.liveYNeed) M.resolveLive(s, lv, &px);   // the dp steps the shadow between the two; here the recorded row of the same tick stands for it
        if (r == R.end()) continue;
        if (emit.is_open()) {   // the HYBRID camtrace: the model's camera on the ticks it calls certain, the recording's on the others (what a perfect fallback gives; for trig_replay --camtrace)
            if (!s.uncertain) { char buf[160]; std::snprintf(buf, sizeof buf, "1,%lld,%.9g,0,%.9g,0,0\n", t, (double)px, (double)z); emit << buf; }
            else emit << r->second.line << "\n";
        }
        if (atA >= 0 && t >= atA && t <= atB && (t - atA) % atS == 0)
            std::printf("t=%lld x=%.4f zoom rec %.9g model %.9g | camPX rec %.6f model %.6f diff %.6f | uncertain %d staticX %d edgeL %d edgeR %d offX %.4f\n", t, kv.second, (double)r->second.zoom, (double)z, (double)r->second.px, (double)px, (double)px - (double)r->second.px, (int)s.uncertain, (int)s.staticX, (int)s.edgeX[1], (int)s.edgeX[2], (double)s.offX.v);
        ++n;
        const bool zeq = z == r->second.zoom;
        zb += zeq;
        const double e = std::fabs((double)px - (double)r->second.px);
        pb += px == r->second.px; p3 += e < 1e-3; p2 += e < 0.01; p05 += e < 0.5;
        if (s.uncertain) { ++unc; continue; }
        ++nc; pbc += px == r->second.px; p3c += e < 1e-3;
        if (e >= 1e-3) { if (zeq) ++offZoomOk; else ++offZoomBad; }
        if (e >= 0.5) { if (rs < 0 || t > re + 60) { if (rs >= 0) bad.push_back({rs, re}); rs = t; } re = t; }
    }
    if (rs >= 0) bad.push_back({rs, re});
    std::printf("ticks %lld: zoom bit-exact %.4f; camPX bit-exact %.4f, <1e-3 %.4f, <0.01 %.4f, <0.5 %.4f | uncertain (a Static Camera / Edge crossed) %lld ticks; on the certain %lld ticks: camPX bit-exact %.4f, <1e-3 %.4f\n",
                n, n ? (double)zb / n : 0.0, n ? (double)pb / n : 0.0, n ? (double)p3 / n : 0.0, n ? (double)p2 / n : 0.0, n ? (double)p05 / n : 0.0, unc, nc, nc ? (double)pbc / nc : 0.0, nc ? (double)p3c / nc : 0.0);
    std::printf("certain ticks off by >= 1e-3: %lld with the model zoom bit-equal to the recorded (the camera X rule or an unmodelled mode), %lld with a different zoom (a zoom the model did not get: spawn / touch driven)\n", offZoomOk, offZoomBad);
    std::printf("certain-camera ranges off by >= 0.5 px (gap 60): %zu:", bad.size());
    for (size_t i = 0; i < bad.size() && i < 12; ++i) std::printf(" %lld-%lld", bad[i].first, bad[i].second);
    std::printf("\n");
    return 0;
}
