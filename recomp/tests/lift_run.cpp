// End-to-end lifter tests: runs the synthetic programs of lift_programs.hpp,
// lifted at build time by qilin_liftgen and compiled with the compiler under
// test, and compares registers, memory and stops with results computed here
// from the instructions' definitions in Hitachi's SH-4 manual. Built with
// QILIN_CHECKED_MEMORY, so a stray access stops instead of corrupting.
// No game data.
#include <bit>
#include <cinttypes>
#include <cmath>
#include <limits>
#include <optional>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "functions.hpp"
#include "guest.hpp"
#include "lift_programs.hpp"
#include "bus.hpp"
#include "memory.hpp"
#include "system.hpp"

namespace {

using qilin::rt::Context;
using qilin::rt::Stop;
namespace T = qilin::test;

int g_failures = 0, g_checks = 0;

void expect(bool ok, const char* what, uint64_t got, uint64_t want) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL %s: got 0x%" PRIX64 ", want 0x%" PRIX64 "\n", what, got, want);
}
void eq(const char* what, uint64_t got, uint64_t want) { expect(got == want, what, got, want); }

constexpr uint32_t kStackTop = T::kBase;           // the stack grows down from the image
constexpr uint32_t kStackSize = 0x1000;
constexpr uint32_t kHostLo = T::kBase - 0x10000u;  // the host buffer's guest address
constexpr uint32_t kReturn = 0xDEAD0000u;          // the return address every test calls with

std::vector<uint8_t> g_host;
T::Programs g_p;

void reset_memory() {
    std::fill(g_host.begin(), g_host.end(), uint8_t{0});
    std::memcpy(g_host.data() + (T::kBase - kHostLo), g_p.image.data(), g_p.image.size() * 2);
}

Context fresh() {
    Context c;
    c.r[15] = kStackTop;
    c.pr = kReturn;
    return c;
}

// Calls a program through the function table; true if it returned normally to kReturn.
bool run(Context& c, const char* name) {
    try {
        qilin::rt::call(c, g_p.entry.at(name), 0);
    } catch (const Stop& s) {
        std::printf("FAIL %s: stopped (kind %u at %08X, value %08X)\n", name, static_cast<unsigned>(s.kind), s.pc, s.value);
        ++g_failures;
        return false;
    }
    eq(name, c.ret, kReturn);
    eq("stack pointer restored", c.r[15], kStackTop);
    return c.ret == kReturn;
}

// Reference semantics, from the instruction definitions.
uint32_t ref_shad(uint32_t rn, uint32_t rm) {
    const int32_t s = static_cast<int32_t>(rm);
    if (s >= 0) return rn << (rm & 31);
    if ((rm & 31) == 0) return static_cast<int32_t>(rn) < 0 ? 0xFFFFFFFFu : 0;
    return static_cast<uint32_t>(static_cast<int32_t>(rn) >> ((~rm & 31) + 1));
}
uint32_t ref_shld(uint32_t rn, uint32_t rm) {
    const int32_t s = static_cast<int32_t>(rm);
    if (s >= 0) return rn << (rm & 31);
    if ((rm & 31) == 0) return 0;
    return rn >> ((~rm & 31) + 1);
}

void test_arith(std::mt19937& rng) {
    for (int k = 0; k < 2000; ++k) {
        const uint64_t a = (uint64_t(rng()) << 32 | rng()) >> (k % 64), b = (uint64_t(rng()) << 32 | rng()) >> ((k / 64) % 64);
        Context c = fresh();
        c.r[4] = static_cast<uint32_t>(a), c.r[5] = static_cast<uint32_t>(a >> 32), c.r[6] = static_cast<uint32_t>(b), c.r[7] = static_cast<uint32_t>(b >> 32);
        Context d = c, e = c;
        if (run(c, "add64")) eq("add64", uint64_t(c.r[1]) << 32 | c.r[0], a + b);
        if (run(d, "sub64")) eq("sub64", uint64_t(d.r[1]) << 32 | d.r[0], a - b);
        if (run(e, "neg64")) eq("neg64", uint64_t(e.r[1]) << 32 | e.r[0], 0 - a);
    }
}

