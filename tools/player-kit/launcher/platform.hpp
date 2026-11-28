// What the launcher needs from the operating system, apart from running
// programs (process.hpp): one implementation for Windows (platform_win.cpp)
// and one for Linux and macOS (platform_posix.cpp). Everything above this is
// the same on every system. Text is UTF-8 throughout.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::launcher {

// A UTF-8 path as the system's path, and back (kit.hpp's show()).
std::filesystem::path from_utf8(std::string_view utf8);

// `path`, usable past Windows' 260-character limit even where long paths
// aren't switched on: absolute, with Windows' \\?\ prefix when it's long.
// Unchanged on Linux and macOS. Only for the launcher's own file operations,
// never for paths handed to other programs.
std::filesystem::path long_path(const std::filesystem::path& path);

// An environment variable ("" if unset).
std::string environment(const char* name);

// Where per-user data lives, before "qilin\<game>": %LOCALAPPDATA% on
// Windows, ~/Library/Application Support on macOS, $XDG_DATA_HOME or
// ~/.local/share on Linux (as host/src/paths.cpp has it). Empty if unknown.
std::filesystem::path user_data_base();

// Replaces `to` with `from` in one step (a rename over it).
std::expected<void, std::string> rename_over(const std::filesystem::path& from, const std::filesystem::path& to);
// Puts `staged` in `target`'s place, keeping the old one as `previous`; a
// failure leaves `target` as it was. (`target` may not exist yet.)
std::expected<void, std::string> swap_in(const std::filesystem::path& target, const std::filesystem::path& staged,
                                         const std::filesystem::path& previous);

// The release download for this system and processor: "windows-x64.zip",
// "linux-x86_64.tar.gz", "linux-arm64.tar.gz" or "macos.dmg".
std::string release_asset_suffix();
// What executables end with here (".exe" or "").
std::string_view executable_suffix();
// The computer's processor architecture as build-flags.txt names it, "arm64"
// or "x86_64": a universal kit (macOS's) has flags for each. A Mac with Apple
// silicon is "arm64" even while this program runs under Rosetta.
std::string host_architecture();

// HTTPS, sending only a generic program name: a short answer as text...
std::expected<std::string, std::string> fetch_text(const std::string& url, const std::vector<std::string>& headers);
// ...or a download to a file. `received` counts as it goes; `cancel` stops it.
std::expected<void, std::string> download(const std::string& url, const std::filesystem::path& to, std::atomic<std::uint64_t>& received,
                                          const std::atomic<bool>& cancel);
// A release download unpacked into `into` (a .zip, .tar.gz or .dmg).
std::expected<void, std::string> unpack(const std::filesystem::path& archive, const std::filesystem::path& into,
                                        const std::filesystem::path& log);

// Memory free for new programs now, in bytes (0 if the system won't say):
// how many compilers can run at once without running out (a Raspberry Pi
// with 2 GB has room for one).
std::uint64_t available_memory();

// Opens a file or folder with what the system opens it with.
void open_with_system(const std::filesystem::path& path);

}  // namespace qilin::launcher
