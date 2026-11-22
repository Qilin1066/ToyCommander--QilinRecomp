#pragma once
// UTF-8/native filesystem conversion, with no platform API headers.
#include <filesystem>
#include <string>
#include <string_view>

namespace qilin {
inline std::filesystem::path native_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace qilin
