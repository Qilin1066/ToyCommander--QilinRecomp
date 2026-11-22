// Vulkan 1.3 device setup (vulkan.hpp).
#include "qilin/host/vulkan.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace qilin::host {

void die(const char* format, ...) {
    std::fputs(QILIN_APP ": ", stderr);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(nullptr);
    std::_Exit(70);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) die("Vulkan: %s failed (VkResult %d)", what, static_cast<int>(r));
}

namespace {

bool has_extension(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, ext.data());
    for (const auto& e : ext)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// What the renderer requires beyond 1.3's mandatory features: each is in
// v3dv's set on a Pi 4. Missing one names it.
struct Requirement {
    const char* name;
    VkBool32 VkPhysicalDeviceVulkan13Features::* f13;
};
constexpr Requirement kRequired13[] = {
    {"dynamicRendering", &VkPhysicalDeviceVulkan13Features::dynamicRendering},
    {"synchronization2", &VkPhysicalDeviceVulkan13Features::synchronization2},
};

}  // namespace

namespace {
std::string g_preferred;
}  // namespace
void Gpu::prefer(const std::string& name) { g_preferred = name; }

Gpu Gpu::create(std::span<const char* const> instance_extensions, bool want_swapchain,
                VkSurfaceKHR (*make_surface)(VkInstance, void*), void* surface_arg, VkSurfaceKHR* surface_out) {
    if (volkInitialize() != VK_SUCCESS)
        die("no Vulkan loader was found: install your GPU's Vulkan 1.3 driver (on macOS, MoltenVK)");
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3)
        die("the Vulkan loader is older than 1.3: update your GPU driver");

    Gpu g;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = QILIN_APP_TITLE " (Qilin recompilation)";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char*> ext(instance_extensions.begin(), instance_extensions.end());
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#if defined(__APPLE__)
    // MoltenVK is a portability implementation: it's only listed if asked for.
    ext.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(ext.size());
    ici.ppEnabledExtensionNames = ext.data();
    check(vkCreateInstance(&ici, nullptr, &g.instance), "vkCreateInstance");
    volkLoadInstance(g.instance);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (make_surface) {
        surface = make_surface(g.instance, surface_arg);
        if (surface_out) *surface_out = surface;
    }

    // The first device with 1.3, the features, a queue for graphics (and the
    // surface), preferring a discrete GPU, then an integrated one.
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(g.instance, &n, devices.data());
    std::string rejected;
    int best_score = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        auto reject = [&](const char* why) {
            rejected += std::string("\n  ") + p.deviceName + ": " + why;
        };
        if (p.apiVersion < VK_API_VERSION_1_3) {
            reject("Vulkan older than 1.3");
            continue;
        }
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        const Requirement* missing = nullptr;
        for (const auto& r : kRequired13)
            if (!(f13.*r.f13)) missing = &r;
        if (missing) {
            reject((std::string("no ") + missing->name).c_str());
            continue;
        }
        if (want_swapchain && !has_extension(pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            reject("no swapchain");
            continue;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        int family = -1;
        for (uint32_t i = 0; i < qn && family < 0; ++i) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_TRUE;
            if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
            if (present) family = static_cast<int>(i);
        }
        if (family < 0) {
            reject("no queue that draws and presents");
            continue;
        }
        g.usable.push_back(p.deviceName);
        const int score = !g_preferred.empty() && g_preferred == p.deviceName           ? 10
                          : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 3
                          : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                   : 1;
        if (score > best_score) {
            best_score = score;
            g.physical = pd;
            g.family = static_cast<uint32_t>(family);
            g.properties = p;
        }
    }
    if (!g.physical)
        die("no GPU here has what the renderer needs (Vulkan 1.3 with dynamic rendering and synchronization2)%s",
            rejected.empty() ? ": no Vulkan devices at all" : rejected.c_str());
    g.name = g.properties.deviceName;
    if (!g_preferred.empty() && g.name != g_preferred)
        std::fprintf(stderr, "the settings' graphics device \"%s\" isn't here: using %s\n", g_preferred.c_str(), g.name.c_str());
    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    for (const auto& r : kRequired13) f13.*r.f13 = VK_TRUE;
    // Optional features: on where the device has them.
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(g.physical, &have);
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    f2.features.logicOp = have.logicOp;
    f2.features.wideLines = have.wideLines;
    f2.features.largePoints = have.largePoints;
    f2.features.samplerAnisotropy = have.samplerAnisotropy;
    f2.features.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
    g.fragment_atomics = have.fragmentStoresAndAtomics;
    if (have.samplerAnisotropy) g.max_anisotropy = g.properties.limits.maxSamplerAnisotropy;
    g.logic_op = have.logicOp;
    g.wide_lines = have.wideLines;
    g.large_points = have.largePoints;
    if (g.wide_lines) g.max_line_width = g.properties.limits.lineWidthRange[1];
    if (g.large_points) g.max_point_size = g.properties.limits.pointSizeRange[1];
    std::vector<const char*> dext;
    if (want_swapchain) dext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // Optional: waiting for a frame to reach the screen (see Gpu::present_wait).
    // QILIN_NO_PRESENT_WAIT=1 leaves it off (a driver that misbehaves with it).
    // Optional: blend state set per draw (see Gpu::dynamic_blend).
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT feds2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    {
        const char* off = std::getenv("QILIN_NO_DYNAMIC_BLEND");
        if (!(off && *off && *off != '0') && has_extension(g.physical, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)) {
            const bool has2 = has_extension(g.physical, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
            feds3.pNext = has2 ? &feds2 : nullptr;
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &feds3};
            vkGetPhysicalDeviceFeatures2(g.physical, &q);
            if (feds3.extendedDynamicState3ColorBlendEnable && feds3.extendedDynamicState3ColorBlendEquation &&
                feds3.extendedDynamicState3ColorWriteMask) {
                g.dynamic_blend = true;
                g.dynamic_logic = g.logic_op && has2 && feds2.extendedDynamicState2LogicOp && feds3.extendedDynamicState3LogicOpEnable;
                // Enable only what's used.
                VkPhysicalDeviceExtendedDynamicState3FeaturesEXT on3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
                on3.extendedDynamicState3ColorBlendEnable = on3.extendedDynamicState3ColorBlendEquation =
                    on3.extendedDynamicState3ColorWriteMask = VK_TRUE;
                on3.extendedDynamicState3LogicOpEnable = g.dynamic_logic;
                feds3 = on3;
                dext.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
                if (g.dynamic_logic) {
                    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT on2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
                    on2.extendedDynamicState2LogicOp = VK_TRUE;
                    feds2 = on2;
                    feds2.pNext = f13.pNext;
                    feds3.pNext = &feds2;
                    dext.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
                } else {
                    feds3.pNext = f13.pNext;
                }
                f13.pNext = &feds3;
            }
        }
    }
    VkPhysicalDevicePresentIdFeaturesKHR fid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR fwait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    {
        const char* off = std::getenv("QILIN_NO_PRESENT_WAIT");
        if (want_swapchain && !(off && *off && *off != '0') && has_extension(g.physical, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
            has_extension(g.physical, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
            fid.pNext = &fwait;
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fid};
            vkGetPhysicalDeviceFeatures2(g.physical, &q);
            if (fid.presentId && fwait.presentWait) {
                g.present_wait = true;
                dext.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                dext.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
                fwait.pNext = f13.pNext;
                f13.pNext = &fid;  // into the device's feature chain
            }
        }
    }
#if defined(__APPLE__)
    if (has_extension(g.physical, "VK_KHR_portability_subset")) dext.push_back("VK_KHR_portability_subset");
#endif
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dext.size());
    dci.ppEnabledExtensionNames = dext.data();
    check(vkCreateDevice(g.physical, &dci, nullptr, &g.device), "vkCreateDevice");
    volkLoadDevice(g.device);
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
    return g;
}

