// What lifted code needs at run time: the guest's register context, its
// memory, and the helpers the lifter's output calls. Header-only and runtime
// code: no std::format (see CMakeLists.txt on macOS 10.15).
//
// The SH-4 is little-endian like every host this port runs on, so memory is
// read and written as it is. Semantics follow Hitachi's SH-4 Software Manual;
// where an instruction's result in a corner the port can't pin down from a
// clean source would matter, the helper stops instead of guessing (mac.l and
// mac.w with S set).
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace qilin::rt {

static_assert(std::endian::native == std::endian::little, "the guest and the hosts are little-endian");

// SR's bits the port holds fixed: MD = 1, RB = 0, BL = 0, FD = 0.
inline constexpr uint32_t kSrSystem = 0x70008000u, kSrFixed = 0x40000000u, kSrTSQM = 0x00000303u;

struct Context {
    uint32_t r[16] = {};
    uint32_t pr = 0, gbr = 0, mach = 0, macl = 0;
    uint32_t t = 0, s = 0, q = 0, m = 0;  // SR's T, S, Q and M bits, each 0 or 1
    uint32_t fpul = 0, fpscr = 0x00040000u;
    // SR but for T, S, Q and M (held in their own fields): privileged mode,
    // register bank 0, exceptions unblocked, the FPU enabled, IMASK as the
    // code sets it (0: every interrupt allowed, as after Shinobi's set-up).
    uint32_t sr = kSrFixed;
    float fr[16] = {};  // the current bank
    float xf[16] = {};  // the back bank (frchg swaps them)
    uint32_t ret = 0;   // where the last function returned to (its rts's target)
};

// What stops lifted code: an invalid memory access, an unknown indirect
// target, a return somewhere unexpected, a trap, something not lifted or not
// supported. Thrown, so the runtime can name it and its caller, and a test
// can compare it.
struct Stop {
    // Unregistered: a replaced function the runtime has no native code for
    // yet (pc: the function, value: where it was called from).
    // Hang: the guest code stops itself on purpose, with a branch to itself
    // (how the game ends on a fatal error): reported, not spun on.
    // Footprint: a native changed a register outside its footprint
    // (replaced_within; pc: the function, value: the register's bit, as
    // the lifter's RegSet numbers them: 0-15 rN, 16-24 T PR GBR MACH MACL S
    // Q M FPUL, 32-47 frN).
    enum Kind : uint32_t { Memory, BadJump, BadReturn, Trap, NotLifted, Unsupported, Unregistered, Hang, Footprint } kind;
    uint32_t pc;     // guest address of the instruction (or function)
    uint32_t value;  // the address or value involved
};

[[noreturn]] inline void bad_jump(uint32_t pc, uint32_t target) { throw Stop{Stop::BadJump, pc, target}; }
[[noreturn]] inline void bad_return(uint32_t pc, uint32_t got) { throw Stop{Stop::BadReturn, pc, got}; }
[[noreturn]] inline void trap(uint32_t pc, uint32_t imm) { throw Stop{Stop::Trap, pc, imm}; }
[[noreturn]] inline void not_lifted(uint32_t fn) { throw Stop{Stop::NotLifted, fn, 0}; }
[[noreturn]] inline void unsupported(uint32_t pc, uint32_t value) { throw Stop{Stop::Unsupported, pc, value}; }
// A function of a replaced library with no native code yet; `from`: the return address it was called with.
[[noreturn]] inline void unregistered(uint32_t fn, uint32_t from) { throw Stop{Stop::Unregistered, fn, from}; }
// `bra` to itself: an endless loop with nothing in it but its delay slot.
// Always throws, but is defined out of line (dispatch.cpp) and not marked
// noreturn: a compiler that saw that would call everything after a call to
// the lifted function unreachable, which warnings-as-errors rejects (MSVC).
void hang(uint32_t pc);

// Guest memory: the 4 GB guest address space at a host base, `base + addr`.
// The runtime maps RAM at each address the game uses for it (0x8C000000 and
// its aliases) as views of one memory object.
inline uint8_t* g_mem = nullptr;

#ifdef QILIN_CHECKED_MEMORY
// Bring-up and tests: accesses outside the regions the host mapped stop.
struct Region {
    uint32_t lo, hi;
};
inline Region g_regions[16] = {};
inline unsigned g_region_count = 0;
inline uint32_t g_pc = 0;  // set by checked code before each instruction, for the report
inline void check(uint32_t a, uint32_t n) {
    for (unsigned i = 0; i < g_region_count; ++i)
        if (a >= g_regions[i].lo && uint64_t(a) + n <= g_regions[i].hi) return;
    throw Stop{Stop::Memory, g_pc, a};
}
#define QILIN_PC(a) (::qilin::rt::g_pc = (a))
#else
inline void check(uint32_t, uint32_t) {}
#define QILIN_PC(a) ((void)0)
#endif

// RAM where the game is linked to use it, 16 MB at 0x8C000000: the fast
// path, one compare. Everything else (RAM's other views, the store queues,
// the hardware) takes the bus (bus.cpp), which hands it to the devices the
// runtime maps there, or stops.
inline constexpr uint32_t kRamLo = 0x8C000000u, kRamBytes = 0x01000000u;
inline bool in_ram(uint32_t a, uint32_t n) { return a - kRamLo <= kRamBytes - n; }
uint32_t bus_read(uint32_t a, unsigned n);
void bus_write(uint32_t a, uint32_t v, unsigned n);

// Each with the base passed in (`mem`: g_mem), as lifted code calls them:
// it takes g_mem into a local once, on entry. A store through a byte
// pointer may change any memory, g_mem itself included, so with the global
// the compiler loads it again after every store; a local nothing can reach
// stays in a register. g_mem is set once, before any guest code runs.
// Native code calls the forms without it.
inline uint32_t ld8(const uint8_t* mem, uint32_t a) {
    if (!in_ram(a, 1)) [[unlikely]] return bus_read(a, 1);
    check(a, 1);
    return mem[a];
}
inline uint32_t ld16(const uint8_t* mem, uint32_t a) {
    if (!in_ram(a, 2)) [[unlikely]] return bus_read(a, 2);
    check(a, 2);
    uint16_t v;
    std::memcpy(&v, mem + a, 2);
    return v;
}
inline uint32_t ld32(const uint8_t* mem, uint32_t a) {
    if (!in_ram(a, 4)) [[unlikely]] return bus_read(a, 4);
    check(a, 4);
    uint32_t v;
    std::memcpy(&v, mem + a, 4);
    return v;
}
inline uint32_t ld8s(const uint8_t* mem, uint32_t a) { return static_cast<uint32_t>(static_cast<int8_t>(ld8(mem, a))); }
inline uint32_t ld16s(const uint8_t* mem, uint32_t a) { return static_cast<uint32_t>(static_cast<int16_t>(ld16(mem, a))); }
inline void st8(uint8_t* mem, uint32_t a, uint32_t v) {
    if (!in_ram(a, 1)) [[unlikely]] return bus_write(a, v & 0xFFu, 1);
    check(a, 1);
    mem[a] = static_cast<uint8_t>(v);
}
inline void st16(uint8_t* mem, uint32_t a, uint32_t v) {
    if (!in_ram(a, 2)) [[unlikely]] return bus_write(a, v & 0xFFFFu, 2);
    check(a, 2);
    const uint16_t h = static_cast<uint16_t>(v);
    std::memcpy(mem + a, &h, 2);
}
inline void st32(uint8_t* mem, uint32_t a, uint32_t v) {
    if (!in_ram(a, 4)) [[unlikely]] return bus_write(a, v, 4);
    check(a, 4);
    std::memcpy(mem + a, &v, 4);
}
inline uint32_t ld8(uint32_t a) { return ld8(g_mem, a); }
inline uint32_t ld16(uint32_t a) { return ld16(g_mem, a); }
inline uint32_t ld32(uint32_t a) { return ld32(g_mem, a); }
inline uint32_t ld8s(uint32_t a) { return ld8s(g_mem, a); }
inline uint32_t ld16s(uint32_t a) { return ld16s(g_mem, a); }
inline void st8(uint32_t a, uint32_t v) { st8(g_mem, a, v); }
inline void st16(uint32_t a, uint32_t v) { st16(g_mem, a, v); }
inline void st32(uint32_t a, uint32_t v) { st32(g_mem, a, v); }

// ---- arithmetic with T ----

// addc: rn = rn + rm + t, t = carry out.
inline void addc(uint32_t& rn, uint32_t rm, uint32_t& t) {
    const uint64_t sum = uint64_t(rn) + rm + t;
    rn = static_cast<uint32_t>(sum);
    t = static_cast<uint32_t>(sum >> 32);
}
// subc: rn = rn - rm - t, t = borrow out.
inline void subc(uint32_t& rn, uint32_t rm, uint32_t& t) {
    const uint64_t diff = uint64_t(rn) - rm - t;
    rn = static_cast<uint32_t>(diff);
    t = static_cast<uint32_t>(diff >> 63);
}
// negc: rn = 0 - rm - t, t = borrow out.
inline void negc(uint32_t& rn, uint32_t rm, uint32_t& t) {
    const uint64_t diff = 0 - uint64_t(rm) - t;
    rn = static_cast<uint32_t>(diff);
    t = static_cast<uint32_t>(diff >> 63);
}
// addv, subv: rn = rn op rm, t = signed overflow.
inline void addv(uint32_t& rn, uint32_t rm, uint32_t& t) {
    const uint32_t sum = rn + rm;
    t = ((~(rn ^ rm) & (rn ^ sum)) >> 31) & 1u;
    rn = sum;
}
inline void subv(uint32_t& rn, uint32_t rm, uint32_t& t) {
    const uint32_t diff = rn - rm;
    t = (((rn ^ rm) & (rn ^ diff)) >> 31) & 1u;
    rn = diff;
}

// div1: one step of the non-restoring division SHC's division sequences use,
// with SR's Q, M and T. Rn is shifted left with T coming in, then Rm is added
// to or subtracted from it according to Q and M; Q becomes the step's
// quotient sign, T whether Q equals M.
inline void div1(uint32_t& rn, uint32_t rm, uint32_t& q, uint32_t m, uint32_t& t) {
    const uint32_t old_q = q;
    q = rn >> 31;
    rn = (rn << 1) | t;
    const uint32_t before = rn;
    uint32_t carry;
    if (old_q == m) {
        rn -= rm;
        carry = rn > before;  // a borrow
    } else {
        rn += rm;
        carry = rn < before;  // a carry
    }
    // Q flips when the operation carried, relative to M and the old Q.
    q = (q ^ carry ^ m) & 1u;
    t = q == m ? 1u : 0u;
}

// shad, shld: shift rn by rm (left if rm >= 0, right by 32 - (rm & 31) otherwise;
// a right shift by 32 fills with the sign, or with zeros).
inline uint32_t shad(uint32_t rn, uint32_t rm) {
    const auto n = static_cast<int32_t>(rn);
    if (static_cast<int32_t>(rm) >= 0) return rn << (rm & 31u);
    if ((rm & 31u) == 0) return n < 0 ? 0xFFFFFFFFu : 0u;
    return static_cast<uint32_t>(n >> ((~rm & 31u) + 1u));
}
inline uint32_t shld(uint32_t rn, uint32_t rm) {
    if (static_cast<int32_t>(rm) >= 0) return rn << (rm & 31u);
    if ((rm & 31u) == 0) return 0u;
    return rn >> ((~rm & 31u) + 1u);
}

// cmp/str: T = whether any byte of rn equals the same byte of rm.
inline uint32_t cmpstr(uint32_t rn, uint32_t rm) {
    const uint32_t x = rn ^ rm;
    return ((x & 0xFF000000u) == 0 || (x & 0x00FF0000u) == 0 || (x & 0x0000FF00u) == 0 || (x & 0x000000FFu) == 0) ? 1u : 0u;
}

// tas.b: T = whether the byte was zero; bit 7 set.
inline uint32_t tasb(uint8_t* mem, uint32_t a) {
    const uint32_t v = ld8(mem, a);
    st8(mem, a, v | 0x80u);
    return v == 0 ? 1u : 0u;
}

// mac.l, mac.w with S clear: MAC += the signed product, as 64 bits. With S
// set the result saturates, by rules no source here pins down: stop.
inline void mac(uint32_t& mach, uint32_t& macl, int64_t product, uint32_t s, uint32_t pc) {
    if (s) unsupported(pc, 0);
    const uint64_t acc = (uint64_t(mach) << 32 | macl) + static_cast<uint64_t>(product);
    mach = static_cast<uint32_t>(acc >> 32);
    macl = static_cast<uint32_t>(acc);
}

// pref: a prefetch is a no-op, but a prefetch of a store-queue address sends
// that queue's 32 bytes on to memory or the tile accelerator (the bus does).
void bus_flush_store_queue(uint32_t a, uint32_t pc);  // bus.cpp
inline void pref(uint32_t a, uint32_t pc) {
    if ((a & 0xFC000000u) == 0xE0000000u) [[unlikely]] bus_flush_store_queue(a, pc);
}

// ---- the FPU ----
// FPU registers are floats; a double register DRn is the pair FRn (the high
// half) and FRn+1. Copies keep a float's bits on every 64-bit host (SSE and
// NEON moves don't touch them), so bit patterns that aren't numbers survive
// fmov, as on the SH-4.
inline uint32_t bits(float f) { return std::bit_cast<uint32_t>(f); }
inline float flt(uint32_t v) { return std::bit_cast<float>(v); }
inline double dbl(float hi, float lo) { return std::bit_cast<double>(uint64_t(bits(hi)) << 32 | bits(lo)); }
inline void set_dbl(float& hi, float& lo, double d) {
    const uint64_t v = std::bit_cast<uint64_t>(d);
    hi = flt(static_cast<uint32_t>(v >> 32));
    lo = flt(static_cast<uint32_t>(v));
}
inline float ldf(const uint8_t* mem, uint32_t a) { return flt(ld32(mem, a)); }
inline void stf(uint8_t* mem, uint32_t a, float f) { st32(mem, a, bits(f)); }
inline float ldf(uint32_t a) { return ldf(g_mem, a); }
inline void stf(uint32_t a, float f) { stf(g_mem, a, f); }
inline float fabs_bits(float f) { return flt(bits(f) & 0x7FFFFFFFu); }
inline float fneg_bits(float f) { return flt(bits(f) ^ 0x80000000u); }

// ftrc: truncation towards zero. Out of range or NaN the result depends on
// rules no clean source here pins down: stop.
inline uint32_t ftrc(double v, uint32_t pc) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) unsupported(pc, 0);
    return static_cast<uint32_t>(static_cast<int32_t>(v));
}

