// The kit's small text files, as the launcher reads them. Portable (no
// platform headers), tested by launcher_test.cpp.
//
//   kit\game.txt   name=value a line (package.sh writes it): the game's names,
//                  its data, how it is recompiled and started (ADOPTING.md)
//   kit\build-flags.txt  `name = value` lines with $kit for the kit's
//                  folder (CMake writes it): cxxflags, ldflags, libs
#pragma once

#include <expected>
#include <map>
#include <optional>
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

// game.txt's commands (`identify`, `step.N`, `run_args`): split into
// arguments at spaces and tabs, the launcher's own rules, no shell. A part in
// double or single quotes keeps its spaces (and the other kind of quote);
// quoted and plain text side by side make one argument (--out="a b" is
// `--out=a b`). There are no escapes, so Windows paths keep their
// backslashes. An unclosed quote is an error.
inline std::expected<std::vector<std::string>, std::string> split_command(std::string_view text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
        if (at >= text.size()) break;
        std::string word;
        while (at < text.size() && text[at] != ' ' && text[at] != '\t') {
            const char c = text[at];
            if (c == '"' || c == '\'') {
                const std::size_t close = text.find(c, at + 1);
                if (close == std::string_view::npos) return std::unexpected("an unclosed " + std::string(1, c) + " in: " + std::string(text));
                word.append(text.substr(at + 1, close - at - 1));
                at = close + 1;
            } else {
                word += c;
                ++at;
            }
        }
        out.push_back(std::move(word));
    }
    return out;
}

// Puts each `{name}` in one argument through `lookup` (which returns the
// value, or nothing for a name it doesn't know: an error). `{{` and `}}` are
// a brace. Done per argument, after splitting, so a value with spaces stays
// one argument.
template <typename Lookup>
std::expected<std::string, std::string> expand_placeholders(std::string_view arg, const Lookup& lookup) {
    std::string out;
    for (std::size_t at = 0; at < arg.size();) {
        const char c = arg[at];
        if ((c == '{' || c == '}') && at + 1 < arg.size() && arg[at + 1] == c) {
            out += c;
            at += 2;
            continue;
        }
        if (c == '}') return std::unexpected("a lone } in " + std::string(arg));
        if (c != '{') {
            out += c;
            ++at;
            continue;
        }
        const std::size_t close = arg.find('}', at + 1);
        if (close == std::string_view::npos)
            return std::unexpected("an unclosed { in " + std::string(arg) + " (a placeholder with a space in it needs quotes)");
        const std::string_view name = arg.substr(at + 1, close - at - 1);
        const std::optional<std::string> value = lookup(name);
        if (!value) return std::unexpected("an unknown placeholder {" + std::string(name) + "}");
        out += *value;
        at = close + 1;
    }
    return out;
}

// "cue;gdi;chd" (or ".cue, .GDI"): lower-case extensions without the dot.
inline std::vector<std::string> split_extensions(std::string_view text) {
    std::vector<std::string> out;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) out.push_back(std::move(word));
        word.clear();
    };
    for (const char c : text) {
        if (c == ';' || c == ',' || c == ' ' || c == '\t') flush();
        else if (c == '.' && word.empty()) continue;
        else word += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    flush();
    return out;
}

// ".cue, .gdi or .chd"
inline std::string list_extensions(const std::vector<std::string>& extensions) {
    std::string out;
    for (std::size_t i = 0; i < extensions.size(); ++i) {
        if (i) out += i + 1 == extensions.size() ? " or " : ", ";
        out += "." + extensions[i];
    }
    return out;
}

// "disc" as "Disc".
inline std::string capitalised(std::string s) {
    if (!s.empty() && s.front() >= 'a' && s.front() <= 'z') s.front() = static_cast<char>(s.front() - 'a' + 'A');
    return s;
}

}  // namespace qilin::launcher
