// `qilin cfg`, `coverage` and `suspects`: text and JSON. Developer tools only.
#include <algorithm>
#include <array>
#include <format>

#include "cfg.hpp"

namespace qilin {

std::string summary(const Cfg& cfg) {
    std::array<size_t, 5> by_origin{};
    size_t insns = 0, calls = 0, tails = 0, ind_calls = 0, ind_jumps = 0, switches = 0, cases = 0, problems = 0;
    for (const auto& [entry, f] : cfg.functions) {
        ++by_origin[static_cast<size_t>(f.origin)];
        insns += f.insns.size();
        calls += f.calls.size();
        tails += f.tail_jumps.size();
        ind_calls += f.indirect_calls.size();
        ind_jumps += f.indirect_jumps.size();
        switches += f.switches.size();
        for (const Switch& sw : f.switches) cases += sw.targets.size();
        problems += !f.problems.empty();
    }
    std::array<size_t, 5> words{};
    for (Kind k : cfg.kind) ++words[static_cast<size_t>(k)];
    size_t rejected_calls = 0;
    for (const Rejected& r : cfg.rejected) rejected_calls += r.origin == Origin::call || r.origin == Origin::tail;
    std::string s;
    s += std::format("functions            {}\n", cfg.functions.size());
    for (size_t i = 0; i < by_origin.size(); ++i) s += std::format("  from {:8}      {}\n", name(static_cast<Origin>(i)), by_origin[i]);
    s += std::format("instructions         {} (counted once per function that reaches them)\n", insns);
    s += std::format("code bytes           {}\n", words[1] * 2);
    s += std::format("literal bytes        {}\n", words[2] * 2);
    s += std::format("jump tables          {} ({} cases, {} bytes)\n", switches, cases, words[3] * 2);
    s += std::format("direct calls         {}\n", calls);
    s += std::format("constant tail jumps  {}\n", tails);
    s += std::format("indirect calls       {}\n", ind_calls);
    s += std::format("indirect jumps       {}\n", ind_jumps);
    s += std::format("functions with problems  {}\n", problems);
    s += std::format("rejected candidates  {} ({} of them call or jump targets)\n", cfg.rejected.size(), rejected_calls);
    return s;
}

std::string to_json(const Cfg& cfg) {
    const auto list = [](const auto& v, auto&& item) {
        std::string s = "[";
        bool first = true;
        for (const auto& x : v) {
            if (!first) s += ",";
            first = false;
            s += item(x);
        }
        return s + "]";
    };
    const auto hex = [](uint32_t a) { return std::format("\"{:08X}\"", a); };
    std::string s = "{\"functions\":[\n";
    bool first = true;
    for (const auto& [entry, f] : cfg.functions) {
        if (!first) s += ",\n";
        first = false;
        s += std::format("{{\"entry\":{},\"origin\":\"{}\",\"end\":{},\"instructions\":{}", hex(f.entry), name(f.origin), hex(f.end()),
                         f.insns.size());
        s += ",\"calls\":" + list(f.calls, [&](const auto& c) { return std::format("[{},{}]", hex(c.first), hex(c.second)); });
        s += ",\"tail_jumps\":" + list(f.tail_jumps, [&](const auto& c) { return std::format("[{},{}]", hex(c.first), hex(c.second)); });
        s += ",\"indirect_calls\":" + list(f.indirect_calls, hex);
        s += ",\"indirect_jumps\":" + list(f.indirect_jumps, hex);
        s += ",\"switches\":" + list(f.switches, [&](const Switch& sw) {
            return std::format("{{\"braf\":{},\"table\":{},\"targets\":{}}}", hex(sw.braf), hex(sw.table), list(sw.targets, hex));
        });
        s += ",\"problems\":" + list(f.problems, [](const std::string& p) { return std::format("\"{}\"", p); });
        s += "}";
    }
    s += "\n],\"rejected\":[\n";
    first = true;
    for (const Rejected& r : cfg.rejected) {
        if (!first) s += ",\n";
        first = false;
        s += std::format("{{\"address\":{},\"origin\":\"{}\",\"reason\":\"{}\"}}", hex(r.address), name(r.origin), r.reason);
    }
    return s + "\n]}\n";
}

std::string coverage(const Cfg& cfg, const BootFile& boot, bool list, uint32_t& unknown_bytes) {
    std::array<uint32_t, 5> bytes{};
    for (Kind k : cfg.kind) bytes[static_cast<size_t>(k)] += 2;
    struct Run {
        uint32_t address, size;
        bool zero;
    };
    std::vector<Run> runs;
    uint32_t zero_bytes = 0;
    for (uint32_t a = boot.base(); a < boot.end();) {
        if (cfg.at(a) != Kind::unknown) {
            a += 2;
            continue;
        }
        uint32_t b = a;
        bool zero = true;
        while (b < boot.end() && cfg.at(b) == Kind::unknown) zero &= boot.u16(b) == 0, b += 2;
        runs.push_back({a, b - a, zero});
        if (zero) zero_bytes += b - a;
        a = b;
    }
    const uint32_t total = boot.size();
    const auto line = [&](std::string_view what, uint32_t n) { return std::format("{:22} {:8} bytes  {:6.2f}%\n", what, n, 100.0 * n / total); };
    std::string s;
    s += line("code", bytes[1]);
    s += line("literal pools", bytes[2]);
    s += line("jump tables", bytes[3]);
    s += line("padding", bytes[4]);
    s += line("unknown, all zero", zero_bytes);
    s += line("unknown", bytes[0] - zero_bytes);
    s += std::format("{} unknown runs; the longest:\n", runs.size());
    std::vector<Run> longest = runs;
    std::ranges::sort(longest, [](const Run& x, const Run& y) { return x.size != y.size ? x.size > y.size : x.address < y.address; });
    for (size_t i = 0; i < longest.size() && i < 10; ++i)
        s += std::format("  {:08X}  {:6} bytes{}\n", longest[i].address, longest[i].size, longest[i].zero ? "  (zero)" : "");
    if (list)
        for (const Run& r : runs) s += std::format("{:08X} {}{}\n", r.address, r.size, r.zero ? " zero" : "");
    unknown_bytes = bytes[0];
    return s;
}

std::string suspects(const Cfg& cfg) {
    std::string s;
    size_t n = 0;
    for (const auto& [entry, f] : cfg.functions) {
        if (f.problems.empty() && f.indirect_jumps.empty()) continue;
        ++n;
        s += std::format("fn_{:08X} ({}):\n", entry, name(f.origin));
        for (const std::string& p : f.problems) s += std::format("  {}\n", p);
        for (uint32_t pc : f.indirect_jumps) s += std::format("  {:08X}: an indirect jump with no constant target and no table\n", pc);
    }
    for (const Rejected& r : cfg.rejected)
        if (r.origin == Origin::call || r.origin == Origin::tail || r.origin == Origin::entry) {
            ++n;
            s += std::format("{:08X} ({} target, not kept): {}\n", r.address, name(r.origin), r.reason);
        }
    s += std::format("{} suspects\n", n);
    return s;
}

}  // namespace qilin
