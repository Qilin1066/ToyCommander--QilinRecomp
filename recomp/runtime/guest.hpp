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

#include <bit>
#include <cstdint>
#include <cstring>

namespace qilin::rt {

static_assert(std::endian::native == std::endian::little, "the guest and the hosts are little-endian");

struct Context {
    uint32_t r[16] = {};
    uint32_t pr = 0, gbr = 0, mach = 0, macl = 0;
    uint32_t t = 0, s = 0, q = 0, m = 0;  // SR's T, S, Q and M bits, each 0 or 1
    uint32_t fpul = 0, fpscr = 0x00040000u;
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
    enum Kind : uint32_t { Memory, BadJump, BadReturn, Trap, NotLifted, Unsupported, Unregistered } kind;
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

inline uint32_t ld8(uint32_t a) {
    check(a, 1);
    return g_mem[a];
}
inline uint32_t ld16(uint32_t a) {
    check(a, 2);
    uint16_t v;
    std::memcpy(&v, g_mem + a, 2);
    return v;
}
inline uint32_t ld32(uint32_t a) {
    check(a, 4);
    uint32_t v;
    std::memcpy(&v, g_mem + a, 4);
    return v;
}
inline uint32_t ld8s(uint32_t a) { return static_cast<uint32_t>(static_cast<int8_t>(ld8(a))); }
inline uint32_t ld16s(uint32_t a) { return static_cast<uint32_t>(static_cast<int16_t>(ld16(a))); }
inline void st8(uint32_t a, uint32_t v) {
    check(a, 1);
    g_mem[a] = static_cast<uint8_t>(v);
}
inline void st16(uint32_t a, uint32_t v) {
    check(a, 2);
    const uint16_t h = static_cast<uint16_t>(v);
    std::memcpy(g_mem + a, &h, 2);
}
inline void st32(uint32_t a, uint32_t v) {
    check(a, 4);
    std::memcpy(g_mem + a, &v, 4);
}

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
inline uint32_t tasb(uint32_t a) {
    const uint32_t v = ld8(a);
    st8(a, v | 0x80u);
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
// the queue to memory or the tile accelerator: the runtime has to take that
// over (until it does, it stops).
inline void pref(uint32_t a, uint32_t pc) {
    if ((a & 0xFC000000u) == 0xE0000000u) unsupported(pc, a);
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

}  // namespace qilin::rt
