// The Maple bus: the controllers (and, later, memory cards) the game's lifted
// Maple driver talks to. The driver writes a command table in RAM and starts
// the bus's DMA (MDST, 0x005F6C18, or every vertical blank with MDTSEL); the
// port answers each frame there and then from the host's input, writes the
// replies where the table says, and raises the DMA's end (normal event 12).
//
// Port A is the host's first pad and the keyboard, with a memory card; every
// pad has a Jump Pack;
// ports B to D the host's other pads, as they are connected (video.cpp).
#pragma once

#include <cstdint>

namespace dreamcast::maple {

// The host's controls for a port, in the Dreamcast controller's terms: the
// buttons (its bits, set = pressed), triggers 0-255, the stick 0-255 (128
// centred). From the window (video.cpp).
struct Controls {
    bool connected = false;
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0, x = 128, y = 128;
};
enum Button : uint16_t {
    kC = 1u << 0, kB = 1u << 1, kA = 1u << 2, kStart = 1u << 3, kUp = 1u << 4, kDown = 1u << 5, kLeft = 1u << 6, kRight = 1u << 7,
    kZ = 1u << 8, kY = 1u << 9, kX = 1u << 10, kD = 1u << 11,
};

// How hard the Jump Pack in a port's controller vibrates, 0 to 1.
float vibration(unsigned port);

// MDST written with 1.
void start();
// A vertical blank: runs the table if the bus is set to (MDTSEL).
void vblank();

}  // namespace dreamcast::maple
