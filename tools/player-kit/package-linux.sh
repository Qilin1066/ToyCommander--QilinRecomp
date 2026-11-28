#!/bin/sh
# Builds a game's Linux download, for the processor it runs on (x86-64 or
# ARM64, the Raspberry Pi's):
#   tools/player-kit/package-linux.sh <toycommander|toyracer> <version> <out.tar.gz>
#
# Built with the zig it bundles (zig cc and zig c++ as CMake's compilers),
# so the kit's library has the libc++ the player's compiles link with, for
# the native glibc target: zig links against the glibc it finds here, which
# sets the oldest glibc the download runs on (CI builds on Ubuntu 22.04:
# glibc 2.35), and finds the system's X11, Wayland and sound headers for
# SDL3. Everything is compiled for the processor family's baseline
# (-mcpu=baseline), not this computer's. Then lays out:
#   <Package>/Play <Title>, README.txt, LICENSES/
#   <Package>/kit/  qilin, lib/lib<game>_kit.a, include/, build-flags.txt,
#                   game.txt, files.txt, VERSION, icon.png,
#                   toolchain/zig/ (the zig release, less other systems' C libraries)
# Nothing in it is made from the game: the player's computer lifts the boot
# file from their own disc dump and compiles that code with the bundled
# zig (Play <Title>, the launcher, tools/player-kit/launcher).
#
# Environment:
#   ZIG_DIR        the unpacked zig release for this processor (pinned by the workflow)
#   ZSTD_LICENSE   zstd v1.5.7's LICENSE file (its BSD licence, for LICENSES/libchdr)
#   BUILD_DIR      where to build (default build-player-kit-linux)
#   CMAKE_EXTRA    more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=...)
set -eu
game=$1
version=$2
out=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/games.sh"
zig_dir=$(cd "${ZIG_DIR:?set ZIG_DIR to the unpacked zig release}" && pwd)
build=${BUILD_DIR:-$root/build-player-kit-linux}
stage=$(mktemp -d)
pkg="$stage/$PACKAGE"
exe=${EXE%.exe}   # ToyCommander: no .exe on Linux

# zig as CMake's compilers and archiver, through wrappers (CMake wants one
# program per tool).
tools="$build/zig-tools"
mkdir -p "$tools"
wrap() {
    printf '#!/bin/sh\nexec %s %s "$@"\n' "'$zig_dir/zig'" "$2" > "$tools/$1"
    chmod 755 "$tools/$1"
}
wrap zig-cc "cc -mcpu=baseline"
wrap zig-c++ "c++ -mcpu=baseline"
wrap zig-ar ar
wrap zig-ranlib ranlib

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$tools/zig-cc" -DCMAKE_CXX_COMPILER="$tools/zig-c++" \
      -DCMAKE_AR="$tools/zig-ar" -DCMAKE_RANLIB="$tools/zig-ranlib" \
      -DQILIN_PLAYER_KIT=ON -DQILIN_RUN_CHECKED=OFF -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target qilin qilin_launcher "${game}_kit" "${game}_kit_link_check" --parallel
cmake --install "$build" --component "$game" --prefix "$pkg"

cp "$build/qilin_launcher" "$pkg/Play $TITLE"
chmod 755 "$pkg/Play $TITLE" "$pkg/kit/qilin"

# zig, without the C libraries and headers of systems it won't compile for
# here (Windows, macOS, the BSDs, WebAssembly): about 130 MB of 300.
mkdir -p "$pkg/kit/toolchain"
cp -R "$zig_dir" "$pkg/kit/toolchain/zig"
z="$pkg/kit/toolchain/zig"
rm -rf "$z/doc" "$z/lib/libc/mingw" "$z/lib/libc/darwin" "$z/lib/libc/wasi" \
       "$z/lib/libc/freebsd" "$z/lib/libc/netbsd" "$z/lib/libc/openbsd"
for d in "$z"/lib/libc/include/*; do
    case $(basename "$d") in
    *-windows-*|*-darwin-*|*-freebsd*|*-netbsd*|*-openbsd*|generic-freebsd|generic-netbsd|generic-openbsd|wasm*) rm -rf "$d" ;;
    esac
done

# What the launcher needs to know about the game, one name=value a line:
# its data folder's and tags' name, its titles, the disc it builds from
# (`build` is recomp/src/bootfile.cpp's known build's title), the release's
# folder name, where releases are published, and the compiler (zig, run as
# `zig c++`).
printf 'game=%s\ntitle=%s\nexe=%s\nboot=%s\nbuild=%s\nproduct=%s\ndisc_version=%s\nrelease=%s\npackage=%s\nrepo=%s\ncompiler=%s\ncompiler_args=%s\n' \
    "$game" "$TITLE" "$exe" "$BOOT" "$BUILD_TITLE" "$PRODUCT" "$DISC_VERSION" "$RELEASE" "$PACKAGE" "Qilin1066/ToyCommander--QilinRecomp" \
    "toolchain/zig/zig" "c++" > "$pkg/kit/game.txt"
fill "$here/README-linux.txt.in" | sed "s|@EXE_LINUX@|$exe|g" > "$pkg/README.txt"
printf '%s\n' "$version" > "$pkg/kit/VERSION"
python3 "$here/launcher/make_icon.py" --png "$pkg/kit/icon.png"   # the menu entry's

# Every licence of what's included, by component.
lic="$pkg/LICENSES"
mkdir -p "$lic/$PACKAGE" "$lic/zig" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk"
cp "$root/LICENSE" "$lic/$PACKAGE/LICENSE.txt"  # GPL-3.0
# zig's (MIT) and those of what it bundles, as zig ships them, at their
# places in its folder: libc++, libc++abi and libunwind (the LLVM
# licence), glibc's and musl's notices for their headers and stubs.
(cd "$z" && find . -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' -o -iname 'NOTICE*' \) | LC_ALL=C sort) |
while read -r f; do
    mkdir -p "$lic/zig/$(dirname "$f")"
    cp "$z/$f" "$lic/zig/$f"
done
[ -s "$lic/zig/LICENSE" ] && [ -s "$lic/zig/lib/libcxx/LICENSE.TXT" ] || { echo "zig's licences weren't found" >&2; exit 1; }
cat > "$lic/zig/README.txt" <<'EOF'
zig (kit/toolchain/zig) is under the MIT licence: LICENSE.
The zig program contains LLVM, Clang and LLD, under the Apache License 2.0
with LLVM Exceptions: the same text as lib/libcxx/LICENSE.TXT, which also
covers libc++, libc++abi (lib/libcxxabi) and libunwind (lib/libunwind).
lib/libc/glibc/LICENSES and lib/libc/musl/COPYRIGHT cover the C library
headers and sources zig bundles (the game links with this system's glibc).
The other files keep their places in zig's folder.
EOF
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

# Every file the release holds (itself too): the launcher removes anything
# else from kit/ and LICENSES/, which an older version unpacked over this
# one left behind (tools/player-kit/launcher/tidy.hpp).
(cd "$pkg" && { find . -type f | sed 's|^\./||'; echo kit/files.txt; } | LC_ALL=C sort -u > kit/files.txt)

# Readable by everyone, writable by the owner; programs stay runnable.
chmod -R u+rwX,go+rX,go-w "$pkg"
rm -f "$out"
(cd "$stage" && tar --owner=0 --group=0 --numeric-owner -cf - "$PACKAGE" | gzip -9 > "$out")
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
