// The download's layout, game.txt and small file helpers (kit.hpp). The
// system's own parts (own_path, show, claim_folder, cpu_identity) are
// platform_*.cpp's.
#include "kit.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "hash.hpp"
#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

bool plain_name(std::string_view s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (const char c : s)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

// What game.txt says when it has none of the data keys: a Dreamcast disc
// image, as the first downloads (Toy Commander's) had it.
constexpr std::string_view kDefaultExtensions = "cue;gdi;chd";
constexpr std::string_view kDefaultDiscHint = "Choose your disc's .cue file (with its .bin tracks beside it), its .gdi or its .chd.";
constexpr std::string_view kDefaultIdentify = "{qilin} identify {input} --json --build {game:build}";
constexpr std::string_view kDefaultStep = "{qilin} lift {input} --out {gen} --files 64";
constexpr int kMaxSteps = 9;

std::unexpected<std::string> bad_kit(std::string_view why) {
    return std::unexpected("kit/game.txt " + std::string(why) + ". Unpack the download again.");
}

}  // namespace

fs::path Kit::game_exe() const { return work / from_utf8(exe); }
fs::path Kit::qilin() const { return kit_tool("qilin"); }
fs::path Kit::kit_tool(std::string_view name) const { return kit / from_utf8(std::string(name) + std::string(executable_suffix())); }

fs::path Kit::compiler() const {
    // game.txt's `compiler`: a path in the kit (the bundled one), or an
    // absolute one (macOS: Apple's, from the Command Line Tools).
    const fs::path p = from_utf8(compiler_path);
    return p.is_absolute() ? p : kit / p;
}

std::string Kit::wanted() const {
    std::string s = build;
    if (!product.empty()) s += ", " + product;
    if (!disc_version.empty()) s += " version " + disc_version;
    return s;
}

