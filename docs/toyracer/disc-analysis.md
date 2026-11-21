# The disc and the boot file

What the supported release is, and what's on it. Found with `qilin disc`, `qilin extract`, `qilin info` and `qilin profile` on the owner's dump (v0.1.0). Nothing from the disc is in this repository.

## The supported release
Toy Racer, Europe (En, Fr, De, Es): the game was only released in Europe. From the disc's bootstrap sector:

| Field | Value |
|---|---|
| Hardware | `SEGA SEGAKATANA` |
| Maker | `SEGA ENTERPRISES` |
| Device | `6116 GD-ROM1/1` |
| Areas | `E` (Europe only) |
| Peripherals | `2799A10`: no Windows CE; VGA box supported (bit 4). Toy Commander's field with one more bit set (bit 25) |
| Product | `MK-5114950` (printed as MK-51149-50) |
| Version | `V1.050` |
| Date | `20001130` |
| Boot file | `1TOYRACE.BIN` |
| Title | `TOY RACER` |

The boot file's SHA-256 is `da29a065f9db779f0c95d778de36c019848b211ad93a667244bf58e9a10ee5d4` (972,324 bytes). The recompiler recognises the build by it (`recomp/src/bootfile.cpp`); `qilin info` exits 3 for anything else.

## The dump
A Redump-style dump: one `.cue` and fourteen `.bin` track files, raw 2352-byte sectors.

| Tracks | Area | What |
|---|---|---|
| 1 | single density | data: the session ordinary CD drives see |
| 2 | single density | audio: the "this is a Dreamcast disc" warning |
| 3 | high density, from sector 45000, 319,441 sectors | data: the bootstrap (16 sectors), the ISO 9660 volume descriptor and directories. Only 23 of its sectors hold anything; the rest is empty, which pushes the files to the outside of the disc |
| 4–13 | high density | audio: ten tracks, the game's music (about 37½ minutes) |
| 14 | high density, index 1 at sector 536967, ending at 549150 | data: every file |

The file system's sector numbers count from the start of the disc, so a file's sectors are found through the track table: all 113 files lie in track 14, the directories in track 3. The audio tracks carry a two-second pregap in their files (`INDEX 00`), track 14 a three-second one. `recomp/src/gdrom.*` reads all of this, unchanged from Toy Commander's reader; a read that runs into an audio track, or a sector without a Mode 1 header, is an error.

## The files
113 files, 24,486,979 bytes. Two programs share the disc: the game, and the web browser Sega's European online games shipped with. The game is 16 files and about 6.3 MB.

### The game