void test_div(std::mt19937& rng) {
    const uint32_t edges[] = {0, 1, 2, 3, 7, 0x7FFFFFFF, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu, 12345, 0x10000};
    std::vector<std::pair<uint32_t, uint32_t>> cases;
    for (uint32_t a : edges)
        for (uint32_t b : edges) cases.emplace_back(a, b);
    for (int k = 0; k < 5000; ++k) cases.emplace_back(rng() >> (k % 32), rng() >> ((k / 32) % 32));
    for (const auto& [a, b] : cases) {
        if (b == 0) continue;
        Context c = fresh();
        c.r[4] = a, c.r[5] = b;
        if (run(c, "udiv")) eq("udiv", c.r[0], a / b);
        const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
        if (sa == INT32_MIN && sb == -1) continue;
        Context d = fresh();
        d.r[1] = a, d.r[0] = b;
        if (run(d, "sdiv")) eq("sdiv", d.r[1], static_cast<uint32_t>(sa / sb));
    }
}

void test_bits(std::mt19937& rng) {
    for (int k = 0; k < 3000; ++k) {
        const uint32_t a = rng(), b = k % 3 == 0 ? rng() : static_cast<uint32_t>(static_cast<int32_t>(rng() % 80) - 40);
        Context c = fresh();
        c.r[4] = a, c.r[5] = k % 7 == 0 ? (a & 0xFF00FF00u) | 0x00340012u : b;
        const uint32_t r5 = c.r[5];
        if (!run(c, "bits")) continue;
        eq("shad", c.r[0], ref_shad(a, r5));
        eq("shld", c.r[1], ref_shld(a, r5));
        // sett ; rotcl ; rotcr ; rotcr
        uint32_t r = a << 1 | 1, t = a >> 31;
        const uint32_t t2 = r & 1;
        r = r >> 1 | t << 31, t = t2;
        r = r >> 1 | t << 31;
        eq("rotcl/rotcr", c.r[2], r);
        bool same = false;
        for (int i = 0; i < 4; ++i) same |= ((a >> (8 * i)) & 0xFF) == ((r5 >> (8 * i)) & 0xFF);
        eq("cmp/str", c.r[3], same);
        eq("xtrct", c.r[6], a >> 16 | r5 << 16);
        eq("swap.b", c.r[7], (a & 0xFFFF0000u) | (a & 0xFF) << 8 | (a >> 8 & 0xFF));
    }
}

void test_mul(std::mt19937& rng) {
    for (int k = 0; k < 2000; ++k) {
        const uint32_t a = rng(), b = rng() >> (k % 32);
        Context c = fresh();
        c.r[4] = a, c.r[5] = b;
        if (!run(c, "mul")) continue;
        const uint64_t s = static_cast<uint64_t>(int64_t(int32_t(a)) * int64_t(int32_t(b))), u = uint64_t(a) * b;
        eq("dmuls.l", uint64_t(c.r[0]) << 32 | c.r[1], s);
        eq("dmulu.l", uint64_t(c.r[2]) << 32 | c.r[3], u);
        eq("mul.l", c.r[6], a * b);
        eq("muls.w", c.r[7], static_cast<uint32_t>(int32_t(int16_t(a)) * int32_t(int16_t(b))));
        eq("mulu.w", c.r[8], (a & 0xFFFF) * (b & 0xFFFF));
    }
}

void test_control() {
    for (uint32_t n = 1; n < 50; ++n) {
        Context c = fresh();
        c.r[4] = n;
        if (run(c, "loop")) eq("loop", c.r[0], n * (n + 1) / 2);
    }
    const uint32_t want[] = {10, 20, 30, 99, 99, 99};
    for (uint32_t i = 0; i < 6; ++i) {
        Context c = fresh();
        c.r[4] = i == 5 ? 0xFFFFFFFFu : i;
        if (run(c, "sw")) eq("switch", c.r[0], want[i]);
    }
    for (uint32_t x : {0u, 5u, 0x7FFFFFFFu}) {
        Context c = fresh();
        c.r[4] = x, c.r[14] = 0x14141414u;
        if (run(c, "calls")) eq("calls", c.r[4], 2 * (x + 1) + 100), eq("r14 preserved", c.r[14], 0x14141414u);
    }
    {
        Context c = fresh();
        if (run(c, "keep")) {
            eq("keep: r4", c.r[4], 0), eq("keep: r5", c.r[5], 4), eq("keep: r8 held across calls", c.r[8], 8);
            eq("keep: r9 set through a callee's callee", c.r[9], 50), eq("keep: r10 held across calls", c.r[10], 10);
        }
    }
    for (uint32_t i : {0u, 1u, 0u, 1u}) {  // each target twice: the second through rt::call's cache
        Context c = fresh();
        c.r[4] = i, c.r[5] = g_p.data;
        if (run(c, "indirect")) eq("indirect", c.r[4], i == 0 ? 8u : 14u);
    }
    {
        Context c = fresh();
        c.r[4] = 40;
        if (run(c, "prcall")) eq("prcall", c.r[4], 42);
    }
    {
        Context c = fresh();
        c.r[4] = 40;
        if (run(c, "prbra")) eq("calls by lds pr and bra", c.r[4], 44);
    }
    for (uint32_t x : {0u, 7u}) {
        Context c = fresh();
        c.r[4] = x;
        if (run(c, "farjmp")) eq("far branch inside a function", c.r[0], x == 0 ? 41u : 42u);
    }
}

