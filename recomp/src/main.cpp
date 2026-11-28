// qilin: Toy Commander static recompiler (work in progress).
//
//   qilin disc    <disc image>                the disc's bootstrap, tracks and files (.cue, .gdi or .chd)
//   qilin extract <disc image> --out <dir>    the disc's files and its bootstrap (IP.BIN), to a folder
//   qilin identify <disc image or boot file> --json [--build <title>]
//                                             what it is, for the launcher: one JSON object on
//                                             stdout (identify.hpp); exit 0 the supported build,
//                                             3 another, 2 unreadable
//   qilin info    <boot file>   (any command taking a boot file also takes the disc image) [--json out.json]
//                                             SHA-256, supported build, load address
//   qilin insns   <boot file> [-o out.txt]    every word of the boot file, decoded (a linear
//                                             sweep: data decodes too, until function discovery)
//   qilin disasm  [--pc addr] [-o out.txt]    all 65,536 words decoded at one address
//                                             (for the cross-check of the whole encoding space)
//   qilin profile <boot file>                 instruction counts of the linear sweep
//   qilin cfg     <boot file> [--json out.json]
//                                             function discovery and control flow, summary
//   qilin coverage <boot file> [--list|--map] every byte: code, literal pool, jump table, padding
//                                             or unknown, per region of the build's layout (exit 3
//                                             while a lifted region has unknown bytes);
//                                             --map: every run with the function that claimed it
//   qilin suspects <boot file>                functions with problems, for review
//   qilin fpmode   <boot file> [--list] [--json out.json]
//                                             the FPSCR mode at every FPU instruction, per region;
//                                             --list: every place it can't be proven;
//                                             --json: every function's entry and exit modes
//   qilin lift     <boot file> --out <dir> [--range lo-hi] [--no-orphans] [--files N]
//                                             the lifted C++: functions.hpp, table.cpp, stubs.cpp,
//                                             lifted_NNN.cpp (256 functions each), lift_report.txt
//   qilin hwaccess <boot file> [--json out.json]
//                                             loads, stores and cache operations at constant
//                                             addresses outside cached RAM, per function
//
// Listings are one line per word: address, word, text ("8c010014 d005 mov.l 0x8c01002c,r0").
// Exit status is non-zero on any error; nothing fails silently.
#include "command_line.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "bootfile.hpp"
#include "cfg.hpp"
#include "decoder.hpp"
#include "fpmode.hpp"
#include "gdrom.hpp"
#include "hwaccess.hpp"
#include "identify.hpp"
#include "layout.hpp"
#include "lift_output.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "unknown"
#endif

using namespace qilin;

namespace {

struct Failure {
    std::string message;
};

int usage() {
    std::print(stderr,
               "qilin " QILIN_VERSION "\n"
               "usage:\n"
               "  qilin disc    <disc image: .cue, .gdi or .chd>\n"
               "  qilin identify <disc image or boot file> --json [--build <title>]\n"
               "  qilin extract <disc image> --out <dir>\n"
               "  qilin info    <boot file> [--json out.json]\n"
               "  qilin insns   <boot file> [-o out.txt]\n"
               "  qilin disasm  [--pc addr] [-o out.txt]\n"
               "  qilin profile <boot file>\n"
               "  qilin cfg     <boot file> [--json out.json]\n"
               "  qilin coverage <boot file> [--list|--map]\n"
               "  qilin suspects <boot file>\n"
               "  qilin fpmode   <boot file> [--list] [--json out.json]\n"
               "  qilin lift     <boot file> --out <dir> [--range lo-hi] [--no-orphans] [--files N]\n"
               "  qilin hwaccess <boot file> [--json out.json]\n"
               "  qilin --version\n");
    return 2;
}

uint32_t parse_address(std::string_view s) {
    if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2);
    uint32_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || s.empty()) throw Failure{std::format("bad address '{}'", s)};
    return v;
}

