cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(gdsolver VERSION 0.1.0 LANGUAGES CXX)

# gdsolver is one repository with two build products that share sources:
#   dp/   the reachability-DP solver core and its CLI (leveldp.exe), built
#         anywhere with a C++ compiler -- no Geode or GD required
#   src/  the Geode mod, which drives GD's own physics as the replay verifier
#         and (eventually) hosts the solver loop in-process
option(GDSOLVER_BUILD_DP "Build the DP core library and the leveldp CLI" ON)
option(GDSOLVER_BUILD_MOD "Build the Geode mod (requires GEODE_SDK)" ON)
# The mod's test instruments (cfg netcatch_testonly, nettrace_testonly, deathtest_testonly): ways to drop a player's requests, to
# trace them, and to flag a level for the death-data upload, which exist to measure the record gate and which a release must not
# have. OFF by default, so a normal build ignores those keys; the measurement builds turn it ON.
option(GDSOLVER_TEST_INSTRUMENTS "Build the mod's test-only instruments (never for a release)" OFF)
# The exe guard (src/mod/exe_guard.hpp): on a Geometry Dash executable the mod was not built for, none of its hooks is enabled and its start-up
# code does not run. Empty is the default -- on for the 2.209 builds (GEODE_COMP_GD_VERSION >= 22090), off for the others; ON / OFF force it.
# OFF is the way out once Geode names the 2.209 executables itself.
set(GDSOLVER_EXE_GUARD "" CACHE STRING "Exe guard: empty = default (on for 2.209 builds), ON or OFF to force")

# The search is throughput-bound, and RelWithDebInfo differed from Release here only in how it
# links: incrementally, which routes calls through jump thunks and keeps /OPT:REF and /OPT:ICF off.
# Link it the Release way (debug info kept). The code generation is unchanged, so the output is
# too; measured on leveldp, the same objects linked both ways used 7% less CPU over three
# official levels (421 -> 391 s, single thread) with byte-identical plans and traces.
if (MSVC)
    foreach (kind EXE SHARED)
        string(REGEX REPLACE "/INCREMENTAL[^ ]*" "" CMAKE_${kind}_LINKER_FLAGS_RELWITHDEBINFO
               "${CMAKE_${kind}_LINKER_FLAGS_RELWITHDEBINFO}")
        string(APPEND CMAKE_${kind}_LINKER_FLAGS_RELWITHDEBINFO " /INCREMENTAL:NO /OPT:REF /OPT:ICF")
    endforeach()
endif()

# The solver core's library (gdsolver_dpcore) is always added: the mod links it too. Only the
# leveldp CLI and its drivers are gated on GDSOLVER_BUILD_DP.
add_subdirectory(dp)

