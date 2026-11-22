// The Vulkan 1.3 renderer of the tile accelerator's scenes (ta.hpp): what
// the PowerVR2's ISP and TSP do with the lists, drawn with the host's GPU
// into an image the display shows.
//
// What it draws so far: the background plane, then the opaque, punch-through
// and translucent lists, each in the order the game sent it, with the
// hardware's depth compare (z = 1/w, larger nearer), culling, flat and
// Gouraud shading, the four texture shading instructions, the offset colour,
// the blend instructions, and textures decoded from video memory
// (texture.hpp) with their mip-maps, the modifier volumes (cheap shadows and
// the two-volume mode), table and vertex fog, the colour clamp and the user
// tile clip (the modifier volumes' too), and the translucent list in
// auto-sort mode sorted for each pixel (per-pixel lists and sort.comp, where
// the device has fragment-shader atomics; else triangle by triangle).
//
// It knows the registers and video memory only through `Source`, so its test
// draws scenes without the rest of the port.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "qilin/host/vulkan.hpp"
#include "ta.hpp"

namespace dreamcast::renderer {

struct Source {
    const ta::Scene* scene = nullptr;
    // The frame buffer drawn into: kept from render to render, as video
    // memory is (a frame can be rendered in parts, each into its own
    // rectangle), and known by its address in video memory.
    uint32_t frame_buffer = 0;
    uint32_t width = 640, height = 480;  // its size in the game's pixels
    // The rectangle this render writes (the frame buffer clip, inclusive):
    // cleared, then drawn; the rest of the frame buffer is kept.
    uint32_t clip_x0 = 0, clip_y0 = 0, clip_x1 = 639, clip_y1 = 479;
    // The background plane, as a strip of four vertices (count 0: none; the
    // picture is cleared to black).
    ta::Strip background{};
    ta::Vertex background_vertices[4]{};
    // Registers: PAL_RAM_CTRL, TEXT_CONTROL's stride, PT_ALPHA_REF, and the
    // palette RAM's 1,024 entries.
    uint32_t palette_format = 0, stride = 0, punch_alpha_ref = 0;
    // ISP_FEED_CFG's bit 0: the translucent list drawn in the order sent
    // (presort); clear, the hardware sorts it by depth (auto-sort).
    bool presort = true;
    // SHADOW_SCALE: bit 8 cheap shadows (the modifier volumes darken), bits
    // 7-0 how much (256ths of the colour kept).
    uint32_t shadow_scale = 0;
    // Table fog (a polygon's TSP fog modes 0 and 3): FOG_COL_RAM
    // (0x00RRGGBB), FOG_DENSITY and FOG_TABLE's 128 entries (0x005F8200);
    // vertex fog's colour, FOG_COL_VERT; the colour clamp's FOG_CLAMP_MAX
    // and FOG_CLAMP_MIN (0xAARRGGBB).
    uint32_t fog_colour = 0, fog_density = 0, fog_vertex_colour = 0;
    uint32_t clamp_max = 0xFFFFFFFFu, clamp_min = 0;
    uint32_t fog_table[128] = {};
    const uint32_t* palette = nullptr;
    // Video memory as textures are addressed (pvr.hpp).
    void (*read)(uint32_t offset, uint8_t* out, uint32_t n) = nullptr;
    uint64_t (*generation)(uint32_t offset, uint32_t n) = nullptr;
};

// The picture of a scene: in TRANSFER_SRC_OPTIMAL layout, its drawing
// submitted to the GPU's queue (what Display::present_image takes).
struct Picture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
};

// Table fog's value at depth `inv_w` (1/w), 0 to 255 (pvr.frag's
// table_fog, the same in C++): the coefficient, FOG_DENSITY (its high byte a
// 1.7 fixed-point mantissa, its low byte a signed power of 2) times 1/w,
// picks an entry by its exponent and the top 4 bits of its mantissa (16
// entries an octave from 1; below 1, entry 0's high byte; from 256, entry
// 127's low byte), and the value goes from the entry's high byte to its low
// byte by the mantissa's next bits. Our own definition, from what the
// emulator's pictures show (tools/dcref, --fog).
float fog_density(uint32_t reg);
float table_fog(float density, const uint32_t* table, float inv_w);

class Renderer {
public:
    // `scale`: host pixels per game pixel, each way (the internal resolution).
    Renderer(qilin::host::Gpu& gpu, uint32_t scale);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Picture draw(const Source& source);

    // Anisotropic filtering for the textures the game filters (bilinear or
    // trilinear): 1 (none, the console's) to the device's limit.
    void set_anisotropy(unsigned level);

    // The frame buffer at an address as last drawn: null image if none has
    // been drawn there.
    Picture picture(uint32_t frame_buffer) const;

    // The last frame buffer drawn, RGBA8 (tests, screenshots).
    std::vector<uint8_t> read_back();

    struct Stats {
        uint32_t draws = 0, triangles = 0, culled = 0, textures = 0, uploads = 0, dropped = 0;
    };
    const Stats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dreamcast::renderer