// Output to a file, or stdout when no path is given.
class Out {
public:
    explicit Out(const std::optional<std::string>& path) {
        if (!path) return;
        #if defined(_WIN32)
        file_ = _wfopen(native_path(*path).c_str(), L"wb");
#else
        file_ = std::fopen(path->c_str(), "wb");
#endif
        if (!file_) throw Failure{std::format("cannot write {}", *path)};
        path_ = *path;
    }
    ~Out() {
        if (file_) std::fclose(file_);
    }
    Out(const Out&) = delete;
    Out& operator=(const Out&) = delete;
    std::FILE* get() const { return file_ ? file_ : stdout; }
    void close() {
        if (!file_) return;
        bool bad = std::ferror(file_) != 0;
        if (std::fclose(file_) != 0) bad = true;
        file_ = nullptr;
        if (bad) throw Failure{std::format("cannot write {}", path_)};
    }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
};

// A boot file, or a disc's (a .cue, .gdi or .chd): the file its bootstrap names.
BootFile load_boot(const std::string& path) {
    if (gdrom::Disc::is_image(native_path(path))) {
        auto disc = gdrom::Disc::open(native_path(path));
        if (!disc) throw Failure{disc.error()};
        const auto ip = disc->bootstrap();
        if (!ip) throw Failure{ip.error()};
        const auto files = disc->files();
        if (!files) throw Failure{files.error()};
        for (const gdrom::File& f : *files)
            if (f.path == ip->boot_file) {
                const auto bytes = disc->read_file(f);
                if (!bytes) throw Failure{bytes.error()};
                auto boot = BootFile::parse(*bytes);
                if (!boot) throw Failure{boot.error()};
                return std::move(*boot);
            }
        throw Failure{std::format("{} has no {}, its boot file", path, ip->boot_file)};
    }
    auto boot = BootFile::load(native_path(path));
    if (!boot) throw Failure{boot.error()};
    return std::move(*boot);
}

gdrom::Disc open_disc(const std::string& path) {
    auto disc = gdrom::Disc::open(native_path(path));
    if (!disc) throw Failure{disc.error()};
    return std::move(*disc);
}

template <typename T>
T must(std::expected<T, std::string> r) {
    if (!r) throw Failure{r.error()};
    return std::move(*r);
}

int cmd_disc(const std::string& cue) {
    const gdrom::Disc disc = open_disc(cue);
    const gdrom::Bootstrap b = must(disc.bootstrap());
    std::println("title      {}", b.title);
    std::println("product    {}  {}  {}", b.product, b.version, b.date);
    std::println("company    {}", b.company);
    std::println("device     {}", b.device);
    std::println("areas      {}", b.areas);
    std::println("peripherals {}", b.peripherals);
    std::println("boot file  {}", b.boot_file);
    std::println("tracks:");
    for (const gdrom::Track& t : disc.tracks())
        std::println("  {:2}  {}  sector {:6}  {:6} sectors  {}", t.number, t.audio ? "audio" : "data ", t.start,
                     t.end() - t.start, t.high_density ? "high density" : "single density");
    const std::vector<gdrom::File> files = must(disc.files());
    uint64_t total = 0;
    std::println("files:");
    for (const gdrom::File& f : files) {
        std::println("  {:6}  {:10}  {}", f.sector, f.size, f.path);
        total += f.size;
    }
    std::println("{} files, {} bytes", files.size(), total);
    return 0;
}

int cmd_extract(const std::string& cue, const std::string& out_dir) {
    const gdrom::Disc disc = open_disc(cue);
    const std::filesystem::path root = native_path(out_dir);
    const auto write = [](const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f.flush()) throw Failure{std::format("cannot write {}", path.string())};
    };
    std::filesystem::create_directories(root);
    write(root / "IP.BIN", must(disc.read(gdrom::kHighDensityStart, 16 * gdrom::kSectorData)));
    const std::vector<gdrom::File> files = must(disc.files());
    for (const gdrom::File& f : files) write(root / "files" / f.path, must(disc.read_file(f)));
    std::println("{} files and IP.BIN written to {}", files.size(), root.string());
    return 0;
}

int cmd_info(const std::string& path, const std::optional<std::string>& json) {
    const BootFile boot = load_boot(path);
    Out out(json);
    std::print(out.get(), "{}", to_json(boot));
    out.close();
    if (json) {
        const KnownBuild* b = known_build(boot.sha256);
        std::println("{}: {}", path, b ? b->title : "not a supported build");
    }
    return known_build(boot.sha256) ? 0 : 3;
}

