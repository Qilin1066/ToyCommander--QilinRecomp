# Code analysis: the boot file's regions, its libraries and the lifted/replaced boundary

What is where in `1GUTH.BIN`, how each boundary was established, and what follows for the lifter and the runtime. Everything here was measured with `qilin` on the supported build (USA, `MK-57020`, `V1.022`; v0.3.0). The layout is recorded in `recomp/src/layout.cpp`; function discovery takes it as input and fails loudly where it disagrees (no function can be found in a data region, and a call into one is reported).

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

**What was tried:** labelling from the anchors and spreading labels along call edges, weighted against shared callees. The result is not usable: the engine calls Kamui 2, the audio manager and the C runtime throughout, so labels bleed across objects, and the runs it produces don't line up with the anchors. It is not used.

**What comes next**, in order of the evidence's strength:
1. Object boundaries from the data: each object's strings and constants are laid out in the same order as its code, so consecutive functions whose references fall in the same stretch of the data segment belong to the same object.
2. The API the game calls: the 278 middleware functions the game calls directly, each with its callees, strings and hardware profile. Kamui 2's and the audio manager's entry points are among them.
3. Behaviour: what reaches the PowerVR2, the sound chip and the DMA controller (below), and through which globals.
4. Names from Sega's published function names for Kamui 2, Shinobi and the audio manager, matched by behaviour. Function-ID databases built from Sega's libraries are not used unless their licence allows it (`prior-art.md`).

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
143 jumps have no constant target and no switch table (`qilin suspects`):

| Where | Count | What |
|---|---|---|
| game | 3 | indirect tail calls: two are the script interpreter dispatching an opcode through a table of handlers (`0x8C01C114`, `0x8C01CA58`), one calls a function pointer held in an object at offset 84 (`0x8C0446E0`) |
| middleware | 60 | to classify with the attribution |
| Shinobi | 80 | replaced: not lifted |

The game's three need no more analysis: as in the sister projects, an indirect tail call goes through the generated function table and traps on an address it doesn't have. The interpreter's handler tables are in the data segment; their entries are among the functions found from pointers.

Other findings that bear on the lifter: two functions with a call whose target register differs between paths (`0x8C075B54`, `0x8C075ED0`) and two in Shinobi (`0x8C0B08D8`, `0x8C0B80FE`), left as indirect calls; two hand-written copy routines that branch into a delay slot (`0x8C04EDF4`, `0x8C04EEE0`), which will be replaced natively, not lifted.

## The runtime's to-do list (v0.3.0)
1. **Shinobi:** replaced whole, at its API: start-up, interrupts, timers, cache, store queues, memory allocation, the GD-ROM file system (over the disc image), Maple (controllers, memory cards, the Jump Pack), the system configuration, the real-time clock.
2. **The middleware's hardware-facing parts:** Kamui 2 and `kmdk` (to Vulkan), the audio manager (to the native mixer; `AUDIO64.DRV` is not run), the DMA and store-queue copy routines. Which functions exactly is the attribution's result.
3. **Lifted:** the game, and the middleware's pure computation: No Cliché's engine, the movie player's decoder, the C runtime and maths, as far as the attribution confirms.
4. **The development system's leftovers** (CodeScape file names, the debugger stubs): replaced by no-ops, with a log line if one is ever reached.
