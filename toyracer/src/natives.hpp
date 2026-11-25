// The native code the port registers for guest functions it replaces.
#pragma once

#include <cstdint>

namespace toyracer {

// Registers every native replacement (rt::provide).
void provide_natives();

}  // namespace toyracer
