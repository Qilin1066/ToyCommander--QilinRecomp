// What Toy Racer's port gives the shared Dreamcast code (dreamcast/src/game.hpp).
#include "game.hpp"

#include "chain.hpp"
#include "natives.hpp"

namespace dreamcast::game {

uint32_t interrupt_handler(uint32_t offset) { return toyracer::interrupt_handler(offset); }
void raise_bus_event(qilin::rt::Context& c, uint32_t event) { toyracer::raise_bus_event(c, event); }
void set_console(unsigned language, bool stereo) { toyracer::set_console(language, stereo); }
void on_sound_mode(void (*save)(bool stereo)) { toyracer::on_sound_mode(save); }

}  // namespace dreamcast::game
