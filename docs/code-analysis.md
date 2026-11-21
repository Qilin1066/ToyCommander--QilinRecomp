# Code analysis: the boot file's regions, its libraries and the lifted/replaced boundary

What is where in `1GUTH.BIN`, how each boundary was established, and what follows for the lifter and the runtime. Everything here was measured with `qilin` on the supported build (USA, `MK-57020`, `V1.022`; v0.3.0, extended in v0.4.0). The layout is recorded in `recomp/src/layout.cpp`; function discovery takes it as input and fails loudly where it disagrees (no function can be found in a data region, and a call into one is reported).

## The regions

| Range | Plan | What | Evidence |
|---|---|---|---|
| `8C010000`–`8C010030` | replace | start-up: sets the cache control register, jumps to `0x8C0B0340` | the entry point; the runtime sets up the CPU itself |
| `8C010030`–`8C010080` | data | two marker strings, "Lib Handle Start" and "Lib Handle End" | nothing references them; Shinobi's start-up uses its own copy at `0x8C0C58A0` |
| `8C010080`–`8C0495B8` | **lift** | the game: No Cliché's code for Toy Commander | below |
| `8C0495B8`–`8C09F5B6` | undecided | the middleware: No Cliché's engine, the audio manager, Kamui 2, `kmdk`, the C runtime | below |
| `8C09F5B6`–`8C0B0340` | data | initialised data of the game and the middleware (strings, tables, function-pointer tables) | from the end of the last function below it to the entry's jump target; no function is found in it |
| `8C0B0340`–`8C0C5690` | replace | Shinobi, Sega's system library, and its start-up | its version strings name 25 parts (`disc-analysis.md`); the entry jumps to its first function |
| `8C0BAFDC`–`8C0BCFE0` | data | an 8 KB zero-filled buffer inside Shinobi's code | six literal pools in the middleware hold `0x8C0BAFE0`, its 32-byte-aligned start |
| `8C0C1C0C`–`8C0C3C24` | data | an 8 KB zero-filled stack | the only references are to its top (`0x8C0C3C20`–`0x8C0C3C28`) |
| `8C0C3CA4`–`8C0C3D04` | data | the data of Shinobi's exception handler, which follows it | referenced from the handler and from `0x8C088C64` |
| `8C0C5690`–`8C0C6B9C` | data | Shinobi's initialised data and version strings | follows its last function |

Shinobi's code is split into four rows in `layout.cpp` around the three data blocks.

### The game's boundary: `0x8C0495B8`
Library code never calls the game; the game calls the libraries. So the game ends at the first address from which no code above calls below:

- From `0x8C0495B8` up to the end of the middleware, **no direct call or constant tail jump lands below `0x8C0495B8`.** No lower function entry above `0x8C010080` has that property. It holds again at the eleven entries from `0x8C0495B8` to `0x8C049C04` and the eight from `0x8C049D82` to `0x8C049E24` (between them, a few middleware functions call their neighbours below), so `0x8C0495B8` is the lowest clean split: the boundary is taken there, after the last game function (`0x8C049550`, ending at `0x8C0495A2`, then padding).
- The other way, the game makes 1,641 direct calls into 278 distinct middleware functions.
- The game's own source files, named in its assertion strings (`loadscene.c`, `Menu.c`, `ressGhq.c`, `tabtree.c`), are all referenced from below the boundary. The engine's (`Anim.c`) are referenced from above it.
- Shinobi's start-up makes exactly one call into the game: `0x8C0B0360` calls `0x8C02796A`, the function that prints the script interpreter's banner. That is the game's `main`.
- The game calls Shinobi directly from only 3 sites (2 functions): it reaches the system through the middleware.

The game region is **lifted**, and its coverage is a gate: **0 unknown bytes** (`qilin coverage` exits 3 otherwise).