std::expected<std::vector<std::string>, std::string> expand_command(const Kit& kit, std::string_view text, const fs::path& input) {
    auto words = split_command(text);
    if (!words) return std::unexpected(words.error());
    if (words->empty()) return std::unexpected(std::string("an empty command"));
    const auto lookup = [&](std::string_view name) -> std::optional<std::string> {
        if (name == "qilin") return show(kit.qilin());
        if (name == "kit") return show(kit.kit);
        if (name == "input") return show(input);
        if (name == "gen") return show(kit.work / "gen");
        if (name == "work") return show(kit.work);
        if (name.starts_with("tool:")) {
            const std::string_view tool = name.substr(5);
            if (!plain_name(tool)) return std::nullopt;
            return show(kit.kit_tool(tool));
        }
        if (name.starts_with("game:")) {
            const auto it = kit.about.find(name.substr(5));
            if (it == kit.about.end()) return std::nullopt;
            return it->second;
        }
        return std::nullopt;
    };
    std::vector<std::string> out;
    for (const std::string& word : *words) {
        auto expanded = expand_placeholders(word, lookup);
        if (!expanded) return std::unexpected(expanded.error());
        out.push_back(std::move(*expanded));
    }
    return out;
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
    k.about = parse_settings(*about_text);
    const auto has = [&](std::string_view key) { return k.about.find(key) != k.about.end(); };
    const auto get = [&](std::string_view key, std::string_view fallback = {}) {
        const auto it = k.about.find(key);
        return it == k.about.end() ? std::string(fallback) : it->second;
    };
    k.game = get("game");
    k.title = get("title");
    k.exe = get("exe");
    k.boot = get("boot");
    k.build = get("build");
    k.product = get("product");
    k.disc_version = get("disc_version");
    k.package = get("package");
    k.repo = get("repo");
    k.release_tag_prefix = get("release_tag_prefix", k.game + "-v");
    k.release_asset_prefix = get("release_asset_prefix", k.package + "-");
    k.compiler_path = get("compiler", "toolchain/bin/x86_64-w64-mingw32-clang++.exe");
    k.compiler_args = split_flags(get("compiler_args"), "");
    if (!plain_name(k.game) || k.title.empty() || !plain_name(k.exe) || k.build.empty())
        return bad_kit("is incomplete (it needs game, title, exe and build)");

    // The player's data.
    const std::string input = get("input", "file");
    if (input == "file") k.input = InputKind::File;
    else if (input == "folder") k.input = InputKind::Folder;
    else return bad_kit("says input = " + input + " (file or folder)");
    if (k.input == InputKind::File) k.input_extensions = split_extensions(get("input_extensions", kDefaultExtensions));
    k.input_noun = get("input_noun", k.input == InputKind::File ? "disc" : "game folder");
    if (k.input_noun.empty()) return bad_kit("has an empty input_noun");
    std::string hint;
    if (k.input == InputKind::Folder) hint = "Choose the folder that holds your " + k.input_noun + ".";
    else if (k.input_extensions == split_extensions(kDefaultExtensions)) hint = kDefaultDiscHint;
    else if (k.input_extensions.empty()) hint = "Choose your " + k.input_noun + "'s file.";
    else hint = "Choose your " + k.input_noun + "'s " + list_extensions(k.input_extensions) + " file.";
    k.input_hint = get("input_hint", hint);

    // The commands: identify, the recompiler's steps, the game's arguments.
    k.identify = get("identify", kDefaultIdentify);
    for (int n = 1; n <= kMaxSteps; ++n) {
        const std::string key = "step." + std::to_string(n);
        if (has(key)) {
            if (static_cast<int>(k.steps.size()) != n - 1) return bad_kit("has " + key + " without step." + std::to_string(n - 1));
            k.steps.push_back(get(key));
        }
    }
    for (const auto& [key, value] : k.about)
        if (key.starts_with("step.")) {
            const std::string_view n = std::string_view(key).substr(5);
            int number = 0;
            const auto [p, ec] = std::from_chars(n.data(), n.data() + n.size(), number);
            if (ec != std::errc{} || p != n.data() + n.size() || number < 1 || number > kMaxSteps)
                return bad_kit("has " + key + " (steps are step.1 to step." + std::to_string(kMaxSteps) + ")");
        }
    if (k.steps.empty()) k.steps.push_back(std::string(kDefaultStep));
    k.run_args = get("run_args", "{input}");
    const auto number = [&](std::string_view key, std::uint64_t& into) -> bool {
        if (!has(key)) return true;
        const std::string text = get(key);
        std::uint64_t v = 0;
        const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
        if (ec != std::errc{} || p != text.data() + text.size() || v == 0) return false;
        into = v;
        return true;
    };
    for (const auto& [key, into] : {std::pair<std::string_view, std::uint64_t*>{"compile_memory_mb", &k.compile_memory_mb},
                                    {"build_space_mb", &k.build_space_mb},
                                    {"cache_mb", &k.cache_mb}})
        if (!number(key, *into)) return bad_kit("says " + std::string(key) + " = " + get(key) + " (a number of megabytes)");

    const auto version = read_file(k.kit / "VERSION");
    if (!version) return std::unexpected(std::string("This download has no kit/VERSION. Unpack the download again."));
    k.version = std::string(trim(*version));

    if (!user.empty()) {
        k.user = user;
    } else if (const std::string save = environment("QILIN_SAVE_DIR"); !save.empty()) {
        k.user = from_utf8(save);  // as the game has it
    } else {
        const fs::path base = user_data_base();
        if (base.empty()) return std::unexpected(std::string("This system names no folder for your saves (no LOCALAPPDATA or HOME)."));
        k.user = base / "qilin" / from_utf8(k.game);
    }
    k.work = app_bundle ? k.user / "work" : k.top / "work";

    // Every command must expand now, not halfway through a build.
    std::vector<std::pair<std::string, std::string>> commands = {{"identify", k.identify}, {"run_args", "game " + k.run_args}};
    for (std::size_t i = 0; i < k.steps.size(); ++i) commands.emplace_back("step." + std::to_string(i + 1), k.steps[i]);
    for (const auto& [key, text] : commands)
        if (const auto words = expand_command(k, text, "input"); !words) return bad_kit("has a " + key + " that can't be used (" + words.error() + ")");

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
    // In pieces: the compiler and the kit's library are hundreds of MB.
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("can't open " + show(path));
    Sha256 h;
    std::vector<char> buffer(1 << 20);
    while (f) {
        f.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = f.gcount();
        if (got > 0) h.add(std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(got)));
    }
    if (f.bad()) return std::unexpected("can't read " + show(path));
    return h.hex();
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
