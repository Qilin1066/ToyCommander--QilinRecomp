# Changelog

All notable changes to this project are recorded here, in the format of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Past entries are kept as written, never rewritten.

Versions are `vX.Y.Z`; the number lives only in `project(... VERSION ...)` in `CMakeLists.txt`. While the recompiler and runtime are being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

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
