// PNG files of RGBA8 pictures (SDL's own reader and writer, SDL 3.4): the
// texture packs' files.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace qilin::host {

// `rgba`: width * height pixels, rows top to bottom, each pixel's bytes red,
// green, blue, alpha (a little-endian uint32_t 0xAABBGGRR). False, with the
// reason in `error`, if it can't be read or written.
bool save_png(const std::filesystem::path& file, uint32_t width, uint32_t height, const uint32_t* rgba, std::string& error);
bool load_png(const std::filesystem::path& file, uint32_t& width, uint32_t& height, std::vector<uint32_t>& rgba, std::string& error);

}  // namespace qilin::host