void expect_stop(const char* name, Stop::Kind kind, uint32_t pc, uint32_t value) {
    Context c = fresh();
    try {
        qilin::rt::call(c, g_p.entry.at(name), 0);
        std::printf("FAIL %s: returned, want a stop\n", name);
        ++g_failures;
    } catch (const Stop& s) {
        eq(name, s.kind, kind);
        eq("stop pc", s.pc, pc);
        eq("stop value", s.value, value);
    }
}

// An interrupt pending while a loop runs is taken at its backward branch:
// the service runs once, sees the loop's stack pointer, may use any register,
// and the loop's result is as without it.
int g_serviced = 0;
uint32_t g_serviced_r15 = 0;
void test_interrupt_poll() {
    qilin::rt::g_irq_service = [](Context& c) {
        ++g_serviced, g_serviced_r15 = c.r[15];
        for (uint32_t& r : c.r) r = 0xDEADBEEFu;
    };
    Context c = fresh();
    c.r[4] = 10;
    const uint32_t sp = c.r[15];
    qilin::rt::g_irq_pending.store(true);
    if (run(c, "loop")) eq("loop with an interrupt", c.r[0], 55u);
    eq("interrupt taken once", static_cast<uint32_t>(g_serviced), 1u);
    eq("interrupt sees the stack pointer", g_serviced_r15, sp);
    eq("interrupt no longer pending", qilin::rt::g_irq_pending.load() ? 1u : 0u, 0u);
    qilin::rt::g_irq_service = nullptr;
}

// A literal the code rewrites is read from memory, not folded from the image.
void test_patched_literal() {
    Context c = fresh();
    if (run(c, "patch")) eq("a patched literal", c.r[0], 7u);
}

void test_stops() {
    // badjump: its jsr is the instruction after sts.l and mov.l.
    expect_stop("badjump", Stop::BadJump, g_p.entry.at("badjump") + 4, T::kBase + 0x4000);
    expect_stop("fpu", Stop::NotLifted, g_p.entry.at("fpu"), 0);
    expect_stop("hang", Stop::Hang, g_p.entry.at("hang"), 0);
    // An indirect call to an address with no function.
    Context c = fresh();
    try {
        qilin::rt::call(c, T::kBase + 2, 0x1234);
        ++g_failures, std::printf("FAIL call into a function's middle returned\n");
    } catch (const Stop& s) {
        eq("bad call", s.kind, Stop::BadJump), eq("bad call from", s.pc, 0x1234), eq("bad call target", s.value, T::kBase + 2);
    }
}

void test_memory() {
    for (uint32_t v : {0x12345678u, 0x89ABCDEFu, 0x00000080u}) {
        reset_memory();
        Context c = fresh();
        const uint32_t p = g_p.data + 16;
        c.r[4] = p, c.r[5] = v;
        if (!run(c, "memory")) continue;
        eq("mov.w @r", c.r[1], static_cast<uint32_t>(int32_t(int16_t(v))));
        eq("mov.l @r", c.r[2], v);
        eq("pre-decrement, post-increment", c.r[3], (v & 0xFFFF) | (v & 0xFF) << 16 | (v & 0xFF) << 24);
        eq("post-increment", c.r[12], p + 16);
        const uint8_t or_b = static_cast<uint8_t>(v | 0x80);
        eq("or.b @(r0,gbr), mov.b @(0,gbr)", c.r[7], static_cast<uint32_t>(int32_t(int8_t(or_b))));
        eq("gbr", c.gbr, p);
        const int16_t w = static_cast<int16_t>((v & 0xFF00) | or_b);
        eq("mac.w", c.r[10], static_cast<uint32_t>(int32_t(w) * int32_t(w)));
        eq("mac.w increments", c.r[8], p + 2), eq("mac.w increments", c.r[9], p + 2);
        eq("tas.b T", c.r[11], 1);
        eq("tas.b sets bit 7", g_host[p + 32 - kHostLo], 0x80);
        eq("tas.b address", c.r[4], p + 32);
        eq("mov.l r14,@-r14: the register after", c.r[14], p + 60);
        uint32_t stored;
        std::memcpy(&stored, g_host.data() + (p + 60 - kHostLo), 4);
        eq("mov.l r14,@-r14: the value before the decrement", stored, p + 64);
    }
    // An access outside the mapped regions stops, naming the instruction.
    reset_memory();
    Context c = fresh();
    c.r[4] = 0x00200000u, c.r[5] = 1;  // nothing is mapped there
    try {
        qilin::rt::call(c, g_p.entry.at("memory"), 0);
        ++g_failures, std::printf("FAIL unmapped store returned\n");
    } catch (const Stop& s) {
        eq("unmapped", s.kind, Stop::Memory), eq("unmapped pc", s.pc, g_p.entry.at("memory") + 4), eq("unmapped address", s.value, 0x00200000u);
    }
}

