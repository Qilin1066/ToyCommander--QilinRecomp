cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(ToyCommanderQilinRecomp VERSION 0.9.0 LANGUAGES CXX)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit hosts only (see docs/roadmap-and-design-targets.md)")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# ---- The boot file loader and the GD-ROM reader: runtime code too (no std::format) ----
add_library(qilin_disc STATIC recomp/src/bootfile.cpp recomp/src/gdrom.cpp recomp/src/sha256.cpp)
target_include_directories(qilin_disc PUBLIC recomp/src)
target_link_libraries(qilin_disc PRIVATE qilin_warnings)

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/bootfile_json.cpp
    recomp/src/decoder.cpp
    recomp/src/cfg.cpp
    recomp/src/cfg_report.cpp
    recomp/src/hwaccess.cpp
    recomp/src/layout.cpp
    recomp/src/fpmode.cpp
    recomp/src/lifter.cpp
    recomp/src/lift_output.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src)
target_link_libraries(qilin_core PUBLIC qilin_disc PRIVATE qilin_warnings)
qilin_tool(qilin_core)

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

# ---- Tests (no game files needed) ----
enable_testing()
add_executable(qilin_tests recomp/tests/tests.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)

# ---- End-to-end lifter tests (no game files needed) ----
# qilin_liftgen lifts synthetic SH-4 programs with the real analysis and
# lifter at build time; qilin_lift_tests compiles that output with the
# compiler under test, with the runtime, and runs it against results computed
# from the instructions' definitions.
add_executable(qilin_liftgen recomp/tests/lift_gen.cpp)
target_include_directories(qilin_liftgen PRIVATE recomp/tests)
target_link_libraries(qilin_liftgen PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_liftgen)
set(QILIN_LIFT_TEST_GEN ${CMAKE_BINARY_DIR}/lift_test_gen)
set(QILIN_LIFT_TEST_SOURCES
    ${QILIN_LIFT_TEST_GEN}/table.cpp ${QILIN_LIFT_TEST_GEN}/stubs.cpp ${QILIN_LIFT_TEST_GEN}/lifted_000.cpp)
add_custom_command(
    OUTPUT ${QILIN_LIFT_TEST_SOURCES} ${QILIN_LIFT_TEST_GEN}/functions.hpp
    COMMAND qilin_liftgen ${QILIN_LIFT_TEST_GEN}
    DEPENDS qilin_liftgen
    COMMENT "Lifting the synthetic test programs")
add_executable(qilin_lift_tests recomp/tests/lift_run.cpp recomp/runtime/dispatch.cpp recomp/runtime/system.cpp recomp/runtime/memory.cpp recomp/runtime/bus.cpp ${QILIN_LIFT_TEST_SOURCES})
target_include_directories(qilin_lift_tests PRIVATE recomp/runtime recomp/tests ${QILIN_LIFT_TEST_GEN})
target_compile_definitions(qilin_lift_tests PRIVATE QILIN_CHECKED_MEMORY)
# Lifted code: each guest FPU operation rounds on its own, so the host compiler
# must not fuse a multiply and an add into one (MSVC doesn't by default).
if(NOT MSVC)
    target_compile_options(qilin_lift_tests PRIVATE -ffp-contract=off)
endif()
target_link_libraries(qilin_lift_tests PRIVATE qilin_warnings)
add_test(NAME qilin_lift_tests COMMAND qilin_lift_tests)

# ---- The differential test (no game files needed) ----
# qilin_difftest_gen lifts random encodings of every instruction the lifter
# lifts, and programs for every kind of branch, with the real analysis and
# lifter; qilin_difftest runs them, compiled by the compiler under test,
# against a reference interpreter written separately from the lifter.
add_executable(qilin_difftest_gen recomp/tests/difftest/gen.cpp)
target_include_directories(qilin_difftest_gen PRIVATE recomp/tests recomp/tests/difftest)
target_link_libraries(qilin_difftest_gen PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_difftest_gen)
set(QILIN_DIFFTEST_GEN ${CMAKE_BINARY_DIR}/difftest_gen)
set(QILIN_DIFFTEST_SOURCES ${QILIN_DIFFTEST_GEN}/table.cpp ${QILIN_DIFFTEST_GEN}/cases.cpp)
foreach(i RANGE 7)
    list(APPEND QILIN_DIFFTEST_SOURCES ${QILIN_DIFFTEST_GEN}/lifted_${i}.cpp)
endforeach()
add_custom_command(
    OUTPUT ${QILIN_DIFFTEST_SOURCES} ${QILIN_DIFFTEST_GEN}/functions.hpp
    COMMAND qilin_difftest_gen ${QILIN_DIFFTEST_GEN}
    DEPENDS qilin_difftest_gen
    COMMENT "Lifting the differential test's cases")
add_executable(qilin_difftest recomp/tests/difftest/run.cpp recomp/tests/difftest/ref.cpp recomp/runtime/dispatch.cpp recomp/runtime/system.cpp recomp/runtime/bus.cpp ${QILIN_DIFFTEST_SOURCES})
target_include_directories(qilin_difftest PRIVATE recomp/runtime recomp/tests recomp/tests/difftest ${QILIN_DIFFTEST_GEN})
target_compile_definitions(qilin_difftest PRIVATE QILIN_CHECKED_MEMORY)
if(NOT MSVC)
    target_compile_options(qilin_difftest PRIVATE -ffp-contract=off)
