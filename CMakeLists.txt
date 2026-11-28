cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(ToyCommanderQilinRecomp VERSION 0.14.1 LANGUAGES C CXX)
# Each game is released on its own (tags toycommander-vX.Y.Z and
# toyracer-vX.Y.Z, its own CHANGELOG): its version is here. The project's
# (the recompiler's, qilin --version) is Toy Commander's.
set(QILIN_TOYCOMMANDER_VERSION ${PROJECT_VERSION})
set(QILIN_TOYRACER_VERSION 0.7.1)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules anywhere: without this, Ninja builds scan every C++ source
# for them with clang-scan-deps, which zig's Clang (CLAUDE.md, building on a
# Windows PC without LLVM) doesn't have, and the build stops at the first one.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
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
option(QILIN_REQUIRE_LAUNCHER_TESTS "Require the launcher's build test (CI): a native build, not a cross-compile" OFF)

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

set(QILIN_SANITIZE "" CACHE STRING "Sanitizers for our code, e.g. address,undefined")
if(QILIN_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "QILIN_SANITIZE currently requires GCC or Clang")
    endif()
    target_compile_options(qilin_warnings INTERFACE -fsanitize=${QILIN_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(qilin_warnings INTERFACE -fsanitize=${QILIN_SANITIZE})
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# Checked file storage, independent of SDL/Vulkan and shared by both games.
add_library(qilin_file_store STATIC host/src/file_store.cpp)
target_include_directories(qilin_file_store PUBLIC host/include)
target_link_libraries(qilin_file_store PRIVATE qilin_warnings)

# ---- libchdr (BSD-3-Clause): reading .chd disc images ----
# v0.3.0, fetched and pinned, built statically with what it bundles: LZMA
# (public domain), miniz (Unlicense), zstd's decompressor (taken under its BSD
# licence) and dr_flac (public domain). Its own CMake project is added by hand
# (SOURCE_SUBDIR points nowhere) so its test programs stay out of the build.
include(FetchContent)
FetchContent_Declare(libchdr
    URL https://github.com/rtissera/libchdr/archive/93d8c239ff0d4e8d7722985992649fce12d2463b.tar.gz
    URL_HASH SHA256=f38fb1b3e907c8927271a57ad6e3c02347eaa2d5803f910880b4b3c3021b3657
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(libchdr)
set(qilin_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "" FORCE)
set(WITH_LZMA_ASM OFF CACHE BOOL "" FORCE)  # plain C: no assembler needed on any target
add_subdirectory(${libchdr_SOURCE_DIR} ${libchdr_BINARY_DIR} EXCLUDE_FROM_ALL SYSTEM)
set(BUILD_SHARED_LIBS ${qilin_saved_shared})
set(QILIN_CHDR_LIBRARIES chdr-static chdr-lzma miniz zstd)
foreach(library IN LISTS QILIN_CHDR_LIBRARIES)
    set_target_properties(${library} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(${library} PRIVATE /W0)  # third-party code: its warnings aren't ours
    else()
        target_compile_options(${library} PRIVATE -w)
    endif()
endforeach()

# ---- The boot file loader and the GD-ROM reader: runtime code too (no std::format) ----
add_library(qilin_disc STATIC recomp/src/bootfile.cpp recomp/src/gdrom.cpp recomp/src/sha256.cpp)
target_include_directories(qilin_disc PUBLIC recomp/src recomp/runtime)
target_link_libraries(qilin_disc PRIVATE chdr-static)
if(WIN32)
    target_link_libraries(qilin_disc PUBLIC shell32)
endif()
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

# The Windows download's launcher (tools/player-kit/launcher): its portable
# parts (the JSON reader, versions and the releases list, the kit's text
# files) are tested everywhere; the rest is built further down, on Windows.
add_executable(qilin_launcher_test tools/player-kit/launcher/launcher_test.cpp)
target_link_libraries(qilin_launcher_test PRIVATE qilin_warnings)
qilin_tool(qilin_launcher_test)
foreach(part IN ITEMS json version kit)
    add_test(NAME qilin_launcher_${part}_test COMMAND qilin_launcher_test ${part})
endforeach()

# The launcher's build engine, process handling, disc check and updates, on
# every desktop system: what the system provides is platform_win.cpp,
# process_win.cpp and shortcut_win.cpp on Windows, the _posix.cpp ones on
# Linux and macOS. The build test runs the real build with stand-in children
# (launcher_stub.cpp), on every system it is built for (not a cross-compile).
if(QILIN_REQUIRE_LAUNCHER_TESTS AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "the launcher's build test needs a native build")
endif()
set(QILIN_LAUNCHER_DIR tools/player-kit/launcher)
if(WIN32)
    set(qilin_launcher_system win)
else()
    set(qilin_launcher_system posix)
endif()
add_library(qilin_launcher_core STATIC ${QILIN_LAUNCHER_DIR}/kit.cpp ${QILIN_LAUNCHER_DIR}/disc.cpp ${QILIN_LAUNCHER_DIR}/build.cpp
                                       ${QILIN_LAUNCHER_DIR}/update.cpp ${QILIN_LAUNCHER_DIR}/tidy.cpp
                                       ${QILIN_LAUNCHER_DIR}/platform_${qilin_launcher_system}.cpp
                                       ${QILIN_LAUNCHER_DIR}/process_${qilin_launcher_system}.cpp
                                       ${QILIN_LAUNCHER_DIR}/shortcut_${qilin_launcher_system}.cpp)
target_include_directories(qilin_launcher_core PUBLIC ${QILIN_LAUNCHER_DIR})
target_link_libraries(qilin_launcher_core PUBLIC qilin_disc PRIVATE qilin_warnings)
if(WIN32)
    target_link_libraries(qilin_launcher_core PUBLIC winhttp ole32 uuid shell32)
endif()
qilin_tool(qilin_launcher_core)
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(qilin_launcher_stub tools/check/launcher_stub.cpp)
    target_include_directories(qilin_launcher_stub PRIVATE recomp/runtime)
    target_link_libraries(qilin_launcher_stub PRIVATE qilin_warnings)
    add_executable(qilin_launcher_build_test ${QILIN_LAUNCHER_DIR}/launcher_build_test.cpp)
    target_link_libraries(qilin_launcher_build_test PRIVATE qilin_launcher_core qilin_warnings)
    if(WIN32)
        target_link_libraries(qilin_launcher_stub PRIVATE shell32)
        target_link_libraries(qilin_launcher_build_test PRIVATE shell32)
    endif()
    qilin_tool(qilin_launcher_stub)
    qilin_tool(qilin_launcher_build_test)
    add_test(NAME qilin_launcher_build_test COMMAND qilin_launcher_build_test "$<TARGET_FILE:qilin_launcher_stub>" "$<TARGET_FILE:qilin>"
                                                    "${CMAKE_CURRENT_BINARY_DIR}/launcher-check/$<CONFIG>")
    set_tests_properties(qilin_launcher_build_test PROPERTIES TIMEOUT 120)
endif()

add_executable(qilin_file_store_test host/tests/file_store_test.cpp)
target_include_directories(qilin_file_store_test PRIVATE recomp/runtime)
target_link_libraries(qilin_file_store_test PRIVATE qilin_file_store qilin_warnings)
add_test(NAME qilin_file_store_test COMMAND qilin_file_store_test)

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

# ---- The host platform: window, input and audio (SDL3), Vulkan ----
# The sister projects' host layer (PunchOutWii--QilinRecomp's host/). Fetched at
# configure time, pinned, built statically: SDL3, the Vulkan headers and volk
# (which loads Vulkan at run time, so no SDK is needed to build or run).
option(QILIN_HOST "Build the host platform layer (window, Vulkan, input, audio)" ON)
if(QILIN_HOST)
    include(FetchContent)
    set(FETCHCONTENT_QUIET ON)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)  # synthetic input: not needed
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG release-3.4.20 GIT_SHALLOW ON)
    FetchContent_Declare(VulkanHeaders GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
                         GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON)
    # volk is one C file: fetched without its own CMake project (SOURCE_SUBDIR points nowhere).
    FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON
                         SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(SDL3 VulkanHeaders volk)
    # SDL's Wayland protocol headers, made from its own XML, before the
    # system's: SDL adds them as a system folder, and zig's compiler (the
    # Linux download's) searches the system's /usr/include first, whose
    # older wayland-client-protocol.h lacks what SDL uses
    # (WL_KEYBOARD_KEY_STATE_REPEATED). A plain -I comes before both.
    if(TARGET SDL3-static AND EXISTS "${sdl3_BINARY_DIR}/wayland-generated-protocols")
        target_include_directories(SDL3-static BEFORE PRIVATE "${sdl3_BINARY_DIR}/wayland-generated-protocols")
    endif()
    add_library(qilin_volk STATIC ${volk_SOURCE_DIR}/volk.c)
    target_include_directories(qilin_volk PUBLIC ${volk_SOURCE_DIR})
    target_link_libraries(qilin_volk PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
    target_compile_definitions(qilin_volk PUBLIC VK_NO_PROTOTYPES)

    # Dear ImGui (MIT) for the settings menu, as the other Qilin recomps draw
    # theirs: the same commit (v1.93.0), its core and its SDL3 and Vulkan
    # backends, fetched and pinned. Its Vulkan backend loads its functions
    # through volk (no prototypes).
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
        URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114)
    FetchContent_MakeAvailable(imgui)
    add_library(qilin_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(qilin_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(qilin_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(qilin_imgui PUBLIC SDL3::SDL3-static Vulkan::Headers)
    if(MSVC)
        target_compile_options(qilin_imgui PRIVATE /W0)  # third-party code: its warnings aren't ours
    else()
        target_compile_options(qilin_imgui PRIVATE -w)
    endif()

    # The host layer and the settings menu, once per game: the game's name in
    # messages, the window's title, the data folder and the settings file.
    # (The tests use Toy Commander's.)
    function(qilin_game_host game title version)
        add_library(qilin_host_${game} STATIC host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp host/src/image.cpp)
        target_include_directories(qilin_host_${game} PUBLIC host/include)
        target_compile_definitions(qilin_host_${game} PUBLIC QILIN_APP="${game}" QILIN_APP_TITLE="${title}")
        target_link_libraries(qilin_host_${game} PUBLIC qilin_volk PRIVATE SDL3::SDL3-static qilin_warnings)
        add_library(dreamcast_menu_${game} STATIC dreamcast/src/settings.cpp dreamcast/src/menu.cpp dreamcast/src/save_history.cpp)
        target_include_directories(dreamcast_menu_${game} PUBLIC dreamcast/src recomp/runtime)
        target_link_libraries(dreamcast_menu_${game} PUBLIC qilin_host_${game} qilin_imgui PRIVATE SDL3::SDL3-static qilin_warnings qilin_file_store)
        target_compile_definitions(dreamcast_menu_${game} PRIVATE QILIN_VERSION="${version}")
    endfunction()
    qilin_game_host(toycommander "Toy Commander" ${QILIN_TOYCOMMANDER_VERSION})
    qilin_game_host(toyracer "Toy Racer" ${QILIN_TOYRACER_VERSION})
    # Vulkan structures are written {sType} with the rest zeroed: that's the idiom, not a mistake.
    # (On the sources, so it comes after -Wextra on the command line.)
    if(NOT MSVC)
        set_source_files_properties(host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp host/tests/vktest.cpp
                                    dreamcast/src/renderer.cpp dreamcast/src/menu.cpp dreamcast/tests/render_test.cpp
                                    PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()

    # The Vulkan layer's own test: a device, an offscreen frame, read back. Skips
    # (exit 77) where there's no Vulkan 1.3 device; Linux CI runs it under Xvfb and
    # Mesa with QILIN_REQUIRE_GRAPHICS=1, which makes a skip a failure.
    add_executable(qilin_vktest host/tests/vktest.cpp)
    target_link_libraries(qilin_vktest PRIVATE qilin_host_toycommander qilin_warnings)
    add_test(NAME qilin_vktest COMMAND qilin_vktest)
    set_tests_properties(qilin_vktest PROPERTIES SKIP_RETURN_CODE 77)
    # Controllers reach the game (SDL's virtual joysticks): opened at start-up
    # and later, each taking its port by its first press.
    add_executable(qilin_padtest host/tests/padtest.cpp)
    target_link_libraries(qilin_padtest PRIVATE qilin_host_toycommander SDL3::SDL3-static qilin_warnings)
    add_test(NAME qilin_padtest COMMAND qilin_padtest)
    set_tests_properties(qilin_padtest PROPERTIES SKIP_RETURN_CODE 77)

    # The texture decoder and the renderer, without the game (the renderer's part
    # skips where there's no Vulkan 1.3 device; the test then exits 77).
    add_executable(dreamcast_render_test dreamcast/tests/render_test.cpp dreamcast/src/texture.cpp dreamcast/src/renderer.cpp)
    target_include_directories(dreamcast_render_test PRIVATE dreamcast/src recomp/runtime)
    target_link_libraries(dreamcast_render_test PRIVATE qilin_host_toycommander qilin_warnings)
    add_test(NAME dreamcast_render_test COMMAND dreamcast_render_test)
    set_tests_properties(dreamcast_render_test PROPERTIES SKIP_RETURN_CODE 77)


    # The settings file's reader and writer (from Punch-Out's).
    add_executable(dreamcast_settings_test dreamcast/tests/settings_test.cpp dreamcast/src/settings.cpp)
    target_include_directories(dreamcast_settings_test PRIVATE dreamcast/src recomp/runtime host/include)
    target_link_libraries(dreamcast_settings_test PRIVATE qilin_warnings qilin_file_store)
    target_compile_definitions(dreamcast_settings_test PRIVATE QILIN_APP="toycommander" QILIN_APP_TITLE="Toy Commander")
    add_test(NAME dreamcast_settings_test COMMAND dreamcast_settings_test)

    # The saves' history (from Punch-Out's).
    add_executable(dreamcast_save_history_test dreamcast/tests/save_history_test.cpp dreamcast/src/save_history.cpp)
    target_include_directories(dreamcast_save_history_test PRIVATE dreamcast/src recomp/runtime)
    target_link_libraries(dreamcast_save_history_test PRIVATE qilin_warnings qilin_file_store)
    add_test(NAME dreamcast_save_history_test COMMAND dreamcast_save_history_test)

    add_executable(qilin_frame_times_test host/tests/frame_times_test.cpp)
    target_include_directories(qilin_frame_times_test PRIVATE host/include)
    target_link_libraries(qilin_frame_times_test PRIVATE qilin_warnings)
    add_test(NAME qilin_frame_times_test COMMAND qilin_frame_times_test)

    # The sound queue under SDL's real callbacks (its dummy device), with
    # pushes, rate changes and volume from other threads (from Punch-Out's).
    add_executable(qilin_audio_callback_test host/tests/audio_callback_test.cpp)
    target_link_libraries(qilin_audio_callback_test PRIVATE qilin_host_toycommander SDL3::SDL3-static qilin_warnings)
    add_test(NAME qilin_audio_callback_test COMMAND qilin_audio_callback_test)
    set_tests_properties(qilin_audio_callback_test PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy;SDL_AUDIO_DEVICE_SAMPLE_FRAMES=128" TIMEOUT 25)

    # The downloads' launcher, `Play <Title>` (Windows: .exe; macOS: the app's
    # program): one program for both games, which reads kit/game.txt. Dear
    # ImGui over SDL_Renderer (its Vulkan or software driver).
    add_library(qilin_imgui_sdlrenderer STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_link_libraries(qilin_imgui_sdlrenderer PUBLIC qilin_imgui)
    if(MSVC)
        target_compile_options(qilin_imgui_sdlrenderer PRIVATE /W0)
    else()
        target_compile_options(qilin_imgui_sdlrenderer PRIVATE -w)
    endif()
    add_executable(qilin_launcher WIN32 tools/player-kit/launcher/main.cpp tools/player-kit/launcher/vulkan_check.cpp)
    target_link_libraries(qilin_launcher PRIVATE qilin_launcher_core qilin_imgui_sdlrenderer qilin_imgui qilin_volk SDL3::SDL3-static Threads::Threads
                                                 qilin_warnings)
    qilin_tool(qilin_launcher)
    if(WIN32)
        # Its icon, file details and manifest (launcher.rc), and the same icon
        # for the game, which the player's PC links (the player kit's icon.o).
        # Off only where there's no resource compiler (zig as the compiler).
        option(QILIN_WINDOWS_RESOURCES "Compile the launcher's and the game's icon and file details (needs windres or rc)" ON)
        if(QILIN_WINDOWS_RESOURCES)
            enable_language(RC)
            target_sources(qilin_launcher PRIVATE tools/player-kit/launcher/launcher.rc)
            if(MSVC)
                target_link_options(qilin_launcher PRIVATE /MANIFEST:NO)  # the manifest is launcher.rc's
            endif()
            add_library(qilin_game_icon OBJECT tools/player-kit/game_icon.rc)
        elseif(QILIN_PLAYER_KIT)
            message(FATAL_ERROR "the player kit needs QILIN_WINDOWS_RESOURCES=ON (the game's icon)")
        endif()
    endif()
endif()

# ---- The games: each built only from the player's own boot file ----
# cmake -DQILIN_TOYCOMMANDER_BOOT=<extracted disc>/files/1GUTH.BIN and/or
# -DQILIN_TOYRACER_BOOT=<extracted disc>/files/1TOYRACE.BIN (or the disc's
# .cue): qilin lift turns each into C++ at build time (into the build tree,
# never the source tree), and the game's program (toycommander, toyracer) is
# linked from it and the rest, which is the library <game>_game: the game's
# own code (<game>/src), the shared Dreamcast code (dreamcast/src) and the
# runtime. That library is built whether or not a boot file is here, and
# linked with an empty function table (<game>_link_check), so CI checks every
# game's program with every compiler. It reads the rest of the game from the
# player's disc dump at run time.
set(QILIN_TOYCOMMANDER_BOOT "" CACHE FILEPATH "Toy Commander (USA, MK-57020, V1.022) 1GUTH.BIN or the disc's .cue: builds toycommander")
set(QILIN_TOYRACER_BOOT "" CACHE FILEPATH "Toy Racer (Europe, MK-5114950, V1.050) 1TOYRACE.BIN or the disc's .cue: builds toyracer")
option(QILIN_RUN_CHECKED "the games check every guest memory access (bring-up; off for the player kit)" ON)
option(QILIN_PLAYER_KIT "Build the player kits (tools/player-kit): each game's library as one archive, for the Windows and Linux downloads" OFF)
set(QILIN_RT_SOURCES recomp/runtime/dispatch.cpp recomp/runtime/system.cpp recomp/runtime/memory.cpp recomp/runtime/bus.cpp)
set(QILIN_DREAMCAST_SOURCES dreamcast/src/pvr.cpp dreamcast/src/texture.cpp dreamcast/src/renderer.cpp dreamcast/src/video.cpp
                            dreamcast/src/maple.cpp dreamcast/src/sh4.cpp dreamcast/src/clock.cpp dreamcast/src/ta.cpp
                            dreamcast/src/interrupts.cpp dreamcast/src/aica.cpp dreamcast/src/sound.cpp dreamcast/src/flash.cpp)
# Each game's own files (<game>/src: its build.hpp, the addresses), and the
# Shinobi replacements both games share (shinobi/src), compiled into each
# game's library with that game's build.hpp.
set(QILIN_GAME_FILES game natives)
set(QILIN_SHINOBI_SOURCES shinobi/src/main.cpp shinobi/src/boot.cpp shinobi/src/heap.cpp shinobi/src/gdrom_bios.cpp
                          shinobi/src/chain.cpp shinobi/src/rtc.cpp shinobi/src/threads.cpp shinobi/src/library.cpp)
if(QILIN_HOST)
    find_package(Threads REQUIRED)
endif()
# What the lifted code is compiled with, here and in the player kit's
# build-flags.txt: the same flags on both sides of the link.
if(MSVC)
    set(QILIN_LIFTED_OPTIONS /bigobj)
else()
    set(QILIN_LIFTED_OPTIONS -ffp-contract=off)
endif()
function(qilin_game game title boot version)
    if(NOT QILIN_HOST)
        if(boot)
            message(FATAL_ERROR "${game} needs the host layer: QILIN_HOST=ON")
        endif()
        return()
    endif()
    set(own)
    foreach(f IN LISTS QILIN_GAME_FILES)
        list(APPEND own ${game}/src/${f}.cpp)
    endforeach()
    add_library(${game}_game STATIC ${own} ${QILIN_SHINOBI_SOURCES} ${QILIN_DREAMCAST_SOURCES} ${QILIN_RT_SOURCES})
    target_include_directories(${game}_game PUBLIC ${game}/src shinobi/src dreamcast/src recomp/runtime)  # the game's build.hpp first
    target_link_libraries(${game}_game PUBLIC qilin_disc dreamcast_menu_${game} qilin_host_${game} qilin_imgui SDL3::SDL3-static Threads::Threads
                                       PRIVATE qilin_warnings)
    target_compile_definitions(${game}_game PUBLIC QILIN_VERSION="${version}")
    target_compile_options(${game}_game PUBLIC ${QILIN_LIFTED_OPTIONS})
    if(QILIN_RUN_CHECKED)
        target_compile_definitions(${game}_game PUBLIC QILIN_CHECKED_MEMORY)
    endif()
    if(MSVC)
        target_link_options(${game}_game INTERFACE /STACK:16777216)  # lifted calls nest as deeply as the guest's
    elseif(WIN32)
        target_link_options(${game}_game INTERFACE -Wl,--stack,16777216)
    endif()
    # The program linked without the lifted code: the library is complete.
    add_executable(${game}_link_check tools/player-kit/link_check.cpp)
    target_link_libraries(${game}_link_check PRIVATE ${game}_game)

    if(QILIN_PLAYER_KIT)
        qilin_player_kit(${game})
    endif()
    if(NOT boot)
        return()
    endif()
    set(QILIN_LIFT_FILES 24)
    set(gen ${CMAKE_BINARY_DIR}/generated/${game})
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
        COMMAND qilin lift ${boot} --out ${gen} --files ${QILIN_LIFT_FILES}
        DEPENDS qilin ${boot}
        COMMENT "Lifting ${title}'s boot file to C++"
        VERBATIM)  # quoted for the shell: a Redump dump's name has parentheses and commas
    add_executable(${game} ${gen_sources})
    target_include_directories(${game} PRIVATE ${gen})
    target_link_libraries(${game} PRIVATE ${game}_game)
endfunction()

# ---- The player kits: the downloads (tools/player-kit, as Punch-Out's) ----
# Per game, everything but the lifted code in one archive (lib<game>_kit.a),
# the runtime's headers, the recompiler, and build-flags.txt (how the player's PC
# compiles and links the code it lifts from its own disc). Windows:
# cross-compiled on Linux with llvm-mingw (tools/player-kit/package.sh).
# Linux: built with the zig the download bundles, so the library's libc++ is
# the one the player's compiles link with (tools/player-kit/package-linux.sh).
function(qilin_player_kit game)
    if(NOT TARGET SDL3-static)
        message(FATAL_ERROR "the player kit needs SDL3 built statically (QILIN_HOST=ON)")
    endif()
    set(kit_libraries ${game}_game qilin_file_store qilin_disc ${QILIN_CHDR_LIBRARIES} dreamcast_menu_${game} qilin_host_${game} qilin_imgui qilin_volk SDL3-static)
    set(kit_archive "${CMAKE_BINARY_DIR}/kit/${game}/lib${game}_kit.a")
    set(mri "CREATE ${kit_archive}\n")
    foreach(library IN LISTS kit_libraries)
        string(APPEND mri "ADDLIB $<TARGET_FILE:${library}>\n")
    endforeach()
    string(APPEND mri "SAVE\nEND\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/kit/${game}/kit.mri" CONTENT "${mri}")
    add_custom_command(OUTPUT "${kit_archive}"
        COMMAND "${CMAKE_COMMAND}" "-DAR=${CMAKE_AR}" "-DMRI=${CMAKE_BINARY_DIR}/kit/${game}/kit.mri" "-DOUT=${kit_archive}"
                -P "${CMAKE_SOURCE_DIR}/tools/player-kit/merge.cmake"
        DEPENDS ${kit_libraries} "${CMAKE_BINARY_DIR}/kit/${game}/kit.mri"
        COMMENT "Merging ${game}'s player kit" VERBATIM)
    add_custom_target(${game}_kit ALL DEPENDS "${kit_archive}")
    string(JOIN " " lifted_text ${QILIN_LIFTED_OPTIONS})
    if(WIN32)
        # The system libraries SDL3 and the runtime need (MinGW names).
        set(kit_system_libraries -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32
            -lsetupapi -lshell32 -ldinput8 -lwinspool -lcomdlg32)
        set(kit_ldflags "-static -O2 -Wl,--stack,16777216")
        set(kit_icon " $kit/lib/icon.o")
        set(kit_native "")  # the launcher's default, -march=native
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # SDL3 loads X11, Wayland, the sound servers and Vulkan at run time:
        # the game links only glibc's own libraries (and zig's libc++).
        set(kit_system_libraries -lm -ldl -lpthread -lrt)
        set(kit_ldflags "-O2")
        set(kit_icon "")
        # For the player's processor: x86-64's -march, ARM's -mcpu.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            set(kit_native "native = -mcpu=native\n")
        else()
            set(kit_native "native = -march=native\n")
        endif()
    else()
        message(FATAL_ERROR "no player kit for ${CMAKE_SYSTEM_NAME} yet")
    endif()
    string(JOIN " " kit_system_text ${kit_system_libraries})
    # Read by the launcher (tools/player-kit/launcher/build.cpp): `name = value`
    # lines, $kit the kit's folder.
    file(WRITE "${CMAKE_BINARY_DIR}/kit/${game}/build-flags.txt"
        "# Written by CMake for the player kit (QILIN_PLAYER_KIT).\n"
        "cxxflags = -O2 -DNDEBUG -std=c++23 -w ${lifted_text} -I$kit/include\n"
        "${kit_native}"
        "ldflags = ${kit_ldflags}\n"
        "libs = $kit/lib/lib${game}_kit.a${kit_icon} ${kit_system_text}\n")
    # The kit's archive linked with stand-ins for the lifted code: it's complete.
    add_executable(${game}_kit_link_check tools/player-kit/link_check.cpp)
    target_include_directories(${game}_kit_link_check PRIVATE recomp/runtime)
    target_link_libraries(${game}_kit_link_check PRIVATE "${kit_archive}" ${kit_system_libraries})
    add_dependencies(${game}_kit_link_check ${game}_kit)
    install(TARGETS qilin RUNTIME DESTINATION kit COMPONENT ${game})
    install(FILES "${kit_archive}" DESTINATION kit/lib COMPONENT ${game})
    if(WIN32)
        # The game's icon (the launcher's), linked into it on the player's PC.
        add_dependencies(${game}_kit qilin_game_icon)
        install(FILES $<TARGET_OBJECTS:qilin_game_icon> DESTINATION kit/lib RENAME icon.o COMPONENT ${game})
    endif()
    install(FILES "${CMAKE_BINARY_DIR}/kit/${game}/build-flags.txt" DESTINATION kit COMPONENT ${game})
    file(GLOB kit_headers recomp/runtime/*.hpp)
    install(FILES ${kit_headers} DESTINATION kit/include COMPONENT ${game})
endfunction()

if(QILIN_PLAYER_KIT AND QILIN_RUN_CHECKED)
    message(FATAL_ERROR "the player kit is a release build: configure it with -DQILIN_RUN_CHECKED=OFF")
endif()
qilin_game(toycommander "Toy Commander" "${QILIN_TOYCOMMANDER_BOOT}" ${QILIN_TOYCOMMANDER_VERSION})
qilin_game(toyracer "Toy Racer" "${QILIN_TOYRACER_BOOT}" ${QILIN_TOYRACER_VERSION})

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
