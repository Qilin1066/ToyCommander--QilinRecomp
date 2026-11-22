// The saves' history and the restart (save_history.hpp; from Punch-Out's).
#include "save_history.hpp"
#include "path.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX  // MinGW defines it already
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "system.hpp"
#include "qilin/host/file_store.hpp"

namespace dreamcast {
namespace fs = std::filesystem;

namespace {

constexpr size_t kKeep = 5;
constexpr const char* kHistory = "save-backups";
constexpr const char* kRequest = "restore-save.txt";

// The files that can be saved and restored, and what the menu calls them.
constexpr struct {
    const char* name;
    const char* label;
} kSaves[] = {{"memory_card_a1.bin", "Memory card"}, {"flash.bin", "Console flash memory"}};

const char* label_of(const std::string& name) {
    for (const auto& s : kSaves)
        if (name == s.name) return s.label;
    return nullptr;
}

bool fail(std::string& error, const std::string& text, const std::error_code& ec = {}) {
    error = text + (ec ? ": " + ec.message() : "");
    return false;
}

// A rename over an existing file; on Windows the file may still be open in
// a process that is closing (a restart's old one), so it's tried again.
void replace_file(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::rename(from, to, ec);
#if defined(_WIN32)
    for (int attempt = 0; ec && attempt < 25; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fs::rename(from, to, ec);
    }
#endif
}

bool copy_flushed(const fs::path& from, const fs::path& to, std::string& error) {
    if (!qilin::host::atomic_copy(from, to)) return fail(error, "cannot copy and flush the save");
    error.clear();
    return true;
}

// A copy in the history: save-backups/<save>.history/<time>-<sequence>.bak,
// nothing else (no other folder, no link). Its save's name in `name`.
bool backup_path(const fs::path& folder, const fs::path& backup, std::string& name, std::string& error) {
    const fs::path history = folder / kHistory;
    const fs::path relative = backup.lexically_relative(history);
    std::vector<fs::path> parts(relative.begin(), relative.end());
    if (parts.size() != 2 || backup.extension() != ".bak") return fail(error, "not a copy in the saves' history");
    std::string dir = qilin::utf8_path(parts[0]);
    constexpr std::string_view kSuffix = ".history";
    if (!dir.ends_with(kSuffix)) return fail(error, "not a copy in the saves' history");
    dir.resize(dir.size() - kSuffix.size());
    if (!label_of(dir)) return fail(error, "not a save this program keeps");
    std::error_code ec;
    for (const fs::path& p : {history, history / parts[0], backup}) {
        const auto status = fs::symlink_status(p, ec);
        if (ec) return fail(error, "cannot inspect a path in the saves' history", ec);
        if (fs::is_symlink(status)) return fail(error, "a link in the saves' history");
    }
    if (!fs::is_regular_file(backup, ec)) return fail(error, "the copy is missing", ec);
    name = dir;
    return true;
}

}  // namespace

bool write_file_safely(const fs::path& file, const uint8_t* data, size_t n, std::string& error) {
    if (!qilin::host::atomic_write(file, {reinterpret_cast<const char*>(data), n})) return fail(error, "cannot replace the file safely");
    error.clear();
    return true;
}

bool backup_save(const fs::path& folder, const std::string& name, std::string& error) {
    if (!label_of(name)) return fail(error, "not a save this program keeps");
    const fs::path file = folder / name;
    std::error_code ec;
    const auto status = fs::symlink_status(file, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found)) return error.clear(), true;
    if (ec) return fail(error, "cannot inspect the existing save", ec);
    if (!fs::is_regular_file(status)) return fail(error, qilin::utf8_path(file) + " isn't a plain file");
    const fs::path history = folder / kHistory, dir = history / (name + ".history");
    for (const fs::path& p : {history, dir}) {
        const auto entry = fs::symlink_status(p, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) return fail(error, "cannot inspect the saves' history", ec);
        if (!ec && fs::is_symlink(entry)) return fail(error, "a link in the saves' history");
    }
    fs::create_directories(dir, ec);
    if (ec) return fail(error, "cannot make the saves' history", ec);
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    fs::path destination;
    for (unsigned sequence = 0;; ++sequence) {
        std::ostringstream n;
        n << std::setw(20) << std::setfill('0') << ticks << '-' << std::setw(6) << sequence << ".bak";
        destination = dir / n.str();
        const bool exists = fs::exists(destination, ec);
        if (ec) return fail(error, "cannot inspect a snapshot name", ec);
        if (!exists) break;
    }
    fs::path temporary = destination;
    temporary += ".tmp";
    if (!copy_flushed(file, temporary, error)) return false;
    fs::rename(temporary, destination, ec);
    if (ec) {
        const auto reason = ec;
        fs::remove(temporary, ec);
        return fail(error, "cannot keep the save's copy", reason);
    }
    std::vector<fs::path> copies;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_regular_file(it->symlink_status(ec)) && it->path().extension() == ".bak") copies.push_back(it->path());
    if (ec) return fail(error, "cannot list snapshots for retention", ec);
    std::sort(copies.begin(), copies.end());
    for (size_t i = 0; i + kKeep < copies.size(); ++i) {
        fs::remove(copies[i], ec);
        if (ec) return fail(error, "cannot remove an expired snapshot", ec);
    }
    error.clear();
    return true;
}

