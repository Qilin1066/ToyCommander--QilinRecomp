// Keeping the download's folder as the release made it (tidy.hpp).
#include "tidy.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

#include "kit_text.hpp"
#include "platform.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

// A path as files.txt writes it ('/' separators), compared without case
// where the system's file names have none (Windows, macOS).
std::string key(const fs::path& relative) {
    const std::u8string text = relative.generic_u8string();
    std::string s(text.begin(), text.end());
#if defined(_WIN32) || defined(__APPLE__)
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return s;
}

void remove_file(const fs::path& p, Tidied& out) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::is_regular_file(st)) return;  // missing, a folder or a link: left alone
    if (fs::remove(p, ec)) out.removed.push_back(p);
    else if (ec) out.problems.push_back(show(p) + ": " + ec.message());
}

// Removes what files.txt doesn't name under top\<folder>, then the folders
// left empty.
void keep_listed(const fs::path& top, const fs::path& folder, const std::set<std::string>& listed, Tidied& out) {
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(folder, ec))) return;
    std::vector<fs::path> files, folders;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::file_status st = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(st)) {
            it.disable_recursion_pending();  // never into a link
            continue;
        }
        if (fs::is_directory(st)) folders.push_back(it->path());
        else if (fs::is_regular_file(st) && !listed.contains(key(it->path().lexically_relative(top)))) files.push_back(it->path());
    }
    for (const fs::path& f : files) remove_file(f, out);
    // Deepest first, so a folder emptied by its children goes too.
    std::ranges::sort(folders, [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    for (const fs::path& d : folders) {
        std::error_code e;
        if (fs::is_empty(d, e) && !e && fs::remove(d, e)) out.removed.push_back(d);
    }
}

}  // namespace

Tidied tidy_folder(const Kit& kit) {
    Tidied out;

    // kit\ and LICENSES\: exactly what this release holds.
    if (const auto text = read_file(kit.kit / "files.txt")) {
        std::set<std::string> listed;
        for (std::string_view rest = *text; !rest.empty();) {
            const size_t end = std::min(rest.find('\n'), rest.size());
            const std::string_view line = trim(rest.substr(0, end));
            rest.remove_prefix(std::min(end + 1, rest.size()));
            if (!line.empty()) listed.insert(key(from_utf8(line)));
        }
        // A list that doesn't name the files the launcher is reading from is
        // the wrong one (or damaged): then nothing is removed. Paths are the
        // download's (macOS: the app's), so the kit is Contents/Resources/kit
        // there; LICENSES is beside the kit.
        const fs::path kit_rel = kit.kit.lexically_relative(kit.top);
        if (listed.contains(key(kit_rel / "game.txt")) && listed.contains(key(kit_rel / "files.txt")) &&
            listed.contains(key(kit_rel / from_utf8("qilin" + std::string(executable_suffix()))))) {
            keep_listed(kit.top, kit.kit, listed, out);
            keep_listed(kit.top, kit.kit.parent_path() / "LICENSES", listed, out);
        }
    }

#if defined(_WIN32)
    std::error_code ec;
    // The top: what older Windows downloads kept there.
    const fs::path old_game = kit.top / from_utf8(kit.exe);
    for (const fs::path& p : {kit.top / from_utf8("Play " + kit.title + ".bat"), old_game, fs::path(old_game).concat(".previous"),
                              fs::path(old_game).concat(".new"), kit.top / "run.log", kit.top / "run-errors.log"})
        remove_file(p, out);

    // work\: Ninja's files, and play.ps1's remembered disc, kept if the
    // launcher has none yet.
    const fs::path old_disc = kit.work / "disc.txt";
    if (fs::is_regular_file(fs::symlink_status(old_disc, ec)) && !fs::exists(kit.disc_file(), ec)) {
        if (const auto text = read_file(old_disc)) {
            std::string_view line = trim(*text);
            if (line.starts_with("\xEF\xBB\xBF")) line.remove_prefix(3);  // Windows PowerShell's UTF-8 mark
            if (!line.empty()) (void)write_file(kit.disc_file(), std::string(trim(line)));
        }
    }
    for (const char* name : {"disc.txt", "disc.txt.tmp", "build.ninja", ".ninja_log", ".ninja_deps"}) remove_file(kit.work / name, out);
    for (fs::directory_iterator it(kit.work / "obj", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".d") remove_file(it->path(), out);  // Ninja's dependency files
#endif

    // This project's own leftovers, as its game.txt names them: where an
    // older download remembered the player's data (taken over if the
    // launcher remembers none yet), then the files themselves.
    for (const std::string& rel : kit.old_input_files) {
        const fs::path old = kit.top / from_utf8(rel);
        std::error_code e;
        if (!fs::is_regular_file(fs::symlink_status(old, e)) || fs::exists(kit.disc_file(), e)) continue;
        if (const auto text = read_file(old)) {
            std::string_view line = trim(*text);
            if (line.starts_with("\xEF\xBB\xBF")) line.remove_prefix(3);  // Windows PowerShell's UTF-8 mark
            line = trim(line.substr(0, std::min(line.find('\n'), line.size())));
            if (!line.empty()) (void)write_file(kit.disc_file(), std::string(line));
        }
    }
    for (const std::string& rel : kit.old_input_files) remove_file(kit.top / from_utf8(rel), out);
    for (const std::string& rel : kit.old_files) remove_file(kit.top / from_utf8(rel), out);
    return out;
}

}  // namespace qilin::launcher
