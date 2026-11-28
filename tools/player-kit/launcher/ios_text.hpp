// What "Play on iPad" (ios_install.hpp, macOS only) reads from Apple's tools,
// as plain functions over their text: portable (no platform headers), tested
// by launcher_test.cpp with canned samples.
//
//   xcrun devicectl list devices --json-output <file>      the devices
//   xcrun devicectl device info files ... --json-output     files in the app's Documents
//   security find-identity -v -p codesigning                the signing identities
//   security find-certificate -a -p -c <name>               their certificates (PEM): the team is the subject's OU
//   defaults export com.apple.dt.Xcode (as XML)             Xcode's accounts' teams
//   security cms -D -i embedded.mobileprovision             the profile (an XML plist): its expiry
//   a .cue's FILE lines, a .gdi's track lines               the files a disc image is made of
//
// Each reader takes what Apple documents (or, where it doesn't, what its
// tools print) and is tolerant of members it doesn't know; the parts that
// rest on observed rather than documented output are marked "assumed".
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "kit_text.hpp"

namespace qilin::launcher::ios {

// ---- The iOS kit's layout (Contents/Resources/ios-kit) ----

// Where the parts are, from ios-kit/build-flags.txt's optional keys (the
// defaults are the agreed layout): everything "Play on iPad" needs to know
// about the kit is here, so a renamed part is one line in that file.
struct KitLayout {
    std::string kit_library;       // `kit_lib`: lib/lib<game>_ios_kit.a
    std::string project;           // `xcode_project`: xcode/QilinGame.xcodeproj
    std::string scheme;            // `xcode_scheme`: QilinGame
    std::string lifted_setting;    // `lifted_lib_setting`: QILIN_LIFTED_LIB, the build setting naming the lifted code's archive
    std::string kit_setting;       // `kit_lib_setting`: QILIN_KIT_LIB, the one naming the kit's
    std::string configuration;     // `xcode_configuration`: Release
};

inline KitLayout kit_layout(const std::map<std::string, std::string, std::less<>>& flags, std::string_view game) {
    const auto get = [&](std::string_view key, std::string fallback) {
        const auto it = flags.find(key);
        return it == flags.end() || it->second.empty() ? fallback : it->second;
    };
    KitLayout k;
    k.kit_library = get("kit_lib", "lib/lib" + std::string(game) + "_ios_kit.a");
    k.project = get("xcode_project", "xcode/QilinGame.xcodeproj");
    k.scheme = get("xcode_scheme", "QilinGame");
    k.lifted_setting = get("lifted_lib_setting", "QILIN_LIFTED_LIB");
    k.kit_setting = get("kit_lib_setting", "QILIN_KIT_LIB");
    k.configuration = get("xcode_configuration", "Release");
    return k;
}

// ---- A small XML property list reader ----
//
// Into json::Value: <dict> an object, <array> an array, <string>, <date>
// (its ISO 8601 text) and <data> (its base64 text) strings, <integer> and
// <real> numbers, <true/> and <false/> booleans. Comments, the XML
// declaration and the DOCTYPE are skipped. Anything else is an error.

namespace detail {

class PlistParser {
public:
    explicit PlistParser(std::string_view text) : s_(text) {}

    std::expected<json::Value, std::string> document() {
        skip_prolog();
        std::string name;
        bool empty = false;
        if (!open_tag(name, empty) || name != "plist") return std::unexpected(std::string("plist: no <plist> element"));
        json::Value v;
        if (empty) return v;
        if (!value(v, 0)) return std::unexpected(error_);
        if (!close_tag("plist")) return std::unexpected(error_);
        return v;
    }

private:
    std::string_view s_;
    std::size_t at_ = 0;
    std::string error_;

