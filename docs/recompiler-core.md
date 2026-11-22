# The recompiler core

`qilin`, in `recomp/src/`. v0.1.0 has the loader, the disc reader and the decoder; v0.2.0 adds function discovery; v0.3.0 the build's layout, coverage per region and the hardware-access profile; v0.4.0 the FPSCR mode pass, with PR and scratch registers tracked through calls. The rest are listed at the end.

## The boot file loader (`bootfile.*`)
A Dreamcast boot file is a flat image of memory from `0x8C010000` (`disc-analysis.md`). The loader checks it's non-empty, a whole number of 16-bit words, and fits in the 16 MB of RAM, and gives bounds-checked little-endian reads; errors are `std::expected` messages. The supported build is recognised by the file's SHA-256 (our own, `sha256.*`).

## The GD-ROM reader (`gdrom.*`)
Reads a Redump-style dump: the `.cue`, the track table with each track's place on the disc (the high-density area starts at sector 45000), Mode 1 data sectors with their sync pattern and mode checked, the bootstrap's fields, and the ISO 9660 file system of the high-density area. It is runtime code too (no `std::format`): the games' programs read the player's dump through it.

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

## Function discovery and control flow (`cfg.*`, v0.2.0; v0.3.0)
The boot file has no symbols, so `qilin cfg` finds the functions from the code, in three stages, each recorded as the function's *origin*:

1. **Recursive descent from the entry point** (origins `entry`, `call`, `tail`): branches, `bsr`, and `jsr @rN` / `jmp @rN` where the register provably holds a constant. SHC's ordinary call is `mov.l pool,rN` then `jsr @rN`, so register constants are tracked: PC-relative loads, `mova`, `mov #imm` and register copies set one, integer arithmetic on constants keeps one (`add`, `sub`, the logic operations, shifts by a fixed count, extensions, `neg`, `not`, `swap`; v0.3.0), any other write clears it, and a call clears r0–r7.
2. **Function pointers** (`pointer`): 32-bit literal-pool values, and aligned words of bytes not yet classified, that point into the image are tried as entries, repeatedly until nothing new turns up.
3. **Orphans** (`orphan`): in each gap left between classified things, the first word that isn't padding is tried as an entry; where that fails, so is every word in the gap that starts a compiled prologue (the first of a run of `mov.l rN,@-r15` for r8–r14, or `sts.l pr,@-r15`; v0.3.0). This is code nothing refers to by address: static libraries and the engine bring unused functions along. Nothing in the image holds an orphan's address at any byte offset or through an address alias (checked), so they are dead unless reached by an address computed at run time; the lifter can leave them out, and the runtime's trap on an unknown indirect target names any that turns out to be needed.

**Constants where paths join.** The tracking is a forward dataflow per function: where two paths meet, a register keeps its constant only if both agree, and the instructions after the join are walked again with what is left. An indirect call is resolved only if every path into it carries the same target; one resolved on one path and lost at a join is reported (`qilin suspects`).

**One assumption**, stated in `cfg.hpp`: a call preserves r8–r15, the compiler's convention. Hand-written library code could break it. So the lifter must not turn a resolved `jsr @r13` into a direct call without a run-time check until the differential test has shown the convention holds everywhere it's relied on.

**Guesses must prove themselves.** A `pointer` or `orphan` candidate is kept only if:
- everything reachable from it decodes, with no branch in a delay slot, no `trapa`, and no path off the image;
- its code doesn't lie on another function's literals or jump table, nor its literals on code;
- everything it calls or jumps to with a constant target passes the same test, recursively;
- it doesn't run on, instruction after instruction, into code that is already known or into the start of another candidate. Compiled code never falls off its end into another function; data that happens to decode (dead literal-pool entries, float tables) does.

A `call` or `tail` target that fails is not a guess but a finding, and is listed by `qilin suspects`.

**Tail calls by branch** (v0.3.0). SHC also tail-calls with a plain `bra`, and sometimes `bt`/`bf`. A branch is a tail call, and its target a function of its own, when the target is the start of a compiled prologue (compiled code reaches a prologue only from outside its function) or an entry any earlier pass found. Because a function found late can be the target of a branch in one found early, the whole analysis runs again with every entry found so far, until the set no longer grows: two passes on the supported build. Before this rule, branches to other functions absorbed them: `0x8C013850`, a function nobody calls, was code of `0x8C01421E`.

**Data reached through `mova`** (v0.3.0). Float constants sit in literal pools too, read with `mova pool,r0 ; fmov @r0,frN` or `mov.l @(disp,r0)`. A load from a constant address within 2 KB of the instruction is recorded as a literal read like a PC-relative one. Reads of 64-bit floats (`fmov` with FPSCR.SZ set) are recorded as 4 bytes until the FPSCR mode pass knows SZ.

**The layout** (v0.3.0). With the build's layout (`recomp/src/layout.cpp`, evidence in `code-analysis.md`), words in its data regions are `data` from the start: no function can be found in them (a guess there is rejected, a call or jump into one is reported), while their words are still read as possible function pointers.

