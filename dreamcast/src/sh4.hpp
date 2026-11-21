// The SH-4's on-chip peripherals that lifted code programs directly: the
// timer unit (TMU), at 0xFFD80000, and the DMA controller (DMAC), at
// 0xFFA00000. (The store queues and QACR are the bus's.)
#pragma once

#include <cstdint>

namespace dreamcast {

void map_sh4_peripherals();

// What Shinobi's timer set-up (0x8C0B4958) does: timer 0 stopped, counting at
// Pφ/64 with no interrupt, from 0xFFFFFFFF with 0xFFFFFFFF to reload, started.
void start_free_running_timer0();
// Timer 0's counter, as a read of TCNT0 gives it.
uint32_t timer0_count();

// DMA channel 2 as programmed: its source, its count of 32-byte blocks, its
// control register and the controller's operation register.
struct Dma2 {
    uint32_t sar, dmatcr, chcr, dmaor;
};
Dma2 dma_channel2();
// Channel 2 has moved `bytes`: SAR2 advanced, DMATCR2 zero, CHCR2.TE set.
void end_dma_channel2(uint32_t bytes);

}  // namespace dreamcast
