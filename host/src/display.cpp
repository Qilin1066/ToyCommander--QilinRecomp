// The window and presentation (display.hpp).
//
// Each present draws into the window's image (or, headless, the target)
// with dynamic rendering: black, then the picture through its look in the
// rectangle the scaling gives, then the overlay. One present is in flight at
// a time (the fence is waited for before the next is recorded), so the one
// descriptor set is rewritten for each.
#include "qilin/host/display.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "qilin/host/vulkan.hpp"
#include "shaders.inc"

namespace qilin::host {

namespace {
constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;

VkSurfaceKHR make_sdl_surface(VkInstance instance, void* window) {
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window), instance, nullptr, &s))
        die("cannot create a Vulkan surface for the window: %s", SDL_GetError());
    return s;
}

void need_format(const Gpu& gpu, VkFormat f, VkFormatFeatureFlags want, const char* what) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(gpu.physical, f, &p);
    if ((p.optimalTilingFeatures & want) != want) die("%s: the GPU can't use format %d for that", what, static_cast<int>(f));
}

struct Push {
    float lines[2];
    float scanlines, mask;
};

VkShaderModule module(const Gpu& gpu, const uint32_t* words, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = words;
    VkShaderModule m;
    check(vkCreateShaderModule(gpu.device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

// What the console says about controllers, so a player can see what SDL found.
void say(const char* format, ...) {
    va_list a;
    va_start(a, format);
    std::fputs(QILIN_APP ": ", stderr);
    std::vfprintf(stderr, format, a);
    std::fputc('\n', stderr);
    va_end(a);
    std::fflush(stderr);
}

// Every joystick SDL sees, and whether it knows its layout (a gamepad).
void list_controllers() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    if (n == 0) say("controllers: none found (keyboard and mouse only). One plugged in later is picked up.");
    for (int i = 0; i < n; ++i) {
        const char* name = SDL_GetJoystickNameForID(ids[i]);
        say("controllers: \"%s\" (vendor %04x, product %04x)%s", name ? name : "?", SDL_GetJoystickVendorForID(ids[i]),
            SDL_GetJoystickProductForID(ids[i]),
            SDL_IsGamepad(ids[i]) ? "" : ": SDL doesn't know its button layout, so it can't be used as a pad");
    }
    SDL_free(ids);
}

SDL_DisplayID display_named(const std::string& name) {
    if (name.empty()) return SDL_GetPrimaryDisplay();
    int n = 0;
    SDL_DisplayID found = 0;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {
        for (int i = 0; i < n && !found; ++i)
            if (const char* d = SDL_GetDisplayName(ids[i]); d && name == d) found = ids[i];
        SDL_free(ids);
    }
    if (!found) {
        SDL_Log("the settings' display \"%s\" isn't connected: using the main one", name.c_str());
        return SDL_GetPrimaryDisplay();
    }
    return found;
}
}  // namespace

struct Display::Impl {
    Options options;
    Picture picture;
    SDL_Window* window = nullptr;
    Gpu gpu;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    std::vector<VkSemaphore> rendered;  // one per swapchain image
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Buffer staging;
    Image frame;      // the uploaded frame
    Image target;     // headless: what's presented
    bool need_rebuild = false;
    bool vsync_built = true;  // the present mode the swapchain has
    // With vsync and VK_KHR_present_wait: each present has an id, and the next
    // frame waits until the one before is on screen, so at most one waits in
    // the queue (a frame less latency than a full FIFO queue). 0: none yet on
    // this swapchain.
    uint64_t present_id = 0, waitable_id = 0;
    PresentStats present_stats;
    uint32_t buttons = 0;
    // Every controller SDL lists is opened: the first it lists can be a
    // virtual pad or a wheel's base, with the player's real one after it.
    // Each takes a place (port A, then B, C, D) when it first has a button
    // pressed: the first whose controller is unplugged, else the next. No
    // one connected ever moves up; an unplugged controller (known by its
    // serial, or its kind and name) takes its place back when it's plugged
    // in again, unless another has taken it meanwhile.
    static constexpr unsigned kPlaces = 4;
    struct Pad {
        SDL_Gamepad* pad = nullptr;
        SDL_JoystickID id = 0;
        std::string key;
        Display::PadState state;  // as of the last poll
        uint32_t buttons = 0;     // with the stick's ways and the triggers as buttons
        uint16_t rumble_level = 0;
        bool pressed = false;  // a button seen since it was opened (said once)
    };
    std::vector<Pad> pads;
    std::vector<std::string> joined;  // the places' controllers, by key, in the order they joined
    bool log_input = false;    // QILIN_INPUT_LOG: every change of the buttons
    uint32_t logged = 0;

    void open_pad(SDL_JoystickID id) {
        for (const Pad& p : pads)
            if (p.id == id) return;
        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad) {
            say("controllers: can't open \"%s\": %s", SDL_GetGamepadNameForID(id) ? SDL_GetGamepadNameForID(id) : "?", SDL_GetError());
            return;
        }
        char guid[33]{};
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof guid);
        const char* identity = SDL_GetGamepadSerial(pad);
        std::string key = std::string(guid) + ":s:";
        if (!identity || !*identity) identity = SDL_GetGamepadName(pad), key = std::string(guid) + ":n:";
        constexpr char hex[] = "0123456789abcdef";
        if (identity)
            for (const unsigned char* at = reinterpret_cast<const unsigned char*>(identity); *at; ++at) key += hex[*at >> 4], key += hex[*at & 15];
        // Two of the same kind without serials: told apart by their order.
        const std::string family = key;
        for (unsigned index = 0;; ++index) {
            key = family + ":" + std::to_string(index);
            if (std::none_of(pads.begin(), pads.end(), [&](const Pad& o) { return o.key == key; })) break;
        }
        Pad entry;
        entry.pad = pad, entry.id = id, entry.key = key;
        pads.push_back(std::move(entry));
        const char* name = SDL_GetGamepadName(pad);
        const int place = place_of(key);
        if (place >= 0) say("controllers: \"%s\" is back, on port %c", name ? name : "a controller", 'A' + place);
        else say("controllers: \"%s\" connected (press a button and it takes the next port)", name ? name : "a controller");
    }
    void close_pad(SDL_JoystickID id) {
        for (size_t i = 0; i < pads.size(); ++i)
            if (pads[i].id == id) {
                const char* name = SDL_GetGamepadName(pads[i].pad);
                say("controllers: \"%s\" disconnected", name ? name : "the controller");
                SDL_CloseGamepad(pads[i].pad);
                pads.erase(pads.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
    }
    void open_all_pads() {
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            for (int i = 0; i < n; ++i) open_pad(ids[i]);
            SDL_free(ids);
        }
    }
    int place_of(const std::string& key) const {
        const auto it = std::find(joined.begin(), joined.end(), key);
        return it == joined.end() ? -1 : static_cast<int>(it - joined.begin());
    }
    Pad* at_place(unsigned place) {
        if (place >= joined.size()) return nullptr;
        for (Pad& p : pads)
            if (p.key == joined[place]) return &p;
        return nullptr;
    }
    const Pad* at_place(unsigned place) const { return const_cast<Impl*>(this)->at_place(place); }
    float stick_x = 0, stick_y = 0;
    bool moved = false;
    float mouse_x = 0, mouse_y = 0;  // window pixels
    float pic_x = 0, pic_y = 0, pic_w = 1, pic_h = 1;  // the picture's place, last present
    std::function<bool(const void*)> hook;
    std::function<void(VkCommandBuffer)> overlay;

    // The picture's drawing.
    VkShaderModule vert = VK_NULL_HANDLE, plain = VK_NULL_HANDLE, crt = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
    VkFormat pipeline_format = VK_FORMAT_UNDEFINED;
    VkPipeline pipelines[2] = {};  // by Look

    // What was shown last, for present_again.
    VkImage last_image = VK_NULL_HANDLE;  // null: the uploaded frame
    VkImageView last_view = VK_NULL_HANDLE;
    uint32_t last_w = 0, last_h = 0, last_lines = 0;
    float last_aspect = 16.0f / 9.0f;
    bool shown_any = false;
    FrameTimes times;
    std::string mode_note;  // what fullscreen chose, and why (even_mode)
    std::chrono::steady_clock::time_point last_new{};  // when the last new picture was presented
    bool counting = false;                            // last_new is set
    void count_new() {  // a new picture was presented
        const auto now = std::chrono::steady_clock::now();
        if (counting) times.add(std::chrono::duration<double, std::milli>(now - last_new).count());
        last_new = now, counting = true;
    }

    void show(VkImage src, VkImageView view, uint32_t sw, uint32_t sh, float aspect, uint32_t lines);

    void make_drawing() {
        vert = module(gpu, kPresentVert, sizeof kPresentVert);
        plain = module(gpu, kPlainFrag, sizeof kPlainFrag);
        crt = module(gpu, kCrtFrag, sizeof kCrtFrag);
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sli.bindingCount = 1;
        sli.pBindings = &b;
        check(vkCreateDescriptorSetLayout(gpu.device, &sli, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
        VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(gpu.device, &pli, nullptr, &layout), "vkCreatePipelineLayout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &size;
        check(vkCreateDescriptorPool(gpu.device, &dpi, nullptr, &dpool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        check(vkAllocateDescriptorSets(gpu.device, &ai, &set), "vkAllocateDescriptorSets");
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(gpu.device, &sci, nullptr, &linear), "vkCreateSampler");
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        check(vkCreateSampler(gpu.device, &sci, nullptr, &nearest), "vkCreateSampler");
    }

    void destroy_pipelines() {
        for (VkPipeline& p : pipelines) {
            if (p) vkDestroyPipeline(gpu.device, p, nullptr);
            p = VK_NULL_HANDLE;
        }
    }

    // The two looks' pipelines, for the format drawn into.
    void make_pipelines(VkFormat format) {
        if (format == pipeline_format) return;
        destroy_pipelines();
        pipeline_format = format;
        VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                  {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &ds;
        pci.layout = layout;
        const VkShaderModule frags[2] = {plain, crt};
        for (int i = 0; i < 2; ++i) {
            stages[1].module = frags[i];
            check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipelines[i]), "vkCreateGraphicsPipelines");
        }
    }

    void destroy_views() {
        for (VkImageView v : swap_views) vkDestroyImageView(gpu.device, v, nullptr);
        swap_views.clear();
    }

    void build_swapchain() {
        vkDeviceWaitIdle(gpu.device);
        VkSurfaceCapabilitiesKHR caps;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) die("the window's surface can't be drawn into");
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent
                                                          : VkExtent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
        if (extent.width == 0 || extent.height == 0) return;  // minimised
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, surface, &n, formats.data());
        VkSurfaceFormatKHR chosen = formats.at(0);
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = f;
        swap_format = chosen.format;
        need_format(gpu, swap_format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "the window's pixel format");
        // vsync: FIFO (every driver has it). Off: mailbox, else immediate, else FIFO after all.
        uint32_t mn = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physical, surface, &mn, nullptr);
        std::vector<VkPresentModeKHR> modes(mn);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physical, surface, &mn, modes.data());
        const auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!picture.vsync) mode = has(VK_PRESENT_MODE_MAILBOX_KHR)     ? VK_PRESENT_MODE_MAILBOX_KHR
                                   : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                        : VK_PRESENT_MODE_FIFO_KHR;
        vsync_built = picture.vsync;
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = std::min(caps.minImageCount + 1, caps.maxImageCount ? caps.maxImageCount : 8u);
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = mode;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = swapchain;
        VkSwapchainKHR next;
        check(vkCreateSwapchainKHR(gpu.device, &sci, nullptr, &next), "vkCreateSwapchainKHR");
        destroy_views();
        if (swapchain) vkDestroySwapchainKHR(gpu.device, swapchain, nullptr);
        swapchain = next;
        vkGetSwapchainImagesKHR(gpu.device, swapchain, &n, nullptr);
        swap_images.resize(n);
        vkGetSwapchainImagesKHR(gpu.device, swapchain, &n, swap_images.data());
        for (VkImage im : swap_images) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = im;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = swap_format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v;
            check(vkCreateImageView(gpu.device, &vci, nullptr, &v), "vkCreateImageView");
            swap_views.push_back(v);
        }
        for (VkSemaphore s : rendered) vkDestroySemaphore(gpu.device, s, nullptr);
        rendered.resize(n);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : rendered) check(vkCreateSemaphore(gpu.device, &si, nullptr, &s), "vkCreateSemaphore");
        make_pipelines(swap_format);
        need_rebuild = false;
        waitable_id = 0;  // ids belong to a swapchain
    }

    // The picture's place in a target this size: see Scaling.
    VkRect2D place(VkExtent2D size, float aspect, uint32_t lines) const {
        const float W = float(size.width), H = float(size.height);
        if (picture.scaling == Scaling::stretch) return {{0, 0}, size};
        if (picture.scaling == Scaling::integer && lines) {
            const uint32_t bh = lines, bw = uint32_t(std::lround(lines * aspect));
            const uint32_t k = std::min(size.width / bw, size.height / bh);
            if (k >= 1) return {{int32_t((size.width - bw * k) / 2), int32_t((size.height - bh * k) / 2)}, {bw * k, bh * k}};
        }
        float w = W, h = W / aspect;  // fit, and whole multiples where even 1x doesn't fit
        if (h > H) h = H, w = H * aspect;
        const int32_t x = int32_t((W - w) / 2), y = int32_t((H - h) / 2);
        return {{x, y}, {uint32_t(std::lround(w)), uint32_t(std::lround(h))}};
    }

    // Record: the source ready to sample (uploading the frame if it's ours),
    // then the drawing into dst, left in final_layout.
    void record(VkImage dst, VkImageView dst_view, VkExtent2D dst_size, VkImageLayout final_layout, float aspect, uint32_t lines,
                VkImage src) {
        if (src == VK_NULL_HANDLE) {
            transition(cmd, frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {frame.width, frame.height, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transition(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else {  // drawn by the caller: wait for it
            transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        transition(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = dst_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, dst_size};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
        const VkRect2D r = place(dst_size, aspect, lines);
        pic_x = float(r.offset.x), pic_y = float(r.offset.y), pic_w = float(r.extent.width), pic_h = float(r.extent.height);
        const VkViewport vp{pic_x, pic_y, pic_w, pic_h, 0, 1};
        const VkRect2D whole{{0, 0}, dst_size};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &whole);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[picture.look == Look::crt ? 1 : 0]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        const Push p{{lines ? float(std::lround(lines * aspect)) : pic_w, lines ? float(lines) : pic_h}, picture.scanlines, picture.mask};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        if (overlay) {
            const VkViewport full{0, 0, float(dst_size.width), float(dst_size.height), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &full);
            overlay(cmd);
        }
        vkCmdEndRendering(cmd);
        transition(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final_layout);
        if (src != VK_NULL_HANDLE)  // back as the caller gave it
            transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
};

Display::Display(const Options& options) : impl_(std::make_unique<Impl>()) {
    Impl& d = *impl_;
    d.options = options;
    d.picture = options.picture;
    if (options.headless) {
        d.gpu = Gpu::create({}, false);
        d.target = Image::create(d.gpu, options.width, options.height, kFrameFormat,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    } else {
        // Controllers work whichever window has the focus (the game is often
        // started from a console, which may keep it).
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) die("SDL can't start: %s", SDL_GetError());
        d.log_input = std::getenv("QILIN_INPUT_LOG") != nullptr;
        list_controllers();
        d.open_all_pads();  // those already connected (not only the ones SDL announces later)
        const SDL_DisplayID on = display_named(options.monitor);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, options.title);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, options.width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, options.height);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(on));
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(on));
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, options.fullscreen);
        d.window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        if (!d.window) die("can't open a window: %s", SDL_GetError());
        Uint32 n = 0;
        const char* const* ext = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!ext) die("SDL has no Vulkan surface support here: %s", SDL_GetError());
        d.gpu = Gpu::create(std::span(ext, n), true, make_sdl_surface, d.window, &d.surface);
    }
    need_format(d.gpu, kFrameFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
                "frames");
    d.make_drawing();
    if (d.window) d.build_swapchain();
    else d.make_pipelines(kFrameFormat);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d.gpu.family;
    check(vkCreateCommandPool(d.gpu.device, &pci, nullptr, &d.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = d.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(d.gpu.device, &cai, &d.cmd), "vkAllocateCommandBuffers");
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(d.gpu.device, &si, nullptr, &d.acquired), "vkCreateSemaphore");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(d.gpu.device, &fi, nullptr, &d.done), "vkCreateFence");
    if (d.window && options.fullscreen) set_fullscreen(true);  // created borderless: now the mode that shows frames evenly
}

