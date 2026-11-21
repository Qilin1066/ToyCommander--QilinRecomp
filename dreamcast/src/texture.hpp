// The PowerVR2's textures, decoded for the renderer: from the bytes in video
// memory (as the texture hardware addresses them, the 64-bit view) and the
// texture control word to RGBA8. Plain code, without Vulkan, so the tests
// run it anywhere.
//
// The formats are the hardware's: 16-bit texels (ARGB1555, RGB565, ARGB4444,
// YUV422), 4- and 8-bit palette indices, in raster or twiddled (Morton) order,
// vector quantised (a 256-entry codebook of 2x2 blocks), with or without
// mip-maps (every level the texture stores). Anything else stops.
#pragma once

#include <cstdint>

namespace dreamcast::texture {

enum Format : uint32_t { kArgb1555 = 0, kRgb565 = 1, kArgb4444 = 2, kYuv422 = 3, kBump = 4, kPalette4 = 5, kPalette8 = 6 };

struct Params {
    uint32_t tcw = 0;             // the texture control word
    uint32_t width = 8, height = 8;  // from the TSP word's sizes
    uint32_t stride = 0;          // TEXT_CONTROL's stride (texels / 32), for stride textures
    uint32_t palette_format = 0;  // PAL_RAM_CTRL: 0 ARGB1555, 1 RGB565, 2 ARGB4444, 3 ARGB8888
    const uint32_t* palette = nullptr;  // the palette RAM's 1,024 entries (paletted formats)
};

constexpr uint32_t format(uint32_t tcw) { return (tcw >> 27) & 7u; }
constexpr bool paletted(uint32_t tcw) { return format(tcw) == kPalette4 || format(tcw) == kPalette8; }
constexpr bool mipmapped(uint32_t tcw) { return tcw >> 31; }
constexpr bool vq(uint32_t tcw) { return (tcw >> 30) & 1u; }
// Paletted textures are always twiddled (the bit is part of their palette selector).
constexpr bool twiddled(uint32_t tcw) { return paletted(tcw) || !((tcw >> 26) & 1u); }
constexpr bool stride_select(uint32_t tcw) { return !paletted(tcw) && ((tcw >> 25) & 1u) && ((tcw >> 26) & 1u); }
// Where the texture is in video memory (the 64-bit view).
constexpr uint32_t address(uint32_t tcw) { return (tcw & 0x1FFFFFu) << 3; }
// The palette entries a paletted texture uses: the first, and how many.
constexpr uint32_t palette_base(uint32_t tcw) {
    return format(tcw) == kPalette4 ? ((tcw >> 21) & 0x3Fu) << 4 : ((tcw >> 25) & 3u) << 8;
}
constexpr uint32_t palette_count(uint32_t tcw) { return format(tcw) == kPalette4 ? 16u : 256u; }

// The bytes of video memory the texture occupies, from its address, as far as
// the decoded level's end. Stops on a format the decoder doesn't know.
uint32_t bytes(const Params& p);

// The levels the texture has: 1, or down to 1x1 for a mip-mapped one.
uint32_t levels(const Params& p);

// Decodes `bytes(p)` bytes at `data` into width x height pixels, each
// R, G, B, A in memory order.
void decode(const Params& p, const uint8_t* data, uint32_t* out);
// The same for a level (0 the largest; level n is max(width >> n, 1)
// square).
void decode_level(const Params& p, const uint8_t* data, uint32_t level, uint32_t* out);

// The place of texel (x, y) in a twiddled w x h texture.
uint32_t twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

}  // namespace dreamcast::texture
