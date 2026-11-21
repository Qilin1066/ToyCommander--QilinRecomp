// The layout of a supported boot file: its regions, what each holds, and
// whether the port lifts it.
//
// A Dreamcast boot file has no sections, so the layout is established by
// analysis (`docs/code-analysis.md` has the evidence for every boundary) and
// recorded here per build. Function discovery takes it as input: it finds no
// code in a data region, and a call into one is reported. Coverage is
// counted per region, and only lifted regions have to be fully accounted for.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace qilin {

enum class Plan : uint8_t {
    lift,       // recompiled with the game: every byte must be accounted for
    replace,    // replaced natively at its API: reported, not a gate
    undecided,  // libraries still to be attributed function by function
    data,       // not code
};
std::string_view name(Plan p);

struct Region {
    uint32_t begin = 0, end = 0;  // [begin, end)
    std::string_view name;
    Plan plan = Plan::undecided;
};

// The regions of a supported build, in address order, covering the whole
// image; empty for anything else.
std::span<const Region> layout(std::string_view sha256_hex);

// Functions outside the replaced regions that are replaced natively all the
// same (they reach hardware the port provides otherwise): the lifter emits a
// stub that goes through the runtime's registry, as for the replaced regions.
struct ReplacedFunction {
    uint32_t entry;
    std::string_view name;  // our name for it
    std::string_view why;
};
std::span<const ReplacedFunction> replaced_functions(std::string_view sha256_hex);

// An indirect call whose targets are a table in the image: `count` words from
// `table`, the non-zero ones being the functions the site at `site` (in
// `function`) can call. The FPSCR pass follows the mode into them.
struct IndirectTargets {
    uint32_t function, site, table, count;
    std::string_view why;
};
std::span<const IndirectTargets> indirect_targets(std::string_view sha256_hex);

}  // namespace qilin