// ---- the instructions no clean source pins down exactly ----
// fmac, fsrra, fipr, ftrv and fsca are approximations on the hardware, within
// error bounds; their exact results aren't documented. These definitions are
// our own, chosen with tools/dcref, which runs them against a Dreamcast
// emulator (Flycast) over random operands: fmac, fsrra, fipr and ftrv agree
// with it on every input without a denormal; fsca doesn't (the emulator has a
// table of its own; these are the exact values). With FPSCR.DN = 1 a denormal
// operand is zero and a denormal result is flushed, as for every other FPU
// instruction (docs/recompiler-core.md); the emulator flushes results only.
inline float daz(float x) { return (bits(x) & 0x7F800000u) ? x : flt(bits(x) & 0x80000000u); }

// fmac: FR0 * FRm + FRn, rounded once.
inline float fmac(float a, float b, float c) { return daz(std::fmaf(daz(a), daz(b), daz(c))); }
// fsrra: 1 / sqrt(FRn), as a division of the rounded square root.
inline float fsrra(float x) { return 1.0f / std::sqrt(x); }
// fipr: the dot product of two vectors, summed in double and rounded once.
inline float fipr(float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3) {
    const auto d = [](float x) { return static_cast<double>(daz(x)); };
    return daz(static_cast<float>(d(a0) * d(b0) + d(a1) * d(b1) + d(a2) * d(b2) + d(a3) * d(b3)));
}
// ftrv: XMTRX (the back bank, column-major: xf[0], xf[4], xf[8], xf[12] are
// its first row) times the vector, each element as fipr computes it.
inline void ftrv(const float (&xf)[16], float& v0, float& v1, float& v2, float& v3) {
    const float a = v0, b = v1, c = v2, d = v3;
    v0 = fipr(xf[0], xf[4], xf[8], xf[12], a, b, c, d);
    v1 = fipr(xf[1], xf[5], xf[9], xf[13], a, b, c, d);
    v2 = fipr(xf[2], xf[6], xf[10], xf[14], a, b, c, d);
    v3 = fipr(xf[3], xf[7], xf[11], xf[15], a, b, c, d);
}
// fsca: sine and cosine of 2 pi (FPUL & 0xFFFF) / 65536, from a table of the
// rounded exact values, built once: the first quarter wave computed, the rest
// by symmetry (so sin(-x) = -sin(x) and the quarter points are exact).
inline const float* sine_table() {
    static const float* table = [] {
        static float t[65536 + 16384];
        constexpr double kStep = 6.283185307179586476925286766559 / 65536.0;
        for (uint32_t k = 0; k <= 16384; ++k) t[k] = static_cast<float>(std::sin(static_cast<double>(k) * kStep));
        t[0] = 0.0f, t[16384] = 1.0f;
        for (uint32_t k = 1; k < 16384; ++k) t[32768 - k] = t[k];
        for (uint32_t k = 0; k < 32768; ++k) t[32768 + k] = flt(bits(t[k]) ^ (k ? 0x80000000u : 0u));
        for (uint32_t k = 0; k < 16384; ++k) t[65536 + k] = t[k];  // so cos(k) = t[k + 16384] needs no wrap
        return t;
    }();
    return table;
}
inline void fsca(uint32_t fpul, float& sin_out, float& cos_out) {
    const float* t = sine_table();
    const uint32_t k = fpul & 0xFFFFu;
    sin_out = t[k], cos_out = t[k + 16384];
}

