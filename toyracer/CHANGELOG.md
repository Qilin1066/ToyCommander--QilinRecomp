# Changelog

All notable changes to this project are recorded here, in the format of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Past entries are kept as written, never rewritten.

This is Toy Racer's changelog, kept in ToyCommander--QilinRecomp since the two games came into one repository (before that, in ToyRacer--QilinRecomp); Toy Commander's is `CHANGELOG.md`. A change to code both games share is recorded in both. Versions are tagged `toyracer-vX.Y.Z` (`vX.Y.Z` in its own repository); the number lives only in `CMakeLists.txt` (`QILIN_TOYRACER_VERSION`). While the recompiler and runtime are being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

## [Unreleased]

### Added
- **Multisample anti-aliasing**: `msaa = 1, 2, 4 or 8` in the settings file and "Anti-aliasing (MSAA)" in the menu's Game page, at most what the GPU has, changed while the game runs. The `desktop` preset has 4x; `original` and `low-end` none. Polygons' edges are smoothed; the translucent list's per-pixel sort blends after the samples are resolved, so its own edges stay as they were. A race's two renders a frame each resolve only their own lines.

### Fixed
- **Less memory traffic a render**: a render draws, stores and resolves only its own area (a race's two renders a frame each their own lines), instead of loading and storing the whole picture; with multisampling, one shared multisampled image whose samples are never stored, instead of one kept per frame buffer for nothing.
- **The sound chip's interrupt from position events** is no longer raised from the mixer thread into the system bus's status register while the game's thread reads and acknowledges it (it could be lost); it waits in a mailbox the game's thread takes in.
- **A stop's report and exit code** are no longer at risk at the end: the program ends without static destructors while the sound mixer and the display's threads still run (after a stop, at the game's end and on a fatal error).
- **`qilin lift` fails if any function wasn't lifted**, and the FPSCR mode pass stops `qilin` if it doesn't settle, instead of going on with modes that could be incomplete.
- **Builds with zig's Clang on Windows**: CMake no longer scans the C++ sources for modules (the project has none), which needed `clang-scan-deps` and stopped the build before it compiled anything.
- **Widescreen from the first frame**: the picture stayed 4:3 until the game's options were changed or loaded from a save (the game widens its camera only then); it's now widened from the start, and follows the window when it's resized. A race checked at 21:9.

## [0.5.1] - 2026-11-22

### Tests
- The five-controller regression identifies the promoted device by its input rather than its display name: SDL's Xbox mapping may give every virtual pad the same name. It checks admission before a new press, then verifies that the fifth pad's unique button reaches only port A. Controller runtime behaviour is unchanged from the preceding version.

## [0.5.0] - 2026-11-22

### Fixed
- A controller that pressed while all four ports were full remains eligible for a vacant port. It no longer needs another unplug/replug; connected players keep their assignments.
- Rumble uses a renewable 300 ms lease rather than a 60-second command. Constant-strength vibration renews every 100 ms; stalled producers expire, disconnect/order reset stops motors, and non-finite strengths stop vibration.
- Destroying a display releases its own video/gamepad subsystem references rather than calling global `SDL_Quit`, preserving audio owned by another component.
- Missing playback devices and recoverable stream-operation failures no longer terminate the game. Output is retried at most once per second, stale queued sound is discarded, and saved volume/mute settings are applied before restarting playback. Stream lifecycle is serialised separately from the callback's SDL lock; callback errors are reported on the originating thread.
- Save history refuses linked folders that the restore validator would reject. Snapshot inspection/listing/pruning failures are reported instead of silently succeeding. Restore requests are bounded to 1024 bytes and accept UTF-8 BOMs/CRLF while still rejecting multiple lines or embedded NULs.

### Tests
- Added real SDL startup-failure/recovery and subsystem-reference checks alongside the concurrent callback/format/volume stress test.
- Extended the graphics-backed pad test with five-controller promotion, rumble renewal/expiry and audio ownership across display destruction. Linux CI requires it to run.
- Added linked-history, Windows-edited restore-request and oversized-request regressions. [Validation record](../docs/validation-0.12-0.5.md); a complete real-disc playthrough remains unverified.

## [0.4.0] - 2026-11-22

### Fixed
- The Windows launcher hashes files through streamed .NET SHA-256, including headers, compiler, manifest and launcher content. Runtime-only updates force a link even with older timestamps; header/compiler/CPU changes invalidate compiled objects. `-Rebuild` cleans generated outputs, and choosing a disc on the quick path remembers it.
- Windows command lines are decoded from UTF-16. Disc, CUE track names, lift/extraction/output folders, save directories and menu paths use explicit native/UTF-8 conversion. New launches retain literal Unicode and shell metacharacters; stdout and stderr go to `run.log` and `run-errors.log`.
- VMU packet writes check bounds, the existing image size, seek, write, flush and close. Failed opens no longer fall back to truncating an existing file. Card and flash loads distinguish missing files from unreadable/malformed ones and bound reads to the image size.
- Settings, flash and save-history copies share checked, flushed file storage with unique temporary files and replacement on success. Failed copies preserve the destination. Settings accept UTF-8 BOMs and refuse to overwrite unreadable existing files with defaults.

### Tests
- Added storage tests for Unicode paths, bounded reads, packet writes, malformed images, multi-chunk copies, copying to self, failed replacements and Windows locked destinations. Settings/history tests cover Unicode folders, repeated replacement and BOMs.
- Windows CI requires real-Ninja launcher checks for both games: stale timestamps, header/compiler/CPU changes, unchanged reuse, forced rebuild, locked-update recovery, Unicode child arguments and the real recompiler's Unicode input/output paths.
- Added optional `QILIN_SANITIZE` for GCC/Clang. [Validation record](../docs/validation-reliability-2026-11-22.md); no complete-game compatibility claim is made.

### Added
- **Movies are never stretched** (shared with Toy Commander, whose 4:3 movies stay 4:3 in its 16:9 mode); Toy Racer has no movies, so nothing changes in its pictures.
- **The picture as wide as the screen**: the game's camera is widened as far as the screen is wider than 4:3 (16:9, 21:9, up to three times as wide) and the scene drawn that wide; the HUD and the menus' text keep their shape, in the middle. Nothing is stretched, and (Toy Commander's) movies stay 4:3. The game's own 16:9 option no longer changes anything.

## [0.3.0] - 2026-11-22
The game runs into a race and is driven there with a controller: from its start-up through the menus, drawn by the Vulkan 1.3 renderer with everything the PowerVR2 draws, with sound, CD music, saves, the settings menu, four controllers and the Jump Pack, built in Toy Commander's repository from the code the two games share.

### Added
- **The translucent list sorted for each pixel** in auto-sort mode, as the hardware does: where translucent surfaces cross (smoke, glass, effects in Toy Commander's missions) each pixel blends them from the farthest. Devices without fragment-shader atomics keep the sort by triangle.
- **CI runs the graphical tests**: on Linux, under Xvfb and Mesa's software Vulkan, the renderer's, the Vulkan layer's and the pads' tests run on every push instead of skipping (`QILIN_REQUIRE_GRAPHICS=1` makes a skip a failure), as in Punch-Out's CI.
- **The saves' history** (from Punch-Out's): before the memory card or the console's flash memory changes, a copy is kept (the five latest of each); the menu's Game page lists them and can restore one and restart. The flash memory is now written through a temporary file, so a crash mid-write leaves the old one.
- **A modifier volume's user tile clip**: the volume counts only inside (or outside) its clip, for the opaque volumes (in the stencil) and the translucent ones (in the fragment shader's test); the render test checks both.
- **Controllers take their ports by their first press**: a virtual pad or a wheel listed first no longer takes port A from the real controller; a controller plugged in to replace an unplugged one takes its port; the menu shows each port's controller and can choose them again.
- **From Punch-Out's later work**: the sound queue that can't come to lag the picture (bounded, emptied by SDL's callback as the device plays; a new test runs it under SDL's real callbacks); input and rumble ignored while another window has the keyboard; settings for anisotropic filtering (the textures the game filters; 16x in the desktop preset), rumble strength, volume and muting when unfocused, in the file and the menu.
- **The rest of the PowerVR2's drawing** (`renderer.cpp`, `pvr.frag`, `ta.cpp`): the two-volume mode (a polygon's second volume drawn inside the modifier volumes, opaque, punch-through and translucent), vertex fog, the fog table's second mode, the colour clamp and the user tile clip. The render test checks each. Neither game's scenes so far use them.
- **Table fog is drawn** (`renderer.cpp`, `pvr.frag`): how the PowerVR2 picks a fog value from a pixel's depth was measured against the emulator as a black box (`qilin_dcref --fog`, a probe that sets the chip up itself and reads its renders back), and our own definition (`renderer::table_fog`) agrees with all 4,200 bands it drew, to within the 16-bit format's rounding. Polygons in fog mode 0 now go towards the fog colour by the table's value at each pixel's depth; the render test checks it. Nothing changes in the scenes seen so far, whose tables leave it invisible.
- **Fog is reported only where it could show**: table fog whose table is 0 everywhere has a fog factor of 0 at every depth, so drawing those polygons unfogged is already exact (Toy Racer's races). Both games' menus have a table with only its first entry set, which the renderer still reports, because how depth picks an entry is still to be measured against the emulator.
- **Every replaced function its lifted code can reach has a native** (`tools/check/check_natives.py`, new): Shinobi's copies over the G2 bus are lifted, and the expansion port's callbacks stop (only a network library that found a device reaches them).
- **The console's flash memory** (`dreamcast/src/flash.*`) under Shinobi's flash functions, now lifted; the network libraries' software helpers lifted; natives for the web browser's loader (stops: another program), the boot ROM exit (quits), part of Shinobi's shutdown, the extension check's answer and the modem library's handler table and poll. 4 replaced functions its lifted code can reach have no native yet: Shinobi's G2 bus DMA and the callbacks on event 0xB60.
- **Toy Racer: 15 more natives, and its GD-ROM calls as Toy Commander's** (`toyracer/src/`): its CD audio now plays through the sound chip; the interrupt chain's removal, the clock's exit, the sound mode, Shinobi's call on its own stack and the date-to-count conversion the game calls are natives; three software-only Shinobi ranges are lifted, as in Toy Commander. 19 replaced functions its lifted code can reach have no native yet (`docs/toyracer/runtime.md`).
- **Every byte of Toy Racer's boot file accounted for**: 5,140 functions, 4,053 lifted, 1,087 replaced, none left, and 0 unknown bytes in every region (`docs/toyracer/code-analysis.md`, "Every byte"). Three general changes came with it (`docs/recompiler-core.md`, "Three more rules from Toy Racer"): a layout's entry is tried even after a pointer guess at the same address failed; filler shorter than 32 bytes that ends on a 32-byte boundary is padding; and the FPSCR mode pass follows a branch into a delay slot, so the instructions after such a slot get a proven mode. Toy Commander's lift output is unchanged.
- **Toy Racer's own sound driver** (`dreamcast/src/aica.cpp`): Toy Racer's `AUDIO64.DRV` is version 1.90h.2, Toy Commander's 1.80d.2. The stand-in now knows both, from the offsets table the installed image publishes, and stops on any other: 1.90's records are elsewhere in sound RAM, its open closes first, its level is the total level, its sends set the direct level, and its stops drop events (`docs/runtime.md`, "Two versions of the driver"). Position events (commands `0x92` and `0xAD`) are now done as both drivers do them, posting the port's number as a message when the channel passes them; Toy Racer uses them, and stopped at its first one.
- **The lift step quotes its paths**: a Redump dump's name has parentheses and commas, which broke `-DQILIN_<GAME>_BOOT=<the .cue>`.
- **One repository for Toy Commander and Toy Racer, a program and a release for each** (the owner, 2026-11-21: "one repo, two releases"): the code both games share is `dreamcast/` (the hardware the port provides, the renderer, sound, the controller, memory card and Jump Pack, the settings file and menu), each game's own is `toycommander/src/` and `toyracer/src/`, and the recompiler knows both builds (`qilin info`, `layout`, `lift`). `qilin` now also takes a disc's `.cue` wherever it takes a boot file, reading the boot file from the dump.
- **A Windows download for each game** (`tools/player-kit`, `.github/workflows/player-kit.yml`, as Punch-Out's): `ToyCommander--QilinRecomp-X.Y.Z-windows-x64.zip` and `ToyRacer--QilinRecomp-X.Y.Z-windows-x64.zip`, nothing from the game in either. Unpack, double-click `Play <game>.bat`, choose your own dump's `.cue`: the bundled recompiler lifts its boot file and the bundled compiler (LLVM-MinGW) links it with the prebuilt rest of the game, once. Each game's tag builds and attaches its own.
- **Each game's program is checked without its disc**: its whole program but the lifted code is the library `<game>_game`, linked in every build with an empty function table (`<game>_link_check`), so CI compiles and links both games with every compiler.
- **The GD-ROM, at the boot ROM's system calls**, from Toy Commander's bring-up through the code match: Shinobi's file system (gdFs, `0x8C0C7D30`–`0x8C0C9BE8`) and GD-ROM driver (gdd, `0x8C0D18B4`–`0x8C0D216C`) are lifted regions with no unknown bytes, and the system-call wrappers under them are replaced over the player's disc dump (`toyracer/src/gdrom_bios.*`). With them: Shinobi's interrupt chains (`chain.*`), its real-time clock over the host's time (`rtc.*`), and its cache operations (no-ops). gdFs's initialisation and the middleware's directory loading run as on the console, on this game's disc.
- **Discovery takes a function-pointer table's entry that points into a known function** as a function when the words beside it point at function entries (Toy Commander's rule, for gdFs's driver table): 5,103 functions; 2,853 lifted.
- **The C runtime's initialisation and exit lists** (`0x8C0C54DA`, `0x8C0C552C`), natively. Their entries are the modem library's static constructors and destructors, which the port answers with nothing: it has no modem.
- **The game now runs past the middleware's system initialisation into `main`'s own start-up**, to Kamui 2's first DMA: the same place as Toy Commander's bring-up at that moment.
- **Toy Commander's interrupts, DMA controller, tile accelerator decoder, display timing and Maple work**, moved here through the code match (`toyracer/src/interrupts.*`, `ta.*`, `pvr.cpp`, `sh4.cpp`; interrupts taken at loops' backward branches; `QILIN_TIMEOUT`). This project had written its own versions of the first four at the same time; they were dropped for Toy Commander's, so the two games share one implementation.
- **Shinobi's peripheral library (pd), memory-card file system (bu) and Maple driver are lifted**, as in Toy Commander, in regions found by matching its functions (`recomp/src/layout.cpp`): 5,118 functions, 3,167 lifted, 0 unknown bytes in every lifted region.
- **Shinobi 1.71's extension check** (`0x8C0D0DC0`), which looks for the modem and for devices on the expansion port: answered natively with "nothing there".
- **The graphics start-up completes** (textures load, display lists are decoded, frames go by, with no renderer yet), and the run goes on into the audio manager's initialisation.
- **Toy Commander's sound RAM and sound chip registers** (`aica.*`) **and bu's threads on host threads** (`threads.*`), moved here: the sound driver's installation runs. The run stops at the first CD audio command the audio set-up sends to the drive (command 22), which is next.
- **CD audio's drive commands and the sub-channel command**, from Toy Commander (`gdrom_bios.cpp`: play, pause, and command 34's Q-channel data, the last written in Toy Commander's repo from this project's bring-up).
- **Shinobi's keyboard library (pdKbd) is lifted** (`0x8C0C5924`–`0x8C0C5AFC`): 3,186 functions lifted.
- **The game reaches its menu's main loop** and keeps running: it builds about 51 scenes a second, unrendered (no renderer, sound output or controller yet).

### Changed
- **The game's program is `toycommander`** (Toy Racer's `toyracer`), no longer `qilin_run`, built with `-DQILIN_TOYCOMMANDER_BOOT=<1GUTH.BIN or the .cue>` (`-DQILIN_TOYRACER_BOOT`) in place of `-DQILIN_GAME_BOOT`. Its window, data folder (`qilin/<game>`) and settings file are each game's own.
- A shared runtime piece Toy Commander hasn't written is no longer waited for: it is written in Toy Commander's repo and moved here (the owner, 2026-11-21: "i'd rather it didnt wait").
- **The first pictures**, from Toy Commander (written there from this project's session): the Vulkan 1.3 renderer of the tile accelerator's scenes, the PowerVR2's texture formats, an SDL3 window, and the sister projects' host layer (`host/`). The game draws its start-up screens and its title screen.
- **Toy Commander's answer to the sound driver's mailbox** (`aica.cpp`), moved here.
- **Controllers and the first races**, from Toy Commander (written there from this project's session): the Maple bus answered from the host's first pad and the keyboard, with the standard pad's product name this game requires; frame buffers kept between renders, so the races' frames, rendered in two parts, compose. The game goes from the title through the menus into a race and drives.
- **Calls made with `bra` and PR loaded by hand** are found as calls (Toy Commander's core change, moved here): no function here changed.
- Toy Commander's level-4 interrupts and bu threads at their caller's mask, and the drive's release command, moved here.

### Fixed
- The renderer's descriptor sets could keep pointing at a buffer that had been made again with the same handle (wrong pixels or a crash now and then); they're rewritten whenever anything they name is remade.

## [0.2.0] - 2026-11-21
The runtime's bring-up begins, from Toy Commander's: the game's own lifted code runs from the start-up through `main` and Kamui 2's initialisation to the first file access.

### Added
- **The runtime** (`recomp/runtime/`, `toyracer/src/`, `docs/runtime.md`), brought over from ToyCommander--QilinRecomp: guest memory (4 GB reserved, RAM committed), the bus for everything outside RAM, the registry of native replacements with its stop that names an unregistered function and its caller, the stop report with the guest call chain, the SR model in the lifter (40 more functions lifted; 2 left, which read VBR), `qilin lift --files`, and `qilin_run`, built only from the player's own boot file (`-DQILIN_GAME_BOOT`).
- **Toy Commander's natives, moved here by matching the two boot files' code.** Functions whose instruction words agree once the link-dependent fields are masked pair up, 1,322 of them, and their literal pools pair 2,131 addresses. Through that map came the native start-up, Shinobi's initialisation, settings (answered as a European console on a VGA cable), hardware set-up and allocator, the store-queue fill and clear, Shinobi's copy and clear, Kamui 2's interrupt registration, and the hardware under Kamui 2 (the PowerVR2's registers, video memory, timer 0). Each constant was checked against this boot file.
- **The compiler's structure copy, natively** (`0x8C04DE3C`, `0x8C04DE48`): a jump through a table into an unrolled run of loads and stores, which discovery can't follow and which was the first stop.
- **Where it stops now:** gdFs's initialisation (`0x8C0C83A2`), exactly where Toy Commander's bring-up stands. The GD-ROM file system over the disc reader is next, for both.

### Changed
- **No online mode**, the owner's decision (2026-11-21). The roadmap's step 10 is gone: the network libraries are replaced by "not connected" for good, and the menu's Network entry will say the port has none. The port is the local game. (The three core rules of v0.1.0 are now in ToyCommander--QilinRecomp too, as the owner asked.)

### Fixed
- **MSVC rejected the lifted code around a hang.** `rt::hang` was marked `noreturn`, so MSVC called everything after a call to a function that ends in one unreachable, an error with warnings as errors. It is now an ordinary function defined out of line (`dispatch.cpp`), and the lifted code returns after it. CI's Windows job caught it on v0.1.0's first push.

## [0.1.0] - 2026-11-21
The project starts with the whole recompiler core, carried over from Toy Commander, and every function of the game lifted to C++.

### Added
- **The project**, with the requirements and scope of the sister QilinRecomp projects, as the owner asked ("exact same as the other projects"): C++23, Vulkan 1.3 only, static recompilation only, 64-bit Windows, Linux and macOS on x86-64 and ARM64, the same settings menu. The roadmap, the design targets and the naming are in `docs/`.
- **The recompiler core of ToyCommander--QilinRecomp at its v0.8.0**: the GD-ROM reader, the boot file loader, the SH-4 decoder with its Capstone cross-check, function discovery and control flow, coverage per region, the hardware-access profile, the FPSCR mode pass, the lifter with the FPU and the approximate instructions' measured definitions, the unit tests, the end-to-end lifter tests, the differential test (242,496 runs, 0 mismatches), `qilin_dcref`, and CI on six configurations. Toy Commander is the same studio's earlier game on the same engine, so all of it applies unchanged.
- **The supported build:** Toy Racer, Europe (`MK-5114950`, `V1.050`, 2000-11-30), recognised by its boot file's SHA-256. The disc and the boot file are described in `docs/disc-analysis.md`: 14 tracks, ten of them music; 113 files, of which 16 are the game and the rest Sega's web browser, which is out of scope; no movies.
- **The layout** of `1TOYRACE.BIN` (`recomp/src/layout.cpp`, evidence in `docs/code-analysis.md`): the game `0x8C010080`–`0x8C04CA58`, the middleware to `0x8C0A5B02`, data to `0x8C0C5440`, Shinobi with Sega's modem library to `0x8C0F9BB4`, their data to the end. The game's boundary is the lowest address above which nothing calls below, with one exception recorded: the authentication module calls four functions at the game's start.
- **Results on the supported build:** 5,102 functions and 33 switch tables; **0 unknown bytes in the game** (0.27% of the image, all in libraries still to be attributed); 96 functions reach hardware at a constant address, none of them in the game; all 33,443 FPU instructions of the game and the middleware have a proven mode; **2,718 functions lifted, every one of the game's 1,018 that anything refers to**, the output warning-free under Clang with warnings as errors.
- **Guesses a real function contains are thrown out** (function discovery). A data word held, by coincidence, the address of the middle of a game function nothing refers to; the code from there decoded, was taken for a function, and the real function was rejected for running on into it, leaving 180 bytes of the game unaccounted for. When a rejected guess with a compiled prologue ran on into a function that came from a pointer and that nothing else supports, that function is now dropped and the analysis run again; the rejected-candidates list says which function it was inside.
- **Far branches inside long functions** (function discovery and the lifter). A function too long for `bra` branches inside itself with `mov.l label,rN ; jmp @rN`. That was read as a tail call to a function at the label, which hid a 7 KB middleware function. A `jmp` to a constant address that the function's other paths reach too is now a branch inside it, lifted as a `goto`. 22 of them in two middleware functions. `qilin cfg` counts them, and its JSON lists them (`computed_branches`).
- **A branch to itself stops loudly** (the lifter and the runtime). `bra` to its own address is how the code stops for good on a fatal error; lifted as an empty loop it compiled with a warning and, being an endless loop with nothing in it, was the compiler's to delete. It is now `rt::hang`, a stop of its own kind that names the instruction. Three places in the supported build.
- **Tests** for the three: unit tests for the far branch (with the label's address as a literal), for a coincidental pointer thrown out and for one that something calls kept; end-to-end programs for a far branch whose delay slot sets a register and for the hang. 128,956 end-to-end checks. Each fails when its rule is broken.
- **The online mode in the roadmap** (step 10, last): the network stack replaced at its socket API by the host's sockets. Until then the network libraries are replaced by "not connected". No modem or PPP is ever emulated.
