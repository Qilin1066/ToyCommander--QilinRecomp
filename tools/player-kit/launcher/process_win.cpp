// Running programs without console windows, on Windows (process.hpp).
#include "process.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <memory>
#include <type_traits>

namespace qilin::launcher {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string system_message(unsigned long code) {
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                   code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out = n && text ? narrow(std::wstring_view(text, n)) : "Windows error " + std::to_string(code);
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

std::wstring quote_argument(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(arg);
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {
            out.append(slashes * 2, L'\\');  // doubled before the closing quote
            break;
        }
        if (arg[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(slashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}

std::string describe_command(const Command& command) {
    std::string text = narrow(quote_argument(command.exe.filename().wstring()));
    for (const std::string& a : command.args) text += " " + narrow(quote_argument(widen(a)));
    return text;
}

std::string describe_exit(unsigned long code) {
    if (code <= 0xFFFF) return "exit code " + std::to_string(code);
    char text[32];
    std::snprintf(text, sizeof text, "exit code 0x%08lX", code);
    return text;
}

namespace {

// The build's job: every build tool is in it, so they all end with the
// launcher (it's closed with the launcher's last handle) or on request.
HANDLE build_job() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        }
        return j;
    }();
    return job;
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) CloseHandle(h);
    }
};

HANDLE open_output(const std::filesystem::path& path, bool append, SECURITY_ATTRIBUTES* sa) {
    if (path.empty())
        return CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, sa,
                       append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

std::expected<Process, std::string> Process::start(const Command& command) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    const Handle in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    const Handle out(open_output(command.out, command.append, &sa));
    if (!out.h) return std::unexpected("can't write " + narrow(command.out.wstring()) + ": " + system_message(GetLastError()));
    Handle err_own;
    if (!command.err.empty()) {
        err_own.h = open_output(command.err, command.append, &sa);
        if (err_own.h == INVALID_HANDLE_VALUE) err_own.h = nullptr;
        if (!err_own.h) return std::unexpected("can't write " + narrow(command.err.wstring()) + ": " + system_message(GetLastError()));
    }
    const HANDLE err = err_own.h ? err_own.h : out.h;
    if (!in.h) return std::unexpected("can't open NUL: " + system_message(GetLastError()));

    // Only these three handles are inherited, so tools started at the same
    // time from other threads never hold each other's files open.
    HANDLE inherit[3] = {in.h, out.h, err};
    const DWORD inherit_count = err == out.h ? 2 : 3;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    auto storage = std::make_unique<unsigned char[]>(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.get());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
        return std::unexpected("can't start a program: " + system_message(GetLastError()));
    const auto release = [](LPPROC_THREAD_ATTRIBUTE_LIST list) { DeleteProcThreadAttributeList(list); };
    const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(release)> list_guard(attributes, release);
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, inherit_count * sizeof(HANDLE), nullptr,
                                   nullptr))
        return std::unexpected("can't start a program: " + system_message(GetLastError()));

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in.h;
    si.StartupInfo.hStdOutput = out.h;
    si.StartupInfo.hStdError = err;
    si.lpAttributeList = attributes;

    std::wstring line = quote_argument(command.exe.wstring());
    for (const std::string& a : command.args) {
        line += L' ';
        line += quote_argument(widen(a));
    }
    DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (command.build_tool) flags |= CREATE_SUSPENDED;
    if (command.low_priority) flags |= BELOW_NORMAL_PRIORITY_CLASS;
    // The launcher's environment, with the command's own variables set.
    std::wstring block;
    if (!command.environment.empty()) {
        const wchar_t* own = GetEnvironmentStringsW();
        for (const wchar_t* p = own; p && *p; p += std::wcslen(p) + 1) {
            const std::wstring_view entry(p);
            bool replaced = false;
            for (const auto& [name, value] : command.environment) {
                const std::wstring key = widen(name) + L"=";
                if (entry.size() >= key.size() && _wcsnicmp(entry.data(), key.c_str(), key.size()) == 0) replaced = true;
            }
            if (!replaced) block.append(entry).push_back(L'\0');
        }
        if (own) FreeEnvironmentStringsW(const_cast<wchar_t*>(own));
        for (const auto& [name, value] : command.environment) block.append(widen(name + "=" + value)).push_back(L'\0');
        block.push_back(L'\0');
    }
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = command.cwd.wstring();
    if (!CreateProcessW(command.exe.c_str(), line.data(), nullptr, nullptr, TRUE, flags, block.empty() ? nullptr : block.data(),
                        cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi))
        return std::unexpected("can't start " + narrow(command.exe.filename().wstring()) + ": " + system_message(GetLastError()));
    if (command.build_tool) {
        if (const HANDLE job = build_job()) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    return Process(pi.hProcess);
}

Process::Process(Process&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        if (handle_) CloseHandle(handle_);
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

Process::~Process() {
    if (handle_) CloseHandle(handle_);
}

bool Process::running() { return handle_ && WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT; }

unsigned long Process::wait() {
    if (!handle_) return 0xFFFFFFFFul;
    WaitForSingleObject(handle_, INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(handle_, &code)) code = 0xFFFFFFFFul;
    return code;
}

void Process::terminate() {
    if (handle_) TerminateProcess(handle_, 1);
}

std::expected<unsigned long, std::string> run(const Command& command) {
    auto p = Process::start(command);
    if (!p) return std::unexpected(p.error());
    return p->wait();
}

void stop_build_tools() {
    if (const HANDLE job = build_job()) TerminateJobObject(job, 1);
}

}  // namespace qilin::launcher