// FPSCR's bits the lifted code relies on: FR (21), SZ (20), PR (19), DN (18), RM (1-0).
inline constexpr uint32_t kFpscrFR = 1u << 21, kFpscrSZ = 1u << 20, kFpscrEnables = 0x00000F80u;

// frchg: the banks swap. The lifted code stores its bank's registers before
// and reloads them after.
inline void frchg(Context& c) {
    std::swap(c.fr, c.xf);
    c.fpscr ^= kFpscrFR;
}
// lds to FPSCR: a change of FR swaps the banks. Exception enables would need
// traps the port doesn't model: stop.
inline void set_fpscr(Context& c, uint32_t v, uint32_t pc) {
    if (v & kFpscrEnables) unsupported(pc, v);
    if ((v ^ c.fpscr) & kFpscrFR) std::swap(c.fr, c.xf);
    c.fpscr = v;
}

// The host's floating-point mode lifted code needs: denormals flushed to zero
// in and out, as the SH-4 does with FPSCR.DN = 1 (the game never clears it;
// the lifter only lifts code proven to run with it set), round to nearest.
// Call once on every thread that runs lifted code.
inline void host_fp_setup() {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x8040u);  // FTZ, DAZ; RC = nearest
#elif defined(_M_ARM64)
    _WriteStatusReg(ARM64_FPCR, (_ReadStatusReg(ARM64_FPCR) & ~(3ll << 22)) | (1ll << 24));  // FZ; RMode = nearest
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (fpcr & ~(uint64_t(3) << 22)) | (uint64_t(1) << 24);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#else
#error "host_fp_setup: an x86-64 or ARM64 host is required"
#endif
}

