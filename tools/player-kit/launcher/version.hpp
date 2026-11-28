// Release versions and GitHub's releases list, for the launcher's Update
// button. Portable (no platform headers), tested by launcher_test.cpp.
#pragma once

#include <charconv>
#include <compare>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "json.hpp"

namespace qilin::launcher {

// X.Y.Z, as a game's tag (`toycommander-v0.13.0`) and kit\VERSION have it.
// An untagged build's VERSION is `git describe`'s: X.Y.Z-N-gHASH, N commits
// after X.Y.Z (`ahead`), so newer than X.Y.Z and older than anything after.
struct Version {
    unsigned major = 0, minor = 0, patch = 0;
    unsigned ahead = 0;
    auto operator<=>(const Version&) const = default;
};

namespace detail {
inline bool read_number(std::string_view& s, unsigned& out) {
    const char* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, out);
    if (ec != std::errc{} || p == s.data()) return false;
    s.remove_prefix(static_cast<std::size_t>(p - s.data()));
    return true;
}
}  // namespace detail

// "0.13.0", "v0.13.0", "0.13.0-5-gabc1234" (with exact = false). Anything
// else (a bare commit hash, a missing part, trailing text) is no version.
inline std::optional<Version> parse_version(std::string_view s, bool exact = false) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
    if (!s.empty() && s.front() == 'v') s.remove_prefix(1);
    Version v;
    if (!detail::read_number(s, v.major) || !s.starts_with('.')) return std::nullopt;
    s.remove_prefix(1);
    if (!detail::read_number(s, v.minor) || !s.starts_with('.')) return std::nullopt;
    s.remove_prefix(1);
    if (!detail::read_number(s, v.patch)) return std::nullopt;
    if (s.empty()) return v;
    if (exact || !s.starts_with('-')) return std::nullopt;
    s.remove_prefix(1);
    if (!detail::read_number(s, v.ahead) || !s.starts_with("-g") || s.size() < 3) return std::nullopt;
    return v;
}

inline std::string to_string(const Version& v) {
    std::string s = std::to_string(v.major) + '.' + std::to_string(v.minor) + '.' + std::to_string(v.patch);
    if (v.ahead) s += " (+" + std::to_string(v.ahead) + ")";
    return s;
}

// The newest published release of one game in GitHub's releases list (the
// JSON array https://api.github.com/repos/<owner>/<repo>/releases returns):
// tags `<game>-vX.Y.Z`, neither drafts nor pre-releases, with the download
// `<package>-X.Y.Z-<suffix>` attached (windows-x64.zip, linux-x86_64.tar.gz,
// linux-arm64.tar.gz, macos.dmg: platform.hpp's release_asset_suffix).
struct Release {
    Version version;
    std::string tag;
    std::string asset_name;
    std::string asset_url;  // browser_download_url
    double asset_size = 0;  // bytes, as GitHub gives it
};

// `tag_prefix` and `asset_prefix` are game.txt's release_tag_prefix
// (`<game>-v` by default) and release_asset_prefix (`<package>-`).
inline std::expected<std::optional<Release>, std::string> newest_release(std::string_view releases_json, std::string_view tag_prefix,
                                                                         std::string_view asset_prefix,
                                                                         std::string_view suffix = "windows-x64.zip") {
    auto doc = json::parse(releases_json);
    if (!doc) return std::unexpected(doc.error());
    if (!doc->is_array()) return std::unexpected(std::string("GitHub's answer isn't a list of releases"));
    const std::string prefix(tag_prefix);
    const std::string package(asset_prefix);
    std::optional<Release> best;
    for (const json::Value& r : doc->items) {
        if (!r.is_object() || r.bool_or("draft", true) || r.bool_or("prerelease", true)) continue;
        const std::string_view tag = r.string_or("tag_name", "");
        if (!tag.starts_with(prefix)) continue;
        const auto v = parse_version(tag.substr(prefix.size()), true);
        if (!v || (best && *v <= best->version)) continue;
        const std::string want = package + std::string(tag.substr(prefix.size())) + "-" + std::string(suffix);
        const json::Value* assets = r.find("assets");
        if (!assets || !assets->is_array()) continue;
        for (const json::Value& a : assets->items) {
            if (a.string_or("name", "") != want) continue;
            const std::string_view url = a.string_or("browser_download_url", "");
            if (!url.starts_with("https://")) continue;
            Release found;
            found.version = *v;
            found.tag = std::string(tag);
            found.asset_name = want;
            found.asset_url = std::string(url);
            if (const json::Value* size = a.find("size"); size && size->is_number()) found.asset_size = size->number;
            best = std::move(found);
            break;
        }
    }
    return best;
}

}  // namespace qilin::launcher
