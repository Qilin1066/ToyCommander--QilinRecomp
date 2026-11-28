// The download's layout, as the launcher finds it (package.sh,
// package-linux.sh and package-macos.sh lay it out). Windows and Linux:
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
// The per-user data folder is the game's too (host/src/paths.cpp):
// %LOCALAPPDATA%\qilin\<game> on Windows, ~/Library/Application
// Support/qilin/<game> on macOS, ~/.local/share/qilin/<game> on Linux (or
// QILIN_SAVE_DIR). The launcher keeps the chosen disc there
// (launcher-disc.txt) and the compiled objects' cache (build-cache/), so a
// new download folder knows both.
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::launcher {

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
    std::string boot;          // 1GUTH.BIN
    std::string build;         // Toy Commander (USA): bootfile.cpp's known build's title
    std::string product;       // MK-57020
    std::string disc_version;  // 1.022
    std::string package;       // ToyCommander--QilinRecomp
    std::string repo;          // Qilin1066/ToyCommander--QilinRecomp
    std::string compiler_path;               // in the kit, or absolute (macOS: Apple's clang++)
    std::vector<std::string> compiler_args;  // before every compiler call (zig's "c++")
    std::string version;       // kit/VERSION

    std::filesystem::path game_exe() const;  // work/<exe>
    std::filesystem::path flags_file() const { return kit / "build-flags.txt"; }
    std::filesystem::path run_log() const { return work / "run.log"; }
    std::filesystem::path run_errors() const { return work / "run-errors.log"; }
    std::filesystem::path compiler() const;
    std::filesystem::path qilin() const;
    std::filesystem::path build_log() const { return work / "build.log"; }
    std::filesystem::path disc_file() const { return user / "launcher-disc.txt"; }
    std::filesystem::path cache() const { return user / "build-cache"; }
    // "Toy Commander (USA), MK-57020 version 1.022"
    std::string wanted_disc() const;
};

// Reads the kit beside `launcher`, or says what's missing.
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
