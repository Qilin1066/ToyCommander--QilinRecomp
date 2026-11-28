#!/bin/sh
# Builds a game's macOS download (what package.sh is for Windows):
#   tools/player-kit/package-macos.sh <toycommander|toyracer> <version> <out.dmg>
#
# On a Mac with Xcode 16 or newer, universal (arm64 and x86_64), into a disk
# image holding the app, a link to /Applications and README.txt:
#   <Title>.app/Contents/Info.plist
#                       MacOS/Play <Title>            the launcher
#                       Frameworks/libMoltenVK.dylib  Vulkan over Metal
#                       Resources/AppIcon.icns, LICENSES/
#                       Resources/kit/  qilin, lib/lib<game>_kit.a, include/,
#                                       build-flags.txt, game.txt, files.txt,
#                                       VERSION
#                       Resources/ios-kit/  the iOS kit, for "Play on iPad"
#                                       (package-ios-kit.sh, README-ios.txt)
# No compiler is bundled: Apple's SDK may not be redistributed, so the
# launcher uses Apple's clang from the Command Line Tools (/usr/bin/clang++)
# and offers Apple's installer where it's missing. Nothing in it is made from
# the game: the player's Mac lifts the boot file from their own disc dump.
# The app is signed ad hoc only (Apple silicon runs nothing unsigned): the
# player allows it once in System Settings, Privacy & Security.
#
# Environment:
#   MOLTENVK          MoltenVK's macOS release (MoltenVK-macos.tar), unpacked
#   MOLTENVK_VERSION  its version (v1.4.2), and MOLTENVK_SHA256 the tar's
#   MOLTENVK_NOTICES  the licences of what MoltenVK's library contains, as
#                     fetched by .github/workflows/player-kit-macos.yml:
#                     SPIRV-Cross-LICENSE, SPIRV-Tools-LICENSE,
#                     SPIRV-Headers-LICENSE, cereal-LICENSE
#   ZSTD_LICENSE      zstd v1.5.7's LICENSE file (its BSD licence, for LICENSES/libchdr)
#   IOS_KIT           the iOS kit, built already (package-ios-kit.sh ... device);
#                     if unset, it's built here, with MOLTENVK_IOS_LIB
#   MOLTENVK_IOS_SHA256  MoltenVK-ios.tar's SHA-256 (the iOS kit's MoltenVK)
#   BUILD_DIR         where to build (default build-player-kit-macos)
#   CMAKE_EXTRA       more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=...)
set -eu
game=$1
version=$2
out=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/games.sh"
EXE=${EXE%.exe}  # no .exe on macOS
build=${BUILD_DIR:-$root/build-player-kit-macos}
moltenvk="${MOLTENVK:?set MOLTENVK to the unpacked MoltenVK-macos.tar}/MoltenVK"
notices=${MOLTENVK_NOTICES:?set MOLTENVK_NOTICES to the folder of MoltenVK contents licences}
stage=$(mktemp -d)
image="$stage/image"
app="$image/$TITLE.app"
contents="$app/Contents"
kit="$contents/Resources/kit"
launcher="Play $TITLE"

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
      -DQILIN_PLAYER_KIT=ON -DQILIN_RUN_CHECKED=OFF -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target qilin qilin_launcher "${game}_kit" "${game}_kit_link_check" --parallel
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources"
cmake --install "$build" --component "$game" --prefix "$contents/Resources"
# The iOS kit: built for iPadOS and iOS (a CMake build of its own), as it is.
ios_kit=${IOS_KIT:-}
if [ -z "$ios_kit" ]; then
    ios_kit="$stage/ios-kit"
    "$here/package-ios-kit.sh" "$game" "$version" "$ios_kit" device
fi
[ -f "$ios_kit/lib/lib${game}_ios_kit.a" ] || { echo "no iOS kit in $ios_kit" >&2; exit 1; }
cp -R "$ios_kit" "$contents/Resources/ios-kit"
cp "$build/qilin_launcher" "$contents/MacOS/$launcher"
cp "$moltenvk/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$contents/Frameworks/"
python3 "$here/launcher/make_icns.py" "$contents/Resources/AppIcon.icns"

