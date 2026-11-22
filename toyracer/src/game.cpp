// What Toy Racer's port gives the shared Dreamcast code (dreamcast/src/game.hpp).
#include "game.hpp"

#include "chain.hpp"
#include "natives.hpp"

namespace dreamcast::game {

uint32_t interrupt_handler(uint32_t offset) { return toyracer::interrupt_handler(offset); }
void raise_bus_event(qilin::rt::Context& c, uint32_t event) { toyracer::raise_bus_event(c, event); }
void set_console(unsigned language, bool stereo) { toyracer::set_console(language, stereo); }
void on_sound_mode(void (*save)(bool stereo)) { toyracer::on_sound_mode(save); }
// The options' screen mode word (0 for 4/3, else 16/9), which 0x8C01AC4E follows when it scales the
// cameras' view by 4/3 or back by 3/4 (the same code as Toy Commander's 0x8C018F42).
bool widescreen() { return qilin::rt::ld32(0x8C0FE86Cu) != 0; }

}  // namespace dreamcast::game
