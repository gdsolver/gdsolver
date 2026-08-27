// seqcall -- call dp::cliMain more than once in one process.
//
// The mod does this (src/mod/dp_bridge.cpp): one GD process solves a level over dozens of
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
#include <cstdio>
#include <string>
#include <vector>

#include "dp/cli.hpp"

int main(int argc, char** argv) {
    std::vector<std::vector<std::string>> groups;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--")) {
            groups.push_back({"leveldp"});
            continue;
        }
        if (groups.empty()) {
            std::printf("usage: seqcall -- <args> [-- <args> ...]\n");
            return 2;
        }
        groups.back().push_back(argv[i]);
    }
    if (groups.empty()) {
        std::printf("usage: seqcall -- <args> [-- <args> ...]\n");
        return 2;
    }
    int rc = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        std::vector<char*> ptr;
        ptr.reserve(groups[g].size());
        for (std::string& s : groups[g]) ptr.push_back(s.data());
        std::printf("=== seqcall: invocation %zu of %zu ===\n", g + 1, groups.size());
        std::fflush(stdout);
        rc = dp::cliMain((int)ptr.size(), ptr.data());
        std::printf("=== seqcall: invocation %zu returned %d ===\n", g + 1, rc);
        std::fflush(stdout);
    }
    return rc;
}