    bool stop(std::string_view what) {
        error_ = "plist: " + std::string(what) + " at byte " + std::to_string(at_);
        return false;
    }
    void space() {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) ++at_;
    }
    // Spaces, comments, <?...?> and <!DOCTYPE ...>.
    void skip_prolog() {
        for (;;) {
            space();
            if (s_.substr(at_, 4) == "<!--") {
                const std::size_t end = s_.find("-->", at_ + 4);
                at_ = end == std::string_view::npos ? s_.size() : end + 3;
            } else if (s_.substr(at_, 2) == "<?") {
                const std::size_t end = s_.find("?>", at_ + 2);
                at_ = end == std::string_view::npos ? s_.size() : end + 2;
            } else if (s_.substr(at_, 2) == "<!") {
                const std::size_t end = s_.find('>', at_ + 2);
                at_ = end == std::string_view::npos ? s_.size() : end + 1;
            } else {
                return;
            }
        }
    }
    // <name ...> or <name .../>: attributes are skipped.
    bool open_tag(std::string& name, bool& empty) {
        skip_prolog();
        if (at_ >= s_.size() || s_[at_] != '<' || s_.substr(at_, 2) == "</") return stop("no element where one should be");
        const std::size_t end = s_.find('>', at_);
        if (end == std::string_view::npos) return stop("an element that never closes");
        std::string_view inside = s_.substr(at_ + 1, end - at_ - 1);
        empty = !inside.empty() && inside.back() == '/';
        if (empty) inside.remove_suffix(1);
        const std::size_t name_end = std::min(inside.find_first_of(" \t\r\n"), inside.size());
        name = std::string(inside.substr(0, name_end));
        at_ = end + 1;
        return !name.empty() || stop("an element without a name");
    }
    bool close_tag(std::string_view name) {
        skip_prolog();
        const std::string want = "</" + std::string(name);
        if (s_.substr(at_, want.size()) != want) return stop("no </" + std::string(name) + ">");
        at_ += want.size();
        space();
        if (at_ >= s_.size() || s_[at_] != '>') return stop("a closing tag that never closes");
        ++at_;
        return true;
    }
    static void put_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    // The text up to the next '<', entities decoded.
    bool text(std::string& out) {
        const std::size_t end = s_.find('<', at_);
        if (end == std::string_view::npos) return stop("text that never ends");
        const std::string_view raw = s_.substr(at_, end - at_);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            const std::size_t semi = raw.find(';', i);
            if (semi == std::string_view::npos) return stop("an entity without ';'");
            const std::string_view entity = raw.substr(i + 1, semi - i - 1);
            if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "amp") out += '&';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (entity.size() > 1 && entity[0] == '#') {
                std::uint32_t cp = 0;
                const bool hex = entity[1] == 'x' || entity[1] == 'X';
                const std::string_view digits = entity.substr(hex ? 2 : 1);
                if (digits.empty()) return stop("an empty character reference");
                for (const char c : digits) {
                    std::uint32_t d = 0;
                    if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
                    else if (hex && c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
                    else if (hex && c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(c - 'A' + 10);
                    else return stop("a bad character reference");
                    cp = cp * (hex ? 16u : 10u) + d;
                    if (cp > 0x10FFFF) return stop("a character reference out of range");
                }
                put_utf8(out, cp);
            } else {
                return stop("an unknown entity");
            }
            i = semi;
        }
        at_ = end;
        return true;
    }
    bool value(json::Value& out, int depth) {
        if (depth > 100) return stop("nesting too deep");
        std::string name;
        bool empty = false;
        if (!open_tag(name, empty)) return false;
        if (name == "true" || name == "false") {
            out.kind = json::Value::Kind::Bool;
            out.boolean = name == "true";
            return empty || close_tag(name);
        }
        if (name == "string" || name == "date" || name == "data" || name == "integer" || name == "real") {
            std::string body;
            if (!empty && (!text(body) || !close_tag(name))) return false;
            if (name == "integer" || name == "real") {
                const std::string_view t = trim(body);
                if (t.empty()) return stop("an empty number");
                char* end = nullptr;
                const std::string copy(t);
                out.kind = json::Value::Kind::Number;
                out.number = std::strtod(copy.c_str(), &end);
                if (end != copy.c_str() + copy.size()) return stop("a bad number");
                return true;
            }
            out.kind = json::Value::Kind::String;
            out.string = name == "string" ? std::move(body) : std::string(trim(body));
            return true;
        }
        if (name == "array") {
            out.kind = json::Value::Kind::Array;
            if (empty) return true;
            for (;;) {
                skip_prolog();
                if (s_.substr(at_, 2) == "</") return close_tag("array");
                json::Value item;
                if (!value(item, depth + 1)) return false;
                out.items.push_back(std::move(item));
            }
        }
        if (name == "dict") {
            out.kind = json::Value::Kind::Object;
            if (empty) return true;
            for (;;) {
                skip_prolog();
                if (s_.substr(at_, 2) == "</") return close_tag("dict");
                std::string key_tag;
                bool key_empty = false;
                if (!open_tag(key_tag, key_empty)) return false;
                if (key_tag != "key") return stop("a <dict> entry without its <key>");
                json::Member m;
                if (!key_empty && (!text(m.key) || !close_tag("key"))) return false;
                if (!value(m.value, depth + 1)) return false;
                out.members.push_back(std::move(m));
            }
        }
        return stop("an unknown element <" + name + ">");
    }
};

}  // namespace detail

