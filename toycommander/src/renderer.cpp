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
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <unordered_map>
#include <vector>

#include "guest.hpp"
#include "shaders.inc"
#include "texture.hpp"

namespace toycommander::renderer {

using qilin::host::Buffer;
using qilin::host::check;
using qilin::host::Gpu;
using qilin::host::Image;
namespace rt = qilin::rt;

namespace {

constexpr VkFormat kColourFormat = VK_FORMAT_R8G8B8A8_UNORM, kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct HostVertex {
    float x, y, z;
    float u, v;
    float base[4], offset[4];
};

struct Push {
    float screen[4];
    uint32_t flags;
    float alpha_ref;
};

// The ISP/TSP instruction word and the TSP instruction word.
constexpr uint32_t depth_mode(uint32_t isp) { return isp >> 29; }
constexpr uint32_t cull_mode(uint32_t isp) { return (isp >> 27) & 3u; }
constexpr bool z_write_off(uint32_t isp) { return (isp >> 26) & 1u; }
constexpr uint32_t src_blend(uint32_t tsp) { return tsp >> 29; }
constexpr uint32_t dst_blend(uint32_t tsp) { return (tsp >> 26) & 7u; }
constexpr uint32_t fog_mode(uint32_t tsp) { return (tsp >> 22) & 3u; }
constexpr bool use_alpha(uint32_t tsp) { return (tsp >> 20) & 1u; }
constexpr bool ignore_texture_alpha(uint32_t tsp) { return (tsp >> 19) & 1u; }
constexpr uint32_t shading(uint32_t tsp) { return (tsp >> 6) & 3u; }
constexpr uint32_t u_size(uint32_t tsp) { return 8u << ((tsp >> 3) & 7u); }
constexpr uint32_t v_size(uint32_t tsp) { return 8u << (tsp & 7u); }
// Filter, clamp and flip: what a sampler is made from.
constexpr uint32_t sampler_bits(uint32_t tsp) { return (tsp >> 13) & 0x3Fu; }

struct Draw {
    uint32_t first = 0, count = 0;  // vertices
    uint32_t pipeline = 0;          // the pipeline's key
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t flags = 0;
    uint32_t depth = 0;
    bool depth_write = false;
    bool same_state(const Draw& o) const {
        return pipeline == o.pipeline && set == o.set && flags == o.flags && depth == o.depth && depth_write == o.depth_write;
    }
};

// A pipeline's key: bit 0 blending, 1-3 and 4-6 its factors, 7 punch-through.
constexpr uint32_t pipeline_key(bool blend, uint32_t src, uint32_t dst, bool punch) {
    return (blend ? 1u | src << 1 | dst << 4 : 0u) | (punch ? 0x80u : 0u);
}

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

void once(bool& said, const char* what) {
    if (said) return;
    said = true;
    std::fprintf(stderr, QILIN_APP ": the renderer doesn't draw %s yet\n", what);
}

}  // namespace

struct Renderer::Impl {
    Gpu& gpu;
    uint32_t scale;
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
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> set_pools;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, frag_punch = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkPipeline> pipelines;
    std::unordered_map<uint32_t, VkSampler> samplers;
    std::unordered_map<uint64_t, Texture> textures;
    Texture white{};
    uint64_t frame = 0;
    Stats stats{};
    bool said_modifiers = false, said_fog = false, said_two_volumes = false;

    // The frame being built.
    std::vector<HostVertex> vertices;
    std::vector<Draw> draws;
    struct Upload {
        VkImage image;
        uint32_t width, height;
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
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(gpu.device, &pli, nullptr, &layout), "vkCreatePipelineLayout");

        vert = module(kPvrVert, sizeof kPvrVert);
        frag = module(kPvrFrag, sizeof kPvrFrag);
        frag_punch = module(kPvrFragPunch, sizeof kPvrFragPunch);

