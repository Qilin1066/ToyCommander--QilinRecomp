#pragma once
// Windows' narrow main() arguments use the active code page. All runtime
// paths are UTF-8, so read the original UTF-16 command line there instead.
#include "path.hpp"
#include <string_view>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <memory>
#include <vector>
#endif

namespace qilin {
class CommandLine {
public:
    CommandLine(int count, char** values) : count_(count), values_(values) {
#ifdef _WIN32
        int wide_count = 0;
        auto release = [](wchar_t** pointer) { LocalFree(pointer); };
        std::unique_ptr<wchar_t*, decltype(release)> wide(CommandLineToArgvW(GetCommandLineW(), &wide_count), release);
        if (!wide) throw std::runtime_error("Cannot read Windows command line");
        strings_.reserve(static_cast<std::size_t>(wide_count));
        for (int i = 0; i < wide_count; ++i) {
            const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get()[i], -1, nullptr, 0, nullptr, nullptr);
            if (!bytes) throw std::runtime_error("Invalid Unicode command line");
            std::string value(static_cast<std::size_t>(bytes), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get()[i], -1, value.data(), bytes, nullptr, nullptr))
                throw std::runtime_error("Cannot convert Windows command line");
            value.pop_back();
            strings_.push_back(std::move(value));
        }
        pointers_.reserve(strings_.size() + 1);
        for (auto& value : strings_) pointers_.push_back(value.data());
        pointers_.push_back(nullptr);
        count_ = wide_count;
        values_ = pointers_.data();
#endif
    }
    CommandLine(const CommandLine&) = delete;
    CommandLine& operator=(const CommandLine&) = delete;
    [[nodiscard]] int count() const { return count_; }
    [[nodiscard]] char** values() const { return values_; }
private:
    int count_;
    char** values_;
#ifdef _WIN32
    std::vector<std::string> strings_;
    std::vector<char*> pointers_;
#endif
};
} // namespace qilin