**PR, the return address** (v0.4.0). Hand-written library code calls by loading PR and jumping (`sts pr,r2 ; mov.l back,r1 ; lds r1,pr ; jmp @r0`: a call that returns to `back`) and returns by jumping through the saved value (`jmp @r2`). The register state tracks PR: a constant loaded with `lds rN,pr`, which registers hold the return address the function was entered with (`sts pr,rN`), and whether PR still does. A `jmp` through the entry return address is a return; a `jmp` while PR holds a constant is a call that continues where PR points (`Function::call_returns`). On the supported build: 18 returns and 2 such calls, all in the matrix library; before, they were indirect jumps and a tail call, which hid the code that runs after the call.

**The pushed return address** (v0.10.0). Compiled code saves PR on the stack (`sts.l pr,@-r15`) and pops it back (`lds.l @r15+,pr`), and the matrix library's shared tails then return through a copy (`sts pr,r2` … `jmp @r2`), reached by `bra` after the pop (`0x8C061AA0`). The register state follows r15 as an offset from its entry value (pushes, pops, `add #imm,r15`; any other write forgets it) and the slot the entry PR was pushed to; a store over that slot, or one through r15 at an unknown offset, forgets the slot, and a pop of exactly that slot restores the entry PR. On the supported build this turned one computed jump into a return; two more in unreferenced code stay as they are (one, `0x8C06170C`, really jumps through a register a callee overwrote).

**Scratch registers across a call** (v0.4.0). A call used to clear every constant in r0–r7, the registers a callee may change by convention. Now it clears only those the callee can write: every scratch register written by its code or by anything it calls, transitively, and all of them if it calls or jumps indirectly. This is a proof per callee, not a convention, and hand-written code that keeps a value in r2 across its own helpers is followed. (r8–r15 are still assumed kept, the compiler's convention.)

**Successors** (v0.4.0). `successors(boot, f, pc)` gives the instructions control can reach next inside a function: a delayed branch's successor is its delay slot, and the slot's are the branch's targets; a call returns after its slot; tail calls, returns and indirect jumps leave. The FPSCR pass is built on it, and so will the lifter be.

**Pointers into a known function** (v0.10.0). A function-pointer table can point at code an earlier function already took in through a branch that is really a tail call: gdFs's driver table points at `0x8C0B1FD6`, which `0x8C0B1704` reaches with `bra`. Such a pointer becomes a function when the words on both sides of it in the table point at function entries and it isn't a delay slot; the next pass then treats the branch as a tail call. On the supported build it finds exactly that one (4,306 functions). The rule is narrow on purpose: any word that happens to point into code would split functions that aren't split.

**Shared tails** (v0.10.0). A guess is rejected when it runs on into known code, because data that decodes does that. But compiled and hand-written code also has short entries that fall into a tail other entries branch to: bu's table of `bra tail ; mov #n,r4` entries ends with one, `mov #7,r4` at `0x8C0B6778`, that falls into the tail, and the matrix library has a run of them (`0x8C0613AC`–`0x8C061A2C`, before a 920-byte unknown run). So a guess from an address the image holds (not an orphan) may run into code that some function branches to. It found 16 functions, and the unknown runs went from 49 to 44 (4,322 functions). Bytes nothing points at that a branch skips are still data (a unit test keeps both).

**Switches.** SHC compiles a dense `switch` to a bounds check and `shll r0 ; mov r0,rX ; mova table,r0 ; mov.w @(r0,rX),r0 ; braf r0`. The table's size is the constant the `cmp/hs` a few instructions back compares with; without that bound there is no table to trust, and the `braf` is reported as an indirect jump. Table words are classified as jump table, targets become branch targets of the function.

**Delay slots** belong to their branch. A branch target that is also some branch's delay slot is kept and reported.

**Every word is classified:** code, literal (read by a PC-relative `mov.w`/`mov.l`, or through `mova`), jump table (SHC's 16-bit offsets, or a table of code addresses a `jmp` goes through), padding (zero or `nop` words between classified things, at most 16 bytes, or to the end of the image; `nop` with its bytes swapped, the engine's filler, up to 32), data (a data region of the layout, or one of its data blocks), or unknown. The layout also names code nothing refers to (`known_entries`) and stubs whose code doesn't decode (a replaced function's size), so that the supported build has no unknown byte at all (`code-analysis.md`, "Every byte"). `qilin coverage` prints a table per region of the layout, and exits 3 while a **lifted** region has an unknown byte (without a layout, the whole image counts as lifted). `--list` adds every unknown run; `--map` prints every run of every kind with the function that claimed it.

### Results on the supported build (v0.3.0; v0.4.0)
`qilin cfg`, 0.3 s (two passes):

