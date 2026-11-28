#!/bin/sh
# Checks a game's macOS download as a player's Mac would see it (CI's smoke
# test, on Apple silicon and on Intel):
#   tools/player-kit/check-macos.sh <toycommander|toyracer> <download.dmg>
# Mounts the disk image; checks the app's layout (the iOS kit's too), that
# its programs and libraries are universal, its signature and files.txt
# (which lists the iOS kit's files too); and builds a program
# as the launcher builds the game, with the kit's build-flags.txt and Apple's
# /usr/bin/clang++ (tools/player-kit/link_check.cpp, the lifted code's
# stand-in, linked with the kit's library), then runs it.
set -eu
game=$1
dmg=$2
here=$(cd "$(dirname "$0")" && pwd)
. "$here/games.sh"
EXE=${EXE%.exe}
fail() { echo "check-macos: $*" >&2; exit 1; }

mnt=$(mktemp -d)
scratch=$(mktemp -d)
hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$mnt" "$dmg" >/dev/null
trap 'hdiutil detach "$mnt" -force >/dev/null 2>&1 || true; rm -rf "$scratch"' EXIT
app="$mnt/$TITLE.app"
contents="$app/Contents"
kit="$contents/Resources/kit"
ios_kit="$contents/Resources/ios-kit"

# The layout.
for f in "$mnt/README.txt" "$contents/Info.plist" "$contents/MacOS/Play $TITLE" "$contents/Frameworks/libMoltenVK.dylib" \
         "$contents/Resources/AppIcon.icns" "$kit/qilin" "$kit/lib/lib${game}_kit.a" "$kit/include/guest.hpp" \
         "$kit/build-flags.txt" "$kit/game.txt" "$kit/files.txt" "$kit/VERSION" \
         "$contents/Resources/LICENSES/$PACKAGE/LICENSE.txt" "$contents/Resources/LICENSES/SDL3/LICENSE.txt" \
         "$contents/Resources/LICENSES/MoltenVK/LICENSE.txt" "$contents/Resources/LICENSES/MoltenVK/SPIRV-Cross-LICENSE.txt" \
         "$contents/Resources/LICENSES/MoltenVK/cereal-LICENSE.txt" "$contents/Resources/LICENSES/libchdr/zstd-LICENSE.txt" \
         "$ios_kit/lib/lib${game}_ios_kit.a" "$ios_kit/include/guest.hpp" "$ios_kit/build-flags.txt" "$ios_kit/README-ios.txt" \
         "$ios_kit/VERSION" "$ios_kit/xcode/QilinGame.xcodeproj/project.pbxproj" "$ios_kit/xcode/Info.plist" \
         "$ios_kit/xcode/Game.xcconfig" "$ios_kit/xcode/Assets.xcassets/AppIcon.appiconset/AppIcon.png"; do
    [ -f "$f" ] || fail "missing: ${f#"$mnt/"}"
done
# The iOS kit: arm64 devices (tools/player-kit/check-ios-kit.sh builds an app from it in CI).
[ "$(lipo -archs "$ios_kit/lib/lib${game}_ios_kit.a")" = arm64 ] || fail "the iOS kit's archive isn't arm64 only"
grep -qx 'sdk = iphoneos' "$ios_kit/build-flags.txt" || fail "the iOS kit isn't the device's"
cmp -s "$ios_kit/VERSION" "$kit/VERSION" || fail "the iOS kit is from another version"
[ -L "$mnt/Applications" ] && [ "$(readlink "$mnt/Applications")" = /Applications ] || fail "no link to /Applications"
[ -x "$contents/MacOS/Play $TITLE" ] && [ -x "$kit/qilin" ] || fail "the launcher or qilin isn't executable"
plutil -lint "$contents/Info.plist" >/dev/null
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$contents/Info.plist")" = "Play $TITLE" ] || fail "CFBundleExecutable"
grep -qx "game=$game" "$kit/game.txt" || fail "game.txt: game"
grep -qx "exe=$EXE" "$kit/game.txt" || fail "game.txt: exe"
grep -qx "compiler=/usr/bin/clang++" "$kit/game.txt" || fail "game.txt: compiler"
if grep -q '^compiler_args' "$kit/game.txt"; then fail "game.txt has compiler_args"; fi

