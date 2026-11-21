// Where this port keeps what it writes (saves, settings, the pipeline cache).
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <filesystem>

namespace qilin::host {

// The game's data folder, as the other Qilin recomps keep theirs:
// qilin/<the game> in the platform's per-user application data (Windows:
// %LOCALAPPDATA%; macOS: ~/Library/Application Support; elsewhere
// $XDG_DATA_HOME or ~/.local/share), or QILIN_SAVE_DIR if set. Created
// if missing.
std::filesystem::path user_folder();

}  // namespace qilin::host