inline std::expected<json::Value, std::string> parse_plist(std::string_view xml) { return detail::PlistParser(xml).document(); }

// ---- Dates ----

// Days since 1970-01-01 of a date in the proleptic Gregorian calendar
// (Howard Hinnant's days_from_civil).
inline std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    if (m <= 2) --y;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "2026-12-05T10:20:30Z" (a plist's <date>, devicectl's dates: UTC, with an
// optional fraction or +hh:mm offset) as seconds since 1970; nothing if it
// isn't one.
inline std::optional<std::int64_t> parse_iso8601(std::string_view s) {
    s = trim(s);
    const auto num = [&](std::size_t at, std::size_t n) -> std::optional<int> {
        if (at + n > s.size()) return std::nullopt;
        int v = 0;
        for (std::size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return std::nullopt;
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    const auto y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2), sec = num(17, 2);
    if (!y || !mo || !d || !h || !mi || !sec || s.size() < 20 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') ||
        s[13] != ':' || s[16] != ':')
        return std::nullopt;
    if (*mo < 1 || *mo > 12 || *d < 1 || *d > 31 || *h > 23 || *mi > 59 || *sec > 60) return std::nullopt;
    std::size_t at = 19;
    if (at < s.size() && s[at] == '.') {
        ++at;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
    }
    std::int64_t offset = 0;
    if (at < s.size() && s[at] == 'Z') {
        ++at;
    } else if (at < s.size() && (s[at] == '+' || s[at] == '-')) {
        const auto oh = num(at + 1, 2);
        const std::size_t colon = at + 3 < s.size() && s[at + 3] == ':' ? 1 : 0;
        const auto om = num(at + 3 + colon, 2);
        if (!oh || !om) return std::nullopt;
        offset = (s[at] == '+' ? 1 : -1) * (static_cast<std::int64_t>(*oh) * 3600 + static_cast<std::int64_t>(*om) * 60);
        at += 5 + colon;
    } else {
        return std::nullopt;
    }
    if (at != s.size()) return std::nullopt;
    const std::int64_t days = days_from_civil(*y, static_cast<unsigned>(*mo), static_cast<unsigned>(*d));
    return days * 86400 + static_cast<std::int64_t>(*h) * 3600 + static_cast<std::int64_t>(*mi) * 60 + *sec - offset;
}

// ---- The provisioning profile ----

struct Profile {
    std::int64_t created = 0;   // CreationDate
    std::int64_t expires = 0;   // ExpirationDate
    std::string name;           // Name
    std::string team_id;        // TeamIdentifier's first
    std::string team_name;      // TeamName
    std::string app_id;         // Entitlements' application-identifier
    std::size_t devices = 0;    // ProvisionedDevices
    // A free (personal) team's profile lasts 7 days, a paid membership's a
    // year: what Apple says of free provisioning. Taken from the profile's
    // own dates, so it needs nothing else to know.
    bool short_lived() const { return expires > created && expires - created <= 8 * 86400; }
};

// From `security cms -D -i embedded.mobileprovision` (an XML plist).
inline std::expected<Profile, std::string> parse_profile(std::string_view xml) {
    const auto plist = parse_plist(xml);
    if (!plist) return std::unexpected(plist.error());
    if (!plist->is_object()) return std::unexpected(std::string("the profile isn't a dictionary"));
    Profile p;
    const auto created = parse_iso8601(plist->string_or("CreationDate", ""));
    const auto expires = parse_iso8601(plist->string_or("ExpirationDate", ""));
    if (!expires) return std::unexpected(std::string("the profile has no ExpirationDate"));
    p.expires = *expires;
    p.created = created.value_or(0);
    p.name = plist->string_or("Name", "");
    p.team_name = plist->string_or("TeamName", "");
    if (const json::Value* teams = plist->find("TeamIdentifier"); teams && teams->is_array() && !teams->items.empty() && teams->items[0].is_string())
        p.team_id = teams->items[0].string;
    if (const json::Value* ent = plist->find("Entitlements"); ent && ent->is_object()) p.app_id = ent->string_or("application-identifier", "");
    if (const json::Value* devices = plist->find("ProvisionedDevices"); devices && devices->is_array()) p.devices = devices->items.size();
    return p;
}

// ---- Devices (devicectl list devices --json-output) ----

struct Device {
    std::string identifier;    // CoreDevice's identifier: what --device takes
    std::string udid;          // hardwareProperties.udid: what xcodebuild's -destination id= takes
    std::string name;          // "Player's iPad"
    std::string model;         // marketingName ("iPad Air (5th generation)"), else productType ("iPad13,16")
    std::string device_type;   // "iPad", "iPhone"
    std::string os_version;    // "18.1"
    std::string pairing;       // connectionProperties.pairingState: "paired", "unpaired", ...
    std::string transport;     // connectionProperties.transportType: "wired", "localNetwork"; empty when not reachable
    std::string tunnel;        // connectionProperties.tunnelState: "connected", "disconnected", "unavailable"
    std::string developer_mode;  // deviceProperties.developerModeStatus: "enabled", "disabled"; empty if not known

    bool paired() const { return pairing == "paired"; }
    // Reachable now, by cable or over the network (assumed: devicectl gives
    // an unreachable device no transport and the tunnel state "unavailable").
    bool reachable() const { return !transport.empty() && tunnel != "unavailable"; }
    // "18.1" -> 18; 0 if unknown.
    int os_major() const {
        int major = 0;
        for (const char c : os_version) {
            if (c < '0' || c > '9') break;
            major = major * 10 + (c - '0');
        }
        return major;
    }
    // Developer Mode came with iOS 16: before it there is none to turn on.
    bool developer_mode_on() const { return developer_mode == "enabled" || (developer_mode.empty() && os_major() > 0 && os_major() < 16); }
    bool ready() const { return reachable() && paired() && developer_mode_on(); }
};

// The iPhones and iPads (physical, iOS or iPadOS) in devicectl's JSON;
// Apple TVs, watches, Vision Pros and simulators are left out.
inline std::expected<std::vector<Device>, std::string> parse_devices(std::string_view text) {
    const auto doc = json::parse(text);
    if (!doc) return std::unexpected(doc.error());
    const json::Value* info = doc->find("info");
    if (info && info->is_object()) {
        const std::string_view outcome = info->string_or("outcome", "success");
        if (outcome != "success") return std::unexpected("devicectl's outcome: " + std::string(outcome));
    }
    const json::Value* result = doc->find("result");
    const json::Value* devices = result ? result->find("devices") : nullptr;
    if (!devices || !devices->is_array()) return std::unexpected(std::string("devicectl's answer has no result.devices"));
    std::vector<Device> out;
    const json::Value none;
    for (const json::Value& d : devices->items) {
        if (!d.is_object()) continue;
        const json::Value* hw_ptr = d.find("hardwareProperties");
        const json::Value* dev_ptr = d.find("deviceProperties");
        const json::Value* conn_ptr = d.find("connectionProperties");
        const json::Value& hw = hw_ptr ? *hw_ptr : none;
        const json::Value& dev = dev_ptr ? *dev_ptr : none;
        const json::Value& conn = conn_ptr ? *conn_ptr : none;
        const std::string_view platform = hw.string_or("platform", "");
        const std::string_view reality = hw.string_or("reality", "physical");
        if (platform != "iOS" || reality != "physical") continue;
        Device x;
        x.identifier = d.string_or("identifier", "");
        if (x.identifier.empty()) continue;
        x.udid = hw.string_or("udid", "");
        x.name = dev.string_or("name", "");
        x.model = hw.string_or("marketingName", hw.string_or("productType", ""));
        x.device_type = hw.string_or("deviceType", "");
        x.os_version = dev.string_or("osVersionNumber", "");
        x.pairing = conn.string_or("pairingState", "");
        x.transport = conn.string_or("transportType", "");
        x.tunnel = conn.string_or("tunnelState", "");
        x.developer_mode = dev.string_or("developerModeStatus", "");
        if (x.name.empty()) x.name = x.model.empty() ? x.identifier : x.model;
        out.push_back(std::move(x));
    }
    // The ready ones first, then the reachable ones, then the rest; by name.
    const auto rank = [](const Device& d) { return d.ready() ? 0 : d.reachable() ? 1 : 2; };
    std::stable_sort(out.begin(), out.end(), [&](const Device& a, const Device& b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : a.name < b.name;
    });
    return out;
}

// ---- Files already on the device (devicectl device info files) ----

struct RemoteFile {
    std::string path;          // relative to the folder asked about
    std::uint64_t size = 0;
};

// Assumed format: devicectl's documentation doesn't describe this answer,
// so every object under `result` with a name ("relativePath", "path" or
// "name") and a size ("size" or "fileSize") is taken as a file. Nothing
// found means nothing is known to be there (the disc is copied again).
inline std::vector<RemoteFile> parse_remote_files(std::string_view text) {
    std::vector<RemoteFile> out;
    const auto doc = json::parse(text);
    if (!doc) return out;
    const json::Value* result = doc->find("result");
    if (!result) return out;
    const auto walk = [&](const auto& self, const json::Value& v, int depth) -> void {
        if (depth > 32) return;
        if (v.is_object()) {
            std::string_view name = v.string_or("relativePath", "");
            if (name.empty()) name = v.string_or("path", "");
            if (name.empty()) name = v.string_or("name", "");
            const json::Value* size = v.find("size");
            if (!size || !size->is_number()) size = v.find("fileSize");
            if (!name.empty() && size && size->is_number() && size->number >= 0) {
                std::string p(name);
                while (p.starts_with("./")) p.erase(0, 2);
                while (p.starts_with('/')) p.erase(0, 1);
                out.push_back({std::move(p), static_cast<std::uint64_t>(size->number)});
            }
            for (const json::Member& m : v.members) self(self, m.value, depth + 1);
        } else if (v.is_array()) {
            for (const json::Value& item : v.items) self(self, item, depth + 1);
        }
    };
    walk(walk, *result, 0);
    return out;
}

// Whether `name` (a path under Documents) is there with `size` bytes: the
// listing's paths may be relative to Documents or start with "Documents/".
inline bool already_there(const std::vector<RemoteFile>& files, std::string_view name, std::uint64_t size) {
    for (const RemoteFile& f : files) {
        std::string_view p = f.path;
        if (p.starts_with("Documents/")) p.remove_prefix(10);
        if (p == name && f.size == size) return true;
    }
    return false;
}

// ---- Signing identities and teams ----

struct Identity {
    std::string sha1;   // the certificate's SHA-1, 40 hex digits
    std::string name;   // "Apple Development: Jane Doe (A1B2C3D4E5)"
};

// `security find-identity -v -p codesigning`'s lines:
//   "  1) 0123...CDEF \"Apple Development: Jane Doe (A1B2C3D4E5)\""
// Only development identities (Apple Development, or iPhone Developer from
// before 2019) can sign an app for a player's own device.
inline std::vector<Identity> parse_identities(std::string_view text) {
    std::vector<Identity> out;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const std::size_t paren = line.find(") ");
        if (paren == std::string_view::npos || paren == 0) continue;
        bool numbered = true;
        for (std::size_t i = 0; i < paren; ++i) numbered = numbered && line[i] >= '0' && line[i] <= '9';
        if (!numbered) continue;
        line.remove_prefix(paren + 2);
        line = trim(line);
        if (line.size() < 42 || line[40] != ' ') continue;
        const std::string_view hash = line.substr(0, 40);
        if (hash.find_first_not_of("0123456789ABCDEFabcdef") != std::string_view::npos) continue;
        line.remove_prefix(41);
        line = trim(line);
        if (line.size() < 2 || line.front() != '"') continue;
        const std::size_t close = line.find('"', 1);
        if (close == std::string_view::npos) continue;
        const std::string_view name = line.substr(1, close - 1);
        if (!name.starts_with("Apple Development:") && !name.starts_with("iPhone Developer:")) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Identity& i) { return i.sha1 == hash; })) continue;  // listed twice
        out.push_back({std::string(hash), std::string(name)});
    }
    return out;
}

