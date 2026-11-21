// The game's boot file: the executable the Dreamcast's boot ROM loads.
//
// A GD-ROM names its boot file in its bootstrap sector (IP.BIN; see
// gdrom.hpp): `1GUTH.BIN` on Toy Commander's disc, `1TOYRACE.BIN` on Toy Racer's. The file has no header and
// no sections: it is the bytes of memory from 0x8C010000 up, code and
// initialised data together, and execution starts at its first byte. Where
// the code ends and what the BSS is are found by analysis, not read here.
//
// Runtime code: no std::format (see CMakeLists.txt).
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace qilin {

// Thrown only for programming errors: reading outside the image.
struct BootFileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The Dreamcast's 16 MB of RAM, as the game addresses it (area 3 through the
// cached P1 region), and where the boot ROM puts the boot file.
inline constexpr uint32_t kRamBase = 0x8C000000u;
inline constexpr uint32_t kRamSize = 16u << 20;
inline constexpr uint32_t kBootFileBase = 0x8C010000u;

// Executables this port supports, recognised by the SHA-256 of the boot file.
struct KnownBuild {
    std::string_view sha256;
    std::string_view product;  // the disc's product number
    std::string_view version;  // the disc's version
    std::string_view title;
};
const KnownBuild* known_build(std::string_view sha256_hex);

class BootFile {
public:
    // Errors (unreadable file, empty, odd length, larger than RAM) come back
    // as a message, never a crash.
    static std::expected<BootFile, std::string> load(const std::filesystem::path& path);
    static std::expected<BootFile, std::string> parse(std::span<const uint8_t> file);

    std::string sha256;  // of the whole file, lower-case hex

    uint32_t base() const { return kBootFileBase; }
    uint32_t entry() const { return kBootFileBase; }
    std::span<const uint8_t> image() const { return image_; }
    uint32_t size() const { return static_cast<uint32_t>(image_.size()); }
    uint32_t end() const { return kBootFileBase + size(); }
    bool mapped(uint32_t a, uint32_t n = 1) const {
        return a >= kBootFileBase && uint64_t(a) + n <= end();
    }
    uint8_t u8(uint32_t a) const;
    uint16_t u16(uint32_t a) const;  // little-endian
    uint32_t u32(uint32_t a) const;

private:
    std::vector<uint8_t> image_;
};

// `qilin info` output (in qilin_core: developer tools only).
std::string to_json(const BootFile& boot);

inline uint16_t read_le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t read_le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

}  // namespace qilin