| | |
|---|---|
| Functions | 4,322: 1 entry, 2,316 call targets, 303 tail-call targets, 907 from pointers (one inside a function found earlier, from a pointer table, and some into shared tails: see below), 795 orphans. By region: 1,319 in the game, 2,205 in the middleware, 797 in Shinobi. Since every byte was accounted for (unreleased): 4,338, with 905 from pointers, 796 orphans and 17 from the layout's entries; 17 jump tables (629 cases), two of them tables of code addresses |
| Direct calls resolved | 11,410; constant tail calls 709; calls by loaded PR 2; returns by `jmp` 18 (v0.4.0) |
| Indirect calls (through a pointer loaded at run time) | 529 |
| Indirect jumps with no constant target and no table | 127: 3 in the game, 44 in the middleware, 80 in Shinobi (`code-analysis.md`) |
| Switch tables | 14, with 578 cases |
| Rejected candidates | 86, 1 of them a call target (below) |

`qilin coverage`, per region of the layout:

| Region | Plan | Code | Literal | Tables | Padding | Data | Unknown |
|---|---|---|---|---|---|---|---|
| start-up | replace | 32 | 14 | 0 | 2 | 0 | 0 |
| markers | data | | | | | 80 | |
| **game** | **lift** | 201,422 | 31,638 | 962 | 786 | 0 | **0** |
| middleware | undecided | 315,008 | 33,674 | 150 | 1,454 | 0 | 1,968 |
| initialised data | data | | | | | 69,002 | |
| Shinobi (4 rows) | replace | 61,980 | 7,960 | 42 | 322 | 0 | 52 |
| Shinobi's buffer, stack, handler data | data | | | | | 16,508 | |
| Shinobi's data | data | | | | | 5,388 | |
| **total** | | 578,442 | 73,286 | 1,154 | 2,564 | 90,978 | 2,020 (0.27%) |

The game is fully accounted for. In v0.2.0, 11% of the image was unknown: the data segments (now described by the layout), float literals reached through `mova` (now recorded) and code absorbed or missed around tail calls (now split). What is left is in regions that aren't lifted yet: in the middleware, jump-stub and byte tables and the debugger stubs, resolved with its attribution (`code-analysis.md`).

Findings (`qilin suspects`):
- **`0x8C099470` is called but isn't an SH-4 instruction in the decoder's set:** its first word is `0x003B`, followed by `rts ; nop`. Three more such words exist in the image, and beside it sits a stub that is `trapa #2 ; rts`. These are the development system's debugger hooks; they will be replaced natively (as no-ops or logging), not lifted. Whether `0x003B` should decode (as the SH-4's debugger break) is left open until a document can be cited for it.
- **Two functions with a branch into a delay slot** (`0x8C04EDF4`, `0x8C04EEE0`): hand-written copy routines (they use `pref` to the store queues and `fschg`) that also jump into themselves by a computed offset. Replaced natively.
- **Four functions with a call whose target register differs between paths** (`0x8C075B54`, `0x8C075ED0`; in Shinobi `0x8C0B08D8`, `0x8C0B80FE`): left unresolved, as indirect calls.
- **The 127 indirect jumps:** by region in `code-analysis.md`. The game's 3 are indirect tail calls through function pointers, which go through the function table.

## The FPSCR mode pass (`fpmode.*`, v0.4.0)
What an FPU instruction does depends on FPSCR: PR (single or double precision), SZ (32- or 64-bit `fmov`), FR (which bank fr0–fr15 name), RM (round to nearest or towards zero) and DN (denormals flushed). `qilin fpmode` works out those bits at every FPU instruction.