| Files | What |
|---|---|
| `1TOYRACE.BIN` (972,324 bytes) | the boot file: all the game's code |
| `AUDIO64.DRV` (19,180 bytes) | the sound driver: ARM code for the sound chip's CPU, loaded by the audio manager (version 1.90h.2 by its own text, dated 2000-06-28) |
| `COMMON.GHQ` (2.2 MB) | the resource bundle every track shares, with the menus. Starts with a table of offsets, as Toy Commander's bundles do |
| `SCENE/TRACKP1.GHQ` … `TRACKP4.GHQ` (480 to 640 KB each) | the four tracks |
| `TOYRACER.LST` | the scene list: five numbered source paths (the shared texture list and the four tracks' `.SCC` scripts) |
| `TOYRACER_ENG.TXT`, `_FR.TXT`, `_DEU.TXT`, `_ESP.TXT` | the game's text in English, French, German and Spanish, one numbered string per line |
| `ARIAL26.FNT`, `ARIAL26.PVR` | a font: its metrics (a `FONT` header, then `CHAR` records) and its texture |
| `0GDTEX.PVR`, `NEXGENLOGO_512.PVR` | PowerVR textures: the disc's picture for the console's menu, and the logo of the network library's maker |

There are no movies on this disc, and the boot file has no movie player's file names in it.

### The web browser

| Files | What |
|---|---|
| `2_DP.BIN` (2.8 MB) | the browser: Dream Passport 2 in its European form (it calls itself DreamKey), built on ACCESS's Avefront |
| `SG_DPLDR.BIN`, `MAIGO.BIN` (14,856 bytes each, not identical) | small loaders; `SG_DPLDR.BIN` names `2_DP.BIN`, and the boot file names `sg_dpldr.bin`, so the menu's "home page" entry presumably starts the browser through it |
| `DP2.INI`, `DPETC/`, `DPFONT/`, `DPSS/`, `DPTEX/`, `DPWWW/` (94 files, 15.3 MB) | the browser's settings, sounds and voices, fonts, pictures, textures and its built-in pages |

The browser is a separate program for a service that no longer exists. It is **out of scope**: the port neither recompiles it nor reads its files, and the menu entry that starts it will say so (`roadmap-and-design-targets.md`).

## The boot file
`1TOYRACE.BIN` has no header: it is memory from `0x8C010000` to `0x8C0FD624`, and execution starts at its first byte. There are no sections, symbols or relocations; where code ends and data begins is established by function discovery (`code-analysis.md`).

- **Entry:** six `nop`s, then the start-up code reads the cache control register (`0xFF00001C`), masks and sets bits in it, and jumps to `0x8C0C5440`. Word for word what Toy Commander's does, with another target.
- **Layout** (`code-analysis.md`): the game's code from `0x8C010080` to `0x8C04CA58`; the middleware's to `0x8C0A5B02`; data to `0x8C0C5440`; then Shinobi and Sega's modem library to `0x8C0F9BB4`; their data to the end.
- **Linear sweep:** 486,162 words, 65,723 of them not SH-4 instructions (data). 22,444 words decode as `fmov` forms (which depend on FPSCR.SZ), 12,108 as arithmetic that depends on FPSCR.PR, and `fschg` appears 125 times and `frchg` 67: the code switches transfer size and register bank, so the FPSCR mode analysis is needed here as it was for Toy Commander. 2,404 words are privileged instructions: the libraries run in privileged mode and handle SR themselves. These counts include data that happens to decode.
- **Libraries linked in** (from their version strings):
  - the Shinobi library 1.71 (built July 2000) with its parts: `syStart`, `syMalloc`, `syTmr`, `syInt`, `syCache`, `sySq`, `syG2`, `syRtc`, `syCfg`, `syCbl`, `syHw`, `syMmu`, `syChain`, `syDds`, `syBt*`, `syExt*`, `gdFs` 1.10, `pd` 1.50 with `pdKbd`, `pdLcd` and `pdVib`, `bu` 1.47, `kd` 1.48, `fmMng`, `ov`;
  - Kamui 2 (16,5,4,1, October 2000) and `kmdk` (1,5,1,1);
  - the audio manager (`am*`, `ac*`, `a64*`; "Audio64 Ver 11.0.h.2") over `audio64.drv`;
  - Sega's modem library (`modem` 1.0311): a whole dial-up modem's controller, with its AT command set and the modulations up to V.90 (Rockwell's copyright string is in it);
  - Sega's network information libraries (`ntInf`, `ntUtl`, `ntIniMdm`, `IspInfo`), which read the provider settings the browser stores in the console's flash memory;
  - NexGen Software's TCP/IP stack (`NexGenOS`, `NexGenIP`, `NexGenPPP`, `NexGenPPPoE`, `NexGenModem`, `NexGenDcLan`, `NexGenUTILS`, `ETD`, all version 1.2, November 2000);
  - a "Transport" layer built the same day as the game, and an authentication module (it names a `dream-key.com` server).
  
  There is no script interpreter banner as in Toy Commander; the game's banner is "ToyRacer Ver 1.00", built 2000-11-30. No Windows CE.
- **Hardware reached directly:** `qilin hwaccess` lists 96 functions that reach hardware at a constant address, none of them in the game (`code-analysis.md`).

## What the game is, for scope
Toy Racer is a racing game built on Toy Commander's engine, with four tracks. Its menu offers Training, Local (several players on one console, in split screen) and Network (races over the internet through the modem). The online mode needed Sega's servers; players have since brought it back with a replacement server (`prior-art.md`). The port is the local game; the online mode is out of scope (`roadmap-and-design-targets.md`).

## The bootstrap
The first 16 sectors of track 3 (`IP.BIN`): the fields above, then Sega's licence screen code and the game's own bootstrap code, which the boot ROM runs before the boot file. What state it leaves for `1TOYRACE.BIN` (stack, VBR, cache, FPSCR) is to be read from its code at roadmap step 3: the runtime sets that state up itself and runs none of it.
