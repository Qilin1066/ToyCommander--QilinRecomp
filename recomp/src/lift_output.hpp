// `qilin lift`'s output: the lifted C++ of a boot file, as files.
//
// functions.hpp declares every function the CFG found; table.cpp is the
// sorted function table indirect calls dispatch through; stubs.cpp holds a
// stub for every function not lifted (replaced ones call rt::replaced, the
// runtime's registry, which stops as unregistered if it has no native code;
// the rest stop as not lifted); lifted_NNN.cpp hold the lifted
// functions, 256 per file counted over every function found, so lifting more
// changes only the files that hold them (or a fixed number of files, for a
// build system that has to name them in advance); lift_report.txt says why each
// function that wasn't lifted wasn't. A file is rewritten only when its
// content changes, so a rebuild recompiles only what changed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bootfile.hpp"
#include "cfg.hpp"
#include "layout.hpp"

namespace qilin {

struct LiftOptions {
    std::optional<std::pair<uint32_t, uint32_t>> range;  // lift only functions starting in [lo, hi)
    bool orphans = true;                                  // lift unreferenced functions too (every function found is lifted or replaced)
    unsigned files = 0;  // lifted_NNN.cpp files: 0 for 256 functions each; else exactly this many (for build systems)
    std::span<const ReplacedFunction> replaced;  // functions replaced natively outside the replaced regions
    std::span<const NativeFootprint> natives;     // replaced functions whose natives' registers are known
};

struct LiftSummary {
    size_t lifted = 0, failed = 0, replaced = 0, unreferenced = 0, outside = 0;
    size_t written = 0, removed = 0;  // files
    std::map<std::string, std::vector<uint32_t>> failures;  // reason -> functions
    std::string report;               // lift_report.txt
};

// Functions in the layout's replaced regions get stubs; the rest are lifted.
// Without a layout, every function is lifted.
LiftSummary write_lifted(const BootFile& boot, const Cfg& cfg, std::span<const Region> regions, const std::filesystem::path& dir,
                         const LiftOptions& options, std::string_view version);

}  // namespace qilin
