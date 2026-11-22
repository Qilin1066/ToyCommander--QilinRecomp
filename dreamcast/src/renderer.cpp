// The Vulkan 1.3 renderer of the tile accelerator's scenes (renderer.hpp).
//
// A scene is turned into one vertex buffer of triangles and a list of draws
// on the CPU (strips expanded, culled and flat-shaded there, which keeps the
// pipelines few: one per blend state), then drawn in one pass with dynamic
// rendering. Depth, its write and its compare are dynamic state.
#include "renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <unordered_map>
#include <vector>

#include "guest.hpp"
#include "shaders.inc"
#include "texture.hpp"

namespace dreamcast::renderer {

using qilin::host::Buffer;
using qilin::host::check;
using qilin::host::Gpu;
using qilin::host::Image;
namespace rt = qilin::rt;

namespace {

constexpr VkFormat kColourFormat = VK_FORMAT_R8G8B8A8_UNORM;

struct HostVertex {
    float x, y, z;
    float u, v;
    float base[4], offset[4];
};

struct Push {
    float screen[4];
    uint32_t flags;
    float alpha_ref;
    uint32_t sort_word;  // sorted translucent draws: the blend factors (bits 31-29, 28-26), the draw's place (25-0)
    float unused;
    float clip[4];  // the user tile clip, scene pixels: x0, y0, x1, y1 (exclusive)
};

// The ISP/TSP instruction word and the TSP instruction word.
constexpr uint32_t depth_mode(uint32_t isp) { return isp >> 29; }
constexpr uint32_t cull_mode(uint32_t isp) { return (isp >> 27) & 3u; }
constexpr bool z_write_off(uint32_t isp) { return (isp >> 26) & 1u; }
constexpr uint32_t src_blend(uint32_t tsp) { return tsp >> 29; }
constexpr uint32_t dst_blend(uint32_t tsp) { return (tsp >> 26) & 7u; }
constexpr uint32_t fog_mode(uint32_t tsp) { return (tsp >> 22) & 3u; }
constexpr bool colour_clamp(uint32_t tsp) { return (tsp >> 21) & 1u; }
constexpr bool use_alpha(uint32_t tsp) { return (tsp >> 20) & 1u; }
constexpr bool ignore_texture_alpha(uint32_t tsp) { return (tsp >> 19) & 1u; }
constexpr uint32_t shading(uint32_t tsp) { return (tsp >> 6) & 3u; }
constexpr uint32_t u_size(uint32_t tsp) { return 8u << ((tsp >> 3) & 7u); }
constexpr uint32_t v_size(uint32_t tsp) { return 8u << (tsp & 7u); }
// Filter, clamp and flip, then the mip-map D adjust: what a sampler is made from.
constexpr uint32_t sampler_bits(uint32_t tsp) { return ((tsp >> 13) & 0x3Fu) | ((tsp >> 8) & 0xFu) << 6; }

// The stencil's use, for the opaque modifier volumes: bit 1 says the polygon
// seen at a pixel is one they affect (its PCW's bit 7, and in the two-volume
// mode its two volumes); bit 0 is flipped by each face of one volume in
// front of it (odd: inside), then moved into bit 2 (inside some volume)
// before the next volume; where bits 1 and 2 are set, the cheap shadow
// darkens, or the polygon's second volume is drawn.
enum class Stencil : uint8_t { off, mark_none, mark_shadow, volume, resolve, shade };

struct Draw {
    uint32_t first = 0, count = 0;  // vertices
    uint32_t pipeline = 0;          // the pipeline's key
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t flags = 0;
    uint32_t depth = 0;
    bool depth_write = false;
    Stencil stencil = Stencil::off;
    uint32_t clip = 0;  // the user tile clip's rectangle, a byte each (tiles), when the flags use it
    uint32_t sort_word = 0;  // pvr.frag's, for a sorted translucent draw
    bool same_state(const Draw& o) const {
        return pipeline == o.pipeline && set == o.set && flags == o.flags && depth == o.depth && depth_write == o.depth_write &&
               stencil == o.stencil && clip == o.clip && sort_word == o.sort_word;
    }
};

// A pipeline's key: bit 0 blending, 1-3 and 4-6 its factors, 7 punch-through,
// 8 no colour written (the modifier volumes).
constexpr uint32_t pipeline_key(bool blend, uint32_t src, uint32_t dst, bool punch) {
    return (blend ? 1u | src << 1 | dst << 4 : 0u) | (punch ? 0x80u : 0u);
}
constexpr uint32_t kVolumeKey = 0x100u;
// 9: the translucent list in auto-sort mode, onto the per-pixel lists (no colour written).
constexpr uint32_t kSortedKey = 0x200u;
// The per-pixel lists' nodes: 16 bytes each, after a header of 16 (count, capacity, width).
constexpr VkDeviceSize kNodeBytes = 16, kNodesHeader = 16;
constexpr uint32_t kNoNode = 0xFFFFFFFFu;
// A draw's flag (pvr.frag's bit 6): darken where inside a translucent modifier volume.
constexpr uint32_t kInVolumesFlag = 0x40u;
constexpr uint32_t kMaxVolumes = 64;  // pvr.frag's array
// A draw's flags (pvr.frag's): bit 7 table fog; 8 and 9 drop the pixels
// inside, or outside, the translucent modifier volumes (a two-volume
// polygon's first and second volume); 10-11 the user tile clip (2: drawn
// only inside it, 3: only outside); 12 vertex fog; 13 the fog table's second
// mode; 14 the colour clamp.
constexpr uint32_t kTableFogFlag = 0x80u, kOutsideVolumesFlag = 0x100u, kInsideVolumesFlag = 0x200u, kVertexFogFlag = 0x1000u,
                   kTableFog2Flag = 0x2000u, kClampFlag = 0x4000u;
constexpr uint32_t kClipShift = 10;
// The frame's buffer for pvr.frag, in floats: a header of 8, the table fog's
// colour (4), vertex fog's colour (4), the colour clamp's maximum (4) and
// minimum (4), the table fog's density (1, then 3 unused) and table (128
// words), then the translucent modifier volumes (8 each, kMaxVolumes of
// them), then their triangles.
constexpr size_t kFogColourAt = 8, kFogVertexColourAt = 12, kClampMaxAt = 16, kClampMinAt = 20, kFogDensityAt = 24, kFogTableAt = 28,
                 kVolumesAt = 156, kTrianglesAt = kVolumesAt + kMaxVolumes * 8;

VkBlendFactor blend_factor(uint32_t f, bool source) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return source ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;  // the other side's colour
    case 3: return source ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_DST_ALPHA;
    default: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

struct Texture {
    Image image;
    uint64_t generation = 0;
    uint64_t palette_hash = 0;
    uint32_t bytes = 0;
    uint64_t used = 0;  // the frame it was last checked in
    std::vector<std::pair<uint32_t, VkDescriptorSet>> sets;  // by sampler bits
};

uint64_t hash_words(const uint32_t* w, uint32_t n, uint64_t h) {
    for (uint32_t k = 0; k < n; ++k) h = (h ^ w[k]) * 0x100000001B3ull;
    return h;
}

}  // namespace

float fog_density(uint32_t reg) { return float(reg >> 8 & 0xFFu) / 128.0f * std::exp2(float(static_cast<int8_t>(reg & 0xFFu))); }

float table_fog(float density, const uint32_t* table, float inv_w) {
    const float coef = density * inv_w;
    if (!(coef >= 1.0f)) return float(table[0] >> 8 & 0xFFu);
    int index = 127;
    float frac = 1.0f;
    if (coef < 256.0f) {
        int e = 0;
        const float steps = (std::frexp(coef, &e) * 2.0f - 1.0f) * 16.0f;
        index = 16 * (e - 1) + int(steps), frac = steps - std::floor(steps);
    }
    const float hi = float(table[index] >> 8 & 0xFFu), lo = float(table[index] & 0xFFu);
    return hi + (lo - hi) * frac;
}

struct Renderer::Impl {
    Gpu& gpu;
    uint32_t scale;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;  // or D24 with S8, what the device has
    struct Target {
        Image colour;
        bool ready = false;  // drawn into at least once: its contents are kept
    };
    std::map<uint32_t, Target> targets;  // by frame buffer address
    Target* current = nullptr;
    Image depth{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer vertex_buffer{}, staging{};
    // The translucent modifier volumes, for the fragment shader's test
    // (pvr.frag): a header, the volumes, their triangles.
    Buffer volume_buffer{};
    std::vector<float> volume_data;
    VkDescriptorSetLayout volume_layout = VK_NULL_HANDLE;
    VkDescriptorPool volume_pool = VK_NULL_HANDLE;
    VkDescriptorSet volume_set = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> set_pools;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, frag_punch = VK_NULL_HANDLE, frag_sorted = VK_NULL_HANDLE;
    // The translucent list's per-pixel sort (auto-sort mode), where the device
    // has fragment-shader atomics: the lists' heads (one a pixel) and nodes,
    // the node count read back to make the lists longer when a frame wanted
    // more, and sort.comp, which sorts and blends them into the picture.
    bool sorting = false;
    Buffer heads{}, nodes{}, node_count{};
    uint32_t node_capacity = 0;
    VkShaderModule sort_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout sort_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout sort_layout = VK_NULL_HANDLE;
    VkPipeline sort_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool sort_pool = VK_NULL_HANDLE;
    VkDescriptorSet sort_set = VK_NULL_HANDLE;
    // Bumped whenever a buffer or image the frame's set or the sort's set
    // names is made again: a new one can come back with the old one's handle,
    // so the sets are written again by this, not by comparing handles.
    uint64_t remade = 1, volume_set_written = 0, sort_set_written = 0;
    VkImageView sort_set_view = VK_NULL_HANDLE;  // the frame buffer the sort's set names (both stay alive: a switch rewrites it)
    std::unordered_map<uint32_t, VkPipeline> pipelines;
    std::unordered_map<uint32_t, VkSampler> samplers;
    std::unordered_map<uint64_t, Texture> textures;
    Texture white{};
    uint64_t frame = 0;
    Stats stats{};
    float anisotropy = 1.0f;  // for filtered textures (set_anisotropy)
    bool cheap = false;    // SHADOW_SCALE's bit 8: cheap shadows; clear, the two-volume mode
    bool opaque_volumes = false;  // this scene has opaque modifier volumes, drawn into the stencil
    bool translucent_volumes = false;  // and translucent modifier volumes for translucent polygons

    // The frame being built.
    std::vector<HostVertex> vertices;
    std::vector<Draw> draws;
    struct Upload {
        VkImage image;
        uint32_t width, height, levels;
        VkDeviceSize at;
        bool fresh;
    };
    std::vector<Upload> uploads;
    std::vector<uint8_t> upload_bytes, raw;
    std::vector<uint32_t> pixels;

    Impl(Gpu& g, uint32_t s) : gpu(g), scale(s) {}

    VkShaderModule module(const uint32_t* words, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = words;
        VkShaderModule m;
        check(vkCreateShaderModule(gpu.device, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    }

    void create() {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = gpu.family;
        check(vkCreateCommandPool(gpu.device, &pci, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(gpu.device, &cai, &cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(gpu.device, &fci, nullptr, &fence), "vkCreateFence");

        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sli.bindingCount = 1;
        sli.pBindings = &b;
        check(vkCreateDescriptorSetLayout(gpu.device, &sli, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
        // The frame's buffer (0), and the per-pixel lists' heads (1) and nodes (2).
        VkDescriptorSetLayoutBinding vb[3]{};
        for (uint32_t k = 0; k < 3; ++k) {
            vb[k].binding = k;
            vb[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            vb[k].descriptorCount = 1;
            vb[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        sli.bindingCount = 3;
        sli.pBindings = vb;
        check(vkCreateDescriptorSetLayout(gpu.device, &sli, nullptr, &volume_layout), "vkCreateDescriptorSetLayout");
        const VkDescriptorPoolSize vsize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
        VkDescriptorPoolCreateInfo vpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        vpi.maxSets = 1;
        vpi.poolSizeCount = 1;
        vpi.pPoolSizes = &vsize;
        check(vkCreateDescriptorPool(gpu.device, &vpi, nullptr, &volume_pool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo vai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        vai.descriptorPool = volume_pool;
        vai.descriptorSetCount = 1;
        vai.pSetLayouts = &volume_layout;
        check(vkAllocateDescriptorSets(gpu.device, &vai, &volume_set), "vkAllocateDescriptorSets");
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkDescriptorSetLayout set_layouts[2] = {set_layout, volume_layout};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = set_layouts;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(gpu.device, &pli, nullptr, &layout), "vkCreatePipelineLayout");

        vert = module(kPvrVert, sizeof kPvrVert);
        frag = module(kPvrFrag, sizeof kPvrFrag);
        frag_punch = module(kPvrFragPunch, sizeof kPvrFragPunch);
        create_sort();

        // Depth with a stencil: one of these two every device has.
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(gpu.physical, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) depth_format = VK_FORMAT_D24_UNORM_S8_UINT;

        // What untextured polygons sample (the shader has one sampler whatever it draws).
        white.image = Image::create(gpu, 1, 1, kColourFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }

    // The per-pixel sort's parts. Without fragment-shader atomics the
    // translucent list in auto-sort mode is sorted by triangle instead
    // (sort_translucent). The lists' buffers start small and are bound at
    // once (the frame's set names them whether or not a frame sorts).
    void create_sort() {
        sorting = gpu.fragment_atomics && !std::getenv("QILIN_SORT_BY_TRIANGLE");
        heads = Buffer::device_local(gpu, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        nodes = Buffer::device_local(gpu, kNodesHeader + kNodeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        node_count = Buffer::host_visible(gpu, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::memset(node_count.mapped, 0, 4);
        if (!sorting) return;
        frag_sorted = module(kPvrFragSorted, sizeof kPvrFragSorted);
        sort_module = module(kSortComp, sizeof kSortComp);
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t k = 0; k < 3; ++k) {
            b[k].binding = k;
            b[k].descriptorType = k == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[k].descriptorCount = 1;
            b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sli.bindingCount = 3;
        sli.pBindings = b;
        check(vkCreateDescriptorSetLayout(gpu.device, &sli, nullptr, &sort_set_layout), "vkCreateDescriptorSetLayout");
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &sort_set_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(gpu.device, &pli, nullptr, &sort_layout), "vkCreatePipelineLayout");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = sort_module;
        ci.stage.pName = "main";
        ci.layout = sort_layout;
        check(vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &ci, nullptr, &sort_pipeline), "vkCreateComputePipelines");
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(gpu.device, &dpi, nullptr, &sort_pool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = sort_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &sort_set_layout;
        check(vkAllocateDescriptorSets(gpu.device, &ai, &sort_set), "vkAllocateDescriptorSets");
    }

    // The lists for a picture of `picture_pixels`: the heads one a pixel, the nodes
    // as many as the last frame wanted (from its count, read back), at least
    // one a pixel (16 bytes: 4.9 MB at 1x, 315 MB at 8x before a scene asks
    // for more). Made again only when they must grow; a frame that wanted
    // more than there was loses its last fragments, once.
    void size_lists(uint32_t picture_pixels) {
        const uint32_t wanted = *static_cast<const uint32_t*>(node_count.mapped);
        uint32_t capacity = std::max(node_capacity, picture_pixels);
        if (wanted > capacity) capacity = wanted + wanted / 4;
        if (heads.size < VkDeviceSize{picture_pixels} * 4) {
            ++remade;
            heads.destroy(gpu);
            heads = Buffer::device_local(gpu, VkDeviceSize{picture_pixels} * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        if (capacity != node_capacity) {
            ++remade;
            nodes.destroy(gpu);
            nodes = Buffer::device_local(gpu, kNodesHeader + VkDeviceSize{capacity} * kNodeBytes,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            node_capacity = capacity;
        }
    }

    // The frame buffer's image (created, or created again larger: the display
    // may still be showing the old one, so the device goes idle first).
    Target& target(uint32_t key, uint32_t width, uint32_t height) {
        Target& t = targets[key];
        // Only ever grown: renders of one frame may see it a line taller or
        // shorter (Toy Racer's last line is inside one part's clip only), and
        // what the others drew must stay.
        const uint32_t w = std::max(width * scale, t.colour.width), h = std::max(height * scale, t.colour.height);
        if (!t.colour.image || t.colour.width != w || t.colour.height != h) {
            check(vkDeviceWaitIdle(gpu.device), "vkDeviceWaitIdle");
            t.colour.destroy(gpu);
            ++remade;
            t.colour = Image::create(gpu, w, h, kColourFormat,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         (sorting ? VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT) : VkImageUsageFlags(0)));
            t.ready = false;
        }
        if (!depth.image || depth.width < w || depth.height < h) {
            check(vkDeviceWaitIdle(gpu.device), "vkDeviceWaitIdle");
            depth.destroy(gpu);
            depth = Image::create(gpu, std::max(w, depth.width), std::max(h, depth.height), depth_format,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        }
        return t;
    }

    void wait() { check(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"); }

    VkPipeline pipeline(uint32_t key) {
        if (const auto it = pipelines.find(key); it != pipelines.end()) return it->second;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = key & 0x80u ? frag_punch : key & kSortedKey ? frag_sorted : frag;
        const VkVertexInputBindingDescription binding{0, sizeof(HostVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[4] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HostVertex, x)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HostVertex, u)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HostVertex, base)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HostVertex, offset)},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;  // culled on the CPU, by the hardware's rule
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;  // write and compare are dynamic
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (key & 1u) {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = blend_factor((key >> 1) & 7u, true);
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = blend_factor((key >> 4) & 7u, false);
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        if (key & (kVolumeKey | kSortedKey)) att.colorWriteMask = 0;
        const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                          VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                                          VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                          VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = uint32_t(std::size(dynamic));
        dy.pDynamicStates = dynamic;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &kColourFormat;
        ri.depthAttachmentFormat = depth_format;
        ri.stencilAttachmentFormat = depth_format;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &ri;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout;
        VkPipeline p;
        check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines");
        pipelines.emplace(key, p);
        return p;
    }

    VkSampler sampler(uint32_t bits) {
        if (const auto it = samplers.find(bits); it != samplers.end()) return it->second;
        // bits: 0-1 the filter, 2 clamp V, 3 clamp U, 4 flip V, 5 flip U, 6-9
        // the D adjust. Clamping wins over flipping.
        const auto mode = [&](bool clamp, bool flip) {
            return clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : flip ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = bits & 3u ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        // Mip-maps: the nearest level for point and bilinear filtering, both
        // levels blended for trilinear (filters 2 and 3, the hardware's two
        // passes); the D adjust (in quarters, 4 = 1) scales the level of
        // detail, a bias of its log2. A texture without levels has one.
        sci.mipmapMode = (bits & 3u) >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (const uint32_t d_adjust = (bits >> 6) & 0xFu) sci.mipLodBias = std::log2(float(d_adjust) / 4.0f);
        sci.addressModeU = mode(bits & 8u, bits & 32u);
        sci.addressModeV = mode(bits & 4u, bits & 16u);
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        // Anisotropic filtering, an enhancement: only where the game filters
        // (a point-sampled texture keeps its hard texels).
        if ((bits & 3u) && anisotropy > 1.0f) sci.anisotropyEnable = VK_TRUE, sci.maxAnisotropy = anisotropy;
        VkSampler s;
        check(vkCreateSampler(gpu.device, &sci, nullptr, &s), "vkCreateSampler");
        samplers.emplace(bits, s);
        return s;
    }

    VkDescriptorSet set_for(Texture& t, uint32_t bits) {
        for (const auto& [b, s] : t.sets)
            if (b == bits) return s;
        VkDescriptorSet set = VK_NULL_HANDLE;
        for (int attempt = 0; attempt < 2 && !set; ++attempt) {
            if (!set_pools.empty()) {
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = set_pools.back();
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &set_layout;
                if (vkAllocateDescriptorSets(gpu.device, &ai, &set) == VK_SUCCESS) break;
                set = VK_NULL_HANDLE;
            }
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024};
            VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pci.maxSets = 1024;
            pci.poolSizeCount = 1;
            pci.pPoolSizes = &size;
            VkDescriptorPool p;
            check(vkCreateDescriptorPool(gpu.device, &pci, nullptr, &p), "vkCreateDescriptorPool");
            set_pools.push_back(p);
        }
        if (!set) qilin::host::die("no descriptor set from a new pool");
        const VkDescriptorImageInfo info{sampler(bits), t.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(gpu.device, 1, &w, 0, nullptr);
        t.sets.emplace_back(bits, set);
        return set;
    }

    // An image's every level, one after another from the largest.
    void queue_upload(const Image& image, const uint32_t* rgba, bool fresh) {
        size_t n = 0;
        for (uint32_t l = 0; l < image.levels; ++l) n += size_t{std::max(image.width >> l, 1u)} * std::max(image.height >> l, 1u) * 4;
        const size_t at = upload_bytes.size();
        upload_bytes.resize(at + n);
        std::memcpy(upload_bytes.data() + at, rgba, n);
        uploads.push_back({image.image, image.width, image.height, image.levels, at, fresh});
        ++stats.uploads;
    }

    // The texture a polygon names, decoded again if video memory or its
    // palette changed since it last was.
    Texture& texture_for(const Source& src, uint32_t tsp, uint32_t tcw) {
        texture::Params p;
        p.tcw = tcw, p.width = u_size(tsp), p.height = v_size(tsp);
        p.stride = texture::stride_select(tcw) ? src.stride : 0;
        p.palette_format = src.palette_format, p.palette = src.palette;
        const uint64_t key = uint64_t{tcw} | uint64_t{tsp & 0x3Fu} << 32 | uint64_t{p.stride} << 38;
        Texture& t = textures[key];
        if (t.used == frame) return t;
        t.used = frame;
        const bool fresh = !t.image.image;
        if (fresh) t.bytes = texture::bytes(p);
        const uint64_t generation = src.generation(texture::address(tcw), t.bytes);
        uint64_t palette_hash = 0;
        if (texture::paletted(tcw)) {
            if (!src.palette) throw rt::Stop{rt::Stop::Unsupported, 0, tcw};
            palette_hash = hash_words(src.palette + texture::palette_base(tcw), texture::palette_count(tcw), 0xCBF29CE484222325ull ^ src.palette_format);
        }
        if (!fresh && generation == t.generation && palette_hash == t.palette_hash) return t;
        t.generation = generation, t.palette_hash = palette_hash;
        raw.resize(t.bytes);
        src.read(texture::address(tcw), raw.data(), t.bytes);
        const uint32_t levels = texture::levels(p);
        size_t texels = 0;
        for (uint32_t l = 0; l < levels; ++l) texels += size_t{std::max(p.width >> l, 1u)} * std::max(p.height >> l, 1u);
        pixels.resize(texels);
        for (uint32_t l = 0, at = 0; l < levels; ++l) {
            texture::decode_level(p, raw.data(), l, pixels.data() + at);
            at += std::max(p.width >> l, 1u) * std::max(p.height >> l, 1u);
        }
        if (fresh) {
            t.image = Image::create(gpu, p.width, p.height, kColourFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, levels);
            ++stats.textures;
        }
        queue_upload(t.image, pixels.data(), fresh);
        return t;
    }

    static bool sane(const ta::Vertex& a) {
        constexpr float kFar = 1e6f;  // pixels: far beyond any picture
        return std::isfinite(a.z) && std::fabs(a.x) < kFar && std::fabs(a.y) < kFar && std::isfinite(a.u[0]) && std::isfinite(a.v[0]);
    }
    static float clean_z(float z) { return z > 0.0f && std::isfinite(z) ? z : 1e-12f; }

    // A polygon affected by the modifier volumes: its PCW's bit 7 (and, in
    // the two-volume mode, its two volumes, bit 6).
    bool modified(const ta::Strip& s) const { return (s.pcw & 0x80u) && (cheap || s.two_volumes); }

    // A strip's triangles in one of its volumes (0, or 1: the second, drawn
    // where the modifier volumes enclose it, in the two-volume mode).
    void add_strip(const Source& src, const ta::Strip& s, const ta::Vertex* v, bool background, int vol = 0) {
        if (s.count < 3) return;
        const uint32_t tsp = s.tsp[vol];
        const bool translucent = s.list == ta::kTranslucent, punch = s.list == ta::kPunchThrough;
        const bool second_volumes = !cheap && !background && modified(s);  // its second volume is drawn too
        Draw d;
        d.pipeline = pipeline_key(translucent, src_blend(tsp), dst_blend(tsp), punch);
        d.flags = (s.textured ? 1u : 0u) | (s.pcw & 0x04u ? 2u : 0u) | (use_alpha(tsp) ? 4u : 0u) | (ignore_texture_alpha(tsp) ? 8u : 0u) |
                  shading(tsp) << 4;
        d.depth = background ? 7u : depth_mode(s.isp);
        d.depth_write = !z_write_off(s.isp);
        if (opaque_volumes && !translucent) d.stencil = !background && modified(s) ? Stencil::mark_shadow : Stencil::mark_none;
        if (vol == 1 && !translucent) {
            // Where the first volume's surface is the one seen and inside a
            // volume: the same triangles at the same depth, over it.
            d.depth = 2, d.depth_write = false, d.stencil = Stencil::shade;
        }
        // A translucent polygon tests itself against the translucent modifier
        // volumes, pixel by pixel (pvr.frag): darkened inside them (cheap
        // shadows), or each volume drawn only on its own side of them.
        if (translucent_volumes && translucent && !background && modified(s)) {
            if (cheap) d.flags |= kInVolumesFlag;
            else d.flags |= vol == 0 ? kOutsideVolumesFlag : kInsideVolumesFlag;
        } else if (translucent && second_volumes && vol == 1) {
            return;  // no translucent modifier volume: nothing is inside one
        }
        switch (fog_mode(tsp)) {
        case 0: d.flags |= kTableFogFlag; break;
        case 1: d.flags |= kVertexFogFlag; break;
        case 3: d.flags |= kTableFog2Flag; break;
        default: break;  // 2: no fog
        }
        if (colour_clamp(tsp)) d.flags |= kClampFlag;
        // The user tile clip (PCW bits 17-16): 0 none; 1 is reserved.
        if (const uint32_t clip = (s.pcw >> 16) & 3u; clip != 0 && !background) {
            if (clip == 1) throw rt::Stop{rt::Stop::Unsupported, 0, s.pcw};
            d.flags |= clip << kClipShift;
            d.clip = uint32_t{s.clip[0]} | uint32_t{s.clip[1]} << 8 | uint32_t{s.clip[2]} << 16 | uint32_t{s.clip[3]} << 24;
        }
        d.set = s.textured ? set_for(texture_for(src, tsp, s.tcw[vol]), sampler_bits(tsp)) : set_for(white, 0);
        d.first = uint32_t(vertices.size());
        const bool gouraud = s.pcw & 0x02u;
        const uint32_t cull = s.sprite || background ? 0u : cull_mode(s.isp);
        for (uint32_t i = 0; i + 2 < s.count; ++i) {
            const ta::Vertex* t[3] = {&v[i], &v[i + 1], &v[i + 2]};
            // A vertex off in the infinite (or not a number) is the game's
            // business on the console, where the hardware draws nothing of
            // it; a GPU's rasteriser may not cope. Such triangles are dropped.
            if (!sane(*t[0]) || !sane(*t[1]) || !sane(*t[2])) {
                ++stats.dropped;
                continue;
            }
            if (cull >= 2) {
                // The triangle's signed area on the screen; a strip's odd triangles wind the other way.
                float area = (t[1]->x - t[0]->x) * (t[2]->y - t[0]->y) - (t[2]->x - t[0]->x) * (t[1]->y - t[0]->y);
                if (i & 1u) area = -area;
                if (cull == 2 ? area < 0.0f : area > 0.0f) {
                    ++stats.culled;
                    continue;
                }
            }
            for (int k = 0; k < 3; ++k) {
                const ta::Vertex& a = *t[k];
                const ta::Vertex& c = gouraud ? a : *t[2];  // flat: the triangle's last vertex colours it
                HostVertex h;
                h.x = a.x, h.y = a.y, h.z = clean_z(a.z), h.u = a.u[vol], h.v = a.v[vol];
                std::memcpy(h.base, c.base[vol], sizeof h.base), std::memcpy(h.offset, c.offset[vol], sizeof h.offset);
                vertices.push_back(h);
            }
            ++stats.triangles;
        }
        d.count = uint32_t(vertices.size()) - d.first;
        if (d.count == 0) return;
        if (!draws.empty() && draws.back().same_state(d) && draws.back().first + draws.back().count == d.first) draws.back().count += d.count;
        else draws.push_back(d);
    }

    // The translucent modifier volumes, for pvr.frag: a header (the volume
    // count, the shadow's factor, the depth scale, the picture's pixels per
    // scene pixel), then per volume its
    // bounds on screen and its triangles' range, then the triangles (three
    // vec4s each: x, y, z). False if there are none.
    bool collect_translucent_volumes(const Source& src, const ta::Scene& scene) {
        volume_data.assign(kTrianglesAt, 0.0f);
        std::vector<float> triangles;
        uint32_t volumes = 0, first = 0;
        float box[4] = {1e30f, 1e30f, -1e30f, -1e30f};
        const auto end_volume = [&] {
            const uint32_t count = uint32_t(triangles.size() / 12) - first;
            if (count == 0) return;
            if (volumes == kMaxVolumes) throw rt::Stop{rt::Stop::Unsupported, 0, volumes};
            float* v = volume_data.data() + kVolumesAt + volumes * 8;
            std::memcpy(v, box, sizeof box);
            const uint32_t range[2] = {first, count};
            std::memcpy(v + 4, range, sizeof range);
            ++volumes, first += count;
            box[0] = box[1] = 1e30f, box[2] = box[3] = -1e30f;
        };
        for (const ta::Modifier& m : scene.modifiers) {
            if (m.list == ta::kOpaqueModifier) continue;
            // The user tile clip, in each triangle's first corner's fourth
            // word (pvr.frag): the mode (bits 21-20), then the rectangle in
            // tiles (x min 6 bits, y min 4, x max 6, y max 4).
            const uint32_t mode = (m.pcw >> 16) & 3u;
            if (mode == 1) throw rt::Stop{rt::Stop::Unsupported, 0, m.pcw};
            const uint32_t clip_word = mode == 0 ? 0u
                                                 : mode << 20 | uint32_t{m.clip[0] & 0x3Fu} << 14 | uint32_t{m.clip[1] & 0xFu} << 10 |
                                                       uint32_t{m.clip[2] & 0x3Fu} << 4 | uint32_t{m.clip[3] & 0xFu};
            float clip_bits;
            std::memcpy(&clip_bits, &clip_word, 4);
            for (uint32_t t = m.first; t < m.first + m.count; ++t) {
                const float* f = scene.modifier_triangles.data() + size_t{t} * 9;
                for (int k = 0; k < 3; ++k) {
                    const float x = f[3 * k], y = f[3 * k + 1];
                    triangles.insert(triangles.end(), {x, y, clean_z(f[3 * k + 2]), k == 0 ? clip_bits : 0.0f});
                    box[0] = std::min(box[0], x), box[1] = std::min(box[1], y), box[2] = std::max(box[2], x), box[3] = std::max(box[3], y);
                }
            }
            if (m.isp & 0x60000000u) end_volume();
        }
        end_volume();
        if (volumes == 0) {
            volume_data.resize(kTrianglesAt);
            return false;
        }
        const uint32_t head[2] = {volumes, 0};
        std::memcpy(volume_data.data(), head, sizeof head);
        volume_data[2] = float(src.shadow_scale & 0xFFu) / 256.0f;
        volume_data.insert(volume_data.end(), triangles.begin(), triangles.end());
        return true;
    }

    // The opaque modifier volumes into the stencil, one volume at a time
    // (a volume ends with the polygon whose ISP word has a volume
    // instruction, bits 30-29), then what being inside one does to the
    // surface the volumes affect: with cheap shadows, its colour times
    // SHADOW_SCALE's low byte, in 256ths; in the two-volume mode, its
    // polygon's second volume drawn instead of the first.
    void add_volumes(const Source& src, const ta::Scene& scene) {
        Draw faces;
        faces.pipeline = kVolumeKey;
        faces.set = set_for(white, 0);
        faces.depth = 4;  // greater: a face nearer than the surface (z = 1/w)
        faces.stencil = Stencil::volume;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;  // the volume's bounds on screen
        bool any = false, drawn_apart = false;  // drawn_apart: some of this volume's faces are already in draws
        auto rectangle = [&](Draw d, float l, float t, float r, float b, float shade) {
            d.first = uint32_t(vertices.size()), d.count = 6;
            const float corners[6][2] = {{l, t}, {r, t}, {l, b}, {r, t}, {r, b}, {l, b}};
            for (const auto& c : corners) {
                HostVertex v{};
                v.x = c[0], v.y = c[1], v.z = 1.0f;
                v.base[0] = v.base[1] = v.base[2] = shade, v.base[3] = 1.0f;
                vertices.push_back(v);
            }
            draws.push_back(d);
        };
        auto end_volume = [&] {
            if (faces.count == 0 && !drawn_apart) return;
            if (faces.count != 0) draws.push_back(faces);
            Draw resolve = faces;
            resolve.flags = 0, resolve.clip = 0;  // bit 0 is set only where the faces counted
            resolve.depth = 7;  // always
            resolve.stencil = Stencil::resolve;
            rectangle(resolve, std::floor(x0), std::floor(y0), std::ceil(x1), std::ceil(y1), 1.0f);
            faces.count = 0, any = true, drawn_apart = false;
            x0 = y0 = 1e30f, x1 = y1 = -1e30f;
        };
        for (const ta::Modifier& m : scene.modifiers) {
            if (m.list != ta::kOpaqueModifier) continue;
            // The user tile clip: the faces count only inside (or outside) it,
            // so the volume's effect is kept to that side.
            Draw clipped = faces;
            clipped.flags = 0, clipped.clip = 0;
            if (const uint32_t clip = (m.pcw >> 16) & 3u; clip != 0) {
                if (clip == 1) throw rt::Stop{rt::Stop::Unsupported, 0, m.pcw};
                clipped.flags = clip << kClipShift;
                clipped.clip = uint32_t{m.clip[0]} | uint32_t{m.clip[1]} << 8 | uint32_t{m.clip[2]} << 16 | uint32_t{m.clip[3]} << 24;
            }
            if (faces.count != 0 && !faces.same_state(clipped)) {  // a volume's parts under other clips: drawn apart
                draws.push_back(faces);
                faces.count = 0, drawn_apart = true;
            }
            faces.flags = clipped.flags, faces.clip = clipped.clip;
            for (uint32_t t = m.first; t < m.first + m.count; ++t) {
                if (faces.count == 0) faces.first = uint32_t(vertices.size());
                const float* f = scene.modifier_triangles.data() + size_t{t} * 9;
                for (int k = 0; k < 3; ++k) {
                    HostVertex h{};
                    h.x = f[3 * k], h.y = f[3 * k + 1], h.z = clean_z(f[3 * k + 2]);
                    x0 = std::min(x0, h.x), y0 = std::min(y0, h.y), x1 = std::max(x1, h.x), y1 = std::max(y1, h.y);
                    vertices.push_back(h);
                }
                faces.count += 3;
            }
            if (m.isp & 0x60000000u) end_volume();
        }
        end_volume();  // a list that ends without the instruction
        if (!any) return;
        if (!cheap) {
            for (const ta::List list : {ta::kOpaque, ta::kPunchThrough})
                for (const ta::Strip& s : scene.strips)
                    if (s.list == list && modified(s)) add_strip(src, s, scene.vertices.data() + s.first, false, 1);
            return;
        }
        Draw shade;
        shade.pipeline = pipeline_key(true, 0, 2, false);  // zero, the colour drawn: the destination times it
        shade.set = set_for(white, 0);
        shade.depth = 7;
        shade.stencil = Stencil::shade;
        rectangle(shade, 0, 0, float(src.width), float(src.height), float(src.shadow_scale & 0xFFu) / 256.0f);
    }

    // The translucent list in auto-sort mode (ISP_FEED_CFG's bit 0 clear):
    // the hardware sorts it by depth for each pixel, so a polygon farther
    // away is blended first. Here its triangles are drawn from the farthest
    // to the nearest, by their mean z (1/w: smaller is farther), the order
    // sent kept between equals.
    void sort_translucent(size_t first_draw, size_t first_vertex) {
        struct Tri {
            float depth;
            uint32_t draw, at;
        };
        std::vector<Tri> tris;
        for (size_t d = first_draw; d < draws.size(); ++d)
            for (uint32_t v = draws[d].first; v < draws[d].first + draws[d].count; v += 3)
                tris.push_back({vertices[v].z + vertices[v + 1].z + vertices[v + 2].z, uint32_t(d), v});
        std::stable_sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.depth < b.depth; });
        std::vector<HostVertex> sorted;
        sorted.reserve(tris.size() * 3);
        std::vector<Draw> out(draws.begin(), draws.begin() + std::ptrdiff_t(first_draw));
        for (const Tri& t : tris) {
            Draw d = draws[t.draw];
            d.first = uint32_t(first_vertex + sorted.size()), d.count = 3;
            sorted.insert(sorted.end(), vertices.begin() + t.at, vertices.begin() + t.at + 3);
            if (out.size() > first_draw && out.back().same_state(d)) out.back().count += 3;
            else out.push_back(d);
        }
        std::copy(sorted.begin(), sorted.end(), vertices.begin() + std::ptrdiff_t(first_vertex));
        draws = std::move(out);
    }

    void set_stencil(Stencil s) {
        const VkStencilFaceFlags both = VK_STENCIL_FACE_FRONT_AND_BACK;
        vkCmdSetStencilTestEnable(cmd, s != Stencil::off);
        if (s == Stencil::off) return;
        switch (s) {
        case Stencil::mark_none:
        case Stencil::mark_shadow:  // what the visible surface is: bit 1 set or clear, bits 0 and 2 cleared
            vkCmdSetStencilOp(cmd, both, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
            vkCmdSetStencilWriteMask(cmd, both, 7), vkCmdSetStencilCompareMask(cmd, both, 0);
            vkCmdSetStencilReference(cmd, both, s == Stencil::mark_shadow ? 2u : 0u);
            break;
        case Stencil::volume:  // each face in front flips bit 0
            vkCmdSetStencilOp(cmd, both, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
            vkCmdSetStencilWriteMask(cmd, both, 1), vkCmdSetStencilCompareMask(cmd, both, 0), vkCmdSetStencilReference(cmd, both, 0);
            break;
        case Stencil::resolve:  // where bit 0 is set: set bit 2, clear bit 0
            vkCmdSetStencilOp(cmd, both, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_NOT_EQUAL);
            vkCmdSetStencilWriteMask(cmd, both, 5), vkCmdSetStencilCompareMask(cmd, both, 1), vkCmdSetStencilReference(cmd, both, 4);
            break;
        default:  // shade: where bits 1 and 2 are set
            vkCmdSetStencilOp(cmd, both, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL);
            vkCmdSetStencilWriteMask(cmd, both, 0), vkCmdSetStencilCompareMask(cmd, both, 6), vkCmdSetStencilReference(cmd, both, 6);
            break;
        }
    }

    void memory_barrier(VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = src_stage, mb.srcAccessMask = src_access, mb.dstStageMask = dst_stage, mb.dstAccessMask = dst_access;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    // The per-pixel lists sorted and blended into the picture (sort.comp),
    // over the render's clip (the target's pixels, x1 and y1 excluded); the
    // picture is left in GENERAL layout, and the node count copied out for
    // the next frame's size_lists.
    void resolve_lists(const Image& colour, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        qilin::host::transition(cmd, colour.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        memory_barrier(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        // (Written while recording: draw waited for the last frame, the only other user of the set.)
        if (sort_set_written != remade || sort_set_view != colour.view) {
            const VkDescriptorImageInfo ii{VK_NULL_HANDLE, colour.view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo bi[2] = {{heads.buffer, 0, VK_WHOLE_SIZE}, {nodes.buffer, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstSet = sort_set, w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[0].pImageInfo = &ii;
            w[1].dstSet = sort_set, w[1].dstBinding = 1, w[1].descriptorCount = 2, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[1].pBufferInfo = bi;
            vkUpdateDescriptorSets(gpu.device, 2, w, 0, nullptr);
            sort_set_written = remade, sort_set_view = colour.view;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sort_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sort_layout, 0, 1, &sort_set, 0, nullptr);
        const int32_t rect[4] = {int32_t(x0), int32_t(y0), int32_t(x1), int32_t(y1)};
        vkCmdPushConstants(cmd, sort_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rect, rect);
        vkCmdDispatch(cmd, (x1 - x0 + 7) / 8, (y1 - y0 + 7) / 8, 1);
        const VkBufferCopy count{0, 0, 4};
        vkCmdCopyBuffer(cmd, nodes.buffer, node_count.buffer, 1, &count);
        memory_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }

    void grow(Buffer& b, VkDeviceSize need, VkBufferUsageFlags usage) {
        if (b.size >= need) return;
        ++remade;
        b.destroy(gpu);
        b = Buffer::host_visible(gpu, std::max<VkDeviceSize>(need * 3 / 2, 1u << 20), usage);
    }

    Picture draw(const Source& src) {
        wait();
        ++frame;
        stats.draws = stats.triangles = stats.culled = stats.uploads = stats.dropped = 0;
        Target& t = target(src.frame_buffer, src.width, src.height);
        current = &t;
        const Image& colour = t.colour;
        vertices.clear(), draws.clear(), uploads.clear(), upload_bytes.clear();
        if (frame == 1) {
            const uint32_t opaque_white = 0xFFFFFFFFu;
            queue_upload(white.image, &opaque_white, true);
        }
        const ta::Scene& scene = *src.scene;
        // The modifier volumes (SHADOW_SCALE's bit 8 says how): with cheap
        // shadows they darken the polygons that take shadows inside them; in
        // the two-volume mode, those polygons are drawn with their second
        // volume inside them. The opaque list's go through the stencil, the
        // translucent list's through the fragment shader's test.
        bool any_opaque = false, any_translucent = false;
        for (const ta::Modifier& m : scene.modifiers) (m.list == ta::kOpaqueModifier ? any_opaque : any_translucent) = true;
        cheap = (src.shadow_scale & 0x100u) != 0;
        opaque_volumes = any_opaque;
        translucent_volumes = any_translucent && collect_translucent_volumes(src, scene);
        if (src.background.count) add_strip(src, src.background, src.background_vertices, true);
        size_t translucent_draws = 0, translucent_vertices = 0;
        for (const ta::List list : {ta::kOpaque, ta::kPunchThrough, ta::kTranslucent}) {
            if (list == ta::kTranslucent) {
                if (opaque_volumes) add_volumes(src, scene);
                translucent_draws = draws.size(), translucent_vertices = vertices.size();
            }
            for (const ta::Strip& s : scene.strips) {
                if (s.list != list) continue;
                add_strip(src, s, scene.vertices.data() + s.first, false);
                if (list == ta::kTranslucent && !cheap && modified(s)) add_strip(src, s, scene.vertices.data() + s.first, false, 1);
            }
        }
        // The translucent list in auto-sort mode: sorted for each pixel where
        // the device can (each draw onto the per-pixel lists, with its blend
        // factors and its place in the order sent), else triangle by triangle.
        const bool sorted = !src.presort && sorting && draws.size() > translucent_draws;
        if (!src.presort && !sorted) sort_translucent(translucent_draws, translucent_vertices);
        if (sorted) {
            size_lists(colour.width * colour.height);
            for (size_t i = translucent_draws; i < draws.size(); ++i) {
                Draw& d = draws[i];
                const uint32_t sf = d.pipeline & 1u ? (d.pipeline >> 1) & 7u : 1u, df = d.pipeline & 1u ? (d.pipeline >> 4) & 7u : 0u;
                d.pipeline = kSortedKey;
                d.sort_word = sf << 29 | df << 26 | (static_cast<uint32_t>(i - translucent_draws) & 0x03FFFFFFu);
            }
        }
        stats.draws = uint32_t(draws.size());

        float z_max = 0.0f;
        for (const HostVertex& v : vertices) z_max = std::max(z_max, v.z);
        Push push{};
        push.screen[0] = 2.0f / float(src.width), push.screen[1] = 2.0f / float(src.height);
        push.screen[2] = z_max > 0.0f ? 1.0f / z_max : 1.0f;
        push.alpha_ref = float(src.punch_alpha_ref & 0xFFu) / 255.0f;

        grow(vertex_buffer, vertices.size() * sizeof(HostVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!vertices.empty()) std::memcpy(vertex_buffer.mapped, vertices.data(), vertices.size() * sizeof(HostVertex));
        // The frame's buffer: the volumes (if any) and the table fog's registers, every frame.
        if (!translucent_volumes) volume_data.assign(kTrianglesAt, 0.0f);
        volume_data[3] = push.screen[2];  // the depth scale, for the volumes' z
        volume_data[4] = float(scale);
        volume_data[kFogColourAt] = float(src.fog_colour >> 16 & 0xFFu) / 255.0f;
        volume_data[kFogColourAt + 1] = float(src.fog_colour >> 8 & 0xFFu) / 255.0f;
        volume_data[kFogColourAt + 2] = float(src.fog_colour & 0xFFu) / 255.0f;
        const auto colour_at = [&](size_t at, uint32_t argb, bool with_alpha) {
            for (int k = 0; k < 3; ++k) volume_data[at + size_t(k)] = float(argb >> (16 - 8 * k) & 0xFFu) / 255.0f;
            volume_data[at + 3] = with_alpha ? float(argb >> 24) / 255.0f : 1.0f;
        };
        colour_at(kFogVertexColourAt, src.fog_vertex_colour, false);
        colour_at(kClampMaxAt, src.clamp_max, true);
        colour_at(kClampMinAt, src.clamp_min, true);
        volume_data[kFogDensityAt] = fog_density(src.fog_density);
        std::memcpy(volume_data.data() + kFogTableAt, src.fog_table, sizeof src.fog_table);
        grow(volume_buffer, volume_data.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(volume_buffer.mapped, volume_data.data(), volume_data.size() * sizeof(float));
        if (volume_set_written != remade) {
            const VkDescriptorBufferInfo bi[3] = {{volume_buffer.buffer, 0, VK_WHOLE_SIZE}, {heads.buffer, 0, VK_WHOLE_SIZE}, {nodes.buffer, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = volume_set;
            w.descriptorCount = 3;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = bi;
            vkUpdateDescriptorSets(gpu.device, 1, &w, 0, nullptr);
            volume_set_written = remade;
        }
        grow(staging, upload_bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!upload_bytes.empty()) std::memcpy(staging.mapped, upload_bytes.data(), upload_bytes.size());

        check(vkResetFences(gpu.device, 1, &fence), "vkResetFences");
        check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        for (const Upload& u : uploads) {
            qilin::host::transition(cmd, u.image, u.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy regions[16]{};
            VkDeviceSize at = u.at;
            for (uint32_t l = 0; l < u.levels; ++l) {
                const uint32_t w = std::max(u.width >> l, 1u), h = std::max(u.height >> l, 1u);
                regions[l].bufferOffset = at;
                regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                regions[l].imageExtent = {w, h, 1};
                at += VkDeviceSize{w} * h * 4;
            }
            vkCmdCopyBufferToImage(cmd, staging.buffer, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, u.levels, regions);
            qilin::host::transition(cmd, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        qilin::host::transition(cmd, colour.image, t.ready ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        qilin::host::transition(cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        if (sorted) {  // empty lists: every head none, no nodes
            vkCmdFillBuffer(cmd, heads.buffer, 0, VkDeviceSize{colour.width} * colour.height * 4, kNoNode);
            const uint32_t header[4] = {0, node_capacity, colour.width, 0};
            vkCmdUpdateBuffer(cmd, nodes.buffer, 0, sizeof header, header);
            memory_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = colour.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = t.ready ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil = {0.0f, 0};  // the farthest: z = 1/w
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {colour.width, colour.height}};
        // The clip, in the target's pixels.
        const uint32_t x0 = std::min(src.clip_x0, src.width - 1) * scale, y0 = std::min(src.clip_y0, src.height - 1) * scale;
        const uint32_t x1 = (std::min(src.clip_x1, src.width - 1) + 1) * scale, y1 = (std::min(src.clip_y1, src.height - 1) + 1) * scale;
        const VkRect2D clip{{int32_t(x0), int32_t(y0)}, {x1 > x0 ? x1 - x0 : 1, y1 > y0 ? y1 - y0 : 1}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        // The game's pixels map onto the image one to `scale` (the image may be larger than this render's frame).
        const VkViewport viewport{0.0f, 0.0f, float(src.width * scale), float(src.height * scale), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &clip);
        {
            // The tiles this render covers start black (then the background plane).
            VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
            clear.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            const VkClearRect rect{clip, 0, 1};
            vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
        }
        if (!draws.empty()) {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &zero);
        }
        uint32_t bound = ~0u;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &volume_set, 0, nullptr);
        for (const Draw& d : draws) {
            if (d.pipeline != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(d.pipeline)), bound = d.pipeline;
            vkCmdSetDepthWriteEnable(cmd, d.depth_write);
            vkCmdSetDepthCompareOp(cmd, static_cast<VkCompareOp>(d.depth));  // the hardware's modes are Vulkan's, in order
            set_stencil(d.stencil);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &d.set, 0, nullptr);
            push.flags = d.flags;
            push.sort_word = d.sort_word;
            // The user tile clip, in scene pixels (tiles of 32), its maximum tile included.
            push.clip[0] = float(d.clip & 0xFFu) * 32.0f, push.clip[1] = float(d.clip >> 8 & 0xFFu) * 32.0f;
            push.clip[2] = float((d.clip >> 16 & 0xFFu) + 1) * 32.0f, push.clip[3] = float((d.clip >> 24) + 1) * 32.0f;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
            vkCmdDraw(cmd, d.count, 1, d.first, 0);
        }
        vkCmdEndRendering(cmd);
        if (sorted) {
            resolve_lists(colour, x0, y0, x1, y1);
            qilin::host::transition(cmd, colour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        } else {
            qilin::host::transition(cmd, colour.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        check(vkQueueSubmit2(gpu.queue, 1, &si, fence), "vkQueueSubmit2");
        t.ready = true;
        return {colour.image, colour.view, colour.width, colour.height};
    }

    void set_anisotropy(unsigned level) {
        const float a = std::clamp(float(level), 1.0f, std::max(gpu.max_anisotropy, 1.0f));
        if (a == anisotropy) return;
        // The samplers, and the texture descriptor sets that name them, are made again as they're used.
        wait();
        for (auto& [k, s] : samplers) vkDestroySampler(gpu.device, s, nullptr);
        samplers.clear();
        for (auto& [k, t] : textures) t.sets.clear();
        white.sets.clear();
        for (VkDescriptorPool p : set_pools) check(vkResetDescriptorPool(gpu.device, p, 0), "vkResetDescriptorPool");
        anisotropy = a;
    }

    Picture picture(uint32_t key) const {
        const auto it = targets.find(key);
        if (it == targets.end() || !it->second.ready) return {};
        const Image& c = it->second.colour;
        return {c.image, c.view, c.width, c.height};
    }

    std::vector<uint8_t> read_back() {
        wait();
        if (!current) return {};
        const Image& colour = current->colour;
        Buffer out = Buffer::host_visible(gpu, VkDeviceSize{colour.width} * colour.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        check(vkResetFences(gpu.device, 1, &fence), "vkResetFences");
        check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {colour.width, colour.height, 1};
        vkCmdCopyImageToBuffer(cmd, colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer, 1, &region);
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        check(vkQueueSubmit2(gpu.queue, 1, &si, fence), "vkQueueSubmit2");
        wait();
        const auto* p = static_cast<const uint8_t*>(out.mapped);
        std::vector<uint8_t> px(p, p + out.size);
        out.destroy(gpu);
        return px;
    }

    void destroy() {
        wait();
        for (auto& [k, t] : textures) t.image.destroy(gpu);
        white.image.destroy(gpu);
        for (auto& [k, p] : pipelines) vkDestroyPipeline(gpu.device, p, nullptr);
        for (auto& [k, s] : samplers) vkDestroySampler(gpu.device, s, nullptr);
        for (VkDescriptorPool p : set_pools) vkDestroyDescriptorPool(gpu.device, p, nullptr);
        vkDestroyShaderModule(gpu.device, vert, nullptr);
        vkDestroyShaderModule(gpu.device, frag, nullptr);
        vkDestroyShaderModule(gpu.device, frag_punch, nullptr);
        if (frag_sorted) vkDestroyShaderModule(gpu.device, frag_sorted, nullptr);
        if (sort_module) vkDestroyShaderModule(gpu.device, sort_module, nullptr);
        if (sort_pipeline) vkDestroyPipeline(gpu.device, sort_pipeline, nullptr);
        if (sort_layout) vkDestroyPipelineLayout(gpu.device, sort_layout, nullptr);
        if (sort_pool) vkDestroyDescriptorPool(gpu.device, sort_pool, nullptr);
        if (sort_set_layout) vkDestroyDescriptorSetLayout(gpu.device, sort_set_layout, nullptr);
        heads.destroy(gpu), nodes.destroy(gpu), node_count.destroy(gpu);
        vkDestroyPipelineLayout(gpu.device, layout, nullptr);
        vkDestroyDescriptorSetLayout(gpu.device, set_layout, nullptr);
        vertex_buffer.destroy(gpu), staging.destroy(gpu), volume_buffer.destroy(gpu);
        vkDestroyDescriptorPool(gpu.device, volume_pool, nullptr);
        vkDestroyDescriptorSetLayout(gpu.device, volume_layout, nullptr);
        for (auto& [k, t] : targets) t.colour.destroy(gpu);
        depth.destroy(gpu);
        vkDestroyFence(gpu.device, fence, nullptr);
        vkDestroyCommandPool(gpu.device, pool, nullptr);
    }
};

Renderer::Renderer(Gpu& gpu, uint32_t scale) : impl_(std::make_unique<Impl>(gpu, std::max(scale, 1u))) { impl_->create(); }
Renderer::~Renderer() { impl_->destroy(); }
Picture Renderer::draw(const Source& source) { return impl_->draw(source); }
void Renderer::set_anisotropy(unsigned level) { impl_->set_anisotropy(level); }
std::vector<uint8_t> Renderer::read_back() { return impl_->read_back(); }
Picture Renderer::picture(uint32_t frame_buffer) const { return impl_->picture(frame_buffer); }
const Renderer::Stats& Renderer::stats() const { return impl_->stats; }

}  // namespace dreamcast::renderer
