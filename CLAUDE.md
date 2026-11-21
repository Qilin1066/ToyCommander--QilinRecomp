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

- `recomp/src/`: `gdrom` (Redump cue/bin reader, bootstrap, ISO 9660), `bootfile` (loader, known-build SHA-256), `sha256`, `decoder` (SH-4, table-driven, X-macro `QILIN_SH4_OPS`), `cfg` (function discovery, constants, switches, tail calls, coverage; `cfg_report.cpp` is its text and JSON), `layout` (the supported build's regions, with plans), `hwaccess` (hardware accesses at constant addresses), `fpmode` (FPSCR mode per FPU instruction, interprocedural, per entry-mode context), `lifter` (one function to C++) and `lift_output` (`qilin lift`'s files), `main.cpp` (the `qilin` CLI).
- `recomp/runtime/`: `guest.hpp` (context, memory, helpers; header-only runtime code) and `dispatch.cpp` (calls through the function table). `gdrom`, `bootfile` and `sha256` are runtime code too (target `qilin_disc`): no `std::format` there.
- `recomp/tests/tests.cpp`: unit tests (`ctest`), no game files. `lift_programs.hpp` (a small assembler and synthetic programs), `lift_gen.cpp` (lifts them at build time), `lift_run.cpp` (runs the lifted code against reference results): the end-to-end lifter tests, which make CI compile generated code with every compiler.
- `tools/check/verify_decoder.py`: the decoder against Capstone, the whole encoding space.
- `tools/dcref/`: `qilin_dcref`, the FPU's approximate instructions against Flycast's libretro core run as a black box (`fetch.sh` downloads it outside the repo; `-DQILIN_DCREF=ON -DQILIN_DCREF_DIR=<folder>`; Linux x86-64, Mesa EGL surfaceless).
- `docs/`: design and results. Keep them current. `roadmap-and-design-targets.md` is the roadmap; `disc-analysis.md` the disc and boot file; `code-analysis.md` the layout, the libraries and the lifted/replaced boundary; `recompiler-core.md` the loader, disc reader and decoder, and what comes next; `prior-art.md`; `naming.md`.

