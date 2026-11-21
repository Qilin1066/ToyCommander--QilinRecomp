# The recompiler core

`qilin`, in `recomp/src/`. v0.1.0 has the loader, the disc reader and the decoder; v0.2.0 adds function discovery. The rest are listed at the end.

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

## Function discovery and control flow (`cfg.*`, v0.2.0)
The boot file has no symbols, so `qilin cfg` finds the functions from the code, in three stages, each recorded as the function's *origin*:

1. **Recursive descent from the entry point** (origins `entry`, `call`, `tail`): branches, `bsr`, and `jsr @rN` / `jmp @rN` where the register provably holds a constant. SHC's ordinary call is `mov.l pool,rN` then `jsr @rN`, so register constants are tracked: PC-relative loads, `mova`, `mov #imm` and register copies set one, any other write clears it, and a call clears r0–r7.
2. **Function pointers** (`pointer`): 32-bit literal-pool values, and aligned words of bytes not yet classified, that point into the image are tried as entries, repeatedly until nothing new turns up.
3. **Orphans** (`orphan`): in each gap left between classified things, the first word that isn't padding is tried as an entry. This is code nothing refers to by address: static libraries and the engine bring unused functions along. Nothing in the image holds an orphan's address at any byte offset or through an address alias (checked), so they are dead unless reached by an address computed at run time; the lifter can leave them out, and the runtime's trap on an unknown indirect target names any that turns out to be needed.

**Constants where paths join.** The tracking is a forward dataflow per function: where two paths meet, a register keeps its constant only if both agree, and the instructions after the join are walked again with what is left. An indirect call is resolved only if every path into it carries the same target; one resolved on one path and lost at a join is reported (`qilin suspects`).

**One assumption**, stated in `cfg.hpp`: a call preserves r8–r15, the compiler's convention. Hand-written library code could break it. So the lifter must not turn a resolved `jsr @r13` into a direct call without a run-time check until the differential test has shown the convention holds everywhere it's relied on.

**Guesses must prove themselves.** A `pointer` or `orphan` candidate is kept only if:
- everything reachable from it decodes, with no branch in a delay slot, no `trapa`, and no path off the image;
- its code doesn't lie on another function's literals or jump table, nor its literals on code;
- everything it calls or jumps to with a constant target passes the same test, recursively;
- it doesn't run on, instruction after instruction, into code that is already known or into the start of another candidate. Compiled code never falls off its end into another function; data that happens to decode (dead literal-pool entries, float tables) does.

A `call` or `tail` target that fails is not a guess but a finding, and is listed by `qilin suspects`.

**Switches.** SHC compiles a dense `switch` to a bounds check and `shll r0 ; mov r0,rX ; mova table,r0 ; mov.w @(r0,rX),r0 ; braf r0`. The table's size is the constant the `cmp/hs` a few instructions back compares with; without that bound there is no table to trust, and the `braf` is reported as an indirect jump. Table words are classified as jump table, targets become branch targets of the function.

**Delay slots** belong to their branch. A branch target that is also some branch's delay slot is kept and reported.

**Every word is classified:** code, literal (read by a PC-relative `mov.w`/`mov.l`), jump table, padding (zero or `nop` words between classified things, at most 16 bytes, or to the end of the image), or unknown. `qilin coverage` prints the totals and the unknown runs, and exits 3 while any byte is unknown.

### Results on the supported build
`qilin cfg`, 0.2 s:

| | |
|---|---|
| Functions | 4,234: 1 entry, 2,341 call targets, 208 constant tail-jump targets, 900 from pointers, 784 orphans |
| Direct calls resolved | 11,911; constant tail jumps 464 |
| Indirect calls (through a pointer loaded at run time) | 516 |
| Indirect jumps with no constant target and no table | 146 (65 below `0x8C0B0000`, 81 in the libraries above it) |
| Switch tables | 21, with 718 cases |
| Rejected candidates | 1,251 pointers and 832 gap starts that aren't code; 1 call target (below) |

