// The Vulkan check before a build (vulkan_check.hpp).
#include "vulkan_check.hpp"

#include <volk.h>

#include <fstream>
#include <iterator>
#include <vector>

namespace qilin::launcher {

#if !defined(__APPLE__)
namespace {

std::string version_text(uint32_t v) { return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)); }

#if defined(__linux__)
// The board's name from the device tree ("Raspberry Pi 4 Model B Rev 1.4").
bool raspberry_pi() {
    for (const char* file : {"/proc/device-tree/model", "/sys/firmware/devicetree/base/model"}) {
        std::ifstream f(file, std::ios::binary);
        const std::string model{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        if (model.find("Raspberry Pi") != std::string::npos) return true;
    }
    return false;
}
#endif

// What to do about it, for this computer.
std::string advice() {
#if defined(_WIN32)
    return "Update your graphics driver: any recent NVIDIA, AMD or Intel driver has Vulkan 1.3.";
#else
#if defined(__linux__)
    if (raspberry_pi()) return "Update Raspberry Pi OS: Trixie or later has Vulkan 1.3 for the Raspberry Pi 4 and 5.";
#endif
    return "Install your graphics driver's Vulkan support (Mesa's mesa-vulkan-drivers, or your NVIDIA driver's), then start this again.";
#endif
}

}  // namespace
#endif

std::expected<void, std::string> check_vulkan() {
#if defined(__APPLE__)
    return {};  // the Mac download carries MoltenVK in the app, where the game (not this check) finds it
#else
    const std::string needs = "The game needs a graphics driver with Vulkan 1.3. ";
    if (volkInitialize() != VK_SUCCESS) return std::unexpected(needs + "This computer has no Vulkan driver at all. " + advice());
    const uint32_t loader = volkGetInstanceVersion();
    if (loader < VK_API_VERSION_1_3)
        return std::unexpected(needs + "This computer's Vulkan is version " + version_text(loader) + ". " + advice());

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "QilinRecomp launcher";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        return std::unexpected(needs + "Vulkan doesn't start on this computer. " + advice());
    volkLoadInstanceOnly(instance);
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count) vkEnumeratePhysicalDevices(instance, &count, devices.data());
    devices.resize(count);

    // A graphics processor with 1.3; or, where there's none at all (a
    // virtual machine), a software device with 1.3.
    bool hardware = false, hardware_ok = false, software_ok = false;
    std::string best_name;
    uint32_t best = 0;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        const bool software = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        const bool ok = p.apiVersion >= VK_API_VERSION_1_3;
        if (software) {
            software_ok = software_ok || ok;
            continue;
        }
        hardware = true;
        hardware_ok = hardware_ok || ok;
        if (p.apiVersion >= best) {
            best = p.apiVersion;
            best_name = p.deviceName;
        }
    }
    vkDestroyInstance(instance, nullptr);
    if (hardware_ok || (!hardware && software_ok)) return {};
    if (hardware)
        return std::unexpected(needs + "This computer's graphics (" + best_name + ") has Vulkan " + version_text(best) + ". " + advice());
    return std::unexpected(needs + "Vulkan finds no graphics device on this computer. " + advice());
#endif
}

}  // namespace qilin::launcher
