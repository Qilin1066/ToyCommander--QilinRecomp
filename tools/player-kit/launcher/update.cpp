// The Update button (update.hpp).
#include "update.hpp"

#include <chrono>
#include <system_error>
#include <thread>

#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

fs::path with_suffix(fs::path p, std::string_view suffix) {
    p += from_utf8(suffix);
    return p;
}

// A name only the launcher makes: it is emptied before each download.
fs::path staging_folder(const Kit& kit) { return with_suffix(kit.top, ".qilin-update"); }

// Moves `from` to `to`; on a failure, says so.
std::expected<void, std::string> move(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) return std::unexpected(ec.message());
    return {};
}

}  // namespace

std::expected<UpdateCheck, std::string> check_for_update(const Kit& kit) {
    if (kit.repo.empty()) return std::unexpected(std::string("This download doesn't say where its updates are published (game.txt has no repo)."));
    const auto body = fetch_text("https://api.github.com/repos/" + kit.repo + "/releases?per_page=100",
                                 {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"});
    if (!body) return std::unexpected("Couldn't ask GitHub for updates: " + body.error() + ".");
    const auto newest = newest_release(*body, kit.release_tag_prefix, kit.release_asset_prefix, release_asset_suffix());
    if (!newest) return std::unexpected("GitHub's list of releases can't be read (" + newest.error() + ").");
    UpdateCheck out;
    if (!*newest) {
        out.message = "There's no " + kit.title + " release for this system on GitHub yet.";
        return out;
    }
    const auto here = parse_version(kit.version);
    if (here && (*newest)->version <= *here) {
        out.message = "This is the newest version (" + to_string(*here) + ").";
        return out;
    }
    out.newer = **newest;
    out.message = "Version " + to_string((*newest)->version) + " is out" + (here ? " (this is " + to_string(*here) + ")" : "") + ".";
    return out;
}

std::expected<fs::path, std::string> install_update(const Kit& kit, const Release& release, UpdateProgress& progress,
                                                    const std::atomic<bool>& cancel) {
    std::error_code ec;
    const fs::path staging = staging_folder(kit);
    if (kit.top.parent_path() == kit.top) return std::unexpected(std::string("This folder is a drive's top folder: move it into a folder of its own to update."));
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "unpacked", ec);
    if (ec) return std::unexpected("Can't make a folder beside this one for the download (" + ec.message() + "). Nothing was changed.");
    const auto give_up = [&](std::string message) {
        std::error_code ignore;
        fs::remove_all(staging, ignore);
        return std::unexpected(std::move(message) + " Nothing was changed.");
    };

    // Download.
    progress.total = release.asset_size > 0 ? static_cast<std::uint64_t>(release.asset_size) : 0;
    const fs::path archive = staging / from_utf8(release.asset_name);
    if (const auto got = download(release.asset_url, archive, progress.received, cancel); !got) {
        if (cancel) return give_up("The update was stopped.");
        return give_up("The download failed: " + got.error() + ".");
    }
    if (progress.total && progress.received != progress.total) return give_up("The download was cut short.");

    // Unpack.
    progress.unpacking = true;
    if (const auto unpacked = unpack(archive, staging / "unpacked", staging / "unpack.log"); !unpacked)
        return give_up("Unpacking the download failed (" + unpacked.error() + ").");
    if (cancel) return give_up("The update was stopped.");

#if defined(__APPLE__)
    // The whole app at once: <Title>.app, beside the one running.
    const fs::path app = staging / "unpacked" / from_utf8(kit.title + ".app");
    const auto about_text = read_file(app / "Contents" / "Resources" / "kit" / "game.txt");
    if (!about_text) return give_up("The download has no " + kit.title + ".app.");
    const auto about = parse_settings(*about_text);
    if (const auto it = about.find("game"); it == about.end() || it->second != kit.game) return give_up("The download is for another game.");
    const fs::path old_app = with_suffix(kit.top, ".old");
    fs::remove_all(old_app, ec);
    if (const auto moved = move(kit.top, old_app); !moved) return give_up("Can't move this app aside (" + moved.error() + ").");
    if (const auto moved = move(app, kit.top); !moved) {
        (void)move(old_app, kit.top);
        return give_up("Can't move the new app into place (" + moved.error() + ").");
    }
    fs::remove_all(staging, ec);
    return kit.top / "Contents" / "MacOS" / kit.launcher.filename();  // the old app goes at the next start
#else
    fs::path root = staging / "unpacked" / from_utf8(kit.package);
    if (!fs::is_directory(root, ec)) return give_up("The download doesn't hold the folder " + kit.package + ".");
    const auto about_text = read_file(root / "kit" / "game.txt");
    if (!about_text) return give_up("The download has no kit/game.txt.");
    const auto about = parse_settings(*about_text);
    const auto value = [&](std::string_view key) {
        const auto it = about.find(key);
        return it == about.end() ? std::string() : it->second;
    };
    if (value("game") != kit.game) return give_up("The download is for another game.");
    const std::string new_name = "Play " + value("title") + std::string(executable_suffix());
    const fs::path new_launcher = root / from_utf8(new_name);
    if (value("title").empty() || !fs::is_regular_file(new_launcher, ec)) return give_up("The download has no " + new_name + ".");
    if (cancel) return give_up("The update was stopped.");

    // Swap kit/ for the new one, then the launcher (a running program can be
    // renamed, not always deleted: the old one goes at the next start).
    const fs::path kit_old = kit.top / "kit.old";
    fs::remove_all(kit_old, ec);
    if (ec) return give_up("An earlier update's kit.old folder can't be removed (" + ec.message() + ").");
    if (const auto moved = move(kit.kit, kit_old); !moved) return give_up("Can't move the kit folder aside (" + moved.error() + "; is a file in it open?).");
    if (const auto moved = move(root / "kit", kit.kit); !moved) {
        (void)move(kit_old, kit.kit);
        return give_up("Can't move the new kit into place (" + moved.error() + ").");
    }
    fs::path launcher_old = with_suffix(kit.launcher, ".old");
    fs::remove(launcher_old, ec);
    if (fs::exists(launcher_old, ec))
        launcher_old = with_suffix(kit.launcher, "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".old");
    const fs::path target = kit.top / from_utf8(new_name);
    const auto restore_kit = [&] {
        (void)move(kit.kit, root / "kit");
        (void)move(kit_old, kit.kit);
    };
    if (const auto moved = move(kit.launcher, launcher_old); !moved) {
        restore_kit();
        return give_up("Can't move this launcher aside (" + moved.error() + ").");
    }
    if (const auto moved = move(new_launcher, target); !moved) {
        (void)move(launcher_old, kit.launcher);
        restore_kit();
        return give_up("Can't move the new launcher into place (" + moved.error() + ").");
    }

    // The rest is only words: the readme and the licences.
    std::error_code ignore;
    fs::copy_file(root / "README.txt", kit.top / "README.txt", fs::copy_options::overwrite_existing, ignore);
    if (fs::is_directory(root / "LICENSES", ignore)) {
        const fs::path licences_old = kit.top / "LICENSES.old";
        fs::remove_all(licences_old, ignore);
        fs::rename(kit.top / "LICENSES", licences_old, ignore);
        fs::rename(root / "LICENSES", kit.top / "LICENSES", ignore);
        if (ignore) fs::rename(licences_old, kit.top / "LICENSES", ignore);
        else fs::remove_all(licences_old, ignore);
    }
    fs::remove_all(kit_old, ignore);  // or at the next start
    fs::remove_all(staging, ignore);
    return target;
#endif
}

void clean_after_update(const Kit& kit) {
    std::thread([top = kit.top, staging = staging_folder(kit)] {
        for (int attempt = 0; attempt < 20; ++attempt) {
            bool left = false;
            std::error_code ec;
            std::vector<fs::path> leftovers = {top / "kit.old", top / "LICENSES.old", staging};
#if defined(__APPLE__)
            leftovers.push_back(with_suffix(top, ".old"));  // the app before an update
#endif
            for (const fs::path& p : leftovers) {
                if (!fs::exists(p, ec)) continue;
                fs::remove_all(p, ec);
                if (ec) left = true;
            }
            for (fs::directory_iterator it(top, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = show(it->path().filename());
                if (name.starts_with("Play ") && name.ends_with(".old")) {
                    std::error_code rm;
                    fs::remove(it->path(), rm);
                    if (rm) left = true;
                }
            }
            if (!left) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));  // the old launcher is still closing
        }
    }).detach();
}

bool start_detached(const fs::path& exe, const fs::path& cwd) {
    Command c;
    c.exe = exe;
    c.cwd = cwd;
    c.build_tool = false;
    return Process::start(c).has_value();
}

}  // namespace qilin::launcher
