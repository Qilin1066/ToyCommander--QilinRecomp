// The kit's small text files, as the launcher reads them. Portable (no
// platform headers), tested by launcher_test.cpp.
//
//   kit\game.txt   name=value a line (package.sh writes it): the game's names
//   kit\build-flags.txt  `name = value` lines with $kit for the kit's
//                  folder (CMake writes it): cxxflags, ldflags, libs
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::launcher {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

// `name=value` (or `name = value`) lines; lines starting with '#' and lines
// without '=' are skipped. A later line with the same name wins.
inline std::map<std::string, std::string, std::less<>> parse_settings(std::string_view text) {
    std::map<std::string, std::string, std::less<>> out;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);  // a UTF-8 byte-order mark
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        out[std::string(trim(line.substr(0, eq)))] = std::string(trim(line.substr(eq + 1)));
    }
    return out;
}

// Splits a command-line fragment at spaces (the kit's flags have no quoted
// parts) and puts `kit` for every `$kit`.
inline std::vector<std::string> split_flags(std::string_view flags, std::string_view kit) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < flags.size()) {
        while (at < flags.size() && (flags[at] == ' ' || flags[at] == '\t')) ++at;
        std::size_t end = at;
        while (end < flags.size() && flags[end] != ' ' && flags[end] != '\t') ++end;
        if (end > at) {
            std::string word(flags.substr(at, end - at));
            for (std::size_t p = word.find("$kit"); p != std::string::npos; p = word.find("$kit", p + kit.size()))
                word.replace(p, 4, kit);
            out.push_back(std::move(word));
        }
        at = end;
    }
    return out;
}

}  // namespace qilin::launcher