        // What untextured polygons sample (the shader has one sampler whatever it draws).
        white.image = Image::create(gpu, 1, 1, kColourFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
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
            t.colour = Image::create(gpu, w, h, kColourFormat,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            t.ready = false;
        }
        if (!depth.image || depth.width < w || depth.height < h) {
            check(vkDeviceWaitIdle(gpu.device), "vkDeviceWaitIdle");
            depth.destroy(gpu);
            depth = Image::create(gpu, std::max(w, depth.width), std::max(h, depth.height), kDepthFormat,
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
        stages[1].module = key & 0x80u ? frag_punch : frag;
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
        const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                          VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = uint32_t(std::size(dynamic));
        dy.pDynamicStates = dynamic;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &kColourFormat;
        ri.depthAttachmentFormat = kDepthFormat;
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
        // bits: 0-1 the filter, 2 clamp V, 3 clamp U, 4 flip V, 5 flip U. Clamping wins over flipping.
        const auto mode = [&](bool clamp, bool flip) {
            return clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : flip ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = bits & 3u ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = mode(bits & 8u, bits & 32u);
        sci.addressModeV = mode(bits & 4u, bits & 16u);
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
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

    void queue_upload(const Image& image, const uint32_t* rgba, bool fresh) {
        const size_t at = upload_bytes.size(), n = size_t{image.width} * image.height * 4;
        upload_bytes.resize(at + n);
        std::memcpy(upload_bytes.data() + at, rgba, n);
        uploads.push_back({image.image, image.width, image.height, at, fresh});
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
        pixels.resize(size_t{p.width} * p.height);
        texture::decode(p, raw.data(), pixels.data());
        if (fresh) {
            t.image = Image::create(gpu, p.width, p.height, kColourFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
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

    void add_strip(const Source& src, const ta::Strip& s, const ta::Vertex* v, bool background) {
        if (s.count < 3) return;
        if (s.two_volumes) once(said_two_volumes, "a polygon's second volume (it draws the first)");
        if (fog_mode(s.tsp[0]) != 2) once(said_fog, "fog");
        const uint32_t tsp = s.tsp[0];
        const bool translucent = s.list == ta::kTranslucent, punch = s.list == ta::kPunchThrough;
        Draw d;
        d.pipeline = pipeline_key(translucent, src_blend(tsp), dst_blend(tsp), punch);
        d.flags = (s.textured ? 1u : 0u) | (s.pcw & 0x04u ? 2u : 0u) | (use_alpha(tsp) ? 4u : 0u) | (ignore_texture_alpha(tsp) ? 8u : 0u) |
                  shading(tsp) << 4;
        d.depth = background ? 7u : depth_mode(s.isp);
        d.depth_write = !z_write_off(s.isp);
        d.set = s.textured ? set_for(texture_for(src, tsp, s.tcw[0]), sampler_bits(tsp)) : set_for(white, 0);
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
                h.x = a.x, h.y = a.y, h.z = clean_z(a.z), h.u = a.u[0], h.v = a.v[0];
                std::memcpy(h.base, c.base[0], sizeof h.base), std::memcpy(h.offset, c.offset[0], sizeof h.offset);
                vertices.push_back(h);
            }
            ++stats.triangles;
        }
        d.count = uint32_t(vertices.size()) - d.first;
        if (d.count == 0) return;
        if (!draws.empty() && draws.back().same_state(d) && draws.back().first + draws.back().count == d.first) draws.back().count += d.count;
        else draws.push_back(d);
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

    void grow(Buffer& b, VkDeviceSize need, VkBufferUsageFlags usage) {
        if (b.size >= need) return;
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
        if (!scene.modifiers.empty()) once(said_modifiers, "modifier volumes (shadows)");
        if (src.background.count) add_strip(src, src.background, src.background_vertices, true);
        size_t translucent_draws = 0, translucent_vertices = 0;
        for (const ta::List list : {ta::kOpaque, ta::kPunchThrough, ta::kTranslucent}) {
            if (list == ta::kTranslucent) translucent_draws = draws.size(), translucent_vertices = vertices.size();
            for (const ta::Strip& s : scene.strips)
                if (s.list == list) add_strip(src, s, scene.vertices.data() + s.first, false);
        }
        if (!src.presort) sort_translucent(translucent_draws, translucent_vertices);
        stats.draws = uint32_t(draws.size());

        float z_max = 0.0f;
        for (const HostVertex& v : vertices) z_max = std::max(z_max, v.z);
        Push push{};
        push.screen[0] = 2.0f / float(src.width), push.screen[1] = 2.0f / float(src.height);
        push.screen[2] = z_max > 0.0f ? 1.0f / z_max : 1.0f;
        push.alpha_ref = float(src.punch_alpha_ref & 0xFFu) / 255.0f;

        grow(vertex_buffer, vertices.size() * sizeof(HostVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!vertices.empty()) std::memcpy(vertex_buffer.mapped, vertices.data(), vertices.size() * sizeof(HostVertex));
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
            VkBufferImageCopy region{};
            region.bufferOffset = u.at;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {u.width, u.height, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            qilin::host::transition(cmd, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        qilin::host::transition(cmd, colour.image, t.ready ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        qilin::host::transition(cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = colour.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = t.ready ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
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
        for (const Draw& d : draws) {
            if (d.pipeline != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(d.pipeline)), bound = d.pipeline;
            vkCmdSetDepthWriteEnable(cmd, d.depth_write);
            vkCmdSetDepthCompareOp(cmd, static_cast<VkCompareOp>(d.depth));  // the hardware's modes are Vulkan's, in order
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &d.set, 0, nullptr);
            push.flags = d.flags;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
            vkCmdDraw(cmd, d.count, 1, d.first, 0);
        }
        vkCmdEndRendering(cmd);
        qilin::host::transition(cmd, colour.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
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
        vkDestroyPipelineLayout(gpu.device, layout, nullptr);
        vkDestroyDescriptorSetLayout(gpu.device, set_layout, nullptr);
        vertex_buffer.destroy(gpu), staging.destroy(gpu);
        for (auto& [k, t] : targets) t.colour.destroy(gpu);
        depth.destroy(gpu);
        vkDestroyFence(gpu.device, fence, nullptr);
        vkDestroyCommandPool(gpu.device, pool, nullptr);
    }
};

Renderer::Renderer(Gpu& gpu, uint32_t scale) : impl_(std::make_unique<Impl>(gpu, std::max(scale, 1u))) { impl_->create(); }
Renderer::~Renderer() { impl_->destroy(); }
Picture Renderer::draw(const Source& source) { return impl_->draw(source); }
std::vector<uint8_t> Renderer::read_back() { return impl_->read_back(); }
Picture Renderer::picture(uint32_t frame_buffer) const { return impl_->picture(frame_buffer); }
const Renderer::Stats& Renderer::stats() const { return impl_->stats; }

}  // namespace toycommander::renderer
