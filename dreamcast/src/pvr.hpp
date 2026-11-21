// The port's PowerVR2 and its system bus, at the hardware's addresses: what
// Kamui 2, which stays lifted, programs in place of the console's chip
// (docs/runtime.md, "Graphics"). Register names follow the console's
// documentation as KallistiOS's BSD-licensed headers give them.
#pragma once

#include <cstdint>

namespace dreamcast {

// Maps the registers and video memory on the bus.
void map_pvr();
// Video memory, 8 MB, in the 32-bit view's order (two 4 MB banks).
const uint8_t* vram();
// Video memory as the texture hardware addresses it (the 64-bit view): n
// bytes from `offset`.
void vram64_read(uint32_t offset, uint8_t* out, uint32_t n);
// A number that grows whenever anything in that range of the 64-bit view is
// written: a texture decoded at one value is still good at the same value.
uint64_t vram64_generation(uint32_t offset, uint32_t n);
// A register, as lifted code reads and writes it (any of its views' addresses).
uint32_t pvr_register(uint32_t a);
void set_pvr_register(uint32_t a, uint32_t v);
// The palette RAM (0x005F9000): 1,024 entries.
const uint32_t* pvr_palette();
// Sets a normal event's bit in the interrupt status (0x005F6900).
void raise_event(unsigned bit);
// An external event (0x005F6904): bit 1 is the sound chip's interrupt.
void raise_external_event(unsigned bit);

}  // namespace dreamcast
