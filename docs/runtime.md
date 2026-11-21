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
- **`memory.*`:** 4 GB of host address space reserved for the guest's (`base + addr`), with only what is committed backed: RAM, 16 MB at `0x8C000000`, where the game is linked to use it.
- **`bus.*`:** every access outside that RAM. `ld`/`st` in `guest.hpp` check one range and call the bus otherwise. Below `0xE0000000` the bus decodes the physical address as the SH-4 does (the low 29 bits): RAM's other views (`0xAC000000`, `0x0C000000`, the mirrors) go to the same RAM, and devices are found by physical range. From `0xE0000000` addresses are taken whole: the store queues (`0xE0000000`, flushed by `pref` to where QACR0/1 say) and the SH-4's own registers. Devices are mapped by the program (`rt::map`), with a block write for whole store-queue bursts. An access nothing handles stops, naming the address and the instruction. `QILIN_TRACE_BUS=1` logs every device access.
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
| `0x8C0BAB98` | `shinobi_set_work_area` | Keeps a work area's address (r4) in a Shinobi global, the word its literal pool names (`0x8C0BAD80`); written in guest RAM as the guest writes it. 0, or −1 for a null address. |
| `0x8C0BABEA`, `0x8C0BAC38`, `0x8C0BAC68`, `0x8C0BACF0` | `shinobi_sound_mode`, `_area`, `_language`, `_broadcast` | The console's settings, which Shinobi reads from its flash memory: each writes its value through r4 and returns 0 (−1 for a null r4). The port answers as a USA console (area `'1'`), NTSC (`'0'`), English (1), stereo (0). The language will come from the settings file. |
| `0x8C0B0758` | `shinobi_cable` | The video cable, from an I/O port's bits 9–8: the port answers VGA (0), 640×480 progressive, what a PC shows best. |
| `0x8C0B4840` | `shinobi_hardware_init` | Shinobi's hardware set-up (interrupt controller, timers, DMA, through three sub-functions); the caller doesn't use its result. The hardware is the port's: nothing to do. |
| `0x8C0B3E10` | `shinobi_heap_init` | Shinobi's allocator set-up over r5 bytes from r4: the middleware passes the end of the program's data (`0x8C124340`) and the rest of RAM (to `0x8D000000`). |
| `0x8C0B3EDE`, `0x8C0B3EE8`; `0x8C0B3FAC`, `0x8C0B3FB6`; `0x8C0B3E6A` | `shinobi_malloc`, `shinobi_free`, `shinobi_heap_exit` | The allocator (`heap.cpp`): Shinobi's own algorithm, natively, over the same guest RAM and state, so every block is where it would be on the console. A circular free list in address order of 32-byte units with a 32-byte header (next, size); first fit from a rover, cutting from the top of the free block; freeing merges with both neighbours. The entry points jump through an operations table only the set-up writes; anything but the default there stops. `realloc`, `calloc` and the statistics are never called. The shut-down is an `rts`. |
| `0x8C04EDF4` | `memset` | The C runtime's fill (r4, the byte r5, r6 bytes) through the store queues; a zero byte goes to `memclr`. Leaves fpul as the guest code does. |
| `0x8C0B4244` | `shinobi_rtc_init` | The real-time clock's set-up: reads the AICA's clock into a Shinobi word and adds a refresh to the interrupt chain. Only Shinobi reads either; returns 0. |
| `0x8C0B3288` | `shinobi_module_init_8C0B3288` | Unidentified: sets up 24 records of Shinobi's own, keeps two buffers the middleware passes, and programs interrupt masks and DMA. Nothing outside Shinobi reads them. |
| `0x8C0B27E4` | `shinobi_record_init_8C0B27E4` | Sets r4's three words to {0, r5, 0} and returns r4 (gdFs's directory record; the middleware passes a block for 0x400 entries). |
| `0x8C085E16` | `kamui_install_interrupt_handler` | Kamui 2's one replaced function: records the handler for the graphics interrupts and the masks it sets, in Kamui 2's state as the guest code leaves it. The port will deliver those interrupts to the handler. |
| `0x8C0B4840` (update) | `shinobi_hardware_init` | Now starts timer 0 as Shinobi does (a free-running count down at Pφ/4), since Kamui 2 times its waits with it. |
| `0x8C0B4958`, `0x8C0B497E`, `0x8C0B4988` | `shinobi_timer_start`, `_count`, `_diff` | Timer 0 restarted as above; the count so far (`0xFFFFFFFF` − TCNT0); the difference of two counts. |
| `0x8C0BDB94`, `0x8C0BDDF2`, `0x8C0BE046` | `shinobi_cache_invalidate`, `_purge`, `_write_back` | syCache's operations over a range (`ocbi`, `ocbp`, `ocbwb` loops, run from the uncached view). The port has no cache between the CPU and its devices: nothing to do. |
| `0x8C0B399C`; `0x8C0B3B6E`, `0x8C0B3BB8` | `shinobi_chain_add`; `shinobi_chain_dispatch_900`, `_920` (`chain.cpp`) | Shinobi's interrupt chains, over the same guest records (256 of 16 bytes at `0x8C116ABC`, list heads at `0x8C117ABC`), in priority order. Events 0x900 and 0x920 are Kamui 2's two callbacks: a list's first handler hooks Shinobi's dispatcher into Kamui 2 by calling Kamui 2's (lifted) registration, as the guest code does, and the dispatchers are natives walking the list. The system bus's events (0xA00 + 0x20 n: normal status bit 12 + n; 0xB00 + 0x20 n: external bit n) set their level-4 mask bit; the port raises them itself (`raise_bus_event`). CPU exception codes (below 0x900) stop until something needs one. |
| `0x8C0B427C`, `0x8C0B42A6` | `shinobi_rtc_refresh`, `shinobi_rtc_get_date` (`rtc.cpp`) | The clock: the date from the host's local time as Shinobi lays it out (year, month, day, hour, minute, second, the day of the week as its congruence gives it, the moon's age from its new moon and month), and the count of seconds since 1950 it comes from, kept where Shinobi keeps it. Shinobi's request flags are 0; anything else stops. The middleware seeds a random number generator from the time of day. |
| `0x8C0C1520`–`0x8C0C1584` | `gd_send_command`, `gd_check_command`, `gd_main_loop`, `gd_init`, `gd_check_drive`, `gd_dma_end` (`gdrom_bios.cpp`) | The boot ROM's GD-ROM system calls, below gdd (see below). |