Display::~Display() {
    Impl& d = *impl_;
    if (d.gpu.device) {
        vkDeviceWaitIdle(d.gpu.device);
        d.staging.destroy(d.gpu);
        d.frame.destroy(d.gpu);
        d.target.destroy(d.gpu);
        d.destroy_pipelines();
        vkDestroySampler(d.gpu.device, d.linear, nullptr);
        vkDestroySampler(d.gpu.device, d.nearest, nullptr);
        vkDestroyDescriptorPool(d.gpu.device, d.dpool, nullptr);
        vkDestroyPipelineLayout(d.gpu.device, d.layout, nullptr);
        vkDestroyDescriptorSetLayout(d.gpu.device, d.set_layout, nullptr);
        for (VkShaderModule m : {d.vert, d.plain, d.crt}) vkDestroyShaderModule(d.gpu.device, m, nullptr);
        for (VkSemaphore s : d.rendered) vkDestroySemaphore(d.gpu.device, s, nullptr);
        vkDestroySemaphore(d.gpu.device, d.acquired, nullptr);
        vkDestroyFence(d.gpu.device, d.done, nullptr);
        vkDestroyCommandPool(d.gpu.device, d.pool, nullptr);
        d.destroy_views();
        if (d.swapchain) vkDestroySwapchainKHR(d.gpu.device, d.swapchain, nullptr);
    }
    if (d.surface) vkDestroySurfaceKHR(d.gpu.instance, d.surface, nullptr);
    d.gpu.destroy();
    for (Impl::Pad& p : d.pads) {
        SDL_RumbleGamepad(p.pad, 0, 0, 0);
        SDL_CloseGamepad(p.pad);
    }
    d.pads.clear();
    if (d.window) {
        SDL_DestroyWindow(d.window);
        SDL_Quit();
    }
}

