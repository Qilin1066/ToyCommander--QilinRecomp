// Function discovery and control flow.
//
// The boot file has no symbols, so functions are found from the code itself:
//
//  1. by recursive descent from the entry point, following branches, calls
//     (`bsr`, and `jsr @rN` where rN provably holds a literal-pool constant)
//     and constant tail jumps;
//  2. from function pointers: literal-pool values and aligned words of
//     unclassified bytes that point into the image are tried as entries, and
//     kept only if everything reachable from them decodes cleanly;
//  3. from the gaps left between functions ("orphans": code nothing refers
//     to by address, which static libraries bring along), under the same test.
//
// Every 16-bit word of the image ends up classified: code, a literal-pool
// entry, part of a jump table, padding, or unknown. Delay slots belong to
// their branch; SHC's `braf` switch tables are resolved with their bounds.
//
// Register constants are tracked per function by a forward dataflow whose
// values only ever fall from "this constant" to "unknown" where paths join,
// so an indirect target is resolved only if every path agrees on it. Calls
// are assumed to keep r8-r15 (the compiler's convention); the lifter must not
// rely on a resolved target without a run-time check until the differential
// test has shown the assumption holds.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bootfile.hpp"
#include "decoder.hpp"
#include "layout.hpp"

namespace qilin {

// The general registers an instruction writes, as a bit mask (bit n: rN):
// its destination and any post-increment or pre-decrement of an address.
uint16_t gpr_written(const sh4::Insn& insn);

enum class Kind : uint8_t {
    unknown,
    code,
    literal,     // read by a PC-relative mov.w or mov.l
    jump_table,  // 16-bit offsets of a braf switch
    padding,     // zero or nop words between classified things
    data,        // in a data region of the build's layout
};

enum class Origin : uint8_t {
    entry,    // the boot file's first byte
    call,     // target of a direct or resolved call
    tail,     // target of a resolved jmp
    pointer,  // an address found in a literal pool or in data
    orphan,   // unreferenced code in a gap
};
std::string_view name(Origin o);

struct Switch {
    uint32_t braf = 0;   // address of the braf
    uint32_t table = 0;  // address of the 16-bit offset table
    std::vector<uint32_t> targets;
};

struct Function {
    uint32_t entry = 0;
    Origin origin = Origin::entry;
    std::vector<uint32_t> insns;    // addresses, sorted; delay slots included
    std::vector<uint32_t> leaders;  // branch targets inside the function, sorted
    std::vector<std::pair<uint32_t, uint32_t>> literals;   // address, size (2 or 4), sorted
    std::map<uint32_t, uint32_t> calls;       // call site -> resolved target
    std::map<uint32_t, uint32_t> tail_jumps;  // jmp site -> resolved target
    std::vector<uint32_t> indirect_calls;     // jsr/bsrf sites with no constant target
    std::vector<uint32_t> indirect_jumps;     // jmp/braf sites with no constant target and no table
    std::vector<Switch> switches;
    std::map<uint32_t, uint32_t> computed_branches;  // braf site -> target, where the offset is a constant
    std::map<uint32_t, uint32_t> call_returns;       // jmp site that is a call (PR loaded by hand) -> where it returns
    std::vector<uint32_t> jmp_returns;               // jmp sites that return (through the saved return address), sorted
    std::vector<std::string> problems;        // for `qilin suspects`

    uint32_t end() const { return insns.empty() ? entry : insns.back() + 2; }
};

struct Rejected {
    uint32_t address = 0;
    Origin origin = Origin::pointer;
    std::string reason;
};

struct Cfg {
    std::map<uint32_t, Function> functions;
    std::vector<Kind> kind;       // one per 16-bit word of the image
    std::vector<uint32_t> owner;  // per word: entry of the first function that claimed it, or 0
    std::vector<Rejected> rejected;  // candidates that didn't decode (calls and tails are findings)
    uint32_t base = 0;

    Kind at(uint32_t a) const { return kind[(a - base) / 2]; }
};

// With a layout, words of its data regions are data from the start: no
// function is found in them, and a call or jump into one is reported.
Cfg analyse(const BootFile& boot, std::span<const Region> regions = {});

// The instructions control can reach next from `pc`, within `f`: a delayed
// branch's successor is its delay slot, and the slot's are the branch's
// targets. Calls return to the instruction after their slot; tail calls and
// returns leave the function.
std::vector<uint32_t> successors(const BootFile& boot, const Function& f, uint32_t pc);

// The general registers' constants on entry to each of a function's
// instructions (a delay slot sees the registers as its branch leaves them),
// by the same dataflow function discovery uses. An empty optional is "not a
// constant". GBR isn't tracked.
//
// PR, the return address, is tracked too, because hand-written library code
// calls by loading PR and jumping (sts pr,r2 ; lds r1,pr ; jmp @r0, which
// returns to r1) and returns by jumping through the saved value (jmp @r2).
struct RegConstants {
    std::array<std::optional<uint32_t>, 16> r{};
    std::optional<uint32_t> pr;  // a constant loaded into PR with lds
    uint16_t entry_pr = 0;       // registers holding the return address the function was entered with
    bool pr_is_entry = true;     // PR still holds that return address
    std::optional<uint32_t>& operator[](size_t i) { return r[i]; }
    const std::optional<uint32_t>& operator[](size_t i) const { return r[i]; }
    bool operator==(const RegConstants&) const = default;
};
std::map<uint32_t, RegConstants> register_constants(const BootFile& boot, const Cfg& cfg, const Function& f);

// Developer tools only (std::format).
std::string summary(const Cfg& cfg);
std::string to_json(const Cfg& cfg);
// unknown_bytes: in the regions that are lifted (all of the image without a layout).
std::string coverage(const Cfg& cfg, const BootFile& boot, std::span<const Region> regions, bool list, uint32_t& unknown_bytes);
std::string suspects(const Cfg& cfg);
// Every run of words of one kind and one owner: address, bytes, kind, owner.
std::string kind_map(const Cfg& cfg, const BootFile& boot);

}  // namespace qilin
