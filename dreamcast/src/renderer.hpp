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
#include <filesystem>
#include <memory>
#include <string>
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
    // The picture drawn wider than the game's own by this (1: as the game
    // draws it), to fill a screen wider than 4:3 when the game's camera has
    // been widened as much (game.hpp): the 3D scene is spread across, and the
    // 2D overlays drawn over it (the HUD, the menus' text: flat, in front)
    // keep their shape, centred.
    float widen = 1.0f;
};

// The picture of a scene: in TRANSFER_SRC_OPTIMAL layout, its drawing
// submitted to the GPU's queue (what Display::present_image takes).
struct Picture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    float widen = 1.0f;  // drawn this much wider than the game's 4:3 (Source::widen)
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

// A texture pack's file name for a texture `width` x `height` texels with
// hash `hash`: "256x128_0123456789abcdef.png"; and the hash in such a name
// (false if it isn't one).
std::string pack_name(uint32_t width, uint32_t height, uint64_t hash);
bool pack_hash(const std::string& name, uint64_t& hash);

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

    // Multisampling (MSAA), an enhancement: `samples` a pixel, 1 (none, the
    // console's), 2, 4 or 8, rounded down to what the device has. Polygons'
    // edges are smoothed; their insides, the textures and the translucent
    // list's per-pixel sort are as before. Nothing to do while unchanged.
    void set_msaa(unsigned samples);
    // The translucent list in auto-sort mode is sorted for each pixel where
    // the device can (fragment-shader atomics), else triangle by triangle
    // (by their mean depth): off, triangle by triangle everywhere (tests: the
    // fallback's path, on any device).
    void set_per_pixel_sort(bool on);
    unsigned msaa() const;  // what it is now, after the device's limit

    // Texture packs, an enhancement. Each texture is known by a hash of what
    // the game gave the hardware for it (its bytes in video memory, its
    // format, size and stride, its palette's colours), wherever it is.
    // `load` (empty: none): PNG files named as pack_name names them, in it or
    // in folders in it, each drawn in place of that texture, at any size
    // (mip-mapped as the original is, where its sides are powers of 2; read
    // on a thread of their own, see wait_texture_packs).
    // `dump` (empty: none): every texture decoded is saved there, once a
    // run, so named (a file already there is kept). Nothing to do while
    // unchanged; a change looks at every texture again when it's next drawn.
    // A replacement that can't be read, or two for one texture, stops the
    // program, naming them.
    void set_texture_packs(const std::filesystem::path& load, const std::filesystem::path& dump);
    // The load folder looked at again (files added, changed or removed), and
    // every texture with it when next drawn.
    void reload_texture_packs();
    // Replacements are read on a thread of their own, and a texture is drawn
    // as the game has it until its replacement is ready (a render never
    // waits for the disk): this waits until those asked for so far are read,
    // so the next render draws them (tests).
    void wait_texture_packs();

    // The frame buffer at an address as last drawn: null image if none has
    // been drawn there.
    Picture picture(uint32_t frame_buffer) const;

    // The last frame buffer drawn, RGBA8 (tests, screenshots).
    std::vector<uint8_t> read_back();

    struct Stats {
        uint32_t draws = 0, triangles = 0, culled = 0, textures = 0, uploads = 0, dropped = 0;
        uint32_t evicted = 0;  // textures let go, unused for a while (all the run's, as `textures` is)
        uint32_t cached = 0;   // textures held now
        uint32_t replaced = 0, dumped = 0;  // texture packs: decodes replaced, textures dumped (the run's)
        uint32_t pack_files = 0;            // replacements found in the load folder
    };
    const Stats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dreamcast::renderer
