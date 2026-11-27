# ToyCommander--QilinRecomp

Static recompilations of **Toy Commander** (No Cliché / Sega, Dreamcast, 1999) and **Toy Racer** (No Cliché / Sega, Dreamcast, 2000) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs run it through MoltenVK.

The two games share an engine and most of this repository's code; each has its own program (`toycommander`, `toyracer`), releases and changelog.

> **No game data is included, and none ever will be.** You need your own copy: Toy Commander's USA disc (`MK-57020`, `V1.022`) or Toy Racer's European disc (`MK-5114950`, `V1.050`, its only release), as a Redump-style dump (a `.cue` beside one `.bin` per track). The tools read it from a path you give them.

## Status

**Toy Commander** plays from the movies through the menus into its missions, with sound, saves, the settings menu, four pads and the Jump Pack. It has not been played to the end yet.

**Toy Racer** plays through its menus into a race, with sound. Its state is in [`docs/toyracer/`](docs/toyracer/).

| Toy Commander | State |
|---|---|
| GD-ROM reader | ✅ reads the supported disc; `qilin disc`, `qilin extract` |
| Boot file loader (`1GUTH.BIN`, by SHA-256) | ✅ |
| SH-4 decoder (209 forms) | ✅ agrees with Capstone on every 16-bit word, reviewed differences listed |
| Function discovery and control flow | ✅ 4,338 functions, 17 jump tables |
| Layout and coverage | ✅ 0 unknown bytes in every region (`docs/code-analysis.md`) |
| Hardware-access profile | ✅ 77 functions reach hardware at constant addresses, none in the game |
| FPSCR modes | ✅ every FPU instruction of the game and the middleware proven |
| Lifter, SH-4 → C++ | ✅ 4,088 functions lifted, 250 replaced natively, none left over |
| Differential test | ✅ 248,016 runs, 0 mismatches; whole call trees to do |
| Runtime: natives, Vulkan 1.3, SDL3 | 🔨 plays into the missions (`docs/runtime.md`) |
| Enhancements | 🔨 internal resolution, anisotropic filtering, MSAA, widescreen to 21:9, texture packs; high refresh rate to do |

Latest reliability checks: [the validation notes](docs/validation-0.12-0.5.md).

## Playing

**On Windows, no tools needed:**

