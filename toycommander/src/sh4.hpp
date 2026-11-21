// The SH-4's on-chip peripherals that lifted code programs directly: the
// timer unit (TMU), at 0xFFD80000. (The store queues and QACR are the bus's.)
#pragma once

#include <cstdint>

namespace toycommander {

void map_sh4_peripherals();

// What Shinobi's timer set-up (0x8C0B4958) does: timer 0 stopped, counting at
// Pφ/64 with no interrupt, from 0xFFFFFFFF with 0xFFFFFFFF to reload, started.
void start_free_running_timer0();
// Timer 0's counter, as a read of TCNT0 gives it.
uint32_t timer0_count();

}  // namespace toycommander
