# Where players can play: desktop, Raspberry Pi, Android, iPhone and iPad

The game's code may never be distributed (it is made from the disc), so on every system the player's own disc is recompiled and the result compiled for their device. The question per system is where that compile happens and how the result gets onto the device. Researched 2026-11-28; sources at the end. Claims not confirmed in a primary source are marked *(unconfirmed)*.

## Desktop: Windows, Linux, macOS (the launcher)

One launcher program (`tools/player-kit/launcher`), the same logic everywhere; the system's parts are `platform_*.cpp`, `process_*.cpp`, `shortcut_*.cpp`. Each download carries or finds a compiler:

| System | Download | Compiler | Notes |
|---|---|---|---|
| Windows 10/11 x64 | zip, `Play <Title>.exe` | LLVM-MinGW, bundled | unsigned: SmartScreen's "More info, Run anyway" once |
| Linux x86-64 and ARM64 | `.tar.gz`, `Play <Title>` | zig (MIT), bundled: its own libc++, the system's glibc (2.35 or later) | menu entry from the launcher; needs a Vulkan 1.3 driver |
| macOS 14.5+ (Xcode 16's tools need it; the runtime itself is built for 10.15), Apple silicon and Intel | `.dmg`, `<Title>.app` (universal) | Apple's clang from the Command Line Tools (free; Apple's SDK may not be redistributed): the launcher offers Apple's installer | MoltenVK (Apache-2.0) inside the app; unsigned: "Open Anyway" in Privacy & Security once (notarisation needs a paid Apple developer account) |

### The macOS download

`tools/player-kit/package-macos.sh` (CI: `.github/workflows/player-kit-macos.yml`) builds one universal disk image, `<Package>-X.Y.Z-macos.dmg`: `<Title>.app`, a link to `/Applications` and `README.txt` (from `README-macos.txt.in`).

- **The app.** `Contents/MacOS/Play <Title>` (the launcher), `Contents/Frameworks/libMoltenVK.dylib` (MoltenVK's own release, pinned by SHA-256), `Contents/Resources/AppIcon.icns` (`launcher/make_icns.py`, the `.ico`'s artwork), `Contents/Resources/LICENSES` and `Contents/Resources/kit`: `qilin`, `lib/lib<game>_kit.a` (both universal; the archive merged by Apple's `libtool`, `tools/player-kit/macos.cmake`), `include/`, `build-flags.txt`, `game.txt` (`compiler=/usr/bin/clang++`), `files.txt` (paths relative to the app) and `VERSION`. The build's `work/` is in `~/Library/Application Support/qilin/<game>`, so the app is never written to and its seal stays whole.
- **Flags.** `build-flags.txt` names each processor's own flags beside the shared ones (`native.arm64 = -arch arm64 -mcpu=native`, `native.x86_64 = -arch x86_64 -march=native`, `ldflags.<arch>`): the launcher takes the Mac's own architecture (`host_architecture()`, Apple silicon even under Rosetta), so the game is built for that Mac only. `$kit` becomes the kit's whole path there (work/ isn't beside it).
- **Apple's compiler.** Before a build, `launcher/apple_tools.cpp` runs `xcode-select -p` and `clang++ --version`: missing tools get "Install Apple's command line tools" (`xcode-select --install`, Apple's own dialog) and "Check again"; an unagreed licence says so; Apple clang older than 16 says to update macOS's software.
- **MoltenVK.** volk opens `libvulkan.dylib`, `libvulkan.1.dylib`, then `libMoltenVK.dylib` by name, and SDL3, given no library, takes any `vkGetInstanceProcAddr` already in the process, which in the game is volk's variable. So the launcher starts the game with `DYLD_FALLBACK_LIBRARY_PATH` set to the app's `Frameworks` alone (volk finds the app's MoltenVK, and not a Vulkan SDK's loader) and `SDL_VULKAN_LIBRARY` set to the same file. The launcher's own window needs neither: SDL looks in `@executable_path/../Frameworks` first, and falls back to its software renderer.
- **Gatekeeper.** Signed ad hoc after assembly (`codesign -s -`: the dylib and `qilin`, then the app); nothing removes quarantine. The README and the release notes give the one-time "Open Anyway" steps.
- **CI's checks.** `tools/player-kit/check-macos.sh` mounts the image on an Apple silicon and an Intel runner: the layout, `lipo` (universal), `codesign --verify --deep --strict`, `files.txt` against the app, then a stand-in game compiled and linked with the kit's flags by `/usr/bin/clang++` and started (it must load and print its usage).

### Linux

`tools/player-kit/package-linux.sh` builds it, `.github/workflows/player-kit-linux.yml` on every push and a game's tag, each processor on its own runner (`ubuntu-22.04`, `ubuntu-22.04-arm`): `<Package>-X.Y.Z-linux-x86_64.tar.gz` and `-linux-arm64.tar.gz`, the folder `<Package>/` with `Play <Title>`, `README.txt` (`README-linux.txt.in`), `LICENSES/` and `kit/` (as `kit.hpp` lists, with `icon.png` for the menu entry and zig in `toolchain/zig`).

- **zig** 0.17.1, pinned by its SHA-256 in the workflow, the whole release less other systems' C libraries and headers (Windows, macOS, the BSDs, WebAssembly; about 130 MB of 300). `game.txt` says `compiler=toolchain/zig/zig`, `compiler_args=c++`.
- **The kit is built with the same zig** (`zig cc`/`zig c++` as CMake's compilers, through wrapper scripts the package script writes), so the archive's libc++ is the one the player's link uses. The target is the native one (zig finds the system's X11, Wayland and sound headers for SDL3 and links against the glibc it finds), with `-mcpu=baseline` so nothing is compiled for the runner's own processor; the player's compiles add `build-flags.txt`'s `native` line (`-march=native` on x86-64, `-mcpu=native` on ARM64).
- **glibc 2.35 is the floor**: the runner's (Ubuntu 22.04). The download runs on Ubuntu 22.04, Debian 12, Fedora 36, Raspberry Pi OS Bookworm and later. An older glibc target (`-target <arch>-linux-gnu.2.31`) would drop the native include paths SDL3 needs, so it wasn't tried. The smoke test fails any program in the download that needs a newer glibc symbol.
- SDL3 builds its Wayland support with zig once its own protocol headers come before the system's (`CMakeLists.txt`: zig searches `/usr/include` before SDL's system folder).
- **Checks in CI**: the launcher's tests built with the bundled zig, and `tools/player-kit/smoke-linux.sh` on the unpacked tarball: the layout and `files.txt`, the bundled zig compiling, linking and running a C++23 program with `build-flags.txt`'s flags, the kit's library linked into a game program with an empty function table (not run), the recompiler reading a stand-in boot file, and each program's glibc symbols and libraries. The launcher's window, the menu entry and the update can't be run there.
- zig keeps its compiled libc++ and C runtime in `~/.cache/zig`: the first link on a player's computer builds them for its processor, which adds to the first build's time (most on a Pi).

## Raspberry Pi

The Linux ARM64 download, built on the Pi itself.

- **Graphics.** The game needs Vulkan 1.3. Mesa's v3dv driver reached 1.3 on both the Pi 4 and the Pi 5 in Mesa 24.3. The current Raspberry Pi OS (Trixie, October 2025) ships Mesa 25.0, so it is fine. Bookworm's Mesa is older (Vulkan 1.2 *(unconfirmed)*): the launcher checks the driver's version before building (`launcher/vulkan_check.cpp`: a minimal instance through volk) and, if it's too old, says to update Raspberry Pi OS (a Pi is known by its device tree's model). Where there is a graphics processor, Mesa's software device (llvmpipe, which has 1.3 and may be installed beside an older v3dv) doesn't count; with no graphics processor at all (a virtual machine), it does.
- **64-bit only.** The 64-bit OS has been the default download since October 2023; 32-bit images are about a sixth of 64-bit downloads. On a 32-bit system the launcher (a 64-bit program) can't start, so the download's README and the release notes say "64-bit Raspberry Pi OS" first (the README with "`getconf LONG_BIT` says 64"). There is no start script to say it at start-up: a shell script is opened in a text editor or behind a "run or display?" question when double-clicked in GNOME's Files and Raspberry Pi OS's file manager, where the program itself just runs; and the launcher's menu entry, update and folder lock find the download through its own path, which a wrapper would move to another name. On a 32-bit OS with a 64-bit kernel (the 32-bit image boots one on a Pi 4 or 5 *(unconfirmed)*) the system's message is "No such file or directory" (no 64-bit loader), which the README names too.
- **Memory.** Pis have 1 to 16 GB. The launcher runs no more compilers at once than free memory allows (about 1.2 GB each): a 2 GB Pi compiles one file at a time, an 8 GB Pi four. A 1 GB Pi is below what the build needs.
- **Time.** The first build is CPU-bound: tens of minutes on a Pi 4 *(to be measured)*, much less on a Pi 5. Later builds take compiled files from the cache. The progress bar says it may take a while on a Raspberry Pi.
- **Space.** The 2 GB check applies; an SD card is slower but the compile is CPU-bound.
- **Desktop.** Raspberry Pi OS runs Wayland (labwc). Whether a file chooser exists there (a GTK portal or zenity) is unconfirmed, so the launcher accepts a dragged file and, where there's no dialog, a typed or pasted path. "Add to the applications menu" puts it in the Pi's menu.

Next steps: on a real Pi, measure a first build on a Pi 4 (4 GB) and a Pi 5 (with zig's own first libc++ build), the peak memory of the largest generated file and the start-up's hashing of the kit (the 170 MB zig program is hashed into the build's identity at every start), then tune the per-compiler allowance; and confirm the Vulkan check's messages, the file chooser and the menu entry on labwc.

## Android: not pursued

The owner, 2026-11-28: "Forget android, those users can sort installation out themselves". No Android build or installer is made here. What was found, for whoever takes it up: an app can't compile and run code on the device (Android 10's execute restrictions); the workable route is a computer building the APK (a fixed template plus the player's native library, signed with a local key) and installing it over adb, which is exempt from Google's coming developer verification. Vulkan 1.3 only (the owner's decision) leaves out the NVIDIA Shield TV (Vulkan 1.1) and Fire TV sticks (Vega OS, or Vulkan 1.1 at best); recent tablets have 1.3. The renderer uses only two 1.3 features (dynamic rendering, synchronization2), in a few places, should a 1.1 path ever be wanted.

## iPad (and iPhone)

The owner, 2026-11-28: the iPad is wanted.

**On the device: impossible.** App Review guideline 2.5.2 forbids apps executing code that changes their features, and iOS doesn't let apps run generated code.

**The route: a Mac, from the Mac download's own app.** Its launcher gets "Play on iPad":

1. **Checks.** Full Xcode (the Command Line Tools have no iOS SDK: `xcrun --sdk iphoneos --show-sdk-path`), an Apple ID signed in to Xcode (its development team, from the signing certificates or Xcode's settings), and an iPad connected and trusted, with Developer Mode on (`xcrun devicectl list devices`). Each missing piece gets a plain message and what to do (App Store link for Xcode; Xcode, Settings, Accounts; the iPad's Settings, Privacy & Security, Developer Mode).
2. **Build.** The disc's recompiled code is the same C++ as for the Mac (`work/gen`): the launcher compiles it for iPadOS (`xcrun --sdk iphoneos clang++ -target arm64-apple-ios…`) with its usual pool and a cache of its own, into one static library. The iOS kit (in the app's `Contents/Resources/ios-kit`) holds the runtime built for iPadOS (static, with SDL3 and MoltenVK), `build-flags.txt`, and a small Xcode project that links the two. `xcodebuild -allowProvisioningUpdates` builds and signs it with the player's team and a bundle identifier made once per player.
3. **Install.** `xcrun devicectl device install app`, then the disc image copied into the app's Documents (`devicectl device copy to`): a `.chd` is one file, which is easiest; a `.cue` goes with its tracks.
4. **Seven days.** With a free Apple ID the app stops opening after 7 days. The launcher shows when, and "Reinstall on iPad" signs and installs again without rebuilding. A paid developer account ($99 a year) makes it a year. Free accounts: at most 3 such apps on a device (both games count).

**The game on iPadOS** (the runtime port): SDL3 for iOS (UIKit) and MoltenVK linked in statically (App Store–compatible build, no private API; Apple GPUs from the A12/M1 on have what Vulkan 1.3 through MoltenVK needs *(to confirm)*); the disc found in Documents (or chosen from the Files app); saves and settings in Documents, so the Files app and backups see them; the game paused when the app goes to the background and resumed after; controllers through SDL (Xbox, PlayStation and MFi pads all work on iPadOS): controller first. Touch controls on screen come second.

**What can't be checked without hardware:** CI's Macs build the iOS kit, compile and link the game for the device (unsigned) and run it in the iPad Simulator; signing, installing and playing on a real iPad need the owner's Mac and iPad.

Windows and Linux players: no sanctioned route. Compiling for iOS needs Apple's iOS SDK, which comes only with Xcode on a Mac; AltStore and SideStore only re-sign an app someone has built. The EU's alternative marketplaces, web distribution and TestFlight all mean distributing the built game, which is the one thing this project can't do.

## Order of work

1. Linux and macOS downloads (released in Toy Commander 0.15.0, Toy Racer 0.8.0), then the Raspberry Pi measurements and a first run on a real Mac.
2. iPad: the runtime port and the iOS kit (CI: device build and Simulator), then the Mac launcher's "Play on iPad"; then a first install on the owner's iPad; then touch controls.

## Sources

- Android 10 behaviour changes (execute restrictions): https://developer.android.com/about/versions/10/behavior-changes-10
- Android 14 behaviour changes (read-only dynamic code): https://developer.android.com/about/versions/14/behavior-changes-14
- Google Play policy on executable code: https://support.google.com/googleplay/android-developer/answer/9888379
- Android developer verification, and its FAQ (adb exempt): https://developer.android.com/developer-verification, https://developer.android.com/developer-verification/guides/faq
- Android SDK licence: https://developer.android.com/studio/terms
- Bionic: https://android.googlesource.com/platform/bionic/ ; zig and Bionic: https://github.com/ziglang/zig/issues/23906
- APK signing: https://source.android.com/docs/security/features/apksigning
- SDL on Android: https://github.com/libsdl-org/SDL/blob/main/docs/README-android.md
- Android distribution dashboard (Vulkan versions): https://developer.android.com/about/dashboards ; Vulkan on Android: https://source.android.com/devices/graphics/implement-vulkan
- App Review Guidelines (2.5.2): https://developer.apple.com/app-store/review/guidelines/
- Apple membership comparison (free provisioning limits): https://developer.apple.com/support/compare-memberships
- devicectl: https://developer.apple.com/documentation/xcode/interacting-with-devices-using-the-command-line
- AltStore, SideStore: https://faq.altstore.io/altstore-pal-v.-altstore-classic.md, https://docs.sidestore.io/docs/installation/prerequisites
- EU distribution: https://developer.apple.com/support/dma-and-apps-in-the-eu/
- MoltenVK: https://github.com/KhronosGroup/MoltenVK
- Mesa 24.3 (v3dv Vulkan 1.3): https://docs.mesa3d.org/relnotes/24.3.0.html
- Raspberry Pi OS Trixie: https://www.raspberrypi.com/news/trixie-the-new-version-of-raspberry-pi-os/
- Raspberry Pi Imager statistics: https://rpi-imager-stats.raspberrypi.com
