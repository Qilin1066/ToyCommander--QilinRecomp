# Changelog

All notable changes to this project are recorded here, in the format of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Past entries are kept as written, never rewritten.

Versions are `vX.Y.Z`; the number lives only in `project(... VERSION ...)` in `CMakeLists.txt`. While the recompiler and runtime are being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

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
