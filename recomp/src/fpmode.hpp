// The FPSCR mode at every instruction: which floating-point unit setting
// each FPU instruction runs under.
//
// What an SH-4 FPU instruction does depends on FPSCR: PR picks single or
// double precision, SZ 32- or 64-bit fmov transfers, FR which register bank
// fr0-fr15 name, RM the rounding mode (nearest or towards zero) and DN
// whether denormals are flushed to zero. The lifter has to know PR, SZ, RM
// and DN at every FPU instruction that depends on them, or read FPSCR at run
// time there. FR it doesn't need: the lifted code keeps the two banks as two
// arrays and frchg swaps them, as the hardware does, so instructions always
// name the current bank. (FR is still tracked, for the reports.)
//
// They are tracked as known bits through fschg and frchg, through lds of a
// register into FPSCR, and through the registers the usual read-modify-write
// (sts fpscr,rN ; and/or masks ; lds rN,fpscr) passes through. A value saved
// with sts.l fpscr,@-rN comes back with the lds.l @rN+,fpscr that restores it
// (assuming, as compiled save/restore code does, that nothing overwrites the
// saved word in between).
//
// The analysis is interprocedural and context-sensitive: each function is
// analysed once per FPSCR mode it is entered in. Address-taken functions (a
// pointer to them exists outside code) and functions no analysed code calls
// are entered in the default mode, the one Shinobi's start-up loads
// (0x00040000: round to nearest, denormals flushed, single precision, 32-bit
// transfers, bank 0); others in the modes their callers call them in. A call
// goes on in its callee's exit mode for that entry; a call to a function with
// no analysis (replaced by native code, or reached indirectly) keeps the
// mode. Hand-written library code has other conventions (the matrix routines
// run in bank 1; double-precision helpers return with PR set), and this
// follows them. A function entered in more than one mode is lifted once per
// mode: its instructions are entry-dependent, each proven in every mode.
// What is reported as a problem: an address-taken function called or
// returning in another mode, an indirect call or jump in another mode, a join
// where paths arrive in different modes (the program's mode there depends on
// the path taken, and the lifter has to read FPSCR at run time from there
// on), and any FPU instruction whose mode bits aren't known.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cfg.hpp"

namespace qilin {

// What is known of a 32-bit value, bit by bit: a bit is known (mask, with
// its value), or equal to the same bit of FPSCR on the function's entry
// (smask), or to its inverse (smask and sinv), or unknown.
struct KnownBits {
    uint32_t mask = 0;   // known bits
    uint32_t value = 0;  // their values (0 elsewhere)
    uint32_t smask = 0;  // bits that follow the entry's
    uint32_t sinv = 0;   // ... inverted (0 elsewhere)
    bool known(uint32_t bits) const { return (mask & bits) == bits; }
    bool operator==(const KnownBits&) const = default;
};

// b's symbolic bits, given what their base is: bits the base knows become
// known, bits that follow the base's own entry stay symbolic.
KnownBits compose(const KnownBits& b, const KnownBits& base);
// Where two paths join: what both agree on.
KnownBits meet(const KnownBits& a, const KnownBits& b);

namespace fpscr {
inline constexpr uint32_t kRM = 0x00000003u;  // rounding mode: 0 nearest, 1 towards zero
inline constexpr uint32_t kDN = 1u << 18;     // denormals are zero
inline constexpr uint32_t kPR = 1u << 19;     // double precision
inline constexpr uint32_t kSZ = 1u << 20;     // 64-bit fmov
inline constexpr uint32_t kFR = 1u << 21;     // register bank
inline constexpr uint32_t kTracked = kRM | kDN | kPR | kSZ | kFR;
inline constexpr uint32_t kDefault = kDN;     // what Shinobi's start-up loads: 0x00040000
}  // namespace fpscr

struct FpModes {
    std::map<uint32_t, KnownBits> at;  // FPSCR's tracked bits on entry to each FPU instruction, concrete
    std::vector<std::string> problems;
    uint32_t fpu = 0;        // FPU instructions
    uint32_t unproven = 0;   // FPU instructions some of whose mode bits aren't known
    uint32_t entry_dependent = 0;  // ... that follow an entry bit its callers disagree on: lifted once per mode
    uint32_t joins = 0;      // places where paths arrive in different modes: the mode depends on the path
    KnownBits entry;         // the mode it is entered in
    std::optional<KnownBits> exit;  // the mode it returns in (none if it never returns)
    std::optional<KnownBits> summary_exit;  // unused since entry modes are concrete (kept for the JSON)
    uint32_t contexts = 0;   // the entry modes it is analysed in
    std::vector<std::pair<KnownBits, std::optional<KnownBits>>> per_context;  // each: entry mode, exit mode
    bool address_taken = false;
    std::map<uint32_t, KnownBits> calls;  // direct callee (or tail-call target) -> the mode it is called in
};

// Every function's modes, iterated over the call graph to a fixed point.
// Functions in regions the layout replaces aren't analysed: they are native
// at run time and don't change the mode, so calls to them return in it.
std::map<uint32_t, FpModes> fp_modes(const BootFile& boot, const Cfg& cfg, std::span<const Region> regions = {});

}  // namespace qilin
