# Adopting the launcher in another project

This folder, `tools/player-kit/launcher/`, is the players' launcher (`Play <Title>`) for Windows, Linux and macOS, with "Play on iPad" from the Mac. It names no game. Everything about the game is in the download's `kit/game.txt`, and what the game's data is, the project's recompiler says (`qilin identify`). So the folder is copied **unchanged** into each project. There is no shared repository: Toy Commander's copy (ToyCommander--QilinRecomp) is the master, and the other projects copy it again when it moves on.

## Keeping copies in step

- **Copy the whole folder verbatim**, `tools/player-kit/launcher/` and everything in it, to the same path in the project. Don't edit a copy. A fix belongs in the master, and is then copied out again.
- **`LAUNCHER_VERSION`** holds one number. It goes up by one with every change to this folder (code, tests, CMake or this file). A project knows how far behind it is by comparing its copy's number with the master's. The number is in every `build.log` ("launcher N") and is defined as `QILIN_LAUNCHER_VERSION` for the code.
- A change that needs something new from projects (a new `game.txt` key with no default, a change to the identify contract) must say so here, under "Changes that need the projects".

## The identify contract

Every project's recompiler implements it. The launcher runs `game.txt`'s `identify` command (by default `{qilin} identify {input} --json --build {game:build}`) in `work/`. The tool prints **one JSON object** on standard output, and nothing else:

```json
{"ok": true, "title": "Toy Commander (USA)", "id": "MK-57020", "version": "1.022", "message": "This is Toy Commander (USA), MK-57020 version 1.022: the right disc."}
```

| Exit | `ok` | Meaning | `message` |
|---|---|---|---|
| 0 | `true` | the supported build | what it is, plainly ("This is …: the right disc.") |
| 3 | `false` | readable, but not the supported build (another game, another region or revision, an unknown version) | names what it is and what's needed ("This is Toy Racer (Europe). This download is for Toy Commander (USA), MK-57020 version 1.022.") |
| 2 | `false` | can't be read at all (missing, damaged, not the kind of data) | why, plainly |

- `title`, `id` and `version` describe **what the input is** (as far as it is known, else `""`), not what is wanted. `id` is the product number, disc ID, title ID or ROM set's name; `version` the revision.
- The message is a sentence for a player: no paths into the kit, no stack traces. It is shown as it is, so end it with a full stop.
- Anything else is a broken tool, and the launcher says so: no JSON, `ok` missing, or `ok` and the exit code disagreeing (`true` with 3, `false` with 0), or another exit code. Diagnostics may go to standard error (kept in `work/logs/identify-errors.txt`; the launcher quotes a line from its end for a broken tool).
- It must be quick (it runs at every start and every time a disc or folder is chosen) and must never write anything.
- Toy Commander's is `recomp/src/identify.cpp`, called from `recomp/src/main.cpp`: the disc's bootstrap names the boot file, whose SHA-256 names the build; `--build <title>` says which known build this download wants (both games share one `qilin`). Its tests are `test_identify` in `recomp/tests/tests.cpp`. Elsewhere identify may check a DOL's SHA-256 with the disc ID and revision, an XBE's title ID and timestamp, or each ROM's SHA-1.

## `kit/game.txt`

`name=value` lines (spaces around `=` are fine; `#` starts a comment; a later line wins). UTF-8, LF or CRLF. Every key absent takes the default, and the defaults are exactly the first downloads' (Toy Commander's): so an old `game.txt` keeps working.

### The game

| Key | Default | What |
|---|---|---|
| `game` | (required) | short name: the per-user data folder (`qilin/<game>`), the release tags. A plain name. |
| `title` | (required) | "Toy Commander": the window, `Play <title>`, the shortcut. |
| `exe` | (required) | the game's program in `work/` (`ToyCommander.exe`; no `.exe` on Linux and macOS). |
| `build` | (required) | the supported build's name, "Toy Commander (USA)". |
| `product`, `disc_version` | empty | its ID and version, shown as "build, product version disc_version" when asking for the data. |
| `boot` | empty | what the recompiler reads, for the progress line ("Recompiling 1GUTH.BIN into C++"); empty: "Recompiling into C++". |
| `package` | empty | the download's folder inside the zip and tarball (`ToyCommander--QilinRecomp`). |
| `release` | — | not read by the launcher; the README's words. |