void Gpu::destroy() {
    if (device) vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

uint32_t Gpu::memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) return i;
    die("Vulkan: no memory type with properties %#x", static_cast<unsigned>(want));
}

Buffer Buffer::host_visible(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    check(vkCreateBuffer(gpu.device, &bci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gpu.device, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(gpu.device, &mai, nullptr, &b.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(gpu.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    check(vkMapMemory(gpu.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    return b;
}

Buffer Buffer::device_local(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    check(vkCreateBuffer(gpu.device, &bci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gpu.device, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(gpu.device, &mai, nullptr, &b.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(gpu.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    return b;
}

void Buffer::destroy(const Gpu& gpu) {
    if (buffer) vkDestroyBuffer(gpu.device, buffer, nullptr);
    if (memory) vkFreeMemory(gpu.device, memory, nullptr);
    *this = {};
}

bool is_depth(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM ||
           f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

Image Image::create(const Gpu& gpu, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, uint32_t levels) {
    Image im;
    im.format = format;
    im.width = width;
    im.height = height;
    im.levels = levels;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(gpu.device, &ici, nullptr, &im.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gpu.device, im.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(gpu.device, &mai, nullptr, &im.memory), "vkAllocateMemory");
    check(vkBindImageMemory(gpu.device, im.image, im.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    const bool stencil = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    vci.subresourceRange = {is_depth(format) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                             : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                            0, levels, 0, 1};
    check(vkCreateImageView(gpu.device, &vci, nullptr, &im.view), "vkCreateImageView");
    return im;
}

void Image::destroy(const Gpu& gpu) {
    if (view) vkDestroyImageView(gpu.device, view, nullptr);
    if (image) vkDestroyImage(gpu.device, image, nullptr);
    if (memory) vkFreeMemory(gpu.device, memory, nullptr);
    *this = {};
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

}  // namespace qilin::host
