// What the launcher needs from Windows (platform.hpp, and kit.hpp's own_path,
// show, claim_folder and cpu_identity).
#include "platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cwchar>
#include <cwctype>
#include <fstream>
#include <functional>
#include <span>

#include "kit.hpp"
#include "process.hpp"
#include "hash.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

fs::path from_utf8(std::string_view utf8) { return fs::path(widen(utf8)); }

std::string show(const fs::path& path) { return narrow(path.wstring()); }

std::string environment(const char* name) {
    const std::wstring wide_name = widen(name);
    const DWORD n = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (!n) return {};
    std::wstring value(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(wide_name.c_str(), value.data(), n);
    value.resize(got < n ? got : 0);
    return narrow(value);
}

fs::path user_data_base() {
    const std::string base = environment("LOCALAPPDATA");
    return base.empty() ? fs::path() : from_utf8(base);
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

std::expected<void, std::string> rename_over(const fs::path& from, const fs::path& to) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {};
    return std::unexpected(system_message(GetLastError()));
}

std::expected<void, std::string> swap_in(const fs::path& target, const fs::path& staged, const fs::path& previous) {
    std::error_code ec;
    const bool ok = fs::exists(target, ec)
                        ? ReplaceFileW(target.c_str(), staged.c_str(), previous.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0
                        : MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
    if (ok) return {};
    return std::unexpected(system_message(GetLastError()));
}

std::string release_asset_suffix() { return "windows-x64.zip"; }
std::string_view executable_suffix() { return ".exe"; }
std::string host_architecture() { return "x86_64"; }  // the Windows download is x64 only

bool claim_folder(const Kit& kit) {
    std::wstring folder = fs::absolute(kit.top).wstring();
    for (wchar_t& c : folder) c = static_cast<wchar_t>(towlower(c));
    const std::string key = narrow(folder);
    const std::string name = "Local\\QilinRecomp-launcher-" + sha256_hex(std::span(reinterpret_cast<const uint8_t*>(key.data()), key.size()));
    // Kept until the launcher ends.
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, widen(name).c_str());
    return mutex && GetLastError() != ERROR_ALREADY_EXISTS;
}

std::string cpu_identity() { return environment("PROCESSOR_IDENTIFIER"); }

std::uint64_t available_memory() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys : 0;
}

void open_with_system(const fs::path& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

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
std::expected<void, std::string> http_get(const std::string& url, const std::vector<std::string>& headers,
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
    std::wstring header_text;
    for (const std::string& h : headers) header_text += widen(h) + L"\r\n";
    if (!WinHttpSendRequest(request.h, header_text.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_text.c_str(),
                            header_text.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return std::unexpected(network_error(GetLastError()));

    DWORD status = 0;
    DWORD size = sizeof status;
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX))
        return std::unexpected(network_error(GetLastError()));
    if (status == 403 || status == 429) return std::unexpected(std::string("GitHub is turning away too many requests just now: try again in an hour"));
    if (status != 200) return std::unexpected("the server answered " + std::to_string(status));

    std::vector<char> buffer(1 << 16);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) return std::unexpected(network_error(GetLastError()));
        if (got == 0) break;
        if (!sink(buffer.data(), got)) return std::unexpected(std::string("stopped"));
    }
    return {};
}

fs::path system_tool(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return fs::path(std::wstring(dir, n < MAX_PATH ? n : 0)) / name;
}

}  // namespace

std::expected<std::string, std::string> fetch_text(const std::string& url, const std::vector<std::string>& headers) {
    std::string body;
    const auto got = http_get(url, headers, [&](const char* p, std::size_t n) {
        body.append(p, n);
        return body.size() < (64u << 20);
    });
    if (!got) return std::unexpected(got.error());
    return body;
}

std::expected<void, std::string> download(const std::string& url, const fs::path& to, std::atomic<std::uint64_t>& received,
                                          const std::atomic<bool>& cancel) {
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return std::unexpected("can't write " + show(to));
    const auto got = http_get(url, {}, [&](const char* p, std::size_t n) {
        out.write(p, static_cast<std::streamsize>(n));
        received += n;
        return !cancel && static_cast<bool>(out);
    });
    out.close();
    if (cancel) return std::unexpected(std::string("stopped"));
    if (!got) return std::unexpected(got.error());
    if (!out) return std::unexpected(std::string("can't write the download (is the drive full?)"));
    return {};
}

std::expected<void, std::string> unpack(const fs::path& archive, const fs::path& into, const fs::path& log) {
    // Windows' own tar.exe (Windows 10 1803 and later) reads zips.
    const fs::path tar = system_tool(L"tar.exe");
    std::error_code ec;
    if (!fs::is_regular_file(tar, ec)) return std::unexpected(std::string("this Windows has no tar.exe to unpack it (Windows 10 1803 or later has it)"));
    const auto code = run(tool(tar, {"-xf", show(archive), "-C", show(into)}, into, log));
    if (!code) return std::unexpected(code.error());
    if (*code != 0) return std::unexpected(describe_exit(*code));
    return {};
}

}  // namespace qilin::launcher