1. Go to [**Releases**](https://github.com/Qilin1066/ToyCommander--QilinRecomp/releases) and download the newest `ToyCommander--QilinRecomp-…-windows-x64.zip` (or `ToyRacer--QilinRecomp-…`). Ignore the "Source code" files.
2. Unzip it anywhere (right-click, **Extract All**).
3. Double-click **`Play Toy Commander.exe`** (or **`Play Toy Racer.exe`**). If Windows says "Windows protected your PC", click **More info**, then **Run anyway**.
4. The first time, click **Choose your disc** and pick your disc's `.cue`, `.gdi` or `.chd` file (or drag it onto the window). It builds the game from your disc once, which takes a few minutes; after that **Play** starts it straight away. **Check for updates** fetches a newer release.

You need 64-bit Windows 10 or 11, a graphics driver with Vulkan 1.3 (any recent NVIDIA, AMD or Intel one), and your own disc dumped as a `.cue` with its `.bin` tracks, a `.gdi` or a `.chd`. The zip's `README.txt` has the controls; **F1** or **Esc** opens the settings menu. Nothing from the game is in the download, and your game files never leave your PC.

### From the source

Build the project, then the game's program from your boot file ([Using it](#using-it)). F1 or Esc (Back + Start on a controller) opens the settings menu.

## Building

You need CMake 3.25 or later and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

SDL3, the Vulkan headers, volk and Dear ImGui are fetched and pinned at configure time. On Linux, SDL3 needs the X11, Wayland and sound development packages listed in `.github/workflows/ci.yml`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI builds with warnings as errors and runs the tests and checks on Linux x64 (GCC 14, Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64, and Windows x64 (MSVC); `.github/workflows/player-kit.yml` builds the Windows downloads.

## Using it

```sh
build/qilin disc    path/to/game.cue                 # the disc's bootstrap, tracks and files
build/qilin extract path/to/game.cue --out extracted # its files and IP.BIN, to a folder (never commit them)
build/qilin info    extracted/files/1GUTH.BIN        # SHA-256, supported build, load address (JSON); exit 3 if unsupported
build/qilin insns   extracted/files/1GUTH.BIN -o listing.txt   # every word decoded (a linear sweep)
build/qilin profile extracted/files/1GUTH.BIN        # instruction counts
build/qilin cfg      extracted/files/1GUTH.BIN --json cfg.json  # function discovery and control flow
build/qilin coverage extracted/files/1GUTH.BIN [--list|--map]   # every byte, per region: code, literal, jump table, padding, data or unknown (exit 3 while a lifted region has unknown bytes)
build/qilin suspects extracted/files/1GUTH.BIN        # functions with problems, for review
build/qilin hwaccess extracted/files/1GUTH.BIN        # accesses to hardware at constant addresses, per function
build/qilin fpmode   extracted/files/1GUTH.BIN [--list] # the FPU's mode at every FPU instruction
build/qilin lift     extracted/files/1GUTH.BIN --out gen  # the lifted C++ (gen/ is ignored: never commit it)
```

Toy Racer's boot file is `1TOYRACE.BIN`. To build a game's program and run it:

```sh
cmake -S . -B build -DQILIN_TOYCOMMANDER_BOOT=extracted/files/1GUTH.BIN     # and/or -DQILIN_TOYRACER_BOOT=.../1TOYRACE.BIN
cmake --build build --target toycommander                                    # or toyracer
build/toycommander path/to/game.cue
```

The lifted C++ goes into the build tree only; each program checks that the disc's boot file is the one it was built from. `QILIN_HEADLESS=1` runs without a window (other variables: `docs/runtime.md`).

### Tests

`ctest` needs no game files: unit tests, end-to-end lifter tests on synthetic programs, and the differential test (every lifted instruction from random states against our own reference interpreter).

### Decoder check

The decoder against Capstone on all 65,536 words, reviewed differences apart. No game files needed:

```sh
pip install capstone
python3 tools/check/verify_decoder.py --qilin build/qilin
```

### Native check

Every replaced function lifted code can reach must have a native:

```sh
build/qilin lift extracted/files/1GUTH.BIN --out gen
python3 tools/check/check_natives.py gen toycommander/src    # Toy Racer: toyracer/src
```

After changing a native, run `python3 tools/check/native_footprints.py`; CI fails while its table is out of date.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `gdrom` (disc reader), `bootfile` (loader), `decoder` (SH-4), `cfg` (function discovery and control flow), `layout` (the supported builds' regions), `hwaccess`, `fpmode` (the FPSCR mode pass), `lifter` and `lift_output` (SH-4 to C++), `main` (the `qilin` CLI) |
| `recomp/runtime/` | What lifted code needs at run time: the guest context, memory, calls through the function table, the registry of natives, the stop report |
| `recomp/tests/` | Unit tests, end-to-end lifter tests and the differential test (`difftest/`) |
| `dreamcast/` | The hardware both games share: the PowerVR2 and tile accelerator, the renderer, sound, the Maple bus, flash memory, the settings and the menu |
| `shinobi/` | Sega's system library, replaced natively for both games at each game's addresses: start-up, allocator, interrupt chains, clock, GD-ROM calls, threads |
| `host/` | The window, Vulkan 1.3 device, display, sound output and data folder |
| `toycommander/src/` | Toy Commander's own: its addresses (`build.hpp`), natives and game code |
| `toyracer/src/` | Toy Racer's own, the same |
| `tools/check/` | The decoder cross-check, the native checks, the Windows launcher check |
| `tools/player-kit/` | Each game's Windows download |
| `tools/dcref/` | Measures the FPU's approximate instructions and the PowerVR2's fog against a Dreamcast emulator run as a black box (Linux developer tool; the emulator is fetched, never committed) |
| `docs/` | Design notes: the roadmap, the disc, the code analysis, the recompiler core, the runtime. Toy Racer's are in `docs/toyracer/`, with [the notes from its earlier repository](docs/toyracer/notes-from-its-repo.md) |

## Versions and changes

Toy Commander's releases are tagged `toycommander-vX.Y.Z` and recorded in [`CHANGELOG.md`](CHANGELOG.md); Toy Racer's are tagged `toyracer-vX.Y.Z` and recorded in [`toyracer/CHANGELOG.md`](toyracer/CHANGELOG.md). Each version lives in one place in `CMakeLists.txt`: `QILIN_TOYCOMMANDER_VERSION` (the project's version, which `qilin --version` prints) and `QILIN_TOYRACER_VERSION`. A minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be each game's first build that plays it through.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0): you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. Versions before 2026-11-21 were released under MIT, and those copies stay under MIT. SDL3, the Vulkan headers, volk and Dear ImGui keep their own licences, which the Windows downloads carry in `LICENSES/`.

The games themselves are Sega's and are not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, SH-4 → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only**, on a small baseline feature set, so one render path runs everywhere.
- **Sega's hardware-facing code is replaced natively** (the Shinobi system layer, the sound driver, the boot ROM's GD-ROM calls); the game, its engine and the software libraries, Kamui 2 included, are lifted for exact behaviour.
- **Ground truth is the original game.**

See [`docs/`](docs/) for the roadmap and the analysis notes.