inline std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else continue;  // line breaks
        buffer = buffer << 6 | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

// A certificate's subject: its common name, organisational unit and
// organisation. Apple's development certificates put the team's identifier
// in OU and its name in O (the identifier in the common name's brackets is
// the person's, not the team's).
struct Subject {
    std::string common_name, unit, organisation;
};

namespace detail {

struct Der {
    const std::uint8_t* p;
    std::size_t n;
};

// One TLV at `d`: its tag, its contents, and `d` moved past it.
inline bool der_next(Der& d, std::uint8_t& tag, Der& contents) {
    if (d.n < 2) return false;
    tag = d.p[0];
    std::size_t len = d.p[1];
    std::size_t head = 2;
    if (len & 0x80) {
        const std::size_t bytes = len & 0x7F;
        if (bytes == 0 || bytes > 4 || d.n < 2 + bytes) return false;
        len = 0;
        for (std::size_t i = 0; i < bytes; ++i) len = len << 8 | d.p[2 + i];
        head += bytes;
    }
    if (d.n - head < len) return false;
    contents = {d.p + head, len};
    d.p += head + len;
    d.n -= head + len;
    return true;
}

}  // namespace detail

// From a certificate's DER bytes (X.509: the TBSCertificate's sixth field,
// after the optional version).
inline std::optional<Subject> certificate_subject(const std::vector<std::uint8_t>& der) {
    using detail::Der;
    Der all{der.data(), der.size()}, cert{}, tbs{}, field{};
    std::uint8_t tag = 0;
    if (!detail::der_next(all, tag, cert) || tag != 0x30) return std::nullopt;
    if (!detail::der_next(cert, tag, tbs) || tag != 0x30) return std::nullopt;
    // [0] version (optional), serial, signature, issuer, validity, subject.
    Der rest = tbs;
    if (!detail::der_next(rest, tag, field)) return std::nullopt;
    if (tag != 0xA0) rest = tbs;  // no version: the serial came first
    for (int i = 0; i < 4; ++i)
        if (!detail::der_next(rest, tag, field)) return std::nullopt;
    Der name{};
    if (!detail::der_next(rest, tag, name) || tag != 0x30) return std::nullopt;
    Subject s;
    for (Der set{}; detail::der_next(name, tag, set);) {
        if (tag != 0x31) return std::nullopt;
        for (Der atv{}; detail::der_next(set, tag, atv);) {
            Der oid{}, value{};
            std::uint8_t oid_tag = 0, value_tag = 0;
            if (tag != 0x30 || !detail::der_next(atv, oid_tag, oid) || oid_tag != 0x06 || !detail::der_next(atv, value_tag, value))
                return std::nullopt;
            if (oid.n != 3 || oid.p[0] != 0x55 || oid.p[1] != 0x04) continue;  // 2.5.4.x
            const std::string text(reinterpret_cast<const char*>(value.p), value.n);
            if (oid.p[2] == 3) s.common_name = text;
            else if (oid.p[2] == 11) s.unit = text;
            else if (oid.p[2] == 10) s.organisation = text;
        }
    }
    return s;
}

