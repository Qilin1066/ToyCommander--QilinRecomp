# CLAUDE.md

Static recompilation of **Toy Commander** (Dreamcast, USA: `MK-57020`, `V1.022`) to native code. Same requirements and scope as the sister projects Qilin1066/GBTGhoulies-QilinRecomp, PunchOutWii-QilinRecomp, BeatTheBeat-QilinRecomp and ApbArcade-QilinRecomp (the owner's instruction, 2026-11-21: "exact same as the other projects", down to the settings menu). When a design question here is undecided, do what they did; BeatTheBeat's `CLAUDE.md` has the longest list of lessons.

## Requirements (non-negotiable)

- **Efficiency first, then accuracy, and fail loudly.** Never paper over an unknown: stop with a clear error (exit codes, CMake `FATAL_ERROR`, later `rt::Stop`).
- **C++23** everywhere: recompiler, runtime and generated code.
- **Vulkan 1.3 only** for graphics. No OpenGL, D3D or Metal backends (macOS goes through MoltenVK).
- **Static recompilation only.** No interpreter, JIT or emulator at run time, not even as a fallback. That includes the sound driver's ARM code: the audio manager is replaced above it.
- Targets: 64-bit **Windows, Linux, macOS** on **x86-64 and ARM64**. The low-end reference is ARM boards like the **Raspberry Pi 4**. No 32-bit.
- Compiler minimums: GCC 14, Clang 19, Apple Clang (Xcode 16), MSVC 2022 17.10.
- No GPL code, tables or constants (no Dreamcast emulator's, not QEMU's): they may be run as black boxes, never read into this code. Hitachi's and Sega's documents are cited by title, not linked, quoted or included. Keep dependencies to the fewest possible; third-party libraries (SDL3, Vulkan headers, Dear ImGui for the menu, when they arrive) are permissive and handled as the sister projects handle them.
- Never commit game data or anything derived from it: disc images, `1GUTH.BIN`, extracted files, listings, `gen/`, `cfg.json`, symbol lists made from the binary. These are gitignored.

## Layout

- `recomp/src/`: `gdrom` (Redump cue/bin reader, bootstrap, ISO 9660), `bootfile` (loader, known-build SHA-256), `sha256`, `decoder` (SH-4, table-driven, X-macro `QILIN_SH4_OPS`), `cfg` (function discovery, constants, switches, coverage; `cfg_report.cpp` is its text and JSON), `main.cpp` (the `qilin` CLI). `gdrom`, `bootfile` and `sha256` are runtime code too (target `qilin_disc`): no `std::format` there.
- `recomp/tests/tests.cpp`: unit tests (`ctest`), no game files.
- `tools/check/verify_decoder.py`: the decoder against Capstone, the whole encoding space.
- `docs/`: design and results. Keep them current. `roadmap-and-design-targets.md` is the roadmap; `disc-analysis.md` the disc and boot file; `recompiler-core.md` the loader, disc reader and decoder, and what comes next; `prior-art.md`; `naming.md`.

## Build and check

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 tools/check/verify_decoder.py --qilin build/qilin     # pip install capstone; 0 unexplained differences
build/qilin disc path/to/game.cue
build/qilin extract path/to/game.cue --out extracted          # outside the repo, or in an ignored folder
build/qilin info extracted/files/1GUTH.BIN                    # exit 0: the supported build
build/qilin cfg extracted/files/1GUTH.BIN                     # 4,234 functions (0.2 s)
build/qilin coverage extracted/files/1GUTH.BIN                # exits 3 today: 99,450 bytes unknown (data, to be described); it should be 0
build/qilin suspects extracted/files/1GUTH.BIN
```

Before a milestone commit, all of these must hold: tests pass, warnings are clean (`QILIN_WERROR`), the decoder check has 0 unexplained differences, and (once they exist) generated code is warning-free under GCC, Clang and MSVC and the differential test has 0 unexplained mismatches.

## Known facts worth not rediscovering

- The boot file is `1GUTH.BIN` ("Guth" is the working title, Gutherman): SHA-256 `ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd`, 748,444 bytes, loaded at `0x8C010000`, ending at `0x8C0C6B9C`, entered at its first byte. No header, sections or symbols.
- The entry code sets the cache control register and jumps to `0x8C0B0340`. Layout from function discovery: code and literal pools `0x8C010000`–`0x8C09F5B6`, data to `0x8C0B0340`, then library code (much of it hand-written assembly) with data between, data again from `0x8C0C5690`.
- Function discovery's results and rules are in `docs/recompiler-core.md`. Things that cost time: literal pools contain dead entries nothing loads, and they decode (float constants look like FPU instructions), so a guessed function must not run on into known code; pointer candidates are tried in address order, so a bogus one just before a real function used to swallow it (fixed by rejecting a guess that runs into another candidate's start); 784 functions are unreferenced by any address in the image (dead code: don't lift them by default).
- `0x003B` is not in the decoder's set but the image has four, in `0x003B ; rts ; nop` stubs that real code calls (`0x8C099470`): debugger hooks, to be replaced natively. Stubs with `trapa #2` sit beside them.
- The call idiom assumption (calls preserve r8–r15) is only the compiler's convention: see the note in `cfg.hpp` before the lifter devirtualises anything.
- The dump is Redump-style: `.cue` plus 15 `.bin` tracks. Tracks 1–2 single density; track 3 (sector 45000) holds the bootstrap and directories; tracks 4–14 are the music (CD audio); track 15 (index 1 at 377422) holds all 81 files. ISO sector numbers are absolute disc sectors.
- Libraries in the boot file: Shinobi 1.61, Kamui 2 (16,3,0,0) with `kmdk`, the audio manager (`am*`/`ac*`, loads `AUDIO64.DRV`, ARM code), a TrueMotion 2X AVI player, No Cliché's engine with its script interpreter. No Windows CE.
- Movies: AVI, `TM2X` video, Duck DK4 ADPCM sound (format tag 0x61), 640×480; `60HZ_` and `50HZ_` versions.
- SH-4 points that shape the lifter: delay slots; PC-relative literal pools inside the code (`mov.l` rounds the PC down to a multiple of 4, `mov.w` doesn't); calls are usually `mov.l pool,rN` + `jsr @rN`; FPU instruction meaning depends on FPSCR.PR/SZ/FR, and the code does switch them (`fschg`, `frchg` both appear); `fmac`, `fsca`, `fsrra`, `fipr`, `ftrv` need a clean source for their exact results before they can be lifted (don't guess, don't take them from an emulator).
- Capstone 5's SH-4 mode also decodes SH-2A/SH-4A instructions and lacks `fipr`, `fcnvds`, `fcnvsd`: both are reviewed differences in `verify_decoder.py`. Unicorn has no SuperH: the differential test needs another reference (`docs/recompiler-core.md`).
- The decoder's table throws at start-up if a form leaves a bit undefined or two forms share a word: after editing `kRows`, run the tests; the pinned counts (59,001 valid words, 8,770 delayed branches) change only if the instruction set does.
- Building where there is no GCC 14 or Clang 19 (a session's Linux container, a Windows PC without LLVM): `pip install ziglang`, then use `python -m ziglang c++` as the compiler (a wrapper script for CMake's `CMAKE_CXX_COMPILER`). GCC 14 and MSVC are then checked only by CI: check CI after pushing. Compiler messages are annotations on the commit (`.github/matchers/compilers.json`).
- GCC's `-Wshadow` also flags a local reusing a file-scope name (Clang's and MSVC's don't), and MSVC's `[[maybe_unused]]` covers only a declaration's first declarator: both bit the sister projects.

## Status (v0.2.0)

- The disc reader, boot file loader, SH-4 decoder (Capstone cross-check: 0 unexplained differences) and function discovery exist, with tests. 4,234 functions, 21 switch tables, 86.7% of the image classified. Nothing is lifted and nothing runs yet.
- CI was green on v0.1.0 on all six jobs; check it after each push.

## Next steps, in order

1. Finish the analysis, into `docs/code-analysis.md`: describe the data segments so `qilin coverage` reaches 0; name the library boundaries (Shinobi, Kamui 2 and `kmdk`, audio manager, TrueMotion, C runtime, game), starting from the version strings' places, the hand-written assembly above `0x8C0B0340` and call-graph clusters; classify the 146 indirect jumps by library; `qilin hwaccess`.
2. The FPSCR mode pass, then the lifter in slices (integer; calls and the function table; FPU), then the differential test (choose the reference: `docs/recompiler-core.md`).
3. The runtime, as the sister projects brought theirs up: `qilin_run`, the replacement registry, stop on anything unregistered and name it.
4. The window, input, sound and the settings menu follow the sister projects exactly (`docs/naming.md`, the menu section of `docs/roadmap-and-design-targets.md`; Punch-Out's `punchout/src/menu.cpp` is the model).

## Conventions

- **Push without asking, on `main`, as in the sister projects** (the owner, 2026-11-21, asked whether this repo is worked on `main` with pushes without asking as the other four are: "Yes, same permissions. Keep working."). Push with `git push origin main`. Only what is 100% legal and above board to share with the world, checked before every push:
- **Audit what is being published:** every path added, binaries, anything named like game data or game-derived output, copied third-party text or code, personal details (local paths, names, e-mail addresses). No game data or anything derived from it. Never force-push published history.
- Releases are `vX.Y.Z`; the version lives only in `project(... VERSION ...)`. Every release gets a `CHANGELOG.md` entry (Keep a Changelog); never rewrite past entries.
- Prose in docs and commit messages: plain, specific, British spelling.
