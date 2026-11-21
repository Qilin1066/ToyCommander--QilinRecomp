// The guest's time: what the timers, the video timing and the frame pacing
// read. For now the host's steady clock since start.
#pragma once

#include <cstdint>

namespace toycommander {

uint64_t guest_ns();

}  // namespace toycommander
