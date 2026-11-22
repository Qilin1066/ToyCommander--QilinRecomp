#pragma once
#include <cstdint>
#include <limits>
#include <filesystem>
#include <string_view>
#include <string>

namespace qilin::host {
// Unique sibling temporary, checked write/flush/close, then atomic replacement.
// Failure before replacement preserves the destination, including on Windows.
bool read_file(const std::filesystem::path& path, std::string& bytes, bool* missing = nullptr, size_t limit = std::numeric_limits<size_t>::max());
// Stream a regular file through the same checked replacement path.
bool atomic_copy(const std::filesystem::path& source, const std::filesystem::path& destination);
// Checked in-place updates for the VMU's small packets; a new file gets the
// complete image. Existing files must already have the expected size.
bool write_range(const std::filesystem::path& destination, const uint8_t* image, size_t size, size_t offset, size_t count);
bool atomic_write(const std::filesystem::path& destination, std::string_view text);
}