### The data

| Key | Default | What |
|---|---|---|
| `input` | `file` | `file` (a disc image) or `folder` (an extracted Xbox disc with `default.xbe`, a Wii `sys/` + `files/` folder, a MAME ROM folder, a PS3 `PS3_GAME` folder with its decrypted `EBOOT.elf` and the fonts beside it). A folder is chosen with the system's folder dialog or dropped onto the window. |
| `input_extensions` | `cue;gdi;chd` | a file's extensions (`;`, `,` or spaces between them, the dot optional, any case): the file dialog's filter, and what the launcher accepts. Present but empty: any file. Ignored for a folder. |
| `input_noun` | `disc` (a file), `game folder` (a folder) | the player's word for it, everywhere in the window: "Choose your ROM set", "Building the game from your ROM set". |
| `input_hint` | for the default extensions, "Choose your disc's .cue file (with its .bin tracks beside it), its .gdi or its .chd."; else "Choose your <noun>'s .iso or .gcm file." or "Choose the folder that holds your <noun>." | one plain sentence for the chooser, shown under the request and after a wrong choice: "Choose the folder you extracted the disc into, the one holding default.xbe." |
| `identify` | `{qilin} identify {input} --json --build {game:build}` | the identify contract's command (above). |

The chosen file or folder is remembered in the data folder (`launcher-disc.txt`), whatever it is.

### Recompiling, building, starting

