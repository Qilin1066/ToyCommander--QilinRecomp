// The player's pad, as the host layer reports it (display.hpp).
#pragma once

#include <cstdint>

namespace qilin::host {

// The pad's buttons, by an Xbox pad's names (any SDL gamepad maps onto them).
// The left stick counts as the D-pad past half way. The keyboard is read
// key by key (key_down), for the game's settings to bind.
enum PadButton : uint32_t {
    kPadUp = 1u << 0, kPadDown = 1u << 1, kPadLeft = 1u << 2, kPadRight = 1u << 3,
    kPadA = 1u << 4, kPadB = 1u << 5, kPadX = 1u << 6, kPadY = 1u << 7,
    kPadStart = 1u << 8, kPadBack = 1u << 9, kPadGuide = 1u << 10,
    kPadClick = 1u << 11,  // the mouse's left button
    kPadLB = 1u << 12, kPadRB = 1u << 13,  // the shoulder buttons
    kPadLT = 1u << 14, kPadRT = 1u << 15,  // the triggers, past half way
    kPadL3 = 1u << 16, kPadR3 = 1u << 17,  // the sticks, pressed in
};

}  // namespace qilin::host