else()
    target_compile_options(qilin_difftest PRIVATE /bigobj)
endif()
target_link_libraries(qilin_difftest PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_difftest)
add_test(NAME qilin_difftest COMMAND qilin_difftest)

# ---- The game: built only from the player's own boot file ----
# cmake -DQILIN_GAME_BOOT=<extracted disc>/files/1GUTH.BIN: qilin lift turns it
# into C++ at build time (into the build tree, never the source tree), and
# qilin_run is built from that with the runtime. It reads the rest of the game
# from the player's disc dump at run time.
set(QILIN_GAME_BOOT "" CACHE FILEPATH "Toy Commander (USA, MK-57020, V1.022) 1GUTH.BIN, from qilin extract: builds qilin_run")
option(QILIN_RUN_CHECKED "qilin_run checks every guest memory access (bring-up)" ON)
set(QILIN_RT_SOURCES recomp/runtime/dispatch.cpp recomp/runtime/system.cpp recomp/runtime/memory.cpp recomp/runtime/bus.cpp)
if(QILIN_GAME_BOOT)
    set(QILIN_LIFT_FILES 24)
    set(gen ${CMAKE_BINARY_DIR}/generated)
    set(gen_sources ${gen}/table.cpp ${gen}/stubs.cpp)
    math(EXPR last "${QILIN_LIFT_FILES} - 1")
    foreach(i RANGE ${last})
        string(LENGTH "${i}" len)
        if(len EQUAL 1)
            set(i "00${i}")
        elseif(len EQUAL 2)
            set(i "0${i}")
        endif()
        list(APPEND gen_sources ${gen}/lifted_${i}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${gen}/functions.hpp ${gen_sources}
        COMMAND qilin lift ${QILIN_GAME_BOOT} --out ${gen} --files ${QILIN_LIFT_FILES}
        DEPENDS qilin ${QILIN_GAME_BOOT}
        COMMENT "Lifting the game's boot file to C++")
    add_executable(qilin_run toycommander/src/main.cpp toycommander/src/boot.cpp toycommander/src/natives.cpp toycommander/src/heap.cpp toycommander/src/pvr.cpp toycommander/src/sh4.cpp toycommander/src/clock.cpp toycommander/src/gdrom_bios.cpp toycommander/src/chain.cpp toycommander/src/rtc.cpp toycommander/src/ta.cpp toycommander/src/interrupts.cpp toycommander/src/threads.cpp toycommander/src/aica.cpp ${QILIN_RT_SOURCES} ${gen_sources})
    target_include_directories(qilin_run PRIVATE toycommander/src recomp/runtime ${gen})
    find_package(Threads REQUIRED)
    target_link_libraries(qilin_run PRIVATE qilin_disc qilin_warnings Threads::Threads)
    if(QILIN_RUN_CHECKED)
        target_compile_definitions(qilin_run PRIVATE QILIN_CHECKED_MEMORY)
    endif()
    if(MSVC)
        target_compile_options(qilin_run PRIVATE /bigobj)
        target_link_options(qilin_run PRIVATE /STACK:16777216)  # lifted calls nest as deeply as the guest's
    else()
        target_compile_options(qilin_run PRIVATE -ffp-contract=off)
    endif()
endif()

# ---- qilin_dcref: the FPU's approximate instructions against a Dreamcast emulator ----
# A developer tool, not built by default and not in CI: it runs Flycast's
# libretro core (GPL, fetched by tools/dcref/fetch.sh, never committed) as a
# black box. Linux x86-64, with Mesa's EGL (the core needs OpenGL to boot).
option(QILIN_DCREF "Build qilin_dcref (needs QILIN_DCREF_DIR, the folder tools/dcref/fetch.sh filled)" OFF)
set(QILIN_DCREF_DIR "" CACHE PATH "The folder tools/dcref/fetch.sh filled (libretro.h, flycast_libretro.so)")
if(QILIN_DCREF)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        message(FATAL_ERROR "qilin_dcref runs on Linux x86-64 only")
    endif()
    if(NOT EXISTS "${QILIN_DCREF_DIR}/libretro.h")
        message(FATAL_ERROR "QILIN_DCREF_DIR has no libretro.h: run tools/dcref/fetch.sh <folder> and pass -DQILIN_DCREF_DIR=<folder>")
    endif()
    add_executable(qilin_dcref tools/dcref/dcref.cpp)
    target_include_directories(qilin_dcref PRIVATE recomp/runtime recomp/tests)
    target_include_directories(qilin_dcref SYSTEM PRIVATE "${QILIN_DCREF_DIR}")
    target_compile_options(qilin_dcref PRIVATE -ffp-contract=off)
    target_link_libraries(qilin_dcref PRIVATE qilin_warnings ${CMAKE_DL_LIBS})
    qilin_tool(qilin_dcref)
endif()
