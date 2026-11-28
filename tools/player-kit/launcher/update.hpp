// The Update button: GitHub's releases list (nothing about the player sent
// but a generic program name), the newest release of this game for this
// system, its download unpacked beside this folder (platform.hpp), and kit/
// and the launcher swapped for the new ones (macOS: the whole app). Any
// failure leaves the old version as it was and says what happened.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include "kit.hpp"
#include "version.hpp"

namespace qilin::launcher {

struct UpdateCheck {
    std::optional<Release> newer;  // a newer release than this download, if any
    std::string message;           // what to tell the player
};

std::expected<UpdateCheck, std::string> check_for_update(const Kit& kit);

struct UpdateProgress {
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> total{0};  // 0 while unknown
    std::atomic<bool> unpacking{false};
};

// Downloads, unpacks and installs `release`: the new launcher's path (start
// it, then quit). The error is a plain message.
std::expected<std::filesystem::path, std::string> install_update(const Kit& kit, const Release& release, UpdateProgress& progress,
                                                const std::atomic<bool>& cancel);

// What an earlier update left behind: kit.old\, the old launcher (.old) and
// the unpacking folder. Called at start-up; retries while the old launcher
// is still closing.
void clean_after_update(const Kit& kit);

// Starts a program on its own (not a build tool), for the relaunch.
bool start_detached(const std::filesystem::path& exe, const std::filesystem::path& cwd);

}  // namespace qilin::launcher
