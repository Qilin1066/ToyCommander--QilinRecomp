#!/bin/sh
# trim-toolchain.sh <llvm-mingw for Windows, unpacked> <destination>
# Copies what compiling and linking x86_64 Windows programs needs from an
# llvm-mingw release (about 700 MB) to <destination> (about 300 MB): the
# clang driver and its libraries, lld, the C/C++ headers, the x86_64 runtime
# libraries, and the licences. Other targets, debuggers, tools and docs stay out.
set -eu
src=$1
dst=$2
mkdir -p "$dst/bin" "$dst/lib" "$dst/licenses"
for f in clang.exe clang++.exe x86_64-w64-mingw32-clang.exe x86_64-w64-mingw32-clang++.exe ld.lld.exe \
         libLLVM-*.dll libclang-cpp.dll libc++.dll libunwind.dll; do
    cp "$src"/bin/$f "$dst/bin/"
done
cp "$src"/bin/clang-[0-9]*.exe "$dst/bin/"
cp "$src"/bin/mingw32-common.cfg "$src"/bin/x86_64-*.cfg "$dst/bin/"   # the driver's defaults: libc++, compiler-rt, lld
cp -r "$src/include" "$dst/"
find "$dst/include" -name '*.idl' -delete      # interface sources: headers made from them are kept
mkdir -p "$dst/x86_64-w64-mingw32"
cp -r "$src/x86_64-w64-mingw32/lib" "$dst/x86_64-w64-mingw32/"
version=$(ls "$src/lib/clang")
mkdir -p "$dst/lib/clang/$version/lib/windows"
cp -r "$src/lib/clang/$version/include" "$dst/lib/clang/$version/"
cp "$src/lib/clang/$version/lib/windows/libclang_rt.builtins-x86_64.a" "$dst/lib/clang/$version/lib/windows/"
cp "$src/LICENSE.TXT" "$dst/licenses/LLVM-LICENSE.TXT"
cp "$src"/x86_64-w64-mingw32/share/mingw32/COPYING* "$dst/licenses/"