int cmd_insns(const std::string& path, const std::optional<std::string>& out_path) {
    const BootFile boot = load_boot(path);
    Out out(out_path);
    for (uint32_t pc = boot.base(); pc < boot.end(); pc += 2) {
        const sh4::Insn i = sh4::decode(boot.u16(pc));
        std::println(out.get(), "{:08x} {:04x} {}", pc, i.word, i.valid() ? sh4::disasm(i, pc) : "(invalid)");
    }
    out.close();
    return 0;
}

int cmd_disasm(uint32_t pc, const std::optional<std::string>& out_path) {
    if (pc % 2) throw Failure{"--pc must be even"};
    Out out(out_path);
    for (uint32_t w = 0; w < 0x10000; ++w) {
        const sh4::Insn i = sh4::decode(static_cast<uint16_t>(w));
        std::println(out.get(), "{:08x} {:04x} {}", pc, w, i.valid() ? sh4::disasm(i, pc) : "(invalid)");
    }
    out.close();
    return 0;
}

int cmd_profile(const std::string& path) {
    const BootFile boot = load_boot(path);
    std::map<std::string_view, uint32_t> count;
    uint32_t invalid = 0, total = 0, fp_pr = 0, fp_sz = 0, privileged = 0;
    for (uint32_t pc = boot.base(); pc < boot.end(); pc += 2, ++total) {
        const sh4::Insn i = sh4::decode(boot.u16(pc));
        if (!i.valid()) {
            ++invalid;
            continue;
        }
        const sh4::Spec& s = sh4::spec(i.op);
        ++count[s.name];
        fp_pr += (s.flags & sh4::kFpPr) != 0;
        fp_sz += (s.flags & sh4::kFpSz) != 0;
        privileged += (s.flags & sh4::kPrivileged) != 0;
    }
    std::vector<std::pair<uint32_t, std::string_view>> sorted;
    for (const auto& [name, n] : count) sorted.emplace_back(n, name);
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    std::println("linear sweep of {} words (data decodes too): {} invalid", total, invalid);
    std::println("depend on FPSCR.PR: {}, on FPSCR.SZ: {}, privileged: {}", fp_pr, fp_sz, privileged);
    for (const auto& [n, name] : sorted) std::println("{:8} {}", n, name);
    return 0;
}

// The analysis, with the build's layout when the build is a supported one.
Cfg analyse_boot(const BootFile& boot) {
    Known known;
    for (const KnownEntry& e : known_entries(boot.sha256)) known.entries.push_back(e.entry);
    for (const DataBlock& d : data_blocks(boot.sha256)) known.data.emplace_back(d.begin, d.end);
    for (const ReplacedFunction& r : replaced_functions(boot.sha256))
        if (r.bytes) known.stubs.emplace_back(r.entry, r.entry + r.bytes);
    Cfg cfg = analyse(boot, layout(boot.sha256), known);
    for (const KnownEntry& e : known_entries(boot.sha256))
        if (!cfg.functions.contains(e.entry)) {
            std::string why = "not a function";
            for (const Rejected& r : cfg.rejected)
                if (r.address == e.entry) why = r.reason;
            throw Failure{std::format("the layout's entry {:08X} ({}): {}", e.entry, e.why, why)};
        }
    for (const IndirectTargets& t : indirect_targets(boot.sha256)) {
        if (!cfg.functions.contains(t.function)) throw Failure{std::format("{:08X} ({}) isn't a function", t.function, t.why)};
        auto& targets = cfg.functions.at(t.function).indirect_targets[t.site];
        for (uint32_t k = 0; k < t.count; ++k) {
            const uint32_t target = boot.u32(t.table + 4 * k);
            if (target == 0) continue;
            if (!cfg.functions.contains(target)) throw Failure{std::format("{:08X} in the table at {:08X} ({}) isn't a function", target, t.table, t.why)};
            targets.push_back(target);
        }
    }
    return cfg;
}

int cmd_cfg(const std::string& path, const std::optional<std::string>& json) {
    const BootFile boot = load_boot(path);
    const Cfg cfg = analyse_boot(boot);
    if (json) {
        Out out(json);
        std::print(out.get(), "{}", to_json(cfg));
        out.close();
    }
    std::print("{}", summary(cfg));
    return 0;
}

