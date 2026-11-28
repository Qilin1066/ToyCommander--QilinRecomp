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

## Android

**On the device: not workable.** Since Android 10 an app can't run programs or map code from files it writes (`execve` and executable mappings of app-writable files are blocked). A compiler packaged as a native library in the app could still run *(unconfirmed on Android 15/16)*, and loading a generated `.so` after making it read-only works in practice, but Google Play forbids executing code it didn't deliver, and the trick is fragile.

**The route: the desktop launcher builds the Android app and installs it over USB or Wi-Fi.**

1. The launcher compiles the game for `aarch64-linux-android` with a bundled LLVM (Apache-2.0) and Android's C library headers and stubs built from AOSP (BSD/Apache). Not the NDK: its licence forbids redistribution *(arguable for its open-source parts)*, and zig has no Android C library.
2. The rest of the app never changes between players: the manifest, SDL3's Java side (`SDLActivity`, whose SDL version must match the native library's exactly) and the runtime. CI builds that as an APK template; the launcher adds the player's `libmain.so` (16 KB page aligned), signs the APK with a key made on the player's PC (Android doesn't check certificate authorities; v2/v3 signatures), and installs it with adb built from AOSP's source (Apache-2.0).
3. adb installs are exempt from Google's developer verification (from 2026-09 in some countries, worldwide in 2027), which will otherwise make sideloaded apps wait 24 hours.

Player's friction: once, enable developer options and USB or wireless debugging and accept the computer; the disc image is copied to the device with the app. Nothing expires.

**Decided and still to do:**
- **Vulkan 1.3 only, on Android too** (the owner, 2026-11-28: "No to vulkan 1.1"). That's about 26% of active Android devices (Google's dashboard, November 2025; 61% have only 1.1); Android 13+ devices *should* have 1.3, so the share grows as phones are replaced. "Install on Android" checks the device's Vulkan version over adb before building and says plainly when a phone can't run it.
- **The devices the owner wants most (2026-11-28): the NVIDIA Shield TV, Android tablets, perhaps Fire TV sticks.** The Shield TV has Vulkan 1.1 only, even on its last update (Android 11; NVIDIA's own page lists 1.1, and device reports show 1.1.178), so it can't run the game while Vulkan 1.3 is required; the owner chose to keep 1.3 knowing that. Fire TV: Amazon's new sticks run Vega OS (not Android) and can't sideload; the older Fire OS ones are low-power with Vulkan 1.1 at best. Tablets: those with recent chips (roughly Android 13 or later) have 1.3. Should 1.1 be wanted later, the renderer uses only two 1.3 features (dynamic rendering, synchronization2), in a few places: a contained change.
- **Android TV boxes with Vulkan 1.3** still matter: the app gets a TV launcher entry and banner (Leanback), and controller-first menus, so a TV box with 1.3 works as well as a tablet.
- The runtime needs an Android port of its own: touch controls over the picture (plus SDL's controller support), the app lifecycle (pause and resume), and saves and the disc in app storage.
- Not on Google Play (its code rules); sideloaded only.

## iPhone and iPad

**On the device: impossible.** App Review guideline 2.5.2 forbids apps executing code that changes their features, and iOS doesn't let apps run generated code.

**The route: a Mac.** The macOS launcher gets "Install on iPhone or iPad": it builds the game for iOS with Xcode's iOS SDK (`xcodebuild -allowProvisioningUpdates`) and installs it on the connected device (`xcrun devicectl device install app`).

Player's friction:
- **Once:** full Xcode (several GB; the Command Line Tools have no iOS SDK), an Apple ID added in Xcode's settings, Developer Mode on the device.
- **With a free Apple ID:** the app stops opening after 7 days until it's installed again (the launcher can do that in one click), at most 3 such apps on a device. A paid developer account ($99 a year) makes it a year.

Windows and Linux players: no sanctioned route. Compiling for iOS needs Apple's iOS SDK, which Xcode's licence ties to Apple hardware *(unconfirmed)*; AltStore and SideStore only re-sign an app someone has built. The EU's alternative marketplaces, web distribution and TestFlight all mean distributing the built game, which is the one thing this project can't do.

Runtime work: SDL3 and MoltenVK (static, App Store–compatible without its private-API option) on iOS, touch controls, the disc and saves in the app's Documents (reachable from the Files app).

## Order of work (recommended)

1. Linux and macOS downloads (released in Toy Commander 0.15.0, Toy Racer 0.8.0), then the Raspberry Pi measurements and a first run on a real Mac.
2. Android (Vulkan 1.3 only): the runtime port, then the launcher's "Install on Android".
3. iOS/iPadOS: after Android, reusing the touch controls; Mac-only.

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
