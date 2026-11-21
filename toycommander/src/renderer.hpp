// The Vulkan 1.3 renderer of the tile accelerator's scenes (ta.hpp): what
// the PowerVR2's ISP and TSP do with the lists, drawn with the host's GPU
// into an image the display shows.
//
// What it draws so far: the background plane, then the opaque, punch-through
// and translucent lists, each in the order the game sent it, with the
// hardware's depth compare (z = 1/w, larger nearer), culling, flat and
// Gouraud shading, the four texture shading instructions, the offset colour,
// the blend instructions, and textures decoded from video memory
// (texture.hpp). What it doesn't yet, each named once on stderr when a scene
// asks for it: modifier volumes (shadows), fog, the translucent list's
// per-pixel sorting, mip-mapping, the user tile clip.
//
// It knows the registers and video memory only through `Source`, so its test
// draws scenes without the rest of the port.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "qilin/host/vulkan.hpp"
#include "ta.hpp"

namespace toycommander::renderer {

struct Source {
    const ta::Scene* scene = nullptr;
    uint32_t width = 640, height = 480;  // the scene's size in the game's pixels
    // The background plane, as a strip of four vertices (count 0: none; the
    // picture is cleared to black).
    ta::Strip background{};
    ta::Vertex background_vertices[4]{};
    // Registers: PAL_RAM_CTRL, TEXT_CONTROL's stride, PT_ALPHA_REF, and the
    // palette RAM's 1,024 entries.
    uint32_t palette_format = 0, stride = 0, punch_alpha_ref = 0;
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

class Renderer {
public:
    // `scale`: host pixels per game pixel, each way (the internal resolution).
    Renderer(qilin::host::Gpu& gpu, uint32_t scale);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Picture draw(const Source& source);

    // For tests: the last picture's pixels, RGBA8.
    std::vector<uint8_t> read_back();

    struct Stats {
        uint32_t draws = 0, triangles = 0, culled = 0, textures = 0, uploads = 0;
    };
    const Stats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace toycommander::renderer
