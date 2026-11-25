#include "flash.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <filesystem>
#include <string>
#include <vector>

#include "guest.hpp"
#include "qilin/host/paths.hpp"
#include "save_history.hpp"
#include "qilin/host/file_store.hpp"
#include "path.hpp"
#include "system.hpp"

namespace dreamcast::flash {

namespace rt = qilin::rt;

namespace {

// The console's flash memory: 128 KB, its partitions where the console has
// them (0 and 1 the factory's and a reserved one, 2 the system's settings,
// 3 and 4 for games and the network settings). Only their sizes matter to
// Shinobi's format: it counts its 64-byte blocks from them.
constexpr uint32_t kBytes = 0x20000u;
constexpr struct {
    uint32_t offset, size;
} kPartitions[] = {{0x1A000u, 0x2000u}, {0x18000u, 0x2000u}, {0x1C000u, 0x4000u}, {0x10000u, 0x8000u}, {0x00000u, 0x10000u}};

std::vector<uint8_t> g_flash;
std::filesystem::path g_file;

void load() {
    if (!g_flash.empty()) return;
    g_file = qilin::host::user_folder() / "flash.bin";
    g_flash.assign(kBytes, 0xFF);
    std::string bytes;
    bool missing = false;
    if (qilin::host::read_file(g_file, bytes, &missing, g_flash.size())) {
        if (bytes.size() != g_flash.size()) rt::fatal("the console flash memory has the wrong size");
        std::copy(bytes.begin(), bytes.end(), g_flash.begin());
    } else if (!missing) rt::fatal("cannot read the existing console flash memory; it has been left unchanged");
}

// The whole file, through a temporary one (a crash leaves the old). A save
// is several writes (Shinobi writes 64-byte blocks, after an erase): the
// file as it was before the first of them (the first after two seconds
// without one) is kept in the saves' history (save_history.hpp), as the
// memory card's is, so the history's few copies are whole saves, not one
// save's steps.
std::chrono::steady_clock::time_point g_last_save{};

void save() {
    std::string error;
    const auto now = std::chrono::steady_clock::now();
    if (g_last_save == std::chrono::steady_clock::time_point{} || now - g_last_save > std::chrono::seconds(2))
        if (!backup_save(g_file.parent_path(), g_file.filename().string(), error))
            rt::fatal("cannot keep a copy of %s before saving: %s", qilin::utf8_path(g_file).c_str(), error.c_str());
    g_last_save = now;
    if (!write_file_safely(g_file, g_flash.data(), g_flash.size(), error)) rt::fatal("%s", error.c_str());
}

bool inside(uint32_t offset, uint32_t n) { return offset <= kBytes && n <= kBytes - offset; }

}  // namespace

bool info(uint32_t partition, uint32_t& offset, uint32_t& size) {
    if (partition >= std::size(kPartitions)) return false;
    offset = kPartitions[partition].offset, size = kPartitions[partition].size;
    return true;
}

int32_t read(uint32_t offset, uint32_t to, uint32_t n) {
    load();
    if (!inside(offset, n)) return -1;
    for (uint32_t k = 0; k < n; ++k) rt::st8(to + k, g_flash[offset + k]);
    return static_cast<int32_t>(n);
}

int32_t write(uint32_t offset, uint32_t from, uint32_t n) {
    load();
    if (!inside(offset, n)) return -1;
    for (uint32_t k = 0; k < n; ++k) g_flash[offset + k] &= static_cast<uint8_t>(rt::ld8(from + k));
    save();
    return static_cast<int32_t>(n);
}

int32_t erase(uint32_t offset) {
    load();
    for (const auto& p : kPartitions)
        if (p.offset == offset) {
            std::fill_n(g_flash.begin() + p.offset, p.size, uint8_t{0xFF});
            save();
            return 0;
        }
    return -1;
}

}  // namespace dreamcast::flash
