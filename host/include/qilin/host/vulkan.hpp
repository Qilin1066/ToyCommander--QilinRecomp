// Vulkan 1.3: the instance, the one device we draw with, and small helpers.
//
// The baseline (docs/roadmap-and-design-targets.md, "Vulkan baseline"): 1.3
// core, and of its optional features only what Mesa's v3dv driver gives a
// Raspberry Pi 4. Anything required is checked when the device is created, and
// a missing piece stops the program with its name: there are no fallbacks.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <volk.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace qilin::host {

[[noreturn]] void die(const char* format, ...);
void check(VkResult r, const char* what);  // dies with the call's name and the error

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;  // graphics, transfer and (with a surface) present
    uint32_t family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::string name;
    // Optional features, enabled where the device has them (the renderer
    // approximates without them, and says so once): logic ops, line
    // widths and its point sizes.
    bool logic_op = false, wide_lines = false, large_points = false;
    float max_line_width = 1.0f, max_point_size = 1.0f;
    // VK_KHR_present_id + VK_KHR_present_wait: the display can wait until a
    // frame is on screen, to keep at most one queued behind it (latency).
    bool present_wait = false;
    // VK_EXT_extended_dynamic_state3's blend enable, equation and write mask:
    // one pipeline serves every blend state, set per draw instead of compiled
    // in (no pipeline to build when the game uses a new one). dynamic_logic:
    // the logic op too (with VK_EXT_extended_dynamic_state2's). QILIN_NO_DYNAMIC_BLEND=1: off.
    bool dynamic_blend = false, dynamic_logic = false;
    // Anisotropic texture filtering: the most the device allows, or 1 without it.
    float max_anisotropy = 1.0f;
    // Stores and atomics in fragment shaders (the renderer's per-pixel lists), where the device has them.
    bool fragment_atomics = false;
    // Multisampling: the most samples a pixel the device renders colour,
    // depth and stencil with together (1, 2, 4, 8; every 1.3 device has 4).
    uint32_t max_samples = 1;
    // Every device here that could draw (the settings menu offers them).
    std::vector<std::string> usable;

    // `instance_extensions`: what the window system needs (none when headless).
    // `surface`: if given, the queue must be able to present to it.
    static Gpu create(std::span<const char* const> instance_extensions, bool want_swapchain,
                      VkSurfaceKHR (*make_surface)(VkInstance, void*) = nullptr, void* surface_arg = nullptr,
                      VkSurfaceKHR* surface_out = nullptr);
    void destroy();

    // The device to use when it's there (by its name; empty: the best one).
    // Set before create.
    static void prefer(const std::string& name);

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) const;
};

// A buffer or image with its own memory (the renderer will pool these).
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    static Buffer host_visible(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage);
    // In the GPU's own memory, not mapped (written by the GPU, or by copies).
    static Buffer device_local(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage);
    void destroy(const Gpu& gpu);
};
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1;
    // Depth formats get a depth view (with the stencil, for a format that has
    // one); others a colour view of every level. `samples` > 1: a
    // multisampled image (one level), to render into and resolve.
    static Image create(const Gpu& gpu, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                        uint32_t levels = 1, uint32_t samples = 1);
    uint32_t samples = 1;
    void destroy(const Gpu& gpu);
};

// One layout transition with synchronization2, for everything the image needs
// (and an execution and memory dependency on all earlier work on the queue).
void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
bool is_depth(VkFormat format);

}  // namespace qilin::host