# The bundle's version: the release's X.Y.Z (an untagged build's describe
# text starts with the last tag's).
short=$(printf '%s\n' "$version" | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
[ -n "$short" ] || short=0.0.0
cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleDisplayName</key>
	<string>$TITLE</string>
	<key>CFBundleExecutable</key>
	<string>$launcher</string>
	<key>CFBundleIconFile</key>
	<string>AppIcon</string>
	<key>CFBundleIdentifier</key>
	<string>io.github.qilin1066.$game-qilinrecomp</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>$TITLE</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$short</string>
	<key>CFBundleVersion</key>
	<string>$short</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.games</string>
	<key>LSMinimumSystemVersion</key>
	<string>13.3</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSHumanReadableCopyright</key>
	<string>$TITLE QilinRecomp $version, GPL-3.0. Built from your own disc; no game data included.</string>
</dict>
</plist>
EOF
plutil -lint "$contents/Info.plist"

# What the launcher needs to know about the game (games.sh), and the
# compiler: Apple's, from the Command Line Tools.
write_game_txt "$kit/game.txt" "$EXE" "/usr/bin/clang++"
fill "$here/README-macos.txt.in" > "$image/README.txt"
printf '%s\n' "$version" > "$kit/VERSION"

# Every licence of what's included, by component.
lic="$contents/Resources/LICENSES"
mkdir -p "$lic/$PACKAGE" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk" "$lic/MoltenVK"
cp "$root/LICENSE" "$lic/$PACKAGE/LICENSE.txt"  # GPL-3.0
cp "$build/_deps/sdl3-src/LICENSE.txt" "$lic/SDL3/"
imgui=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_IMGUI:PATH=//p' "$build/CMakeCache.txt")
[ -n "$imgui" ] || imgui="$build/_deps/imgui-src"
cp "$imgui/LICENSE.txt" "$lic/Dear ImGui/"
cp "$build/_deps/vulkanheaders-src/LICENSE.md" "$lic/Vulkan-Headers/" 2>/dev/null || cp "$build/_deps/vulkanheaders-src/LICENSE"* "$lic/Vulkan-Headers/"
cp "$build/_deps/volk-src/LICENSE.md" "$lic/volk/"
# libchdr (the .chd reader) and what it bundles, as package.sh collects them.
chdr=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_LIBCHDR:PATH=//p' "$build/CMakeCache.txt")
[ -n "$chdr" ] || chdr="$build/_deps/libchdr-src"
mkdir -p "$lic/libchdr"
cp "$chdr/LICENSE.txt" "$lic/libchdr/LICENSE.txt"
cp "$chdr"/deps/lzma-*/LICENSE "$lic/libchdr/LZMA-LICENSE.txt"
sed -n '/This is free and unencumbered/,/unlicense.org/p' "$chdr"/deps/miniz-*/miniz.c > "$lic/libchdr/miniz-LICENSE.txt"
sed -n '/^This software is available as a choice/,$p' "$chdr/include/dr_libs/dr_flac.h" > "$lic/libchdr/dr_flac-LICENSE.txt"
for f in miniz dr_flac; do [ -s "$lic/libchdr/$f-LICENSE.txt" ] || { echo "no licence text found for $f" >&2; exit 1; }; done
cp "${ZSTD_LICENSE:?set ZSTD_LICENSE to zstd v1.5.7 LICENSE file}" "$lic/libchdr/zstd-LICENSE.txt"
# MoltenVK (Apache-2.0) ships its own licence; its library also contains
# SPIRV-Cross and SPIRV-Tools (Apache-2.0), SPIRV-Headers' tables (MIT-style)
# and cereal (BSD-3-Clause), whose licences are taken at the revisions
# MoltenVK's release names (ExternalRevisions).
cp "$moltenvk/LICENSE" "$lic/MoltenVK/LICENSE.txt"
for f in SPIRV-Cross SPIRV-Tools SPIRV-Headers cereal; do cp "$notices/$f-LICENSE" "$lic/MoltenVK/$f-LICENSE.txt"; done
cat > "$lic/MoltenVK/ABOUT.txt" <<EOF
Contents/Frameworks/libMoltenVK.dylib is MoltenVK ${MOLTENVK_VERSION:?set MOLTENVK_VERSION}
(https://github.com/KhronosGroup/MoltenVK), Vulkan over Apple's Metal, from
its release's MoltenVK-macos.tar (SHA-256 ${MOLTENVK_SHA256:?set MOLTENVK_SHA256}),
unchanged but for its code signature: it is signed again, ad hoc, with the
rest of the app. Its licence is LICENSE.txt (Apache-2.0). The library also
contains SPIRV-Cross and SPIRV-Tools (Apache-2.0), tables from SPIRV-Headers
and cereal (BSD-3-Clause), at the revisions MoltenVK names in its
ExternalRevisions folder: their licences are the other files here.

Contents/Resources/ios-kit/lib/lib${game}_ios_kit.a (the iOS kit, for "Play
on iPad") contains MoltenVK ${MOLTENVK_VERSION} too, built for iPadOS and iOS:
the static library (MoltenVK.xcframework, ios-arm64) from its release's
MoltenVK-ios.tar (SHA-256 ${MOLTENVK_IOS_SHA256:?set MOLTENVK_IOS_SHA256}),
unchanged, with the same contents and licences. The iOS kit's other parts
(SDL3, Dear ImGui, Vulkan-Headers, volk, libchdr and what it bundles) are
the ones whose licences are beside this folder.
EOF

# Every file the release holds (itself too), relative to the app: the
# launcher removes anything else from Resources/kit and Resources/LICENSES
# (tools/player-kit/launcher/tidy.hpp).
(cd "$app" && { find . -type f | sed 's|^\./||'; echo Contents/Resources/kit/files.txt; } | LC_ALL=C sort -u > Contents/Resources/kit/files.txt)

# Signed ad hoc, the parts inside first, then the whole app (which seals
# every file in it). Not notarised: that needs a paid Apple developer account.
codesign --force --sign - "$contents/Frameworks/libMoltenVK.dylib"
codesign --force --sign - "$kit/qilin"
codesign --force --sign - "$app"
codesign --verify --deep --strict --verbose=2 "$app"

# The disk image: the app, a link to /Applications to drag it onto, the readme.
ln -s /Applications "$image/Applications"
rm -f "$out"
made=no
for attempt in 1 2 3; do
    # hdiutil is known to fail now and then on a busy machine ("Resource busy").
    if hdiutil create -volname "$TITLE" -srcfolder "$image" -fs HFS+ -format UDZO -ov "$out"; then made=yes; break; fi
    echo "hdiutil create failed (attempt $attempt)" >&2
    sleep 5
done
[ "$made" = yes ] || { echo "can't make the disk image" >&2; exit 1; }
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
