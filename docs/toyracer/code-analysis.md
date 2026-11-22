# Code analysis: the boot file's regions, its libraries and the lifted/replaced boundary

What is where in `1TOYRACE.BIN`, how each boundary was established, and what follows for the lifter and the runtime. Everything here was measured with `qilin` on the supported build (Europe, `MK-5114950`, `V1.050`; v0.1.0). The layout is recorded in `recomp/src/layout.cpp`; function discovery takes it as input and fails loudly where it disagrees (no function can be found in a data region, and a call into one is reported).

The method is Toy Commander's (ToyCommander--QilinRecomp, `docs/code-analysis.md`), and the two games share an engine, so much of this reads the same. What differs is said where it differs.

## The regions

| Range | Plan | What | Evidence |
|---|---|---|---|
| `8C010000`–`8C010030` | replace | start-up: sets the cache control register, jumps to `0x8C0C5440` | the entry point; the runtime sets up the CPU itself |
| `8C010030`–`8C010080` | data | two marker strings, "Lib Handle Start" and "Lib Handle End" | nothing references them; Shinobi has its own copy at `0x8C0F9DCC` |
| `8C010080`–`8C04CA58` | **lift** | the game: No Cliché's code for Toy Racer | below |
| `8C04CA58`–`8C0A5B02` | undecided | the middleware: No Cliché's engine, the audio manager, Kamui 2, `kmdk`, the network libraries, the C runtime | below |
| `8C0A5B02`–`8C0C5440` | data | initialised data of the game and the middleware (strings, tables, function-pointer tables) | from the end of the last function below it to the entry's jump target; no function is found in it |
| `8C0C5440`–`8C0F9BB4` | replace | Shinobi, Sega's system library, with its start-up, and Sega's modem library | the entry jumps to its first function; its version strings name the parts (`disc-analysis.md`) |
| `8C0F9BB4`–`8C0FD624` | data | Shinobi's and the modem library's initialised data and version strings | follows the last function |

Unlike Toy Commander's, Shinobi's code here has no data blocks inside it: the region is one run of code, with 92 bytes not yet accounted for.

### The game's boundary: `0x8C04CA58`
Library code doesn't call the game; the game calls the libraries. So the game ends at the first address from which no code above calls below.