**Known bits.** Each tracked bit, in FPSCR and in every general register, is known (0 or 1) or unknown. That is followed through `fschg` and `frchg`, through `lds rN,fpscr`, through the read-modify-write the code uses (`sts fpscr,rN`, `and`/`or`/`xor`/`not` with masks, `lds rN,fpscr`), and through saves and restores on the stack (`sts.l fpscr,@-rN` … `lds.l @rN+,fpscr`, assuming as compiled code does that the saved word isn't overwritten in between).

**Contexts.** The pass is interprocedural and context-sensitive: each function is analysed once per mode it is entered in. Functions whose address is taken (a pointer to them exists outside code; a literal-pool word that its own function only calls through doesn't count) and functions no analysed code calls are entered in the default mode. The others are entered in the modes their callers call them in. A call goes on in the callee's exit mode for that entry; a call to a function with no analysis (in a replaced region, or reached indirectly) keeps the mode. Recursion is handled by assuming a callee keeps the mode until its result is known, and iterating until no context or exit changes. Functions in regions the layout replaces aren't analysed: they are native at run time and don't change the mode.

**What instructions need.** Arithmetic needs PR, RM and DN; `fmov` and `fschg` need SZ. FR isn't needed: the lifted code keeps the current and the back bank as two arrays and `frchg` swaps them, as the hardware does, so instructions always name the current bank (`code-analysis.md` has why that matters here). FR is tracked all the same, for the reports.

**What is reported:**
- an FPU instruction whose needed bits aren't known (*unproven*);
- one whose bits are known in each of its function's contexts but differ between them (*entry-dependent*: lifted once per mode);
- a join where paths arrive with PR, SZ, RM or DN different;
- an indirect call or jump in a mode other than the default;
- an address-taken function called, tail-called or returning in another mode.

`--list` prints every problem; `--json` every function's contexts with their entry and exit modes.

**How it got there**, because each step was a real finding:
1. A per-function pass assuming the default mode at every call and return reported 34 functions that break the convention: the matrix library and the double-precision helpers.
2. Taking callers' modes and callees' exits interprocedurally made almost everything unknown. Shinobi's context-switching code (and `longjmp`, which it calls) returns in a mode loaded from memory, and that spread to every caller of Shinobi. Fixed by leaving replaced regions out.
3. Summaries relative to the entry ("the bit as on entry", or its inverse) lost precision where a known value met an "as on entry" one that was in fact equal. Replaced by concrete contexts.
4. What was still lost came from two control-flow errors in hand-written code: the call made by loading PR and the return through a saved PR, and a return address kept in a scratch register across a helper. Fixed in the CFG (above).
5. What then remained was real: the engine runs in bank 1 after some matrix calls, and paths rejoin there in different banks. That is why FR is handled by swapping banks rather than by knowing it.

Results are in `code-analysis.md`: every FPU instruction of the game (14,669) and of the middleware (21,290) is proven, with 1 entry-dependent. (v0.4.0 counted 14,432 and 21,143: a function called only from replaced code, such as the game's `main`, which only Shinobi's start-up calls, wasn't analysed, and nor was anything reached only through it. Since v0.6.0 calls from replaced code don't count, so such functions start in the default mode.)

## Hardware-access profile (`hwaccess.*`, v0.3.0)
`qilin hwaccess` lists, per function, every load, store and cache operation whose address is a constant outside cached RAM, with the hardware block it reaches (`hw_region`: the SH-4's own registers in P4, then the Dreamcast's area 0 registers, sound chip, video memory, tile accelerator and RAM views, from the SH7750 hardware manual's and the Dreamcast's memory maps). Addresses come from the CFG's register constants (`register_constants`, the same dataflow). Accesses through pointers loaded from memory, or relative to GBR, aren't seen. Results are in `code-analysis.md`: 76 functions, none of them in the game.

## The lifter (`lifter.*`, `lift_output.*`, v0.5.0): slice 1, integer code
`qilin lift <1GUTH.BIN> --out <dir> [--range lo-hi] [--no-orphans]` turns each function the CFG found into one C++ function, `void fn_XXXXXXXX(rt::Context&)`. The first slice lifts every function without floating point or privileged instructions: all the integer instructions, delay slots, branches, calls, tail calls, switch tables, the calls by loaded PR and the returns through a saved PR.

**What the output looks like.** Guest registers live in locals, declared only for the registers the function uses (r0–r15, T, PR, GBR, MACH, MACL and SR's S, Q, M). They are loaded from the context on entry, stored back before every call, tail call and return, and reloaded after a call. Labels are emitted only at addresses something branches to. Literal loads become constants read from the boot file at lift time. Memory goes through `rt::ld8`…`st32` on a base pointer (`recomp/runtime/guest.hpp`); in a build with `QILIN_CHECKED_MEMORY` every access is checked against the regions the host mapped, and the instruction's address is recorded first, so a stray access stops naming it.

**Control flow.**
- A delayed branch reads what it needs (its condition, its target register, PR) before its slot runs, then runs the slot, then branches, as the hardware does. `bt` and `bf`, which have no slot, are a plain `if`.
- A branch to another function is a tail call: `fn_T(c); return;`.
- A call (`bsr`, `bsrf`, `jsr`) sets PR and calls `fn_T` directly when the target is a constant that names a function, or `rt::call` through the sorted function table otherwise. Afterwards it checks that the callee returned to the address after the slot (`c.ret`): a guest that changes its return address stops with *bad return* instead of running on in the wrong place.
- `rts` stores PR in `c.ret` and returns.
- A switch (`braf` through a table) becomes a C++ `switch` on the table's offsets; an offset not in the table stops with *bad jump*. A `jmp` through a table of code addresses becomes a `switch` on the address, the same way.
- The hand-written PR idioms (`cfg.hpp`'s `call_returns` and `jmp_returns`) become a call that continues at the loaded return address, and a return.
- Any other indirect jump is a tail call through the table.

**What stops instead of guessing.** `mac.w` and `mac.l` with S set (no clean source here pins down their saturation), `pref` of a store-queue address (the runtime must take the store queues over), and the run-time checks above. No function contains `trapa`: discovery rejects it, and the image's debugger stubs are replaced natively. A function the lifter can't lift gets a stub that stops as *not lifted*; a function in a replaced region (Shinobi) gets one that stops as *unregistered*, naming its caller, for the runtime's registry to fill. Unreferenced functions are lifted too (since every byte was accounted for; `--no-orphans` skips them): an address the analysis couldn't see being taken still has code behind it.

**Output files.** `functions.hpp` (every declaration), `table.cpp` (the function table), `stubs.cpp`, `lifted_NNN.cpp` with 256 functions each (counted over every function found, so lifting more changes only the chunks that hold them), and `lift_report.txt` with the reason for every function not lifted. A file is rewritten only when its content changes, so a rebuild recompiles only what changed.

**Results on the supported build.** Of 4,307 functions, 796 are Shinobi's (stubs) and 574 are unreferenced (skipped). Of the other 2,937, **1,832 are lifted** (730 in the game, 1,102 in the middleware). 1,105 aren't yet:
- 1,083 use floating point (529 in the game, 554 in the middleware): the FPU slice;
- 21 have privileged instructions, all in the middleware: they face the hardware and are to be replaced;
- 1 in the middleware (`0x8C04EEE0`) has a branch into a delay slot, which needs the slot's instruction duplicated.

The output compiles without a warning under Clang with `-Wall -Wextra -Wpedantic -Wshadow -Werror`, for x86-64 and ARM64, with and without checked memory. GCC and MSVC see the generated code through the end-to-end tests in CI.

**End-to-end tests** (`recomp/tests/lift_programs.hpp`, `lift_gen.cpp`, `lift_run.cpp`). A small assembler builds synthetic SH-4 programs into one boot file image:
- 64-bit addition, subtraction and negation through `addc`, `subc` and `negc`;
- unsigned and signed 32-bit division by SHC's `div0u`/`div0s` and 32 × `rotcl`/`div1` sequences;
- `shad`, `shld`, the rotates through T, `cmp/str`, `xtrct`, `swap.b`, and every multiply;
- a `dt` loop with `bf/s`, and a switch;
- calls by `bsr` and through the literal pool, a tail call by `bra`, an indirect call through a table in data, a call by loaded PR, a call to an address with no function;
- a function with floating point (not lifted: it must stop);
- loads and stores of every size, pre-decrement and post-increment, the GBR forms, `mac.w` and `tas.b`.

At build time `qilin_liftgen` lifts the image with the real analysis and lifter. `qilin_lift_tests` compiles the result with the compiler under test, so CI checks generated code under GCC 14, Clang 19, Apple Clang and MSVC with warnings as errors. It then runs it with checked memory against results computed from the instructions' definitions: 86,286 checks, including thousands of random operands for the divisions, shifts and multiplies, and the stops (a bad jump, a function not lifted, an access outside the mapped regions). Breaking `div1`'s Q update, or one case of a generated switch, makes them fail.

## Literals the code rewrites, and transfers of both sizes (v0.10.0)
SHC's literal pools are constants, and the lifter folds them into the code. The movie decoder's set-up breaks that: it rewrites three offsets in its dispatcher's pool at run time (found by comparing the console's RAM with the boot file). Discovery now marks a literal word as patched when some function loads its address as a constant (a 4-byte literal whose value falls inside the word), and the lifter emits a memory read for PC-relative loads of those. On this build: 23 words, among them the decoder's three; the rest are pointers to pool words that nothing may write, which cost a load each and are still exact. An end-to-end test runs a function that rewrites its own literal.

An FPU transfer (`fmov`) means different things with SZ = 0 and SZ = 1 (one register or a pair; FRn odd is an XD pair). When a function is analysed in contexts that disagree on SZ at an instruction, each context's mode is proven but the instruction's isn't: it is then lifted both ways, choosing by `c.fpscr`'s SZ at run time, which lifted code keeps exact (`fschg`, `lds ...,fpscr`). On this build: 14, all in the movie decoder's block handlers.

## The lifter's FPU slice (v0.6.0)
Floating point is lifted in the mode `fpmode` proves at each instruction. An instruction whose mode isn't proven, or that would round towards zero or run with denormals kept (DN = 0), stops the lift of its function with the reason; none of the game's or the middleware's does.

**Registers.** The current bank's fr0–fr15 live in host `float` locals, declared only for the registers used, and are stored to and reloaded from the context around calls, returns and bank changes. The back bank stays in the context (`c.xf`). A double register DRn is the pair FRn (the high half) and FRn+1. Copies keep a float's bits on every 64-bit host (SSE and NEON moves don't touch them), so `fmov` moves words that aren't numbers, such as signalling NaNs and denormals, unchanged; the end-to-end tests check that on every CI compiler.

**What is lifted:**
- `fadd`, `fsub`, `fmul`, `fdiv`, `fsqrt`, `fcmp/eq`, `fcmp/gt`, `float` and `ftrc` in single and double precision; `fabs` and `fneg` as operations on the sign bit; `fldi0`, `fldi1`; `fcnvsd`, `fcnvds`; `flds`, `fsts` and every FPUL transfer.
- `fmov` with 32-bit transfers, and with 64-bit transfers (SZ = 1) between pairs, including the back bank's XD registers. A 64-bit transfer to or from memory moves FRn at the lower address and FRn+1 at the higher one.
- `fschg`; `frchg`, which stores the bank's locals, swaps the two bank arrays and reloads; `lds` and `lds.l` to FPSCR, which swap the banks when FR changes; `sts` and `sts.l` of FPSCR.

**Denormals and rounding.** The SH-4 with DN = 1 treats a denormal operand as zero and flushes a denormal result to zero. The host does the same once `rt::host_fp_setup()` has set its mode: FTZ and DAZ on x86-64, FZ on ARM64, round to nearest on both. The runtime calls it on every thread that runs lifted code. Generated code is compiled with `-ffp-contract=off` (MSVC doesn't contract by default), so no multiply and add are fused into one rounding. With those, single and double arithmetic is IEEE 754's, which is the SH-4's for these operations.

**What stops:**
- `ftrc` of a value outside the 32-bit integer range, or of a NaN;
- an `lds` to FPSCR that sets an exception enable (the port doesn't model FPU traps).

**What isn't modelled:**
- the bit patterns of NaNs that arithmetic produces (the host's; the tests compare NaNs as a class);
- FPSCR's cause and flag bits, which read as 0. Nothing in the game reads them that the analysis has seen; the differential test is to confirm it.

**Not lifted in v0.6.0:** `fmac`, `fipr`, `ftrv`, `fsca` and `fsrra`, whose exact results no clean source gave. Since v0.7.0 they are lifted with definitions of our own, measured against an emulator (below).

**Results on the supported build.** **2,708 functions are lifted**, 1,193 in the game and 1,515 in the middleware: every function of the game except the 66 that use `fmac` (57), `fsca` (4) or `fsrra` (5). What remains outside Shinobi's stubs and the unreferenced functions:
- 206 functions use those instructions (66 in the game, 140 in the middleware, which also uses `fipr` and `ftrv`);
- 21 have privileged instructions;
- 1 has a branch into a delay slot;
- 1 has `mov.l r15,@-r15`, whose stored value (before or after the decrement) no clean source here pins down.

The output compiles without a warning under Clang for x86-64 and ARM64, with and without checked memory.

**End-to-end tests for the slice:**
- `farith`: single-precision arithmetic, comparisons, `fneg`, `fabs`, `fldi1`. It runs 2,000 random operand sets against the host and exact bit patterns for 1/3, √2, denormal operands, a denormal result and −denormal + 0.
- `ftrc`: `ftrc` and `float` at the range's edges, with stops outside it.
- `fpair`: 64-bit moves to and from memory, DR and XD pairs, `frchg` and FPSCR across a bank change. It moves a signalling NaN and a denormal and checks their bits are kept.
- `fdouble`: `lds` to FPSCR setting PR, then `fcnvsd`, double-precision add, multiply, divide and compare, and `fcnvds`, over 1,000 random pairs; 1/3 is checked exactly.

Swapping an operand order in the lifter, leaving out the host's flush mode, or swapping a pair's halves makes them fail.

## The approximate instructions, measured against an emulator (v0.7.0)
On the hardware, `fmac`, `fsrra`, `fipr`, `ftrv` and `fsca` give results within error bounds, and their exact results aren't documented. No Dreamcast was at hand to measure them on. So the owner asked (2026-11-21) for an emulator to be used for testing: Flycast, the most maintained Dreamcast emulator, run as a black box as the project's rules allow. Nothing of its code or its tables is read or copied. The port's definitions are our own, written from the instructions' descriptions, and the emulator only says which of them agrees with it.

**The tool** (`tools/dcref/`, `qilin_dcref`). `fetch.sh` downloads Flycast's libretro core and libretro's header (MIT) to a folder outside the repository. The tool is a minimal libretro host, about 150 lines. It boots a probe program as an ELF on the emulator's own BIOS replacement, so no Dreamcast BIOS is needed, and gives the core an OpenGL context with no window (EGL's surfaceless platform on Mesa's software renderer). It then reads the results from guest RAM. The probe is built with the end-to-end tests' assembler: one loop per instruction over tens of thousands of inputs (ordinary magnitudes, raw bit patterns, and the edges: zeros, infinities, NaNs, denormals), and all 65,536 angles for `fsca`. Each candidate definition is counted three ways: on all inputs, on ordinary inputs (normal numbers and zeros), and on inputs without denormals. It is a Linux x86-64 developer tool, built with `-DQILIN_DCREF=ON -DQILIN_DCREF_DIR=<folder>`; CI doesn't build it, since the core isn't in the repository.

**What it found** (Flycast nightly of 2026-11-20, 50,000 cases per instruction):

| Instruction | The port's definition | Agrees with the emulator |
|---|---|---|
| `fmac` | one rounding (`fmaf`) | every input without a denormal (47,329); two roundings agree on 94% |
| `fsrra` | `1.0f / sqrtf(x)` (two roundings) | every input without a denormal (49,032); the correctly rounded value agrees on 87% |
| `fipr` | the four products summed in double, rounded once | every input without a denormal (43,077); single-precision sums in any order agree on 85–91% |
| `ftrv` | each row as `fipr`, the matrix column-major in the back bank | every input without a denormal (17,238) |
| `fsca` | the exact sine and cosine, rounded | 18,510 of 65,536 angles: the emulator has a table of its own, at most 2⁻²³ from the exact values |
| `fadd`, `fsub`, `fmul`, `fdiv`, `fsqrt` | IEEE, denormals flushed | every input without a denormal: a check on the slice of v0.6.0 |

**Denormals are the one disagreement.** Hitachi's SH-4 software manual describes DN = 1 as treating a denormalised number as zero, and the port applies that to operands and results alike, as it has since v0.6.0. Flycast flushes results but computes with denormal operands. The port keeps the manual's reading; a console would settle it. The game's code, which runs in DN = 1 throughout, shouldn't care either way, since denormals are 10⁻³⁸ and below.

**`fsca`.** Flycast's results are multiples of 2⁻³⁰ that sit up to 15 units off the true values, which a formula doesn't give: they are a table, whatever its origin. Copying it would bring a GPL table into the port. The port's table is the exact values: the first quarter wave computed in double and rounded, the rest by symmetry, built once at start-up (256 KB). The two differ by at most 2⁻²³, about the hardware's own documented error.

**Also measured:** `mov.l Rn,@-Rn`, with the same register as source and address, stores the register's value from before the decrement. The lifter now lifts it that way. It occurs once, in the middleware.

**Tests** (end to end, every CI compiler):
- the five instructions' wiring (which registers, operand order, the matrix's orientation) against the runtime's definitions over random operands;
- exact values: `fmac` 3 × 4 + 5; `fmac` of (1 + 2⁻¹²)² − 1, which only one rounding gets right; a denormal operand; `fsrra` 4; `fipr` (1, 2, 3, 4) · (5, 6, 7, 8); `ftrv` with a matrix whose transpose gives other results; `fsca` at the quarter points, its symmetry and its accuracy over every 97th angle.

Transposing the matrix, or rounding `fmac` twice, makes them fail.

**Results on the supported build:** **2,916 functions are lifted, every one of the game's 1,259 and 1,657 of the middleware.** The branch into a delay slot (`0x8C04EEE0`) is now lifted too, by copying the slot's instruction under its own label after the function's body, followed by a jump to the instruction after it. That function is the C runtime's `memset`. It clears memory through the SH-4's store queues (`fmov` to `0xE0000040` downwards, then `pref` of a store-queue address) and finishes with a computed jump into an unrolled run of `mov.b` stores, so the lifted code stops there, naming the store-queue access, until the runtime gives it a native replacement. What isn't lifted: Shinobi (796 stubs), the 574 unreferenced functions, and the 21 middleware functions with privileged instructions, which are hardware-facing and are to be replaced.

## The differential test (`recomp/tests/difftest/`, v0.8.0)
As in the sister projects (Punch-Out's `recomp/tests/difftest/` is the model): every instruction the lifter lifts runs from random states, lifted and on a reference interpreter written separately, and everything must agree. It runs in CI on every configuration and needs no game files.

**The cases** (`gen.cpp`, `qilin_difftest_gen`, at build time). Each of the 109 non-privileged, non-branch instruction forms gets 16 random valid encodings; the decoder decides validity. Each encoding is placed in small functions:
- on its own, before `rts`;
- in `rts`'s delay slot (except PC-relative forms and those that touch PR, which the lifter refuses there);
- between two `fschg`, where FPSCR.SZ matters (64-bit transfers);
- between loads of FPSCR that set and clear PR, where precision matters.

Twelve programs cover the branches:
- `bt` and `bf` over an add;
- `bt/s` and `bf/s` with a slot that sets or clears T, so the branch must have read T first;
- `bra` with a slot;
- `bsr` to a callee of its own, with PR saved on the stack;
- `jsr` through the literal pool with T set in the slot;
- a `dt` loop;
- `jsr`, `jmp`, `braf` and `bsrf` to addresses with no function.

The cases go into one synthetic boot file, 64 bytes each, with words that decode as nothing in between. A PC-relative load reads its own case's filler, never another case's code, which analysis would rightly take for data. A table of their addresses lets the real analysis find them as functions (as pointers in data), and the real FPSCR pass and lifter lift them. An encoding the lifter refuses is left out and counted: 221 of them, every one a form the SH-4 leaves undefined (double precision on an odd register, `fldi0`/`fldi1`/`fmac`/`fipr`/`fsrra`/`fsca`/`ftrv` in double precision, `fcnvds`/`fcnvsd` in single) or one with the same register as address and data, which the lifter stops on by design. That leaves 5,052 cases and 5,055 functions, written as C++ for the compiler under test.

**The reference** (`ref.cpp`). It shares only the decoder's identification of the opcode with the lifter:
- it works from the raw word's fields, with memory read and written byte by byte;
- `div1` is written from its definition as a 33-bit result, `shad`/`shld` from theirs;
- the FPU computes in double precision with the DN = 1 flush done by hand, so it doesn't depend on the host's floating-point mode, which the lifted side sets with `rt::host_fp_setup()`;
- branches run with their delay slots, calls and returns through a call stack, and indirect branches to addresses with no function stop as *bad jump*.

For the five approximate instructions it uses the port's own definitions (measured against the emulator, above), so what it checks there is the wiring.

**The run** (`run.cpp`, `qilin_difftest`). Each case runs 48 times from random states:
- registers mix random values with the edges (0, 1, 31, 32, INT_MIN, INT_MAX, −1 …);
- the FPU registers mix ordinary magnitudes, infinities, both kinds of NaN, denormals, the single-precision limits and raw bit patterns;
- S is set in a quarter of the runs (so `mac` must stop);
- memory operands are steered into a 16 KB window, aligned to their access, and the window is filled at random.

The lifted code runs with checked memory. These must all agree:
- r0–r15, PR, GBR, MACH, MACL, T, S, Q, M, FPUL, FPSCR, and both banks bit for bit (NaNs from arithmetic as a class, since their bits are the host's);
- where the code returned to;
- every stop (kind, instruction, value);
- the whole window.

**Results: 242,496 runs, 0 mismatches**, 4,035 of them ending at a stop both sides make (bad jumps, `mac` with S set, `ftrc` out of range, FPSCR loads with exception enables), in about 35 s. Planted bugs fail at once: `subc` with its operands swapped (1,440 mismatches), `fcmp/gt` reversed (1,343), `bt/s` reading T after its slot (41).

**What it found:**
- The reference was wrong once, and the lifter right: a call sets PR even when no function is at its target.
- The lifter emitted `r13 = r13;`, `t = r15 > r15;` and the like for encodings with one register as both operands, which warnings-as-errors rejects. The game never uses them; such forms now become their known results.

## Unit tests (`recomp/tests/tests.cpp`)
No game files: SHA-256 vectors; boot files (good, empty, odd, a byte too large for RAM); 60 instruction words pinned to their text, 36 words that must not decode, every form's canonical word and its word with all field bits set, the count of valid words and of delayed branches; function discovery on synthetic programs (calls through the pool and `bsr`, a switch with and without its bound, constants that agree and differ at a join, pointers to code and to data, data that would run on into code, orphans, a branch in a delay slot, a branch into one, running off the image; v0.3.0: constants through arithmetic, a float reached through `mova`, tail calls by `bra` to a prologue and to an entry found by a call, a layout's data region and coverage gated by it; v0.4.0: a call by loaded PR and a return through the saved PR, a scratch register kept across a callee that doesn't write it and lost across one that does, successors through delay slots) and the registers each instruction writes; the hardware region map and the profile of a small program, and access sizes; the supported build's layout covering its image in order; the FPSCR pass (`fschg` pairs, the read-modify-write into double precision, a save and restore on the stack, a callee analysed in its caller's mode, a join in different modes, an indirect call in another mode, a bank switch, a function called only from replaced code (v0.6.0), and composing and meeting known bits); a synthetic five-track GD-ROM written to a scratch folder (pregaps, a file in the last track, a file running into an audio track, a sector of the wrong mode) and 12 malformed cue sheets, a missing cue and a truncated track file.

## Three rules from Toy Racer
ToyRacer--QilinRecomp started from this core, and its boot file needed three things this one hadn't shown. They are general, so they are here too, with their tests (its `docs/recompiler-core.md` has them in full):

- **Far branches inside long functions.** A function too long for `bra` branches inside itself with `mov.l label,rN ; jmp @rN`. A `jmp` to a constant address that the function's other paths reach too is a branch inside it (`Function::computed_branches`), lifted as a `goto`, not a tail call. Here: 6 of them in `0x8C090B1C` (2,427 instructions). Two of its labels, `0x8C0921C8` and `0x8C0921E0`, had been taken for functions, so the build now has **4,305 functions** (302 tail-call targets), 689 constant tail calls and **2,932 lifted**. Coverage, the suspects list and every other lifted function are unchanged, and `qilin_run` stops where it did.
- **Guesses a real function contains are thrown out** (`Analysis::find_evictions`): a data word that holds, by coincidence, the address of the middle of a function. None in this build.
- **A branch to itself stops loudly.** `bra` to its own address is the code's stop on a fatal error; it is lifted as `rt::hang` (a stop of its own kind), not as an empty loop, which a C++ compiler may delete. One here, at `0x8C087AD0`.

The counts earlier in this file are as they were when each section was written.

## Three more rules from Toy Racer (2026-11-22)
Bringing Toy Racer's boot file to no unknown bytes (`docs/toyracer/code-analysis.md`, "Every byte") changed discovery and the mode pass in three general ways; Toy Commander's results are unchanged by all three (its lift output is identical):
- **A layout's entry is always tried as such**, even where a pointer guess at the same address failed first (a guess is tried only once). The layout knows an entry may run on into its neighbour; the guess doesn't.
- **Filler before an aligned loop head**: zero or `nop` words fewer than 32 bytes that end on a 32-byte boundary are padding, whatever their length below that (before, 16 bytes at most).
- **A branch into a delay slot** is followed by `successors()` as a node of its own, the slot's address with its low bit set (instructions are 2-byte aligned, so no instruction has that address): the slot's instruction runs on its own, then the one after it. The FPSCR mode pass decodes `pc & ~1` and gives the instruction the meet of both paths' modes. The lifter already emitted such a slot as a plain copy. A unit test has one, and fails if the walk treats the slot only as its branch's.

## What comes next
In the order the sister projects took:

1. **Whole call trees** in the differential test, under UBSan too, once the runtime can run the game's functions.
2. **The runtime** (roadmap step 3): `qilin_run`, guest RAM at its aliases, the registry of native replacements (the store-queue `memset` among the first), stopping on anything unregistered and naming it.