| Bytes (`qilin coverage`) | | |
|---|---|---|
| code | 577,626 | 77.18% |
| literal pools | 67,770 | 9.05% |
| jump tables | 1,156 | 0.15% |
| padding | 2,442 | 0.33% |
| unknown, all zero | 16,512 | 2.21% |
| unknown | 82,938 | 11.08% |

The unknown bytes are data, still to be described: 69,002 bytes from `0x8C09F5B6` to `0x8C0B0340` (the game's initialised data: pointer tables, strings; exactly the stretch between the last function below it and the entry code's jump target), 5,388 at the end of the image, two 8 KB zero-filled blocks in the libraries, and about 8.5 KB in some 850 small runs inside the code (literal-pool entries no reached instruction loads, small tables). Getting this to zero, by describing the data segments and the leftovers, is the next coverage step.

Findings (`qilin suspects`):
- **`0x8C099470` is called but isn't an SH-4 instruction in the decoder's set:** its first word is `0x003B`, followed by `rts ; nop`. Three more such words exist in the image, and beside it sits a stub that is `trapa #2 ; rts`. These are the development system's debugger hooks; they will be replaced natively (as no-ops or logging), not lifted. Whether `0x003B` should decode (as the SH-4's debugger break) is left open until a document can be cited for it.
- **Two functions with a branch into a delay slot** (`0x8C04EDF4`, `0x8C04EEE0`): hand-written copy routines (they use `pref` and `fschg`) that also jump into themselves by a computed offset.
- **Five functions with a call whose target register differs between paths:** left unresolved, as indirect calls.
- **The 146 indirect jumps:** tail calls through a pointer loaded from memory (`mov.l @(n,rX),r0 ; jmp @r0`), what look like the libraries' system-call trampolines (`mov.l @r0,r0 ; jmp @r0` on a pool address), jumps into the uncached mirror of the same code for cache control, and a few hand-written tables (`jmp` through a 32-bit table). Which of them are in lifted code decides what the lifter must support; that needs the library boundaries.

## Tests (`recomp/tests/tests.cpp`)
No game files: SHA-256 vectors; boot files (good, empty, odd, a byte too large for RAM); 60 instruction words pinned to their text, 36 words that must not decode, every form's canonical word and its word with all field bits set, the count of valid words and of delayed branches; function discovery on synthetic programs (calls through the pool and `bsr`, a switch with and without its bound, constants that agree and differ at a join, pointers to code and to data, data that would run on into code, orphans, a branch in a delay slot, a branch into one, running off the image) and the registers each instruction writes; a synthetic five-track GD-ROM written to a scratch folder (pregaps, a file in the last track, a file running into an audio track, a sector of the wrong mode) and 12 malformed cue sheets, a missing cue and a truncated track file.

## What comes next
In the order the sister projects took:

1. **Finish the analysis** (`docs/code-analysis.md`): the data segments described so `qilin coverage` reaches zero unknown; the library boundaries (Shinobi, Kamui 2, the audio manager, TrueMotion, the C runtime, the game), named; the indirect jumps in lifted code resolved or given a run-time form; direct hardware accesses per function (`qilin hwaccess`).
2. **The FPSCR mode pass:** PR, SZ and FR at every FPU instruction, per function, with the convention at calls established from the libraries' code.
3. **The lifter** (`qilin lift`): one C++ function per guest function, registers in locals, in slices as Ghoulies did (integer; calls, tail calls and the function table; the FPU).
4. **The differential test.** The sister projects run each lifted function against the original code under Unicorn; Unicorn has no SuperH. The candidates for the reference: QEMU's SH-4 as an external tool, driven through its GDB stub, and an independent model of the instructions written from Hitachi's manual (as Beat the Beat's `paired.py` is for the paired singles). To be decided when the lifter's first slice exists; the requirement stays the same: every function, whole call trees, 0 unexplained mismatches, also under UBSan.