// ---- interrupts ----
// The runtime's interrupts are taken at loops' backward branches, as a
// real interrupt would come at some instruction: generated code tests
// g_irq_pending there (one relaxed load) and calls poll_irq, which passes the
// stack pointer (the handler's frame goes below it) and runs g_irq_service.
// The service runs the handlers on the context; poll_irq then puts the whole
// context back, as an interrupt's return does, whatever the service left in
// it. The interrupted function relies on that: its own registers are its
// locals, and one it never assigns isn't reloaded (the lifter's assigned
// set). g_irq_waiting: something is pending that IMASK holds back; lowering
// IMASK makes it pending again.
inline std::atomic<bool> g_irq_pending{false};
inline bool g_irq_waiting = false;
inline void (*g_irq_service)(Context&) = nullptr;
inline void poll_irq(Context& c, uint32_t r15) {
    g_irq_pending.store(false, std::memory_order_relaxed);
    if (!g_irq_service) return;
    const Context saved = c;
    c.r[15] = r15;
    g_irq_service(c);
    c = saved;
}
#define QILIN_POLL_IRQ(r15) \
    do { \
        if (::qilin::rt::g_irq_pending.load(std::memory_order_relaxed)) [[unlikely]] ::qilin::rt::poll_irq(c, (r15)); \
    } while (0)

