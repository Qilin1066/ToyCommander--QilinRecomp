// Whether a disc image is the one this download builds from, read in-process
// with the recompiler's own disc reader (qilin_disc): the bootstrap names the
// boot file, whose SHA-256 names the build (recomp/src/bootfile.cpp).
#pragma once

#include <filesystem>
#include <string>

#include "kit.hpp"

namespace qilin::launcher {

struct DiscCheck {
    bool ok = false;
    std::string message;  // plain words for the player, either way
};

// A .cue, .gdi or .chd, by its name.
bool disc_image_name(const std::filesystem::path& path);

DiscCheck check_disc(const Kit& kit, const std::filesystem::path& disc);

}  // namespace qilin::launcher
