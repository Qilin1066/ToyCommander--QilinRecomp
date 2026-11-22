// What a game's port gives the shared Dreamcast code (dreamcast/src): the
// parts of the console's system software it replaces that the hardware's
// side needs. Each game's program defines these (toycommander/src for Toy
// Commander), because where they live in guest memory is the game's build's.
#pragma once

#include <cstdint>

#include "guest.hpp"

namespace dreamcast::game {

// The handler code in the system software's exception table at VBR +
// `offset` (0x224: the graphics chip's level-6 interrupts), or 0.
uint32_t interrupt_handler(uint32_t offset);
// Runs the handlers the system software chained on a system bus event
// (0xA00 + 0x20 n, 0xB00 + 0x20 n), if any, as its level-4 handler would.
void raise_bus_event(qilin::rt::Context& c, uint32_t event);
// The console's own settings the game reads: its language (1 English,
// 2 German, 3 French, 4 Spanish) and stereo or mono sound.
void set_console(unsigned language, bool stereo);
// Called when the game changes the console's sound mode itself (the system
// software writes it to flash memory): the settings file keeps it.
void on_sound_mode(void (*save)(bool stereo));
// The game's own screen mode option is 16:9 rather than 4:3. Both games
// have one (Toy Commander's "Wide Screen (16/9)", Toy Racer's "Screen mode:
// 16/9"): the camera's view is widened by 4/3 and the picture drawn
// anamorphic, a 16:9 scene in the same 640x480 for a widescreen television
// to stretch, so the port shows it at 16:9.
bool widescreen();
// A movie is playing. The movies are 4:3 pictures in the game's 640x480
// whatever its screen mode, so they're shown at 4:3, never stretched.
bool movie_playing();

}  // namespace dreamcast::game
