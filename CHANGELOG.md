# Changelog

All notable changes to this project are recorded here, in the format of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Past entries are kept as written, never rewritten.

Versions are `vX.Y.Z`; the number lives only in `project(... VERSION ...)` in `CMakeLists.txt`. While the recompiler and runtime are being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

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
