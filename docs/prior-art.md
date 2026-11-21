# Prior art

What exists that this project can learn from, and what it may and may not take. To be extended as each piece is read properly; v0.1.0 has only a first survey.

## Rules
- **Nothing GPL goes in**, as code, tables or constants: that covers the Dreamcast emulators (Flycast, redream's open parts, lxdream, MAME's Dreamcast driver) and QEMU. They can be *run* as black boxes to compare behaviour, never read into this code.
- **Hitachi's and Sega's documents are cited by title**, not linked, quoted or included: the *SH-4 Software Manual*, the *SH7750 Series Hardware Manual*, the Dreamcast SDK's documentation.
- **No game data and nothing derived from it** in the repository.

## Sister projects (same author, MIT)
GBTGhoulies--QilinRecomp (Xbox, x86), PunchOutWii--QilinRecomp and RhythmHeavenFever--QilinRecomp (Beat the Beat; Wii, PowerPC), ApbArcade--QilinRecomp (arcade, T-11 and 6502). This project takes from them directly: the layout, the build and CI set-up, the SHA-256, the table-driven decoder with an X-macro instruction list and its cross-check, `std::expected` loaders, the register-in-locals lifter design, the replacement registry and bring-up loop, the settings menu, and the conventions in `naming.md`. Lessons already applied here: one decoder table checked for overlaps when built; reviewed differences listed in the check script, never silently skipped; open every gamepad; runtime code without `std::format` for macOS 10.15.

## Other Dreamcast static recompilations
Found in a first search (November 2026), not yet read in detail; licences to be checked before anything is looked at closely:
- **Street Fighter Alpha 3** (irnutsmurt/Street-Alpha-3-Dreamcast-Recomp): the US Dreamcast game's SH-4 code recompiled to C, with per-character code overlays loaded at fixed addresses.
- **ChuChu Rocket!** (sp00nznet/chuchu): SH-4 to C, no interpreter or JIT at run time; its notes on the sound driver's interrupt back to the SH-4 are relevant to how much of the audio path must exist.

Both recompile to C and model the hardware beneath Sega's libraries; this project replaces the libraries at their API instead, as its sisters do. Their experience with SHC's code (delay slots, literal pools, the FPSCR modes, the software divide) is the useful part.

## Hardware references
- **KallistiOS** (an open-source Dreamcast development library, BSD-style licence): its headers name the PowerVR2's and the system bus's registers, and the port's code uses the same names. Nothing is copied from it; what each register's bits do is read from Kamui 2's own code in the boot file.

## Decompilations and tools
- **Tokyo Bus Guide** (lhsazevedo/tbg-decomp) and **Sonic Adventure** (reshaped/sonic-adventure): Dreamcast decompilations built with the original compiler. Useful for how Hitachi's SHC lays out code, pools and switch tables, and for naming Shinobi and Kamui 2 functions by matching code.
- **Ghidra's Dreamcast scripts and function-ID databases** (the "Dreamcast Disassembly, Debugging and Decompilation Diaries" collection lists them): a way to name library functions in the boot file. Whether their data can be used, or only the method, depends on each one's licence.

## The game
- **No Cliché's engine:** the game's missions are driven by an event-based scripting language run by an interpreter in the boot file (its version string is there; the `.SCC` paths in `GUTH.LST` are its sources), and a reader's comment on the article below says they are reverse engineering it. The interpreter is lifted with the game, so the port doesn't need the format understood.
- **`.GHQ` bundles:** no public description found; an old forum thread got no further than raw data. The lifted loader reads them as shipped.
- **Cheat codes and hidden content** are documented in the "Under the microscope: Toy Commander" article (32bits.substack.com), useful for testing later missions.

## Video
The movies are Duck TrueMotion 2X. The MultimediaWiki has a description of the format (frames made of tagged chunks; YUV 4:4:4), whether a free decoder exists was not established in this survey. The game's own decoder is lifted, so none is needed.
