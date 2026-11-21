# Changelog

All notable changes to this project are recorded here, in the format of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Past entries are kept as written, never rewritten.

Versions are `vX.Y.Z`; the number lives only in `project(... VERSION ...)` in `CMakeLists.txt`. While the recompiler and runtime are being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

## [Unreleased]

### Added
- **The bring-up goes on** (`docs/runtime.md`): natives for Shinobi's work-area pointer, the console's settings (a USA console, NTSC, English, stereo, a VGA cable), its hardware set-up, its allocator (Shinobi's own algorithm over the same guest RAM, so every block lands where it would on the console), its real-time clock's set-up, the C runtime's `memset` and more. The game now runs through the middleware's whole system initialisation, Kamui 2's included, to the GD-ROM file system's initialisation, which is next.
- **The bus** (`recomp/runtime/bus.*`): RAM at `0x8C000000` stays the one-compare fast path; every other access goes to the bus, which handles RAM's other views, the SH-4's store queues (with QACR0/1) and the devices the port maps, and stops on anything else.
- **Hardware under Kamui 2** (`toycommander/src/pvr.*`, `sh4.*`): the PowerVR2's and the system bus's registers, video memory with its 64-bit view, and the SH-4's timer unit (timer 0 free-running, as Shinobi leaves it, counting guest time).

### Changed
- **Graphics: Kamui 2 is lifted and the port replaces the hardware under it** (the PowerVR2's registers, the tile accelerator's parameter stream, video memory) rather than Kamui 2's API. Without symbols, telling Kamui 2's functions apart inside the interleaved middleware proved unreliable, and Kamui 2 is the code that knows how its own calls become hardware commands: the parameter stream it writes is a small, documented format to decode into Vulkan 1.3 draws. The interrupt registration (`0x8C085E16`) is the one Kamui 2 function replaced.
- **The stop report ends with the guest call chain**, read from the return addresses saved on the stack; `QILIN_STACK=1` dumps the stack too.
- **Three rules from ToyRacer--QilinRecomp's core**, which started from this one and found them on its boot file (`docs/recompiler-core.md`), each with unit and end-to-end tests:
  - a `jmp` to a constant address that the function's other paths reach too is a far branch inside the function, lifted as a `goto`, not a tail call. `0x8C090B1C` has 6; two of its labels had been taken for functions, so the build has 4,305 functions and 2,932 lifted. Nothing else in the lifted code changes, and `qilin_run` stops where it did;
  - a pointer guess is thrown out when a function with a compiled prologue contains it and nothing else supports it (none in this build);
  - `bra` to itself, the code's fatal-error stop, is lifted as `rt::hang`, a stop of its own kind, not as an empty loop (one, at `0x8C087AD0`). `qilin cfg` counts branches by register, and its JSON lists them.

## [0.9.0] - 2026-11-21
The runtime's bring-up begins: the game's own lifted code runs, from the start-up into `main` and the middleware's system initialisation.

### Added
- **`qilin_run`** (`toycommander/src/`), built only from the player's own boot file: `-DQILIN_GAME_BOOT=<extracted>/files/1GUTH.BIN` makes `qilin lift` write the lifted C++ into the build tree at build time. It reads the boot file from the disc dump, checks it is the build it was lifted from, maps guest RAM and runs Shinobi's start-up natively (the stack fill, the C runtime's section set-up, Shinobi's initialisation, `main`, Shinobi's exit). Anything the port doesn't handle yet stops it with a report naming what was reached, where from, and the registers (exit code 3). `docs/runtime.md` has the design and the bring-up log.
- **The runtime library** (`recomp/runtime/`): guest memory reserved as 4 GB of address space with RAM committed; the registry of native replacements, which the stubs for replaced functions now call (`rt::replaced`); calls from native code into guest code; the stop report.
- **Native replacements** for the store-queue `memclr` and Shinobi's library initialisation and exit.
- **Replaced functions outside the replaced regions** (`replaced_functions` in `layout.cpp`, starting with `memclr`): `qilin lift` writes stubs for them that go through the registry. `qilin lift --files N` writes exactly N files of lifted code, for a build system that must name them in advance.
- **SR is modelled:** lifted code reads it and writes its interrupt mask, which the middleware does around its hardware set-up; a change of mode, register bank, exception blocking or FPU enable stops. 19 more functions lift: **2,934 in all**, only 2 not (exception set-up, to be replaced). The differential test covers the SR forms: 248,016 runs, 0 mismatches.
- **Tests** for the registry (a replaced function stops, naming itself and its caller, until native code is provided), `call_guest` and guest memory.

### Changed
- **The repository is `ToyCommander--QilinRecomp`**, renamed by the owner for readability, like all the QilinRecomp projects (Beat the Beat's is now `RhythmHeavenFever--QilinRecomp`). The docs and their links use the new names.

## [0.8.0] - 2026-11-21
The differential test: every instruction the lifter lifts, against a reference interpreter of our own.

### Added
- **The differential test** (`qilin_difftest`, `recomp/tests/difftest/`), as the sister projects have it. Random valid encodings of all 109 non-privileged instruction forms run on their own, in a delay slot, with 64-bit transfers and in double precision, plus twelve branch programs. The real analysis and lifter lift them at build time, and they run 48 times each from random states, lifted and on a reference interpreter written separately from the instruction definitions (memory byte by byte, the FPU in double with the denormal flush by hand). Registers, flags, MAC, FPUL, FPSCR, both FPU banks, return addresses, stops and a 16 KB memory window must agree: **242,496 runs, 0 mismatches**, in CI on every configuration. Planted bugs (`subc`'s operands swapped, `fcmp/gt` reversed, `bt/s` reading T after its slot) fail at once.

### Changed
- **Everything unarguably legal**, as the owner asked: CLAUDE.md now says so, and keeps the rule against other people's GPL code and tables (an emulator's included) although this project is GPL-3.0, and against any Dreamcast BIOS.

### Fixed
- The lifter emitted self-assignments and self-comparisons (`r13 = r13;`, `t = r15 > r15;`) for encodings with one register as both operands, which warnings-as-errors rejects. They now become their known results. The game has none, so its lifted code is unchanged.

## [0.7.0] - 2026-11-21
Every function of the game is lifted. The FPU's approximate instructions are measured against a Dreamcast emulator, as the owner asked with no console at hand.

### Added
- **`fmac`, `fsrra`, `fipr`, `ftrv` and `fsca` in the lifter**, with definitions of our own (`recomp/runtime/guest.hpp`):
  - `fmac` rounds once;
  - `fsrra` divides 1 by the rounded square root;
  - `fipr` and `ftrv` sum in double and round once, with the matrix column-major in the back bank;
  - `fsca` reads a table of the exact sine values, built once at start-up.
  
  With DN = 1, denormal operands are zero, as for every FPU instruction.
- **`qilin_dcref`** (`tools/dcref/`): measures those definitions against Flycast's libretro core, run as a black box. A probe program is booted as an ELF on the emulator's BIOS replacement, with an OpenGL context and no window; its results come from guest RAM. Over 50,000 cases each, `fmac`, `fsrra`, `fipr`, `ftrv` and the ordinary arithmetic agree with the emulator on every input without a denormal; the emulator keeps denormal operands where the manual flushes them. `fsca` agrees on 18,510 of 65,536 angles: the emulator has a table of its own, at most 2⁻²³ from the exact values, and copying a GPL table isn't allowed. `tools/dcref/fetch.sh` fetches the core; nothing of it is committed. Opt-in (`-DQILIN_DCREF=ON`), Linux x86-64.
- **A branch into a delay slot** is lifted: the slot's instruction is copied under its own label after the function's body, followed by a jump on. That function, `0x8C04EEE0`, is the C runtime's store-queue `memset` (`docs/code-analysis.md`): its lifted code stops at the store queue until the runtime replaces it.
- **`mov.l Rn,@-Rn`** stores the register's value from before the decrement, as the emulator does.
- **End-to-end tests** for the five instructions' wiring and exact values (one rounding in `fmac`, the matrix's orientation in `ftrv`, `fsca` at the quarter points and its symmetry), and for `mov.l r14,@-r14`: 128,947 checks.

### Changed
- **Licence: GPL-3.0** (`LICENSE`), as in all the QilinRecomp projects, in place of MIT: anyone may use, share, change and sell it, but whatever they distribute that is built from it must come with its full source under the same licence. Earlier versions stay under MIT.
- **2,916 functions are lifted** (was 2,708): all 1,259 of the game's and 1,657 of the middleware's. Outside Shinobi and the unreferenced functions, only the 21 middleware functions with privileged instructions are left, to be replaced.
- The differential test's reference follows the sister projects: a reference interpreter of our own, written from the manual, with the emulator as a black-box check where the manual leaves room.

## [0.6.0] - 2026-11-21
The lifter's FPU slice: every function of the game is lifted except the 66 that use `fmac`, `fsca` or `fsrra`.

### Added
- **Floating point in the lifter**, in the mode the FPSCR pass proves at each instruction:
  - single and double precision arithmetic, comparisons, conversions and FPUL transfers;
  - `fmov` with 32- and 64-bit transfers, including the back bank's XD registers;
  - `fschg`, `frchg` (the two banks are arrays that swap), and FPSCR loads and stores.
  
  The current bank lives in host `float` locals. Denormals are flushed by the host itself, once `rt::host_fp_setup()` has set FTZ and DAZ on x86-64 or FZ on ARM64, as FPSCR.DN = 1 requires. Generated code is compiled without floating-point contraction. **2,708 functions are lifted** (was 1,832): 1,193 in the game, 1,515 in the middleware.
- **Stops for what isn't pinned down:** `ftrc` out of range or of a NaN, and an FPSCR load that enables FPU exceptions. `fmac`, `fipr`, `ftrv`, `fsca` and `fsrra` aren't lifted until a clean source gives their exact results (206 functions).
- **End-to-end FPU tests:** single-precision arithmetic against exact bit patterns (1/3, √2, denormal operands and results) and 2,000 random operand sets; `ftrc` at its range's edges; 64-bit moves, XD pairs and bank changes, keeping the bits of a signalling NaN and a denormal; double precision over 1,000 random pairs. 121,469 checks in all.

### Fixed
- **The FPSCR pass skipped functions called only from replaced code.** The game's `main`, which only Shinobi's start-up calls, counted as called, so it was never entered in the default mode, and nor was anything reached only through it. They are analysed now: the game has 14,669 FPU instructions (v0.4.0 reported 14,432) and the middleware 21,290 (21,143), all proven.

## [0.5.0] - 2026-11-21
The lifter's first slice: integer code, lifted to C++ and tested end to end under every CI compiler.

### Added
- **The lifter** (`qilin lift`, `recomp/src/lifter.*`, `lift_output.*`): one C++ function per guest function, with guest registers in locals declared only for the registers used. It lifts every integer instruction, delay slots, conditional and delayed branches, calls (direct when the target is a known function, through the function table otherwise, with the return address checked afterwards), tail calls, switch tables as C++ `switch`es, the calls by loaded PR and the returns through a saved PR. Floating point (the next slice) and privileged instructions aren't lifted yet: such functions get a stub that stops as *not lifted*, and Shinobi's functions get stubs that stop as *unregistered*, naming the caller. On the supported build, **1,832 functions are lifted** (730 in the game, 1,102 in the middleware); 1,083 wait for the FPU slice, 21 have privileged instructions and 1 has a branch into a delay slot. `--range` lifts only part of the image, `--orphans` the unreferenced functions too. Output files are rewritten only when they change.
- **The runtime's first pieces** (`recomp/runtime/`): the guest context, memory access with an optional checked mode (`QILIN_CHECKED_MEMORY`) that stops on an access outside the mapped regions and names the instruction, the helpers for the instructions with carries, overflow, division steps, shifts, `cmp/str`, `tas.b`, `mac` and `pref`, and indirect calls through the sorted function table. What can't be pinned down from a clean source (`mac` with S set) or needs the runtime (`pref` of a store queue) stops.
- **End-to-end lifter tests** (`qilin_liftgen`, `qilin_lift_tests`): synthetic SH-4 programs built by a small assembler are lifted at build time by the real analysis and lifter, compiled with the compiler under test, and run with checked memory against results computed from the instructions' definitions: 86,286 checks over 64-bit arithmetic, SHC's signed and unsigned division sequences, shifts, rotates, multiplies, loops, a switch, every kind of call, memory of every size and addressing mode, and the stops. So CI now compiles generated code under GCC 14, Clang 19, Apple Clang and MSVC with warnings as errors.

## [0.4.0] - 2026-11-21
The FPU's mode at every FPU instruction, which the lifter needs to emit the right operations, and the control-flow fixes that finding it uncovered.

### Added
- **The FPSCR mode pass** (`qilin fpmode`, `recomp/src/fpmode.*`): precision (PR), transfer size (SZ), bank (FR), rounding (RM) and denormals (DN) at every FPU instruction, as known bits through `fschg`, `frchg`, `lds`, the read-modify-write sequences and saves on the stack. Interprocedural and context-sensitive: each function is analysed once per mode it is entered in, calls go on in their callee's exit mode, and the default mode (`0x00040000`, which Shinobi's start-up loads) is assumed only for address-taken functions and indirect calls. **All 14,432 FPU instructions of the game and all 21,143 of the middleware are proven.** The game runs entirely in the default mode; the middleware uses double precision (36), 64-bit transfers (214) and bank 1 (209). `--list` prints what it reports, `--json` every function's entry and exit modes.
- **`successors`:** the instructions control reaches next inside a function, through delay slots, calls and tail calls.
- **Tests** for each of these, and for the control-flow changes below.

### Changed
- **The bank (FR) doesn't need to be known:** the lifted code will keep the two banks as two arrays and swap them on `frchg`, as the hardware does. The engine really runs in bank 1 for a while after some of the matrix library's calls, and paths rejoin there in different banks; compiled code never notices which bank is current.
- **Function discovery tracks PR, the return address.** Hand-written code calls by loading PR and jumping (`lds r1,pr ; jmp @r0`) and returns through a saved PR (`jmp @r2`). 2 such calls and 18 such returns were found, all in the matrix library; they were a tail call and indirect jumps, which hid the code that runs after the call. Indirect jumps: 143 → 127.
- **A call clears only the scratch registers its callee can write**, worked out per callee and transitively (all of them if it calls indirectly), instead of all of r0–r7 by convention.
- **The middleware's attribution is driven by bring-up** (`docs/code-analysis.md`): grouping functions by shared global variables names libraries (the audio manager's core is one group) but doesn't give a complete map, since the libraries interleave at a finer grain than object files. The middleware is lifted with the game, and where it stops on hardware names what to replace.

