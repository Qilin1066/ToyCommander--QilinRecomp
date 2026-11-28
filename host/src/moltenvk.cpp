// MoltenVK, linked in statically on iPadOS and iOS (QILIN_IOS builds only):
// its own vkGetInstanceProcAddr, for volk (vulkan.cpp). Apps can't dlopen a
// library of their own there, so volk is given this instead of looking one
// up. This file doesn't include volk.h: volk's function pointers have the
// same names (in namespace volk on iOS, CMakeLists.txt).
#include <vulkan/vulkan_core.h>  // VK_NO_PROTOTYPES is defined: the types only

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name);

namespace qilin::host {

PFN_vkGetInstanceProcAddr moltenvk_loader() { return &vkGetInstanceProcAddr; }

}  // namespace qilin::host
