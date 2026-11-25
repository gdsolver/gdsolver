// call_compare: the port's answer next to GD's own, for one gd_call (mcp/gdmcp/tools.py, src/mod/callverify.hpp).
//   call_compare updateSlopeRotation <player_in.hex> <player_out.hex> [dt]
// <player_in.hex> is the scratch player's bytes as GD was entered with them (call_out.txt's `player_in`), <player_out.hex> what GD left (`player`). The port runs on a copy of the first and
// the result is compared with the second word by word; the words that hold pointers (the scratch object's own) and the sprite-side words are not compared.
//   cl /std:c++17 /O2 /EHsc /I dp/src tools/call_compare.cpp
#include "dp/upd.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dp::gdcol;

static std::vector<uint8_t> readHex(const char* path) {
    std::ifstream f(path);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    std::vector<uint8_t> b(s.size() / 2);
    auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)(v(s[2 * i]) * 16 + v(s[2 * i + 1]));
    return b;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: call_compare updateSlopeRotation <player_in.hex> <player_out.hex> [dt]\n"); return 2; }
    const std::string fn = argv[1];
    const auto in = readHex(argv[2]), out = readHex(argv[3]);
    const float dt = argc > 4 ? (float)std::atof(argv[4]) : 0.25f;
    if (in.size() < 0xa20 || out.size() != in.size()) { std::printf("status: UNOBSERVED (sizes %zu / %zu)\n", in.size(), out.size()); return 3; }
    Mem P(std::max<size_t>(in.size(), 0xc80));
    std::copy(in.begin(), in.end(), P.b.begin());
    int portRet = -1;
    if (fn == "updateSlopeRotation") upd::updateSlopeRotation(P, dt);
    else if (fn == "collidedWithObjectInternal") {
        // call_compare collidedWithObjectInternal in out dt rx ry rw rh gdRet: the rect call (the object is not remembered, as in the port's rect path)
        if (argc < 10) { std::fprintf(stderr, "usage: call_compare collidedWithObjectInternal <in> <out> dt rx ry rw rh gdRet\n"); return 2; }
        const float rect[4] = {(float)std::atof(argv[5]), (float)std::atof(argv[6]), (float)std::atof(argv[7]), (float)std::atof(argv[8])};
        Ctx c(P);
        GObj dummy;
        if (argc >= 14) {
            // ... gdRet cx cy prevCx prevCy [type]: the object path -- the rect is the object's own and the object's centre / last centre are what the call reads of it
            dummy.rx = rect[0]; dummy.ry = rect[1]; dummy.rw = rect[2]; dummy.rh = rect[3];
            dummy.cx = (float)std::atof(argv[10]); dummy.cy = (float)std::atof(argv[11]);
            dummy.prevCx = (float)std::atof(argv[12]); dummy.prevCy = (float)std::atof(argv[13]);
            dummy.type = argc > 14 ? std::atoi(argv[14]) : 0;
            portRet = collidedWithObjectInternal(c, dt, &dummy, nullptr, false) ? 1 : 0;
        } else
            portRet = collidedWithObjectInternal(c, dt, &dummy, rect, false) ? 1 : 0;
        if (c.unsupported) { std::printf("status: UNOBSERVED (the port reached code it does not have: %d)\n", c.unsupported); return 3; }
        if (portRet != std::atoi(argv[9])) std::printf("  return port %d GD %s\n", portRet, argv[9]);
    } else { std::printf("status: UNOBSERVED (function %s has no port here)\n", fn.c_str()); return 3; }
    // the words that carry the physics (position, rotation, spin, velocity, flags) are 0x38..0x3f, 0x4c..0x53, 0x670, 0x728..0x733, 0x938..0x947, 0x9a8..0x9bf, 0x9c0..0x9cf; the rest of
    // the object (sprites, containers, pointers) is the scratch object's own and not compared
    std::vector<std::pair<size_t, size_t>> ranges = {{0x38, 0x40}, {0x4c, 0x54}, {0x670, 0x671}, {0x728, 0x734}, {0x938, 0x948}, {0x9a8, 0x9c0}, {0x9c0, 0xa00}, {0xa00, 0xa20}};
    ranges.push_back({0x630, 0x638});
    ranges.push_back({0x958, 0x9a8});
    int bad = 0, compared = 0;
    if (portRet >= 0 && portRet != std::atoi(argv[9])) ++bad;
    for (auto r : ranges)
        for (size_t o = r.first; o < r.second; ++o) {
            ++compared;
            if (P.b[o] != out[o]) { ++bad; std::printf("  +0x%zx port %02x GD %02x\n", o, P.b[o], out[o]); }
        }
    float a, b2;
    std::memcpy(&a, &P.b[0x38], 4); std::memcpy(&b2, &out[0x38], 4);
    std::printf("status: %s (compared %d bytes; rotation port %.9g GD %.9g)\n", bad ? "DIFFERS" : "IDENTICAL", compared, (double)a, (double)b2);
    return bad ? 1 : 0;
}
