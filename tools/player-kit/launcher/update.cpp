// The Update button (update.hpp).
#include "update.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cwchar>
#include <fstream>
#include <functional>
#include <system_error>
#include <thread>

#include "kit_text.hpp"
#include "process.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

struct Internet {
    HINTERNET h = nullptr;
    explicit Internet(HINTERNET v) : h(v) {}
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    ~Internet() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::string network_error(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT: return "the connection timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "can't find the server (is this PC online?)";
        case ERROR_WINHTTP_CANNOT_CONNECT: return "can't connect to the server (is this PC online?)";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was broken";
        case ERROR_WINHTTP_SECURE_FAILURE: return "the secure connection failed";
        default: break;
    }
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out = n && text ? narrow(std::wstring_view(text, n)) : system_message(code);
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

// An HTTPS GET; the body goes to `sink` piece by piece (false from it stops).
// Redirects (GitHub's downloads go to its file servers) are followed.
std::expected<void, std::string> http_get(const std::string& url, const wchar_t* headers, UpdateProgress* progress,
                                          const std::function<bool(const char*, std::size_t)>& sink) {
    const std::wstring wide = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return std::unexpected("not an https address: " + url);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    // Nothing about the player: a generic name, the system's proxy settings.
    Internet session(WinHttpOpen(L"QilinRecomp-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h)  // before Windows 8.1
        session.h = WinHttpOpen(L"QilinRecomp-launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return std::unexpected(network_error(GetLastError()));
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);
    const Internet connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (!connection.h) return std::unexpected(network_error(GetLastError()));
    const Internet request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
    if (!request.h) return std::unexpected(network_error(GetLastError()));
    if (!WinHttpSendRequest(request.h, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? static_cast<DWORD>(-1) : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return std::unexpected(network_error(GetLastError()));

    DWORD status = 0;
    DWORD size = sizeof status;
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX))
        return std::unexpected(network_error(GetLastError()));
    if (status == 403 || status == 429) return std::unexpected(std::string("GitHub is turning away too many requests just now: try again in an hour"));
    if (status != 200) return std::unexpected("the server answered " + std::to_string(status));
    if (progress) {
        wchar_t length[32] = {};
        DWORD bytes = sizeof length;
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &bytes, WINHTTP_NO_HEADER_INDEX))
            progress->total = std::wcstoull(length, nullptr, 10);
    }

    std::vector<char> buffer(1 << 16);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) return std::unexpected(network_error(GetLastError()));
        if (got == 0) break;
        if (!sink(buffer.data(), got)) return std::unexpected(std::string("stopped"));
        if (progress) progress->received += got;
    }
    return {};
}

fs::path system_tool(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return fs::path(std::wstring(dir, n < MAX_PATH ? n : 0)) / name;
}

fs::path with_suffix(fs::path p, const wchar_t* suffix) {
    p += suffix;
    return p;
}

fs::path staging_folder(const Kit& kit) { return with_suffix(kit.top, L".update"); }

}  // namespace