bool Display::poll() {
    Impl& d = *impl_;
    if (!d.window) return true;
    SDL_Event e;
    d.moved = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) d.need_rebuild = true;
        if (e.type == SDL_EVENT_JOYSTICK_ADDED && !SDL_IsGamepad(e.jdevice.which)) list_controllers();  // say why it isn't used
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) d.open_pad(e.gdevice.which);
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED) d.close_pad(e.gdevice.which);
        // Full screen, whatever else is going on.
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
            (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
            set_fullscreen(!fullscreen());
            continue;
        }
        if (d.hook && d.hook(&e)) continue;
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            const float density = SDL_GetWindowPixelDensity(d.window);
            d.mouse_x = e.motion.x * density;
            d.mouse_y = e.motion.y * density;
            d.moved = true;
        }
    }
    // Another window has the keyboard: the game sees nothing held, and no pad rumbles.
    if (!focused()) {
        d.buttons = 0, d.stick_x = d.stick_y = 0, d.moved = false;
        for (Impl::Pad& pad : d.pads) pad.state = {}, pad.buttons = 0;
        for (unsigned k = 0; k < Impl::kPlaces; ++k) rumble(k, 0.0f);
        return true;
    }
    uint32_t b = 0;
    d.stick_x = d.stick_y = 0;
    for (Impl::Pad& pad : d.pads) {
        SDL_Gamepad* p = pad.pad;
        struct Map {
            SDL_GamepadButton sdl;
            uint32_t bit;
        };
        static constexpr Map kMap[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_UP, kPadUp},         {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadLeft},     {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadRight},
            {SDL_GAMEPAD_BUTTON_SOUTH, kPadA},            {SDL_GAMEPAD_BUTTON_EAST, kPadB},
            {SDL_GAMEPAD_BUTTON_WEST, kPadX},             {SDL_GAMEPAD_BUTTON_NORTH, kPadY},
            {SDL_GAMEPAD_BUTTON_START, kPadStart},        {SDL_GAMEPAD_BUTTON_BACK, kPadBack},
            {SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide},        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLB},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRB},  {SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadL3},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadR3},
        };
        uint32_t held = 0;
        for (const Map& m : kMap)
            if (SDL_GetGamepadButton(p, m.sdl)) held |= m.bit;
        const auto unit = [p](SDL_GamepadAxis ax) { return std::clamp(SDL_GetGamepadAxis(p, ax) / 32767.0f, -1.0f, 1.0f); };
        Display::PadState& r = pad.state;
        r.connected = true;
        r.lx = unit(SDL_GAMEPAD_AXIS_LEFTX), r.ly = unit(SDL_GAMEPAD_AXIS_LEFTY);
        r.rx = unit(SDL_GAMEPAD_AXIS_RIGHTX), r.ry = unit(SDL_GAMEPAD_AXIS_RIGHTY);
        r.lt = std::max(0.0f, unit(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)), r.rt = std::max(0.0f, unit(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        r.buttons = held;
        // The triggers and the left stick's four ways, as buttons too (half way).
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) held |= kPadLT;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) held |= kPadRT;
        const int x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -16384) held |= kPadLeft;
        if (x > 16384) held |= kPadRight;
        if (y < -16384) held |= kPadUp;
        if (y > 16384) held |= kPadDown;
        pad.buttons = held;
        b |= held;
        if (!pad.pressed && held) {
            pad.pressed = true;
            const char* name = SDL_GetGamepadName(p);
            if (d.place_of(pad.key) < 0) {
                // The first place whose controller is unplugged, else the next.
                unsigned place = 0;
                while (place < d.joined.size() && d.at_place(place)) ++place;
                if (place < d.joined.size()) d.joined[place] = pad.key;
                else if (place < Impl::kPlaces) d.joined.push_back(pad.key);
                if (place < Impl::kPlaces) say("controllers: \"%s\" works (buttons 0x%x): port %c", name ? name : "the controller", held, char('A' + place));
                else say("controllers: \"%s\" works, but the four ports are taken", name ? name : "the controller");
            }
        }
        // The right stick (a dead zone of a quarter, then linear to 1): whichever pad's is pushed furthest.
        const auto axis = [](int v) {
            const float f = std::clamp(v / 32767.0f, -1.0f, 1.0f);
            return std::fabs(f) < 0.25f ? 0.0f : (f - std::copysign(0.25f, f)) / 0.75f;
        };
        const float sx = axis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX)), sy = -axis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY));
        if (sx * sx + sy * sy > d.stick_x * d.stick_x + d.stick_y * d.stick_y) d.stick_x = sx, d.stick_y = sy;
    }
    if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) b |= kPadClick;
    if (d.log_input && b != d.logged) say("input: buttons 0x%05x", b);
    d.logged = b;
    d.buttons = b;
    return true;
}