## Build and check

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 tools/check/verify_decoder.py --qilin build/qilin     # pip install capstone; 0 unexplained differences
build/qilin disc path/to/game.cue
build/qilin extract path/to/game.cue --out extracted          # outside the repo, or in an ignored folder
build/qilin info extracted/files/1GUTH.BIN                    # exit 0: the supported build
build/qilin cfg extracted/files/1GUTH.BIN                     # 4,307 functions (0.3 s, two passes)
build/qilin coverage extracted/files/1GUTH.BIN                # exit 0: 0 unknown bytes in lifted regions (the game); 2,020 elsewhere
build/qilin suspects extracted/files/1GUTH.BIN
build/qilin hwaccess extracted/files/1GUTH.BIN                # 76 functions, none in the game
build/qilin fpmode extracted/files/1GUTH.BIN --list           # 0 unproven FPU instructions in the game and middleware; 4 middleware problems (known, docs/code-analysis.md)
build/qilin lift extracted/files/1GUTH.BIN --out gen          # 2,916 lifted (all 1,259 of the game's), 21 not (privileged, to replace); gen/ is ignored, never commit it
build-dcref/qilin_dcref <folder>/flycast_libretro.so          # the approximate FPU instructions against the emulator (optional tool)
```

Before a milestone commit, all of these must hold: tests pass, warnings are clean (`QILIN_WERROR`), the decoder check has 0 unexplained differences, and (once they exist) generated code is warning-free under GCC, Clang and MSVC and the differential test has 0 unexplained mismatches.

## Known facts worth not rediscovering

- The boot file is `1GUTH.BIN` ("Guth" is the working title, Gutherman): SHA-256 `ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd`, 748,444 bytes, loaded at `0x8C010000`, ending at `0x8C0C6B9C`, entered at its first byte. No header, sections or symbols.
- The entry code sets the cache control register and jumps to `0x8C0B0340` (Shinobi's start-up), which calls the game's `main` at `0x8C02796A`. The layout is in `recomp/src/layout.cpp`, its evidence in `docs/code-analysis.md`: game `0x8C010080`–`0x8C0495B8` (lifted, 0 unknown bytes: keep it so); middleware `0x8C0495B8`–`0x8C09F5B6` (engine, audio manager, Kamui 2, `kmdk`, C runtime, interleaved object by object: attribution by range does not work, and spreading labels along call edges was tried and is too noisy); data `0x8C09F5B6`–`0x8C0B0340`; Shinobi `0x8C0B0340`–`0x8C0C5690` with three data blocks inside; Shinobi's data to the end.
- The game never touches hardware (`qilin hwaccess`) and calls Shinobi directly from only 3 sites: the runtime's work is replacing Shinobi and the middleware's hardware-facing parts.
- Function discovery's results and rules are in `docs/recompiler-core.md`. Things that cost time: literal pools contain dead entries nothing loads, and they decode (float constants look like FPU instructions), so a guessed function must not run on into known code; pointer candidates are tried in address order, so a bogus one just before a real function used to swallow it (fixed by rejecting a guess that runs into another candidate's start); SHC tail-calls with `bra` (and `bt`/`bf`), which absorbed the callee into the caller until v0.3.0 (now: a branch to a prologue start or a known entry is a tail call, and the analysis iterates to a fixed point); float constants are read through `mova` + `fmov @r0`, not PC-relative loads, so they need the constant-address literal rule; the layout's data regions must still be scanned for function pointers (the pointer tables are there); 784 functions are unreferenced by any address in the image (dead code: don't lift them by default).
- `0x003B` is not in the decoder's set but the image has four, in `0x003B ; rts ; nop` stubs that real code calls (`0x8C099470`): debugger hooks, to be replaced natively. Stubs with `trapa #2` sit beside them.
- The lifter: a delay slot's branch reads its condition, target and PR before the slot runs; every call checks `c.ret` against the address after its slot; `trapa` never reaches it (discovery rejects functions with one). Every label target must be an emitted instruction, not a slot. Tests that break `div1`'s Q update or a generated switch case must fail: keep the end-to-end tests able to catch that.
- The lifter's FPU: the current bank in `float` locals, the back bank in `c.xf`; DRn = FRn (high) : FRn+1; a 64-bit fmov puts FRn at the lower address. Denormals are flushed by the host's mode (`rt::host_fp_setup()`: FTZ+DAZ, ARM64 FZ), which every thread running lifted code must call; generated code needs `-ffp-contract=off`. NaN bit patterns from arithmetic are the host's, and FPSCR's cause/flag bits read as 0: both for the differential test to confirm. `fpmode` counts only calls from analysed code as calls: a function called only from replaced code starts in the default mode (missing this skipped `main`'s subtree until v0.6.0).
- `0x8C04EEE0` is the C runtime's `memset` through the store queues (`docs/code-analysis.md`): a native replacement is needed early in bring-up. `mov.l Rn,@-Rn` stores the value before the decrement (Flycast agrees).
- The call idiom assumption (calls preserve r8–r15) is only the compiler's convention: see the note in `cfg.hpp` before the lifter devirtualises anything. r0–r7 are no longer assumed clobbered by every call: a call clears only the scratch registers its callee can write (computed per callee, transitively).
- Hand-written library code calls with `lds rN,pr ; jmp @rM` and returns with `jmp @r2` through a saved PR: the CFG tracks PR for this (`call_returns`, `jmp_returns`). The lifter must handle both.
- The FPU: the default mode is `0x00040000` (round to nearest, **DN = 1: denormals flushed to zero**, single, 32-bit transfers, bank 0). The game never leaves it. FR is handled by swapping two bank arrays on `frchg` (never needs to be known); the engine really runs in bank 1 after some matrix calls (`0x8C06166C` returns in the other bank). Open: the dispatcher `0x8C09F2AC` calls table handlers with SZ = 1 (see `docs/code-analysis.md`). How the pass got to its design (and the dead ends: per-function defaults, symbolic entry summaries) is in `docs/recompiler-core.md`; don't go back to them.
- The middleware's attribution: decided in v0.4.0 to drive it from bring-up (lift the middleware, stop on hardware, replace upward). Shared global variables group functions (audio core = one group; Kamui 2 spans the whole middleware); constant-section order confirms link order; padding means nothing.
- The dump is Redump-style: `.cue` plus 15 `.bin` tracks. Tracks 1–2 single density; track 3 (sector 45000) holds the bootstrap and directories; tracks 4–14 are the music (CD audio); track 15 (index 1 at 377422) holds all 81 files. ISO sector numbers are absolute disc sectors.
- Libraries in the boot file: Shinobi 1.61, Kamui 2 (16,3,0,0) with `kmdk`, the audio manager (`am*`/`ac*`, loads `AUDIO64.DRV`, ARM code), a TrueMotion 2X AVI player, No Cliché's engine with its script interpreter. No Windows CE.
- Movies: AVI, `TM2X` video, Duck DK4 ADPCM sound (format tag 0x61), 640×480; `60HZ_` and `50HZ_` versions.
- SH-4 points that shape the lifter: delay slots; PC-relative literal pools inside the code (`mov.l` rounds the PC down to a multiple of 4, `mov.w` doesn't); calls are usually `mov.l pool,rN` + `jsr @rN`; FPU instruction meaning depends on FPSCR.PR/SZ/FR, and the code does switch them (`fschg`, `frchg` both appear); `fmac`, `fsca`, `fsrra`, `fipr`, `ftrv` have no documented exact results: with no console at hand, the owner asked (2026-11-21: "i dont have a dreamcast to hand. why dont you install a dreamcast emulator for your testing") for an emulator to test against. Their definitions are our own (`guest.hpp`), chosen with `qilin_dcref` against Flycast: all agree on every input without a denormal except `fsca` (Flycast's own table, within 2⁻²³; never copy it). Flycast keeps denormal operands where the manual's DN = 1 zeroes them: the port follows the manual.
- Capstone 5's SH-4 mode also decodes SH-2A/SH-4A instructions and lacks `fipr`, `fcnvds`, `fcnvsd`: both are reviewed differences in `verify_decoder.py`. Unicorn has no SuperH: the differential test uses a reference of our own, as the sister projects do.
- The decoder's table throws at start-up if a form leaves a bit undefined or two forms share a word: after editing `kRows`, run the tests; the pinned counts (59,001 valid words, 8,770 delayed branches) change only if the instruction set does.
- Building where there is no GCC 14 or Clang 19 (a session's Linux container, a Windows PC without LLVM): `pip install ziglang`, then use `python -m ziglang c++` as the compiler (a wrapper script for CMake's `CMAKE_CXX_COMPILER`). GCC 14 and MSVC are then checked only by CI: check CI after pushing. Compiler messages are annotations on the commit (`.github/matchers/compilers.json`).
- GCC's `-Wshadow` also flags a local reusing a file-scope name (Clang's and MSVC's don't), and MSVC's `[[maybe_unused]]` covers only a declaration's first declarator: both bit the sister projects.

