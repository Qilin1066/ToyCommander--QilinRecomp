// What the launcher needs from Linux and macOS (platform.hpp, and kit.hpp's
// own_path, show, claim_folder and cpu_identity). HTTPS goes through the
// system's curl (or wget): every desktop Linux and every Mac has one, so no
// TLS library is bundled.
#include "platform.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <thread>

#include "kit.hpp"
#include "kit_text.hpp"
#include "process.hpp"
#include "hash.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

fs::path from_utf8(std::string_view utf8) { return fs::path(std::string(utf8)); }

std::string show(const fs::path& path) { return path.string(); }

fs::path long_path(const fs::path& path) { return path; }  // no such limit here

std::string environment(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

fs::path user_data_base() {
    const std::string home = environment("HOME");
#if defined(__APPLE__)
    return home.empty() ? fs::path() : fs::path(home) / "Library" / "Application Support";
#else
    if (const std::string xdg = environment("XDG_DATA_HOME"); !xdg.empty()) return xdg;
    return home.empty() ? fs::path() : fs::path(home) / ".local" / "share";
#endif
}

fs::path own_path() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::strlen(buffer.c_str()));
    std::error_code ec;
    const fs::path real = fs::canonical(buffer, ec);
    return ec ? fs::path(buffer) : real;
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#endif
}

std::expected<void, std::string> rename_over(const fs::path& from, const fs::path& to) {
    if (std::rename(from.c_str(), to.c_str()) == 0) return {};
    return std::unexpected(std::string(std::strerror(errno)));
}

std::expected<void, std::string> swap_in(const fs::path& target, const fs::path& staged, const fs::path& previous) {
    std::error_code ec;
    const bool had = fs::exists(target, ec);
    if (had) {
        fs::remove(previous, ec);
        if (std::rename(target.c_str(), previous.c_str()) != 0) return std::unexpected(std::string(std::strerror(errno)));
    }
    if (std::rename(staged.c_str(), target.c_str()) != 0) {
        const std::string why = std::strerror(errno);
        if (had) (void)std::rename(previous.c_str(), target.c_str());
        return std::unexpected(why);
    }
    return {};
}

std::string release_asset_suffix() {
#if defined(__APPLE__)
    return "macos.dmg";  // one for Apple silicon and Intel Macs
#elif defined(__aarch64__)
    return "linux-arm64.tar.gz";
#else
    return "linux-x86_64.tar.gz";
#endif
}

std::string_view executable_suffix() { return ""; }

std::string host_architecture() {
#if defined(__APPLE__)
    // The Mac's, not this process's (which may be Intel code under Rosetta).
    int arm64 = 0;
    size_t size = sizeof arm64;
    if (sysctlbyname("hw.optional.arm64", &arm64, &size, nullptr, 0) == 0 && arm64) return "arm64";
    return "x86_64";
#else
    struct utsname u{};
    const std::string machine = uname(&u) == 0 ? std::string(u.machine) : std::string();
    return machine == "aarch64" || machine == "arm64" ? "arm64" : machine;
#endif
}

