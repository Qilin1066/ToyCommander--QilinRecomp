// Running programs on Linux and macOS (process.hpp): fork and exec, output to
// files, each build tool the leader of its own process group so it and
// anything it starts end together.
#include "process.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

extern char** environ;

namespace qilin::launcher {

std::string system_message(unsigned long code) { return std::strerror(static_cast<int>(code)); }

namespace {

// Quoted for a POSIX shell: as it is when it's plain, else in single quotes.
std::string shell_quote(std::string_view s) {
    if (!s.empty() && s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_./=:+,@%") == std::string_view::npos)
        return std::string(s);
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

// The build tools still running (each its own process group).
std::mutex g_mutex;
std::set<int> g_tools;

}  // namespace

std::string describe_command(const Command& command) {
    std::string text = shell_quote(command.exe.filename().string());
    for (const std::string& a : command.args) text += " " + shell_quote(a);
    return text;
}

std::string describe_exit(unsigned long code) {
    if (code > 128 && code < 128 + 65) {
        const int sig = static_cast<int>(code - 128);
        const char* name = strsignal(sig);
        return "signal " + std::to_string(sig) + (name ? std::string(" (") + name + ")" : std::string());
    }
    return "exit code " + std::to_string(code);
}

std::expected<Process, std::string> Process::start(const Command& command) {
    // Everything the child needs is made before the fork: after it, in a
    // program with threads, only async-signal-safe calls are allowed.
    const std::string exe = command.exe.string();
    std::vector<std::string> args_text;
    args_text.push_back(exe);
    for (const std::string& a : command.args) args_text.push_back(a);
    std::vector<char*> argv;
    for (std::string& a : args_text) argv.push_back(a.data());
    argv.push_back(nullptr);

    std::vector<std::string> env_text;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view entry(*e);
        bool replaced = false;
        for (const auto& [name, value] : command.environment)
            if (entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=') replaced = true;
        if (!replaced) env_text.emplace_back(entry);
    }
    for (const auto& [name, value] : command.environment) env_text.push_back(name + "=" + value);
    std::vector<char*> envp;
    for (std::string& e : env_text) envp.push_back(e.data());
    envp.push_back(nullptr);

    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (command.append ? O_APPEND : O_TRUNC);
    const int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
    const int out = command.out.empty() ? open("/dev/null", O_WRONLY | O_CLOEXEC) : open(command.out.c_str(), flags, 0644);
    if (out < 0) {
        const int e = errno;
        if (in >= 0) close(in);
        return std::unexpected("can't write " + command.out.string() + ": " + std::strerror(e));
    }
    int err = out;
    if (!command.err.empty()) {
        err = open(command.err.c_str(), flags, 0644);
        if (err < 0) {
            const int e = errno;
            close(out);
            if (in >= 0) close(in);
            return std::unexpected("can't write " + command.err.string() + ": " + std::strerror(e));
        }
    }
    const std::string cwd = command.cwd.string();
    // A pipe that closes on exec: the child writes errno into it if exec fails.
    int report[2];
    if (pipe(report) != 0) return std::unexpected(std::string("can't start a program: ") + std::strerror(errno));
    fcntl(report[0], F_SETFD, FD_CLOEXEC);
    fcntl(report[1], F_SETFD, FD_CLOEXEC);

    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid == 0) {
        if (command.build_tool) {
            setpgid(0, 0);
#if defined(__linux__)
            prctl(PR_SET_PDEATHSIG, SIGKILL);  // ends with the launcher
            if (getppid() != parent) _exit(127);
#endif
        }
        if (command.low_priority) setpriority(PRIO_PROCESS, 0, 10);
        if (in >= 0) dup2(in, 0);
        dup2(out, 1);
        dup2(err, 2);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            const int e = errno;
            (void)!write(report[1], &e, sizeof e);
            _exit(127);
        }
        execve(exe.c_str(), argv.data(), envp.data());
        const int e = errno;
        (void)!write(report[1], &e, sizeof e);
        _exit(127);
    }
    (void)parent;
    const int fork_errno = errno;
    close(report[1]);
    if (in >= 0) close(in);
    close(out);
    if (err != out) close(err);
    if (pid < 0) {
        close(report[0]);
        return std::unexpected(std::string("can't start a program: ") + std::strerror(fork_errno));
    }
    if (command.build_tool) {
        setpgid(pid, pid);  // as the child does: whichever runs first
        const std::lock_guard lock(g_mutex);
        g_tools.insert(pid);
    }
    int child_errno = 0;
    ssize_t got;
    do got = read(report[0], &child_errno, sizeof child_errno);
    while (got < 0 && errno == EINTR);
    close(report[0]);
    Process p(pid);
    if (got == static_cast<ssize_t>(sizeof child_errno)) {
        p.wait();
        return std::unexpected("can't start " + command.exe.filename().string() + ": " + std::strerror(child_errno));
    }
    return p;
}

Process::Process(Process&& other) noexcept : pid_(other.pid_), done_(other.done_), code_(other.code_) { other.pid_ = -1; }

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        pid_ = other.pid_;
        done_ = other.done_;
        code_ = other.code_;
        other.pid_ = -1;
    }
    return *this;
}

Process::~Process() = default;  // a process still running is left to run (the game, after a relaunch)

namespace {

unsigned long exit_code(int status) {
    if (WIFEXITED(status)) return static_cast<unsigned long>(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return 128ul + static_cast<unsigned long>(WTERMSIG(status));
    return 255;
}

void forget_tool(int pid) {
    const std::lock_guard lock(g_mutex);
    g_tools.erase(pid);
}

}  // namespace

bool Process::running() {
    if (pid_ < 0 || done_) return false;
    int status = 0;
    const pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == 0) return true;
    done_ = true;
    code_ = r == pid_ ? exit_code(status) : 255;
    forget_tool(pid_);
    return false;
}

unsigned long Process::wait() {
    if (pid_ < 0) return 255;
    if (!done_) {
        int status = 0;
        pid_t r;
        do r = waitpid(pid_, &status, 0);
        while (r < 0 && errno == EINTR);
        done_ = true;
        code_ = r == pid_ ? exit_code(status) : 255;
        forget_tool(pid_);
    }
    return code_;
}

void Process::terminate() {
    if (pid_ < 0 || done_) return;
    bool tool = false;
    {
        const std::lock_guard lock(g_mutex);
        tool = g_tools.contains(pid_);
    }
    kill(tool ? -pid_ : pid_, SIGKILL);
}

std::expected<unsigned long, std::string> run(const Command& command) {
    auto p = Process::start(command);
    if (!p) return std::unexpected(p.error());
    return p->wait();
}

void stop_build_tools() {
    const std::lock_guard lock(g_mutex);
    for (const int pid : g_tools) kill(-pid, SIGKILL);
}

}  // namespace qilin::launcher
