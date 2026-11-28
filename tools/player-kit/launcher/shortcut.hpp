// A shortcut to the launcher on the player's desktop: "<Title>.lnk".
#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include "kit.hpp"

namespace qilin::launcher {

// Where the shortcut goes (empty if Windows names no desktop folder).
std::filesystem::path desktop_shortcut(const Kit& kit);
// Whether there is one that starts this folder's launcher.
bool has_desktop_shortcut(const Kit& kit);
// Makes (or remakes) it.
std::expected<void, std::string> make_desktop_shortcut(const Kit& kit);

}  // namespace qilin::launcher