## [0.3.0] - 2026-11-21
The boot file's layout: where the game, the middleware and Shinobi are, what is data, and what reaches hardware. The game's code is now fully accounted for.

### Added
- **The layout of the supported build** (`recomp/src/layout.*`): 13 regions covering the whole image, each with a plan (lift, replace, undecided, data). The evidence for every boundary is in the new `docs/code-analysis.md`. The game is `0x8C010080`–`0x8C0495B8`: no code above it calls into it, while it makes 1,641 calls up into 278 middleware functions. Shinobi's start-up calls the game once, at its `main` (`0x8C02796A`). Function discovery takes the layout as input: no function is found in a data region, and a call into one is reported.
- **Coverage per region** (`qilin coverage`): a table of code, literal, table, padding, data and unknown bytes per region. It exits 3 while a *lifted* region has unknown bytes, as in the sister projects. **The game has 0**; 0.27% of the image is still unknown (2,020 bytes), all in the middleware and Shinobi. `--map` prints every run of every kind with the function that claimed it.
- **Hardware-access profile** (`qilin hwaccess`, `recomp/src/hwaccess.*`): every load, store and cache operation at a constant address outside cached RAM, per function, named by hardware block (the SH-4's registers, the system bus, Maple, GD-ROM, the PowerVR2, the sound chip, video memory, the tile accelerator, the store queues). 76 functions: 1 in the start-up, 30 in the middleware, 45 in Shinobi, **none in the game**.
- **`docs/code-analysis.md`:** the regions and their evidence, the middleware's anchors (functions named by their strings: the audio manager in three places, Kamui 2, `kmdk`, the C runtime, the engine), why the middleware has to be attributed per function, the hardware profile, the indirect jumps by region, and the runtime's to-do list.
- **Tests** for each new rule, the layout, the region map and the profile.

### Changed
- **Function discovery** finds 4,307 functions (was 4,234), and the coverage of the image rose from 86.7% to 99.73% (counting the layout's data regions):
  - integer arithmetic on constants keeps them constant (`add`, `sub`, logic, fixed shifts, extensions);
  - loads from constant addresses near the code (`mova pool,r0 ; fmov @r0,frN`) mark their words as literals: these were most of the small unknown runs, float constants;
  - a branch to another function is a tail call: to the start of a compiled prologue, or to an entry any pass found. The analysis repeats until the set of entries stops growing (two passes). Before, such branches absorbed the function they reached;
  - in gaps whose first word isn't code, the starts of compiled prologues are tried as well.
- `qilin cfg` counts each switch table once: 14 tables with 578 cases. Before, a table reached from several functions through shared code was counted for each (the 21 tables and 718 cases reported for v0.2.0 were counted that way).
- `register_constants` exposes the CFG's dataflow for other passes; `sh4::access_size` and `sh4::register_address` give an instruction's access size and address.

## [0.2.0] - 2026-11-21
Function discovery: the boot file's code found and every byte of it classified.

### Added
- **Function discovery and control flow** (`recomp/src/cfg.*`, `qilin cfg`): recursive descent from the entry point with register constants tracked through literal pools, `mova`, immediates and copies, as a dataflow that keeps a constant across a join only if every path agrees; SHC's `braf` switch tables with their bounds; delay slots; function pointers from literal pools and data; unreferenced code in the gaps ("orphans"). Guessed entries are kept only if all they reach decodes, all they call does too, and they don't run on into known code. On the supported build: 4,234 functions (1 entry, 2,341 call targets, 208 tail-jump targets, 900 from pointers, 784 orphans), 11,911 direct calls resolved, 21 switch tables with 718 cases.
- **`qilin coverage`:** every 16-bit word as code, literal, jump table, padding or unknown; exits 3 while anything is unknown. Today 86.7% is classified (code 77.18%, literal pools 9.05%); the rest is data still to be described, 69,002 bytes of it one block between the code and the libraries.
- **`qilin suspects`:** functions with problems and call targets that aren't code. Its first findings are in `docs/recompiler-core.md`: the development system's debugger hooks (a word outside the decoder's instruction set, called from two functions), two hand-written copy routines that branch into a delay slot, and the 146 indirect jumps without a table.
- **Tests:** function discovery on thirteen synthetic programs, and the registers instructions write.

### Changed
- `docs/disc-analysis.md`: the boot file's layout from function discovery replaces the guess from invalid-word density.

## [0.1.0] - 2026-11-21
The project starts, on the sister projects' architecture and requirements: our own SH-4 → C++23 recompiler, Sega's libraries replaced natively, Vulkan 1.3 only. The first pieces are the disc reader, the boot file loader and an SH-4 decoder checked against Capstone over the whole encoding space.

### Added
- **The design:** `docs/roadmap-and-design-targets.md` (the sister projects' targets applied to the Dreamcast: delay slots, the FPSCR modes, the memory aliases, the library boundary, Kamui 2 and the tile accelerator, sound, the movies, hardware scope, Vulkan baseline, the settings menu, the roadmap), `docs/disc-analysis.md` (the supported release, the dump's tracks, the files, the boot file), `docs/recompiler-core.md`, `docs/prior-art.md`, `docs/naming.md`.
- **GD-ROM reader** (`recomp/src/gdrom.*`): Redump-style dumps (`.cue` and a `.bin` per track), both density areas, Mode 1 sectors checked, the bootstrap's fields, the high-density area's ISO 9660 file system. `std::expected` errors for anything malformed or unsupported.
- **Boot file loader** (`recomp/src/bootfile.*`): the flat image at `0x8C010000`, bounds-checked little-endian reads. The supported build (USA, `MK-57020`, `V1.022`) is recognised by the SHA-256 of `1GUTH.BIN` (`recomp/src/sha256.*`, from the sister projects).
- **SH-4 decoder** (`recomp/src/decoder.*`): all 209 instruction forms of the SH7750 family, table-driven, with operand fields and flags for the analysis passes and the lifter (delayed branches, FPSCR-dependent instructions, privileged ones). The table is checked as it is built: every form defines all 16 bits exactly once and no word decodes two ways. 59,001 of the 65,536 words are instructions; later SuperH cores' additions decode as invalid. Text in objdump's form.
- **`qilin` CLI:** `disc`, `extract`, `info`, `insns`, `disasm`, `profile`, `--version`.
- **Decoder cross-check** (`tools/check/verify_decoder.py`) against Capstone 5.0.7: every word of the encoding space at two addresses. 58,969 words agree, 6,050 are invalid in both, 0 unexplained differences; the two reviewed ones (485 words of later cores that Capstone lets through, 32 words of `fipr`, `fcnvds` and `fcnvsd` that it lacks) are listed in the script and in `docs/recompiler-core.md`.
- **Tests** (`qilin_tests`, no game files): SHA-256 vectors, boot files good and malformed, 60 encodings pinned to their text, 36 words that must not decode, every form's canonical and all-fields-set words, a synthetic five-track GD-ROM and malformed cue sheets.
- **CI** with warnings as errors on Linux x64 (GCC 14, Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64, Windows x64 (MSVC), plus the decoder check.