uint32_t Display::buttons() const { return impl_->buttons; }
int Display::key_code(const char* name) const {
    const SDL_Scancode sc = SDL_GetScancodeFromName(name);
    return sc == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<int>(sc);
}
bool Display::key_down(int code) const {
    if (impl_->window == nullptr || code < 0 || !focused()) return false;
    int n = 0;
    const bool* k = SDL_GetKeyboardState(&n);
    return code < n && k[code];
}
bool Display::focused() const { return impl_->window && (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_INPUT_FOCUS); }
Display::PadState Display::pad_state() const { return pad_state(0); }
Display::PadState Display::pad_state(unsigned player) const {
    const Impl::Pad* p = player < Impl::kPlaces ? impl_->at_place(player) : nullptr;
    return p ? p->state : PadState{};
}
std::string Display::player_controller(unsigned player) const {
    const Impl& d = *impl_;
    if (player >= d.joined.size()) return {};
    const Impl::Pad* p = d.at_place(player);
    if (!p) return "(unplugged: its place is kept)";
    const char* name = SDL_GetGamepadName(p->pad);
    return name ? name : "a controller";
}
unsigned Display::controllers_connected() const { return static_cast<unsigned>(impl_->pads.size()); }
void Display::reset_player_order() {
    Impl& d = *impl_;
    for (unsigned k = 0; k < Impl::kPlaces; ++k) rumble(k, 0.0f);
    d.joined.clear();
    for (Impl::Pad& p : d.pads) p.pressed = false;
}
void Display::right_stick(float& x, float& y) const {
    x = impl_->stick_x;
    y = impl_->stick_y;
}
bool Display::mouse_moved(float& x, float& y) const {
    const Impl& d = *impl_;
    if (!d.moved) return false;
    x = (d.mouse_x - d.pic_x) / d.pic_w * 2.0f - 1.0f;
    y = 1.0f - (d.mouse_y - d.pic_y) / d.pic_h * 2.0f;
    return true;
}

