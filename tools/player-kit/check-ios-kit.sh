#!/bin/sh
# Checks a game's iOS kit as the Mac app's "Play on iPad" uses it (CI, on a
# Mac with Xcode):
#   tools/player-kit/check-ios-kit.sh <toycommander|toyracer> <ios-kit> [<simulator ios-kit>]
# The kit's layout and archive; then the game's app built as README-ios.txt
# says, with tools/player-kit/link_check.cpp (an empty function table) as
# the lifted code: compiled with build-flags.txt, archived, and linked by the
# kit's Xcode project for a device, unsigned. With a Simulator kit, the same
# for the Simulator, and the app installed on an iPad Simulator and started
# in its self test (QILIN_IOS_SELFTEST=1): with no disc it must reach the
# "no disc" message, then open its window and draw with MoltenVK.
set -eu
game=$1
kit=$(cd "$2" && pwd)
sim=
[ $# -ge 3 ] && sim=$(cd "$3" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
. "$here/games.sh"
EXE=${EXE%.exe}
fail() { echo "check-ios-kit: $*" >&2; exit 1; }
scratch=$(mktemp -d)
udid=
cleanup() {
    if [ -n "$udid" ]; then xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true; fi
    rm -rf "$scratch"
}
trap cleanup EXIT

# The layout.
layout() {  # <kit>
    for f in lib/lib${game}_ios_kit.a include/guest.hpp build-flags.txt README-ios.txt VERSION \
             xcode/QilinGame.xcodeproj/project.pbxproj xcode/QilinGame.xcodeproj/xcshareddata/xcschemes/QilinGame.xcscheme \
             xcode/Info.plist xcode/Game.xcconfig xcode/QilinGame.xcconfig xcode/app.c \
             xcode/Assets.xcassets/Contents.json xcode/Assets.xcassets/AppIcon.appiconset/Contents.json \
             xcode/Assets.xcassets/AppIcon.appiconset/AppIcon.png; do
        [ -f "$1/$f" ] || fail "missing: $1/$f"
    done
    plutil -lint "$1/xcode/Info.plist" >/dev/null
    if grep -q '^native' "$1/build-flags.txt"; then fail "build-flags.txt has a native line"; fi
    grep -q '^cxxflags = -target arm64-apple-ios16.0' "$1/build-flags.txt" || fail "build-flags.txt: no iOS 16 target"
    archs=$(lipo -archs "$1/lib/lib${game}_ios_kit.a")
    [ "$archs" = arm64 ] || fail "the kit's archive is $archs, not arm64 only"
    # MoltenVK is in it, and what volk is given from it.
    nm -g "$1/lib/lib${game}_ios_kit.a" 2>/dev/null | grep -q ' T _vkGetInstanceProcAddr$' || fail "no MoltenVK in the archive"
    nm -g "$1/lib/lib${game}_ios_kit.a" 2>/dev/null | grep -q ' T _main$' || fail "no main in the archive"
}
layout "$kit"
grep -qx 'sdk = iphoneos' "$kit/build-flags.txt" || fail "the device kit's sdk isn't iphoneos"

# The app, built as the launcher builds it. Sets $app.
build_app() {  # <kit> <sdk> <destination> <name>
    # (A folder with a space: the player's work folder is in Application Support.)
    k=$1 sdk=$2 destination=$3 work="$scratch/$4 build"
    mkdir -p "$work"
    set --
    for f in $(sed -n 's/^cxxflags = //p' "$k/build-flags.txt"); do set -- "$@" "$(printf '%s' "$f" | sed "s|\\\$kit|$k|g")"; done
    cp "$here/link_check.cpp" "$work/stand_in.cpp"
    echo "> xcrun --sdk $sdk clang++ $* -c stand_in.cpp"
    (cd "$work" && xcrun --sdk "$sdk" clang++ "$@" -c stand_in.cpp -o stand_in.o)
    xcrun --sdk "$sdk" libtool -static -o "$work/liblifted.a" "$work/stand_in.o"
    cp -R "$k/xcode" "$work/xcode"  # as the launcher does: the app's own copy is read-only
    xcodebuild -project "$work/xcode/QilinGame.xcodeproj" -scheme QilinGame -configuration Release \
        -destination "$destination" -derivedDataPath "$work/derived" \
        CODE_SIGNING_ALLOWED=NO DEVELOPMENT_TEAM= PRODUCT_BUNDLE_IDENTIFIER="io.github.qilin1066.$game-check" \
        PRODUCT_NAME="$EXE" QILIN_LIFTED_LIB="$work/liblifted.a" QILIN_KIT_LIB="$k/lib/lib${game}_ios_kit.a" QILIN_IOS_KIT="$k" build
    app="$work/derived/Build/Products/Release-$sdk/$EXE.app"
    [ -d "$app" ] || fail "no $app"
    [ -f "$app/$EXE" ] || fail "the app has no program $EXE"
    lipo -archs "$app/$EXE" | grep -qx arm64 || fail "the app's program isn't arm64"
    plist="$app/Info.plist"
    [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist")" = "$EXE" ] || fail "CFBundleExecutable"
    [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist")" = "io.github.qilin1066.$game-check" ] || fail "CFBundleIdentifier"
    [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleDisplayName' "$plist")" = "$TITLE" ] || fail "CFBundleDisplayName"
    [ "$(/usr/libexec/PlistBuddy -c 'Print :UIFileSharingEnabled' "$plist")" = true ] || fail "UIFileSharingEnabled"
    [ "$(/usr/libexec/PlistBuddy -c 'Print :MinimumOSVersion' "$plist")" = 16.0 ] || fail "MinimumOSVersion"
    /usr/libexec/PlistBuddy -c 'Print :CFBundleIcons' "$plist" >/dev/null 2>&1 || fail "no icon in the app"
    [ -f "$app/Assets.car" ] || fail "no Assets.car (the icon)"
    otool -L "$app/$EXE"
    # MoltenVK and SDL are inside: no library of theirs is loaded at run time.
    if otool -L "$app/$EXE" | grep -qi 'moltenvk\|libvulkan\|libSDL'; then fail "the app loads MoltenVK, Vulkan or SDL from outside"; fi
    echo "check-ios-kit: built $app ($(du -sh "$app" | cut -f1))"
}
build_app "$kit" iphoneos "generic/platform=iOS" device

[ -n "$sim" ] || { echo "check-ios-kit: $kit is good (no Simulator kit given)"; exit 0; }
layout "$sim"
grep -qx 'sdk = iphonesimulator' "$sim/build-flags.txt" || fail "the Simulator kit's sdk isn't iphonesimulator"
build_app "$sim" iphonesimulator "generic/platform=iOS Simulator" simulator

# An iPad Simulator: the newest iOS runtime's first available iPad.
udid=$(xcrun simctl list devices available -j | python3 -c '
import json, re, sys
devices = json.load(sys.stdin)["devices"]
def version(runtime):
    m = re.search(r"iOS-(\d+)-(\d+)", runtime)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)
for runtime in sorted((r for r in devices if "iOS" in r), key=version, reverse=True):
    for d in devices[runtime]:
        if d["name"].startswith("iPad"):
            print(d["udid"])
            sys.exit(0)
')
[ -n "$udid" ] || fail "no iPad Simulator here"
xcrun simctl list devices | grep "$udid"
xcrun simctl boot "$udid" 2>/dev/null || true
xcrun simctl bootstatus "$udid" -b
bundle="io.github.qilin1066.$game-check"
xcrun simctl install "$udid" "$app"
# The self test: started with no disc in its Documents. It logs to its data
# folder's log.txt (Documents/qilin/<game>), read here from the app's
# container (a console attached by simctl was found to end before the app);
# it ends by itself, or the check stops it after 3 minutes.
SIMCTL_CHILD_QILIN_IOS_SELFTEST=1 xcrun simctl launch --terminate-running-process "$udid" "$bundle"
container=$(xcrun simctl get_app_container "$udid" "$bundle" data)
log="$container/Documents/qilin/$game/log.txt"
echo "check-ios-kit: the self test's log is $log"
waited=0
until [ -f "$log" ] && grep -qE '^QILIN_IOS_SELFTEST: (passed|ended by)' "$log"; do
    [ "$waited" -ge 180 ] && break
    sleep 2
    waited=$((waited + 2))
done
sleep 2
xcrun simctl terminate "$udid" "$bundle" 2>/dev/null || true
[ -f "$log" ] || fail "the self test wrote no log"
cp "$log" "$scratch/run.txt"
cat "$scratch/run.txt"
if ! grep -q '^QILIN_IOS_SELFTEST: passed' "$scratch/run.txt"; then
    # A crash: the Simulator's report is the Mac's (DiagnosticReports).
    sleep 5
    for f in $(ls -t "$HOME"/Library/Logs/DiagnosticReports/"$EXE"*.ips "$HOME"/Library/Logs/DiagnosticReports/*/"$EXE"*.ips 2>/dev/null | head -1); do
        echo "check-ios-kit: crash report $f"
        python3 - "$f" <<'PY' || head -c 20000 "$f"
import json, sys
text = open(sys.argv[1]).read()
body = json.loads(text[text.index("\n") + 1:])
print("exception:", json.dumps(body.get("exception")), "termination:", json.dumps(body.get("termination")))
images = body.get("usedImages", [])
thread = body["threads"][body.get("faultingThread", 0)]
for frame in thread["frames"][:40]:
    image = images[frame["imageIndex"]].get("name", "?") if frame.get("imageIndex", -1) < len(images) else "?"
    print(f'  {image}: {frame.get("symbol", "?")} +{frame.get("symbolLocation", frame.get("imageOffset"))}')
PY
    done
    xcrun simctl spawn "$udid" log show --last 5m --style compact --predicate "process == \"$EXE\"" 2>/dev/null | tail -60 || true
fi
grep -q '^QILIN_IOS_SELFTEST: no disc: No disc image here yet' "$scratch/run.txt" || fail "the self test didn't reach the no-disc message"
grep -q '^QILIN_IOS_SELFTEST: drawing with ' "$scratch/run.txt" || fail "the self test didn't make a Vulkan device (MoltenVK)"
grep -q '^QILIN_IOS_SELFTEST: passed' "$scratch/run.txt" || fail "the self test didn't finish"
echo "check-ios-kit: $kit and $sim are good; the Simulator ran the self test"
