#!/bin/sh
# Builds a game's iOS kit (iPadOS and iOS), which the macOS download carries
# in its app as Contents/Resources/ios-kit (package-macos.sh):
#   tools/player-kit/package-ios-kit.sh <toycommander|toyracer> <version> <out folder> [device|simulator]
#
# On a Mac with Xcode 16 or newer (the iOS SDK), arm64, iOS 16 and later:
#   <out>/lib/lib<game>_ios_kit.a   everything but the lifted code, MoltenVK too
#        include/                   the runtime's headers
#        build-flags.txt            sdk, cxxflags ($kit: the ios-kit folder)
#        xcode/                     QilinGame.xcodeproj, Info.plist, Game.xcconfig,
#                                   QilinGame.xcconfig, app.c, Assets.xcassets
#        README-ios.txt, VERSION
# `simulator` builds the same for the iOS Simulator (CI's check only; the
# download carries the device's). Nothing in it is made from the game.
#
# Environment:
#   MOLTENVK_IOS_LIB        device: MoltenVK's release, MoltenVK-ios.tar's
#                           static MoltenVK.xcframework/ios-arm64/libMoltenVK.a
#   MOLTENVK_SIMULATOR_LIB  simulator: MoltenVK-all.tar's
#                           MoltenVK.xcframework/ios-arm64_x86_64-simulator/libMoltenVK.a
#   BUILD_DIR               where to build (default build-ios-kit-<device|simulator>)
#   CMAKE_EXTRA             more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=...)
set -eu
game=$1
version=$2
mkdir -p "$3"
out=$(cd "$3" && pwd)
sdk=${4:-device}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/games.sh"
EXE=${EXE%.exe}
case "$sdk" in
device) sysroot=iphoneos; moltenvk=${MOLTENVK_IOS_LIB:?set MOLTENVK_IOS_LIB to MoltenVK-ios.tar static ios-arm64/libMoltenVK.a} ;;
simulator) sysroot=iphonesimulator; moltenvk=${MOLTENVK_SIMULATOR_LIB:?set MOLTENVK_SIMULATOR_LIB to MoltenVK-all.tar static ios-arm64_x86_64-simulator/libMoltenVK.a} ;;
*) echo "package-ios-kit: device or simulator, not $sdk" >&2; exit 2 ;;
esac
[ -f "$moltenvk" ] || { echo "package-ios-kit: no $moltenvk" >&2; exit 1; }
build=${BUILD_DIR:-$root/build-ios-kit-$sdk}
# The Simulator's library holds x86_64 too: the kit is arm64 only.
if [ "$(lipo -archs "$moltenvk")" != arm64 ]; then
    mkdir -p "$build"
    lipo "$moltenvk" -thin arm64 -output "$build/libMoltenVK-arm64.a"
    moltenvk="$build/libMoltenVK-arm64.a"
fi

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS "-DCMAKE_OSX_SYSROOT=$sysroot" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 "-DQILIN_MOLTENVK_STATIC=$moltenvk" \
      -DQILIN_PLAYER_KIT=ON -DQILIN_RUN_CHECKED=OFF -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target "${game}_ios_kit" --parallel

rm -rf "$out"
mkdir -p "$out/lib" "$out/include" "$out/xcode"
cp "$build/ios-kit/$game/lib/lib${game}_ios_kit.a" "$out/lib/"
cp "$build/ios-kit/$game/build-flags.txt" "$out/"
cp "$root"/recomp/runtime/*.hpp "$out/include/"
cp -R "$here/ios/QilinGame.xcodeproj" "$here/ios/Assets.xcassets" "$here/ios/Info.plist" "$here/ios/QilinGame.xcconfig" \
      "$here/ios/app.c" "$out/xcode/"
python3 "$here/ios/make_ios_icon.py" "$out/xcode/Assets.xcassets/AppIcon.appiconset/AppIcon.png"
plutil -lint "$out/xcode/Info.plist"

# The bundle's version: the release's X.Y.Z (an untagged build's describe
# text starts with the last tag's).
short=$(printf '%s\n' "$version" | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
[ -n "$short" ] || short=0.0.0
cat > "$out/xcode/Game.xcconfig" <<EOF
// $TITLE's defaults (tools/player-kit/package-ios-kit.sh): xcodebuild's
// command line sets PRODUCT_NAME, PRODUCT_BUNDLE_IDENTIFIER, DEVELOPMENT_TEAM,
// QILIN_LIFTED_LIB and QILIN_KIT_LIB (README-ios.txt).
#include "QilinGame.xcconfig"

QILIN_GAME = $game
QILIN_DISPLAY_NAME = $TITLE
PRODUCT_NAME = $EXE
PRODUCT_BUNDLE_IDENTIFIER = io.github.qilin1066.$game-qilinrecomp
MARKETING_VERSION = $short
CURRENT_PROJECT_VERSION = $short
EOF
fill "$here/ios/README-ios.txt.in" > "$out/README-ios.txt"
printf '%s\n' "$version" > "$out/VERSION"
echo "Packaged the $sdk iOS kit in $out ($(du -sh "$out" | cut -f1))"
