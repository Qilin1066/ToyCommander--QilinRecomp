# ToyCommander--QilinRecomp

A static recompilation of **Toy Commander** (No Cliché / Sega, Dreamcast, 1999) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); Macs are covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor.

It has the same requirements and scope as its sister projects, [GBTGhoulies--QilinRecomp](https://github.com/Qilin1066/GBTGhoulies--QilinRecomp), [PunchOutWii--QilinRecomp](https://github.com/Qilin1066/PunchOutWii--QilinRecomp), [RhythmHeavenFever--QilinRecomp](https://github.com/Qilin1066/RhythmHeavenFever--QilinRecomp) and [ApbArcade--QilinRecomp](https://github.com/Qilin1066/ApbArcade--QilinRecomp).

> **No game data is included, and none ever will be.** You need your own copy of the game. The tools read your disc dump from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

**It doesn't run the game yet.** The recompiler core is being built (roadmap steps 1 and 2).

| Piece | State |
|---|---|
| GD-ROM reader (Redump-style `.cue` + `.bin` dumps: tracks, bootstrap, files) | ✅ reads the supported disc; `qilin disc`, `qilin extract` |
| Boot file loader (`1GUTH.BIN`, recognised by SHA-256) | ✅ |
| SH-4 decoder (all 209 instruction forms, table-driven) | ✅ agrees with Capstone on every word of the 16-bit encoding space, the reviewed differences listed |
| Function discovery and control flow (recursive descent, constants through literal pools, switch tables, tail calls, function pointers, unreferenced code) | ✅ 4,307 functions, 14 switch tables |
| Layout: the game, the middleware, Shinobi and the data regions | ✅ every boundary with its evidence (`docs/code-analysis.md`). The game's code is fully accounted for: 0 unknown bytes. 0.27% of the whole image is still unknown, all in libraries not yet attributed |
| Hardware-access profile | ✅ 76 functions reach hardware at constant addresses, none in the game itself (`qilin hwaccess`) |
| FPSCR modes (precision, transfer size, bank, rounding, denormals) at every FPU instruction | ✅ all 35,959 FPU instructions in the game and the middleware proven (`qilin fpmode`) |
| Lifter, SH-4 → C++ (`qilin lift`) | ✅ every function of the game (1,259) and all but 21 hardware-facing ones of the middleware: 2,916 lifted, tested end to end under every CI compiler. The FPU's approximate instructions are measured against a Dreamcast emulator (`tools/dcref`) |
| Middleware attribution (engine, Kamui 2, audio manager, C runtime) | during bring-up: the lifted middleware stops on hardware, which names what to replace |
| Differential test of every lifted instruction | ✅ against a reference interpreter of our own, in CI: 242,496 runs, 0 mismatches. Whole call trees to come |
| Runtime: Sega's libraries replaced natively, Vulkan 1.3, SDL3 | to do |
| Enhancements (resolution, widescreen, high refresh rate) and the settings menu shared with the sister projects | to do, after the game plays at original spec |

The supported release is the USA disc (`MK-57020`, `V1.022`); see `docs/disc-analysis.md`.

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). Past entries are kept, never rewritten. The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. 1.0.0 will be the first build that plays the game.

## Building

The whole project is **C++23**. You need CMake ≥ 3.25 and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older. There are no other dependencies yet.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI builds and tests with warnings as errors on Linux x64 (GCC 14 and Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64, and Windows x64 (MSVC).

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

The dump is a Redump-style one: a `.cue` beside one `.bin` per track.

The decoder check needs no game files:

```sh
pip install capstone
python3 tools/check/verify_decoder.py --qilin build/qilin
```

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `gdrom` (disc reader), `bootfile` (loader), `decoder` (SH-4), `cfg` (function discovery and control flow), `layout` (the supported build's regions), `hwaccess` (the hardware-access profile), `fpmode` (the FPSCR mode pass), `lifter` and `lift_output` (SH-4 to C++), `main` (the `qilin` CLI) |
| `recomp/runtime/` | What lifted code needs at run time: the guest context, memory access, the helpers, calls through the function table |
| `recomp/tests/` | Unit tests and end-to-end lifter tests on synthetic programs, so they need no game files |
| `tools/check/` | The decoder cross-check against Capstone |
| `tools/dcref/` | Measures the FPU's approximate instructions against a Dreamcast emulator run as a black box (Linux developer tool; the emulator is fetched, never committed) |
| `docs/` | Design notes: the roadmap and targets, the disc, the code analysis, the recompiler core, prior art, naming |

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0), as in the other QilinRecomp projects: you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. (Versions before 2026-11-21 were released under MIT, and those copies stay under MIT.)

The game itself is Sega's and is not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, SH-4 → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only**, on a deliberately small baseline feature set so one render path runs on everything from low-end ARM GPUs upwards.
- **Sega's hardware-facing libraries are replaced natively** (Kamui 2, the Shinobi system layer, the audio manager); the game, its engine and the pure-software libraries are lifted for exact behaviour.
- **Ground truth is the original game.**

See `docs/` for the full roadmap and the analysis notes.