## The GD-ROM: gdFs and gdd lifted, the boot ROM's system calls replaced
Shinobi's file system, gdFs (1.08), is ISO 9660 and handle bookkeeping over a driver table (`0x8C0C5980`, 31 slots); its GD-ROM driver, gdd, is a table of 20 operations (`0x8C0C6260`) over the boot ROM's system calls, which it reaches through 13 one-line wrappers (`0x8C0C1520`–`0x8C0C161C`: `r6` = 0 or −1, `r7` = the call, then a jump through `0x8C0000BC`). None of gdFs or gdd touches hardware: everything goes through those calls, a few of syCache's and syTmr's functions and the interrupt chains. So, as for Kamui 2, the library is lifted and what lies beneath it is the port's: gdFs (`0x8C0B13D4`–`0x8C0B3288`) and gdd (`0x8C0BD264`–`0x8C0BDB14`) are lifted regions, with no unknown bytes, and every handle, directory record and state gdFs keeps is the console's, byte for byte. Replacing gdFs's 26 functions the middleware calls would have meant reconstructing those structures by guesswork.

The calls (`gdrom_bios.cpp`), on the player's own disc dump: commands are done when they are sent, and every request is complete the first time gdd asks. Frame addresses are the disc reader's sectors plus 150 (the lead-in). So far:
- call 0, send a command: 24 (the drive's initialisation), 40 (its version string, of which gdFs reads three digits into a word nothing reads), 16 and 17 (PIO and DMA reads of 2048-byte sectors to a buffer); any other command stops, naming it;
- call 1, a request's state: complete (2), with the bytes transferred;
- call 2, the boot ROM's turn to work, and call 5, the DMA's end: nothing to do;
- call 3, the drive's initialisation; call 4, its state: paused, with a GD-ROM in.

A DMA read ends, on the console, with the system bus's event 0xA40 (GD-ROM DMA end), whose chain gdFs has a handler on. The port raises it at the next system call made with interrupts at level 4 unmasked: after the read was sent and the caller has recorded what it waits for, as on the console, never inside the call that sent it.

## The bring-up so far
Each run stops at the next thing the port doesn't have, as the sister projects found theirs.

1. `0x8C0B068A`, Shinobi's initialisation, called by the start-up: written (above).
2. `0x8C04E8D6`, the middleware's system initialisation, called by `main`: not lifted because of `stc sr`. That led to the SR model (above), which lifts 19 of the 21 functions that weren't.
3. `0x8C0BAB98`, then the five settings queries (`0x8C04E79C` gathers them into a word of flags), then Shinobi's hardware set-up and its allocator: written (above).
4. **Now:** the first hardware access. The middleware's system initialisation calls `0x8C056BC0`, Kamui 2's initialisation. That reaches `0x8C08790C`, which programs the graphics chip through two small helpers, `0x8C084382` (read a register) and `0x8C084378` (write one), and stops reading its ID register at `0xA05F8000`. The call chain is main → `0x8C04E8D6` → `0x8C056BC0` → `0x8C06BE68` → `0x8C08790C` → `0x8C084382`.

**The decision: the hardware under Kamui 2, not Kamui 2's API.** The plan was Kamui 2 replaced at its API, as Punch-Out replaced GX. Trying to name its functions showed that without symbols it can't be done reliably: its objects are interleaved with the engine's and the audio manager's, and its entry points are only told apart by guesswork. So Kamui 2 stays lifted, and the port provides what it drives: the PowerVR2's registers (`pvr.cpp`: the ID and revision the code checks, the interrupt status registers' write-one-to-clear, a register file for the rest), video memory in both its views, and the SH-4's timer 0 (`sh4.cpp`), which Kamui 2 times its waits with. The tile accelerator's parameter stream comes next, decoded into Vulkan 1.3 draws. Register names come from KallistiOS's headers (`prior-art.md`); what each bit does is read from Kamui 2's own code.

