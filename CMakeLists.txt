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

# The solver core is an INTERFACE target (header-only), so it is always added: the mod links
# it too. Only the leveldp CLI is gated on GDSOLVER_BUILD_DP.
add_subdirectory(dp)

if (GDSOLVER_BUILD_MOD)
    file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
    add_library(${PROJECT_NAME} SHARED ${SOURCES})
    # src/mod/*.hpp include each other and the solver/ headers by path from src/
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # ...and dp/src for the solver core (src/mod/dp_bridge.cpp is the only TU that uses it).
    # Plain signature on purpose: Geode's setup_geode_mod links without keywords, and CMake
    # refuses to mix the two forms on one target.
    target_link_libraries(${PROJECT_NAME} gdsolver_dp)
    # Match the CLI's code generation for the solver TU. The mod's DP must behave exactly like
    # the dev CLI -- that equality is an acceptance criterion, not an aspiration -- so it is
    # compiled with the same inlining budget rather than the mod's default.
    if (MSVC)
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/mod/dp_bridge.cpp
            PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:RelWithDebInfo>:/Ob2>")
    endif()

    if (NOT DEFINED ENV{GEODE_SDK})
        message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode (or configure with -DGDSOLVER_BUILD_MOD=OFF)")
    else()
        message(STATUS "Found Geode: $ENV{GEODE_SDK}")
    endif()

    add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

    setup_geode_mod(${PROJECT_NAME})
endif()
