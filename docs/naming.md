# Naming

The same rule as the other Qilin recomps (GBTGhoulies--QilinRecomp, ApbArcade--QilinRecomp, RhythmHeavenFever--QilinRecomp (Beat the Beat), PunchOutWii--QilinRecomp): **Qilin** is the author's name and goes on the *tools*, the things that do the recompiling. Anything a player sees is named after the game.

This repository holds two games (the owner, 2026-11-21: one repository, a program and a release for each), so everything a player sees comes in two, one per game.

| Thing | Toy Commander | Toy Racer | Why |
|---|---|---|---|
| The repo / project | ToyCommander--QilinRecomp | (the same repository) | The games plus the toolchain that ports them |
| The recompiler CLI | `qilin` | `qilin` | The author's tool: short, unique, easy to type; it knows both builds by their SHA-256 |
| C++ namespaces | `toycommander` (the game's own code); `dreamcast` (shared) | `toyracer`; `dreamcast` | `qilin`, `qilin::sh4`, `qilin::gdrom`, `qilin::rt`, `qilin::gen` for the toolchain and the runtime |
| Build options, macros, CMake targets, environment variables | `QILIN_*`, `qilin_*`; per game `QILIN_TOYCOMMANDER_*`, `toycommander_*` | `QILIN_TOYRACER_*`, `toyracer_*` | Same prefix as the namespace, so it's obvious what owns them |
| The game executable players run | `toycommander` (`ToyCommander.exe` in the download) | `toyracer` (`ToyRacer.exe`) | Named after the game. Players launch the game, not a tool |
| The window's title, SDL's app name | Toy Commander | Toy Racer | Same reason |
| The game's data folder | `<per-user data>/qilin/toycommander` | `<per-user data>/qilin/toyracer` | As the other recomps keep theirs (`qilin/<game>`): memory-card saves, the pipeline cache |
| The settings file | `toycommander.ini`, in the data folder | `toyracer.ini` | After the game |
| The settings menu | F1 or Esc (controller: Back + Start); Controls, Display and Game pages; pauses the game; saves on close | the same | The other recomps' menu (Dear ImGui), kept in step with them: see `roadmap-and-design-targets.md` |
| Releases | tags `toycommander-vX.Y.Z`, `CHANGELOG.md` | tags `toyracer-vX.Y.Z`, `toyracer/CHANGELOG.md` | Each game is released on its own |
| The Windows download | `ToyCommander--QilinRecomp-<version>-windows-x64.zip` | `ToyRacer--QilinRecomp-<version>-windows-x64.zip` | After each game's project name |
| Lifted functions | `fn_<address>`, e.g. `fn_8C0B0340` | the same | By their address in the boot file as loaded; library functions get real names where analysis identifies them |

The game executables were `qilin_run` while the runtime was brought up, as in the sister projects; they took their games' names when the two games came into one repository.

New code follows the same rule: the prefix marks the toolchain, and anything a player sees is named after the game.
