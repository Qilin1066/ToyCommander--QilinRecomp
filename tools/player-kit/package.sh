#!/bin/sh
# Builds a game's Windows download (as Punch-Out's):
#   tools/player-kit/package.sh <toycommander|toyracer> <version> <out.zip>
#
# Cross-compiles on Linux with llvm-mingw, then lays out:
#   <Package>/Play <Title>.exe, README.txt, LICENSES/
#   <Package>/kit/  qilin.exe, lib/lib<game>_kit.a, include/, kit.ninja,
#                   game.txt, toolchain/ (llvm-mingw, trimmed), VERSION
# Nothing in it is made from the game: the player's PC lifts the boot file
# from their own disc dump and compiles that code with the bundled compiler
# (Play <Title>.exe, the launcher: tools/player-kit/launcher, the same
# program for both games, which reads kit/game.txt).
#
# Environment:
#   LLVM_MINGW_CROSS    the Linux-hosted llvm-mingw release, unpacked
#   LLVM_MINGW_WINDOWS  the x86_64 Windows-hosted release of the same version, unpacked
#   ZSTD_LICENSE        zstd v1.5.7's LICENSE file (its BSD licence, for LICENSES/libchdr)
#   BUILD_DIR           where to build (default build-player-kit)
#   CMAKE_EXTRA         more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=...)
set -eu
game=$1
version=$2
out=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/games.sh"
build=${BUILD_DIR:-$root/build-player-kit}
stage=$(mktemp -d)
pkg="$stage/$PACKAGE"

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$here/mingw-cross.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DQILIN_PLAYER_KIT=ON -DQILIN_RUN_CHECKED=OFF -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target qilin qilin_launcher "${game}_kit" "${game}_kit_link_check" --parallel
cmake --install "$build" --component "$game" --prefix "$pkg"

"$here/trim-toolchain.sh" "$LLVM_MINGW_WINDOWS" "$pkg/kit/toolchain"
cp "$build/qilin_launcher.exe" "$pkg/Play $TITLE.exe"
# What the launcher needs to know about the game, one name=value a line:
# its data folder's and tags' name, its titles, the disc it builds from
# (`build` is recomp/src/bootfile.cpp's known build's title), the release's
# zip name and where releases are published.
printf 'game=%s\r\ntitle=%s\r\nexe=%s\r\nboot=%s\r\nbuild=%s\r\nproduct=%s\r\ndisc_version=%s\r\nrelease=%s\r\npackage=%s\r\nrepo=%s\r\n' \
    "$game" "$TITLE" "$EXE" "$BOOT" "$BUILD_TITLE" "$PRODUCT" "$DISC_VERSION" "$RELEASE" "$PACKAGE" "Qilin1066/ToyCommander--QilinRecomp" \
    > "$pkg/kit/game.txt"
fill "$here/README.txt.in" | sed 's/$/\r/' > "$pkg/README.txt"
printf '%s\n' "$version" > "$pkg/kit/VERSION"

# Every licence of what's included, by component.
lic="$pkg/LICENSES"
mkdir -p "$lic/$PACKAGE" "$lic/llvm-mingw" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk"
mv "$pkg/kit/toolchain/licenses/"* "$lic/llvm-mingw/"
rmdir "$pkg/kit/toolchain/licenses"
sed 's/$/\r/' "$root/LICENSE" > "$lic/$PACKAGE/LICENSE.txt"  # GPL-3.0
cp "$build/_deps/sdl3-src/LICENSE.txt" "$lic/SDL3/"
imgui=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_IMGUI:PATH=//p' "$build/CMakeCache.txt")
[ -n "$imgui" ] || imgui="$build/_deps/imgui-src"
cp "$imgui/LICENSE.txt" "$lic/Dear ImGui/"
cp "$build/_deps/vulkanheaders-src/LICENSE.md" "$lic/Vulkan-Headers/" 2>/dev/null || cp "$build/_deps/vulkanheaders-src/LICENSE"* "$lic/Vulkan-Headers/"
cp "$build/_deps/volk-src/LICENSE.md" "$lic/volk/"
# libchdr (the .chd reader) and what it bundles: LZMA, miniz, dr_flac (their
# licence texts are in their sources) and zstd's decompressor, taken under
# its BSD licence (ZSTD_LICENSE: that file from zstd's own repository).
chdr=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_LIBCHDR:PATH=//p' "$build/CMakeCache.txt")
[ -n "$chdr" ] || chdr="$build/_deps/libchdr-src"
mkdir -p "$lic/libchdr"
cp "$chdr/LICENSE.txt" "$lic/libchdr/LICENSE.txt"
cp "$chdr"/deps/lzma-*/LICENSE "$lic/libchdr/LZMA-LICENSE.txt"
sed -n '/This is free and unencumbered/,/unlicense.org/p' "$chdr"/deps/miniz-*/miniz.c > "$lic/libchdr/miniz-LICENSE.txt"
sed -n '/^This software is available as a choice/,$p' "$chdr/include/dr_libs/dr_flac.h" > "$lic/libchdr/dr_flac-LICENSE.txt"
for f in miniz dr_flac; do [ -s "$lic/libchdr/$f-LICENSE.txt" ] || { echo "no licence text found for $f" >&2; exit 1; }; done
cp "${ZSTD_LICENSE:?set ZSTD_LICENSE to zstd v1.5.7 LICENSE file}" "$lic/libchdr/zstd-LICENSE.txt"

rm -f "$out"
(cd "$stage" && zip -q -r -9 "$out" "$PACKAGE")
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
