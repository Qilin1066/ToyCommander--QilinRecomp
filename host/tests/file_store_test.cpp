#include "qilin/host/file_store.hpp"
#include "path.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace fs = std::filesystem;
namespace io = qilin::host;
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); } };
    const auto dir = fs::temp_directory_path() / fs::path(u8"qilin_storage_保_é");
    fs::remove_all(dir); fs::create_directories(dir);
    const auto file = dir / fs::path(u8"card_保存.bin");
    std::string bytes = "unchanged";
    bool missing = false;
    check(!io::read_file(file, bytes, &missing) && missing && bytes == "unchanged", "missing read preserves the output");
    check(!io::read_file(dir, bytes, &missing) && !missing && bytes == "unchanged", "a directory is not a missing save");
    check(io::atomic_write(file, "one") && io::atomic_write(file, "two"), "replace an existing Unicode file");
    check(io::read_file(file, bytes) && bytes == "two", "read the replacement");
#ifdef _WIN32
    const HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "lock fixture");
    if (locked != INVALID_HANDLE_VALUE) {
        check(!io::atomic_write(file, "blocked"), "a locked destination refuses replacement");
        CloseHandle(locked);
        check(io::read_file(file, bytes) && bytes == "two", "locked replacement preserves original bytes");
    }
#endif
    bytes = "preserved";
    check(!io::read_file(file, bytes, &missing, 2) && !missing && bytes == "preserved", "oversized image refused before reading");
    auto legacy = file; legacy += ".tmp";
    check(io::atomic_write(legacy, "unrelated"), "stale temporary fixture");
    check(io::atomic_write(file, "three") && io::read_file(legacy, bytes) && bytes == "unrelated", "do not clobber another temporary file");
    std::string pattern(2 * 65536 + 17, '\0');
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<char>(i % 251);
    const auto copy = dir / "copy.bin";
    check(io::atomic_write(file, pattern) && io::atomic_copy(file, copy), "stream a copy over multiple chunks");
    check(io::read_file(copy, bytes) && bytes == pattern, "copy retains binary bytes");
    check(io::atomic_copy(copy, copy) && io::read_file(copy, bytes) && bytes == pattern, "copy to self");
    check(!io::atomic_copy(dir / "missing", copy) && io::read_file(copy, bytes) && bytes == pattern, "failed copy keeps destination");
    const auto blocked = dir / "blocked";
    fs::create_directory(blocked);
    check(!io::atomic_write(blocked, "no") && fs::is_directory(blocked), "failed replacement keeps its destination");
    std::vector<uint8_t> image(131072, 0x12);
    const auto card = dir / "memory_card_a1.bin";
    check(io::write_range(card, image.data(), image.size(), 128, 128), "a new VMU file gets the complete image");
    std::fill_n(image.begin() + 128, 128, uint8_t{0xAB});
    check(io::write_range(card, image.data(), image.size(), 128, 128), "a small VMU packet is written");
    check(io::read_file(card, bytes) && bytes.size() == image.size() &&
          std::equal(image.begin(), image.end(), bytes.begin(), [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); }), "packet leaves surrounding card data unchanged");
    check(!io::write_range(card, image.data(), image.size(), image.size() - 1, 2), "invalid packet range rejected");
    check(io::atomic_write(card, "short") && !io::write_range(card, image.data(), image.size(), 0, 128) &&
          io::read_file(card, bytes) && bytes == "short", "a malformed existing card is not overwritten");
    check(!io::write_range(blocked, image.data(), image.size(), 0, 128) && fs::is_directory(blocked), "an unopenable existing card is not truncated");
    check(io::atomic_write(copy, "") && io::read_file(copy, bytes) && bytes.empty(), "empty files");
    for (const auto& entry : fs::directory_iterator(dir)) check(entry.path() == legacy || qilin::utf8_path(entry.path().filename()).find(".tmp.") == std::string::npos, "temporary files cleaned up");
    fs::remove_all(dir);
    std::printf("file_store_test: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