## Status (v0.7.0)

- The disc reader, boot file loader, SH-4 decoder (Capstone cross-check: 0 unexplained differences), function discovery (with PR tracking), the build's layout, coverage per region, the hardware profile and the FPSCR mode pass exist, with tests. 4,307 functions, 14 switch tables; the game region has 0 unknown bytes; every FPU instruction of the game and middleware has a proven mode. The lifter lifts 2,916 functions: all 1,259 of the game's and 1,657 of the middleware's (21 privileged ones left, to be replaced); its output compiles warning-free, and the end-to-end tests run lifted synthetic programs under every CI compiler. Nothing of the game runs yet.
- CI was green on v0.1.0 on all six jobs; check it after each push.

## Next steps, in order

1. The differential test: decided by the sister projects' precedent, a reference interpreter of our own from the manual (Punch-Out's `recomp/tests/difftest/` is the model: random encodings of every lifted instruction, random states, everything compared), with Flycast (`tools/dcref`) as a black-box check where the manual leaves room.
2. The runtime, as the sister projects brought theirs up: `qilin_run`, the replacement registry, stop on anything unregistered and name it.
3. The window, input, sound and the settings menu follow the sister projects exactly (`docs/naming.md`, the menu section of `docs/roadmap-and-design-targets.md`; Punch-Out's `punchout/src/menu.cpp` is the model).

## Conventions

- **Push without asking, on `main`, as in the sister projects** (the owner, 2026-11-21, asked whether this repo is worked on `main` with pushes without asking as the other four are: "Yes, same permissions. Keep working."). Push with `git push origin main`. Only what is 100% legal and above board to share with the world, checked before every push:
- **Audit what is being published:** every path added, binaries, anything named like game data or game-derived output, copied third-party text or code, personal details (local paths, names, e-mail addresses). No game data or anything derived from it. Never force-push published history.
- Releases are `vX.Y.Z`; the version lives only in `project(... VERSION ...)`. Every release gets a `CHANGELOG.md` entry (Keep a Changelog); never rewrite past entries.
- Prose in docs and commit messages: plain, specific, British spelling.
