// A reference interpreter for the SH-4 code the lifter lifts, written
// separately from the lifter: from the instruction set's definitions, on the
// raw instruction word's fields, with memory read and written byte by byte
// (little-endian) and the FPU's arithmetic done in double precision with the
// denormal flush of FPSCR.DN = 1 applied by hand. It shares with the lifter
// only the decoder's identification of the opcode and, for the five
// instructions whose results are definitions of the port's own (fmac, fsrra,
// fipr, ftrv, fsca: recomp/runtime/guest.hpp, measured against an emulator
// with tools/dcref), those definitions; what it checks there is the wiring.
//
// It exists only in the tests (qilin_difftest): the port runs no interpreter.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace qilin::ref {

struct State {
    uint32_t r[16] = {};
    uint32_t pr = 0, gbr = 0, mach = 0, macl = 0;
    uint32_t t = 0, s = 0, q = 0, m = 0;
    uint32_t fpul = 0, fpscr = 0x00040000u;
    uint32_t sr = 0x40000000u;  // SR but for T, S, Q and M: MD = 1, RB = BL = FD = 0, IMASK free
    uint32_t fr[16] = {}, xf[16] = {};  // the current and the back bank, as bit patterns
};

// How a run ended: by the return at call depth 0 (to `value`), or by a stop
// the lifted code also makes (kinds as rt::Stop's), or by an error of the
// test itself (an access outside the memory it set up, an instruction the
// reference doesn't know).
struct Outcome {
    enum Kind { returned, memory, bad_jump, bad_return, trap, not_lifted, unsupported, unregistered, error } kind = error;
    uint32_t pc = 0, value = 0;
    std::string why;  // for error
};

struct Machine {
    uint8_t* mem = nullptr;  // guest address a is mem[a - base]
    uint32_t base = 0, size = 0;
    // Whether an access is to memory the test set up (else it stops with
    // memory, as the lifted code's checked build does).
    std::function<bool(uint32_t, uint32_t)> allowed;
    // Whether a function starts at an address (an indirect call or jump
    // anywhere else stops with bad_jump, as the lifted code's table does).
    std::function<bool(uint32_t)> is_function;
};

// Runs from `entry` until the return at depth 0 (or a stop), at most `limit` instructions.
Outcome run(State& s, const Machine& m, uint32_t entry, unsigned limit = 10000);

}  // namespace qilin::ref