- From `0x8C04CA58` up to the end of the middleware, **no direct call or constant tail jump lands below `0x8C04CA58`, with one exception** (next point). No lower function entry above `0x8C010080` has that property, so the boundary is taken there, after the last game function (`0x8C04C9F6`, ending at `0x8C04CA58`).
- **The exception: the authentication module calls back.** Ten functions at `0x8C057B82`–`0x8C0589F6` make 11 calls to four functions at the very start of the game (`0x8C010930`, `0x8C010ABE`, `0x8C010B00`, `0x8C010B82`). The callers are the module that talks to the publisher's authentication server (its functions reference the server's name and the protocol's message tags); the callees sit at the start of the game, next to the functions that hold the lobby's strings. So this is a game-level network module that was linked among the libraries, calling the game's own helpers. It doesn't move the boundary: it is the only code above it that reaches below, and it belongs to the online mode, which the port leaves out (`roadmap-and-design-targets.md`).
- The other way, the game makes 2,187 direct calls into 305 distinct middleware functions.
- The game's own source files, named in its assertion strings (`common.c`, `loadscene.c`, `object.c`, `ressGhq.c`, `tabtree.c`, `trmenu.c`, `trmenutexts.c`), are all referenced from below the boundary: 18 functions between `0x8C0146B4` and `0x8C046A78`. The engine's (`Anim.c`) is referenced from above it. `loadscene.c`, `ressGhq.c` and `tabtree.c` are Toy Commander's file names too.
- Shinobi calls the game from two places: its start-up (`0x8C0C5460`) calls `0x8C0204E4`, the function that references the game's name, which is the game's `main`; and `0x8C0C7CF0` calls `0x8C011882`, which is an empty function (`rts`), a hook the game leaves empty.
- The game calls Shinobi and the modem library directly from 29 sites in 23 functions, reaching 16 functions (Toy Commander's did from 3 sites). Among them are functions that reference the flash memory's signature and the one that names the browser's loader.

The game region is **lifted**, and its coverage is a gate: **0 unknown bytes** (`qilin coverage` exits 3 otherwise).

### The middleware: `0x8C04CA58`–`0x8C0A5B02`
Everything here is called by the game and, the authentication module apart, calls nothing in it. As in Toy Commander it is not laid out library by library. The anchors, functions identified by the strings they reference:

| Address | Anchor |
|---|---|
| `0x8C04CDFA` | "%u bps", a connection-speed message |
| `0x8C04D758`–`0x8C04E568` | most of the 27 functions Shinobi and the modem library call down to (310 calls): small routines, presumably the C runtime's, and `0x8C04D758`, which writes a timer register |
| `0x8C057954`–`0x8C058678` | the authentication module (17 functions reference its message tags or the server's name) |
| `0x8C05E01C`, `0x8C05E108` | the two hand-written routines that use the store queues (below) |
| `0x8C05ED7E`, `0x8C05F496` | `Anim.c` (No Cliché's engine: animation loading) |
| `0x8C066A30` | the engine's screenshot message ("…Save it under CodeScape") |
| `0x8C07FA3E`–`0x8C0827D4` | the audio manager: `acDigi*`, `acMidi*`, `acDsp*`, `acCd*`, `acSystem*`, `a64File*` (with `0x8C0A3CA4`–`0x8C0A3E90`: 69 functions reference such a message) |
| `0x8C084612` | `stdin`, `stdout` (the C runtime's streams) |
| `0x8C088AAC`, `0x8C088B42` | "Kamui V%d", "FPS %02d" |
| `0x8C08EDEA` | the development system's file names (`GINXFER.BIN`, `\MANIFEST\MANIFEST.BIN`, `\1ST_READ.BIN`) |
| `0x8C0A1CA4`–`0x8C0A586C` | the audio manager's upper layer: `amInit`, `amHeap*`, `amSound*`, `_amVoice*` (69 functions) |

Not anchored yet: Kamui 2 and `kmdk` (nothing references their version strings), and NexGen's TCP/IP stack, whose strings are few. The serial port's registers are reached from the middleware 92 times and the timers' 99 times (`qilin hwaccess`), which is where the stack's serial and timing code will be found.

**Decision, as for Toy Commander: the attribution is driven by bring-up**, not completed statically first. The middleware is lifted with the game, in a build whose checked memory stops on any access to hardware it doesn't handle, naming the instruction and the function. Each stop names a function that must be replaced, or one whose hardware access the runtime takes over; its callers show where the library's API is. Toy Commander's notes record what was tried before that decision (labels spread along call edges, the order of the constants, padding, shared global variables) and why none of them gives a per-function map; they are not repeated here. What is never done: emulating the PowerVR2, the sound chip or the modem under an unreplaced library.

2,564 bytes of the middleware are still unknown: a table of jump stubs (`0x8C06E4EC`, 920 bytes), byte and word tables (`0x8C06EBEA`, `0x8C075CB0`, `0x8C04DE58`, `0x8C05E190`), and the area around the debugger stubs at `0x8C09A514` (below). They are resolved with the attribution, and the middleware's coverage becomes a gate for whatever of it is lifted.

## Two things this build has that Toy Commander's didn't show
Both were found because the game's coverage gate failed at first (180 unknown bytes in the game, 9,588 in the middleware). Both are now rules of function discovery (`recompiler-core.md`), with unit tests.

- **A data word that holds a code address by coincidence.** A table in the data region (it reads like code for another processor) has the value `0x8C024820` at `0x8C0B9D70`. That address is the middle of a game function nothing refers to (`0x8C02477C`), and the code from there to the function's `rts` decodes, so it was taken for a function, and the real one was then rejected for running on into it. Discovery now throws such a guess out when a function with a compiled prologue contains it and nothing else supports it.
- **Far branches inside long functions.** Two middleware functions are too long for `bra` to span: `0x8C052528` (3,292 instructions, 7 KB, unoptimised code) and `0x8C09C09A` (2,427 instructions). SHC then branches with `mov.l label,rN ; jmp @rN`: 22 such branches between them. Discovery used to read each as a tail call to a function at the label, which isn't one. A `jmp` to a constant address that the function's other paths reach too is now a branch inside it, and the lifter emits a `goto`.

A third, small one: three places branch to themselves (`0x8C01832A` in the game, `0x8C057C66` and `0x8C0926EC` in the middleware), the code's way of stopping for good on a fatal error. The lifted code stops there loudly (`rt::hang`) instead of spinning.

## Hardware access
`qilin hwaccess` lists the loads, stores and cache operations at constant addresses outside cached RAM (the region map is `recomp/src/hwaccess.cpp`'s `hw_region`). **96 functions** do:

| Where | Functions | What they reach |
|---|---|---|
| start-up | 1 | the cache control register |
| game | **0** | nothing |
| middleware | 54 | the SH-4's timers and its serial port with FIFO (the network stack's timing and serial PPP), the interrupt controller, the SH-4's DMA controller with the PowerVR2's DMA registers, the system bus's DMA and interrupt registers and the G2 bus (the audio manager's transfers), the PowerVR2's registers, cache control, the store queues |
| Shinobi and the modem library | 41 | cache control, the system bus registers, the PowerVR2's registers, the interrupt controller, the clock controller, the serial ports, the G2 bus, the modem |

As in Toy Commander, little reaches hardware at a constant address: Kamui 2 keeps its register bases in variables, and accesses through pointers loaded from memory aren't seen by this pass. The runtime's checked memory build stops on any access outside RAM that it doesn't handle, naming the instruction.

What it shows already: **the game itself never touches hardware.** Replacing the middleware's hardware-facing parts and Shinobi is enough for the game's own code to run lifted, unchanged.

## Indirect jumps
126 jumps have no constant target and no switch table (`qilin suspects`):

| Where | Count | What |
|---|---|---|
| game | 16 | indirect tail calls: a function pointer loaded from an object or a table, jumped to after the epilogue. Two dispatch through a table indexed by a value less 106 (`0x8C01DE42`, `0x8C01E2A0`) |
| middleware | 26 | to classify during bring-up |
| Shinobi and the modem library | 84 | replaced: not lifted |

The game's need no more analysis: as in the sister projects, an indirect tail call goes through the generated function table and traps on an address it doesn't have.

The CFG tracks PR, as it has since Toy Commander's v0.4.0: 18 jumps are returns (`jmp @r2` through the saved return address) and 2 are calls made by loading PR by hand, in the hand-written matrix library around `0x8C06E2FC`–`0x8C06EC8C`. Toy Commander's has the same counts.

Other findings that bear on the lifter:
- **Calls whose target register differs between paths:** four in the game (`0x8C019552`, `0x8C033AC6`, `0x8C035CEA`, `0x8C0362C6`), two in one middleware function (`0x8C095734`), three in Shinobi. Left as indirect calls.
- **Branches into a delay slot:** the two store-queue routines (`0x8C05E01C`, `0x8C05E108`) and `0x8C06F104`. The lifter handles them by copying the slot's instruction.
- **`0x8C0A1DD4` is called but isn't an SH-4 instruction in the decoder's set:** its first word is `0x003B`, as at Toy Commander's `0x8C099470`. These are the development system's debugger hooks; they are replaced natively (as no-ops or logging), not lifted.

## The FPU's modes
`qilin fpmode` works out FPSCR's mode bits at every FPU instruction (`recompiler-core.md`). On the supported build:

| Region | Functions with FPU code | FPU instructions | Unproven | Double precision | 64-bit transfers | In bank 1 |
|---|---|---|---|---|---|---|
| game | 421 | 16,496 | **0** | 0 | 0 | 0 |
| middleware | 598 | 16,947 | **0** | 36 | 214 | 246 |

- **The default mode is `0x00040000`**, as in Toy Commander: round to nearest, **denormals flushed to zero (DN = 1)**, single precision, 32-bit transfers, bank 0. Shinobi loads it with the same `mov #4,r0 ; shll16 r0 ; lds r0,fpscr` (at `0x8C0D2B10`, and four more times near its exception handler's register dump).
- **The game runs entirely in the default mode,** at every one of its FPU instructions.
- **The matrix library switches banks**, and the engine runs in bank 1 after some of its calls, so the lifter keeps the two banks as two arrays that `frchg` swaps (`recompiler-core.md`).
- **Double precision** is used by the C runtime's conversion helpers. Three of them have paths that rejoin with PR differing (`0x8C0974DC`, `0x8C0A20C6`, `0x8C0A2244`); no FPU instruction that depends on PR follows before PR is set again, so nothing is unproven.
- Toy Commander's open point (a dispatcher that calls table handlers with 64-bit transfers on) doesn't arise here: the pass reports no indirect call in a mode other than the default.

## The store-queue routines
`0x8C05E108` clears memory (r4: address, r5: byte count), hand-written: byte stores reach a 32-byte boundary, then each 32 bytes are cleared through the SH-4's store queues (zeros written with 64-bit `fmov` downwards from `0xE0000040`, the address turned into a store-queue address, `pref` sending the queue to memory). The game calls it from 103 places and the middleware from 48, so it is among the first native replacements the runtime needs. Until then the lifted code stops at its first store-queue store, naming it. `0x8C05E01C`, its neighbour, has the same shape and no direct caller.

## What is lifted
Since v0.2.0, with SR modelled (`runtime.md`): 2,756 lifted, 2 not (they read VBR), 1,635 stubs (the replaced regions and the six middleware functions replaced by name in `layout.cpp`). As of v0.1.0, `qilin lift` gave this: of 5,102 functions, **2,718 are lifted: all 1,018 of the game's that anything refers to, and 1,700 of the middleware's.** 43 middleware functions have privileged instructions (they face the hardware and are to be replaced); the 1,631 functions of Shinobi and the modem library are stubs that stop as *unregistered*, naming their caller; 710 functions nothing refers to are left out (184 in the game). The output compiles without a warning under Clang with `-Wall -Wextra -Wpedantic -Wshadow -Werror`.

### Every byte (in the combined repository, 2026-11-22)
As for Toy Commander (`docs/code-analysis.md`, "Every byte"): `qilin coverage` now reports **0 unknown bytes in every region** and `qilin lift` gives **5,140 functions: 4,053 lifted, 1,087 replaced, 0 not lifted**. The 1,444 bytes (32 runs) and 5 functions that were left once the code came here were:
- **The engine's matrix code's spare entries**, the same code as Toy Commander's 0xCE34 bytes on, byte for byte (13 known entries), **the debugger stubs** (`0x8C0A1DD4`, `0x8C0A1DE0`, with the stack-pointer entry between) and **Shinobi's exception stub** (`0x8C0E0424`), with their literals; the middle word of an 8-byte constant, Shinobi's build tag and a literal nothing loads (data blocks), as in Toy Commander.
- **A debug adapter check** (`0x8C09A900`): it compares the words at `0x8C004020` with the development kit's debug adapter's name and breaks (`0x003B`) only if they match. The break made discovery reject the three functions that call it; it is now replaced (a stub of 24 bytes, its literal and the name a data block) by a native that does the comparison and stops only on a match.
- **An interrupt chain handler** (`0x8C08220E`): one instruction (`mov.l`, r4) before the function at `0x8C082210`, registered for event 0x900 by `0x8C082140`. Its address is a constant there, so it had been tried as a pointer guess and rejected for running into its neighbour, and the layout's entry then wasn't tried at all (a guess is tried once). A layout's entry is now always tried. Without it the game would have stopped the first time that handler ran.
- **`nop` filler before loop heads** the compiler aligns to 32 bytes inside functions: six runs of 20 to 30 bytes. The padding rule now also takes any run of filler shorter than 32 bytes that ends on a 32-byte boundary.
- **Literal-pool entries nothing loads** (5 data blocks), and after one of them a small delay loop nothing calls (`0x8C09B96C`), which discovery then finds as an orphan.
- **A branch into a delay slot** (`0x8C06F13A` to `0x8C06F156`, in Kamui 2's vertex transform; Toy Commander's version has none): the FPSCR mode pass followed the slot only as the `bra`'s, so the four instructions after it, which only that path reaches, had no proven mode. Its walk now has a slot entered by a branch as a node of its own (the address with its low bit set), as the lifter's slot copies already were.
- **Four functions that read VBR or DBR**: Kamui 2's interrupt removal (`0x8C090B84`, the same code as Toy Commander's `0x8C085ED4`), the C runtime's `read_dbr` (`0x8C05E2F8`), and two of Shinobi's timer-interrupt set-ups linked among the middleware here (`0x8C04D7E0`, `0x8C04D916`), which nothing refers to. All four are natives now (the last three stop if reached).

Still to do: 34 replaced functions that lifted code can reach have no native yet (Toy Commander has none); `docs/toyracer/runtime.md` lists them as they are written.

## The runtime's to-do list
1. **Shinobi:** replaced whole, at its API: start-up, interrupts, timers, cache, store queues, memory allocation, the GD-ROM file system (over the disc image), Maple (controllers, memory cards, the Jump Pack, the keyboard), the system configuration and flash memory, the real-time clock.
2. **The middleware's hardware-facing parts:** Kamui 2 and `kmdk` (to Vulkan), the audio manager (to the native mixer; `AUDIO64.DRV` is not run), the DMA and store-queue routines. Which functions exactly is the attribution's result.
3. **Lifted:** the game, and the middleware's pure computation: No Cliché's engine, the C runtime and maths, as far as the attribution confirms.
4. **The network:** the modem library, the provider-settings libraries and the TCP/IP stack's hardware side are never run. They are replaced by "no connection": the port has no online mode (`roadmap-and-design-targets.md`).
5. **The web browser:** not part of the port. The function that starts its loader is replaced by one that says so.
6. **The development system's leftovers** (CodeScape file names, the debugger stubs): replaced by no-ops, with a log line if one is ever reached.
