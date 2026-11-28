// A way to start the launcher without finding its folder: a desktop shortcut
// on Windows ("<Title>.lnk", shortcut_win.cpp), an entry in the applications
// menu on Linux (~/.local/share/applications, shortcut_posix.cpp). On macOS
// the app itself is that (in Applications, the Dock): none is offered.
#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include "kit.hpp"

namespace qilin::launcher {

// The button's words, or nullptr where there's nothing to offer.
const char* shortcut_label();
// Where the shortcut goes (empty if the system names no place for it).
std::filesystem::path desktop_shortcut(const Kit& kit);
// Whether there is one that starts this folder's launcher.
bool has_desktop_shortcut(const Kit& kit);
// Makes (or remakes) it.
std::expected<void, std::string> make_desktop_shortcut(const Kit& kit);

}  // namespace qilin::launcher
