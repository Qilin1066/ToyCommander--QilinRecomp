// The native code the port registers for guest functions it replaces.
#pragma once

namespace toycommander {

// Registers every native replacement (rt::provide).
void provide_natives();

}  // namespace toycommander
