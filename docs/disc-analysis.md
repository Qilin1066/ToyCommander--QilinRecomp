# The disc and the boot file

What the supported release is, and what's on it. Found with `qilin disc`, `qilin extract`, `qilin info` and `qilin profile` on the owner's dump (v0.1.0). Nothing from the disc is in this repository.

## The supported release
Toy Commander, USA (En, Fr, De, Es). From the disc's bootstrap sector:

| Field | Value |
|---|---|
| Hardware | `SEGA SEGAKATANA` |
| Maker | `SEGA ENTERPRISES` |
| Device | `F8A6 GD-ROM1/1` |
| Areas | `U` (USA only) |
| Peripherals | `0799A10`: no Windows CE; VGA box supported (bit 4) |
| Product | `MK-57020` |
| Version | `V1.022` |
| Date | `19990930` |
| Boot file | `1GUTH.BIN` |
| Title | `TOY COMMANDER` |

The boot file's SHA-256 is `ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd` (748,444 bytes). The recompiler recognises the build by it (`recomp/src/bootfile.cpp`); `qilin info` exits 3 for anything else. "Guth" is the game's working title, Gutherman.

## The dump
A Redump-style dump: one `.cue` and fifteen `.bin` track files, raw 2352-byte sectors.

| Tracks | Area | What |
|---|---|---|
| 1 | single density | data: the session ordinary CD drives see |
| 2 | single density | audio: the "this is a Dreamcast disc" warning |
| 3 | high density, from sector 45000 | data: the bootstrap (16 sectors), the ISO 9660 volume descriptor and directories |
| 4–14 | high density | audio: eleven tracks, the game's music (about 39 minutes) |
| 15 | high density, index 1 at sector 377422, ending at 549150 | data: every file |

The file system's sector numbers count from the start of the disc, so a file's sectors are found through the track table: all 81 files lie in track 15, the directories in track 3. The audio tracks carry a two-second pregap in their files (`INDEX 00`), track 15 a three-second one. `recomp/src/gdrom.*` reads all of this; a read that runs into an audio track, or a sector without a Mode 1 header, is an error.

## The files
81 files, 351,300,123 bytes.

| Files | What |
|---|---|
| `1GUTH.BIN` | the boot file: all the game's code |
| `AUDIO64.DRV` (20,740 bytes) | the sound driver: ARM code for the sound chip's CPU, loaded by the audio manager |
| `MENU.GHQ`, `SCENE/*.GHQ` (64 scenes, 1.4 to 2.9 MB each; the menu 3.2 MB) | the game's resource bundles: the menus, and one per mission or multiplayer arena. Each starts with a table of offsets |
| `GUTH.LST` | the scene list: numbered source paths of the scenes (`…\AREAS\Area01\training.SCC` …) |
| `GUTH_AM.TXT`, `GUTH_FR.TXT`, `GUTH_DEU.TXT`, `GUTH_ESP.TXT` | the game's text in American English, French, German and Spanish, one numbered string per line |
| `60HZ_INTRO_LRTF.AVI`, `60HZ_BUMPER_LRTF.AVI`, `60HZ_FINAL_LRTF.AVI`, and `50HZ_` versions | the movies: AVI, 640×480, video `TM2X` (Duck TrueMotion 2X) at 29.97 frames a second in the 60 Hz files, sound format tag 0x61 (Duck's DK4 ADPCM), stereo, 44.1 kHz |
| `0GDTEX.PVR`, `NCLOGO.PVR`, `TRUEMOTION.PVR` | PowerVR textures: the disc's picture for the console's menu, the No Cliché and TrueMotion logos |

The boot file names `*.GHQ`, the `.LST` and `.TXT` files, `Menu.ghq`, the three movies and the logos, and holds the AVI chunk names and `TM2X`: the resource loader, the script interpreter and the movie player are in the boot file, not on the side.

## The boot file
`1GUTH.BIN` has no header: it is memory from `0x8C010000` to `0x8C0C6B9C`, and execution starts at its first byte. There are no sections, symbols or relocations; where code ends and data begins is for function discovery to establish.

- **Entry:** six `nop`s, then the start-up code reads the cache control register (`0xFF00001C`), masks and sets bits in it, and jumps to `0x8C0B0340`.
- **Layout** (from function discovery, v0.2.0; `recompiler-core.md`): code with its literal pools from `0x8C010000` to `0x8C09F5B6`; data from there to `0x8C0B0340`; then, starting at the address the entry code jumps to, more code (hand-written library code among it) with data and two 8 KB zero-filled blocks between, and a last 5 KB of data at the end.
- **Linear sweep:** 374,222 words, 53,476 of them not SH-4 instructions (data). 23,577 words decode as `fmov` forms (which depend on FPSCR.SZ), 12,658 as arithmetic that depends on FPSCR.PR, and `fschg` appears 126 times and `frchg` 65: the code switches transfer size and register bank, so the mode analysis in `roadmap-and-design-targets.md` is needed, not optional. 1,171 words are privileged instructions: the libraries run in privileged mode and handle SR themselves. These counts include data that happens to decode; exact ones come with function discovery.
- **Libraries linked in** (from their version strings): the Shinobi library 1.61 (built July 1999) with its parts (`syStart`, `syMalloc`, `syTmr`, `syInt`, `syCache`, `sySq`, `syG2`, `syRtc`, `syCfg`, `syBt*`, `gdFs` 1.08, `pd` 1.50 with `pdLcd` and `pdVib`, `bu` 1.47, `kd` 1.48, `fmMng`, `ov`), Kamui 2 (16,3,0,0, July 1999) and `kmdk`, the audio manager (`am*`, `ac*`: digital and MIDI playback over `audio64.drv`), and the game's script system ("Interpreteur Ver 1.00", built 1999-09-30).
- **Hardware reached directly:** aligned words that look like literal-pool entries (a few may be coincidences) hold addresses of the SH-4's on-chip registers (`0xFFxxxxxx`), the PowerVR2's registers (`0xA05F8000`), the system bus, Maple and GD-ROM registers (`0xA05F6000`–`0xA05F7FFF`), the sound chip's registers and memory (`0xA0700000`, `0xA0800000`), video memory (`0xA4000000`–`0xA5FFFFFF`), the tile accelerator's input (`0x10000000`, written through the store queues at `0xE0000000`), and the boot ROM's system-call vectors in low RAM (`0x8C0000B0`, `B4`, `B8`, `BC`, and `0x8C0000E0`). The inventory per function is `qilin hwaccess` (v0.3.0): 76 functions reach hardware at a constant address, none of them in the game (`code-analysis.md`).

## The bootstrap
The first 16 sectors of track 3 (`IP.BIN`): the fields above, then Sega's licence screen code and the game's own bootstrap code, which the boot ROM runs before the boot file. What state it leaves for `1GUTH.BIN` (stack, VBR, cache, FPSCR) is to be read from its code at roadmap step 3: the runtime sets that state up itself and runs none of it.