| Key | Default | What |
|---|---|---|
| `step.1` … `step.9` | `step.1 = {qilin} lift {input} --out {gen} --files 64` | the recompiler's commands, run in order in `work/` before compiling; each must exit 0. Numbered from 1 with no gaps (a gap, or `step.10`, is an error at start-up). |
| `compile_memory_mb` | `1200` | what one compiler needs at its peak: no more compilers run at once than free memory allows (3000 or 8000 elsewhere). |
| `build_space_mb` | `2048` | free space a build needs on the drive holding `work/`. |
| `cache_mb` | `2048` | the object cache (`build-cache` in the data folder) is trimmed to its newest this much after a build. |
| `run_args` | `{input}` | the game's arguments (`--data {input} --fullscreen`; empty: none). The game starts in the download's folder, its output in `work/run.log` and `work/run-errors.log`. |
| `old_files` | (none) | what this project's downloads before the launcher left in the download folder, `;`-separated, relative to it (`Play Ghoulies.bat; work/kit.ninja`). Removed at every start. Only plain relative paths inside the folder count; `..` and absolute paths are ignored. |
| `old_input_file` | (none) | where an older download remembered the player's data (`work/disc-folder.txt`): its first line is taken over if the launcher remembers nothing yet, then the file is removed. |
| `compiler` | `toolchain/bin/x86_64-w64-mingw32-clang++.exe` | the compiler: a path in the kit, or absolute (`/usr/bin/clang++` on macOS). |
| `compiler_args` | empty | put before every compiler argument (zig's `c++`). |

**What is compiled:** every `.cpp` under `work/gen`, subfolders too. Each object goes to the same relative path under `work/obj` (`gen/dsp/mixer.cpp` → `obj/dsp/mixer.o`), so names can't collide. Every `.h` and `.hpp` under `gen` is part of every object's cache key. The objects are linked in sorted order, then `build-flags.txt`'s `libs`.

**Commands** (`identify`, `step.N`, and `run_args` after the program) are split into arguments by the launcher itself, never by a shell. Spaces and tabs separate arguments. `"…"` or `'…'` keeps spaces (and the other kind of quote), and quoted and plain text side by side make one argument (`--out="{gen}/sub dir"`). There are no escapes, so Windows backslashes stay as they are. Then the placeholders are replaced within each argument, so a path with spaces or Unicode stays one argument:

| Placeholder | Becomes |
|---|---|
| `{qilin}` | the kit's `qilin` (`qilin.exe` on Windows) |
| `{tool:NAME}` | the kit's `NAME` plus the system's executable suffix (`{tool:qilin-dsp}`). Quote it if NAME has a space: `"{tool:my tool}"`. |
| `{kit}` | the kit folder |
| `{input}` | the player's file or folder |
| `{gen}` | `work/gen` |
| `{work}` | `work/` |
| `{game:KEY}` | game.txt's own `KEY` (`{game:build}`) |
| `{{`, `}}` | a brace |

The program is the first argument. A relative program is taken from the kit folder. An unknown placeholder, an unclosed quote or brace, a bad number or a bad `input` stops the launcher at start-up with a plain message, not halfway through a build. The kit's programs that the steps name (`{tool:NAME}`, or a program in the kit) are part of the build's identity, so a new one means a new build.

Examples:

```
# GameCube: the DOL from the disc, lifted with a plan, then the DSP's code.
input = file
input_extensions = iso;gcm;ciso
input_hint = Choose your disc's .iso, .gcm or .ciso file.
step.1 = {tool:qilin-disc} dol {input} main.dol
step.2 = {qilin} lift main.dol --plan {kit}/plan.tsv --files 32 --out {gen}
step.3 = {tool:qilin-dsp} lift {input} --out {gen}/dsp
compile_memory_mb = 3000

# An arcade game: a ROM folder, two CPUs.
input = folder
input_noun = ROM set
input_hint = Choose the folder holding your ROM set's files.
step.1 = {qilin} lift {input} --out {gen}
step.2 = {qilin} lift {input} --cpu sound --out {gen}/sound
run_args = --roms {input}
```

### Updates

| Key | Default | What |
|---|---|---|
| `repo` | empty (no updates: the Update button says so) | `owner/name` on GitHub, where releases are published. |
| `release_tag_prefix` | `<game>-v` | the game's release tags before X.Y.Z (`toycommander-v0.16.0`). |
| `release_asset_prefix` | `<package>-` | the downloads' names before `X.Y.Z-<system>`. |

The `<system>` part is fixed by the launcher (`platform_*.cpp`'s `release_asset_suffix`): `windows-x64.zip`, `linux-x86_64.tar.gz`, `linux-arm64.tar.gz`, `macos.dmg`. Drafts and pre-releases are skipped. The zip and tarball hold one folder, `package`, with `Play <title>` and `kit/game.txt` inside, whose `game` must match. The dmg holds `<title>.app`.

## What a project provides, step by step

1. **The launcher folder**, copied verbatim (above). In the project's `CMakeLists.txt`, after `project()` and before anything uses the targets:

   ```cmake
   set(QILIN_LAUNCHER_QILIN qilin)   # the recompiler's target, whose identify the build test runs
   include(tools/player-kit/launcher/launcher.cmake)
   ```

   and, once SDL3, Dear ImGui and volk exist (the desktop builds):

   ```cmake
   qilin_launcher_program(IMGUI_DIR ${imgui_SOURCE_DIR} LIBRARIES <imgui target> <volk target> SDL3::SDL3-static Threads::Threads)
   ```

   `launcher.cmake` makes `qilin_launcher_core`, `qilin_launcher_test` (ctest: `qilin_launcher_{json,version,kit,ios}_test`), `qilin_launcher_stub` and `qilin_launcher_build_test` (not when cross-compiling), and the option `QILIN_WINDOWS_RESOURCES` (Windows: `launcher.rc`, the icon; off where there is no resource compiler). It links a `qilin_warnings` target and calls `qilin_tool(<target>)` where the project has them. The launcher needs C++23, SDL3 (built statically), Dear ImGui (its SDL_Renderer backend is compiled here), volk and the Vulkan headers. It needs nothing else from the project: its SHA-256 is its own (`hash.hpp`), and so is the test stub.
2. **`qilin identify`**, the contract above, with unit tests: the supported build, another build (exit 3), an unknown version (exit 3) and an unreadable input (exit 2). `launcher_build_test` also runs the real one on a two-byte file with a Unicode name and expects `"ok": false` with exit 2 or 3. Keep it so.
3. **The player kit** (`-DQILIN_PLAYER_KIT=ON`, Toy Commander's `qilin_player_kit()` in `CMakeLists.txt`, `macos.cmake`, `ios.cmake`): one archive with everything but the lifted code (`kit/lib/lib<game>_kit.a`), the runtime's headers (`kit/include/`), the recompiler and any other tools the steps name (`kit/qilin`, `kit/<tool>`), and `kit/build-flags.txt`: `name = value` lines, `$kit` for the kit folder, read by `build.cpp`:
   - `cxxflags`: how the lifted code is compiled (`-O2 -DNDEBUG -std=c++23 -w … -I$kit/include`); the launcher adds `-Igen`, and this computer's processor's flag.
   - `native` (or `native.<arch>`): that flag (`-march=native` by default; `-mcpu=native` on ARM).
   - `ldflags`, `libs`: the link. Extra objects the game must link whole (a `force_link.o`, the Windows icon's `icon.o`) go in `libs` as `$kit/lib/<file>`.
   - A universal kit (macOS) adds `<name>.arm64` and `<name>.x86_64` after the shared ones.
   - A link check that links the archive with an empty function table proves it complete in CI.
4. **`game.txt`**, written by the packaging scripts (Toy Commander's: `games.sh`'s `write_game_txt`, which writes every key, defaults too, so the file tells the whole story).
5. **The three packaging scripts**, adapted from Toy Commander's (`package.sh` Windows, `package-linux.sh`, `package-macos.sh`, with `games.sh`; and `package-ios-kit.sh` for the iPad). They lay out the download as `kit.hpp` describes. Each writes `kit/VERSION` (the release's X.Y.Z, or `git describe`'s text), `kit/game.txt` and, last of all, `kit/files.txt`: every file the release holds, itself too, relative to the download (macOS: to the app). At every start the launcher removes from `kit/` and `LICENSES/` whatever that list doesn't name (`tidy.hpp`), so a file missing from it is deleted on the player's computer. The launcher is installed as `Play <title>` (`.exe` on Windows; `<title>.app/Contents/MacOS/Play <title>` on macOS). Windows bundles llvm-mingw (`trim-toolchain.sh`), Linux bundles zig (`compiler=toolchain/zig/zig`, `compiler_args=c++`), and macOS uses Apple's clang (`compiler=/usr/bin/clang++`).
6. **The three workflows** (`.github/workflows/player-kit.yml`, `player-kit-linux.yml`, `player-kit-macos.yml`): build each download, check it (`smoke-linux.sh`, `check-macos.sh`, `check-ios-kit.sh`), run the launcher's tests with the bundled compiler on Linux, and attach the files to the release of a `<game>-vX.Y.Z` tag, named `<package>-X.Y.Z-<system>`. CI's main workflow runs `ctest` on Windows (with `-DQILIN_REQUIRE_LAUNCHER_TESTS=ON`), Linux and macOS, which includes the launcher's tests.
7. **The README templates** (`README.txt.in`, `README-linux.txt.in`, `README-macos.txt.in`, `ios/README-ios.txt.in`), filled by `games.sh`'s `fill`: the data the player needs and how to dump it, what's in the download, the controls, the source link.
8. **The licences**, in `LICENSES/<component>/` (macOS: `Contents/Resources/LICENSES`): the project's own; SDL3, Dear ImGui, Vulkan-Headers and volk (the launcher's and the runtime's); the bundled compiler's (llvm-mingw's, or zig's with what it bundles); and whatever the project's runtime adds (Toy Commander: libchdr and what it bundles).
9. **macOS: MoltenVK.** The app carries MoltenVK's own release `libMoltenVK.dylib` in `Contents/Frameworks` (pinned by SHA-256 in the workflow, its licence and the licences of what it contains in `LICENSES/MoltenVK`). The launcher starts the game with `DYLD_FALLBACK_LIBRARY_PATH` and `SDL_VULKAN_LIBRARY` pointing there (`apple_tools.cpp`). The app is signed ad hoc, the dylib and `kit/qilin` first, then the app (`package-macos.sh`). Apple's clang is checked before a build (`xcode-select -p`, Apple clang 16 or newer).
10. **iPad ("Play on iPad", optional).** The Mac app carries `Contents/Resources/ios-kit/` (`docs/platforms.md`, "The iOS kit: the contract with the Mac app"): `lib/lib<game>_ios_kit.a` (the runtime built for iOS, statically, with SDL3 and MoltenVK's static iOS library), `include/`, `build-flags.txt` (`cxxflags`; optional `kit_lib`, `xcode_project`, `xcode_scheme`, `xcode_configuration`, `lifted_lib_setting`, `kit_lib_setting`), `xcode/` (Toy Commander's `tools/player-kit/ios/` project, which takes `QILIN_LIFTED_LIB`, `QILIN_KIT_LIB` and `QILIN_IOS_KIT`), `README-ios.txt` and `VERSION`. Without it, the button isn't shown. The runtime port must:
    - start through SDL3's `main` and run the game on a stack of its own (iOS gives the main thread 1 MB);
    - link MoltenVK statically (volk with `VOLK_NAMESPACE`, `volkInitializeCustom`);
    - keep its data folder in `Documents/qilin/<game>`;
    - find its data in `Documents`, where the launcher copies it: a file input as the file (a `.cue` or `.gdi` with the track files it names beside it), a folder input as `Documents/<folder name>/…`, every file in it at its relative path. It must skip names starting with a dot, and say plainly what to do when nothing, or more than one, is there.
    - stop GPU work in the background;
    - keep the 4 GB reservation possible (or ask for the extended virtual addressing entitlement);
    - pass a self test in the Simulator (`QILIN_IOS_SELFTEST=1`, `check-ios-kit.sh`).

## Things to watch

- `files.txt` must name every file of the release, or the launcher deletes the unnamed ones from `kit/` at the next start. A list that doesn't name `kit/game.txt`, `kit/files.txt` and `kit/qilin` is taken as wrong, and then nothing is removed.
- identify's message is shown to players word for word. Write it as the player would want it: what they chose, what's needed.
- Steps run every build, and a step that rewrites unchanged files with new timestamps is fine (the cache keys on content), but one that writes different text every time (a date, a random name) recompiles everything every time.
- `run_args` is for the game's real arguments. On iOS there is no command line: the iOS runtime finds its data in Documents.
- The default identify passes `--build {game:build}`. A project whose `qilin` has only one build can set `identify = {qilin} identify {input} --json`.
- Placeholders are replaced after splitting: `{input}` with spaces is safe unquoted, but a literal argument with spaces needs quotes.

## Changes that need the projects

- Launcher 3: nothing for projects to do; copy the folder again. The iPad tests' sample device is "Player's iPad" (it named a person before).

- Launcher 2: two new, optional `game.txt` keys, `old_files` and `old_input_file`, for the files a project's downloads before the launcher left behind (its old `.bat`, its own remembered-data file). Without them nothing changes; with them, unzipping the new download over an old one leaves no clutter and keeps the player's choice.

- Launcher 1 (the first game-neutral copy): projects need `qilin identify` (the defaults run it), `repo` in `game.txt` (it used to default to Toy Commander's repository), and `launcher.cmake` in place of their own launcher targets. The test stub moved from `tools/check/launcher_stub.cpp` to `test_stub.cpp` here.