uint32_t u32(std::mt19937& rng) { return static_cast<uint32_t>(rng()); }
uint32_t fbits(float f) { return std::bit_cast<uint32_t>(f); }
float ffrom(uint32_t v) { return std::bit_cast<float>(v); }
// What a word of the scratch block holds.
uint32_t word_at(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, g_host.data() + (a - kHostLo), 4);
    return v;
}
void put_word(uint32_t a, uint32_t v) { std::memcpy(g_host.data() + (a - kHostLo), &v, 4); }

// Single precision, with exact expected bits where the host mustn't decide
// (denormals flushed, as FPSCR.DN = 1; IEEE rounding), and against the
// host's own arithmetic for random operands (the lifter's operand order,
// registers and results).
void test_farith(std::mt19937& rng) {
    const uint32_t in = g_p.data + 8, out = g_p.data + 24;
    struct Case {
        uint32_t a, b, c;
    };
    std::vector<Case> cases = {
        {fbits(1.0f), fbits(3.0f), fbits(2.0f)},
        {0x000116C2u /* a denormal, 1e-40 */, 0, 0x00000001u},
        {0x0DA24260u /* 1e-30 */, 0x2EDBE6FFu /* 1e-10 */, fbits(-1.0f)},
        {0x80000001u, fbits(0.0f), fbits(4.0f)},
        {fbits(2.0f), fbits(2.0f), fbits(0.0f)},
    };
    for (int k = 0; k < 2000; ++k) cases.push_back({u32(rng), u32(rng), u32(rng) & 0x7FFFFFFFu});
    const auto run_one = [&](const Case& x, uint32_t res[8], uint32_t& gt, uint32_t& eqf) {
        reset_memory();
        put_word(in, x.a), put_word(in + 4, x.b), put_word(in + 8, x.c);
        Context c = fresh();
        c.r[4] = in, c.r[5] = out;
        if (!run(c, "farith")) return false;
        for (unsigned k = 0; k < 8; ++k) res[k] = word_at(out + 4 * k);
        gt = c.r[1], eqf = c.r[2];
        eq("farith: r4 advanced", c.r[4], in + 12);
        return true;
    };
    for (size_t k = 0; k < cases.size(); ++k) {
        uint32_t r[8], gt, eqf;
        if (!run_one(cases[k], r, gt, eqf)) continue;
        // A NaN's bits are the host's (the SH-4's differ): compare NaNs as a class.
        const auto same = [&](const char* what, uint32_t got, float want) {
            const bool nan = std::isnan(ffrom(got)) && std::isnan(want);
            expect(nan || got == fbits(want), what, got, fbits(want));
        };
        const float a = ffrom(cases[k].a), b = ffrom(cases[k].b), cc = ffrom(cases[k].c);
        same("fadd", r[0], a + b), same("fsub", r[1], a - b), same("fmul", r[2], a * b), same("fdiv", r[3], a / b);
        same("fsqrt", r[4], std::sqrt(cc));
        eq("fneg", r[5], cases[k].a ^ 0x80000000u), eq("fabs", r[6], cases[k].b & 0x7FFFFFFFu);
        same("fldi1, fadd", r[7], 1.0f + a);
        eq("fcmp/gt", gt, a > b), eq("fcmp/eq", eqf, a == b);
        if (k == 0) {  // exact values
            eq("1/3", r[3], 0x3EAAAAABu), eq("sqrt 2", r[4], 0x3FB504F3u), eq("1-3", r[1], fbits(-2.0f));
        } else if (k == 1) {  // a denormal operand is zero; so is a denormal result
            eq("denormal + 0", r[0], 0), eq("denormal * 0", r[2], 0), eq("sqrt denormal", r[4], 0);
            eq("fcmp/eq denormal 0", eqf, 1), eq("fcmp/gt denormal 0", gt, 0);
            eq("fneg keeps the bits", r[5], 0x800116C2u);
        } else if (k == 2) {
            eq("1e-30 * 1e-10 flushed", r[2], 0);
        } else if (k == 3) {
            eq("-denormal + 0", r[0], 0), eq("fcmp/eq -denormal 0", eqf, 1);
        } else if (k == 4) {
            eq("2 == 2", eqf, 1), eq("2 > 2", gt, 0), eq("2 - 2", r[1], 0);
        }
    }
}

