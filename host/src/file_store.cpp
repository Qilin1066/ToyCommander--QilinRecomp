#include "qilin/host/file_store.hpp"
#include <span>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace qilin::host {
bool read_file(const std::filesystem::path& path, std::string& bytes, bool* missing, size_t limit) {
    if (missing) *missing = false;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) {
        if (missing) *missing = true;
        return false;
    }
    if (ec) return false;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    if (std::filesystem::file_size(path, ec) > limit || ec) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string candidate;
    char block[65536];
    for (;;) {
        input.read(block, sizeof(block));
        const size_t count = static_cast<size_t>(input.gcount());
        if (count > limit - candidate.size()) return false;
        candidate.append(block, count);
        if (input.bad()) return false;
        if (input.eof()) break;
        if (input.fail()) return false;
    }
    bytes = std::move(candidate);
    return true;
}

namespace {
template <typename Write>
bool replace_file(const std::filesystem::path& destination, Write write) {
    static std::atomic<unsigned> serial{0};
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    std::filesystem::path temporary;
    std::FILE* file = nullptr;
    for (int attempt = 0; attempt < 32 && !file; ++attempt) {
        temporary = destination;
        temporary += ".tmp." + std::to_string(pid) + "." + std::to_string(serial.fetch_add(1));
#ifdef _WIN32
        file = _wfopen(temporary.c_str(), L"wbx");
#else
        file = std::fopen(temporary.c_str(), "wbx");
#endif
        if (!file && errno != EEXIST) break;
    }
    if (!file) return false;
    bool ok = write(file) && std::fflush(file) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(file)) == 0;
#else
    if (ok) ok = fsync(fileno(file)) == 0;
#endif
    if (std::fclose(file) != 0) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code ec;
        std::filesystem::rename(temporary, destination, ec);
        ok = !ec;
        if (ok) {
            const auto parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
            const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
            if (directory >= 0) { (void)fsync(directory); close(directory); }
        }
#endif
    }
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return ok;
}
} // namespace

bool atomic_write(const std::filesystem::path& destination, std::string_view text) {
    return replace_file(destination, [&](std::FILE* file) {
        return std::fwrite(text.data(), 1, text.size(), file) == text.size();
    });
}

bool write_range(const std::filesystem::path& destination, const uint8_t* image, size_t size, size_t offset, size_t count) {
    if (offset > size || count > size - offset) return false;
#ifdef _WIN32
    std::FILE* file = _wfopen(destination.c_str(), L"r+b");
#else
    std::FILE* file = std::fopen(destination.c_str(), "r+b");
#endif
    if (!file) {
        if (errno != ENOENT) return false;  // never truncate an unreadable existing file
        return atomic_write(destination, {reinterpret_cast<const char*>(image), size});
    }
    // VMU images are 128 KiB; long offsets are sufficient on every target.
    bool ok = size <= static_cast<size_t>(LONG_MAX) && std::fseek(file, 0, SEEK_END) == 0 && std::ftell(file) == static_cast<long>(size);
    if (ok) ok = std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 && std::fwrite(image + offset, 1, count, file) == count;
    if (std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    return ok;
}

bool atomic_copy(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec) return false;
    std::ifstream input(source, std::ios::binary);
    if (!input) return false;
    return replace_file(destination, [&](std::FILE* file) {
        std::array<char, 65536> buffer;
        for (;;) {
            input.read(buffer.data(), buffer.size());
            const auto count = static_cast<size_t>(input.gcount());
            if (input.bad() || (input.fail() && !input.eof()) || std::fwrite(buffer.data(), 1, count, file) != count) return false;
            if (input.eof()) break;
        }
        // Close before replacement, including when source == destination on Windows.
        input.clear(); input.close();
        return !input.fail();
    });
}
} // namespace qilin::host
