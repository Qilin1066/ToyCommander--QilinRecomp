// The native code the port registers for guest functions it replaces.
#pragma once

#include <cstdint>

namespace toycommander {

// Registers every native replacement (rt::provide).
void provide_natives();
// A movie is on screen (play_movie, 0x8C04C700, is running).
bool movie_playing();

}  // namespace toycommander