# Universal: Apple silicon and Intel.
for f in "$contents/MacOS/Play $TITLE" "$kit/qilin" "$kit/lib/lib${game}_kit.a" "$contents/Frameworks/libMoltenVK.dylib"; do
    archs=$(lipo -archs "$f")
    echo "$(basename "$f"): $archs"
    case " $archs " in *" arm64 "*) ;; *) fail "$f has no arm64";; esac
    case " $archs " in *" x86_64 "*) ;; *) fail "$f has no x86_64";; esac
done
lipo -info "$kit/lib/lib${game}_kit.a"

# Signed (ad hoc), every file sealed as it was packaged.
codesign --verify --deep --strict --verbose=2 "$app"
codesign -dv "$app" 2>&1 | grep -q 'Signature=adhoc' || fail "the app isn't signed ad hoc"

# files.txt names every file the release holds, and nothing else, in the kits and the licences.
(cd "$app" && while IFS= read -r f; do [ -f "$f" ] || fail "files.txt names $f, which isn't there"; done < "$kit/files.txt")
(cd "$app" && find Contents/Resources/kit Contents/Resources/ios-kit Contents/Resources/LICENSES -type f | LC_ALL=C sort) > "$scratch/found.txt"
grep -E '^Contents/Resources/(kit|ios-kit|LICENSES)/' "$kit/files.txt" > "$scratch/listed.txt"
diff "$scratch/listed.txt" "$scratch/found.txt" || fail "files.txt and the app disagree"

"$kit/qilin" --version

# The game's build, as the launcher does it (build.cpp): from work/, the
# flags with $kit for the kit's folder, this Mac's own `<name>.<arch>`.
arch=x86_64
[ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = 1 ] && arch=arm64
flags() {
    for key in "$1" "$1.$arch"; do
        sed -n "s/^$key = //p" "$kit/build-flags.txt"
    done
}
# Arguments one by one, $kit made the kit's path after splitting (paths have spaces).
set --
for f in $(flags cxxflags) $(flags native); do set -- "$@" "$(printf '%s' "$f" | sed "s|\\\$kit|$kit|g")"; done
[ $# -gt 3 ] || fail "build-flags.txt has no cxxflags or native.$arch"
cp "$here/link_check.cpp" "$scratch/stand_in.cpp"
echo "> /usr/bin/clang++ $* -c stand_in.cpp"
(cd "$scratch" && /usr/bin/clang++ "$@" -c stand_in.cpp -o stand_in.o)
set --
for f in $(flags ldflags); do set -- "$@" "$f"; done
set -- "$@" -o "$EXE" stand_in.o
for f in $(flags libs); do set -- "$@" "$(printf '%s' "$f" | sed "s|\\\$kit|$kit|g")"; done
echo "> /usr/bin/clang++ $*"
(cd "$scratch" && /usr/bin/clang++ "$@")
file "$scratch/$EXE"
lipo -archs "$scratch/$EXE" | grep -qx "$arch" || fail "the game isn't built for $arch only"
otool -L "$scratch/$EXE"
# Started with no disc it stops at once with its usage (exit 2,
# shinobi/src/main.cpp), having loaded every library it links.
set +e
(cd "$scratch" && "./$EXE") > "$scratch/run.txt" 2>&1
code=$?
set -e
cat "$scratch/run.txt"
[ "$code" = 2 ] && grep -q '^usage: ' "$scratch/run.txt" || fail "the stand-in game ended with $code, not its usage"
echo "check-macos: $dmg is good for $arch"
