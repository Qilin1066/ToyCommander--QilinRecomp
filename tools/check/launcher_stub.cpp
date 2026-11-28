// Child-process stand-ins for the launcher's build test (launcher_build_test):
// the recompiler (`lift`), the compiler (`--version`, `-c`, linking) and the
// game (one argument: a file that exists). No game data or code. Windows,
// Linux and macOS; as the compiler, a leading "c++" (zig's) is skipped.
#include "command_line.hpp"
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count(); argv = command_line.values();
    const char* self = argv[0];
    if (argc >= 2 && std::strcmp(argv[1], "c++") == 0) { ++argv; --argc; }  // zig c++
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        // The test changes the "compiler" by changing what it says it is.
        const char* version = std::getenv("QILIN_STUB_COMPILER");
        std::printf("stub clang version %s\n", version ? version : "1");
        return 0;
    }
    if (argc == 7 && std::strcmp(argv[1], "lift") == 0) {
        const auto dir = qilin::native_path(argv[4]);
        std::filesystem::create_directories(dir);
        // Match qilin lift: unchanged output keeps its timestamps.
        if (!std::filesystem::exists(dir / "fixture.cpp")) std::ofstream(dir / "fixture.cpp") << "// fixture\n";
        if (!std::filesystem::exists(dir / "table.cpp")) std::ofstream(dir / "table.cpp") << "// fixture table\n";
        if (!std::filesystem::exists(dir / "functions.hpp")) std::ofstream(dir / "functions.hpp") << "// fixture\n";
        return 0;
    }
    // Stand in for the compiler.
    bool compile = false;
    std::string output;
    std::vector<std::string> objects;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-c") compile = true;
        if (arg == "-o" && i + 1 < argc) output = argv[++i];
        else if (arg.ends_with(".o")) objects.push_back(arg);
    }
    if (!output.empty()) {
        {
            // Compiles run side by side: an append by the system is atomic
            // (the C runtime's seeks to the end, then writes, and can lose lines).
            const char* line = compile ? "compile\n" : "link\n";
#if defined(_WIN32)
            const HANDLE log = CreateFileW(L"tool-calls.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log == INVALID_HANDLE_VALUE) return 6;
            DWORD written = 0;
            const BOOL ok = WriteFile(log, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
            CloseHandle(log);
            if (!ok) return 6;
#else
            const int log = open("tool-calls.txt", O_WRONLY | O_APPEND | O_CREAT, 0644);
            if (log < 0) return 6;
            const auto length = static_cast<ssize_t>(std::strlen(line));
            const bool ok = write(log, line, static_cast<size_t>(length)) == length;
            close(log);
            if (!ok) return 6;
#endif
        }
        const auto target = qilin::native_path(output);
        if (compile) {
            std::filesystem::create_directories(target.parent_path());
            std::ofstream object(target, std::ios::binary);
            std::ifstream header("../kit/include/stub.hpp", std::ios::binary);
            object << header.rdbuf();
            return object ? 0 : 3;
        }
        std::filesystem::copy_file(qilin::native_path(self), target, std::filesystem::copy_options::overwrite_existing);
        std::error_code runnable;
        std::filesystem::permissions(target, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add, runnable);
        // PE, ELF and Mach-O all run with data after them. It makes stale/fresh
        // links observable while retaining a runnable native child for the
        // argument-quoting check.
        std::ofstream executable(target, std::ios::binary | std::ios::app);
        for (const auto& name : objects) { std::ifstream object(qilin::native_path(name), std::ios::binary); executable << object.rdbuf(); }
        const auto archive_path = std::filesystem::directory_iterator("../kit/lib")->path();
        std::ifstream archive(archive_path, std::ios::binary);
        executable << archive.rdbuf();
        return executable ? 0 : 4;
    }
    if (argc == 2 && std::filesystem::exists(qilin::native_path(argv[1]))) {
        std::ofstream log(qilin::native_path(self).parent_path() / "launched.txt", std::ios::binary);
        log << argv[1]; return log ? 0 : 5;
    }
    return 2;
}