5. Kamui 2's whole initialisation now runs lifted against that hardware, and its interrupt registration (`0x8C085E16`, the one Kamui 2 function replaced) records its handler. Open: the display's sync status reads 0, and no interrupt is delivered yet; both are needed once something waits for a frame.
6. Then Shinobi's remaining set-ups called by the system initialisation (`0x8C0B3288`, the clock `0x8C0B4244`), the allocator in full (above: the game's first `malloc`), and gdFs's directory record.
7. `0x8C0B1A46`, gdFs's initialisation, called by the middleware's file layer (`0x8C04D5A2`, retried a few times on failure). Reading gdFs showed it is software over gdd over the boot ROM's calls: gdFs and gdd were made lifted regions and the calls written (above). On the way: the interrupt chains (gdFs adds a handler for the vertical blank and one for the GD-ROM DMA's end), syTmr's reads (gdFs times its waits), syCache's operations (gdd invalidates the cache after a DMA), and a gap in discovery (`0x8C0B1FD6`, a driver-table entry reached by a tail-call branch).
8. gdFs's initialisation, the drive's version and status, its first reads and the middleware's directory loading (`0x8C04D3A4`) now run as on the console. Then the clock (`rtc.cpp`), timer 0's restart, and `main` goes on into its own start-up.
9. **Now:** Kamui 2 starts a DMA with the SH-4's DMA controller (`0xFFA0002C`, channel 2's control register, from `0x8C09595C`): the transfer of its first display list or texture to the PowerVR2.

The report now ends with the call chain: SHC keeps no frame pointers, but every function that calls saves PR on the stack, so the stack's return addresses (words where the instruction four bytes earlier is a call) are the chain. `QILIN_STACK=1` dumps the whole stack too.
