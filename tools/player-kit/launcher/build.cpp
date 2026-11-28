// Building the game from the player's data (build.hpp).
#include "build.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <system_error>
#include <thread>
#include <vector>

#include "hash.hpp"
#include "input.hpp"
#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"

#ifndef QILIN_LAUNCHER_VERSION
#define QILIN_LAUNCHER_VERSION "unknown"
#endif

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

constexpr const char* kSeeLog = " The details are in build.log (Show build.log opens it).";

// A game.txt command as a Command: the program first (a relative one is in
// the kit), run in work/.
std::expected<Command, std::string> kit_command(const Kit& kit, std::string_view text, const fs::path& input, const fs::path& out) {
    const auto words = expand_command(kit, text, input);
    if (!words) return std::unexpected(words.error());
    fs::path exe = from_utf8(words->front());
    if (exe.is_relative()) exe = kit.kit / exe;
    return tool(exe, {words->begin() + 1, words->end()}, kit.work, out);
}

std::string hash_text(std::string_view text) {
    return sha256_hex(text);
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

std::string unique_suffix() { return std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())); }

}  // namespace

void BuildLog::line(std::string_view text) {
    const std::lock_guard lock(mutex_);
    file_ << text << "\r\n";
    file_.flush();
}

void BuildLog::tool(const Command& command, const fs::path& output, std::string_view result) {
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

// The newest `keep` bytes of cached objects stay; the rest (and any
// half-written leftovers) go.
void trim_cache(const fs::path& cache_folder, std::uintmax_t keep) {
    const fs::path cache = long_path(cache_folder);
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
    // The kit's other tools the steps run (game.txt's {tool:NAME}, or a
    // program named by its path in the kit): a new one means a new build.
    for (std::size_t i = 0; i < kit.steps.size(); ++i) {
        const std::string& text = kit.steps[i];
        std::vector<fs::path> programs;
        for (std::size_t at = text.find("{tool:"); at != std::string::npos; at = text.find("{tool:", at + 1))
            if (const std::size_t close = text.find('}', at); close != std::string::npos) programs.push_back(kit.kit_tool(text.substr(at + 6, close - at - 6)));
        if (const auto words = expand_command(kit, text, "input")) {
            fs::path exe = from_utf8(words->front());
            if (exe.is_relative()) exe = kit.kit / exe;
            const std::string rel = show(exe.lexically_relative(kit.kit));
            std::error_code ec;
            if (!rel.empty() && !rel.starts_with("..") && fs::is_regular_file(exe, ec)) programs.push_back(exe);
        }
        for (const fs::path& p : programs)
            if (p != kit.qilin() && std::none_of(parts.begin(), parts.end(), [&](const auto& part) { return part.second == p; }))
                parts.emplace_back("step." + std::to_string(i + 1) + ":" + show(p.filename()), p);
    }

    // The files' hashes, remembered by path, size and time: hashing the
    // compiler and the library (a few hundred MB) at every start takes
    // seconds on a Raspberry Pi. A file whose size or time changed is read again.
    const fs::path remembered = kit.user / "file-hashes.txt";
    std::map<std::string, std::string, std::less<>> known;  // "path|size|time" -> hash
    if (const auto text = read_file(remembered)) {
        for (std::string_view rest = *text; !rest.empty();) {
            const size_t end = std::min(rest.find('\n'), rest.size());
            const std::string_view line = rest.substr(0, end);
            rest.remove_prefix(std::min(end + 1, rest.size()));
            if (const size_t tab = line.find('\t'); tab != std::string_view::npos) known.emplace(line.substr(tab + 1), line.substr(0, tab));
        }
    }
    std::string fresh;
    bool changed = false;
    std::string lines;
    for (const auto& [name, path] : parts) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        const auto time = ec ? fs::file_time_type{} : fs::last_write_time(path, ec);
        const std::string key = show(fs::absolute(path, ec)) + "|" + std::to_string(size) + "|" + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
        std::string hash;
        if (const auto it = known.find(key); it != known.end()) {
            hash = it->second;
        } else {
            const auto h = sha256_file(path);
            if (!h) return std::unexpected("This download is incomplete: " + h.error() + ". Unpack it again.");
            hash = *h;
            changed = true;
        }
        fresh += hash + "\t" + key + "\n";
        lines += name + ":" + hash + "\n";
    }
    if (changed) (void)write_file(remembered, fresh);  // only what's here now: old folders' entries go
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

std::expected<std::vector<std::string>, std::string> compile_lifted(const CompileJob& job, BuildLog& log, BuildProgress& progress,
                                                                   const std::atomic<bool>& cancel) {
    std::error_code ec;
    const fs::path& work = job.cwd;
    const fs::path gen = work / from_utf8(job.gen);
    const fs::path obj = work / from_utf8(job.obj);
    const auto compiler_tool = [&](const std::vector<std::string>& args, const fs::path& out) {
        Command c = tool(job.compiler, job.compiler_args, work, out);
        c.args.insert(c.args.end(), args.begin(), args.end());
        c.environment = job.environment;
        return c;
    };
    fs::create_directories(job.logs, ec);

    // Every .cpp under gen, subfolders too (gen/dsp/x.cpp): by its path
    // relative to gen ('/' separators), its object at the same path under obj,
    // so two files of one name in different folders can't collide.
    struct Source {
        std::string name, stem;  // "dsp/x.cpp", "dsp/x"
        std::uintmax_t size = 0;
        std::string key;
        std::size_t index = 0;   // its compiler's own log: logs/compile-<index>.log
    };
    std::vector<Source> sources;
    for (const auto& [rel, path] : files_under(gen)) {
        if (lower(show(path.extension())) != ".cpp") continue;
        std::error_code size_ec;
        sources.push_back({rel, rel.substr(0, rel.size() - 4), fs::file_size(path, size_ec), {}, sources.size()});
    }
    if (sources.empty()) return std::unexpected("The recompiler wrote no C++ files." + job.see_log);
    // The largest first, so the longest compiles don't start last.
    std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) {
        return a.size != b.size ? a.size > b.size : a.name < b.name;
    });

    // The cache key: each source's text and everything else that decides its
    // object: the target (this computer's CPU, or another system's), the
    // flags, the compiler, the lifted headers and the kit's headers.
    std::string common = job.target + "flags\n";
    for (const std::string& f : job.cxxflags) common += f + "\n";
    {
        const Command version = compiler_tool({"--version"}, job.logs / "compiler-version.txt");
        const auto code = run(version);
        const auto text = read_file(version.out);
        log.tool(version, version.out, code ? describe_exit(*code) : code.error());
        if (!code || *code != 0 || !text)
            return std::unexpected("The compiler doesn't start (" + (code ? describe_exit(*code) : code.error()) + ")." + job.see_log);
        common += "compiler\n" + *text + "\n";
    }
    for (const auto& [rel, path] : files_under(gen)) {
        const std::string ext = lower(show(path.extension()));
        if (ext != ".hpp" && ext != ".h") continue;
        const auto h = sha256_file(path);
        if (!h) return std::unexpected("Can't read " + show(gen / from_utf8(rel)) + ".");
        common += "gen/" + rel + ":" + *h + "\n";
    }
    for (const auto& [rel, path] : files_under(job.include)) {
        const auto h = sha256_file(path);
        if (!h) return std::unexpected("This download is incomplete: " + h.error() + ". Unpack it again.");
        common += "include/" + rel + ":" + *h + "\n";
    }
    const std::string common_key = hash_text(common);

    progress.total = static_cast<int>(sources.size());
    progress.stage = Stage::Compiling;
    // The cache is in the data folder, whose path the player doesn't choose:
    // past Windows' 260-character limit it still works (platform.hpp).
    const fs::path cache = long_path(job.cache);
    fs::create_directories(obj, ec);
    fs::create_directories(cache, ec);
    std::vector<const Source*> misses;
    for (Source& s : sources) {
        const auto text = read_file(gen / from_utf8(s.name));
        if (!text) return std::unexpected("Can't read " + show(gen / from_utf8(s.name)) + ".");
        s.key = hash_text(common_key + "\n" + *text);
        const fs::path cached = cache / from_utf8(s.key + ".o");
        const fs::path object = obj / from_utf8(s.stem + ".o");
        std::error_code copy;
        fs::create_directories(object.parent_path(), copy);
        if (job.use_cache && fs::is_regular_file(cached, copy) && fs::copy_file(cached, object, fs::copy_options::overwrite_existing, copy) && !copy) {
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
        unsigned jobs = job.jobs ? job.jobs : std::max(1u, std::thread::hardware_concurrency());
        // No more at once than memory allows: game.txt's compile_memory_mb a
        // compiler at its peak (with 1.2 GB, a Raspberry Pi with 2 GB
        // compiles one file at a time).
        if (!job.jobs) {
            if (const std::uint64_t room = available_memory(); room > 0) {
                const std::uint64_t each = std::max<std::uint64_t>(1, job.memory_mb) << 20;
                const auto fit = static_cast<unsigned>(std::max<std::uint64_t>(1, room / each));
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
                const fs::path object = obj / from_utf8(s.stem + ".o");
                std::error_code rm;
                fs::remove(object, rm);
                Command cc = compiler_tool(job.cxxflags, job.logs / ("compile-" + std::to_string(s.index) + ".log"));
                cc.low_priority = true;
                cc.args.insert(cc.args.end(), {"-c", job.gen + "/" + s.name, "-o", job.obj + "/" + s.stem + ".o"});
                const auto code = run(cc);
                log.tool(cc, cc.out, code ? describe_exit(*code) : code.error());
                if (!code || *code != 0) {
                    if (cancel) return;
                    const std::lock_guard lock(error_mutex);
                    if (!broken.exchange(true))
                        error = "Compiling " + s.name + " failed (" + (code ? describe_exit(*code) : code.error()) + ")." + job.see_log;
                    return;
                }
                // Into the cache: written beside, then renamed, so a half-written object is never taken.
                // A failure costs only a recompile next time, but is logged: a cache that never fills
                // otherwise just looks like slow builds.
                std::error_code keep;
                const fs::path cached = cache / from_utf8(s.key + ".o");
                fs::path temporary = cached;
                temporary += from_utf8("." + unique_suffix() + ".tmp");
                if (fs::copy_file(object, temporary, fs::copy_options::overwrite_existing, keep) && !keep) {
                    fs::rename(temporary, cached, keep);
                    if (keep) {
                        log.line("Not cached (" + keep.message() + "): " + s.name);
                        std::error_code rm2;
                        fs::remove(temporary, rm2);
                    }
                } else {
                    log.line("Not cached (" + keep.message() + "): " + s.name);
                }
                ++progress.done;
            }
        };
        // std::thread, joined here: the Intel Macs' Apple Clang has no std::jthread.
        std::vector<std::thread> pool;
        for (unsigned j = 0; j < jobs; ++j) pool.emplace_back(worker);
        for (std::thread& t : pool) t.join();
        if (cancel) return std::unexpected(std::string("The build was stopped."));
        if (broken) return std::unexpected(error);
    }
    if (cancel) return std::unexpected(std::string("The build was stopped."));

    std::vector<std::string> objects;
    for (const Source& s : sources) objects.push_back(job.obj + "/" + s.stem + ".o");
    std::sort(objects.begin(), objects.end());
    return objects;
}

