// The download's layout, as the launcher finds it (each project's packaging
// scripts lay it out; ADOPTING.md). Windows and Linux:
//
//   <folder>/Play <Title>(.exe)    this launcher
//   <folder>/README.txt, LICENSES/
//   <folder>/kit/                  game.txt, VERSION, files.txt (every file the
//                                  release holds), build-flags.txt, qilin(.exe),
//                                  lib/, include/, toolchain/
//   <folder>/work/                 made here: the game, its logs (run.log,
//                                  run-errors.log) and the build (gen/, obj/,
//                                  logs/, build.log, built-with.txt)
//
// macOS: <Title>.app, the launcher in Contents/MacOS, the kit in
// Contents/Resources/kit, and work/ in the data folder (an app may be
// read-only, or run from a copy Gatekeeper makes).
//
// The per-user data folder is the game's too (the runtime's own paths):
// %LOCALAPPDATA%\qilin\<game> on Windows, ~/Library/Application
// Support/qilin/<game> on macOS, ~/.local/share/qilin/<game> on Linux (or
// QILIN_SAVE_DIR). The launcher keeps the chosen disc or folder there
// (launcher-disc.txt) and the compiled objects' cache (build-cache/), so a
// new download folder knows both.
//
// Everything about the game is kit/game.txt's (ADOPTING.md lists its keys):
// nothing in the launcher's code names a game.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::launcher {

// What the player gives the launcher: one file (a disc image) or a folder
// (an extracted disc, a ROM set).
enum class InputKind { File, Folder };

struct Kit {
    std::filesystem::path top;       // the download's folder (macOS: the .app)
    std::filesystem::path kit;       // top/kit (macOS: in the app's resources)
    std::filesystem::path work;      // top/work (macOS: in the data folder)
    std::filesystem::path launcher;  // this program
    std::filesystem::path user;      // the per-user data folder
    // From kit/game.txt.
    std::string game;          // toycommander: the data folder's and the tags' name
    std::string title;         // Toy Commander
    std::string exe;           // ToyCommander.exe (ToyCommander on Linux and macOS)
    std::string boot;          // 1GUTH.BIN: what the recompiler reads, for the progress line (may be empty)
    std::string build;         // Toy Commander (USA): the supported build's name
    std::string product;       // MK-57020: its product number or ID (may be empty)
    std::string disc_version;  // 1.022: its version (may be empty)
    std::string package;       // ToyCommander--QilinRecomp: the download's folder, the release assets' name
    std::string repo;          // Qilin1066/ToyCommander--QilinRecomp: where releases are published (empty: no updates)
    std::string release_tag_prefix;    // "toycommander-v": the game's release tags, before X.Y.Z
    std::string release_asset_prefix;  // "ToyCommander--QilinRecomp-": the downloads' names, before X.Y.Z-<system>
    std::string compiler_path;               // in the kit, or absolute (macOS: Apple's clang++)
    std::vector<std::string> compiler_args;  // before every compiler call (zig's "c++")
    // The player's data.
    InputKind input = InputKind::File;
    std::vector<std::string> input_extensions;  // a file's: cue, gdi, chd (lower case, no dot); empty: any
    std::string input_noun;    // "disc", "game folder", "ROM set": the player's word for it
    std::string input_hint;    // a sentence: how to choose it
    // Commands, as game.txt has them (placeholders and all; build.hpp runs them).
    std::string identify;            // prints the identify contract's JSON (ADOPTING.md)
    std::vector<std::string> steps;  // step.1 to step.9: the recompiler's work, in order
    std::string run_args;            // the game's arguments
    // What this project's downloads before the launcher left behind (paths
    // relative to the download folder; tidy.cpp removes them), and where one
    // remembered the player's data (its path is taken over, then removed).
    std::vector<std::string> old_files;
    std::vector<std::string> old_input_files;
    std::uint64_t compile_memory_mb = 1200;  // what one compiler needs at its peak
    std::uint64_t build_space_mb = 2048;     // free space a build needs on the work folder's drive
    std::uint64_t cache_mb = 2048;           // the object cache is trimmed to its newest this much
    std::map<std::string, std::string, std::less<>> about;  // every game.txt line, for {game:KEY}
    std::string version;       // kit/VERSION

    std::filesystem::path game_exe() const;  // work/<exe>
    std::filesystem::path flags_file() const { return kit / "build-flags.txt"; }
    std::filesystem::path run_log() const { return work / "run.log"; }
    std::filesystem::path run_errors() const { return work / "run-errors.log"; }
    std::filesystem::path compiler() const;
    std::filesystem::path qilin() const;
    // kit/<name> plus the system's executable suffix ({tool:NAME}).
    std::filesystem::path kit_tool(std::string_view name) const;
    std::filesystem::path build_log() const { return work / "build.log"; }
    std::filesystem::path disc_file() const { return user / "launcher-disc.txt"; }
    std::filesystem::path cache() const { return user / "build-cache"; }
    // "Toy Commander (USA), MK-57020 version 1.022"
    std::string wanted() const;
    // "the disc" / "the game folder"
    std::string the_input() const { return "the " + input_noun; }
};

// A game.txt command (`identify`, `step.N`) as the program and its
// arguments, split by the launcher's own rules (kit_text.hpp's
// split_command, no shell), each placeholder replaced within its argument:
// {qilin}, {kit}, {input}, {gen}, {work}, {tool:NAME}, {game:KEY}.
std::expected<std::vector<std::string>, std::string> expand_command(const Kit& kit, std::string_view text, const std::filesystem::path& input);

// Reads the kit beside `launcher`, or says what's missing or wrong.
std::expected<Kit, std::string> load_kit(const std::filesystem::path& launcher);
// The same, with the data folder given (the tests).
std::expected<Kit, std::string> load_kit(const std::filesystem::path& launcher, const std::filesystem::path& user);

// One launcher per folder at a time: false if another already has it.
bool claim_folder(const Kit& kit);

// The launcher's own path.
std::filesystem::path own_path();

// Small file helpers. Paths in messages are shown as the player knows them.
std::string show(const std::filesystem::path& path);
std::expected<std::string, std::string> read_file(const std::filesystem::path& path);
// Written to a temporary file beside it, then renamed over it.
std::expected<void, std::string> write_file(const std::filesystem::path& path, std::string_view text);
std::expected<std::string, std::string> sha256_file(const std::filesystem::path& path);
// The last `lines` lines of a text file ("" if it's missing).
std::string tail(const std::filesystem::path& path, int lines);

// The processor, as precisely as the system says: the game is compiled for
// this one (build-flags.txt's `native`), so another means a new build.
std::string cpu_identity();

}  // namespace qilin::launcher