bool claim_folder(const Kit& kit) {
    std::error_code ec;
    const std::string key = fs::absolute(kit.top, ec).string();
    const std::string name = "launcher-" + sha256_hex(std::span(reinterpret_cast<const uint8_t*>(key.data()), key.size())).substr(0, 16) + ".lock";
    // Kept open (and locked) until the launcher ends.
    const int fd = open((kit.user / name).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
}

std::string cpu_identity() {
    // The model and the instruction set extensions: -march=native follows them.
    struct utsname u{};
    std::string id = uname(&u) == 0 ? std::string(u.machine) : std::string("unknown");
#if defined(__APPLE__)
    char brand[256] = {};
    size_t size = sizeof brand - 1;
    if (sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0) id += std::string(" ") + brand;
#else
    std::ifstream info("/proc/cpuinfo");
    std::string features;
    for (std::string line; std::getline(info, line);) {
        const auto value = [&] { return std::string(trim(std::string_view(line).substr(line.find(':') + 1))); };
        if (line.find(':') == std::string::npos) {
            if (!features.empty()) break;  // the first processor's block is enough
            continue;
        }
        if (line.starts_with("model name") || line.starts_with("CPU part") || line.starts_with("CPU implementer")) id += " " + value();
        if (line.starts_with("flags") || line.starts_with("Features")) features = value();
    }
    if (!features.empty())
        id += " " + sha256_hex(std::span(reinterpret_cast<const uint8_t*>(features.data()), features.size())).substr(0, 16);
#endif
    return id;
}

namespace {

// A program on PATH (or in the usual places), if any.
fs::path find_program(std::string_view name) {
    std::string path = environment("PATH");
    path += ":/usr/bin:/bin:/usr/local/bin";
    for (std::string_view rest = path; !rest.empty();) {
        const size_t end = std::min(rest.find(':'), rest.size());
        const std::string_view dir = rest.substr(0, end);
        rest.remove_prefix(std::min(end + 1, rest.size()));
        if (dir.empty()) continue;
        const fs::path p = fs::path(std::string(dir)) / std::string(name);
        if (access(p.c_str(), X_OK) == 0) return p;
    }
    return {};
}

fs::path scratch_file(std::string_view what) {
    static std::atomic<unsigned> n{0};
    return fs::temp_directory_path() / ("qilin-launcher-" + std::to_string(getpid()) + "-" + std::to_string(n++) + "-" + std::string(what));
}

// curl (or wget) fetching `url` to `to`; `received` follows the file's size.
std::expected<void, std::string> transfer(const std::string& url, const std::vector<std::string>& headers, const fs::path& to,
                                          std::atomic<std::uint64_t>* received, const std::atomic<bool>* cancel) {
    if (!url.starts_with("https://")) return std::unexpected("not an https address: " + url);
    const fs::path errors = scratch_file("errors.txt");
    Command c;
    c.build_tool = false;
    c.out = errors;
    if (const fs::path curl = find_program("curl"); !curl.empty()) {
        c.exe = curl;
        c.args = {"--fail", "--silent", "--show-error", "--location", "--proto", "=https", "--proto-redir", "=https",
                  "--connect-timeout", "15", "--retry", "2", "--user-agent", "QilinRecomp-launcher", "--output", to.string()};
        for (const std::string& h : headers) c.args.insert(c.args.end(), {"--header", h});
    } else if (const fs::path wget = find_program("wget"); !wget.empty()) {
        c.exe = wget;
        c.args = {"--quiet", "--https-only", "--timeout=30", "--tries=3", "--user-agent=QilinRecomp-launcher", "--output-document=" + to.string()};
        for (const std::string& h : headers) c.args.push_back("--header=" + h);
    } else {
        return std::unexpected(std::string("this system has neither curl nor wget to download with (install curl)"));
    }
    c.args.push_back(url);
    auto p = Process::start(c);
    if (!p) return std::unexpected(p.error());
    while (p->running()) {
        if (cancel && *cancel) {
            p->terminate();
            p->wait();
            std::error_code ec;
            fs::remove(errors, ec);
            return std::unexpected(std::string("stopped"));
        }
        if (received) {
            std::error_code ec;
            const auto size = fs::file_size(to, ec);
            if (!ec) *received = size;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const unsigned long code = p->wait();
    std::string said = std::string(trim(read_file(errors).value_or("")));
    std::error_code ec;
    fs::remove(errors, ec);
    if (code == 0) {
        if (received) *received = fs::file_size(to, ec);
        return {};
    }
    if (said.find("403") != std::string::npos || said.find("429") != std::string::npos)
        return std::unexpected(std::string("GitHub is turning away too many requests just now: try again in an hour"));
    if (said.find("resolve") != std::string::npos || said.find("connect") != std::string::npos)
        return std::unexpected(std::string("can't reach the server (is this computer online?)"));
    return std::unexpected(said.empty() ? describe_exit(code) : said);
}

}  // namespace

std::expected<std::string, std::string> fetch_text(const std::string& url, const std::vector<std::string>& headers) {
    const fs::path file = scratch_file("answer.txt");
    auto got = transfer(url, headers, file, nullptr, nullptr);
    auto text = got ? read_file(file) : std::expected<std::string, std::string>(std::unexpected(got.error()));
    std::error_code ec;
    fs::remove(file, ec);
    return text;
}

std::expected<void, std::string> download(const std::string& url, const fs::path& to, std::atomic<std::uint64_t>& received,
                                          const std::atomic<bool>& cancel) {
    return transfer(url, {}, to, &received, &cancel);
}

std::expected<void, std::string> unpack(const fs::path& archive, const fs::path& into, const fs::path& log) {
    const std::string name = archive.filename().string();
    std::error_code ec;
    fs::create_directories(into, ec);
    if (name.ends_with(".dmg")) {
#if defined(__APPLE__)
        // Mounted read-only and out of sight, its contents copied, unmounted.
        const fs::path mount = into.parent_path() / "mounted";
        fs::create_directories(mount, ec);
        auto attached = run(tool("/usr/bin/hdiutil", {"attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount.string(), archive.string()},
                                 into, log));
        if (!attached || *attached != 0) return std::unexpected("the disk image can't be opened (" + (attached ? describe_exit(*attached) : attached.error()) + ")");
        std::expected<void, std::string> result;
        for (fs::directory_iterator it(mount, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".app") continue;
            Command copy = tool("/usr/bin/ditto", {it->path().string(), (into / it->path().filename()).string()}, into, log);
            copy.append = true;
            const auto copied = run(copy);
            if (!copied || *copied != 0) result = std::unexpected(std::string("copying from the disk image failed"));
        }
        Command detach = tool("/usr/bin/hdiutil", {"detach", mount.string(), "-force"}, into, log);
        detach.append = true;
        (void)run(detach);
        fs::remove(mount, ec);
        return result;
#else
        return std::unexpected(std::string("a .dmg is a macOS download"));
#endif
    }
    const fs::path tar = find_program("tar");
    if (tar.empty()) return std::unexpected(std::string("this system has no tar to unpack it"));
    const auto code = run(tool(tar, {"-xzf", archive.string(), "-C", into.string()}, into, log));
    if (!code) return std::unexpected(code.error());
    if (*code != 0) return std::unexpected(describe_exit(*code));
    return {};
}

std::uint64_t available_memory() {
#if defined(__APPLE__)
    // macOS keeps free memory low on purpose (its cache gives it back): half
    // of what the Mac has is what a build can count on.
    uint64_t total = 0;
    size_t size = sizeof total;
    return sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0 ? total / 2 : 0;
#else
    std::ifstream info("/proc/meminfo");
    for (std::string line; std::getline(info, line);)
        if (line.starts_with("MemAvailable:")) return std::strtoull(line.c_str() + 13, nullptr, 10) * 1024;  // kB
    return 0;
#endif
}

void open_with_system(const fs::path& path) {
    Command c;
    c.build_tool = false;
#if defined(__APPLE__)
    c.exe = "/usr/bin/open";
#else
    c.exe = find_program("xdg-open");
    if (c.exe.empty()) return;
#endif
    c.args = {path.string()};
    (void)Process::start(c);  // left to run: it hands the file over and ends
}

}  // namespace qilin::launcher
