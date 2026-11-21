// The PowerVR2's texture formats (texture.hpp).
#include "texture.hpp"

#include <algorithm>
#include <cstring>

#include "guest.hpp"

namespace dreamcast::texture {

namespace rt = qilin::rt;

namespace {

[[noreturn]] void unsupported(const Params& p) { throw rt::Stop{rt::Stop::Unsupported, 0, p.tcw}; }

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
constexpr uint32_t x5(uint32_t v) { return v << 3 | v >> 2; }
constexpr uint32_t x6(uint32_t v) { return v << 2 | v >> 4; }
constexpr uint32_t x4(uint32_t v) { return v << 4 | v; }

constexpr uint32_t argb1555(uint32_t t) { return rgba(x5((t >> 10) & 31u), x5((t >> 5) & 31u), x5(t & 31u), t & 0x8000u ? 255u : 0u); }
constexpr uint32_t rgb565(uint32_t t) { return rgba(x5(t >> 11), x6((t >> 5) & 63u), x5(t & 31u), 255u); }
constexpr uint32_t argb4444(uint32_t t) { return rgba(x4((t >> 8) & 15u), x4((t >> 4) & 15u), x4(t & 15u), x4(t >> 12)); }
constexpr uint32_t argb8888(uint32_t t) { return rgba((t >> 16) & 255u, (t >> 8) & 255u, t & 255u, t >> 24); }

uint32_t texel16(uint32_t fmt, uint32_t t) {
    switch (fmt) {
    case kArgb1555: return argb1555(t);
    case kRgb565: return rgb565(t);
    default: return argb4444(t);
    }
}

uint32_t palette_entry(const Params& p, uint32_t index) {
    const uint32_t e = p.palette[index & 1023u];
    switch (p.palette_format & 3u) {
    case 0: return argb1555(e & 0xFFFFu);
    case 1: return rgb565(e & 0xFFFFu);
    case 2: return argb4444(e & 0xFFFFu);
    default: return argb8888(e);
    }
}

uint32_t rd16(const uint8_t* d, uint32_t texel) { return uint32_t(d[texel * 2]) | uint32_t(d[texel * 2 + 1]) << 8; }

// The texels of the mip levels under a square level of side s: 1 + 4 + ... + (s/2)^2.
constexpr uint32_t below(uint32_t s) { return (s * s - 1) / 3; }

// Where the level of side s starts, from the texture's address, in texels
// (bytes for VQ's indices, nibbles for 4-bit palette indices): a mip-mapped
// texture stores its levels from the smallest, 1x1, which is three texels
// in (a VQ texture's 1x1 is one index right after the codebook, and its
// level of side s has an index for each 2x2 block).
uint32_t level_start(const Params& p, uint32_t s) {
    if (!mipmapped(p.tcw)) return vq(p.tcw) ? 2048u : 0u;
    if (p.width != p.height) unsupported(p);  // mip-maps are square
    if (vq(p.tcw)) return s == 1 ? 2048u : 2048u + 1u + below(s / 2);
    return below(s) + 3u;
}

// The same in bytes, rounded down (a 4-bit 1x1 level starts mid-byte).
uint32_t level_offset(const Params& p, uint32_t s) {
    const uint32_t fmt = format(p.tcw), t = level_start(p, s);
    if (vq(p.tcw) || fmt == kPalette8) return t;
    if (fmt == kPalette4) return t / 2;
    return t * 2;
}

uint32_t clamp8(float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f)); }

// YUV422: texel pairs share U and V. The conversion is the hardware manual's.
void yuv_pair(uint32_t w0, uint32_t w1, uint32_t* out) {
    const float u = float(w0 & 255u) - 128.0f, v = float(w1 & 255u) - 128.0f;
    const float ys[2] = {float(w0 >> 8), float(w1 >> 8)};
    for (int k = 0; k < 2; ++k) {
        const float y = ys[k];
        out[k] = rgba(clamp8(y + 1.375f * v), clamp8(y - 0.34375f * u - 0.6875f * v), clamp8(y + 1.71875f * u), 255u);
    }
}

}  // namespace

