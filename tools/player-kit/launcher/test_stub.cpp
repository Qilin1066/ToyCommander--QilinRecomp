// Child-process stand-ins for the launcher's build test (launcher_build_test):
// the recompiler (`identify`, `lift`, and `sub`, a second step's tool), the
// compiler (`--version`, `-c`, linking) and the game (a path that exists, or
// `--data <folder> --fullscreen`). No game data or code. Windows, Linux and
// macOS; as the compiler, a leading "c++" (zig's) is skipped. Self-contained:
// it needs nothing from the project the launcher folder is copied into.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A UTF-8 argument as the system's path.
fs::path native(const std::string& utf8) {
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
    return fs::path(wide);
#else
    return fs::path(utf8);
#endif
}

// The arguments as UTF-8 (Windows: from the UTF-16 command line).
std::vector<std::string> arguments(int argc, char** argv) {
    std::vector<std::string> out;
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    int n = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; wide && i < n; ++i) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(bytes > 0 ? bytes - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), bytes, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    if (wide) LocalFree(wide);
#else
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
#endif
    return out;
}

// One line appended to work/tool-calls.txt (the current folder). Compiles run
// side by side: an append by the system is atomic (the C runtime's seeks to
// the end, then writes, and can lose lines).
bool record(const char* what) {
    const std::string line = std::string(what) + "\n";
#if defined(_WIN32)
    const HANDLE log = CreateFileW(L"tool-calls.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(log, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(log);
    return ok != 0;
#else
    const int log = open("tool-calls.txt", O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (log < 0) return false;
    const bool ok = write(log, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    close(log);
    return ok;
#endif
}

std::string read_all(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// qilin identify's contract (ADOPTING.md), answered from the input's text (a
// folder's identity.txt): "right", "wrong", "liar" (says ok, exits 3), or
// anything else (unreadable).
int identify(const fs::path& input) {
    std::error_code ec;
    const std::string text = read_all(fs::is_directory(input, ec) ? input / "identity.txt" : input);
    if (text.starts_with("right")) {
        std::printf("{\"ok\": true, \"title\": \"Fixture (World)\", \"id\": \"T-0\", \"version\": \"1.000\", "
                    "\"message\": \"This is Fixture (World), T-0 version 1.000: the right one.\"}\n");
        return 0;
    }
    if (text.starts_with("wrong")) {
        std::printf("{\"ok\": false, \"title\": \"Other Game (Europe)\", \"id\": \"T-1\", \"version\": \"1.050\", "
                    "\"message\": \"This is Other Game (Europe). This download is for Fixture (World), T-0 version 1.000.\"}\n");
        return 3;
    }
    if (text.starts_with("liar")) {
        std::printf("{\"ok\": true, \"title\": \"\", \"id\": \"\", \"version\": \"\", \"message\": \"\"}\n");
        return 3;
    }
    std::fprintf(stderr, "stub: nothing to read\n");
    std::printf("{\"ok\": false, \"title\": \"\", \"id\": \"\", \"version\": \"\", \"message\": \"Can't read it at all.\"}\n");
    return 2;
}

// A lifted file, written only when missing (qilin lift leaves unchanged output's timestamps).
void lifted(const fs::path& path, const char* text) {
    if (!fs::exists(path)) std::ofstream(path) << text;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args = arguments(argc, argv);
    const std::string self = args.empty() ? std::string() : args[0];
    if (args.size() >= 2 && args[1] == "c++") args.erase(args.begin() + 1);  // zig c++
    const std::size_t n = args.size();
    if (n == 2 && args[1] == "--version") {
        // The test changes the "compiler" by changing what it says it is.
        const char* version = std::getenv("QILIN_STUB_COMPILER");
        std::printf("stub clang version %s\n", version ? version : "1");
        return 0;
    }
    if (n >= 4 && args[1] == "identify" && args[3] == "--json") return identify(native(args[2]));
    if (n == 7 && args[1] == "lift" && args[3] == "--out" && args[5] == "--files") {
        if (!fs::exists(native(args[2]))) return 7;
        const fs::path dir = native(args[4]);
        fs::create_directories(dir);
        lifted(dir / "fixture.cpp", "// fixture\n");
        lifted(dir / "table.cpp", "// fixture table\n");
        lifted(dir / "functions.hpp", "// fixture\n");
        return record("lift") ? 0 : 6;
    }
    // A second step: its output in a subfolder of gen, its last argument one
    // with a space (the launcher's quoting kept it whole).
    if (n == 6 && args[1] == "sub" && args[3] == "--out") {
        if (!fs::exists(native(args[2]))) return 7;
        if (args[5] != "--note=two words") return 8;
        const fs::path dir = native(args[4]);
        fs::create_directories(dir);
        lifted(dir / "extra.cpp", "// fixture, in a subfolder\n");
        lifted(dir / "extra.hpp", "// fixture\n");
        return record("sub") ? 0 : 6;
    }
    // Stand in for the compiler.
    bool compile = false;
    std::string output;
    std::vector<std::string> objects;
    for (std::size_t i = 1; i < n; ++i) {
        const std::string& arg = args[i];
        if (arg == "-c") compile = true;
        if (arg == "-o" && i + 1 < n) output = args[++i];
        else if (arg.ends_with(".o")) objects.push_back(arg);
    }
    if (!output.empty()) {
        if (!record(compile ? "compile" : "link")) return 6;
        const fs::path target = native(output);
        if (compile) {
            fs::create_directories(target.parent_path());
            std::ofstream object(target, std::ios::binary);
            std::ifstream header("../kit/include/stub.hpp", std::ios::binary);
            object << header.rdbuf();
            return object ? 0 : 3;
        }
        fs::copy_file(native(self), target, fs::copy_options::overwrite_existing);
        std::error_code runnable;
        fs::permissions(target, fs::perms::owner_exec, fs::perm_options::add, runnable);
        // PE, ELF and Mach-O all run with data after them. It makes stale/fresh
        // links observable while retaining a runnable native child for the
        // argument-quoting check.
        std::ofstream executable(target, std::ios::binary | std::ios::app);
        for (const auto& name : objects) {
            std::ifstream object(native(name), std::ios::binary);
            if (!object) return 4;
            executable << object.rdbuf();
        }
        const auto archive_path = fs::directory_iterator("../kit/lib")->path();
        std::ifstream archive(archive_path, std::ios::binary);
        executable << archive.rdbuf();
        return executable ? 0 : 4;
    }
    // The game: what it was given, beside itself.
    std::string launched;
    if (n == 2 && fs::exists(native(args[1]))) launched = args[1];
    else if (n == 4 && args[1] == "--data" && fs::is_directory(native(args[2])) && args[3] == "--fullscreen") launched = args[2];
    if (!launched.empty()) {
        std::ofstream log(native(self).parent_path() / "launched.txt", std::ios::binary);
        log << launched;
        return log ? 0 : 5;
    }
    return 2;
}
