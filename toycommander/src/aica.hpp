// The sound chip (AICA) as far as the port needs one: its 2 MB of sound RAM
// (physical 0x00800000, the G2 bus's view of it) and its registers
// (0x00700000-0x0070FFFF) as a register file. The chip's ARM CPU and its
// driver (AUDIO64.DRV) are never run: the audio manager's commands to the
// driver are answered natively (audio.cpp), and its voices are mixed by the
// port. What the audio manager writes to the registers is kept; the ARM's
// reset register is recorded and nothing starts.
#pragma once

#include <cstdint>

namespace toycommander {

void map_aica();
// Sound RAM, 2 MB.
uint8_t* sound_ram();

}  // namespace toycommander