uint32_t twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const uint32_t m = std::min(w, h);
    uint32_t i = 0;
    for (uint32_t bit = 0; (1u << bit) < m; ++bit) i |= ((y >> bit) & 1u) << (2 * bit) | ((x >> bit) & 1u) << (2 * bit + 1);
    // A rectangle is a row of squares along its longer side.
    if (w > h) i += (x / m) * m * m;
    else if (h > w) i += (y / m) * m * m;
    return i;
}

uint32_t bytes(const Params& p) {
    const uint32_t fmt = format(p.tcw);
    if (fmt == kBump || fmt == 7u) unsupported(p);
    if (vq(p.tcw)) {
        if (paletted(p.tcw) || fmt == kYuv422 || !twiddled(p.tcw)) unsupported(p);
        return level_offset(p, p.width) + p.width * p.height / 4;
    }
    if (stride_select(p.tcw)) {
        if (mipmapped(p.tcw) || p.stride == 0) unsupported(p);
        return p.stride * 32u * p.height * 2u;
    }
    if (fmt == kYuv422 && twiddled(p.tcw)) unsupported(p);
    const uint32_t texels = p.width * p.height;
    return level_offset(p, p.width) + (fmt == kPalette8 ? texels : fmt == kPalette4 ? texels / 2 : texels * 2);
}

uint32_t levels(const Params& p) {
    if (!mipmapped(p.tcw)) return 1;
    uint32_t n = 1;
    while ((1u << (n - 1)) < p.width) ++n;
    return n;
}

void decode(const Params& p, const uint8_t* data, uint32_t* out) { decode_level(p, data, 0, out); }

void decode_level(const Params& p, const uint8_t* data, uint32_t level, uint32_t* out) {
    const uint32_t fmt = format(p.tcw), w = std::max(p.width >> level, 1u), h = std::max(p.height >> level, 1u);
    if (level >= levels(p)) unsupported(p);
    const uint8_t* d = data + level_offset(p, w);
    if (vq(p.tcw)) {
        // The codebook: 256 blocks of four 16-bit texels, in twiddled order
        // ((0,0), (0,1), (1,0), (1,1) as x, y).
        uint32_t book[256][4];
        for (uint32_t e = 0; e < 256; ++e)
            for (uint32_t k = 0; k < 4; ++k) book[e][k] = texel16(fmt, rd16(data, e * 4 + k));
        if (w == 1) {  // a block's first texel
            out[0] = book[d[0]][0];
            return;
        }
        for (uint32_t y = 0; y < h; y += 2)
            for (uint32_t x = 0; x < w; x += 2) {
                const uint32_t* b = book[d[twiddle(x / 2, y / 2, w / 2, h / 2)]];
                out[y * w + x] = b[0], out[(y + 1) * w + x] = b[1], out[y * w + x + 1] = b[2], out[(y + 1) * w + x + 1] = b[3];
            }
        return;
    }
    if (fmt == kPalette4 || fmt == kPalette8) {
        if (!p.palette) unsupported(p);
        const uint32_t base = palette_base(p.tcw), half = level_start(p, w) & 1u;  // a 4-bit level that starts at a high nibble
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t i = twiddle(x, y, w, h);
                const uint32_t n = fmt == kPalette8 ? d[i] : (d[(i + half) / 2] >> (((i + half) & 1u) * 4)) & 15u;
                out[y * w + x] = palette_entry(p, base + n);
            }
        return;
    }
    const uint32_t row = stride_select(p.tcw) ? p.stride * 32u : w;  // texels stored per row
    if (fmt == kYuv422) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; x += 2) {
                uint32_t pair[2] = {rgba(0, 0, 0, 255), rgba(0, 0, 0, 255)};
                if (x + 1 < row) yuv_pair(rd16(d, y * row + x), rd16(d, y * row + x + 1), pair);
                out[y * w + x] = pair[0], out[y * w + x + 1] = pair[1];
            }
        return;
    }
    if (twiddled(p.tcw)) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) out[y * w + x] = texel16(fmt, rd16(d, twiddle(x, y, w, h)));
        return;
    }
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) out[y * w + x] = x < row ? texel16(fmt, rd16(d, y * row + x)) : 0u;
}

}  // namespace dreamcast::texture