int cmd_coverage(const std::string& path, bool list, bool map) {
    const BootFile boot = load_boot(path);
    const Cfg cfg = analyse_boot(boot);
    uint32_t unknown = 0;
    const std::string report = coverage(cfg, boot, layout(boot.sha256), list, unknown);
    std::print("{}", map ? kind_map(cfg, boot) : report);
    return unknown ? 3 : 0;
}

int cmd_suspects(const std::string& path) {
    std::print("{}", suspects(analyse_boot(load_boot(path))));
    return 0;
}

int cmd_lift(const std::string& path, const std::string& out_dir, const std::optional<std::string>& range, bool orphans, unsigned files) {
    const BootFile boot = load_boot(path);
    const Cfg cfg = analyse_boot(boot);
    LiftOptions options;
    options.orphans = orphans;
    options.files = files;
    options.replaced = replaced_functions(boot.sha256);
    options.natives = native_footprints(boot.sha256);
    options.idle = idle_loops(boot.sha256);
    if (range) {
        const size_t dash = range->find('-');
        if (dash == std::string::npos) throw Failure{"--range takes lo-hi"};
        options.range = std::pair(parse_address(range->substr(0, dash)), parse_address(range->substr(dash + 1)));
    }
    const LiftSummary sum = write_lifted(boot, cfg, layout(boot.sha256), native_path(out_dir), options, QILIN_VERSION);
    std::print("{}", sum.report.substr(0, sum.report.find('\n') + 1));
    for (const auto& [why, fns] : sum.failures) std::println("  {:5}  {}", fns.size(), why);
    std::println("{} files written, {} removed ({})", sum.written, sum.removed, out_dir);
    // A function not lifted would stop the game when reached: the build stops now instead.
    if (sum.failed != 0) throw Failure{std::format("{} function{} not lifted (above)", sum.failed, sum.failed == 1 ? "" : "s")};
    return 0;
}

