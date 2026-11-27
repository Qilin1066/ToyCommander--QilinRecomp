// Running the kit's programs on Windows without console windows: the
// recompiler, the compiler, tar.exe and the game itself. Every path and
// argument is UTF-8 here and UTF-16 to Windows.
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::launcher {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
// What a Windows error code says, in the system's words (one line).
std::string system_message(unsigned long code);

// One argument quoted as CommandLineToArgvW and the C runtime read it back.
std::wstring quote_argument(std::wstring_view arg);

struct Command {
    std::filesystem::path exe;
    std::vector<std::string> args;  // UTF-8, without the program's own name
    std::filesystem::path cwd;
    std::filesystem::path out;  // standard output (and errors, without `err`); empty: discarded
    std::filesystem::path err;  // standard error, if apart
    bool append = false;        // add to `out`/`err` rather than replace them
    bool build_tool = true;     // in the build's job: killed with the launcher, or by stop_build_tools()
    bool low_priority = false;  // below normal, so the PC stays usable while it compiles
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

class Process {
public:
    static std::expected<Process, std::string> start(const Command& command);
    Process(Process&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();

    bool running() const;
    // Waits for the end; the exit code.
    unsigned long wait();

private:
    explicit Process(void* handle) : handle_(handle) {}
    void* handle_ = nullptr;
};

// Starts and waits: the exit code, or why it couldn't start.
std::expected<unsigned long, std::string> run(const Command& command);

// Ends every build tool still running (closing the launcher does the same).
void stop_build_tools();

// "exit code 1", or for a crash "exit code 0xC0000005".
std::string describe_exit(unsigned long code);

}  // namespace qilin::launcher
