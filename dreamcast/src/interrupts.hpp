// Interrupt delivery: the system bus's events reach the handlers the code
// installed, as the console's interrupt controller would deliver them, at
// the points where lifted code polls (guest.hpp). And the video timing: the
// vertical blank, 60 times a second of guest time.
#pragma once

namespace dreamcast {

// Installs the service and starts the video clock.
void start_interrupts();
// An event was raised: have the next poll look.
void interrupt_pending();

}  // namespace dreamcast
