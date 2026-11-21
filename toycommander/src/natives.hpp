// The native code the port registers for guest functions it replaces.
#pragma once

#include <cstdint>

namespace toycommander {

// Registers every native replacement (rt::provide).
void provide_natives();
// The handler code put in Shinobi's exception table at VBR + `offset` (0x224:
// the graphics chip's level-6 interrupts, Kamui 2's), or 0.
uint32_t interrupt_handler(uint32_t offset);

}  // namespace toycommander