// Every certificate in PEM text (`security find-certificate -a -p`), by subject.
inline std::vector<Subject> pem_subjects(std::string_view pem) {
    std::vector<Subject> out;
    constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----", kEnd = "-----END CERTIFICATE-----";
    for (std::size_t at = pem.find(kBegin); at != std::string_view::npos; at = pem.find(kBegin, at)) {
        const std::size_t body = at + kBegin.size();
        const std::size_t end = pem.find(kEnd, body);
        if (end == std::string_view::npos) break;
        if (const auto s = certificate_subject(base64_decode(pem.substr(body, end - body)))) out.push_back(*s);
        at = end + kEnd.size();
    }
    return out;
}

struct Team {
    std::string id;     // "ABCDE12345": DEVELOPMENT_TEAM
    std::string name;   // "Jane Doe (Personal Team)", "Example Ltd"
    bool free = false;  // a personal team without a paid membership: apps last 7 days
    bool known_free = false;  // whether `free` was said (Xcode's settings) or only guessed
    bool has_certificate = false;  // a development identity for it is in the keychain
};

inline void add_team(std::vector<Team>& teams, Team t) {
    for (Team& have : teams) {
        if (have.id != t.id) continue;
        if (have.name.empty()) have.name = t.name;
        if (t.known_free && !have.known_free) {
            have.free = t.free;
            have.known_free = true;
        }
        have.has_certificate = have.has_certificate || t.has_certificate;
        return;
    }
    teams.push_back(std::move(t));
}

