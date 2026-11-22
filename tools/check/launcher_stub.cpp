// Child-process fixtures for the real launcher and Ninja. No game data or code.
#include "command_line.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count(); argv = command_line.values();
    if (argc == 3 && std::strcmp(argv[1], "info") == 0) {
        std::puts("{\"build\": {\"title\": \"fixture\"}, \"sha256\": \"fixture\"}");
        return 0;
    }
    if (argc == 7 && std::strcmp(argv[1], "lift") == 0) {
        const auto dir = qilin::native_path(argv[4]);
        std::filesystem::create_directories(dir);
        // Match qilin lift: unchanged output keeps its timestamps.
        if (!std::filesystem::exists(dir / "fixture.cpp")) std::ofstream(dir / "fixture.cpp") << "// fixture\n";
        return 0;
    }
    // Stand in for the compiler only. Ninja itself is the real executable.
    bool compile = false;
    std::string output, dependency, revision;
    std::vector<std::string> objects;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-c") compile = true;
        if (arg == "-o" && i + 1 < argc) output = argv[++i];
        else if (arg == "-MF" && i + 1 < argc) dependency = argv[++i];
        else if (arg.starts_with("-DQILIN_KIT_COMPILE_ID=")) revision = arg;
        else if (arg.ends_with(".o")) objects.push_back(arg);
    }
    if (!output.empty()) {
        std::ofstream log("tool-calls.txt", std::ios::app);
        log << (compile ? "compile" : "link") << '\n';
        const auto target = qilin::native_path(output);
        if (compile) {
            std::filesystem::create_directories(target.parent_path());
            std::ofstream object(target, std::ios::binary);
            object << revision << '\n';
            std::ifstream header("../kit/include/stub.hpp", std::ios::binary);
            object << header.rdbuf();
            if (!dependency.empty()) std::ofstream(qilin::native_path(dependency)) << output << ": gen/fixture.cpp ../kit/include/stub.hpp\n";
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
