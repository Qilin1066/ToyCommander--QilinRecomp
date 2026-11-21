// qilin_difftest: every generated case, run lifted and on the reference
// interpreter from the same random states. Registers, T, S, Q, M, MACH, MACL,
// GBR, PR, FPUL, FPSCR, both FPU banks, where the code returned to, the stops
// (kind, instruction, value) and a window of memory must all agree.
//
// The lifted code runs with checked memory and the host's floating-point
// mode set as the runtime sets it (rt::host_fp_setup); the reference flushes
// denormals by hand and computes in double, so it doesn't depend on that mode.
#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "cases.hpp"
#include "decoder.hpp"
#include "functions.hpp"
#include "guest.hpp"
#include "lift_programs.hpp"
#include "ref.hpp"

using namespace qilin;
using difftest::Case;
using sh4::Op;
using sh4::Opnd;

namespace qilin::rt {
// The generated function table (table.cpp).
extern const FnEntry g_functions[];
extern const size_t g_function_count;
}  // namespace qilin::rt

namespace {

constexpr uint32_t kLo = 0x8C000000u, kSize = 0x00800000u;  // the guest memory both sides have
constexpr uint32_t kWin = 0x8C400000u, kWinSize = 0x4000u;   // what the cases may write
constexpr uint32_t kReturn = 0xDEAD0000u;
constexpr int kTrials = 48;

uint32_t u32(std::mt19937& rng) { return static_cast<uint32_t>(rng()); }

uint32_t value(std::mt19937& rng) {
    static const uint32_t special[] = {0, 1, 2, 31, 32, 33, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x8000u, 0xFFFF8000u, 0xFFu, 0x80u};
    return rng() % 4 == 0 ? special[rng() % std::size(special)] : u32(rng);
}
// FPU register contents: ordinary magnitudes, values exactly representable in
// both precisions, the edges, and raw bit patterns.
uint32_t fvalue(std::mt19937& rng) {
    static const uint32_t special[] = {0, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u,
                                       0xFFC00001u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0x4F000000u, 0xCF000000u, 0x3F000000u};
    switch (rng() % 6) {
    case 0: return special[rng() % std::size(special)];
    case 1: return u32(rng);
    default: return (u32(rng) & 0x807FFFFFu) | (127u - 20u + u32(rng) % 41u) << 23;
    }
}

// The instruction's memory operands point into the window, aligned to the
// access; the base and index of r0-indexed forms share the offset.
void steer(ref::State& s, const sh4::Insn& in, bool sz, std::mt19937& rng) {
    const sh4::Spec& sp = sh4::spec(in.op);
    uint32_t size = sh4::access_size(in);
    if ((sp.flags & sh4::kFloat) && sz && size == 4) size = 8;
    if (size == 0 || size > 8) size = size > 8 ? 32 : 4;
    const auto addr = [&] { return (kWin + 0x400u + u32(rng) % 0x3000u) & ~(size - 1); };
    const auto small = [&] { return (u32(rng) % 0x100u) & ~(size - 1); };
    for (const Opnd o : sp.operands) {
        switch (o) {
        case Opnd::at_N: case Opnd::at_inc_N: case Opnd::at_dec_N: case Opnd::at_disp_N4: s.r[in.n()] = addr(); break;
        case Opnd::at_M: case Opnd::at_inc_M: case Opnd::at_disp_M1: case Opnd::at_disp_M2: case Opnd::at_disp_M4: s.r[in.m()] = addr(); break;
        case Opnd::at_r0_N: s.r[0] = small(), s.r[in.n()] = addr(); break;
        case Opnd::at_r0_M: s.r[0] = small(), s.r[in.m()] = addr(); break;
        case Opnd::at_gbr1: case Opnd::at_gbr2: case Opnd::at_gbr4: s.gbr = addr() & ~0x3FFu; break;
        case Opnd::at_r0_gbr: s.gbr = addr(), s.r[0] = small(); break;
        default: break;
        }
    }
    if (in.op == Op::mac_l || in.op == Op::mac_w) s.r[in.n()] = addr(), s.r[in.m()] = addr();
}

bool is_nan(uint32_t v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu); }

