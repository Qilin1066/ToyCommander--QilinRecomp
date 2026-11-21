# The recompiler core

`qilin`, in `recomp/src/`. v0.1.0 has the first three pieces; the rest are listed at the end.

## The boot file loader (`bootfile.*`)
A Dreamcast boot file is a flat image of memory from `0x8C010000` (`disc-analysis.md`). The loader checks it's non-empty, a whole number of 16-bit words, and fits in the 16 MB of RAM, and gives bounds-checked little-endian reads; errors are `std::expected` messages. The supported build is recognised by the file's SHA-256 (our own, `sha256.*`).

## The GD-ROM reader (`gdrom.*`)
Reads a Redump-style dump: the `.cue`, the track table with each track's place on the disc (the high-density area starts at sector 45000), Mode 1 data sectors with their sync pattern and mode checked, the bootstrap's fields, and the ISO 9660 file system of the high-density area. It is runtime code too (no `std::format`): `qilin_run` will read the player's dump through it.

What it refuses, each with a message: cue commands it doesn't know, more than one track in a file, track modes other than `MODE1/2352` and `AUDIO`, tracks out of order, a dump whose high-density area doesn't start at track 3, track files that aren't whole sectors, reads that run into an audio track or off the disc, sectors without a sync pattern or of another mode, malformed or multi-extent directory records.

It doesn't check a sector's error-correction bytes: a dump verified against Redump's hashes has them right, and the hash check belongs to the installer step.

## The SH-4 decoder (`decoder.*`)
The Dreamcast's CPU is an SH7091, an SH7750-family SH-4. Instructions are 16-bit words, so the decoder is a 65,536-entry table built from one row per instruction form (209 forms): the form's fixed bits and up to three operands, each of which owns a bit field.

Built into the table, and checked when it is built (a mistake is an exception at start-up and a failed unit test):
- a form's fixed bits and operand fields cover all 16 bits, each exactly once;
- no word belongs to two forms;
- every form in the instruction list has a row.

**59,001 of the 65,536 words are SH-4 instructions.** The rest are invalid: undefined encodings, and the SH-2A, SH-4A and SH-DSP additions (`movrt`, `nott`, `synco`, `fpchg`, `movua.l`, `movmu.l`, the `tbr` forms, `ldc Rm,SGR` and so on), which the SH7091 doesn't have.

Each form carries flags for the passes to come: branch, conditional, delayed (the next instruction is its delay slot), call, return, indirect, load, store, float, privileged, PC-relative, cache control, and for the FPU whether the instruction's meaning depends on FPSCR.PR or FPSCR.SZ and whether it changes FPSCR. The decoder doesn't know the FPSCR mode: `fmov fr2,fr1` is printed that way whether it moves a single, or a double because SZ is set. The mode at each instruction is the analysis's job (`roadmap-and-design-targets.md`).

Text follows GNU objdump's forms: `mov.l 0x8c010024,r0` (a PC-relative load is printed as the address it reads), `mov.l r14,@(12,r15)` (displacements scaled, in decimal), `bf/s 0x8c010102`, `stc r7_bank,r15`, `ftrv xmtrx,fv12`.

### The cross-check (`tools/check/verify_decoder.py`)
Every one of the 65,536 words, at two addresses (PC-relative operands depend on the address and its alignment), against Capstone 5.0.7's SuperH decoder in SH-4 mode: the same words valid, the same text. Result: **58,969 words agree, 6,050 are invalid in both, and 0 differ unexplained.** Two reviewed differences, both listed in the script:

1. **485 words Capstone decodes and qilin doesn't:** instructions of later SuperH cores, which Capstone's SH-4 mode lets through. Recognised by mnemonic, register (`tbr`) or addressing mode, never by a list of words.
2. **32 words qilin decodes and Capstone doesn't:** `fipr`, `fcnvds` and `fcnvsd`, which are SH-4 instructions Capstone 5 lacks. Their encodings and text are from Hitachi's *SH-4 Software Manual* and pinned in the unit tests.

CI runs it on every push. It needs no game files.

GNU binutils' `objdump` for SuperH is a second independent decoder; a check against it is still to be added (it wasn't available where v0.1.0 was built).

## Tests (`recomp/tests/tests.cpp`)
No game files: SHA-256 vectors; boot files (good, empty, odd, a byte too large for RAM); 60 instruction words pinned to their text, 36 words that must not decode, every form's canonical word and its word with all field bits set, the count of valid words and of delayed branches; a synthetic five-track GD-ROM written to a scratch folder (pregaps, a file in the last track, a file running into an audio track, a sector of the wrong mode) and 12 malformed cue sheets, a missing cue and a truncated track file.

## What comes next
In the order the sister projects took:

1. **Function discovery and the CFG** (`qilin cfg`, `coverage`, `suspects`): recursive descent from the entry point; literal pools (every `mov.w`/`mov.l @(disp,PC)` and `mova` marks its data); SHC's switch tables; delay slots as part of their branch; calls through literal-pool addresses (`mov.l …,rN` then `jsr @rN` is the compiler's ordinary call) resolved to direct calls where the register provably holds the literal.
2. **The FPSCR mode pass:** PR, SZ and FR at every FPU instruction, per function, with the convention at calls established from the libraries' code.
3. **The lifter** (`qilin lift`): one C++ function per guest function, registers in locals, in slices as Ghoulies did (integer; calls, tail calls and the function table; the FPU).
4. **The differential test.** The sister projects run each lifted function against the original code under Unicorn; Unicorn has no SuperH. The candidates for the reference: QEMU's SH-4 as an external tool, driven through its GDB stub, and an independent model of the instructions written from Hitachi's manual (as Beat the Beat's `paired.py` is for the paired singles). To be decided when the lifter's first slice exists; the requirement stays the same: every function, whole call trees, 0 unexplained mismatches, also under UBSan.
