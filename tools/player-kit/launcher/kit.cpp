// The download's layout and small file helpers (kit.hpp). The system's own
// parts (own_path, show, claim_folder, cpu_identity) are platform_*.cpp's.
#include "kit.hpp"

#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"
#include "sha256.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

bool plain_name(std::string_view s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (const char c : s)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

}  // namespace

fs::path Kit::game_exe() const { return work / from_utf8(exe); }
fs::path Kit::qilin() const { return kit / from_utf8("qilin" + std::string(executable_suffix())); }

fs::path Kit::compiler() const {
    // game.txt's `compiler`: a path in the kit (the bundled one), or an
    // absolute one (macOS: Apple's, from the Command Line Tools).
    const fs::path p = from_utf8(compiler_path);
    return p.is_absolute() ? p : kit / p;
}

std::string Kit::wanted_disc() const {
    std::string s = build;
    if (!product.empty()) s += ", " + product;
    if (!disc_version.empty()) s += " version " + disc_version;
    return s;
}

std::expected<Kit, std::string> load_kit(const fs::path& launcher) { return load_kit(launcher, {}); }

std::expected<Kit, std::string> load_kit(const fs::path& launcher, const fs::path& user) {
    Kit k;
    k.launcher = launcher;
    k.top = launcher.parent_path();
    k.kit = k.top / "kit";
    // A macOS app: <Title>.app/Contents/MacOS/<launcher>, the kit in
    // Contents/Resources/kit. The app may be read-only (or moved aside by
    // Gatekeeper's translocation), so the build goes to the data folder.
    const bool app_bundle = k.top.filename() == "MacOS" && k.top.parent_path().filename() == "Contents" &&
                            k.top.parent_path().parent_path().extension() == ".app";
    if (app_bundle) {
        k.top = k.top.parent_path().parent_path();
        k.kit = k.top / "Contents" / "Resources" / "kit";
    }
    const auto about_text = read_file(k.kit / "game.txt");
    if (!about_text) return std::unexpected(std::string("This download has no kit/game.txt. Unpack the whole download, then start Play from its folder."));
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
    k.compiler_path = get("compiler", "toolchain/bin/x86_64-w64-mingw32-clang++.exe");
    k.compiler_args = split_flags(get("compiler_args"), "");
    if (!plain_name(k.game) || k.title.empty() || !plain_name(k.exe) || k.build.empty())
        return std::unexpected(std::string("kit/game.txt is incomplete (it needs game, title, exe and build). Unpack the download again."));
    const auto version = read_file(k.kit / "VERSION");
    if (!version) return std::unexpected(std::string("This download has no kit/VERSION. Unpack the download again."));
    k.version = std::string(trim(*version));

    if (!user.empty()) {
        k.user = user;
    } else if (const std::string save = environment("QILIN_SAVE_DIR"); !save.empty()) {
        k.user = from_utf8(save);  // as the game has it (host/src/paths.cpp)
    } else {
        const fs::path base = user_data_base();
        if (base.empty()) return std::unexpected(std::string("This system names no folder for your saves (no LOCALAPPDATA or HOME)."));
        k.user = base / "qilin" / from_utf8(k.game);
    }
    k.work = app_bundle ? k.user / "work" : k.top / "work";
    std::error_code ec;
    fs::create_directories(k.user, ec);
    if (ec) return std::unexpected("Can't make the folder " + show(k.user) + ": " + ec.message());
    return k;
}

std::expected<std::string, std::string> read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("can't open " + show(path));
    std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) return std::unexpected("can't read " + show(path));
    return text;
}

std::expected<void, std::string> write_file(const fs::path& path, std::string_view text) {
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.close();
        if (!f) return std::unexpected("can't write " + show(temporary));
    }
    if (const auto moved = rename_over(temporary, path); !moved) {
        std::error_code ec;
        fs::remove(temporary, ec);
        return std::unexpected("can't write " + show(path) + ": " + moved.error());
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

}  // namespace qilin::launcher