### The middleware: `0x8C0495B8`–`0x8C09F5B6`
Everything here is called by the game and calls nothing in it. It is not laid out library by library. The anchors, functions identified by the strings they reference (the audio manager's error messages name the function they are in):

| Address | Anchor |
|---|---|
| `0x8C04F8F6`, `0x8C050046` | `Anim.c` (No Cliché's engine: animation loading) |
| `0x8C0575FC` | the engine's vertex-list overflow and screenshot messages ("…Save it under CodeScape") |
| `0x8C068420`–`0x8C06AD60` | the audio manager: `acDigi*`, `acMidi*`, `acDsp*`, `acInt*`, `amHeap*`, `amInit`, `amSelectDriver`, `amFile*` (96 functions reference such a message) |
| `0x8C06BE68` | Kamui 2's copyright and version string (`16,3,0,0`) |
| `0x8C06D60C` | "Init Movie Sound Failed" (the movie player) |
| `0x8C06F036` | `stdin`, `stdout`, `stderr` (the C runtime's streams) |
| `0x8C0734A8`, `0x8C07353E` | "Kamui V%d", "FPS %02d" |
| `0x8C0815C0`–`0x8C081A72` | the development system's file names (`GINXFER.BIN`, `\MANIFEST\MANIFEST.BIN`, `\1ST_READ.BIN`) |
| `0x8C082F34`–`0x8C08424C` | the audio manager again: its error reporting, `acSystem*`, `_amStreamIo*`, `_amVoice*`, `acG2*`, `_acUtilMemCpy` |
| `0x8C094B04`–`0x8C094CD2` | the audio manager's `acDma*` |
| `0x8C0955B4` | `kmdk`'s copyright and version string (`KAMUI-Darkness`, `1,3,0,0`) |

The audio manager alone appears in three places with Kamui 2, the C runtime and engine code between them. Hitachi's linker places object files in the order it resolves them, so libraries interleave object by object, and attribution has to be per function (in practice per object file), not by range.

**What was tried** (v0.3.0, v0.4.0):
- *Labelling from the anchors and spreading labels along call edges*, weighted against shared callees. Not usable: the engine calls Kamui 2, the audio manager and the C runtime throughout, so labels bleed across objects.
- *The order of the constants.* The constant section (strings and tables, `0x8C09F5B6` to about `0x8C0A3A00`) follows the code's link order: of the 205 functions that reference a string, 194 of 204 consecutive pairs reference strings in rising order, and the 10 exceptions are within one object (shared strings). So it confirms an order, but too few functions reference strings to place every boundary.
- *Padding.* The 2-byte zero fill between functions is SHC aligning literal pools, inside objects as much as between them: it marks nothing.
- *Shared global variables*, the strongest signal. Of the 1,467 variables the code references through literal pools (initialised data, and the zero-initialised area above the image), 715 are used only by the middleware, 460 only by the game and 230 only by Shinobi; just 61 are shared by the game and the middleware, and 1 by the middleware and Shinobi. Grouping middleware functions that share a variable gives 114 groups: the audio manager's core is one of 39 functions (`acDigi*`, `acSystem*`, `acG2*`, across its three places in the code), and Kamui 2's anchor sits in a group of 56 spread over `0x8C04C050`–`0x8C0970AC`, the whole middleware. So the libraries interleave at a finer grain than whole object files, and these groups name libraries but don't give a complete per-function map.

**Decision (v0.4.0): the attribution is driven by bring-up**, not completed statically first. The middleware is lifted with the game, in a build whose checked memory stops on any access to hardware it doesn't handle, naming the instruction and the function (`roadmap-and-design-targets.md`). Each stop names a function that must be replaced, or one whose hardware access the runtime takes over; its callers show where the library's API is. That is the bring-up loop the sister projects used, and it finds exactly the functions the game reaches, in the order it reaches them. The static evidence above (anchors, variable groups, the hardware profile) is used to name what each stop belongs to and to choose the level of the API to replace. What is never done: emulating the PowerVR2 or the sound chip under an unreplaced library.

1,968 bytes of the middleware are still unknown: tables of jump stubs (`0x8C081C40`, `0x8C0938E4`), byte tables (`0x8C09D194`), 0x0900 filler words inside one function at `0x8C081400`, and the debugger stubs at `0x8C099470` (`disc-analysis.md`). They are resolved with the attribution, and the middleware's coverage becomes a gate for whatever of it is lifted.

## Hardware access
`qilin hwaccess` lists the loads, stores and cache operations at constant addresses outside cached RAM (the region map is `recomp/src/hwaccess.cpp`'s `hw_region`, from the SH7750 hardware manual's and the Dreamcast's memory maps). **76 functions** do:

| Where | Functions | What they reach |
|---|---|---|
| start-up | 1 | the cache control register |
| game | **0** | nothing |
| middleware | 30 | the system bus's DMA and interrupt registers (the audio manager's G2 transfers, `0x8C083C3C`–`0x8C08424C`), the SH-4's timers (`0x8C086BD4`–`0x8C089184`), the SH-4's DMA controller with the PowerVR2's DMA registers (`0x8C094DE4`, `0x8C09595C`, next to `kmdk`), the PowerVR2's registers (`0x8C056E68`), sound memory (`0x8C04A0FA`), the store queues (`0x8C04EDF4`, `0x8C04EEE0`: the two hand-written copy routines) |
| Shinobi | 45 | timers, the interrupt controller, cache control, the system bus registers, the PowerVR2's registers, the bus state controller, the clock controller, the serial ports, the G2 bus |

So little reaches hardware at a constant address: Kamui 2 in particular keeps its register bases in variables, and accesses through pointers loaded from memory aren't seen by this pass. The runtime's checked memory build stops on any access outside RAM that it doesn't handle, naming the instruction, which covers what this static pass can't see.

What it shows already: **the game itself never touches hardware.** Replacing the middleware's hardware-facing parts and Shinobi is enough for the game's own code to run lifted, unchanged.

## Indirect jumps
127 jumps have no constant target and no switch table (`qilin suspects`; 143 before v0.4.0):

| Where | Count | What |
|---|---|---|
| game | 3 | indirect tail calls: two are the script interpreter dispatching an opcode through a table of handlers (`0x8C01C114`, `0x8C01CA58`), one calls a function pointer held in an object at offset 84 (`0x8C0446E0`) |
| middleware | 44 | to classify during bring-up |
| Shinobi | 80 | replaced: not lifted |

The game's three need no more analysis: as in the sister projects, an indirect tail call goes through the generated function table and traps on an address it doesn't have. The interpreter's handler tables are in the data segment; their entries are among the functions found from pointers.

Since v0.4.0 the CFG tracks PR, and 18 of what were indirect jumps are returns (`jmp @r2` through the saved return address) and 2 are calls made by loading PR by hand (`lds r1,pr ; jmp @r0`, returning where r1 points), in the hand-written matrix library. That leaves 127: 3 in the game (above), 44 in the middleware, 80 in Shinobi.

Other findings that bear on the lifter: two functions with a call whose target register differs between paths (`0x8C075B54`, `0x8C075ED0`) and two in Shinobi (`0x8C0B08D8`, `0x8C0B80FE`), left as indirect calls; two hand-written copy routines that branch into a delay slot (`0x8C04EDF4`, `0x8C04EEE0`), which will be replaced natively, not lifted.

## The FPU's modes (v0.4.0)
`qilin fpmode` works out FPSCR's mode bits at every FPU instruction (`recompiler-core.md`). On the supported build (counts as of v0.6.0, which also analyses the functions reached only from Shinobi's start-up, such as `main`; v0.4.0 missed them):

| Region | Functions with FPU code | FPU instructions | Unproven | Double precision | 64-bit transfers | In bank 1 |
|---|---|---|---|---|---|---|
| game | 547 | 14,669 | **0** | 0 | 0 | 0 |
| middleware | 700 | 21,290 | **0** | 36 | 214 | 209 |

The conventions it found:
- **The default mode is `0x00040000`:** round to nearest, **denormals flushed to zero (DN = 1)**, single precision, 32-bit transfers, bank 0. Shinobi's start-up loads it (`mov #4,r0 ; shll16 r0 ; lds r0,fpscr`), and so does its exception handler. The lifted code must flush denormals to zero like the hardware does, in every arithmetic operation.
- **The game runs entirely in the default mode,** at every one of its FPU instructions.
- **The hand-written matrix library** (around `0x8C0613D4`–`0x8C061EEC`) switches banks. It loads matrices into the back bank for `ftrv`, and some of its entry points return in the other bank: `0x8C06166C`, entered in bank 0, returns in bank 1 (it enters its own callee with the bank flipped, by an `frchg` in a call's delay slot). The engine then runs in bank 1 for a while, which compiled code doesn't notice: it only ever names "the current fr0–fr15". So the bank isn't something the lifter needs to know: it keeps the two banks as two arrays and `frchg` swaps them.
- **Double precision** is used by the C runtime's conversion helpers (`0x8C09C768` and those it calls), which set PR themselves with `sts fpscr ; or ; lds` and return with it set. Three of them have paths that rejoin with PR differing (`0x8C09411A`, `0x8C09C902`, `0x8C09CA38`); no FPU instruction that depends on PR follows before PR is set again, so nothing is unproven, but the lifter has to treat FPSCR there as a run-time value.
- **64-bit transfers** (`fschg` around `fmov` pairs) are used throughout the middleware for fast copies.
- **Open:** two callers (`0x8C09DE06`, `0x8C09E0FE`) turn on 64-bit transfers and call a dispatcher (`0x8C09F2AC`) that calls through a table indexed by data (`jsr @r6` at `0x8C09F2F6`). The handlers it reaches run with SZ = 1, but are analysed in the default mode, since they're only known as address-taken functions. Until the table is found, the lifter must read SZ at run time in the `fmov`s of functions that can be reached through it.

## The runtime's to-do list (v0.3.0)
1. **Shinobi:** replaced whole, at its API: start-up, interrupts, timers, cache, store queues, memory allocation, the GD-ROM file system (over the disc image), Maple (controllers, memory cards, the Jump Pack), the system configuration, the real-time clock.
2. **The middleware's hardware-facing parts:** Kamui 2 and `kmdk` (to Vulkan), the audio manager (to the native mixer; `AUDIO64.DRV` is not run), the DMA and store-queue copy routines. Which functions exactly is the attribution's result.
3. **Lifted:** the game, and the middleware's pure computation: No Cliché's engine, the movie player's decoder, the C runtime and maths, as far as the attribution confirms.
4. **The development system's leftovers** (CodeScape file names, the debugger stubs): replaced by no-ops, with a log line if one is ever reached.

## The store-queue memset (v0.7.0)
`0x8C04EEE0` is the C runtime's `memset`, hand-written. Byte stores reach a 32-byte boundary. Then each 32 bytes are cleared through the SH-4's store queues: zeros are written with `fmov` (64-bit transfers) downwards from `0xE0000040`, the address is turned into a store-queue address by XOR with `0x6C000000`, and `pref` sends the queue to memory. It ends with a computed jump into an unrolled run of `mov.b` stores. `qilin hwaccess` doesn't list it, since its store-queue addresses are computed. The game calls it, so it is among the first native replacements the runtime needs. Until then the lifted code stops at its first store-queue store, naming it.