int cmd_fpmode(const std::string& path, bool list, const std::optional<std::string>& json) {
    const BootFile boot = load_boot(path);
    const Cfg cfg = analyse_boot(boot);
    const auto regions = layout(boot.sha256);
    const auto region_of = [&](uint32_t a) -> std::string_view {
        for (const Region& r : regions)
            if (a >= r.begin && a < r.end) return r.name;
        return "the whole image";
    };
    struct Totals {
        uint32_t functions = 0, fpu = 0, unproven = 0, dependent = 0, joins = 0, problems = 0, pr = 0, sz = 0, fr = 0, rz = 0, nodn = 0, other_entry = 0;
    };
    std::map<std::string_view, Totals> totals;
    std::string text;
    const std::map<uint32_t, FpModes> modes = fp_modes(boot, cfg, regions);
    for (const auto& [entry, m] : modes) {
        if (!m.fpu && m.problems.empty()) continue;
        Totals& t = totals[region_of(entry)];
        ++t.functions;
        t.fpu += m.fpu;
        t.unproven += m.unproven;
        t.dependent += m.entry_dependent;
        t.joins += m.joins != 0;
        t.problems += !m.problems.empty();
        // Entered other than in the default mode, in a bit that matters (the bank doesn't).
        constexpr uint32_t kMatters = fpscr::kPR | fpscr::kSZ | fpscr::kRM | fpscr::kDN;
        t.other_entry += !(m.entry.known(kMatters) && (m.entry.value & kMatters) == (fpscr::kDefault & kMatters));
        for (const auto& [pc, b] : m.at) {
            const auto on = [&](uint32_t bit) { return b.known(bit) && (b.value & bit); };
            t.pr += on(fpscr::kPR);
            t.sz += on(fpscr::kSZ);
            t.fr += on(fpscr::kFR);
            t.rz += b.known(fpscr::kRM) && (b.value & fpscr::kRM) == 1;
            t.nodn += b.known(fpscr::kDN) && !(b.value & fpscr::kDN);
        }
        if (list && !m.problems.empty()) {
            text += std::format("fn_{:08X}\n", entry);
            for (const std::string& p : m.problems) text += std::format("  {}\n", p);
        }
    }
    if (json) {
        Out out(json);
        const auto bits = [](const KnownBits& b) {
            return std::format("{{\"mask\":\"{:08X}\",\"value\":\"{:08X}\",\"smask\":\"{:08X}\",\"sinv\":\"{:08X}\"}}", b.mask, b.value, b.smask,
                               b.sinv);
        };
        std::print(out.get(), "{{\"functions\":[\n");
        bool first = true;
        for (const auto& [entry, m] : modes) {
            std::string ctx;
            for (const auto& [en, ex] : m.per_context)
                ctx += std::format("{}[{},{}]", ctx.empty() ? "" : ",", bits(en), ex ? bits(*ex) : "null");
            std::string calls;
            for (const auto& [callee, mode] : m.calls) calls += std::format("{}[\"{:08X}\",{}]", calls.empty() ? "" : ",", callee, bits(mode));
            std::print(out.get(), "{}{{\"entry\":\"{:08X}\",\"address_taken\":{},\"entry_mode\":{},\"exit_mode\":{},\"summary_exit\":{},\"fpu\":{},\"unproven\":{},\"contexts\":[{}],\"calls\":[{}],\"problems\":{}}}",
                       first ? "" : ",\n", entry, m.address_taken, bits(m.entry), m.exit ? bits(*m.exit) : "null", m.summary_exit ? bits(*m.summary_exit) : "null", m.fpu, m.unproven, ctx, calls,
                       m.problems.size());
            first = false;
        }
        std::print(out.get(), "\n]}}\n");
        out.close();
    }
    std::print("{}", text);
    std::println("{:>9} {:>8} {:>9} {:>9} {:>9} {:>9}   {:>6} {:>6} {:>6} {:>6} {:>6}   region", "functions", "fpu", "unproven", "entry-dep",
                 "problems", "not-dflt", "PR=1", "SZ=1", "FR=1", "RM=1", "DN=0");
    for (const auto& [region, t] : totals)
        std::println("{:9} {:8} {:9} {:9} {:9} {:9}   {:6} {:6} {:6} {:6} {:6}   {}", t.functions, t.fpu, t.unproven, t.dependent, t.problems,
                     t.other_entry, t.pr, t.sz, t.fr, t.rz, t.nodn, region);
    std::println("functions, fpu: those with FPU instructions or problems; unproven: FPU instructions whose mode bits aren't known;");
    std::println("entry-dep: FPU instructions whose mode follows an entry bit the callers disagree on (lifted once per mode);");
    std::println("not-dflt: functions entered in a mode other than the default; PR=1 ... DN=0: FPU instructions run in that mode");
    return 0;
}

int cmd_hwaccess(const std::string& path, const std::optional<std::string>& json) {
    const BootFile boot = load_boot(path);
    const Cfg cfg = analyse_boot(boot);
    static constexpr std::string_view kKind[] = {"load", "store", "modify", "cache"};
    std::map<std::string_view, std::pair<std::set<uint32_t>, uint32_t>> regions;  // functions, accesses
    std::string text, js = "{\"functions\":[\n";
    bool first = true;
    size_t functions = 0;
    for (const auto& [entry, f] : cfg.functions) {
        const std::vector<HwAccess> found = hardware_accesses(boot, cfg, f);
        if (found.empty()) continue;
        ++functions;
        text += std::format("fn_{:08X}\n", entry);
        js += std::format("{}{{\"entry\":\"{:08X}\",\"accesses\":[", first ? "" : ",\n", entry);
        first = false;
        for (size_t k = 0; k < found.size(); ++k) {
            const HwAccess& h = found[k];
            const std::string_view region = hw_region(h.ea);
            text += std::format("  {:08X}  {:6} {:2}  {:08X}  {}\n", h.pc, kKind[h.kind], h.size, h.ea, region);
            js += std::format("{}{{\"pc\":\"{:08X}\",\"kind\":\"{}\",\"size\":{},\"ea\":\"{:08X}\",\"region\":\"{}\"}}",
                              k ? "," : "", h.pc, kKind[h.kind], h.size, h.ea, region);
            auto& [fns, n] = regions[region];
            fns.insert(entry);
            ++n;
        }
        js += "]}";
    }
    js += "\n]}\n";
    if (json) {
        Out out(json);
        std::print(out.get(), "{}", js);
        out.close();
    } else {
        std::print("{}", text);
    }
    std::println("{} functions reach hardware at constant addresses:", functions);
    for (const auto& [region, v] : regions) std::println("  {:6} accesses in {:4} functions  {}", v.second, v.first.size(), region);
    return 0;
}

