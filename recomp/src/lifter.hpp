// Lifter: SH-4 code -> C++23, one function per guest function.
//
// Slice 1 covers the integer instruction set (arithmetic with T, division
// steps, multiplies into MAC, shifts and rotates, logic, every integer load
// and store, the GBR forms, SR's T/S/Q/M bits) and all control flow: delay
// slots, conditional and unconditional branches, calls (direct, through a
// literal-pool address, by loaded PR), returns (rts, and jmp through the saved
// return address), tail calls, SHC's switch tables and indirect calls and
// jumps through the generated function table. Floating point is slice 3: a
// function that uses it isn't lifted yet, and the reason is reported.
// Privileged instructions are never lifted (they belong to replaced code).
//
// Generated code holds the guest registers it uses in host locals and writes
// them back to the context (rt::Context) only around calls and returns, so
// the host compiler can keep them in registers. Every call sets PR and checks
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
#include <string>

#include "bootfile.hpp"
#include "cfg.hpp"

namespace qilin {

std::string fn_name(uint32_t addr);  // "fn_8C010080"

struct LiftResult {
    bool ok = false;
    std::string code;      // the function's C++ definition
    uint32_t fail_at = 0;  // the instruction that stopped it
    std::string why;       // e.g. "floating point (slice 3): fadd"
};

class Lifter {
public:
    Lifter(const BootFile& boot, const Cfg& cfg) : boot_(boot), cfg_(cfg) {}
    LiftResult lift(const Function& f) const;

private:
    const BootFile& boot_;
    const Cfg& cfg_;
};

}  // namespace qilin
