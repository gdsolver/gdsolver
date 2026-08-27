// leveldp -- the command-line front end of the reachability-DP solver.
//
// The whole of it lives in dp/cli.hpp so that the Geode mod can run the identical code
// path in-process; this file exists to give the executable an entry point.
#include "dp/cli.hpp"

int main(int argc, char** argv) {
    return dp::cliMain(argc, argv);
}
