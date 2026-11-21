// How the window shows the picture (display.hpp), apart from the display so
// the settings can name it without Vulkan's headers.
#pragma once

#include <cstdint>

namespace qilin::host {

// How the picture fills the window: whole multiples of the console's picture
// (the sharpest), the largest that keeps its shape, or the whole window.
enum class Scaling : uint8_t { integer, fit, stretch };
// What it's drawn through: as it is, or a CRT television's raster lines and
// phosphor stripes.
enum class Look : uint8_t { plain, crt };

struct Picture {
    Scaling scaling = Scaling::fit;
    bool smooth = true;       // bilinear; off: nearest pixel
    Look look = Look::plain;
    float scanlines = 0.6f;   // CRT: how dark the gaps between raster lines are, 0..1
    float mask = 0.3f;        // CRT: the phosphor stripes' strength, 0..1
    bool vsync = true;        // at the display's refresh, no tearing; off: the lowest latency
    // Fullscreen on a display whose refresh isn't a whole multiple of 60 Hz
    // (75, 144, 165 Hz): switch to the highest mode at the same resolution
    // that is (120 Hz on a 144 Hz monitor), so every frame is shown alike.
    bool match_refresh = true;
    // A G-Sync, FreeSync or VRR display with it on in the driver: no mode
    // switch and no locking to the refresh; the display follows the game's
    // 60 Hz. (No program can see the driver's setting, so it's a switch.)
    bool variable_refresh = false;
};

}  // namespace qilin::host
