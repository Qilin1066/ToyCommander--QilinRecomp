// Booting the game: the boot file into guest RAM, then what Shinobi's
// start-up does, natively.
#pragma once

#include <cstdint>
#include <span>

#include "guest.hpp"

namespace toycommander {

// Copies the boot file to 0x8C010000 and runs the start-up: the stack's fill
// pattern, the C runtime's section set-up, Shinobi's initialisation, the
// game's main, Shinobi's exit. Throws rt::Stop where it can't go on.
void boot(qilin::rt::Context& c, std::span<const uint8_t> boot_file);

}  // namespace toycommander
