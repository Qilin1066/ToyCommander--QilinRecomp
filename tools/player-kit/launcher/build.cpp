// Building the game from the player's disc (build.hpp).
#include "build.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>
#include <thread>
#include <vector>

#include "disc.hpp"
#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"
#include "sha256.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

constexpr const char* kSeeLog = " The details are in build.log (Show build.log opens it).";

std::string hash_text(std::string_view text) {
    return sha256_hex(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Every regular file under `root`, by path relative to it ('/' separators), sorted.
std::vector<std::pair<std::string, fs::path>> files_under(const fs::path& root) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = show(it->path().lexically_relative(root));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        out.emplace_back(std::move(rel), it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uintmax_t size_under(const fs::path& root) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

// work\build.log: what every tool printed, one tool after another.
class Log {
public:
    explicit Log(const fs::path& path) : file_(path, std::ios::binary | std::ios::trunc) {}
    void line(std::string_view text) {
        const std::lock_guard lock(mutex_);
        file_ << text << "\r\n";
        file_.flush();
    }
    // A tool's command and output (its own log file, then removed).
    void tool(const Command& command, const fs::path& output, std::string_view result) {
        const std::string text = "> " + describe_command(command);
        const auto printed = read_file(output);
        std::error_code ec;
        fs::remove(output, ec);
        const std::lock_guard lock(mutex_);
        file_ << text << "\r\n";
        if (printed && !printed->empty()) {
            file_ << *printed;
            if (printed->back() != '\n') file_ << "\r\n";
        }
        if (!result.empty()) file_ << "(" << result << ")\r\n";
        file_.flush();
    }

private:
    std::mutex mutex_;
    std::ofstream file_;
};

std::string unique_suffix() { return std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())); }

// The newest `keep` bytes of cached objects stay; the rest (and any
// half-written leftovers) go.
void trim_cache(const fs::path& cache, std::uintmax_t keep) {
    struct Entry {
        fs::path path;
        fs::file_time_type time;
        std::uintmax_t size;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (fs::directory_iterator it(cache, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        if (p.extension() != ".o") {
            // A temporary file from a build that stopped: a day old is surely abandoned.
            std::error_code age;
            if (fs::file_time_type::clock::now() - it->last_write_time(age) > std::chrono::hours(24)) fs::remove(p, age);
            continue;
        }
        entries.push_back({p, it->last_write_time(ec), it->file_size(ec)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time > b.time; });
    std::uintmax_t total = 0;
    for (const Entry& e : entries) {
        total += e.size;
        if (total > keep) fs::remove(e.path, ec);
    }
}

}  // namespace

std::string show_bytes(std::uintmax_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    else std::snprintf(text, sizeof text, "%.0f MB", static_cast<double>(bytes) / (1ull << 20));
    return text;
}

std::expected<std::string, std::string> build_identity(const Kit& kit) {
    // What the link reads (the library, the flags), what decides the lifted
    // code (qilin.exe), the compiler and headers, game.txt and this launcher.
    std::vector<std::pair<std::string, fs::path>> parts = {
        {"qilin.exe", kit.qilin()},
        {"game.txt", kit.kit / "game.txt"},
        {"build-flags.txt", kit.flags_file()},
        {"compiler", kit.compiler()},
        {"launcher", kit.launcher},
    };
    for (auto& [rel, path] : files_under(kit.kit / "lib")) parts.emplace_back("lib/" + rel, path);
    for (auto& [rel, path] : files_under(kit.kit / "include")) parts.emplace_back("include/" + rel, path);
    std::string lines;
    for (const auto& [name, path] : parts) {
        const auto h = sha256_file(path);
        if (!h) return std::unexpected("This download is incomplete: " + h.error() + ". Unpack it again.");
        lines += name + ":" + *h + "\n";
    }
    return kit.version + " | " + cpu_identity() + " | " + hash_text(lines);
}

std::optional<std::string> built_with(const Kit& kit) {
    const auto text = read_file(kit.work / "built-with.txt");
    if (!text) return std::nullopt;
    return std::string(trim(*text));
}

bool up_to_date(const Kit& kit, const std::string& identity) {
    std::error_code ec;
    return fs::is_regular_file(kit.game_exe(), ec) && built_with(kit) == identity;
}

std::expected<void, std::string> build_game(const Kit& kit, const fs::path& disc, BuildProgress& progress,
                                            const std::atomic<bool>& cancel, const BuildOptions& options) {
    progress.stage = Stage::CheckingDisc;
    progress.done = 0;
    progress.total = 0;
    progress.cached = 0;
    std::error_code ec;
    const fs::path& work = kit.work;
    fs::create_directories(work / "logs", ec);
    if (ec) return std::unexpected("Can't make the folder " + show(work) + ": " + ec.message() + ".");
    Log log(kit.build_log());
    log.line(kit.title + " QilinRecomp " + kit.version);
    log.line("Disc: " + show(disc));
    const auto failed = [&](std::string message) -> std::unexpected<std::string> {
        log.line("Stopped: " + message);
        return std::unexpected(std::move(message));
    };
    const auto stopped = [&] { return failed("The build was stopped."); };
    // The compiler, with what the kit puts before its arguments (zig's "c++").
    const auto compiler_tool = [&](const std::vector<std::string>& args, const fs::path& out) {
        Command c = tool(kit.compiler(), kit.compiler_args, work, out);
        c.args.insert(c.args.end(), args.begin(), args.end());
        return c;
    };

    if (options.check_disc) {
        const DiscCheck check = check_disc(kit, disc);
        if (!check.ok) return failed(check.message);
    }
    const auto identity = build_identity(kit);
    if (!identity) return failed(identity.error());
    log.line("Kit: " + *identity);

    // Room for gen/, obj/ and the game (less what an earlier build left there).
    const fs::space_info space = fs::space(work, ec);
    if (!ec) {
        const std::uintmax_t have = size_under(work / "gen") + size_under(work / "obj");
        const std::uintmax_t need = options.need_bytes > have ? options.need_bytes - have : 0;
        if (space.available < need) {
#if defined(_WIN32)
            std::string where = "drive " + show(work.root_name());
            if (work.root_name().empty()) where = "drive " + show(work.root_path());
            const char* advice = "Free some space there (or move this folder to another drive), then try again.";
#else
            const std::string where = "the disk holding " + show(work);
            const char* advice = "Free some space there, then try again.";
#endif
            return failed("Building the game needs about " + show_bytes(need) + " free on " + where + ", and it has " +
                          show_bytes(space.available) + " free. " + advice);
        }
    }
    if (cancel) return stopped();

    // Lift the boot file. Files whose text is unchanged are left alone.
    progress.stage = Stage::Recompiling;
    {
        const Command lift = tool(kit.qilin(), {"lift", show(disc), "--out", show(work / "gen"), "--files", "64"}, work, work / "logs" / "lift.log");
        const auto code = run(lift);
        log.tool(lift, lift.out, code ? describe_exit(*code) : code.error());
        if (cancel) return stopped();
        if (!code) return failed("The recompiler didn't start: " + code.error() + ".");
        if (*code != 0) return failed("Recompiling the disc's " + kit.boot + " failed (" + describe_exit(*code) + ")." + kSeeLog);
    }

    struct Source {
        std::string name, stem;
        std::uintmax_t size = 0;
        std::string key;
    };
    std::vector<Source> sources;
    for (fs::directory_iterator it(work / "gen", ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || lower(show(it->path().extension())) != ".cpp") continue;
        sources.push_back({show(it->path().filename()), show(it->path().stem()), it->file_size(ec), {}});
    }
    if (sources.empty()) return failed(std::string("The recompiler wrote no C++ files.") + kSeeLog);
    // The largest first, so the longest compiles don't start last.
    std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) {
        return a.size != b.size ? a.size > b.size : a.name < b.name;
    });

    // The kit's flags (build-flags.txt), paths relative to work\.
    const auto flag_text = read_file(kit.flags_file());
    if (!flag_text) return failed("This download has no kit\\build-flags.txt. Unpack it again.");
    const auto vars = parse_settings(*flag_text);
    const auto flags_of = [&](std::string_view name) {
        const auto it = vars.find(name);
        return it == vars.end() ? std::vector<std::string>{} : split_flags(it->second, "../kit");
    };
    std::vector<std::string> cxxflags = flags_of("cxxflags");
    // For this computer's processor: build-flags.txt's `native` where the
    // kit says (-mcpu=native on ARM), else the option's -march=native.
    if (const auto native = vars.find("native"); native != vars.end() && options.native == BuildOptions{}.native) {
        for (std::string& f : split_flags(native->second, "../kit")) cxxflags.push_back(std::move(f));
    } else if (!options.native.empty()) {
        cxxflags.push_back(options.native);
    }
    cxxflags.push_back("-Igen");
    const std::vector<std::string> ldflags = flags_of("ldflags");
    const std::vector<std::string> libs = flags_of("libs");

    // The cache key: each source's text and everything else that decides its
    // object: the lifted headers, the kit's headers, the flags, the compiler
    // and the CPU.
    std::string common = "cpu\n" + cpu_identity() + "\nflags\n";
    for (const std::string& f : cxxflags) common += f + "\n";
    {
        const Command version = compiler_tool({"--version"}, work / "logs" / "compiler-version.txt");
        const auto code = run(version);
        const auto text = read_file(version.out);
        log.tool(version, version.out, code ? describe_exit(*code) : code.error());
        if (!code || *code != 0 || !text)
            return failed("The compiler doesn't start (" + (code ? describe_exit(*code) : code.error()) + ")." + kSeeLog);
        common += "compiler\n" + *text + "\n";
    }
    for (const auto& [rel, path] : files_under(work / "gen")) {
        const std::string ext = lower(show(path.extension()));
        if (ext != ".hpp" && ext != ".h") continue;
        const auto h = sha256_file(path);
        if (!h) return failed("Can't read " + show(work / "gen" / from_utf8(rel)) + ".");
        common += "gen/" + rel + ":" + *h + "\n";
    }
    for (const auto& [rel, path] : files_under(kit.kit / "include")) {
        const auto h = sha256_file(path);
        if (!h) return failed("This download is incomplete: " + h.error() + ". Unpack it again.");
        common += "include/" + rel + ":" + *h + "\n";
    }
    const std::string common_key = hash_text(common);

    progress.total = static_cast<int>(sources.size());
    progress.stage = Stage::Compiling;
    const fs::path cache = kit.cache();
    fs::create_directories(work / "obj", ec);
    fs::create_directories(cache, ec);
    std::vector<const Source*> misses;
    for (Source& s : sources) {
        const auto text = read_file(work / "gen" / from_utf8(s.name));
        if (!text) return failed("Can't read " + show(work / "gen" / from_utf8(s.name)) + ".");
        s.key = hash_text(common_key + "\n" + *text);
        const fs::path cached = cache / from_utf8(s.key + ".o");
        const fs::path object = work / "obj" / from_utf8(s.stem + ".o");
        std::error_code copy;
        if (options.use_cache && fs::is_regular_file(cached, copy) && fs::copy_file(cached, object, fs::copy_options::overwrite_existing, copy) && !copy) {
            fs::last_write_time(cached, fs::file_time_type::clock::now(), copy);  // the newest stay when the cache is trimmed
            log.line("From the cache: " + s.name);
            ++progress.cached;
            ++progress.done;
        } else {
            misses.push_back(&s);
        }
    }

    // Compile the rest: one worker per core, each taking the next largest.
    if (!misses.empty()) {
        unsigned jobs = options.jobs ? options.jobs : std::max(1u, std::thread::hardware_concurrency());
        // No more at once than memory allows: about 1.2 GB a compiler at
        // its peak (a Raspberry Pi with 2 GB compiles one file at a time).
        if (!options.jobs) {
            if (const std::uint64_t room = available_memory(); room > 0) {
                const auto fit = static_cast<unsigned>(std::max<std::uint64_t>(1, room / (1200ull << 20)));
                if (fit < jobs) {
                    log.line("Compiling " + std::to_string(fit) + " at a time, not " + std::to_string(jobs) + ": " + show_bytes(room) +
                             " of memory free.");
                    jobs = fit;
                }
            }
        }
        jobs = static_cast<unsigned>(std::min<std::size_t>(jobs, misses.size()));
        std::atomic<std::size_t> next{0};
        std::atomic<bool> broken{false};
        std::mutex error_mutex;
        std::string error;
        const auto worker = [&] {
            for (;;) {
                if (broken || cancel) return;
                const std::size_t i = next++;
                if (i >= misses.size()) return;
                const Source& s = *misses[i];
                const fs::path object = work / "obj" / from_utf8(s.stem + ".o");
                std::error_code rm;
                fs::remove(object, rm);
                Command cc = compiler_tool(cxxflags, work / "logs" / from_utf8(s.stem + ".log"));
                cc.low_priority = true;
                cc.args.insert(cc.args.end(), {"-c", "gen/" + s.name, "-o", "obj/" + s.stem + ".o"});
                const auto code = run(cc);
                log.tool(cc, cc.out, code ? describe_exit(*code) : code.error());
                if (!code || *code != 0) {
                    if (cancel) return;
                    const std::lock_guard lock(error_mutex);
                    if (!broken.exchange(true))
                        error = "Compiling " + s.name + " failed (" + (code ? describe_exit(*code) : code.error()) + ")." + kSeeLog;
                    return;
                }
                // Into the cache: written beside, then renamed, so a half-written object is never taken.
                std::error_code keep;
                const fs::path cached = cache / from_utf8(s.key + ".o");
                fs::path temporary = cached;
                temporary += from_utf8("." + unique_suffix() + ".tmp");
                if (fs::copy_file(object, temporary, fs::copy_options::overwrite_existing, keep) && !keep) {
                    fs::rename(temporary, cached, keep);
                    if (keep) fs::remove(temporary, keep);
                }
                ++progress.done;
            }
        };
        std::vector<std::jthread> pool;
        for (unsigned j = 0; j < jobs; ++j) pool.emplace_back(worker);
        pool.clear();  // joins
        if (cancel) return stopped();
        if (broken) return failed(error);
    }
    if (cancel) return stopped();

    // Link.
    progress.stage = Stage::Linking;
    const fs::path linked = work / "obj" / from_utf8(kit.exe);  // moved to work/<exe> below
    fs::remove(linked, ec);
    {
        std::vector<std::string> objects;
        for (const Source& s : sources) objects.push_back("obj/" + s.stem + ".o");
        std::sort(objects.begin(), objects.end());
        Command link = compiler_tool(ldflags, work / "logs" / "link.log");
        link.args.push_back("-o");
        link.args.push_back("obj/" + kit.exe);
        link.args.insert(link.args.end(), objects.begin(), objects.end());
        link.args.insert(link.args.end(), libs.begin(), libs.end());
        const auto code = run(link);
        log.tool(link, link.out, code ? describe_exit(*code) : code.error());
        if (cancel) return stopped();
        if (!code || *code != 0 || !fs::is_regular_file(linked, ec))
            return failed("Linking the game failed (" + (code ? describe_exit(*code) : code.error()) + ")." + kSeeLog);
    }

    // Stage beside the old one and swap: a failure leaves the old game working.
    const fs::path game = kit.game_exe();
    fs::path staged = game;
    staged += ".new";
    fs::path previous = game;
    previous += ".previous";
    fs::remove(staged, ec);
    fs::rename(linked, staged, ec);
    if (ec) return failed("Can't move the game into " + show(work) + ": " + ec.message() + ".");
    if (const auto replaced = swap_in(game, staged, previous); !replaced) {
        fs::remove(staged, ec);
        return failed("Can't replace " + kit.exe + " (" + replaced.error() + "). If the game is still running, close it and try again.");
    }
    if (const auto stamp = write_file(work / "built-with.txt", *identity); !stamp) return failed(stamp.error());
    trim_cache(cache, options.cache_bytes);
    log.line("Built " + kit.exe + ": " + std::to_string(progress.cached.load()) + " of " + std::to_string(sources.size()) +
             " files from the cache.");
    progress.stage = Stage::Done;
    return {};
}

}  // namespace qilin::launcher
