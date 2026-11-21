# The runtime and the bring-up

What runs the lifted program: guest memory, the registry of native replacements, the start-up, and the stops that say what to write next. The game executable is `qilin_run` during bring-up (`docs/naming.md`).

## Building and running
The program is built only from the player's own boot file, as Punch-Out's is from its `main.dol`:

```sh
build/qilin extract path/to/game.cue --out extracted        # outside the repo, or in an ignored folder
cmake -S . -B build -DQILIN_GAME_BOOT=extracted/files/1GUTH.BIN
cmake --build build --target qilin_run
build/qilin_run path/to/game.cue
```

At build time `qilin lift --files 24` writes the lifted C++ into the build tree (`build/generated`, never the source tree), and `qilin_run` is compiled from it. At run time `qilin_run` reads the boot file from the disc dump again, checks that its SHA-256 is the one the program was lifted from (`g_lifted_sha256` in the generated `table.cpp`), and boots it. While the port is being brought up it is built with checked memory (`QILIN_RUN_CHECKED`, on by default), so a stray access stops, naming the instruction.

## The pieces (`recomp/runtime/`)
- **`guest.hpp`:** the context, memory access and the helpers the lifted code calls, and `rt::replaced`.
- **`memory.*`:** 4 GB of host address space reserved for the guest's (`base + addr`), with only what is committed backed. For now that is RAM, 16 MB at `0x8C000000`, where the game is linked to use it. The uncached and physical views (`0xAC000000`, `0x0C000000`) are added when code uses them; a checked build names any that does.
- **`system.*`:** the registry of native replacements (`provide`), lookups in the function table (`function_containing`), calls from native code into guest code (`call_guest`, with a return address no guest code has), and the report. Every stub `qilin lift` writes for a replaced function calls `rt::replaced`. That runs the native code registered for the address and returns to the caller, as the guest code would; if there is none, it stops as *unregistered*, naming the function and its caller. That is how the bring-up finds the next thing to write.
- **`dispatch.cpp`:** indirect calls through the generated function table.

Native code follows SHC's convention: arguments in r4–r7, the result in r0. It leaves r8–r15 alone, and writes no register its guest counterpart doesn't, since the callers' analysis relies on what each callee writes.

**SR.** Lifted code reads SR and writes its interrupt mask: the middleware masks interrupts around its hardware set-up (`stc sr,r0 ; and ; or #0xF0 ; ldc r0,sr`) in 19 functions. SR is held in the context but for T, S, Q and M, which live in their own locals. The system bits are fixed: privileged mode, register bank 0, exceptions unblocked, the FPU enabled. IMASK is whatever the code sets; the runtime will deliver its interrupts (vertical blank and the like) only where IMASK allows them, as the hardware does. An `ldc` that would change a system bit stops. The differential test covers the four SR forms. That leaves 2 functions unlifted, both setting up exception handling (`stc vbr`, `ldc rN,r1_bank`), which the port replaces.

## The start-up (`toycommander/src/boot.cpp`)
The boot file's entry code sets the cache control register and jumps to Shinobi's start-up at `0x8C0B0340`. The runtime does what that start-up does, natively:
1. The boot file goes to `0x8C010000`; the rest of RAM is zero.
2. r15 is `0x8C00F400`, and `0x8C00C000`–`0x8C00F400` is filled with the word `0x41474553` ("SEGA"), as the start-up fills its stack.
3. The C runtime's sections are set up as SHC's section initialisation does, from four tables of start and end addresses in the image (`0x8C09F5B8`–`0x8C09F5DC` and Shinobi's, `0x8C0C56A0`–`0x8C0C56C4`): two zero-filled, two copied.
4. `0x8C0B068A`, Shinobi's initialisation, is called with r4 = the word at `0x8C0C5690` (`0x8C010000`).
5. Then the game's `main`, `0x8C02796A`.
6. Then `0x8C0B06E2`, Shinobi's exit, with the same r4.

The calls go through the function table, so they reach the registry.

## Native replacements (`toycommander/src/natives.cpp`)
| Address | Name | What it does, and what the native code does |
|---|---|---|
| `0x8C04EEE0` | `memclr` | The C runtime's clear: zeroes r5 bytes from r4 and returns r4. The guest code clears 32-byte blocks through the store queues (`docs/code-analysis.md`), which leaves fr0 and fr1 zero when it takes that path. The native code is a bounds-checked `memset` and zeroes fr0 and fr1 on the same condition. |
| `0x8C0B068A` | `shinobi_library_init` | Registers each library handle between the "Lib Handle Start" and "Lib Handle End" markers (one, at `0x8C010040`) in a table only Shinobi reads, with interrupts masked. The port replaces Shinobi: nothing to do; returns 0. |
| `0x8C0B06E2` | `shinobi_library_exit` | The same walk, removing them: nothing to do; returns 0. |

## The bring-up so far
Each run stops at the next thing the port doesn't have, as the sister projects found theirs.

1. `0x8C0B068A`, Shinobi's initialisation, called by the start-up: written (above).
2. `0x8C04E8D6`, the middleware's system initialisation, called by `main`: not lifted because of `stc sr`. That led to the SR model (above), which lifts 19 of the 21 functions that weren't.
3. **Now:** `0x8C0BAB98`, a Shinobi function the system initialisation calls first, with r4 = `0x8C0F80BC`.
