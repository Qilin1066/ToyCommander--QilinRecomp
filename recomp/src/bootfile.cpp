#include "bootfile.hpp"
#include "path.hpp"

#include <fstream>
#include <iterator>

#include "sha256.hpp"

namespace qilin {

namespace {

constexpr KnownBuild kKnownBuilds[] = {
    // USA (En,Fr,De,Es). Product number, version and date as the disc's
    // bootstrap sector gives them; docs/disc-analysis.md has what was checked.
    {"ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd", "MK-57020", "V1.022",
     "Toy Commander (USA)"},
    // Toy Racer, Europe (En,Fr,De,Es): `1TOYRACE.BIN`.
    {"da29a065f9db779f0c95d778de36c019848b211ad93a667244bf58e9a10ee5d4", "MK-5114950", "V1.050",
     "Toy Racer (Europe)"},
};

std::string hex32(uint32_t v) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string s = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) s += kHex[(v >> shift) & 15];
    return s;
}

std::unexpected<std::string> fail(std::string message) { return std::unexpected(std::move(message)); }

}  // namespace

std::span<const KnownBuild> known_builds() { return kKnownBuilds; }

const KnownBuild* known_build(std::string_view sha256_hex) {
    for (const KnownBuild& b : kKnownBuilds)
        if (b.sha256 == sha256_hex) return &b;
    return nullptr;
}

std::expected<BootFile, std::string> BootFile::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + qilin::utf8_path(path));
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) return fail("cannot read " + qilin::utf8_path(path));
    return parse(bytes);
}

std::expected<BootFile, std::string> BootFile::parse(std::span<const uint8_t> file) {
    if (file.empty()) return fail("not a boot file: it is empty");
    if (file.size() % 2) return fail("not a boot file: an odd number of bytes (SH-4 instructions are 16-bit words)");
    const uint64_t room = uint64_t(kRamBase) + kRamSize - kBootFileBase;
    if (file.size() > room)
        return fail("not a boot file: " + std::to_string(file.size()) + " bytes don't fit in the Dreamcast's RAM above " +
                    hex32(kBootFileBase));
    BootFile boot;
    boot.image_.assign(file.begin(), file.end());
    boot.sha256 = sha256_hex(file);
    return boot;
}

uint8_t BootFile::u8(uint32_t a) const {
    if (!mapped(a)) throw BootFileError("read outside the boot file at " + hex32(a));
    return image_[a - kBootFileBase];
}

uint16_t BootFile::u16(uint32_t a) const {
    if (!mapped(a, 2)) throw BootFileError("read outside the boot file at " + hex32(a));
    return read_le16(image_.data() + (a - kBootFileBase));
}

uint32_t BootFile::u32(uint32_t a) const {
    if (!mapped(a, 4)) throw BootFileError("read outside the boot file at " + hex32(a));
    return read_le32(image_.data() + (a - kBootFileBase));
}

}  // namespace qilin
