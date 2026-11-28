# The iOS kit's CMake side (iPadOS and iOS; tools/player-kit/package-ios-kit.sh
# lays the kit out): included from CMakeLists.txt when CMAKE_SYSTEM_NAME is
# iOS, for a device (CMAKE_OSX_SYSROOT=iphoneos) or the Simulator
# (iphonesimulator), arm64 only.

# MoltenVK's static library for the SDK being built for, from its release
# (MoltenVK.xcframework's ios-arm64 or ios-arm64_x86_64-simulator folder):
# merged into the kit's archive, so the app needs no framework of its own.
set(QILIN_MOLTENVK_STATIC "" CACHE FILEPATH "MoltenVK's libMoltenVK.a for this SDK (its release's static MoltenVK.xcframework)")

# A game's iOS kit (called by qilin_player_kit): everything but the lifted
# code in one static archive, lib<game>_ios_kit.a (the game's library, the
# runtime, SDL3, Dear ImGui, volk, libchdr and what it bundles, MoltenVK), and
# build-flags.txt, which the Mac launcher compiles the player's lifted code
# with (xcrun --sdk iphoneos clang++). The app around them is the kit's Xcode
# project (tools/player-kit/ios).
function(qilin_player_kit_ios game)
    if(NOT TARGET SDL3-static)
        message(FATAL_ERROR "the iOS kit needs SDL3 built statically (QILIN_HOST=ON)")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        message(FATAL_ERROR "the iOS kit is arm64 only: configure it with -DCMAKE_OSX_ARCHITECTURES=arm64")
    endif()
    if(NOT QILIN_MOLTENVK_STATIC OR NOT EXISTS "${QILIN_MOLTENVK_STATIC}")
        message(FATAL_ERROR "the iOS kit needs MoltenVK's static library: -DQILIN_MOLTENVK_STATIC=<MoltenVK.xcframework/.../libMoltenVK.a>")
    endif()
    # The device's SDK or the Simulator's: the lifted code's target.
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(target "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}-simulator")
        set(sdk iphonesimulator)
    else()
        set(target "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
        set(sdk iphoneos)
    endif()
    # Apple's libtool merges archives (Apple's ar has no MRI scripts), as for macOS.
    find_program(QILIN_APPLE_LIBTOOL NAMES libtool PATHS /usr/bin NO_DEFAULT_PATH REQUIRED)
    set(kit_libraries ${game}_game qilin_file_store qilin_disc ${QILIN_CHDR_LIBRARIES} dreamcast_menu_${game} qilin_host_${game} qilin_imgui qilin_volk SDL3-static)
    set(kit_dir "${CMAKE_BINARY_DIR}/ios-kit/${game}")
    set(kit_archive "${kit_dir}/lib/lib${game}_ios_kit.a")
    set(inputs)
    foreach(library IN LISTS kit_libraries)
        list(APPEND inputs "$<TARGET_FILE:${library}>")
    endforeach()
    add_custom_command(OUTPUT "${kit_archive}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${kit_dir}/lib"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${kit_archive}"
        COMMAND "${QILIN_APPLE_LIBTOOL}" -static -no_warning_for_no_symbols -o "${kit_archive}" ${inputs} "${QILIN_MOLTENVK_STATIC}"
        DEPENDS ${kit_libraries} "${QILIN_MOLTENVK_STATIC}"
        COMMENT "Merging ${game}'s iOS kit (${sdk})" VERBATIM)
    add_custom_target(${game}_ios_kit ALL DEPENDS "${kit_archive}")
    string(JOIN " " lifted_text ${QILIN_LIFTED_OPTIONS})
    # Read by the Mac launcher: `name = value` lines, $kit the ios-kit folder.
    # The player's lifted code is compiled with these (xcrun --sdk <sdk>
    # clang++) and archived into one static library, which the Xcode project
    # links with the kit's (QILIN_LIFTED_LIB, QILIN_KIT_LIB). No native line:
    # the code is built for every arm64 iPad and iPhone the iOS version runs on.
    file(WRITE "${kit_dir}/build-flags.txt"
        "# Written by CMake for the iOS kit (QILIN_PLAYER_KIT, CMAKE_SYSTEM_NAME=iOS): ${sdk}, arm64.\n"
        "sdk = ${sdk}\n"
        "cxxflags = -target ${target} -O2 -DNDEBUG -std=c++23 -w ${lifted_text} -I$kit/include\n")
endfunction()
