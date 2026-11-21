# Naming

The same rule as the other Qilin recomps (GBTGhoulies--QilinRecomp, ApbArcade--QilinRecomp, RhythmHeavenFever--QilinRecomp (Beat the Beat), PunchOutWii--QilinRecomp): **Qilin** is the author's name and goes on the *tools*, the things that do the recompiling. Anything a player sees is named after the game.

| Thing | Name | Why |
|---|---|---|
| The repo / project | ToyCommander--QilinRecomp | The game plus the toolchain that ports it |
| The recompiler CLI | `qilin` | The author's tool: short, unique, easy to type |
| C++ namespaces | `qilin`, `qilin::sh4` (the decoder), `qilin::gdrom` (the disc reader), later `qilin::rt`, `qilin::gen` | The project's namespace; avoids collisions with game, SDK or system names |
| Build options, macros, CMake targets, environment variables | `QILIN_*`, `qilin_*` | Same prefix as the namespace, so it's obvious what owns them |
| The game executable players run | `toycommander` | Named after the game. Players launch Toy Commander, not a tool |
| The window's title, SDL's app name | Toy Commander | Same reason |
| The game's data folder | `<per-user data>/qilin/toycommander` | As the other recomps keep theirs (`qilin/<game>`): memory-card saves, the pipeline cache |
| The settings file | `toycommander.ini`, in the data folder | After the game |
| The settings menu | F1 or Esc (controller: Back + Start); Controls, Display and Game pages; pauses the game; saves on close | The other recomps' menu (Dear ImGui), kept in step with them: see `roadmap-and-design-targets.md` |
| The Windows download | `ToyCommander--QilinRecomp-<version>-windows-x64.zip` | After the project |
| Lifted functions | `fn_<address>`, e.g. `fn_8C0B0340` | By their address in the boot file as loaded; library functions get real names where analysis identifies them |

The game executable is `qilin_run` while the runtime is brought up, as in the sister projects; it takes the game's name with the first player release.

New code follows the same rule: the prefix marks the toolchain, and anything a player sees is named after the game.
