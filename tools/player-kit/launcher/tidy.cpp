// Keeping the download's folder as the release made it (tidy.hpp).
#include "tidy.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

#include "kit_text.hpp"
#include "process.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

// A path as files.txt writes it, compared without case (as Windows does).
std::string key(const fs::path& relative) {
    std::string s = narrow(relative.generic_wstring());
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
    std::error_code ec;

    // kit\ and LICENSES\: exactly what this release holds.
    if (const auto text = read_file(kit.kit / "files.txt")) {
        std::set<std::string> listed;
        for (std::string_view rest = *text; !rest.empty();) {
            const size_t end = std::min(rest.find('\n'), rest.size());
            const std::string_view line = trim(rest.substr(0, end));
            rest.remove_prefix(std::min(end + 1, rest.size()));
            if (!line.empty()) listed.insert(key(fs::path(widen(line))));
        }
        // A list that doesn't name the files the launcher is reading from is
        // the wrong one (or damaged): then nothing is removed.
        if (listed.contains("kit/game.txt") && listed.contains("kit/files.txt") && listed.contains("kit/qilin.exe")) {
            keep_listed(kit.top, kit.kit, listed, out);
            keep_listed(kit.top, kit.top / "LICENSES", listed, out);
        }
    }

    // The top: what older downloads kept there.
    const fs::path old_game = kit.top / widen(kit.exe);
    for (const fs::path& p : {kit.top / widen("Play " + kit.title + ".bat"), old_game, fs::path(old_game).concat(L".previous"),
                              fs::path(old_game).concat(L".new"), kit.top / "run.log", kit.top / "run-errors.log"})
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
        if (it->path().extension() == L".d") remove_file(it->path(), out);  // Ninja's dependency files
    return out;
}

}  // namespace qilin::launcher
