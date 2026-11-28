// Running the kit's programs: the recompiler, the compiler, the unpacker and
// the game itself, with no console windows, their output to files. Windows:
// process_win.cpp (CreateProcessW, a job object for the build's tools);
// Linux and macOS: process_posix.cpp (fork and exec, a process group per
// build tool). Paths and arguments are UTF-8 here.
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qilin::launcher {

#if defined(_WIN32)
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
// One argument quoted as CommandLineToArgvW and the C runtime read it back.
std::wstring quote_argument(std::wstring_view arg);
#endif
// What a system error code says, in the system's words (one line).
std::string system_message(unsigned long code);

struct Command {
    std::filesystem::path exe;
    std::vector<std::string> args;  // UTF-8, without the program's own name
    std::filesystem::path cwd;
    std::filesystem::path out;  // standard output (and errors, without `err`); empty: discarded
    std::filesystem::path err;  // standard error, if apart
    bool append = false;        // add to `out`/`err` rather than replace them
    bool build_tool = true;     // a build tool: killed with the launcher, or by stop_build_tools()
    bool low_priority = false;  // below normal, so the computer stays usable while it compiles
    std::vector<std::pair<std::string, std::string>> environment;  // set for it, on top of the launcher's own
};

// A build tool: run in `cwd`, everything it prints to `out`.
inline Command tool(std::filesystem::path exe, std::vector<std::string> args, std::filesystem::path cwd, std::filesystem::path out) {
    Command c;
    c.exe = std::move(exe);
    c.args = std::move(args);
    c.cwd = std::move(cwd);
    c.out = std::move(out);
    return c;
}

// The command as one line for build.log, quoted as this system's shell would.
std::string describe_command(const Command& command);

class Process {
public:
    static std::expected<Process, std::string> start(const Command& command);
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();

    bool running();
    // Waits for the end; the exit code (on Linux and macOS a death by a
    // signal is 128 + the signal, as shells report it).
    unsigned long wait();
    // Ends it now (and, for a build tool, what it started).
    void terminate();

private:
#if defined(_WIN32)
    explicit Process(void* handle) : handle_(handle) {}
    void* handle_ = nullptr;
#else
    explicit Process(int pid) : pid_(pid) {}
    int pid_ = -1;
    bool done_ = false;
    unsigned long code_ = 0;
#endif
};

// Starts and waits: the exit code, or why it couldn't start.
std::expected<unsigned long, std::string> run(const Command& command);

// Ends every build tool still running (closing the launcher does the same).
void stop_build_tools();

// "exit code 1"; a crash: "exit code 0xC0000005" (Windows), "signal 11"
// (Linux, macOS).
std::string describe_exit(unsigned long code);

}  // namespace qilin::launcher