// The launcher's contract (identify.hpp): JSON whatever happens, and only
// exit codes 0, 3 and 2.
int cmd_identify(int argc, char** argv) {
    std::optional<std::string> path, build;
    bool json = false;
    std::string problem;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--build" && i + 1 < argc) build = argv[++i];
        else if (!a.starts_with("-") && !path) path = std::string(a);
        else problem = std::format("identify: unexpected argument {}", a);
    }
    if (!path) problem = "identify takes one file";
    if (!json) problem = "identify needs --json";
    Identity id;
    if (!problem.empty()) {
        id.message = problem;
    } else {
        try {
            id = identify(native_path(*path), build.value_or(""));
        } catch (const std::exception& e) {
            id = Identity{};
            id.message = std::format("The recompiler failed: {}.", e.what());
        }
    }
    std::print("{}", to_json(id));
    std::fflush(stdout);
    return id.code;
}

int run(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "identify") return cmd_identify(argc, argv);
    std::vector<std::string> pos;
    std::optional<std::string> out, json, out_dir, pc;
    bool list = false, map = false, orphans = true;
    std::optional<std::string> range, files;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto value = [&](std::optional<std::string>& into) {
            if (++i >= argc) throw Failure{std::format("{} needs a value", a)};
            into = argv[i];
        };
        if (a == "--version") {
            std::println("qilin " QILIN_VERSION);
            return 0;
        } else if (a == "-o") value(out);
        else if (a == "--json") value(json);
        else if (a == "--out") value(out_dir);
        else if (a == "--pc") value(pc);
        else if (a == "--list") list = true;
        else if (a == "--map") map = true;
        else if (a == "--no-orphans") orphans = false;
        else if (a == "--range") value(range);
        else if (a == "--files") value(files);
        else if (a.starts_with("-")) throw Failure{std::format("unknown option {}", a)};
        else pos.emplace_back(a);
    }
    if (pos.empty()) return usage();
    const std::string& command = pos[0];
    const auto input = [&]() -> const std::string& {
        if (pos.size() != 2) throw Failure{std::format("{} takes one file", command)};
        return pos[1];
    };
    if (command == "disc") return cmd_disc(input());
    if (command == "extract") {
        if (!out_dir) throw Failure{"extract needs --out <dir>"};
        return cmd_extract(input(), *out_dir);
    }
    if (command == "info") return cmd_info(input(), json);
    if (command == "insns") return cmd_insns(input(), out);
    if (command == "profile") return cmd_profile(input());
    if (command == "cfg") return cmd_cfg(input(), json);
    if (command == "coverage") return cmd_coverage(input(), list, map);
    if (command == "suspects") return cmd_suspects(input());
    if (command == "hwaccess") return cmd_hwaccess(input(), json);
    if (command == "fpmode") return cmd_fpmode(input(), list, json);
    if (command == "lift") {
        if (!out_dir) throw Failure{"lift needs --out <dir>"};
        unsigned n = 0;
        if (files) {
            n = static_cast<unsigned>(std::strtoul(files->c_str(), nullptr, 10));
            if (n == 0 || n > 4096) throw Failure{"--files takes a count from 1 to 4096"};
        }
        return cmd_lift(input(), *out_dir, range, orphans, n);
    }
    if (command == "disasm") {
        if (pos.size() != 1) throw Failure{"disasm takes no file"};
        return cmd_disasm(pc ? parse_address(*pc) : kBootFileBase, out);
    }
    return usage();
}

}  // namespace

int main(int argc, char** argv) {
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count(); argv = command_line.values();
    try {
        return run(argc, argv);
    } catch (const Failure& f) {
        std::println(stderr, "qilin: {}", f.message);
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: internal error: {}", e.what());
    }
    return 1;
}