// Teams from Xcode's settings (`defaults export com.apple.dt.Xcode`, as XML).
// Assumed format: Xcode keeps its accounts' teams under
// IDEProvisioningTeams (older) or IDEProvisioningTeamByIdentifier (newer) as
// dictionaries with teamID, teamName and isFreeProvisioningTeam; any such
// dictionary anywhere in the settings is taken.
inline std::vector<Team> xcode_teams(std::string_view xml) {
    std::vector<Team> out;
    const auto plist = parse_plist(xml);
    if (!plist) return out;
    const auto walk = [&](const auto& self, const json::Value& v, int depth) -> void {
        if (depth > 32) return;
        if (v.is_object()) {
            const std::string_view id = v.string_or("teamID", "");
            if (!id.empty()) {
                Team t;
                t.id = std::string(id);
                t.name = std::string(v.string_or("teamName", ""));
                if (const json::Value* f = v.find("isFreeProvisioningTeam"); f && (f->is_bool() || f->is_number())) {
                    t.free = f->is_bool() ? f->boolean : f->number != 0;
                    t.known_free = true;
                }
                add_team(out, std::move(t));
            }
            for (const json::Member& m : v.members) self(self, m.value, depth + 1);
        } else if (v.is_array()) {
            for (const json::Value& item : v.items) self(self, item, depth + 1);
        }
    };
    walk(walk, *plist, 0);
    return out;
}

