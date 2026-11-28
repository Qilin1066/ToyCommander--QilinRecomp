// Building the game from the player's disc: `qilin lift` writes the lifted
// C++ (64 files of about equal size) to work\gen, the bundled clang++
// compiles each file (a pool of worker threads, the largest files first;
// objects come from the per-user cache when their inputs are unchanged), and
// the objects are linked with the kit's prebuilt library into the game's .exe,
// staged and swapped in as work\<Game>.exe. Every tool's output goes to
// work\build.log.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "kit.hpp"

namespace qilin::launcher {

enum class Stage { CheckingDisc, Recompiling, Compiling, Linking, Done };

struct BuildProgress {
    std::atomic<Stage> stage{Stage::CheckingDisc};
    std::atomic<int> done{0};   // files compiled (or taken from the cache)
    std::atomic<int> total{0};  // files to compile
    std::atomic<int> cached{0};
};

struct BuildOptions {
    bool check_disc = true;                       // the tests' discs are stand-ins
    std::uintmax_t need_bytes = 2ull << 30;       // free space a build needs on the folder's drive
    std::uintmax_t cache_bytes = 2ull << 30;      // the cache is trimmed to its newest this much
    std::string native = "-march=native";         // the game is compiled for this PC's CPU
    unsigned jobs = 0;                            // 0: one per core
    bool use_cache = true;                        // false: compile everything again (and refill the cache)
};

// What the game's .exe is built from: the kit's version, the CPU and the
// SHA-256 of everything the build reads from the kit (and of this launcher).
// A different one at start-up means the build is out of date.
std::expected<std::string, std::string> build_identity(const Kit& kit);
// What the last build recorded (work\built-with.txt), if any.
std::optional<std::string> built_with(const Kit& kit);
// Whether the game's .exe is there and was built from this kit.
bool up_to_date(const Kit& kit, const std::string& identity);

// Builds; the error is a plain message for the player (pointing to
// build.log where there's more). `cancel` stops it between steps.
std::expected<void, std::string> build_game(const Kit& kit, const std::filesystem::path& disc, BuildProgress& progress,
                                            const std::atomic<bool>& cancel, const BuildOptions& options = {});

// "1.4 GB"
std::string show_bytes(std::uintmax_t bytes);

}  // namespace qilin::launcher
