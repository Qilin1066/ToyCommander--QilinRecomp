// The guest's time: what the timers, the video timing, the sound and the
// frame pacing read. The host's steady clock since start, less the time the
// game spent paused (the settings menu).
#pragma once

#include <cstdint>

namespace dreamcast {

uint64_t guest_ns();
// Stops the guest's time (true) and starts it again (false).
void pause_guest_time(bool paused);

}  // namespace dreamcast
