// The launcher in the applications menu on Linux: a freedesktop.org desktop
// entry in ~/.local/share/applications (shortcut.hpp). Nothing on macOS.
#include "shortcut.hpp"

#include <system_error>

#include "kit_text.hpp"
#include "platform.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

#if defined(__APPLE__)

const char* shortcut_label() { return nullptr; }
fs::path desktop_shortcut(const Kit&) { return {}; }
bool has_desktop_shortcut(const Kit&) { return true; }
std::expected<void, std::string> make_desktop_shortcut(const Kit&) { return {}; }

#else

namespace {

// A desktop entry's value: its own escapes for the Exec key's quoting.
std::string exec_quoted(const std::string& path) {
    std::string out = "\"";
    for (const char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string entry_text(const Kit& kit) {
    std::string text = "[Desktop Entry]\n";
    text += "Type=Application\n";
    text += "Version=1.0\n";
    text += "Name=" + kit.title + "\n";
    text += "Comment=Play " + kit.title + " (QilinRecomp), built from your own " + kit.input_noun + "\n";
    text += "Exec=" + exec_quoted(show(kit.launcher)) + "\n";
    text += "Path=" + show(kit.top) + "\n";
    text += "Icon=" + show(kit.kit / "icon.png") + "\n";
    text += "Terminal=false\n";
    text += "Categories=Game;\n";
    text += "StartupNotify=true\n";
    return text;
}

}  // namespace

const char* shortcut_label() { return "Add to the applications menu"; }

fs::path desktop_shortcut(const Kit& kit) {
    const fs::path base = user_data_base();  // $XDG_DATA_HOME or ~/.local/share
    return base.empty() ? fs::path() : base / "applications" / ("qilinrecomp-" + kit.game + ".desktop");
}

bool has_desktop_shortcut(const Kit& kit) {
    const fs::path entry = desktop_shortcut(kit);
    const auto text = entry.empty() ? std::expected<std::string, std::string>(std::unexpected(std::string())) : read_file(entry);
    return text && *text == entry_text(kit);  // one for another folder is made again for this one
}

std::expected<void, std::string> make_desktop_shortcut(const Kit& kit) {
    const fs::path entry = desktop_shortcut(kit);
    if (entry.empty()) return std::unexpected(std::string("this system names no folder for applications (no HOME)"));
    std::error_code ec;
    fs::create_directories(entry.parent_path(), ec);
    if (ec) return std::unexpected("can't make " + show(entry.parent_path()) + ": " + ec.message());
    if (auto written = write_file(entry, entry_text(kit)); !written) return written;
    fs::permissions(entry, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    return {};
}

#endif

}  // namespace qilin::launcher