void test_ftrc() {
    for (const float v : {0.0f, 1.5f, -1.5f, 2147483520.0f, -2147483648.0f, 123456.75f, -0.99f}) {
        Context c = fresh();
        c.r[4] = fbits(v);
        if (!run(c, "ftrc")) continue;
        eq("ftrc", c.r[0], static_cast<uint32_t>(static_cast<int32_t>(v)));
        eq("float", c.r[1], fbits(static_cast<float>(static_cast<int32_t>(v))));
    }
    const uint32_t ftrc_pc = g_p.entry.at("ftrc") + 4;
    for (const float v : {2147483648.0f, -2147483904.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        Context c = fresh();
        c.r[4] = fbits(v);
        try {
            qilin::rt::call(c, g_p.entry.at("ftrc"), 0);
            ++g_failures, std::printf("FAIL ftrc out of range returned\n");
        } catch (const Stop& st) {
            eq("ftrc out of range", st.kind, Stop::Unsupported), eq("ftrc stop pc", st.pc, ftrc_pc);
        }
    }
}

void test_fpair() {
    const uint32_t in = g_p.data + 8, out = g_p.data + 24;
    // Patterns that aren't numbers: a moved word keeps its bits.
    const uint32_t w[4] = {0x7F800001u /* a signalling NaN */, 0x00000001u /* a denormal */, 0xFFC12345u, 0x3F800000u};
    reset_memory();
    for (unsigned k = 0; k < 4; ++k) put_word(in + 4 * k, w[k]);
    Context c = fresh();
    c.r[4] = in, c.r[5] = out;
    c.xf[2] = c.xf[7] = 123.0f;  // the back bank's other registers are left alone
    if (!run(c, "fpair")) return;
    const uint32_t want[7] = {w[2], w[3], w[0], w[1], w[2], w[1], w[0]};
    for (unsigned k = 0; k < 7; ++k) eq("fpair", word_at(out + 4 * k), want[k]);
    eq("fmov @r4+ (64-bit) advances by 8", c.r[4], in + 16);
    eq("FPSCR in the back bank", c.r[1], 0x00240000u), eq("FPSCR after", c.r[2], 0x00040000u), eq("c.fpscr", c.fpscr, 0x00040000u);
    eq("fr0", fbits(c.fr[0]), w[0]), eq("fr1", fbits(c.fr[1]), w[1]), eq("fr6", fbits(c.fr[6]), w[2]), eq("fr7", fbits(c.fr[7]), w[3]);
    eq("xf2", fbits(c.xf[2]), w[2]), eq("xf3", fbits(c.xf[3]), w[3]), eq("xf4", fbits(c.xf[4]), w[0]), eq("xf5", fbits(c.xf[5]), w[1]);
    eq("xf7 untouched", fbits(c.xf[7]), fbits(123.0f));
}

void test_fdouble(std::mt19937& rng) {
    std::vector<std::pair<float, float>> cases = {{1.0f, 3.0f}, {2.0f, 2.0f}, {-5.5f, 0.25f}, {1e30f, 1e-30f}};
    for (int k = 0; k < 1000; ++k) cases.emplace_back(ffrom(u32(rng) & 0xBFFFFFFFu), ffrom((u32(rng) & 0xBFFFFFFFu) | 0x00800000u));
    for (const auto& [a, b] : cases) {
        Context c = fresh();
        c.r[4] = fbits(a), c.r[5] = fbits(b);
        if (!run(c, "fdouble")) continue;
        const double da = a, db = b;
        eq("dadd", c.r[0], fbits(static_cast<float>(da + db)));
        eq("dmul", c.r[1], fbits(static_cast<float>(da * db)));
        eq("ddiv", c.r[2], fbits(static_cast<float>(da / db)));
        eq("dcmp/gt", c.r[3], da > db);
        const uint64_t q = std::bit_cast<uint64_t>(da / db);
        eq("dr4: the high half in fr4", c.r[6], static_cast<uint32_t>(q >> 32)), eq("dr4: the low half in fr5", c.r[7], static_cast<uint32_t>(q));
        eq("FPSCR restored", c.fpscr, 0x00040000u);
    }
    // 1/3 in double, exactly.
    Context c = fresh();
    c.r[4] = fbits(1.0f), c.r[5] = fbits(3.0f);
    if (run(c, "fdouble")) eq("1/3 (double)", uint64_t(c.r[6]) << 32 | c.r[7], 0x3FD5555555555555ull);
}

// The approximate instructions: the wiring (which registers, which operand
// order, the matrix's orientation) against the runtime's definitions, and
// values every definition must give exactly.
void test_approximate(std::mt19937& rng) {
    namespace rt = qilin::rt;
    const uint32_t in = g_p.data + 8, out = g_p.data + 160;
    const auto ordinary = [&] { return ffrom((u32(rng) & 0x807FFFFFu) | (127u - 10u + u32(rng) % 21u) << 23); };
    const auto run_with = [&](const char* name, const std::vector<float>& x, std::optional<uint32_t> r4) {
        reset_memory();
        for (size_t k = 0; k < x.size(); ++k) put_word(in + 4 * static_cast<uint32_t>(k), fbits(x[k]));
        Context c = fresh();
        c.r[4] = r4 ? *r4 : in, c.r[5] = out;
        return std::pair(run(c, name), c);
    };
    for (int k = 0; k < 300; ++k) {
        const float a = ordinary(), b = ordinary(), cc = ordinary();
        if (run_with("fmac", {a, b, cc}, {}).first) eq("fmac", word_at(out), fbits(rt::fmac(a, b, cc)));
        if (run_with("fsrra", {std::fabs(a)}, {}).first) eq("fsrra", word_at(out), fbits(rt::fsrra(std::fabs(a))));
        std::vector<float> v(8);
        for (float& x : v) x = ordinary();
        if (run_with("fipr", v, {}).first) eq("fipr", word_at(out), fbits(rt::fipr(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])));
    }
    // Exact values.
    if (run_with("fmac", {3.0f, 4.0f, 5.0f}, {}).first) eq("fmac 3*4+5", word_at(out), fbits(17.0f));
    // One rounding: (1 + 2^-12)^2 - 1 is 2^-11 + 2^-24, which a single holds;
    // rounding the square first (a tie, to even) would lose the 2^-24.
    if (run_with("fmac", {ffrom(0x3F800800u), ffrom(0x3F800800u), -1.0f}, {}).first) eq("fmac one rounding", word_at(out), 0x3A000400u);
    if (run_with("fmac", {ffrom(0x00000001u), 1.0f, 0.0f}, {}).first) eq("fmac denormal operand", word_at(out), 0);
    if (run_with("fsrra", {4.0f}, {}).first) eq("fsrra 4", word_at(out), fbits(0.5f));
    if (run_with("fipr", {1, 2, 3, 4, 5, 6, 7, 8}, {}).first) eq("fipr", word_at(out), fbits(70.0f));
    {
        // A matrix whose rows are distinct, so a transposed one gives other results:
        // row r is (r+1, 10(r+1), 100(r+1), 1000(r+1)), stored column-major.
        std::vector<float> m(20);
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) m[r + 4 * col] = static_cast<float>((r + 1) * (col == 0 ? 1 : col == 1 ? 10 : col == 2 ? 100 : 1000));
        m[16] = 1, m[17] = 2, m[18] = 3, m[19] = 4;
        const auto [ok, c] = run_with("ftrv", m, {});
        if (ok) {
            for (int r = 0; r < 4; ++r) eq("ftrv", word_at(out + 4 * static_cast<uint32_t>(r)), fbits(static_cast<float>(4321 * (r + 1))));
            eq("ftrv: the matrix stays in the back bank", fbits(c.xf[5]), fbits(20.0f));
        }
    }
    const struct {
        uint32_t angle;
        float s, c;
    } quarter[] = {{0, 0.0f, 1.0f}, {0x4000, 1.0f, 0.0f}, {0x8000, 0.0f, -1.0f}, {0xC000, -1.0f, 0.0f}, {0x12340000u, 0.0f, 1.0f}};
    for (const auto& q : quarter)
        if (run_with("fsca", {}, q.angle).first) eq("fsca sin", word_at(out) & 0x7FFFFFFFu, fbits(q.s) & 0x7FFFFFFFu), eq("fsca cos", word_at(out + 4), fbits(q.c));
    for (uint32_t k = 0; k < 65536; k += 97) {
        if (!run_with("fsca", {}, k).first) break;
        const double t = k * (6.283185307179586 / 65536.0);
        expect(std::abs(ffrom(word_at(out)) - std::sin(t)) <= 6e-8 && std::abs(ffrom(word_at(out + 4)) - std::cos(t)) <= 6e-8, "fsca accuracy", k, 0);
        float s0, c0;
        rt::fsca(k, s0, c0);
        eq("fsca", word_at(out), fbits(s0)), eq("fsca", word_at(out + 4), fbits(c0));
        float sn, cn;
        rt::fsca(65536 - k, sn, cn);
        if (k) eq("fsca odd", fbits(sn), fbits(s0) ^ 0x80000000u), eq("fsca even", fbits(cn), fbits(c0));
    }
}

