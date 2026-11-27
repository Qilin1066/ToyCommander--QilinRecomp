// The download's layout, as the launcher finds it (package.sh lays it out):
//
//   <folder>\Play <Title>.exe      this launcher
//   <folder>\kit\                  game.txt, VERSION, kit.ninja, qilin.exe,
//                                  lib\, include\, toolchain\
//   <folder>\work\                 the build: gen\, obj\, build.log, built-with.txt
//   <folder>\<Game>.exe            the game, once built
//
// and the per-user data folder the game uses too (host/src/paths.cpp):
// %LOCALAPPDATA%\qilin\<game>\ (or QILIN_SAVE_DIR), where the launcher keeps
// the chosen disc (launcher-disc.txt) and the compiled objects' cache
// (build-cache\), so a new download folder knows both.
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace qilin::launcher {

struct Kit {
    std::filesystem::path top;       // the download's folder
    std::filesystem::path kit;       // top\kit
    std::filesystem::path work;      // top\work
    std::filesystem::path launcher;  // this program
    std::filesystem::path user;      // the per-user data folder
    // From kit\game.txt.
    std::string game;          // toycommander: the data folder's and the tags' name
    std::string title;         // Toy Commander
    std::string exe;           // ToyCommander.exe
    std::string boot;          // 1GUTH.BIN
    std::string build;         // Toy Commander (USA): bootfile.cpp's known build's title
    std::string product;       // MK-57020
    std::string disc_version;  // 1.022
    std::string package;       // ToyCommander--QilinRecomp
    std::string repo;          // Qilin1066/ToyCommander--QilinRecomp
    std::string version;       // kit\VERSION

    std::filesystem::path game_exe() const;
    std::filesystem::path compiler() const;  // the bundled clang++
    std::filesystem::path qilin() const;
    std::filesystem::path build_log() const { return work / "build.log"; }
    std::filesystem::path disc_file() const { return user / "launcher-disc.txt"; }
    std::filesystem::path cache() const { return user / "build-cache"; }
    // "Toy Commander (USA), MK-57020 version 1.022"
    std::string wanted_disc() const;
};

// Reads the kit beside `launcher` (top = its folder), or says what's missing.
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

// PROCESSOR_IDENTIFIER: the game is compiled for this CPU (-march=native).
std::string cpu_identity();

}  // namespace qilin::launcher
