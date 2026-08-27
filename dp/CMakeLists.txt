# Reachability-DP core ("leveldp") and its command-line front end.
#
# The solver core lives in src/dp/*.hpp (imported from GD-approx-physics @
# f7ede1e and split into modules) on top of the header-only Ship/UFO parameter
# tables in src/models. It is header-only for now, so the library target is an
# INTERFACE target: the Geode mod and the CLI compile the *same* sources.

add_library(gdsolver_dp INTERFACE)
target_include_directories(gdsolver_dp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(gdsolver_dp INTERFACE cxx_std_17)

if (NOT GDSOLVER_BUILD_DP)
    return()   # the mod still links the INTERFACE core above; only the CLI is skipped
endif()

add_executable(leveldp src/leveldp_main.cpp)
target_link_libraries(leveldp PRIVATE gdsolver_dp)

# The same core, called more than once in one process -- what the mod does and the CLI never
# does. Used to prove that a solve is independent of the solves before it in the same process.
add_executable(seqcall src/seqcall_main.cpp)
target_link_libraries(seqcall PRIVATE gdsolver_dp)

# Keep the code generation identical to the historical standalone build
# (GD-approx-physics: /W4 /permissive- /utf-8, Release = /O2 /Ob2 /DNDEBUG).
# The search is throughput-bound, and the DP inside the mod must behave
# bit-for-bit like the dev CLI, so every configuration gets the Release
# optimisation level for these targets.
if (MSVC)
    target_compile_options(leveldp PRIVATE /W4 /permissive- /utf-8
        $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    target_compile_options(seqcall PRIVATE /W4 /permissive- /utf-8
        $<$<CONFIG:RelWithDebInfo>:/Ob2>)
else()
    target_compile_options(leveldp PRIVATE -Wall -Wextra -Wpedantic -O2)
    target_compile_options(seqcall PRIVATE -Wall -Wextra -Wpedantic -O2)
endif()