// The registry of native replacements: a replaced function stops, naming
// itself and its caller, until native code is provided; then the call works.
void test_registry() {
    const uint32_t native = g_p.entry.at("native_me"), caller = g_p.entry.at("call_native");
    {
        Context c = fresh();
        try {
            qilin::rt::call(c, caller, 0);
            ++g_failures, std::printf("FAIL an unregistered function returned\n");
        } catch (const Stop& s) {
            eq("unregistered", s.kind, Stop::Unregistered), eq("unregistered: the function", s.pc, native);
            eq("unregistered: the caller's return address", s.value, caller + 6);
        }
    }
    static const qilin::rt::Provided natives[] = {
        {native, "native_me", [](qilin::rt::Context& c) { c.r[0] = 41; }},
        {g_p.entry.at("native_checked"), "native_checked", [](qilin::rt::Context& c) { c.r[0] = 41; }},
        {g_p.entry.at("native_bad"), "native_bad", [](qilin::rt::Context& c) { c.r[0] = 0, c.r[9] = 1; }},
    };
    qilin::rt::provide(natives);
    // A native with a known footprint: the caller's r8 held across the call
    // without a store, r0 reloaded; one that writes outside it stops.
    {
        Context k = fresh();
        if (run(k, "call_checked")) eq("a native footprint: r0 reloaded, r8 held", k.r[0], 49);
        Context b = fresh();
        try {
            qilin::rt::call(b, g_p.entry.at("native_bad"), 0);
            ++g_failures, std::printf("FAIL a native writing outside its footprint returned\n");
        } catch (const Stop& s) {
            eq("outside its footprint", s.kind, Stop::Footprint), eq("outside its footprint: the native", s.pc, g_p.entry.at("native_bad"));
            eq("outside its footprint: the register (r9)", s.value, 9);
        }
    }
    eq("native_name", qilin::rt::native_name(native) != nullptr, 1);
    Context c = fresh();
    if (run(c, "call_native")) eq("native code ran, and returned to its caller", c.r[0], 42);
    eq("function_containing", qilin::rt::function_containing(caller + 4), caller);
    // call_guest: from native code, with PR restored afterwards.
    Context d = fresh();
    d.pr = 0x12345678u;
    qilin::rt::call_guest(d, caller);
    eq("call_guest", d.r[0], 42), eq("call_guest restores PR", d.pr, 0x12345678u);
}