std::expected<Command, std::string> game_command(const Kit& kit, const fs::path& input) {
    auto words = expand_command(kit, "game " + kit.run_args, input);
    if (!words) return std::unexpected("kit/game.txt's run_args can't be used (" + words.error() + "). Unpack the download again.");
    Command c;
    c.exe = kit.game_exe();
    c.args.assign(words->begin() + 1, words->end());
    c.cwd = kit.top;
    c.out = kit.run_log();
    c.err = kit.run_errors();
    c.build_tool = false;
    return c;
}

std::expected<void, std::string> build_game(const Kit& kit, const fs::path& input, BuildProgress& progress,
                                            const std::atomic<bool>& cancel, const BuildOptions& options) {
    progress.stage = Stage::Checking;
    progress.done = 0;
    progress.total = 0;
    progress.cached = 0;
    std::error_code ec;
    const fs::path& work = kit.work;
    fs::create_directories(work / "logs", ec);
    if (ec) return std::unexpected("Can't make the folder " + show(work) + ": " + ec.message() + ".");
    BuildLog log(kit.build_log());
    log.line(kit.title + " QilinRecomp " + kit.version + " (launcher " QILIN_LAUNCHER_VERSION ")");
    log.line(capitalised(kit.input_noun) + ": " + show(input));
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

    if (options.check_input) {
        const InputCheck check = check_input(kit, input);
        if (!check.ok) return failed(check.message);
        log.line("Identified: " + check.message);
    }
    const auto identity = build_identity(kit);
    if (!identity) return failed(identity.error());
    log.line("Kit: " + *identity);

    // Room for gen/, obj/ and the game (less what an earlier build left there).
    const fs::space_info space = fs::space(work, ec);
    if (!ec) {
        const std::uintmax_t have = size_under(work / "gen") + size_under(work / "obj");
        const std::uintmax_t want = options.need_bytes.value_or(kit.build_space_mb << 20);
        const std::uintmax_t need = want > have ? want - have : 0;
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

    // The recompiler's steps (game.txt's step.1 to step.9), in order, in
    // work/. (qilin lift leaves files whose text is unchanged alone, so their
    // objects come from the cache.)
    progress.stage = Stage::Recompiling;
    progress.total = static_cast<int>(kit.steps.size());
    for (std::size_t i = 0; i < kit.steps.size(); ++i) {
        const auto command = kit_command(kit, kit.steps[i], input, work / "logs" / ("step-" + std::to_string(i + 1) + ".log"));
        if (!command) return failed("kit/game.txt's step." + std::to_string(i + 1) + " can't be used (" + command.error() + "). Unpack the download again.");
        const auto code = run(*command);
        log.tool(*command, command->out, code ? describe_exit(*code) : code.error());
        if (cancel) return stopped();
        const std::string which = kit.steps.size() == 1 ? std::string()
                                                         : " at step " + std::to_string(i + 1) + " of " + std::to_string(kit.steps.size()) + " (" +
                                                               show(command->exe.filename()) + ")";
        if (!code) return failed("The recompiler didn't start" + which + ": " + code.error() + ".");
        if (*code != 0) {
            const std::string what = kit.steps.size() == 1 && !kit.boot.empty() ? "Recompiling " + kit.the_input() + "'s " + kit.boot
                                                                                : "Recompiling " + kit.the_input();
            return failed(what + " failed" + which + " (" + describe_exit(*code) + ")." + kSeeLog);
        }
        progress.done = static_cast<int>(i + 1);
    }
    progress.done = 0;
    progress.total = 0;

    // The kit's flags (build-flags.txt), paths relative to work\ where the
    // kit is beside it (macOS: work/ is in the data folder, the kit in the
    // app, so its whole path; each flag is one argument, spaces and all).
    const auto flag_text = read_file(kit.flags_file());
    if (!flag_text) return failed("This download has no kit\\build-flags.txt. Unpack it again.");
    const auto vars = parse_settings(*flag_text);
    std::error_code same_ec;
    const std::string kit_dir = fs::equivalent(kit.kit, work.parent_path() / "kit", same_ec) ? "../kit" : show(kit.kit);
    // A universal kit (macOS's) adds each processor's own after the shared
    // ones: `ldflags.arm64` after `ldflags` (host_architecture()).
    const std::string arch = host_architecture();
    const auto flags_of = [&](std::string_view name) {
        std::vector<std::string> out;
        for (const std::string& key : {std::string(name), std::string(name) + "." + arch})
            if (const auto it = vars.find(key); it != vars.end())
                for (std::string& f : split_flags(it->second, kit_dir)) out.push_back(std::move(f));
        return out;
    };
    std::vector<std::string> cxxflags = flags_of("cxxflags");
    // For this computer's processor: build-flags.txt's `native` where the
    // kit says (-mcpu=native on ARM; `native.arm64` in a universal kit),
    // else the option's -march=native.
    auto native = vars.find("native." + arch);
    if (native == vars.end()) native = vars.find("native");
    if (native != vars.end() && options.native == BuildOptions{}.native) {
        for (std::string& f : split_flags(native->second, kit_dir)) cxxflags.push_back(std::move(f));
    } else if (!options.native.empty()) {
        cxxflags.push_back(options.native);
    }
    cxxflags.push_back("-Igen");
    const std::vector<std::string> ldflags = flags_of("ldflags");
    const std::vector<std::string> libs = flags_of("libs");

    CompileJob job;
    job.cwd = work;
    job.logs = work / "logs";
    job.cache = kit.cache();
    job.compiler = kit.compiler();
    job.compiler_args = kit.compiler_args;
    job.cxxflags = cxxflags;
    job.target = "cpu\n" + cpu_identity() + "\n";
    job.include = kit.kit / "include";
    job.use_cache = options.use_cache;
    job.jobs = options.jobs;
    job.memory_mb = kit.compile_memory_mb;
    job.see_log = kSeeLog;
    const auto objects = compile_lifted(job, log, progress, cancel);
    if (!objects) return failed(objects.error());
    if (cancel) return stopped();

    // Link.
    progress.stage = Stage::Linking;
    const fs::path linked = work / "obj" / from_utf8(kit.exe);  // moved to work/<exe> below
    fs::remove(linked, ec);
    {
        Command link = compiler_tool(ldflags, work / "logs" / "link.log");
        link.args.push_back("-o");
        link.args.push_back("obj/" + kit.exe);
        link.args.insert(link.args.end(), objects->begin(), objects->end());
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
    trim_cache(kit.cache(), options.cache_bytes.value_or(kit.cache_mb << 20));
    log.line("Built " + kit.exe + ": " + std::to_string(progress.cached.load()) + " of " + std::to_string(progress.total.load()) +
             " files from the cache.");
    progress.stage = Stage::Done;
    return {};
}

}  // namespace qilin::launcher
