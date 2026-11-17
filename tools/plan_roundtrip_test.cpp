// The plan file's writer and reader (src/mod/plan_edges.hpp) must give the edges back: writeInputLines, then each `input=` line through
// pushInputMask as loadInputsFile reads it. A tick whose button changes twice (a release and a press of player 2's button in one tick) was
// merged into the mask it already had, and the reader took that for no edge: a solution file lost the press the loop had flown.
//
//   cmake --build build-dp --config Release --target planroundtrip
//   build-dp/Release/planroundtrip.exe
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "mod/plan_edges.hpp"

using p1::InputCmd;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// loadInputsFile's loop over one block of lines
static std::vector<InputCmd> readBack(const std::string& text) {
    std::vector<InputCmd> out;
    std::istringstream in(text);
    std::string line;
    int prevMask = 0;
    while (std::getline(in, line)) {
        int t = 0, d = 0;
        if (line.rfind("input=", 0) == 0 && std::sscanf(line.c_str() + 6, "%d,%d", &t, &d) == 2)
            p1::pushInputMask(out, t, d, prevMask);
    }
    return out;
}
static std::string written(const std::vector<InputCmd>& plan) {
    std::ostringstream o;
    p1::writeInputLines(o, plan);
    return o.str();
}
static bool same(const std::vector<InputCmd>& a, const std::vector<InputCmd>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].step != b[i].step || a[i].down != b[i].down || a[i].p2 != b[i].p2) return false;
    return true;
}
// the order of a P1 and a P2 edge within one tick is not kept by a merged line (the reader gives player 1's first): compare per tick, per button
static std::vector<InputCmd> canon(std::vector<InputCmd> v) {
    std::stable_sort(v.begin(), v.end(), [](const InputCmd& a, const InputCmd& b) {
        if (a.step != b.step) return a.step < b.step;
        return a.p2 < b.p2;
    });
    return v;
}
static void roundtrip(const char* name, const std::vector<InputCmd>& plan, bool exact) {
    const std::vector<InputCmd> back = readBack(written(plan));
    const bool ok = exact ? same(plan, back) : same(canon(plan), canon(back));
    if (!ok) {
        std::printf("FAIL %s\n  wrote:\n%s", name, written(plan).c_str());
        ++g_fail;
    }
}

// planroundtrip <plan files...>: each file read as loadInputsFile reads it and written back by writeInputLines must be the same bytes (a plan with no
// second-player edge, or without a tick whose button changes twice, is written as it always was)
static int fileMode(int argc, char** argv) {
    int bad = 0, n = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        if (!f) { std::printf("cannot read %s\n", argv[i]); ++bad; continue; }
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
        std::string norm;   // the file as lines of input=, LF
        {
            std::istringstream in(text);
            std::string l;
            while (std::getline(in, l)) {
                if (!l.empty() && l.back() == 13) l.pop_back();
                if (l.rfind("input=", 0) == 0) norm += l + "\n";
            }
        }
        const std::string again = written(readBack(norm));
        ++n;
        if (again != norm) { std::printf("DIFFERS %s\n", argv[i]); ++bad; }
    }
    std::printf("%d plan file(s) read back and rewritten, %d differ\n", n, bad);
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return fileMode(argc, argv);
    // the case that lost the press: P2 held, released and pressed again at 27905, again at 27917
    roundtrip("p2 release+press in one tick", {{100, true, true}, {200, false, true}, {200, true, true}, {300, false, true}}, true);
    roundtrip("p2 held across two re-presses", {{10, true, true}, {20, false, true}, {20, true, true}, {40, false, true}, {40, true, true}, {50, false, true}}, true);
    roundtrip("p1 release+press in one tick, p2 present", {{10, true}, {20, false}, {20, true}, {30, false}, {35, true, true}, {36, false, true}}, true);
    roundtrip("both players re-press in one tick", {{10, true}, {10, true, true}, {20, false}, {20, true}, {20, false, true}, {20, true, true}, {30, false}, {30, false, true}}, false);
    roundtrip("p1 and p2 press together", {{10, true}, {10, true, true}, {20, false}, {20, false, true}}, false);
    roundtrip("p2 only", {{5, true, true}, {9, false, true}, {12, true, true}, {13, false, true}}, true);
    roundtrip("p1 only, taps in one tick", {{5, true}, {5, false}, {9, true}, {9, false}}, true);
    roundtrip("p1 only", {{5, true}, {9, false}, {12, true}, {13, false}}, true);
    // a solver's tail is read against the buttons the anchor holds (loadInputsFile's initialMask): player 2 held at the anchor, the tail releases it at 27905,
    // presses at 27917 and releases at 27919
    {
        std::vector<InputCmd> held, fresh;
        int prevHeld = 2, prevFresh = 0;
        for (const auto& l : std::vector<std::pair<int, int>>{{27905, 0}, {27917, 2}, {27919, 0}}) {
            p1::pushInputMask(held, l.first, l.second, prevHeld);
            p1::pushInputMask(fresh, l.first, l.second, prevFresh);
        }
        CHECK(same(held, {{27905, false, true}, {27917, true, true}, {27919, false, true}}));
        // read against nothing held (the old reading) the release is no edge and the press is a press of a button that is still down
        CHECK(!same(fresh, held));
    }
    // the written file for the lost-press case has the two lines
    {
        const std::string w = written({{100, true, true}, {200, false, true}, {200, true, true}, {300, false, true}});
        CHECK(w == "input=100,2\ninput=200,0\ninput=200,2\ninput=300,0\n");
    }
    // a tick with no double change is still one line
    {
        const std::string w = written({{10, true}, {10, true, true}, {20, false}, {20, false, true}});
        CHECK(w == "input=10,3\ninput=20,0\n");
    }
    // random plans: each button alternates down / up (a button is released only after it is pressed), edges grouped into ticks in order
    std::mt19937 rng(20261022);
    for (int round = 0; round < 20000; ++round) {
        std::vector<InputCmd> plan;
        bool held[2] = {false, false};
        int t = 1;
        const int n = 1 + (int)(rng() % 14);
        const bool twoPlayer = rng() % 4 != 0;
        for (int k = 0; k < n; ++k) {
            if (rng() % 3 != 0) t += 1 + (int)(rng() % 5);   // otherwise stay on the same tick
            const int who = twoPlayer ? (int)(rng() % 2) : 0;
            held[who] = !held[who];
            plan.push_back({t, held[who], who == 1});
        }
        // a tick holds each button's edges in the order they were made; between buttons the reader's order is player 1 first
        const std::vector<InputCmd> back = readBack(written(plan));
        if (!same(canon(plan), canon(back))) {
            std::printf("FAIL random round %d\n  wrote:\n%s", round, written(plan).c_str());
            ++g_fail;
            break;
        }
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("plan writer/reader round trip: ok\n");
    return 0;
}
