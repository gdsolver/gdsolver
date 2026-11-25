# Reachability-DP core ("leveldp") and its command-line front end.
#
# The solver core lives in src/dp/*.hpp (imported from GD-approx-physics @
# f7ede1e and split into modules) on top of the header-only Ship/UFO parameter
# tables in src/models. The headers are the INTERFACE target gdsolver_dp; they
# are compiled once, in src/dpcore.cpp, as the static library gdsolver_dpcore,
# which both the leveldp CLI and the Geode mod link -- so the CLI and the mod run
# the same compiled code, not two compilations of the same sources. Callers see
# only src/dpcore.hpp.

add_library(gdsolver_dp INTERFACE)
target_include_directories(gdsolver_dp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(gdsolver_dp INTERFACE cxx_std_17)

# Keep the code generation identical to the historical standalone build
# (GD-approx-physics: /W4 /permissive- /utf-8, Release = /O2 /Ob2 /DNDEBUG).
# The search is throughput-bound, and the DP inside the mod must behave
# bit-for-bit like the dev CLI, so every target that compiles the solver gets the
# Release optimisation level -- the core libraries below, and the test drivers.
function(gdsolver_dp_options target)
    if (MSVC)
        # (/bigobj: the object file format's limit on sections, which the core's translation unit has reached; it changes
        # the object file's layout, not the code)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /bigobj
            $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -O2)
    endif()
endfunction()

# The core, compiled once. Always built: the mod links it even when the CLI is off
# (GDSOLVER_BUILD_DP=OFF), and it goes into a shared library there, hence PIC.
add_library(gdsolver_dpcore STATIC src/dpcore.cpp)
target_link_libraries(gdsolver_dpcore PUBLIC gdsolver_dp)
set_target_properties(gdsolver_dpcore PROPERTIES POSITION_INDEPENDENT_CODE ON)
gdsolver_dp_options(gdsolver_dpcore)

if (NOT GDSOLVER_BUILD_DP)
    return()   # the mod still links the core above; only the CLI and its drivers are skipped
endif()

add_executable(leveldp src/leveldp_main.cpp)
target_link_libraries(leveldp PRIVATE gdsolver_dpcore)

# The same core, called more than once in one process -- what the mod does and the CLI never
# does. Used to prove that a solve is independent of the solves before it in the same process.
# It compiles the headers itself (it reads the core's globals between calls), so it is not part
# of the routine build: `cmake --build build --config RelWithDebInfo --target seqcall`.
add_executable(seqcall EXCLUDE_FROM_ALL src/seqcall_main.cpp)
target_link_libraries(seqcall PRIVATE gdsolver_dp)

# The rules a corpus of whole sections cannot referee -- a boundary the recordings in it do not
# contain. Run it by hand next to quick_regress; it needs no game and no data. It stays in the
# routine build: it is the cheapest of the solver's translation units, and the acceptance steps
# run it right after a build, where a stale binary would pass without a word.
add_executable(dptest src/dptest_main.cpp)
target_link_libraries(dptest PRIVATE gdsolver_dp)

gdsolver_dp_options(leveldp)
gdsolver_dp_options(seqcall)
gdsolver_dp_options(dptest)