// Teams from the keychain: each development identity's certificate's OU.
inline std::vector<Team> identity_teams(const std::vector<Identity>& identities, const std::vector<Subject>& certificates) {
    std::vector<Team> out;
    for (const Identity& id : identities) {
        for (const Subject& s : certificates) {
            if (s.common_name != id.name || s.unit.empty()) continue;
            Team t;
            t.id = s.unit;
            t.name = s.organisation;
            t.has_certificate = true;
            add_team(out, std::move(t));
        }
    }
    return out;
}

// ---- The disc's files ----

// The files a disc image is made of, relative to its own folder ('/'
// separators), the image itself first: a .cue's FILE lines, a .gdi's
// tracks, or just the file (a .chd). A name leaving the folder is an error.
inline std::expected<std::vector<std::string>, std::string> disc_files(std::string_view image_name, std::string_view text) {
    std::string ext;
    if (const std::size_t dot = image_name.rfind('.'); dot != std::string_view::npos)
        for (const char c : image_name.substr(dot)) ext += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    std::string own(image_name);
    if (const std::size_t slash = own.find_last_of("/\\"); slash != std::string::npos) own.erase(0, slash + 1);
    std::vector<std::string> out{own};
    const auto add = [&](std::string name) -> std::expected<void, std::string> {
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty()) return std::unexpected(std::string("a track without a file name"));
        if (name.front() == '/' || (name.size() > 1 && name[1] == ':') || name == ".." || name.starts_with("../") ||
            name.find("/../") != std::string::npos || name.ends_with("/.."))
            return std::unexpected("a track outside the disc image's folder (" + name + ")");
        while (name.starts_with("./")) name.erase(0, 2);
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
        return {};
    };
    // A quoted or plain word at the start of `s`, and `s` moved past it.
    const auto word = [](std::string_view& s) -> std::optional<std::string> {
        s = trim(s);
        if (s.empty()) return std::nullopt;
        std::string w;
        if (s.front() == '"') {
            const std::size_t close = s.find('"', 1);
            if (close == std::string_view::npos) return std::nullopt;
            w = std::string(s.substr(1, close - 1));
            s.remove_prefix(close + 1);
        } else {
            const std::size_t end = std::min(s.find_first_of(" \t"), s.size());
            w = std::string(s.substr(0, end));
            s.remove_prefix(end);
        }
        return w;
    };
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    if (ext == ".cue") {
        for (std::string_view rest = text; !rest.empty();) {
            const std::size_t nl = rest.find('\n');
            std::string_view line = trim(rest.substr(0, nl));
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            if (line.size() < 5) continue;
            std::string head(line.substr(0, 5));
            for (char& c : head) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
            if (head != "FILE " && head != "FILE\t") continue;
            line.remove_prefix(5);
            // FILE "name" TYPE, or FILE name TYPE (the name may hold spaces: the type is the last word).
            std::string name;
            if (trim(line).starts_with('"')) {
                const auto w = word(line);
                if (!w) return std::unexpected(std::string("a FILE line with an unclosed quote"));
                name = *w;
            } else {
                line = trim(line);
                const std::size_t last = line.find_last_of(" \t");
                name = std::string(trim(last == std::string_view::npos ? line : line.substr(0, last)));
            }
            if (auto added = add(name); !added) return std::unexpected(added.error());
        }
        if (out.size() == 1) return std::unexpected(std::string("the .cue names no track files"));
    } else if (ext == ".gdi") {
        std::string_view rest = text;
        bool first = true;
        std::size_t tracks = 0, count = 0;
        while (!rest.empty()) {
            const std::size_t nl = rest.find('\n');
            std::string_view line = trim(rest.substr(0, nl));
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            if (line.empty()) continue;
            if (first) {
                first = false;
                for (const char c : line) {
                    if (c < '0' || c > '9') return std::unexpected(std::string("the .gdi doesn't start with its number of tracks"));
                    count = count * 10 + static_cast<std::size_t>(c - '0');
                }
                continue;
            }
            // number, start sector, type, sector size, file name, offset
            for (int i = 0; i < 4; ++i)
                if (!word(line)) return std::unexpected(std::string("a .gdi track line is cut short"));
            const auto name = word(line);
            if (!name) return std::unexpected(std::string("a .gdi track line has no file name"));
            if (auto added = add(*name); !added) return std::unexpected(added.error());
            ++tracks;
        }
        if (tracks == 0 || tracks != count) return std::unexpected("the .gdi says " + std::to_string(count) + " tracks and lists " + std::to_string(tracks));
    }
    return out;
}

