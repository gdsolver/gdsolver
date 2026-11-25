// seqcall -- call dp::cliMain more than once in one process.
//
// The mod does this (dp/src/dpcore.cpp): one GD process solves a level over dozens of
// invocations, where the CLI gets a fresh process every time. Anything the solver leaves in a
// namespace-scope global therefore reaches the mod's NEXT call and nobody else's, which makes
// leaked state look exactly like a level the in-process loop cannot solve.
//
// This exists to settle that question by measurement rather than by reading:
//
//   seqcall -- <B args>                 B alone, in a fresh process
//   seqcall -- <A args> -- <B args>     A first, then B, in one process
//
// If B's plan, trace and verdict differ between the two, state is leaking between calls, and
// the argument groups say which state. Each group is passed to cliMain exactly as a command
// line would be, argv[0] included.
//
//   seqcall --inproc <objrects> -- <args> [-- <args> ...]
//
// ...and the same with the level handed over the way the mod hands it: the file's bytes go into
// dp::g_levelCsv before every call (dpcore.cpp's solveInProcess), so the caches that key on the
// level text are exercised as they are in the game. Give the groups "(in-process level)" as their
// level argument, as the mod does.
//
//   seqcall ... -- <A args> --cp <src> <dst> -- <B args>
//
// --cp copies <src> over <dst> (bytes, replacing it) before the next invocation runs: a file the
// calls name by the same path (the mod's DATA_DIR/forceblocks.txt, a seed file) gets new content
// between two calls of one process, as it does when the mod writes it again.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dp/cli.hpp"

int main(int argc, char** argv) {
    std::vector<std::vector<std::string>> groups;
    std::string inproc;
    std::vector<std::pair<size_t, std::pair<std::string, std::string>>> copies;   // (run before invocation k, (src, dst))
    int i = 1;
    if (argc > 2 && !std::strcmp(argv[1], "--inproc")) {
        std::ifstream f(argv[2], std::ios::binary);
        if (!f) {
            std::printf("seqcall: cannot read %s\n", argv[2]);
            return 2;
        }
        inproc.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        i = 3;
    }
    for (; i < argc; ++i) {
        // --file <path>: the invocations from a file, one per line, the arguments split at white space (a path with a space in it
        // cannot be given this way). A command line holds some 32,000 characters and one replay's arguments are three thousand.
        if (!std::strcmp(argv[i], "--file") && i + 1 < argc) {
            std::ifstream lf(argv[++i]);
            std::string line;
            while (std::getline(lf, line)) {
                std::vector<std::string> g{"leveldp"};
                std::string tok;
                for (const char ch : line) {
                    if (ch == ' ' || ch == '\t' || ch == '\r') {
                        if (!tok.empty()) g.push_back(tok), tok.clear();
                    } else {
                        tok += ch;
                    }
                }
                if (!tok.empty()) g.push_back(tok);
                if (g.size() > 1) groups.push_back(g);
            }
            continue;
        }
        if (!std::strcmp(argv[i], "--cp") && i + 2 < argc) {
            copies.push_back({groups.size(), {argv[i + 1], argv[i + 2]}});
            i += 2;
            continue;
        }
        if (!std::strcmp(argv[i], "--")) {
            groups.push_back({"leveldp"});
            continue;
        }
        if (groups.empty()) {
            std::printf("usage: seqcall [--inproc <objrects>] -- <args> [-- <args> ...]\n");
            return 2;
        }
        groups.back().push_back(argv[i]);
    }
    if (groups.empty()) {
        std::printf("usage: seqcall [--inproc <objrects>] -- <args> [-- <args> ...]\n");
        return 2;
    }
    int rc = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        std::vector<char*> ptr;
        ptr.reserve(groups[g].size());
        for (std::string& s : groups[g]) ptr.push_back(s.data());
        for (const auto& c : copies) {
            if (c.first != g) continue;
            std::error_code ec;
            std::filesystem::copy_file(c.second.first, c.second.second, std::filesystem::copy_options::overwrite_existing, ec);
            std::printf("=== seqcall: copied %s over %s%s ===\n", c.second.first.c_str(), c.second.second.c_str(), ec ? " (FAILED)" : "");
            if (ec) return 2;
        }
        std::printf("=== seqcall: invocation %zu of %zu ===\n", g + 1, groups.size());
        std::fflush(stdout);
        if (!inproc.empty()) dp::g_levelCsv = inproc;
        const auto t0 = std::chrono::steady_clock::now();
        rc = dp::cliMain((int)ptr.size(), ptr.data());
        const double sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!inproc.empty()) dp::g_levelCsv.clear();
        std::printf("=== seqcall: invocation %zu returned %d in %.3f s (prep marks: args %.3f "
                    "groups %.3f objpos %.3f touch %.3f level %.3f tables %.3f) ===\n",
                    g + 1, rc, sec, dp::g_prepMark[0], dp::g_prepMark[1], dp::g_prepMark[5],
                    dp::g_prepMark[2], dp::g_prepMark[3], dp::g_prepMark[4]);
        std::fflush(stdout);
    }
    return rc;
}
