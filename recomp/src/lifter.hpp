// Lifter: SH-4 code -> C++23, one function per guest function.
//
// Slice 1 covers the integer instruction set (arithmetic with T, division
// steps, multiplies into MAC, shifts and rotates, logic, every integer load
// and store, the GBR forms, SR's T/S/Q/M bits) and all control flow: delay
// slots, conditional and unconditional branches, calls (direct, through a
// literal-pool address, by loaded PR), returns (rts, and jmp through the saved
// return address), tail calls, SHC's switch tables and indirect calls and
// jumps through the generated function table.
//
// Slice 3 covers the FPU, in the mode fpmode proves at each instruction:
// single and double precision, 32- and 64-bit fmov (with the back bank's XD
// registers), FPUL and FPSCR transfers, fschg, frchg (the banks are two
// arrays that swap). Denormals are flushed by the host, which the runtime
// sets up to do so (rt::host_fp_setup), so only code proven to run with
// FPSCR.DN = 1 and round to nearest is lifted. fmac, fipr, ftrv, fsca and
// fsrra aren't lifted until a clean source gives their exact results.
// Privileged instructions are never lifted (they belong to replaced code).
//
// Generated code holds the guest registers it uses in host locals and writes
// them back to the context (rt::Context) only around calls and returns, so
// the host compiler can keep them in registers. Only the registers the
// function's own code assigns are ever stored (the others hold what the
// context does). A direct call to a lifted function stores only those the
// callee and the ones it calls can read or change (its footprint, see
// link), and reloads only those they can change; any other call stores all
// it assigns and reloads all of them, and every return stores all it
// assigns. Every call sets PR and checks
// afterwards that the callee returned to the instruction after the call's
// delay slot, so a longjmp-style return stops instead of misbehaving.
//
// Literal-pool loads (mov.w and mov.l @(disp,PC), mova) become constants,
// read from the boot file when lifting: the game doesn't write its code.
//
// Anything the lifter can't translate exactly makes the whole function fail,
// with the instruction and the reason: a function is lifted whole or not at all.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "bootfile.hpp"
#include "cfg.hpp"
#include "fpmode.hpp"

namespace qilin {

std::string fn_name(uint32_t addr);  // "fn_8C010080"

// Registers as the lifted code holds them in locals: bits 0-15 r0-r15, 16-24
// T, PR, GBR, MACH, MACL, S, Q, M, FPUL; 32-47 fr0-fr15 of the current bank.
// (SR's other bits, FPSCR, the back bank and the return address are always
// in the context.)
using RegSet = uint64_t;
inline constexpr RegSet kIntRegs = 0x1FFFFFFu, kFloatRegs = 0xFFFFull << 32, kAllRegs = kIntRegs | kFloatRegs;

struct LiftResult {
    bool ok = false;
    std::string code;      // the function's C++ definition
    uint32_t fail_at = 0;  // the instruction that stopped it
    std::string why;       // e.g. "floating point (slice 3): fadd"
    // For link: the registers the function keeps in locals; those it can
    // read or change in the context itself (its locals; every float if it
    // swaps the banks; everything if it calls through the function table);
    // and the functions it calls or tail-calls directly, sorted.
    RegSet locals = 0, touches = 0;
    std::vector<uint32_t> callees;
    // The locals its own code assigns (a local it never assigns holds what
    // the context does at every call and return, so it's never stored); and
    // those it can change in the context itself (those it assigns; every
    // float if it swaps the banks; everything if it calls through the
    // function table).
    RegSet assigned = 0, writes = 0;
};

// A function's registers, its own and those of everything it calls
// directly (Lifter::footprints): those it may read or change in the
// context, and those of them it may change.
struct Footprint {
    RegSet touches = kAllRegs, writes = kAllRegs;
};

class Lifter {
public:
    // `modes`: fp_modes()'s result, the FPSCR mode at every FPU instruction.
    Lifter(const BootFile& boot, const Cfg& cfg, const std::map<uint32_t, FpModes>& modes) : boot_(boot), cfg_(cfg), modes_(modes) {}
    // Every call stores and reloads all the function's registers.
    LiftResult lift(const Function& f) const;
    // The code with its direct calls left for link to fill in.
    LiftResult lift_unlinked(const Function& f) const;

    // Fills in the direct calls of lift_unlinked's code: each stores the
    // caller's registers in the callee's footprint that the caller assigns,
    // and reloads those the callee may change (all of them for a callee
    // `footprints` doesn't have). A function's footprint is what it and
    // everything it calls directly touch (footprints computes it; a function
    // that isn't lifted touches everything): a register outside it the
    // callee neither reads nor changes in the context, so the caller's local
    // stays right without a store or reload; one it reads but never changes
    // is stored and not reloaded.
    static void link(LiftResult& r, const std::map<uint32_t, Footprint>& footprints);
    // Footprints of the functions lifted (entry -> its unlinked result), by
    // a fixed point over the calls (recursion included). `natives`: the
    // registers of functions that aren't lifted but replaced by natives
    // whose registers are known (layout.hpp's NativeFootprint), each taken
    // as changed.
    static std::map<uint32_t, Footprint> footprints(const std::map<uint32_t, LiftResult>& lifted, const std::map<uint32_t, RegSet>& natives = {});

private:
    const BootFile& boot_;
    const Cfg& cfg_;
    const std::map<uint32_t, FpModes>& modes_;
};

}  // namespace qilin
