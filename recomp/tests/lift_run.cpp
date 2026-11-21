// End-to-end lifter tests: runs the synthetic programs of lift_programs.hpp,
// lifted at build time by qilin_liftgen and compiled with the compiler under
// test, and compares registers, memory and stops with results computed here
// from the instructions' definitions in Hitachi's SH-4 manual. Built with
// QILIN_CHECKED_MEMORY, so a stray access stops instead of corrupting.
// No game data.
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "functions.hpp"
#include "guest.hpp"
#include "lift_programs.hpp"

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
    for (uint32_t i : {0u, 1u}) {
        Context c = fresh();
        c.r[4] = i, c.r[5] = g_p.data;
        if (run(c, "indirect")) eq("indirect", c.r[4], i == 0 ? 8u : 14u);
    }
    {
        Context c = fresh();
        c.r[4] = 40;
        if (run(c, "prcall")) eq("prcall", c.r[4], 42);
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

void test_stops() {
    // badjump: its jsr is the instruction after sts.l and mov.l.
    expect_stop("badjump", Stop::BadJump, g_p.entry.at("badjump") + 4, T::kBase + 0x4000);
    expect_stop("fpu", Stop::NotLifted, g_p.entry.at("fpu"), 0);
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
    }
    // An access outside the mapped regions stops, naming the instruction.
    reset_memory();
    Context c = fresh();
    c.r[4] = 0x0C000000u, c.r[5] = 1;
    try {
        qilin::rt::call(c, g_p.entry.at("memory"), 0);
        ++g_failures, std::printf("FAIL unmapped store returned\n");
    } catch (const Stop& s) {
        eq("unmapped", s.kind, Stop::Memory), eq("unmapped pc", s.pc, g_p.entry.at("memory") + 4), eq("unmapped address", s.value, 0x0C000000u);
    }
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
}

}  // namespace

int main() {
    g_p = T::build();
    g_host.resize(0x20000);
    qilin::rt::g_mem = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(g_host.data()) - kHostLo);
    qilin::rt::g_regions[0] = {kStackTop - kStackSize, T::kBase + static_cast<uint32_t>(g_p.image.size() * 2)};
    qilin::rt::g_region_count = 1;
    reset_memory();
    std::mt19937 rng(20261121);
    test_helpers();
    test_arith(rng);
    test_div(rng);
    test_bits(rng);
    test_mul(rng);
    test_control();
    test_stops();
    test_memory();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