// The bus: RAM's other views, the store queues sent on by pref, a device.
void test_bus() {
    namespace rt = qilin::rt;
    reset_memory();
    // 64-byte aligned (data is 4-aligned: round up), so that bit 5 picks
    // store queue 0, the one this test fills and QACR0 directs.
    const uint32_t p = g_p.data + 64;
    const uint32_t block = (p + 63) & ~63u;
    rt::st32(block, 0x11223344u);
    eq("the uncached view", rt::ld32(block - 0x8C000000u + 0xAC000000u), 0x11223344u);
    eq("the physical view", rt::ld32(block - 0x8C000000u + 0x0C000000u), 0x11223344u);
    eq("a mirror", rt::ld32(block - 0x8C000000u + 0x8D000000u), 0x11223344u);
    rt::st16(block - 0x8C000000u + 0xAC000002u, 0xBEEFu);
    eq("a store through a view", rt::ld32(block), 0xBEEF3344u);
    // A store queue sent to RAM: QACR0 names area 3 (0x0C000000, bits 4-2 = 3).
    rt::st32(0xFF000038u, 0x0Cu);
    for (uint32_t k = 0; k < 32; k += 4) rt::st32(0xE0000000u + k, 0xA0000000u + k);
    rt::pref(0xE0000000u | (block & 0x03FFFFE0u), 0);
    eq("store queue target", rt::store_queue_target(0xE0000000u | (block & 0x03FFFFE0u)), (block & 0x03FFFFE0u) | 0x0C000000u);
    eq("store queue sent", rt::ld32(block + 28), 0xA000001Cu);
    // A device, and its block writes from a store queue.
    static uint32_t last_write = 0, blocks = 0;
    rt::map({0x10000000u, 0x10800000u, "test device", [](uint32_t a, unsigned) { return a ^ 0xFFFFFFFFu; },
             [](uint32_t, uint32_t v, unsigned) { last_write = v; }, [](uint32_t, const uint8_t*, unsigned n) { blocks += n; }});
    eq("device read (a P2 address)", rt::ld32(0xB0000010u), 0x10000010u ^ 0xFFFFFFFFu);
    rt::st32(0x10000020u, 7), eq("device write", last_write, 7);
    rt::st32(0xFF00003Cu, 0x10u);  // QACR1: area 4 (0x10000000)
    rt::pref(0xE0000020u, 0), eq("a store queue to a device", blocks, 32);
}

