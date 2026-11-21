// `qilin cfg`, `coverage` and `suspects`: text and JSON. Developer tools only.
#include <algorithm>
#include <array>
#include <format>
#include <map>

#include "cfg.hpp"

namespace qilin {

std::string summary(const Cfg& cfg) {
    std::array<size_t, 5> by_origin{};
    size_t insns = 0, calls = 0, tails = 0, ind_calls = 0, ind_jumps = 0, problems = 0;
    std::map<uint32_t, size_t> switches;  // braf -> cases: shared code reaches a switch from several functions
    for (const auto& [entry, f] : cfg.functions) {
        ++by_origin[static_cast<size_t>(f.origin)];
        insns += f.insns.size();
        calls += f.calls.size();
        tails += f.tail_jumps.size();
        ind_calls += f.indirect_calls.size();
        ind_jumps += f.indirect_jumps.size();
        for (const Switch& sw : f.switches) switches[sw.braf] = sw.targets.size();
        problems += !f.problems.empty();
    }
    std::array<size_t, 6> words{};
    for (Kind k : cfg.kind) ++words[static_cast<size_t>(k)];
    size_t rejected_calls = 0;
    for (const Rejected& r : cfg.rejected) rejected_calls += r.origin == Origin::call || r.origin == Origin::tail;
    std::string s;
    s += std::format("functions            {}\n", cfg.functions.size());
    for (size_t i = 0; i < by_origin.size(); ++i) s += std::format("  from {:8}      {}\n", name(static_cast<Origin>(i)), by_origin[i]);
    s += std::format("instructions         {} (counted once per function that reaches them)\n", insns);
    s += std::format("code bytes           {}\n", words[1] * 2);
    s += std::format("literal bytes        {}\n", words[2] * 2);
    size_t cases = 0;
    for (const auto& [braf, n] : switches) cases += n;
    s += std::format("jump tables          {} ({} cases, {} bytes)\n", switches.size(), cases, words[3] * 2);
    s += std::format("data bytes           {} (the layout's data regions)\n", words[5] * 2);
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

std::string coverage(const Cfg& cfg, const BootFile& boot, std::span<const Region> regions, bool list, uint32_t& unknown_bytes) {
    // Without a layout, the whole image is one region that must be accounted for.
    const Region whole{boot.base(), boot.end(), "the whole image", Plan::lift};
    if (regions.empty()) regions = std::span(&whole, 1);
    struct Run {
        uint32_t address, size;
        bool zero;
    };
    std::string s = std::format("{:19}  {:9}  {:>7} {:>7} {:>6} {:>6} {:>7} {:>7}  {}\n", "range", "plan", "code", "literal", "tables",
                                "pad", "data", "unknown", "region");
    std::array<uint64_t, 6> total{};
    std::vector<Run> runs;
    unknown_bytes = 0;
    for (const Region& r : regions) {
        std::array<uint32_t, 6> bytes{};
        for (uint32_t a = std::max(r.begin, boot.base()); a < r.end && a < boot.end(); a += 2) bytes[static_cast<size_t>(cfg.at(a))] += 2;
        for (size_t k = 0; k < bytes.size(); ++k) total[k] += bytes[k];
        s += std::format("{:08X}-{:08X}  {:9}  {:7} {:7} {:6} {:6} {:7} {:7}  {}\n", r.begin, r.end, name(r.plan), bytes[1], bytes[2], bytes[3],
                         bytes[4], bytes[5], bytes[0], r.name);
        if (r.plan == Plan::lift) unknown_bytes += bytes[0];
        for (uint32_t a = r.begin; a < r.end;) {
            if (cfg.at(a) != Kind::unknown) {
                a += 2;
                continue;
            }
            uint32_t b = a;
            bool zero = true;
            while (b < r.end && cfg.at(b) == Kind::unknown) zero &= boot.u16(b) == 0, b += 2;
            runs.push_back({a, b - a, zero});
            a = b;
        }
    }
    const uint64_t size = boot.size();
    s += std::format("{:19}  {:9}  {:7} {:7} {:6} {:6} {:7} {:7}  ({:.2f}% unknown)\n", "total", "", total[1], total[2], total[3], total[4],
                     total[5], total[0], 100.0 * double(total[0]) / double(size));
    s += std::format("{} unknown runs; {} unknown bytes in lifted regions\n", runs.size(), unknown_bytes);
    std::vector<Run> longest = runs;
    std::ranges::sort(longest, [](const Run& x, const Run& y) { return x.size != y.size ? x.size > y.size : x.address < y.address; });
    for (size_t i = 0; i < longest.size() && i < 10; ++i)
        s += std::format("  {:08X}  {:6} bytes{}\n", longest[i].address, longest[i].size, longest[i].zero ? "  (zero)" : "");
    if (list)
        for (const Run& r : runs) s += std::format("{:08X} {}{}\n", r.address, r.size, r.zero ? " zero" : "");
    return s;
}

std::string kind_map(const Cfg& cfg, const BootFile& boot) {
    static constexpr std::string_view kName[] = {"unknown", "code", "literal", "jump_table", "padding", "data"};
    std::string s;
    for (uint32_t a = boot.base(); a < boot.end();) {
        const size_t i = (a - boot.base()) / 2;
        const Kind k = cfg.kind[i];
        const uint32_t owner = cfg.owner[i];
        uint32_t b = a;
        while (b < boot.end() && cfg.kind[(b - boot.base()) / 2] == k && cfg.owner[(b - boot.base()) / 2] == owner) b += 2;
        s += owner ? std::format("{:08X} {:6} {:10} fn_{:08X}\n", a, b - a, kName[static_cast<size_t>(k)], owner)
                   : std::format("{:08X} {:6} {}\n", a, b - a, kName[static_cast<size_t>(k)]);
        a = b;
    }
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
