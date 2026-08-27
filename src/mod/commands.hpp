#pragma once
// cmd.txt / hud.txt polling (pause, resume, step, rerun).
#include "mod/postmortem.hpp"

namespace p1 {

inline void pollCommandFileImpl(const std::string& cmd);

// Receive DP-mode progress from the driver (data/hud.txt, read at ~5Hz).
// Format: "iter=<n> verified=<x> anchort=<t> anchorx=<x> phase=<text>"
inline void pollHudFile() {
    auto path = std::string(DATA_DIR) + "/hud.txt";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line)) return;
    auto num = [&](const char* key, double& out) {
        auto p = line.find(key);
        if (p == std::string::npos) return;
        out = std::atof(line.c_str() + p + std::strlen(key));
    };
    double it = g_hudIter, vx = g_hudVerifiedX, at = (double)g_hudAnchorT,
           ax = g_hudAnchorX;
    num("iter=", it);
    num("verified=", vx);
    num("anchort=", at);
    num("anchorx=", ax);
    g_hudIter = (int)it;
    g_hudVerifiedX = (float)vx;
    g_hudAnchorT = (long long)at;
    g_hudAnchorX = (float)ax;
    // fixups=n/cap (placed before phase= — phase reads to the end of the line)
    auto q = line.find("fixups=");
    if (q != std::string::npos) {
        g_hudFixups = std::atoi(line.c_str() + q + 7);
        auto s = line.find('/', q);
        if (s != std::string::npos) g_hudFixupCap = std::atoi(line.c_str() + s + 1);
    }
    auto p = line.find("phase=");
    if (p != std::string::npos) g_hudPhase = line.substr(p + 6);
}

inline void pollCommandFile() {
    static int s_counter = 0;
    if (++s_counter % 30 != 0) return; // automation channel: ~5Hz is enough
    if (g_serveMode) pollHudFile();
    auto path = std::string(DATA_DIR) + "/cmd.txt";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    std::ifstream f(path);
    std::string cmd;
    std::getline(f, cmd);
    f.close();
    std::filesystem::remove(path, ec);
    pollCommandFileImpl(cmd);
}


// Interval of the heap check (in attempts). 0=disabled. The implementation is postmortem::heapOk()
inline long long g_heapCheckEvery = 0;

}  // namespace p1
