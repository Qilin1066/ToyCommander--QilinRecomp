# The launcher's CMake side, the same in every project (ADOPTING.md): include
# this file from the project's CMakeLists.txt, then call
# qilin_launcher_program() where SDL3, Dear ImGui and volk are known.
#
#   include(tools/player-kit/launcher/launcher.cmake)
#       qilin_launcher_core   the build engine, processes, input check, updates
#       qilin_launcher_test   its portable parts (ctest: qilin_launcher_<part>_test)
#       qilin_launcher_stub, qilin_launcher_build_test
#                             the build with stand-in children (not when
#                             cross-compiling); QILIN_LAUNCHER_QILIN names the
#                             project's recompiler target (default qilin), whose
#                             `identify` the test runs too
#   qilin_launcher_program(IMGUI_DIR <imgui source> LIBRARIES <imgui> <volk> <SDL3> ...)
#       qilin_launcher        the program (Windows: launcher.rc where the
#                             option QILIN_WINDOWS_RESOURCES, defined here, is
#                             on; macOS: the Apple side and "Play on iPad")
#
# Optional from the project: a qilin_warnings target (linked to each target)
# and a qilin_tool(<target>) function (called for each).
set(QILIN_LAUNCHER_DIR ${CMAKE_CURRENT_LIST_DIR})
file(READ "${QILIN_LAUNCHER_DIR}/LAUNCHER_VERSION" QILIN_LAUNCHER_VERSION)
string(STRIP "${QILIN_LAUNCHER_VERSION}" QILIN_LAUNCHER_VERSION)
if(NOT QILIN_LAUNCHER_VERSION MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${QILIN_LAUNCHER_DIR}/LAUNCHER_VERSION must hold a number")
endif()
if(NOT DEFINED QILIN_LAUNCHER_QILIN)
    set(QILIN_LAUNCHER_QILIN qilin)
endif()
if(WIN32)
    # launcher.rc (and the project's own resources, the game's icon): off only
    # where there's no resource compiler (zig as the compiler).
    option(QILIN_WINDOWS_RESOURCES "Compile the launcher's and the game's icon and file details (needs windres or rc)" ON)
    if(QILIN_WINDOWS_RESOURCES)
        enable_language(RC)  # at file scope: CMake allows it nowhere else
    endif()
endif()

function(qilin_launcher_target target)
    if(TARGET qilin_warnings)
        target_link_libraries(${target} PRIVATE qilin_warnings)
    endif()
    if(COMMAND qilin_tool)
        qilin_tool(${target})
    endif()
endfunction()

# The portable parts (the JSON reader, versions and the releases list, the
# kit's text files and commands, SHA-256, what "Play on iPad" reads from
# Apple's tools), tested everywhere.
add_executable(qilin_launcher_test ${QILIN_LAUNCHER_DIR}/launcher_test.cpp)
qilin_launcher_target(qilin_launcher_test)
foreach(part IN ITEMS json version kit ios)
    add_test(NAME qilin_launcher_${part}_test COMMAND qilin_launcher_test ${part})
endforeach()

# The build engine, process handling, input check and updates, on every
# desktop system: what the system provides is platform_win.cpp,
# process_win.cpp and shortcut_win.cpp on Windows, the _posix.cpp ones on
# Linux and macOS.
if(WIN32)
    set(qilin_launcher_system win)
else()
    set(qilin_launcher_system posix)
endif()
add_library(qilin_launcher_core STATIC ${QILIN_LAUNCHER_DIR}/kit.cpp ${QILIN_LAUNCHER_DIR}/input.cpp ${QILIN_LAUNCHER_DIR}/build.cpp
                                       ${QILIN_LAUNCHER_DIR}/update.cpp ${QILIN_LAUNCHER_DIR}/tidy.cpp
                                       ${QILIN_LAUNCHER_DIR}/platform_${qilin_launcher_system}.cpp
                                       ${QILIN_LAUNCHER_DIR}/process_${qilin_launcher_system}.cpp
                                       ${QILIN_LAUNCHER_DIR}/shortcut_${qilin_launcher_system}.cpp)
target_include_directories(qilin_launcher_core PUBLIC ${QILIN_LAUNCHER_DIR})
target_compile_definitions(qilin_launcher_core PRIVATE QILIN_LAUNCHER_VERSION="${QILIN_LAUNCHER_VERSION}")
if(WIN32)
    target_link_libraries(qilin_launcher_core PUBLIC winhttp ole32 uuid shell32)
endif()
qilin_launcher_target(qilin_launcher_core)

# The build test runs the real build with stand-in children (test_stub.cpp)
# on every system it is built for (not a cross-compile), and the project's
# recompiler's `identify`.
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(qilin_launcher_stub ${QILIN_LAUNCHER_DIR}/test_stub.cpp)
    add_executable(qilin_launcher_build_test ${QILIN_LAUNCHER_DIR}/launcher_build_test.cpp)
    target_link_libraries(qilin_launcher_build_test PRIVATE qilin_launcher_core)
    if(WIN32)
        target_link_libraries(qilin_launcher_stub PRIVATE shell32)
        target_link_libraries(qilin_launcher_build_test PRIVATE shell32)
    endif()
    qilin_launcher_target(qilin_launcher_stub)
    qilin_launcher_target(qilin_launcher_build_test)
    add_test(NAME qilin_launcher_build_test
             COMMAND qilin_launcher_build_test "$<TARGET_FILE:qilin_launcher_stub>" "$<TARGET_FILE:${QILIN_LAUNCHER_QILIN}>"
                     "${CMAKE_CURRENT_BINARY_DIR}/launcher-check/$<CONFIG>")
    set_tests_properties(qilin_launcher_build_test PROPERTIES TIMEOUT 120)
endif()

# The program, `Play <Title>` (Windows: .exe; macOS: the app's program): Dear
# ImGui over SDL_Renderer (its Vulkan or software driver).
function(qilin_launcher_program)
    cmake_parse_arguments(L "" "IMGUI_DIR" "LIBRARIES" ${ARGN})
    if(NOT L_IMGUI_DIR OR NOT EXISTS "${L_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
        message(FATAL_ERROR "qilin_launcher_program needs IMGUI_DIR, Dear ImGui's source folder")
    endif()
    add_library(qilin_imgui_sdlrenderer STATIC ${L_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_link_libraries(qilin_imgui_sdlrenderer PUBLIC ${L_LIBRARIES})
    if(MSVC)
        target_compile_options(qilin_imgui_sdlrenderer PRIVATE /W0)  # third-party code
    else()
        target_compile_options(qilin_imgui_sdlrenderer PRIVATE -w)
    endif()
    add_executable(qilin_launcher WIN32 ${QILIN_LAUNCHER_DIR}/main.cpp ${QILIN_LAUNCHER_DIR}/vulkan_check.cpp)
    target_link_libraries(qilin_launcher PRIVATE qilin_launcher_core qilin_imgui_sdlrenderer ${L_LIBRARIES})
    qilin_launcher_target(qilin_launcher)
    if(WIN32 AND QILIN_WINDOWS_RESOURCES)
        # Its icon, file details and manifest.
        target_sources(qilin_launcher PRIVATE ${QILIN_LAUNCHER_DIR}/launcher.rc)
        if(MSVC)
            target_link_options(qilin_launcher PRIVATE /MANIFEST:NO)  # the manifest is launcher.rc's
        endif()
    endif()
    if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # Apple's compiler checked before a build, the game started so that it
        # finds the app's MoltenVK (apple_tools.hpp), and "Play on iPad" where
        # the app carries an iOS kit (ios_install.hpp).
        target_sources(qilin_launcher PRIVATE ${QILIN_LAUNCHER_DIR}/apple_tools.cpp ${QILIN_LAUNCHER_DIR}/ios_install.cpp)
    endif()
endfunction()
