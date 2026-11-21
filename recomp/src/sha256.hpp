// SHA-256 (FIPS 180-4), to recognise the one executable this port supports.
// Runtime code: no std::format (see CMakeLists.txt on macOS 10.15).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace qilin {

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
std::string sha256_hex(std::span<const uint8_t> data);  // lower-case hex

}  // namespace qilin