// Guest memory: 4 GB reserved, a range committed and usable.
void test_guest_memory() {
    qilin::rt::GuestMemory m;
    eq("reserve", m.reserve(), 1);
    eq("commit", m.commit(0x8C000000u, 16u << 20), 1);
    m.base()[0x8C000000u] = 1, m.base()[0x8CFFFFFFu] = 2;
    eq("committed memory", m.base()[0x8C000000u] + m.base()[0x8CFFFFFFu], 3);
}

// The runtime helpers on their own, at the edges.
void test_helpers() {
    using namespace qilin::rt;
    uint32_t rn = 0xFFFFFFFFu, t = 1;
    addc(rn, 0, t), eq("addc carry", rn, 0), eq("addc T", t, 1);
    rn = 0, t = 1, subc(rn, 0, t), eq("subc borrow", rn, 0xFFFFFFFFu), eq("subc T", t, 1);
    rn = 0, t = 0, negc(rn, 0, t), eq("negc 0", rn, 0), eq("negc T", t, 0);
    rn = 0, t = 0, negc(rn, 1, t), eq("negc 1", rn, 0xFFFFFFFFu), eq("negc T", t, 1);
    rn = 0x7FFFFFFFu, addv(rn, 1, t), eq("addv overflow", t, 1);
    rn = 0x80000000u, subv(rn, 1, t), eq("subv overflow", t, 1);
    rn = 1, addv(rn, 1, t), eq("addv no overflow", t, 0);
    for (uint32_t s : {0u, 1u, 31u, 32u, 0xFFFFFFFFu, 0xFFFFFFE0u, 0xFFFFFFE1u, 0x80000000u})
        for (uint32_t x : {1u, 0x80000001u, 0xFFFFFFFFu})
            eq("shad", shad(x, s), ref_shad(x, s)), eq("shld", shld(x, s), ref_shld(x, s));
    eq("cmp/str", cmpstr(0x11223344u, 0x55663377u), 1);
    eq("cmp/str none", cmpstr(0x11223344u, 0x55667788u), 0);
    // set_fpscr: a change of FR swaps the banks; exception enables stop.
    Context c;
    c.fr[0] = 1.0f, c.xf[0] = 2.0f;
    set_fpscr(c, 0x00240000u, 0), eq("set_fpscr FR", fbits(c.fr[0]), fbits(2.0f)), eq("set_fpscr value", c.fpscr, 0x00240000u);
    set_fpscr(c, 0x00340000u, 0), eq("set_fpscr same FR", fbits(c.fr[0]), fbits(2.0f));
    try {
        set_fpscr(c, 0x00040200u, 0x42);
        ++g_failures, std::printf("FAIL set_fpscr with an enable returned\n");
    } catch (const Stop& s) {
        eq("set_fpscr enable", s.kind, Stop::Unsupported), eq("set_fpscr enable pc", s.pc, 0x42);
    }
    eq("dbl", std::bit_cast<uint64_t>(dbl(ffrom(0x3FF00000u), ffrom(1))), 0x3FF0000000000001ull);
}

}  // namespace

int main() {
    g_p = T::build();
    g_host.resize(0x20000);
    qilin::rt::g_mem = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(g_host.data()) - kHostLo);
    qilin::rt::g_regions[0] = {kStackTop - kStackSize, T::kBase + static_cast<uint32_t>(g_p.image.size() * 2)};
    qilin::rt::g_region_count = 1;
    reset_memory();
    qilin::rt::host_fp_setup();
    std::mt19937 rng(20261121);
    test_helpers();
    test_arith(rng);
    test_div(rng);
    test_bits(rng);
    test_mul(rng);
    test_control();
    test_interrupt_poll();
    test_patched_literal();
    test_stops();
    test_memory();
    test_farith(rng);
    test_ftrc();
    test_fpair();
    test_fdouble(rng);
    test_approximate(rng);
    test_registry();
    test_guest_memory();
    test_bus();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
