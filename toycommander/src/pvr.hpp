// The port's PowerVR2 and its system bus, at the hardware's addresses: what
// Kamui 2, which stays lifted, programs in place of the console's chip
// (docs/runtime.md, "Graphics"). Register names follow the console's
// documentation as KallistiOS's BSD-licensed headers give them.
#pragma once

#include <cstdint>

namespace toycommander {

// Maps the registers and video memory on the bus.
void map_pvr();
// Video memory, 8 MB, in the 32-bit view's order (two 4 MB banks).
const uint8_t* vram();
// A register, as lifted code reads and writes it (any of its views' addresses).
uint32_t pvr_register(uint32_t a);
void set_pvr_register(uint32_t a, uint32_t v);
// Sets a normal event's bit in the interrupt status (0x005F6900).
void raise_event(unsigned bit);

}  // namespace toycommander
