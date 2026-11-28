// Before building: is there a Vulkan 1.3 device to play on? The game draws
// only with Vulkan 1.3 (host/src/vulkan.cpp), so a computer without one
// would build for minutes and then not start. A minimal instance through
// volk, the same loader the game uses; nothing is drawn.
#pragma once

#include <expected>
#include <string>

namespace qilin::launcher {

// Nothing if the game will find a device; else a plain message for the
// player: what this computer has and what to do (on a Raspberry Pi, update
// Raspberry Pi OS; elsewhere, install the graphics driver's Vulkan support).
// Where there is a graphics processor, a software device (Mesa's llvmpipe)
// doesn't count: a Raspberry Pi on an older Raspberry Pi OS has llvmpipe's
// Vulkan 1.3 beside its own 1.2 driver, and the game would crawl on it.
std::expected<void, std::string> check_vulkan();

}  // namespace qilin::launcher