// ---- What is remembered ----

// The bundle identifier made once per player and game: Apple's rules allow
// letters, digits, '-' and '.'.
inline std::string make_bundle_id(std::string_view game, std::uint32_t salt) {
    std::string clean;
    for (const char c : game)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') clean += c;
    if (clean.empty()) clean = "game";
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", static_cast<unsigned>(salt));
    return "io.github.qilinrecomp." + clean + ".r" + hex;
}

inline bool valid_bundle_id(std::string_view id) {
    if (id.empty() || id.size() > 155 || id.front() == '.' || id.back() == '.' || id.find("..") != std::string_view::npos) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    return id.find('.') != std::string_view::npos;
}

// The last install: when, with which team, on which device, until when
// (ios-install.txt in the data folder: name=value lines).
struct Install {
    std::int64_t installed = 0;  // seconds since 1970
    std::int64_t expires = 0;    // the profile's ExpirationDate; 0 if unknown
    bool short_lived = false;    // a 7-day (free team's) profile
    std::string team;
    std::string device;          // devicectl's identifier
    std::string device_name;
    std::string bundle_id;
    std::string app;             // the built .app (for Reinstall)
    std::string disc;            // the disc image whose files were copied
};

inline std::string format_install(const Install& i) {
    return "installed=" + std::to_string(i.installed) + "\nexpires=" + std::to_string(i.expires) + "\nshort_lived=" + (i.short_lived ? "1" : "0") +
           "\nteam=" + i.team + "\ndevice=" + i.device + "\ndevice_name=" + i.device_name + "\nbundle_id=" + i.bundle_id + "\napp=" + i.app +
           "\ndisc=" + i.disc + "\n";
}

inline std::optional<Install> parse_install(std::string_view text) {
    const auto v = parse_settings(text);
    const auto get = [&](std::string_view key) {
        const auto it = v.find(key);
        return it == v.end() ? std::string() : it->second;
    };
    const auto number = [&](std::string_view key) -> std::int64_t {
        const std::string s = get(key);
        std::int64_t n = 0;
        for (const char c : s) {
            if (c < '0' || c > '9') return 0;
            n = n * 10 + (c - '0');
        }
        return n;
    };
    Install i;
    i.installed = number("installed");
    if (i.installed == 0) return std::nullopt;
    i.expires = number("expires");
    i.short_lived = get("short_lived") == "1";
    i.team = get("team");
    i.device = get("device");
    i.device_name = get("device_name");
    i.bundle_id = get("bundle_id");
    i.app = get("app");
    i.disc = get("disc");
    return i;
}

}  // namespace qilin::launcher::ios
