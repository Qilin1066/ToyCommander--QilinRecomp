// qilin_difftest_gen: the differential test's cases, lifted at build time.
//
// For every instruction the lifter lifts, random valid encodings (the decoder
// decides validity), each in small functions: on its own before rts; in rts's
// delay slot; between two fschg (64-bit transfers) where FPSCR.SZ matters;
// between loads of FPSCR setting and clearing PR where precision matters. And
// programs for every kind of branch. The functions are found and lifted by
// the real analysis and lifter, from one synthetic boot file; an encoding the
// lifter refuses is left out and counted. Writes into <dir>: functions.hpp,
// lifted_0.cpp ... lifted_7.cpp, table.cpp (the function table) and
// cases.cpp (the image and the cases).
//
//   qilin_difftest_gen <dir> [encodings-per-instruction]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <print>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "cases.hpp"
#include "cfg.hpp"
#include "decoder.hpp"
#include "fpmode.hpp"
#include "lift_programs.hpp"
#include "lifter.hpp"

using namespace qilin;
using sh4::Op;
using sh4::Opnd;
using test::Asm;
using test::imm;
using test::kNop;
using test::kRts;
using test::n_;
using test::nm;

namespace {

constexpr uint32_t kCases = test::kBase + 0x10000u;  // the cases start here, 64 bytes each (more for some)
constexpr unsigned kFiles = 8;

struct Planned {
    difftest::Kind kind;
    std::vector<uint16_t> words;  // for the instruction kinds: the instruction (one word)
    unsigned program = 0;         // for kind program: which one
};

// Words that decode as nothing, with random bits, to fill the space between
// cases: PC-relative loads read them, and analysis can't take them for code.
uint16_t filler(std::mt19937& rng) {
    for (;;) {
        const uint16_t w = static_cast<uint16_t>(0xF00Fu | (rng() & 0x0FF0u));
        if (!sh4::decode(w).valid()) return w;
    }
}

bool uses_pr(Op op) { return op == Op::lds_pr || op == Op::ldsl_pr || op == Op::sts_pr || op == Op::stsl_pr; }

// Emits case k into the assembler at a 64-byte boundary; returns the entry's label.
std::string emit(Asm& a, const Planned& p, unsigned k, std::mt19937& rng) {
    while ((a.here() - kCases) % 64) a.word(filler(rng));
    const std::string L = std::format("c{}", k), T = L + "t";
    a.label(L);
    const uint16_t w = p.words.empty() ? 0 : p.words[0];
    const sh4::Insn in = sh4::decode(w);
    switch (p.kind) {
    case difftest::plain: a.word(w), a.word(kRts), a.word(kNop); break;
    case difftest::slot: a.word(kRts), a.word(w); break;
    case difftest::sz64: a.word(0xF3FD), a.word(w), a.word(0xF3FD), a.word(kRts), a.word(kNop); break;
    case difftest::double_: {
        // A register no operand of the instruction names, for FPSCR's value.
        unsigned x = 1;
        while (x == in.n() || x == in.m()) ++x;
        a.movl(x, 0x000C0000u), a.word(n_(0x406A, x)), a.word(w);
        a.movl(x, 0x00040000u), a.word(n_(0x406A, x)), a.word(kRts), a.word(kNop);
        a.pool();
        break;
    }
    case difftest::program:
    case difftest::loop:
    case difftest::nowhere:
        switch (p.program) {
        case 0: case 1:  // bt, bf over an add
            a.b8(p.program == 0 ? 0x8900 : 0x8B00, T), a.word(imm(0x7000, 0, 1)), a.word(kRts), a.word(kNop);
            a.label(T), a.word(imm(0x7000, 0, 2)), a.word(kRts), a.word(kNop);
            break;
        case 2: case 3:  // bt/s, bf/s, with a slot that changes T: the branch must have read it first
            a.b8(p.program == 2 ? 0x8D00 : 0x8F00, T), a.word(p.program == 2 ? 0x0018 : 0x0008), a.word(imm(0x7000, 0, 1)), a.word(kRts), a.word(kNop);
            a.label(T), a.word(imm(0x7000, 0, 2)), a.word(kRts), a.word(kNop);
            break;
        case 4:  // bra over an add, with an add in the slot
            a.b12(0xA000, T), a.word(imm(0x7000, 0, 4)), a.word(imm(0x7000, 0, 1));
            a.label(T), a.word(kRts), a.word(kNop);
            break;
        case 5:  // bsr to a callee of its own, PR saved on the stack
            a.word(0x4F22), a.b12(0xB000, T), a.word(imm(0x7000, 1, 4)), a.word(0x4F26), a.word(kRts), a.word(imm(0x7000, 0, 1));
            a.label(T), a.word(imm(0x7000, 0, 2)), a.word(kRts), a.word(nm(0x300C, 0, 0));
            break;
        case 6:  // jsr through the literal pool, with T set in the slot
            a.word(0x4F22), a.movl_label(2, T), a.word(n_(0x400B, 2)), a.word(0x0018), a.word(0x4F26), a.word(kRts), a.word(kNop);
            a.pool();
            a.label(T), a.word(n_(0x0029, 6)), a.word(kRts), a.word(imm(0x7000, 0, 8));
            break;
        case 7:  // a dt loop
            a.label(T), a.word(imm(0x7000, 0, 3)), a.word(n_(0x4010, 1)), a.b8(0x8B00, T), a.word(kRts), a.word(kNop);
            break;
        case 8: a.word(n_(0x400B, 3)), a.word(imm(0x7000, 0, 1)), a.word(kRts), a.word(kNop); break;  // jsr @r3 to nowhere
        case 9: a.word(n_(0x402B, 3)), a.word(imm(0x7000, 0, 1)); break;                              // jmp @r3 to nowhere
        case 10: a.word(n_(0x0023, 3)), a.word(imm(0x7000, 0, 1)); break;                             // braf r3 to nowhere
        case 11: a.word(n_(0x0003, 3)), a.word(imm(0x7000, 0, 1)), a.word(kRts), a.word(kNop); break; // bsrf r3 to nowhere
        default: break;
        }
        break;
    }
    return L;
}

// The steering a program needs (run.cpp sets the registers).
difftest::Kind program_kind(unsigned program) { return program >= 8 ? difftest::nowhere : program == 7 ? difftest::loop : difftest::program; }
constexpr unsigned kPrograms = 12;

struct Built {
    std::vector<uint16_t> image;
    std::vector<std::pair<std::string, uint32_t>> labels;  // case k's entry
};

Built build(const std::vector<Planned>& plan, uint32_t seed) {
    std::mt19937 rng(seed);
    Asm a;
    a.word(kRts), a.word(kNop);  // the boot file's entry: a function on its own
    // A table of every case's address: how analysis finds them (as function pointers).
    std::vector<std::string> names;
    for (unsigned k = 0; k < plan.size(); ++k) names.push_back(std::format("c{}", k)), a.long_of(names.back());
    if (a.here() > kCases) throw std::runtime_error("too many cases for the table");
    while (a.here() < kCases) a.word(filler(rng));
    for (unsigned k = 0; k < plan.size(); ++k) emit(a, plan[k], k, rng);
    while ((a.here() - kCases) % 64) a.word(filler(rng));
    for (int k = 0; k < 512; ++k) a.word(filler(rng));  // what the last PC-relative loads can reach
    Built b;
    b.image = a.finish();
    for (unsigned k = 0; k < plan.size(); ++k) b.labels.emplace_back(names[k], a.at(names[k]));
    return b;
}

struct Analysed {
    BootFile boot;
    Cfg cfg;
    std::map<uint32_t, FpModes> modes;
};

Analysed analyse_image(const std::vector<uint16_t>& image) {
    std::vector<uint8_t> bytes;
    for (const uint16_t w : image) bytes.push_back(static_cast<uint8_t>(w)), bytes.push_back(static_cast<uint8_t>(w >> 8));
    auto boot = BootFile::parse(bytes);
    if (!boot) throw std::runtime_error(boot.error());
    Cfg cfg = analyse(*boot);
    auto modes = fp_modes(*boot, cfg);
    return {std::move(*boot), std::move(cfg), std::move(modes)};
}

// The functions a case consists of: its entry and what it calls directly.
std::vector<uint32_t> functions_of(const Cfg& cfg, uint32_t entry) {
    std::vector<uint32_t> out{entry};
    for (size_t i = 0; i < out.size(); ++i) {
        const auto it = cfg.functions.find(out[i]);
        if (it == cfg.functions.end()) return {};
        for (const auto& [site, target] : it->second.calls)
            if (std::ranges::find(out, target) == out.end()) out.push_back(target);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: qilin_difftest_gen <dir> [encodings-per-instruction]");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    const int per = argc > 2 ? std::atoi(argv[2]) : 16;
    try {
        std::mt19937 rng(20261121);
        std::vector<Planned> plan;
        std::map<std::string, int> planned_per_op;
        for (const sh4::Spec& s : sh4::specs()) {
            const bool sr_op = s.op == Op::stc_sr || s.op == Op::ldc_sr || s.op == Op::stcl_sr || s.op == Op::ldcl_sr;  // modelled
            if (s.op == Op::invalid || (s.flags & sh4::kBranch) || ((s.flags & sh4::kPrivileged) && !sr_op)) continue;
            std::set<uint16_t> seen;
            for (int tries = 0; static_cast<int>(seen.size()) < per && tries < per * 200; ++tries) {
                const uint16_t w = static_cast<uint16_t>(s.match | (rng() & ~s.mask & 0xFFFFu));
                const sh4::Insn in = sh4::decode(w);
                if (in.op != s.op || !seen.insert(w).second) continue;
                // An r0-indexed address whose other register is r0 itself: no one value serves as both.
                bool skip = false;
                for (Opnd o : s.operands)
                    if ((o == Opnd::at_r0_N && in.n() == 0) || (o == Opnd::at_r0_M && in.m() == 0)) skip = true;
                // A PC-relative load reads its own case's filler (64 bytes), not another case's code.
                if ((s.op == Op::movw_pc && (in.disp8() < 2 || in.disp8() > 28)) || ((s.op == Op::movl_pc || s.op == Op::mova) && (in.disp8() < 2 || in.disp8() > 13)))
                    skip = true;
                if (skip) continue;
                plan.push_back({difftest::plain, {w}, 0});
                if (!(s.flags & sh4::kPcRelative) && !uses_pr(s.op)) plan.push_back({difftest::slot, {w}, 0});
                if (s.flags & sh4::kFpSz) plan.push_back({difftest::sz64, {w}, 0});
                if (s.flags & sh4::kFpPr) plan.push_back({difftest::double_, {w}, 0});
                ++planned_per_op[std::format("{}", static_cast<int>(s.op))];
            }
        }
        for (unsigned p = 0; p < kPrograms; ++p) plan.push_back({program_kind(p), {}, p});

        // Pass 1: keep the cases whose functions all lift.
        std::vector<Planned> kept;
        std::map<std::string, int> refused;
        {
            const Built b = build(plan, 1);
            const Analysed an = analyse_image(b.image);
            const Lifter lifter(an.boot, an.cfg, an.modes);
            for (size_t k = 0; k < plan.size(); ++k) {
                const auto fns = functions_of(an.cfg, b.labels[k].second);
                bool ok = !fns.empty();
                std::string why = ok ? "" : "not found as a function";
                for (const uint32_t f : fns) {
                    const LiftResult r = lifter.lift(an.cfg.functions.at(f));
                    if (!r.ok) ok = false, why = r.why;
                }
                if (ok) kept.push_back(plan[k]);
                else if (plan[k].kind == difftest::program || plan[k].kind == difftest::loop || plan[k].kind == difftest::nowhere)
                    throw std::runtime_error(std::format("branch program {} doesn't lift: {}", plan[k].program, why));
                else ++refused[why.substr(0, why.find(':'))];
            }
        }
        // Pass 2: the cases that lift, analysed and lifted again where they now are.
        const Built b = build(kept, 2);
        const Analysed an = analyse_image(b.image);
        const Lifter lifter(an.boot, an.cfg, an.modes);
        std::set<uint32_t> fns{test::kBase};
        for (size_t k = 0; k < kept.size(); ++k) {
            const auto f = functions_of(an.cfg, b.labels[k].second);
            if (f.empty()) {
                const uint32_t e = b.labels[k].second;
                std::string near;
                for (auto it = an.cfg.functions.upper_bound(e); it != an.cfg.functions.begin();) {
                    --it;
                    near = std::format("{:08X} ({} insns, origin {})", it->first, it->second.insns.size(), static_cast<int>(it->second.origin));
                    break;
                }
                throw std::runtime_error(std::format("case {} at {:08X} (kind {}, word {:04X}) isn't a function in pass 2; the function before: {}", k, e,
                                                     static_cast<int>(kept[k].kind), kept[k].words.empty() ? 0 : kept[k].words[0], near));
            }
            fns.insert(f.begin(), f.end());
        }
        std::string decls, table, files[kFiles];
        unsigned idx = 0;
        for (const uint32_t f : fns) {
            const LiftResult r = lifter.lift(an.cfg.functions.at(f));
            if (!r.ok) throw std::runtime_error(std::format("{:08X} doesn't lift in pass 2: {}", f, r.why));
            decls += std::format("void {}(rt::Context& c);\n", fn_name(f));
            table += std::format("    {{0x{:08X}u, gen::{}}},\n", f, fn_name(f));
            files[idx++ % kFiles] += r.code + "\n";
        }
        std::filesystem::create_directories(dir);
        const auto write = [&](const char* name, const std::string& text) {
            std::ofstream out(dir / name, std::ios::binary);
            out << "// Generated by qilin_difftest_gen: the differential test's cases (synthetic code, no game data).\n" << text;
            if (!out) throw std::runtime_error(std::format("cannot write {}", name));
        };
        write("functions.hpp", "#pragma once\n#include \"guest.hpp\"\n\nnamespace qilin::gen {\n" + decls + "}  // namespace qilin::gen\n");
        for (unsigned i = 0; i < kFiles; ++i)
            write(std::format("lifted_{}.cpp", i).c_str(), "#include \"functions.hpp\"\n\nnamespace qilin::gen {\n\n" + files[i] + "}  // namespace qilin::gen\n");
        write("table.cpp", "#include \"functions.hpp\"\n\nnamespace qilin::rt {\nextern const FnEntry g_functions[] = {\n" + table +
                               "};\nextern const size_t g_function_count = sizeof(g_functions) / sizeof(g_functions[0]);\n"
                               "extern const char g_lifted_sha256[] = \"synthetic\";\n}  // namespace qilin::rt\n");
        std::string cases = "#include \"cases.hpp\"\n\nnamespace qilin::difftest {\n\nextern const uint16_t kImage[] = {";
        for (size_t i = 0; i < b.image.size(); ++i) cases += std::format("{}0x{:04X},", i % 16 ? "" : "\n    ", b.image[i]);
        cases += std::format("\n}};\nextern const uint32_t kImageWords = {};\n\nextern const Case kCaseList[] = {{\n", b.image.size());
        for (size_t k = 0; k < kept.size(); ++k) {
            const uint32_t entry = b.labels[k].second;
            uint32_t insn = 0;
            switch (kept[k].kind) {
            case difftest::plain: insn = entry; break;
            case difftest::slot: case difftest::sz64: insn = entry + 2; break;
            case difftest::double_: insn = entry + 4; break;
            default: break;
            }
            cases += std::format("    {{difftest::Kind({}), 0x{:08X}u, 0x{:08X}u}},\n", static_cast<int>(kept[k].kind), entry, insn);
        }
        cases += std::format("}};\nextern const uint32_t kCaseCount = {};\n\n}}  // namespace qilin::difftest\n", kept.size());
        write("cases.cpp", cases);

        size_t total_refused = 0;
        for (const auto& [why, n] : refused) total_refused += n;
        std::println("difftest_gen: {} cases ({} instruction forms, {} branch programs), {} functions; {} encodings the lifter refuses:", kept.size(),
                     planned_per_op.size(), kPrograms, fns.size(), total_refused);
        for (const auto& [why, n] : refused) std::println("  {:5}  {}", n, why);
        return 0;
    } catch (const std::exception& e) {
        std::println(stderr, "difftest_gen: {}", e.what());
        return 1;
    }
}
