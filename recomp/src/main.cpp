// qilin: Toy Commander static recompiler (work in progress).
//
//   qilin disc    <game.cue>                  the disc's bootstrap, tracks and files
//   qilin extract <game.cue> --out <dir>      the disc's files and its bootstrap (IP.BIN), to a folder
//   qilin info    <1GUTH.BIN> [--json out.json]
//                                             SHA-256, supported build, load address
//   qilin insns   <1GUTH.BIN> [-o out.txt]    every word of the boot file, decoded (a linear
//                                             sweep: data decodes too, until function discovery)
//   qilin disasm  [--pc addr] [-o out.txt]    all 65,536 words decoded at one address
//                                             (for the cross-check of the whole encoding space)
//   qilin profile <1GUTH.BIN>                 instruction counts of the linear sweep
//
// Listings are one line per word: address, word, text ("8c010014 d005 mov.l 0x8c01002c,r0").
// Exit status is non-zero on any error; nothing fails silently.
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <expected>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "bootfile.hpp"
#include "decoder.hpp"
#include "gdrom.hpp"

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
               "  qilin disc    <game.cue>\n"
               "  qilin extract <game.cue> --out <dir>\n"
               "  qilin info    <1GUTH.BIN> [--json out.json]\n"
               "  qilin insns   <1GUTH.BIN> [-o out.txt]\n"
               "  qilin disasm  [--pc addr] [-o out.txt]\n"
               "  qilin profile <1GUTH.BIN>\n"
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
        file_ = std::fopen(path->c_str(), "wb");
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
        const bool bad = std::ferror(file_) || std::fclose(file_) != 0;
        file_ = nullptr;
        if (bad) throw Failure{std::format("cannot write {}", path_)};
    }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
};

BootFile load_boot(const std::string& path) {
    auto boot = BootFile::load(path);
    if (!boot) throw Failure{boot.error()};
    return std::move(*boot);
}

gdrom::Disc open_disc(const std::string& path) {
    auto disc = gdrom::Disc::open(path);
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
    const std::filesystem::path root(out_dir);
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

int run(int argc, char** argv) {
    std::vector<std::string> pos;
    std::optional<std::string> out, json, out_dir, pc;
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
    if (command == "disasm") {
        if (pos.size() != 1) throw Failure{"disasm takes no file"};
        return cmd_disasm(pc ? parse_address(*pc) : kBootFileBase, out);
    }
    return usage();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const Failure& f) {
        std::println(stderr, "qilin: {}", f.message);
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: internal error: {}", e.what());
    }
    return 1;
}