# The replay panel's pure geometry (src/mod/replay_panel_geom.hpp: the level's boxes, GD's path, the clipped primitives),
# checked without the game. Not part of the routine build:
#   cmake --build build-dp --config Release --target panelgeomtest
add_executable(panelgeomtest EXCLUDE_FROM_ALL tools/panel_geom_test.cpp)
# The plan file's writer and reader (src/mod/plan_edges.hpp): a plan written and read back gives the same edges (a tick whose button changes twice included).
add_executable(planroundtrip EXCLUDE_FROM_ALL tools/plan_roundtrip_test.cpp)
target_include_directories(panelgeomtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(panelgeomtest PRIVATE cxx_std_17)
target_include_directories(planroundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(planroundtrip PRIVATE cxx_std_17)

if (GDSOLVER_BUILD_MOD)
    file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
    # The second copy of the core is not a mod source: it is compiled as the CLI's code is,
    # below, without the mod's options and prelude.
    list(REMOVE_ITEM SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/mod/dp_bridge2.cpp)
    add_library(${PROJECT_NAME} SHARED ${SOURCES})
    # src/mod/*.hpp include each other and the solver/ headers by path from src/
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if (GDSOLVER_TEST_INSTRUMENTS)
        target_compile_definitions(${PROJECT_NAME} PRIVATE GDSOLVER_TEST_INSTRUMENTS=1)
    endif()
    if (NOT GDSOLVER_EXE_GUARD STREQUAL "")
        if (GDSOLVER_EXE_GUARD)
            target_compile_definitions(${PROJECT_NAME} PRIVATE GDSOLVER_EXE_GUARD=1)
        else()
            target_compile_definitions(${PROJECT_NAME} PRIVATE GDSOLVER_EXE_GUARD=0)
        endif()
    endif()

    # The mod's DP must behave exactly like the dev CLI -- that equality is an acceptance
    # criterion, not an aspiration -- so the mod does not compile the solver at all. Its first
    # copy is the CLI's own compiled core (gdsolver_dpcore), and its second copy
    # (src/mod/dp_bridge2.cpp, the solver's headers again under a renamed namespace) is a library
    # of its own with the same options. The mod sees dp/src/dpcore.hpp through
    # src/mod/dp_bridge.hpp; the only other dp/ header it includes is dp/band_rows.hpp, a file
    # format shared with dptest that compiles no solver code.
    add_library(gdsolver_dpcore2 STATIC src/mod/dp_bridge2.cpp)
    target_include_directories(gdsolver_dpcore2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(gdsolver_dpcore2 PRIVATE gdsolver_dp)
    set_target_properties(gdsolver_dpcore2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    gdsolver_dp_options(gdsolver_dpcore2)

    # Plain signature on purpose: Geode's setup_geode_mod links without keywords, and CMake
    # refuses to mix the two forms on one target.
    target_link_libraries(${PROJECT_NAME} gdsolver_dpcore gdsolver_dpcore2)

    # A 2.209 build links what the 2.209 libcocos2d exports and the SDK's libcocos2d.lib (made for 2.2081) does not list: an import
    # library made from tools/libcocos2d_209extra.def, by the toolchain's own linker (link.exe or lld-link, both take /lib).
    # (Matched as text: as versions "2.2081" would compare greater than "2.209".)
    if (WIN32 AND GEODE_GD_VERSION MATCHES "^2\\.209")
        set(GDSOLVER_COCOS_EXTRA_LIB ${CMAKE_BINARY_DIR}/libcocos2d_209extra.lib)
        add_custom_command(OUTPUT ${GDSOLVER_COCOS_EXTRA_LIB}
            COMMAND ${CMAKE_LINKER} /lib /nologo /machine:x64 /def:${CMAKE_CURRENT_SOURCE_DIR}/tools/libcocos2d_209extra.def
                    /out:${GDSOLVER_COCOS_EXTRA_LIB}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/libcocos2d_209extra.def
            VERBATIM)
        add_custom_target(gdsolver_cocos_extra DEPENDS ${GDSOLVER_COCOS_EXTRA_LIB})
        add_dependencies(${PROJECT_NAME} gdsolver_cocos_extra)
        target_link_libraries(${PROJECT_NAME} ${GDSOLVER_COCOS_EXTRA_LIB})
    endif()

    if (NOT DEFINED ENV{GEODE_SDK})
        message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode (or configure with -DGDSOLVER_BUILD_MOD=OFF)")
    else()
        message(STATUS "Found Geode: $ENV{GEODE_SDK}")
    endif()

    add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

    # Which game the package is for. mod.json names the release (gd.win 2.209), and the SDK reads the package files (mod.json, about.md,
    # changelog.md, logo.png) from CMAKE_CURRENT_SOURCE_DIR and refuses a mod.json whose gd.<platform> is not the GEODE_GD_VERSION being
    # built. A build for the other game (-DGEODE_GD_VERSION=2.2081 -DGEODE_COMP_GD_VERSION=22081: the 2.2081 workers, Android) takes the same
    # files from a directory of its own in the build tree with that one value replaced, so the tracked file never changes and the tree stays
    # clean. A package for 2.2081 names 2.2081, which is what makes Geode on a 2.209 game refuse it.
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/mod.json GDSOLVER_MOD_JSON)
    string(JSON GDSOLVER_MOD_GD GET "${GDSOLVER_MOD_JSON}" gd ${GEODE_TARGET_PLATFORM_JSON})
    set(GDSOLVER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    if (NOT GDSOLVER_MOD_GD STREQUAL GEODE_GD_VERSION AND NOT GDSOLVER_MOD_GD STREQUAL "*")
        message(STATUS "mod.json names GD ${GDSOLVER_MOD_GD}; packaging for GD ${GEODE_GD_VERSION} from ${CMAKE_CURRENT_BINARY_DIR}/package")
        string(JSON GDSOLVER_MOD_JSON SET "${GDSOLVER_MOD_JSON}" gd ${GEODE_TARGET_PLATFORM_JSON} "\"${GEODE_GD_VERSION}\"")
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/package)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/package/mod.json "${GDSOLVER_MOD_JSON}\n")
        foreach (f about.md changelog.md logo.png)
            configure_file(${GDSOLVER_SOURCE_DIR}/${f} ${CMAKE_CURRENT_BINARY_DIR}/package/${f} COPYONLY)
        endforeach()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GDSOLVER_SOURCE_DIR}/mod.json)
        set(CMAKE_CURRENT_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/package)
    endif()

    setup_geode_mod(${PROJECT_NAME})
    set(CMAKE_CURRENT_SOURCE_DIR ${GDSOLVER_SOURCE_DIR})
endif()