std::vector<SaveBackup> save_backups(const fs::path& folder) {
    std::vector<SaveBackup> out;
    std::error_code ec;
    for (const auto& s : kSaves) {
        const fs::path dir = folder / kHistory / (std::string(s.name) + ".history");
        if (!fs::is_directory(dir, ec)) continue;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name, why;
            if (!backup_path(folder, it->path(), name, why)) continue;
            const std::string stamp = qilin::utf8_path(it->path().stem()).substr(0, 20);
            const auto seconds = static_cast<std::time_t>(std::strtoll(stamp.c_str(), nullptr, 10) / 1000000);
            std::tm calendar{};
#if defined(_WIN32)
            gmtime_s(&calendar, &seconds);
#else
            gmtime_r(&seconds, &calendar);
#endif
            char when[40]{};
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &calendar);
            out.push_back({it->path(), std::string(s.label) + " / " + when});
        }
    }
    std::sort(out.begin(), out.end(), [](const SaveBackup& a, const SaveBackup& b) { return a.file.filename() > b.file.filename(); });
    return out;
}

bool request_save_restore(const fs::path& folder, const fs::path& backup, std::string& error) {
    std::string name;
    if (!backup_path(folder, backup, name, error)) return false;
    const std::string line = backup.lexically_relative(folder / kHistory).generic_string() + "\n";
    return write_file_safely(folder / kRequest, reinterpret_cast<const uint8_t*>(line.data()), line.size(), error);
}

bool apply_save_restore(const fs::path& folder, std::string& error) {
    const fs::path request = folder / kRequest;
    std::error_code ec;
    std::string relative;
    bool missing = false;
    if (!qilin::host::read_file(request, relative, &missing, 1024))
        return missing ? (error.clear(), true) : fail(error, "cannot read the restore request (maximum 1024 bytes)");
    if (relative.starts_with("\xEF\xBB\xBF")) relative.erase(0, 3);
    if (relative.ends_with('\n')) relative.pop_back();
    if (relative.ends_with('\r')) relative.pop_back();
    if (relative.empty() || relative.find_first_of("\r\n") != std::string::npos || relative.find('\0') != std::string::npos)
        return fail(error, "the restore request isn't one line");
    const fs::path backup = folder / kHistory / qilin::native_path(relative);
    std::string name;
    if (!backup_path(folder, backup, name, error)) return false;
    // The copy is staged before the present save is kept: keeping it may
    // remove the oldest copy, which may be the one asked for.
    const fs::path target = folder / name;
    fs::path temporary = target;
    temporary += ".restore.tmp";
    if (!copy_flushed(backup, temporary, error)) return false;
    if (!backup_save(folder, name, error)) {
        fs::remove(temporary, ec);
        return false;
    }
    replace_file(temporary, target, ec);
    if (ec) {
        const auto reason = ec;
        fs::remove(temporary, ec);
        return fail(error, "cannot restore " + qilin::utf8_path(target), reason);
    }
    fs::remove(request, ec);
    if (ec) return fail(error, "the save was restored, but the request couldn't be removed", ec);
    qilin::rt::log("saves: %s restored from %s", name.c_str(), relative.c_str());
    error.clear();
    return true;
}

// ---- the restart

namespace {
std::vector<std::string> g_args;  // argv, argv[0] made absolute if it was a path
}

void set_command_line(int argc, char** argv) {
    g_args.assign(argv, argv + argc);
    // A relative path to the program would break if anything changed folder;
    // a bare name is found on PATH again.
    if (!g_args.empty() && g_args[0].find_first_of("/\\") != std::string::npos) g_args[0] = qilin::utf8_path(fs::absolute(qilin::native_path(g_args[0])));
}

void restart_program() {
    std::fflush(nullptr);
#if defined(_WIN32)
    // The command line exactly as this process got it: arguments with spaces
    // (disc folders often have them) stay quoted as the player quoted them.
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring line = GetCommandLineW();
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        qilin::rt::fatal("restart: cannot start the program again (error %lu)", GetLastError());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::_Exit(0);  // guest threads are parked on host threads: no static destructors
#else
    if (g_args.empty()) qilin::rt::fatal("restart: the command line wasn't recorded");
    std::vector<char*> argv;
    for (std::string& a : g_args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());  // returns only on failure
    qilin::rt::fatal("restart: cannot start %s again", argv[0]);
#endif
}

}  // namespace dreamcast