void Display::rumble(bool on) { rumble(0, on ? 1.0f : 0.0f); }
void Display::rumble(unsigned player, float strength) {
    Impl& d = *impl_;
    Impl::Pad* p = player < Impl::kPlaces ? d.at_place(player) : nullptr;
    if (!p) return;
    if (!focused()) strength = 0.0f;  // not while another window has the keyboard
    const uint16_t level = static_cast<uint16_t>(std::clamp(strength, 0.0f, 1.0f) * 65535.0f + 0.5f);
    if (level == p->rumble_level) return;
    p->rumble_level = level;
    SDL_RumbleGamepad(p->pad, level, level, level ? 60000 : 0);
}

void Display::Impl::show(VkImage src, VkImageView view, uint32_t sw, uint32_t sh, float aspect, uint32_t lines) {
    Impl& d = *this;
    last_image = src, last_view = view, last_w = sw, last_h = sh, last_aspect = aspect, last_lines = lines, shown_any = true;
    VkImage dst;
    VkImageView dst_view;
    VkExtent2D dst_size;
    uint32_t index = 0;
    if (d.window) {
        if (d.need_rebuild || d.vsync_built != d.picture.vsync) d.build_swapchain();
        if (d.extent.width == 0) return;  // minimised
        const auto waited_from = std::chrono::steady_clock::now();
        if (d.gpu.present_wait && d.vsync_built && d.waitable_id) {
            // Bounded: a display that stops refreshing (asleep, covered) only costs 50 ms.
            const VkResult w = vkWaitForPresentKHR(d.gpu.device, d.swapchain, d.waitable_id, 50'000'000);
            if (w == VK_ERROR_OUT_OF_DATE_KHR) {
                d.need_rebuild = true;
                return;
            }
            if (w != VK_TIMEOUT && w != VK_SUBOPTIMAL_KHR) check(w, "vkWaitForPresentKHR");
        }
        const VkResult r = vkAcquireNextImageKHR(d.gpu.device, d.swapchain, UINT64_MAX, d.acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            d.need_rebuild = true;
            return;
        }
        if (r != VK_SUBOPTIMAL_KHR) check(r, "vkAcquireNextImageKHR");
        ++d.present_stats.presents;
        d.present_stats.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waited_from).count();
        dst = d.swap_images[index];
        dst_view = d.swap_views[index];
        dst_size = d.extent;
    } else {
        dst = d.target.image;
        dst_view = d.target.view;
        dst_size = {d.target.width, d.target.height};
    }
    // The source, through the sampler the settings ask for (the fence was
    // waited for: the set isn't in use).
    VkDescriptorImageInfo ii{d.picture.smooth || d.picture.look == Look::crt ? d.linear : d.nearest,
                             src ? view : d.frame.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = d.set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d.gpu.device, 1, &wr, 0, nullptr);

    check(vkResetFences(d.gpu.device, 1, &d.done), "vkResetFences");
    check(vkResetCommandBuffer(d.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(d.cmd, &bi), "vkBeginCommandBuffer");
    d.record(dst, dst_view, dst_size, d.window ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect,
             lines, src);
    check(vkEndCommandBuffer(d.cmd), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = d.cmd;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = d.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    if (d.window) {
        signal.semaphore = d.rendered[index];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
    }
    check(vkQueueSubmit2(d.gpu.queue, 1, &si, d.done), "vkQueueSubmit2");
    if (d.window) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &d.rendered[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &d.swapchain;
        pi.pImageIndices = &index;
        const uint64_t id = ++d.present_id;
        VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        pid.swapchainCount = 1;
        pid.pPresentIds = &id;
        if (d.gpu.present_wait) pi.pNext = &pid;
        const VkResult r = vkQueuePresentKHR(d.gpu.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) d.need_rebuild = true;
        else check(r, "vkQueuePresentKHR");
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) d.waitable_id = id;
    }
}