// ldc to SR: IMASK and the other free bits are kept (the runtime delivers
// interrupts only where IMASK allows them); a change of mode, register bank,
// exception blocking or the FPU's enable isn't something lifted code can
// follow: stop. T, S, Q and M go to their fields (the lifter does that).
inline void set_sr(Context& c, uint32_t v, uint32_t pc) {
    if ((v & kSrSystem) != kSrFixed) unsupported(pc, v);
    if (g_irq_waiting && ((v >> 4) & 15u) < ((c.sr >> 4) & 15u)) g_irq_pending.store(true, std::memory_order_relaxed);
    c.sr = v & ~kSrTSQM;
}

// ---- calls through the function table ----
using Fn = void (*)(Context&);
struct FnEntry {
    uint32_t addr;
    Fn fn;
};
// Calls the function at `target` (the generated table, sorted by address);
// stops if there is none. `from`: the instruction that called.
void call(Context& c, uint32_t target, uint32_t from);

// A function the port replaces natively (the stubs qilin lift writes for the
// replaced regions and functions call this): runs the native code registered
// for it, and returns to the caller as the guest code would (c.ret = PR);
// stops as unregistered, naming it and its caller, if there is none.
// Defined by the runtime (system.cpp).
void replaced(Context& c, uint32_t fn);
// The native code registered for `fn`; stops as unregistered (naming `from`,
// the return address it was called with) if there is none. Natives are only
// ever added, so a stub looks its native up once and keeps it.
Fn native_of(uint32_t fn, uint32_t from);
// Runs a replaced function's native `f` (native_of's) and returns to the
// caller as the guest code would.
inline void run_native(Context& c, Fn f) {
    f(c);
    c.ret = c.pr;
}
// The same for a native whose footprint the lifter knows (`regs`: the
// registers it may read or change, as the lifter's RegSet numbers them;
// tools/check/native_footprints.py works them out from its source): direct
// calls to it store and reload only those, so it is checked to have changed
// no other, and stops (Stop::Footprint) if it did. The stubs call
// run_native_within, which checks it in builds with QILIN_CHECKED_MEMORY
// (bring-up, the tests) and in a release build runs the native as
// run_native does (two snapshots of the registers a call cost more than a
// small native's work).
void replaced_within(Context& c, uint32_t fn, uint64_t regs);
void run_native_checked(Context& c, Fn f, uint32_t fn, uint64_t regs);
inline void run_native_within(Context& c, Fn f, [[maybe_unused]] uint32_t fn, [[maybe_unused]] uint64_t regs) {
#ifdef QILIN_CHECKED_MEMORY
    run_native_checked(c, f, fn, regs);
#else
    run_native(c, f);
#endif
}

}  // namespace qilin::rt
