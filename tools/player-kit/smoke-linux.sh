#!/bin/sh
# Checks a built Linux download as a player would get it, without a disc
# and without starting the launcher's window or the game:
#   tools/player-kit/smoke-linux.sh <download.tar.gz> [oldest glibc, default 2.35]
#
# Unpacks it into an empty folder, then: the layout and kit/files.txt (both
# ways); the bundled zig runs, and with build-flags.txt as the launcher
# reads it ($kit, the native line) compiles, links and runs a small C++23
# program; the kit's library links into a game program with an empty
# function table (tools/player-kit/link_check.cpp, as the launcher links the
# lifted code; not run); the recompiler starts and reads a stand-in boot
# file; and the launcher, the recompiler and that program need nothing newer
# than the given glibc, and no C++ or SDL library from the system.
set -eu
tarball=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
floor=${2:-2.35}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
fail() { echo "smoke test: $*" >&2; exit 1; }

mkdir "$scratch/unpacked"
tar -xzf "$tarball" -C "$scratch/unpacked"
set -- "$scratch"/unpacked/*
[ $# -eq 1 ] && [ -d "$1" ] || fail "the download should hold one folder"
top=$1
kit="$top/kit"
title=$(sed -n 's/^title=//p' "$kit/game.txt")
exe=$(sed -n 's/^exe=//p' "$kit/game.txt")
[ -n "$title" ] && [ -n "$exe" ] || fail "kit/game.txt has no title or exe"
case $exe in *.exe) fail "game.txt's exe is $exe: no .exe on Linux" ;; esac
grep -qx 'compiler=toolchain/zig/zig' "$kit/game.txt" && grep -qx 'compiler_args=c++' "$kit/game.txt" || fail "game.txt doesn't name zig as the compiler"
for f in "Play $title" README.txt kit/qilin kit/build-flags.txt kit/files.txt kit/VERSION kit/icon.png kit/toolchain/zig/zig; do
    [ -f "$top/$f" ] || fail "no $f"
done
for f in "Play $title" kit/qilin kit/toolchain/zig/zig; do [ -x "$top/$f" ] || fail "$f isn't executable"; done
ls "$kit"/lib/lib*_kit.a > /dev/null || fail "no kit/lib/lib<game>_kit.a"
[ -n "$(ls "$kit/include")" ] || fail "kit/include is empty"
[ -n "$(ls "$top/LICENSES")" ] || fail "LICENSES is empty"
for f in zig/LICENSE zig/lib/libcxx/LICENSE.TXT SDL3/LICENSE.txt "Dear ImGui/LICENSE.txt" volk/LICENSE.md libchdr/LICENSE.txt libchdr/zstd-LICENSE.txt; do
    [ -s "$top/LICENSES/$f" ] || fail "no LICENSES/$f"
done
head -c 8 "$kit/icon.png" | od -An -tx1 | tr -d ' \n' | grep -q '^89504e470d0a1a0a$' || fail "kit/icon.png isn't a PNG"
# files.txt names every file, and only those.
(cd "$top" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$scratch/present.txt"
LC_ALL=C sort "$kit/files.txt" > "$scratch/listed.txt"
cmp -s "$scratch/present.txt" "$scratch/listed.txt" || { diff "$scratch/listed.txt" "$scratch/present.txt" | head -20; fail "kit/files.txt doesn't match the files"; }
echo "layout: ok ($(wc -l < "$kit/files.txt") files)"

# zig, with a cache of its own, as on a player's computer the first time.
export ZIG_GLOBAL_CACHE_DIR="$scratch/zig-cache" ZIG_LOCAL_CACHE_DIR="$scratch/zig-cache"
zig="$kit/toolchain/zig/zig"
"$zig" c++ --version | head -1

# build-flags.txt as the launcher reads it (build.cpp): `name = value`, $kit the kit.
flag() { sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$kit/build-flags.txt" | sed "s|\\\$kit|$kit|g"; }
cxxflags=$(flag cxxflags)
native=$(flag native)
ldflags=$(flag ldflags)
libs=$(flag libs)
[ -n "$cxxflags" ] && [ -n "$libs" ] || fail "build-flags.txt has no cxxflags or libs"
[ -n "$native" ] || fail "build-flags.txt has no native line"
echo "native: $native"

cat > "$scratch/hello.cpp" <<'EOF'
#include <atomic>
#include <cstdio>
#include <expected>
#include <string>
#include <thread>
#include <vector>
static std::expected<int, std::string> half(int n) {
    if (n % 2) return std::unexpected(std::string("odd"));
    return n / 2;
}
int main() {
    std::vector<std::thread> pool;
    std::atomic<int> sum{0};
    for (int i = 0; i < 4; ++i) pool.emplace_back([&sum, i] { sum += i; });
    for (auto& t : pool) t.join();
    const auto h = half(84);
    std::printf("hello %d %s\n", h ? *h + sum.load() : -1, half(3) ? "?" : half(3).error().c_str());
    return 0;
}
EOF
cd "$scratch"
# shellcheck disable=SC2086
"$zig" c++ $cxxflags $native -c hello.cpp -o hello.o
# shellcheck disable=SC2086
"$zig" c++ $ldflags -o hello hello.o $libs
[ "$(./hello)" = "hello 48 odd" ] || fail "the compiled program said: $(./hello)"
echo "compile, link and run: ok"

# The kit's library with an empty function table: the launcher's link.
# shellcheck disable=SC2086
"$zig" c++ $cxxflags $native -c "$here/link_check.cpp" -o link_check.o
# shellcheck disable=SC2086
"$zig" c++ $ldflags -o "$exe" link_check.o $libs
echo "kit link: ok ($(du -h "$exe" | cut -f1))"

# The recompiler starts and reads a boot file (a stand-in: not a supported build).
"$kit/qilin" --version
printf '\011\000' > standin.bin
set +e
"$kit/qilin" info standin.bin --json standin.json > qilin.log 2>&1
code=$?
set -e
[ "$code" -eq 3 ] && grep -q '"sha256"' standin.json || { cat qilin.log; fail "qilin info said $code"; }
# The launcher's identify contract: JSON on standard output, exit 3 (not the supported build).
set +e
"$kit/qilin" identify standin.bin --json > identify.json 2> identify.log
code=$?
set -e
[ "$code" -eq 3 ] && grep -q '"ok": false' identify.json || { cat identify.json identify.log; fail "qilin identify said $code"; }
echo "recompiler: ok"

# Nothing newer than the oldest glibc, and only the system's own libraries.
for f in "$top/Play $title" "$kit/qilin" "$scratch/$exe"; do
    newest=$(objdump -T "$f" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
    [ "$(printf '%s\n%s\n' "$newest" "$floor" | sort -V | tail -1)" = "$floor" ] || fail "$(basename "$f") needs glibc $newest (newer than $floor)"
    needed=$(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
    for n in $needed; do
        case $n in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*.so.*) ;;
        *) fail "$(basename "$f") needs $n from the system" ;;
        esac
    done
    echo "$(basename "$f"): glibc $newest at most; needs $needed"
done
echo "smoke test: passed"