void Display::present(uint32_t width, uint32_t height, const uint8_t* rgba, float aspect, uint32_t lines) {
    Impl& d = *impl_;
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const VkDeviceSize bytes = VkDeviceSize{width} * height * 4;
    if (d.staging.size < bytes) {
        d.staging.destroy(d.gpu);
        d.staging = Buffer::host_visible(d.gpu, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    if (d.frame.width != width || d.frame.height != height) {
        d.frame.destroy(d.gpu);
        d.frame = Image::create(d.gpu, width, height, kFrameFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    std::memcpy(d.staging.mapped, rgba, bytes);
    d.show(VK_NULL_HANDLE, VK_NULL_HANDLE, width, height, aspect, lines);
    d.count_new();
}

void Display::present_image(VkImage image, VkImageView view, uint32_t width, uint32_t height, float aspect, uint32_t lines) {
    Impl& d = *impl_;
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    d.show(image, view, width, height, aspect, lines);
    d.count_new();
}

Display::PresentStats Display::take_present_stats() {
    const PresentStats s = impl_->present_stats;
    impl_->present_stats = {};
    return s;
}
const FrameTimes& Display::frame_times() const { return impl_->times; }

void Display::present_again() {
    Impl& d = *impl_;
    d.counting = false;  // the pause isn't a frame time
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    if (!d.shown_any) {  // nothing yet: black
        static const uint8_t black[4] = {0, 0, 0, 255};
        present(1, 1, black, 16.0f / 9.0f, 0);
        return;
    }
    d.show(d.last_image, d.last_view, d.last_w, d.last_h, d.last_aspect, d.last_lines);
}

Gpu& Display::gpu() { return impl_->gpu; }

std::vector<uint8_t> Display::read_back() {
    Impl& d = *impl_;
    if (d.window) die("read_back is for headless displays");
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    Buffer out = Buffer::host_visible(d.gpu, VkDeviceSize{d.target.width} * d.target.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    check(vkResetFences(d.gpu.device, 1, &d.done), "vkResetFences");
    check(vkResetCommandBuffer(d.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(d.cmd, &bi), "vkBeginCommandBuffer");
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {d.target.width, d.target.height, 1};
    vkCmdCopyImageToBuffer(d.cmd, d.target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer, 1, &region);
    check(vkEndCommandBuffer(d.cmd), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = d.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    check(vkQueueSubmit2(d.gpu.queue, 1, &si, d.done), "vkQueueSubmit2");
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const auto* p = static_cast<const uint8_t*>(out.mapped);
    std::vector<uint8_t> pixels(p, p + out.size);
    out.destroy(d.gpu);
    return pixels;
}

const std::string& Display::device_name() const { return impl_->gpu.name; }

void Display::set_picture(const Picture& p) { impl_->picture = p; }
const Picture& Display::picture() const { return impl_->picture; }

bool Display::fullscreen() const { return impl_->window && (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_FULLSCREEN) != 0; }
// The fullscreen mode for even frames: the desktop's (borderless) when its
// refresh is a whole multiple of 60 Hz (or of 59.94) within 0.5%, or with
// variable refresh; else the highest mode at the same size that is, if any.
static const SDL_DisplayMode* even_mode(SDL_Window* w, const Picture& p, std::string& why) {
    const SDL_DisplayID id = SDL_GetDisplayForWindow(w);
    const SDL_DisplayMode* desk = SDL_GetDesktopDisplayMode(id);
    if (!desk || p.variable_refresh || !p.match_refresh || !p.vsync) return nullptr;
    const auto even = [](const SDL_DisplayMode& m) {
        const double hz = m.refresh_rate_numerator && m.refresh_rate_denominator
                              ? double(m.refresh_rate_numerator) / double(m.refresh_rate_denominator) : double(m.refresh_rate);
        for (int k = 1; k <= 8; ++k)
            if (std::fabs(hz / k - 60.0) <= 0.3 || std::fabs(hz / k - 59.94) <= 0.3) return true;
        return false;
    };
    if (even(*desk)) return nullptr;
    int n = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &n);
    const SDL_DisplayMode* best = nullptr;
    for (int i = 0; modes && i < n; ++i)
        if (modes[i]->w == desk->w && modes[i]->h == desk->h && even(*modes[i]) &&
            (!best || modes[i]->refresh_rate > best->refresh_rate))
            best = modes[i];
    char line[320];
    if (best)
        std::snprintf(line, sizeof line, "fullscreen at %dx%d %.2f Hz: the desktop's %.2f Hz isn't a multiple of 60 Hz (frames would be uneven)",
                      best->w, best->h, double(best->refresh_rate), double(desk->refresh_rate));
    else
        std::snprintf(line, sizeof line, "the desktop's %.2f Hz isn't a multiple of 60 Hz and the display has no mode at %dx%d that is: "
                      "frames are uneven (variable refresh, if the display has it, shows them evenly)",
                      double(desk->refresh_rate), desk->w, desk->h);
    why = line;
    static SDL_DisplayMode chosen;  // SDL keeps a pointer to the mode it's given only for the call
    if (best) chosen = *best;
    SDL_free(modes);
    return best ? &chosen : nullptr;
}

void Display::set_fullscreen(bool on) {
    if (!impl_->window) return;
    std::string why;
    const SDL_DisplayMode* mode = on ? even_mode(impl_->window, impl_->picture, why) : nullptr;
    impl_->mode_note = why;
    SDL_SetWindowFullscreenMode(impl_->window, mode);  // nullptr: borderless, at the desktop's mode
    SDL_SetWindowFullscreen(impl_->window, on);
}
const std::string& Display::mode_note() const { return impl_->mode_note; }

std::vector<std::string> Display::monitors() const {
    std::vector<std::string> out;
    if (!impl_->window) return out;
    int n = 0;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {
        for (int i = 0; i < n; ++i)
            if (const char* name = SDL_GetDisplayName(ids[i])) out.push_back(name);
        SDL_free(ids);
    }
    return out;
}
void Display::move_to(const std::string& monitor) {
    SDL_Window* w = impl_->window;
    if (!w) return;
    const SDL_DisplayID id = display_named(monitor);
    const bool full = fullscreen();
    if (full) SDL_SetWindowFullscreen(w, false);
    const int at = int(SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SDL_SetWindowPosition(w, at, at);
    if (full) SDL_SetWindowFullscreen(w, true);
}

void* Display::sdl_window() const { return impl_->window; }
VkFormat Display::swap_format() const { return impl_->window ? impl_->swap_format : VK_FORMAT_UNDEFINED; }
uint32_t Display::swap_image_count() const { return static_cast<uint32_t>(impl_->swap_images.size()); }
void Display::set_event_hook(std::function<bool(const void*)> hook) { impl_->hook = std::move(hook); }
void Display::set_overlay(std::function<void(VkCommandBuffer)> overlay) { impl_->overlay = std::move(overlay); }

}  // namespace qilin::host
