// Child-process stand-ins for the launcher's build test (launcher_build_test):
// the recompiler (`lift`), the compiler (`--version`, `-c`, linking) and the
// game (one argument: a file that exists). No game data or code.
#include "command_line.hpp"
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
            const HANDLE log = CreateFileW(L"tool-calls.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log == INVALID_HANDLE_VALUE) return 6;
            const char* line = compile ? "compile\n" : "link\n";
            DWORD written = 0;
            const BOOL ok = WriteFile(log, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
            CloseHandle(log);
            if (!ok) return 6;
        }
        const auto target = qilin::native_path(output);
        if (compile) {
            std::filesystem::create_directories(target.parent_path());
            std::ofstream object(target, std::ios::binary);
            std::ifstream header("../kit/include/stub.hpp", std::ios::binary);
            object << header.rdbuf();
            return object ? 0 : 3;
        }
        std::filesystem::copy_file(qilin::native_path(argv[0]), target, std::filesystem::copy_options::overwrite_existing);
        // PE permits an overlay. It makes stale/fresh links observable while
        // retaining a runnable native child for the argument-quoting check.
        std::ofstream executable(target, std::ios::binary | std::ios::app);
        for (const auto& name : objects) { std::ifstream object(qilin::native_path(name), std::ios::binary); executable << object.rdbuf(); }
        const auto archive_path = std::filesystem::directory_iterator("../kit/lib")->path();
        std::ifstream archive(archive_path, std::ios::binary);
        executable << archive.rdbuf();
        return executable ? 0 : 4;
    }
    if (argc == 2 && std::filesystem::exists(qilin::native_path(argv[1]))) {
        std::ofstream log(qilin::native_path(argv[0]).parent_path() / "launched.txt", std::ios::binary);
        log << argv[1]; return log ? 0 : 5;
    }
    return 2;
}
