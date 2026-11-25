// Native replacements for the parts of Shinobi, Kamui 2 and the C runtime
// both games link in the same form (library.cpp): memory fills and copies,
// the allocator's entry points, the console's settings, the timer and
// cache calls, the interrupt set-up. Each game registers them at its own
// addresses (kBuild.natives, <game>/src/build.hpp).
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>

namespace shinobi {

// Registers them (rt::provide).
void provide_library();
// The handler code put in Shinobi's exception table at VBR + `offset` (0x224:
// the graphics chip's level-6 interrupts, Kamui 2's), or 0.
uint32_t interrupt_handler(uint32_t offset);
// The Dreamcast's own settings the game reads: its language (1 English,
// 2 German, 3 French, 4 Spanish) and stereo or mono sound.
void set_console(unsigned language, bool stereo);
// Called when the game changes the console's sound mode (Shinobi writes it
// to the console's flash memory): the settings file keeps it.
void on_sound_mode(void (*save)(bool stereo));

}  // namespace shinobi
