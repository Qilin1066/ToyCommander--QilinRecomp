// Hardware-access profile: the loads, stores and cache operations in each
// function whose address is a constant outside cached RAM.
//
// The Dreamcast has no operating system: Sega's libraries drive the hardware
// directly, through registers in area 0 (the system bus, Maple, GD-ROM, the
// PowerVR2, the sound chip), video memory, the tile accelerator's input and
// the SH-4's own registers. A function that does is one the runtime has to
// replace (or one whose accesses it has to take over), so this is part of
// drawing the lifted/replaced boundary.
//
// Addresses come from the register constants function discovery tracks
// (literal-pool loads, mova, immediates, copies, through the control flow).
// Accesses through pointers loaded from memory, or relative to GBR, aren't
// seen: the runtime's checked memory stops on those instead.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "cfg.hpp"

namespace qilin {

struct HwAccess {
    uint32_t pc = 0;
    uint32_t ea = 0;    // the address (the first byte)
    uint32_t size = 0;  // bytes: 1, 2, 4 or 8 (a 64-bit fmov); 32 for a cache-line operation
    enum Kind : uint8_t { load, store, modify, cache } kind = load;  // modify: read-modify-write (tas.b, movca.l)
};

// Where an address goes: "" for cached RAM (0x8C000000-0x8CFFFFFF), else a
// region or hardware block ("PowerVR2 registers", "store queues", ...).
// From the SH7750 memory map and the Dreamcast's (`docs/code-analysis.md`).
std::string_view hw_region(uint32_t ea);

// The accesses outside cached RAM at constant addresses in one function.
// A 64-bit fmov's size can't be known without FPSCR.SZ: it is reported as 4.
std::vector<HwAccess> hardware_accesses(const BootFile& boot, const Cfg& cfg, const Function& f);

}  // namespace qilin