// Arithmetic whose NaN results carry the host's bits (compared as a class).
bool arithmetic(Op op) {
    const uint32_t f = sh4::spec(op).flags;
    return (f & sh4::kFpPr) && op != Op::fabs && op != Op::fneg && op != Op::fldi0 && op != Op::fldi1;
}

const char* kKind[] = {"returned", "memory", "bad jump", "bad return", "trap", "not lifted", "unsupported", "unregistered", "error"};

}  // namespace

int main() {
    std::vector<uint8_t> mem_a(kSize), mem_b(kSize);
    const uint32_t image_end = test::kBase + difftest::kImageWords * 2;
    std::memcpy(mem_a.data() + (test::kBase - kLo), difftest::kImage, difftest::kImageWords * 2);
    std::memcpy(mem_b.data() + (test::kBase - kLo), difftest::kImage, difftest::kImageWords * 2);
    rt::g_mem = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mem_a.data()) - kLo);
    rt::g_regions[0] = {test::kBase, image_end};
    rt::g_regions[1] = {kWin, kWin + kWinSize};
    rt::g_region_count = 2;
    rt::host_fp_setup();
    const auto is_function = [](uint32_t a) {
        const rt::FnEntry* end = rt::g_functions + rt::g_function_count;
        const rt::FnEntry* it = std::lower_bound(rt::g_functions, end, a, [](const rt::FnEntry& e, uint32_t x) { return e.addr < x; });
        return it != end && it->addr == a;
    };
    const ref::Machine machine{mem_b.data(), kLo, kSize,
                               [&](uint32_t a, uint32_t n) {
                                   return (a >= test::kBase && uint64_t(a) + n <= image_end) || (a >= kWin && uint64_t(a) + n <= kWin + kWinSize);
                               },
                               is_function};
    std::mt19937 rng(18112026);
    std::vector<uint8_t> window(kWinSize);
    unsigned failures = 0, runs = 0, shown = 0, stops = 0;
    for (uint32_t k = 0; k < difftest::kCaseCount; ++k) {
        const Case& t = difftest::kCaseList[k];
        const sh4::Insn in = t.insn ? sh4::decode(static_cast<uint16_t>(difftest::kImage[(t.insn - test::kBase) / 2])) : sh4::Insn{};
        for (int trial = 0; trial < kTrials; ++trial, ++runs) {
            ref::State s;
            std::optional<uint32_t> pending_sr;
            for (uint32_t& r : s.r) r = value(rng);
            s.pr = kReturn, s.gbr = value(rng), s.mach = value(rng), s.macl = value(rng), s.fpul = value(rng);
            s.t = u32(rng) & 1, s.s = trial % 4 == 0 ? 1u : 0u, s.q = u32(rng) & 1, s.m = u32(rng) & 1;
            // SR: IMASK and the other free bits at random; for ldc, a value
            // that keeps the system bits half the time.
            s.sr = 0x40000000u | (u32(rng) & 0x000000F0u);
            if (t.insn && (in.op == Op::ldc_sr || in.op == Op::ldcl_sr) && trial % 2 == 0) {
                const uint32_t v = (u32(rng) & ~0x70008000u) | 0x40000000u;
                if (in.op == Op::ldc_sr) s.r[in.n()] = v;
                else pending_sr = v;
            }
            for (uint32_t& f : s.fr) f = fvalue(rng);
            for (uint32_t& f : s.xf) f = fvalue(rng);
            if (t.insn) steer(s, in, t.kind == difftest::sz64, rng);
            s.r[15] = kWin + 0x3800u;  // a stack, for the programs that call
            if (t.kind == difftest::loop) s.r[1] = 1 + u32(rng) % 20;
            if (t.kind == difftest::nowhere)
                do s.r[3] = u32(rng) & ~1u;
                while (is_function(s.r[3]) || is_function(t.entry + 4 + s.r[3]));
            for (uint8_t& b : window) b = static_cast<uint8_t>(rng());
            if (pending_sr && in.op == Op::ldcl_sr) std::memcpy(window.data() + (s.r[in.n()] - kWin), &*pending_sr, 4);
            pending_sr.reset();
            std::memcpy(mem_a.data() + (kWin - kLo), window.data(), kWinSize);
            std::memcpy(mem_b.data() + (kWin - kLo), window.data(), kWinSize);

            rt::Context c;
            std::memcpy(c.r, s.r, sizeof c.r);
            c.pr = s.pr, c.gbr = s.gbr, c.mach = s.mach, c.macl = s.macl, c.t = s.t, c.s = s.s, c.q = s.q, c.m = s.m, c.fpul = s.fpul, c.fpscr = s.fpscr, c.sr = s.sr;
            for (int i = 0; i < 16; ++i) c.fr[i] = std::bit_cast<float>(s.fr[i]), c.xf[i] = std::bit_cast<float>(s.xf[i]);
            ref::Outcome lifted{ref::Outcome::returned, 0, 0, {}};
            try {
                rt::call(c, t.entry, 0);
                lifted.value = c.ret;
            } catch (const rt::Stop& st) {
                lifted = {static_cast<ref::Outcome::Kind>(st.kind + 1), st.pc, st.value, {}};
            }
            const ref::Outcome want = ref::run(s, machine, t.entry);

            std::string diff;
            const auto cmp = [&](const char* what, uint32_t got, uint32_t expected) {
                if (got != expected) diff += std::format(" {}={:08X}/{:08X}", what, got, expected);
            };
            if (want.kind == ref::Outcome::error) diff += " reference: " + want.why;
            if (lifted.kind != want.kind) diff += std::format(" outcome={}/{}", kKind[lifted.kind], kKind[want.kind]);
            else if (lifted.kind != ref::Outcome::returned) cmp("stop pc", lifted.pc, want.pc), cmp("stop value", lifted.value, want.value);
            else cmp("returned to", lifted.value, want.value);
            // State is compared where the lifted code has written it back: on a
            // return, and at an indirect branch with no function behind it.
            if (lifted.kind == want.kind && want.kind != ref::Outcome::returned) ++stops;
            if (lifted.kind == want.kind && (want.kind == ref::Outcome::returned || want.kind == ref::Outcome::bad_jump)) {
                for (int i = 0; i < 16; ++i) cmp(std::format("r{}", i).c_str(), c.r[i], s.r[i]);
                cmp("pr", c.pr, s.pr), cmp("gbr", c.gbr, s.gbr), cmp("mach", c.mach, s.mach), cmp("macl", c.macl, s.macl);
                cmp("t", c.t, s.t), cmp("s", c.s, s.s), cmp("q", c.q, s.q), cmp("m", c.m, s.m), cmp("fpscr", c.fpscr, s.fpscr), cmp("sr", c.sr, s.sr);
                const bool nan_class = t.insn && arithmetic(in.op);
                const auto fcmp = [&](const char* what, uint32_t got, uint32_t expected) {
                    if (nan_class && is_nan(got) && is_nan(expected)) return;
                    cmp(what, got, expected);
                };
                fcmp("fpul", c.fpul, s.fpul);
                for (int i = 0; i < 16; ++i)
                    fcmp(std::format("fr{}", i).c_str(), std::bit_cast<uint32_t>(c.fr[i]), s.fr[i]),
                        fcmp(std::format("xf{}", i).c_str(), std::bit_cast<uint32_t>(c.xf[i]), s.xf[i]);
                for (uint32_t a = 0; a < kWinSize; ++a) {
                    const uint8_t x = mem_a[kWin - kLo + a], y = mem_b[kWin - kLo + a];
                    if (x != y) {
                        diff += std::format(" mem[{:08X}]={:02X}/{:02X}", kWin + a, x, y);
                        break;
                    }
                }
            }
            if (!diff.empty()) {
                ++failures;
                if (shown++ < 30) {
                    const std::string what = t.insn ? sh4::disasm(in, t.insn) : std::format("program at {:08X}", t.entry);
                    std::printf("MISMATCH case %u (kind %d) %s trial %d:%s\n", k, static_cast<int>(t.kind), what.c_str(), trial, diff.c_str());
                }
            }
        }
    }
    std::printf("difftest: %u cases, %u runs, %u of them ending at a stop both sides make; %u mismatches\n", difftest::kCaseCount, runs, stops,
                failures);
    return failures ? 1 : 0;
}