std::expected<UpdateCheck, std::string> check_for_update(const Kit& kit) {
    std::string body;
    const auto got = http_get("https://api.github.com/repos/" + kit.repo + "/releases?per_page=100",
                              L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28", nullptr,
                              [&](const char* p, std::size_t n) {
                                  body.append(p, n);
                                  return body.size() < (64u << 20);
                              });
    if (!got) return std::unexpected("Couldn't ask GitHub for updates: " + got.error() + ".");
    const auto newest = newest_release(body, kit.game, kit.package);
    if (!newest) return std::unexpected("GitHub's list of releases can't be read (" + newest.error() + ").");
    UpdateCheck out;
    if (!*newest) {
        out.message = "There's no " + kit.title + " release on GitHub yet.";
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
    const fs::path zip = staging / widen(release.asset_name);
    {
        std::ofstream out(zip, std::ios::binary | std::ios::trunc);
        if (!out) return give_up("Can't write " + show(zip) + ".");
        const auto got = http_get(release.asset_url, nullptr, &progress, [&](const char* p, std::size_t n) {
            out.write(p, static_cast<std::streamsize>(n));
            return !cancel && static_cast<bool>(out);
        });
        out.close();
        if (cancel) return give_up("The update was stopped.");
        if (!got) return give_up("The download failed: " + got.error() + ".");
        if (!out) return give_up("Can't write the download (is the drive full?).");
        if (progress.total && progress.received != progress.total) return give_up("The download was cut short.");
    }

    // Unpack with Windows' own tar.exe (Windows 10 1803 and later).
    progress.unpacking = true;
    const fs::path tar = system_tool(L"tar.exe");
    if (!fs::is_regular_file(tar, ec)) return give_up("This Windows has no tar.exe to unpack the download (Windows 10 1803 or later has it).");
    {
        const auto code = run(tool(tar, {"-xf", show(zip), "-C", show(staging / "unpacked")}, staging, staging / "tar.log"));
        if (!code || *code != 0) return give_up("Unpacking the download failed (" + (code ? describe_exit(*code) : code.error()) + ").");
    }
    fs::path root = staging / "unpacked" / widen(kit.package);
    if (!fs::is_directory(root, ec)) return give_up("The download doesn't hold the folder " + kit.package + ".");
    const auto about_text = read_file(root / "kit" / "game.txt");
    if (!about_text) return give_up("The download has no kit\\game.txt.");
    const auto about = parse_settings(*about_text);
    const auto value = [&](std::string_view key) {
        const auto it = about.find(key);
        return it == about.end() ? std::string() : it->second;
    };
    if (value("game") != kit.game) return give_up("The download is for another game.");
    const std::string new_name = "Play " + value("title") + ".exe";
    const fs::path new_launcher = root / widen(new_name);
    if (value("title").empty() || !fs::is_regular_file(new_launcher, ec)) return give_up("The download has no " + new_name + ".");
    if (cancel) return give_up("The update was stopped.");

    // Swap kit\ for the new one, then the launcher (a running program can be
    // renamed, not deleted: the old one is deleted at the next start).
    const fs::path kit_old = kit.top / "kit.old";
    fs::remove_all(kit_old, ec);
    if (ec) return give_up("An earlier update's kit.old folder can't be removed (" + ec.message() + ").");
    fs::rename(kit.kit, kit_old, ec);
    if (ec) return give_up("Can't move the kit folder aside (" + ec.message() + "; is a file in it open?).");
    fs::rename(root / "kit", kit.kit, ec);
    if (ec) {
        std::error_code back;
        fs::rename(kit_old, kit.kit, back);
        return give_up("Can't move the new kit into place (" + ec.message() + ").");
    }
    fs::path launcher_old = with_suffix(kit.launcher, L".old");
    fs::remove(launcher_old, ec);
    if (fs::exists(launcher_old, ec))
        launcher_old = with_suffix(kit.launcher, widen("." + std::to_string(GetTickCount64()) + ".old").c_str());
    const fs::path target = kit.top / widen(new_name);
    const auto restore_kit = [&] {
        std::error_code back;
        fs::rename(kit.kit, root / "kit", back);
        fs::rename(kit_old, kit.kit, back);
    };
    fs::rename(kit.launcher, launcher_old, ec);
    if (ec) {
        restore_kit();
        return give_up("Can't move this launcher aside (" + ec.message() + ").");
    }
    fs::rename(new_launcher, target, ec);
    if (ec) {
        std::error_code back;
        fs::rename(launcher_old, kit.launcher, back);
        restore_kit();
        return give_up("Can't move the new launcher into place (" + ec.message() + ").");
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
}

void clean_after_update(const Kit& kit) {
    std::thread([top = kit.top, staging = staging_folder(kit)] {
        for (int attempt = 0; attempt < 20; ++attempt) {
            bool left = false;
            std::error_code ec;
            for (const fs::path& p : {top / "kit.old", top / "LICENSES.old", staging}) {
                if (!fs::exists(p, ec)) continue;
                fs::remove_all(p, ec);
                if (ec) left = true;
            }
            for (fs::directory_iterator it(top, ec), end; !ec && it != end; it.increment(ec)) {
                const std::wstring name = it->path().filename().wstring();
                if (name.starts_with(L"Play ") && name.ends_with(L".old")) {
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
