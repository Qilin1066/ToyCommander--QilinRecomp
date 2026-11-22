// What Toy Commander's port gives the shared Dreamcast code (dreamcast/src/game.hpp).
#include "game.hpp"

#include "chain.hpp"
#include "natives.hpp"

namespace dreamcast::game {

uint32_t interrupt_handler(uint32_t offset) { return toycommander::interrupt_handler(offset); }
void raise_bus_event(qilin::rt::Context& c, uint32_t event) { toycommander::raise_bus_event(c, event); }
void set_console(unsigned language, bool stereo) { toycommander::set_console(language, stereo); }
void on_sound_mode(void (*save)(bool stereo)) { toycommander::on_sound_mode(save); }
// The options' screen mode word (0 standard, else wide), which 0x8C018F42 follows when it scales the
// cameras' view by 4/3 or back by 3/4, and 0x8C032816 writes into the save with the other options.
bool widescreen() { return qilin::rt::ld32(0x8C0A5FF8u) != 0; }
bool movie_playing() { return toycommander::movie_playing(); }

}  // namespace dreamcast::game
