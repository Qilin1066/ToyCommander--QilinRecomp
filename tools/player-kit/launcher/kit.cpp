// The download's layout and small file helpers (kit.hpp).
#include "kit.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "kit_text.hpp"
#include "process.hpp"
#include "sha256.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

std::wstring environment(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    std::wstring value(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, value.data(), n);
    value.resize(got < n ? got : 0);
    return value;
}

bool plain_name(std::string_view s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (const char c : s)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

}  // namespace

fs::path Kit::game_exe() const { return top / widen(exe); }
fs::path Kit::compiler() const { return kit / "toolchain" / "bin" / "x86_64-w64-mingw32-clang++.exe"; }
fs::path Kit::qilin() const { return kit / "qilin.exe"; }

std::string Kit::wanted_disc() const {
    std::string s = build;
    if (!product.empty()) s += ", " + product;
    if (!disc_version.empty()) s += " version " + disc_version;
    return s;
}

fs::path own_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n < buffer.size()) {
            buffer.resize(n);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::expected<Kit, std::string> load_kit(const fs::path& launcher) { return load_kit(launcher, {}); }

std::expected<Kit, std::string> load_kit(const fs::path& launcher, const fs::path& user) {
    Kit k;
    k.launcher = launcher;
    k.top = launcher.parent_path();
    k.kit = k.top / "kit";
    k.work = k.top / "work";
    const auto about_text = read_file(k.kit / "game.txt");
    if (!about_text) return std::unexpected("This folder has no kit\\game.txt. Unpack the whole download, then start Play from its folder.");
    const auto about = parse_settings(*about_text);
    const auto get = [&](std::string_view key, std::string_view fallback = {}) {
        const auto it = about.find(key);
        return it == about.end() ? std::string(fallback) : it->second;
    };
    k.game = get("game");
    k.title = get("title");
    k.exe = get("exe");
    k.boot = get("boot");
    k.build = get("build");
    k.product = get("product");
    k.disc_version = get("disc_version");
    k.package = get("package");
    k.repo = get("repo", "Qilin1066/ToyCommander--QilinRecomp");
    if (!plain_name(k.game) || k.title.empty() || !plain_name(k.exe) || k.build.empty())
        return std::unexpected(std::string("kit\\game.txt is incomplete (it needs game, title, exe and build). Unpack the download again."));
    const auto version = read_file(k.kit / "VERSION");
    if (!version) return std::unexpected(std::string("This folder has no kit\\VERSION. Unpack the download again."));
    k.version = std::string(trim(*version));

    if (!user.empty()) {
        k.user = user;
    } else if (const std::wstring save = environment(L"QILIN_SAVE_DIR"); !save.empty()) {
        k.user = save;  // as the game has it (host/src/paths.cpp)
    } else {
        const std::wstring base = environment(L"LOCALAPPDATA");
        if (base.empty()) return std::unexpected(std::string("Windows gives no LOCALAPPDATA folder for your saves."));
        k.user = fs::path(base) / "qilin" / widen(k.game);
    }
    std::error_code ec;
    fs::create_directories(k.user, ec);
    if (ec) return std::unexpected("Can't make the folder " + show(k.user) + ": " + ec.message());
    return k;
}

std::string show(const fs::path& path) { return narrow(path.wstring()); }

std::expected<std::string, std::string> read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("can't open " + show(path));
    std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) return std::unexpected("can't read " + show(path));
    return text;
}

std::expected<void, std::string> write_file(const fs::path& path, std::string_view text) {
    fs::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.close();
        if (!f) return std::unexpected("can't write " + show(temporary));
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::string why = system_message(GetLastError());
        std::error_code ec;
        fs::remove(temporary, ec);
        return std::unexpected("can't write " + show(path) + ": " + why);
    }
    return {};
}

std::expected<std::string, std::string> sha256_file(const fs::path& path) {
    const auto bytes = read_file(path);
    if (!bytes) return std::unexpected(bytes.error());
    return sha256_hex(std::span(reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size()));
}

std::string tail(const fs::path& path, int lines) {
    const auto text = read_file(path);
    if (!text) return {};
    std::string_view s = trim(*text);
    std::size_t at = s.size();
    for (int n = 0; n < lines && at > 0; ++n) {
        const std::size_t nl = s.rfind('\n', at - 1);
        if (nl == std::string_view::npos) {
            at = 0;
            break;
        }
        at = nl;
    }
    if (at > 0) ++at;  // past the newline
    std::string out(s.substr(at));
    std::erase(out, '\r');
    return out;
}

bool claim_folder(const Kit& kit) {
    std::wstring folder = fs::absolute(kit.top).wstring();
    for (wchar_t& c : folder) c = static_cast<wchar_t>(towlower(c));
    const std::string key = narrow(folder);
    const std::string name = "Local\\QilinRecomp-launcher-" + sha256_hex(std::span(reinterpret_cast<const uint8_t*>(key.data()), key.size()));
    // Kept until the launcher ends.
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, widen(name).c_str());
    return mutex && GetLastError() != ERROR_ALREADY_EXISTS;
}

std::string cpu_identity() { return narrow(environment(L"PROCESSOR_IDENTIFIER")); }

}  // namespace qilin::launcher
