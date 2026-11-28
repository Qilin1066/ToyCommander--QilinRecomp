// Building the game from the player's data: game.txt's steps run in order
// (by default `qilin lift`, which writes the lifted C++ to work/gen), the
// kit's compiler compiles every .cpp under work/gen (a pool of worker
// threads, the largest files first; objects come from the per-user cache
// when their inputs are unchanged), and the objects are linked with the
// kit's prebuilt library into the game's program, staged and swapped in as
// work/<exe>. Every tool's output goes to work/build.log.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kit.hpp"
#include "process.hpp"

namespace qilin::launcher {

enum class Stage { Checking, Recompiling, Compiling, Linking, Done };

struct BuildProgress {
    std::atomic<Stage> stage{Stage::Checking};
    std::atomic<int> done{0};   // files compiled (or taken from the cache); while Recompiling, steps done
    std::atomic<int> total{0};  // files to compile; while Recompiling, steps
    std::atomic<int> cached{0};
};

struct BuildOptions {
    bool check_input = true;                      // run game.txt's identify first
    std::optional<std::uintmax_t> need_bytes;     // free space a build needs (default: game.txt's build_space_mb)
    std::optional<std::uintmax_t> cache_bytes;    // the cache is trimmed to its newest this much (default: cache_mb)
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
std::expected<void, std::string> build_game(const Kit& kit, const std::filesystem::path& input, BuildProgress& progress,
                                            const std::atomic<bool>& cancel, const BuildOptions& options = {});

// The game, started with game.txt's run_args (by default the input's path)
// from the download's folder, its output to work/run.log and run-errors.log.
std::expected<Command, std::string> game_command(const Kit& kit, const std::filesystem::path& input);

// "1.4 GB"
std::string show_bytes(std::uintmax_t bytes);

// A build's log (work\build.log; the iPad's work/ios/build.log): what every
// tool printed, one tool after another. Safe from several threads.
class BuildLog {
public:
    explicit BuildLog(const std::filesystem::path& path) : file_(path, std::ios::binary | std::ios::trunc) {}
    void line(std::string_view text);
    // A tool's command and output (its own log file, then removed).
    void tool(const Command& command, const std::filesystem::path& output, std::string_view result);

private:
    std::mutex mutex_;
    std::ofstream file_;
};

// Compiling the lifted C++ (every .cpp under `gen`, in subfolders too, each
// object at the same relative path under `obj`) with a pool of workers and
// the object cache: the game's build for this computer, and the iPad's
// (ios_install.cpp) with another compiler, flags and cache.
struct CompileJob {
    std::filesystem::path cwd;               // where the compiler runs (work/); `gen` and `obj` are relative to it
    std::string gen = "gen";
    std::string obj = "obj";
    std::filesystem::path logs;              // each compiler's own output, before it goes into the log
    std::filesystem::path cache;
    std::filesystem::path compiler;
    std::vector<std::string> compiler_args;  // before every compiler call (zig's "c++"; xcrun's "--sdk iphoneos clang++")
    std::vector<std::string> cxxflags;
    std::vector<std::pair<std::string, std::string>> environment;  // set for the compiler (macOS: DEVELOPER_DIR)
    // What the objects are for, first in each cache key: "cpu\n<cpu_identity()>\n"
    // for this computer's game, another target's name for another's.
    std::string target;
    std::filesystem::path include;           // the kit's headers, which every object depends on
    bool use_cache = true;
    unsigned jobs = 0;                       // 0: one per core (no more than memory allows)
    std::uint64_t memory_mb = 1200;          // one compiler's peak (game.txt's compile_memory_mb)
    std::string see_log;                    // added to a failure's message
};

// The objects (relative to `cwd`, sorted), or a plain message. progress.total
// and .done count the files; `cancel` stops it ("The build was stopped.").
std::expected<std::vector<std::string>, std::string> compile_lifted(const CompileJob& job, BuildLog& log, BuildProgress& progress,
                                                                   const std::atomic<bool>& cancel);

// The newest `keep` bytes of cached objects stay in `cache`; the rest go.
void trim_cache(const std::filesystem::path& cache, std::uintmax_t keep);

}  // namespace qilin::launcher
