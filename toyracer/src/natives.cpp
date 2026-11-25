// Native replacements for guest functions (rt::provide). Each one does what
// its guest code does, as analysis of that code shows (docs/code-analysis.md),
// with what reaches hardware done the host's way. The natives both games
// share are in shinobi/src/library.cpp; these are Toy Racer's own.
#include "natives.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "flash.hpp"
#include "game.hpp"
#include "video.hpp"
#include "system.hpp"

namespace toyracer {

using namespace dreamcast;

namespace rt = qilin::rt;

namespace {

// 0x8C04D7E0 (entered with r7 = 2 through 0x8C04D7DC) and 0x8C04D916:
// Shinobi's timer interrupts, linked among the middleware in this build:
// each puts a handler in the exception table through VBR and enables a
// timer's underflow interrupt. Nothing refers to either; the port's timer
// unit raises no interrupts: stop if one is ever reached.
void shinobi_timer_interrupt_a(rt::Context&) { throw rt::Stop{rt::Stop::Unsupported, 0x8C04D7E0u, 0}; }
void shinobi_timer_interrupt_b(rt::Context&) { throw rt::Stop{rt::Stop::Unsupported, 0x8C04D916u, 0}; }

// 0x8C0D0AF8, 0x8C0D0B0C, 0x8C0D0B20, 0x8C0D0B34: the boot ROM's flash calls
// 0 to 3 (each sets r7 to its number and jumps through the vector at
// 0x8C0000B8): a partition's place and size into r5's two words, a read,
// a write (programming bits) and a partition's erasure, over the port's
// flash memory (dreamcast/src/flash.cpp). Shinobi's flash functions above
// them are lifted; what they expect of the results is read from them: -1
// for a failed read, the bytes written, 0 for an erasure.
void flash_info(rt::Context& c) {
    uint32_t offset = 0, size = 0;
    if (!flash::info(c.r[4], offset, size)) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st32(c.r[5], offset), rt::st32(c.r[5] + 4, size);
    c.r[0] = 0;
}
void flash_read(rt::Context& c) { c.r[0] = static_cast<uint32_t>(flash::read(c.r[4], c.r[5], c.r[6])); }
void flash_write(rt::Context& c) { c.r[0] = static_cast<uint32_t>(flash::write(c.r[4], c.r[5], c.r[6])); }
void flash_delete(rt::Context& c) { c.r[0] = static_cast<uint32_t>(flash::erase(c.r[4])); }

// 0x8C0C7D04: loads the disc's Dream Passport loader (sg_dpldr.bin, through
// gdFs) to 0x8C008300 and jumps to it: the web browser, another program,
// for the game's online features. The port runs only this game: stop.
void run_browser(rt::Context&) { throw rt::Stop{rt::Stop::Unsupported, 0x8C0C7D04u, 0x8C008300u}; }

// 0x8C0D0C80, the boot ROM exit (Shinobi's last step out): with the bytes
// at 0x8C0080FC and 0x8C0080FE both 1 it jumps to a program loaded at
// 0x8C008300, else into the boot ROM's menu (the vector at 0x8C0000E0). The
// port has no other program and no boot ROM: the first stops, the second
// quits, as Toy Commander's exit does.
void boot_rom_exit(rt::Context&) {
    if (rt::ld8(0x8C0080FCu) == 1 && rt::ld8(0x8C0080FEu) == 1) throw rt::Stop{rt::Stop::Unsupported, 0x8C0D0C80u, 0x8C008300u};
    rt::log("the game exited to the console's menu (the boot ROM exit, 0x8C0D0C80)");
    std::fflush(nullptr);
    std::_Exit(0);
}

// 0x8C0CAB2E, part of Shinobi's shutdown: with interrupts masked, the
// expansion device's shutdown (0x8C0D0EC8), the Maple driver's (0x8C0D2E92,
// lifted), then the operand cache cleared. The port has no expansion device
// and no cache: only the Maple driver's shutdown is run.
void shinobi_peripherals_exit(rt::Context& c) {
    const uint32_t sr = c.sr;
    c.sr |= 0xF0u;
    rt::call_guest(c, 0x8C0D2E92u);
    c.sr = sr;
}

// 0x8C0D0F40: whether the extension check (0x8C0D0DC0) found a device of
// kind r4 (1 to 4: the words at 0x8C0FD390, 0x8C0FD394, 0x8C0FD398,
// 0x8C0FD39C): 0 if that word is -1 (found), else -1; -2 for another kind or
// before the check ran (0x8C0FD384 not -1). The code's own answer, read from
// the same words. The port's check finds nothing, so the words stay 0 and the
// answer is -1: the network library's open (0x8C04E768) then gives up (-40)
// before it registers anything on the expansion port.
void extension_found(rt::Context& c) {
    static constexpr uint32_t kWords[] = {0x8C0FD390u, 0x8C0FD394u, 0x8C0FD398u, 0x8C0FD39Cu};
    if (rt::ld32(0x8C0FD384u) != 0xFFFFFFFFu || c.r[4] < 1 || c.r[4] > 4) {
        c.r[0] = 0xFFFFFFFEu;
        return;
    }
    c.r[0] = rt::ld32(kWords[c.r[4] - 1]) == 0xFFFFFFFFu ? 0 : 0xFFFFFFFFu;
}

// 0x8C0D116C: the modem library's three handlers (0x8C0D11FE, 0x8C0D1348,
// 0x8C0D13C0) into its table at 0x8C1C7818, which it returns; as the code.
void modem_handlers(rt::Context& c) {
    rt::st32(0x8C1C7818u, 0x8C0D11FEu), rt::st32(0x8C1C781Cu, 0x8C0D1348u), rt::st32(0x8C1C7820u, 0x8C0D13C0u);
    c.r[0] = c.r[4] = 0x8C1C7818u;
}

// 0x8C0D1184: the modem library's poll, which does nothing while its active
// byte (0x8C1C7828) is 0. Only the modem's start-up sets it, and the port
// has no modem: if it is ever set, stop.
void modem_poll(rt::Context&) {
    if (rt::ld8(0x8C1C7828u) != 0) throw rt::Stop{rt::Stop::Unsupported, 0x8C0D1184u, 0x8C1C7828u};
}

// 0x8C0D636C and 0x8C0D6464: a callback registered and removed for an
// expansion port device's slot (r4), its interrupt enabled or disabled in
// the device's register (0xA1000014 + 0x400 r4) and the slots' dispatcher on
// the system bus's event 0xB60. Only a network library whose open found a
// device (0x8C0D0F40) reaches them, and the port has nothing on the
// expansion port: stop if one is ever reached.
void expansion_callback_add(rt::Context& c) { throw rt::Stop{rt::Stop::Unsupported, 0x8C0D636Cu, c.r[4]}; }
void expansion_callback_remove(rt::Context& c) { throw rt::Stop{rt::Stop::Unsupported, 0x8C0D6464u, c.r[4]}; }

// 0x8C09A900: compares the words from 0x8C004020 with the development kit's
// debug adapter's name (in its literal pool, ending in a 0)
// until two differ, and breaks into the debugger (0x003B) if the name ran to
// its end. A console has no adapter there; if the port's RAM ever matched,
// the break stops as debug_break does. Registers as the code leaves them.
void debug_adapter_check(rt::Context& c) {
    uint32_t a = 0x8C004020u, b = 0x8C09A91Cu, x, y;
    do {
        x = rt::ld32(a), y = rt::ld32(b);
        a += 4, b += 4;
    } while (x == y);
    c.r[0] = b, c.r[1] = a, c.r[2] = y, c.r[3] = x;
    c.t = y == 0;
    if (y == 0) throw rt::Stop{rt::Stop::Trap, 0x8C09A910u, 0x003B};
}

// 0x8C0C9144: sets up a three-word record at r4 as {0, r5, 0} and returns
// r4 (the middleware gives it a 0x400-byte block it just allocated). r6 ends 0.
void shinobi_record_init_8C0C9144(rt::Context& c) {
    rt::st32(c.r[4], 0), rt::st32(c.r[4] + 4, c.r[5]), rt::st32(c.r[4] + 8, 0);
    c.r[6] = 0, c.r[0] = c.r[4];
}

// 0x8C04DE3C and 0x8C04DE48, the compiler's helper for copying a structure:
// r0 bytes (a multiple of 4, up to 64) from r2 to r1, a word at a time from
// the top, through a table of entry points into one unrolled run of loads and
// stores (0x8C04DE9C, indexed by r0). The run alternates between r0 and r3 as
// its temporary, so there are two entries: 0x8C04DE3C for an even number of
// words, 0x8C04DE48 for an odd one. Each word is loaded before the one above
// it is stored. r3 is saved and restored; r0 ends as the word at r2 + 4, or 0
// when one word was copied. Anything the compiler wouldn't pass stops.
void struct_copy(rt::Context& c, uint32_t entry, uint32_t parity) {
    const uint32_t n = c.r[0], words = n / 4, dst = c.r[1], src = c.r[2];
    if (n == 0 || n % 4 || n > 64 || (words & 1u) != parity) throw rt::Stop{rt::Stop::Unsupported, entry, n};
    uint32_t held = rt::ld32(src + n - 4), second = words == 2 ? held : 0;
    for (uint32_t i = words - 1; i-- > 0;) {
        const uint32_t next = rt::ld32(src + 4 * i);
        if (i == 1) second = next;
        rt::st32(dst + 4 * (i + 1), held);
        held = next;
    }
    rt::st32(dst, held);
    c.r[0] = words == 1 ? 0 : second;
}
void struct_copy_even(rt::Context& c) { struct_copy(c, 0x8C04DE3Cu, 0); }
void struct_copy_odd(rt::Context& c) { struct_copy(c, 0x8C04DE48u, 1); }

// 0x8C0C54DA and 0x8C0C552C, in Shinobi's start-up code: the C runtime's
// lists of functions to call before main's body and at its end. Each walks
// two tables of function pointers, whose bounds are words in the image, and
// calls every entry that is neither 0 nor -1. main calls the first itself.
// In this build the first has 9 entries in one table (0x8C0C5400) and the
// second 4 (0x8C0C5424).
void call_table(rt::Context& c, uint32_t first_word, uint32_t end_word) {
    for (uint32_t at = rt::ld32(first_word); at < rt::ld32(end_word); at += 4) {
        const uint32_t fn = rt::ld32(at);
        if (fn != 0 && fn != 0xFFFFFFFFu) rt::call_guest(c, fn);
    }
}
void runtime_init_lists(rt::Context& c) {
    call_table(c, 0x8C0A5B2Cu, 0x8C0A5B30u);
    call_table(c, 0x8C0A5B3Cu, 0x8C0A5B40u);
}
void runtime_exit_lists(rt::Context& c) {
    call_table(c, 0x8C0A5B34u, 0x8C0A5B38u);
    call_table(c, 0x8C0A5B44u, 0x8C0A5B48u);
}

// The entries of those lists: five constructors and two destructors of
// static objects in Sega's modem library (C++: 0x8C0D15AA builds two objects
// through 0x8C0DFF2C and 0x8C0DEB82, and the rest lie among the modem's own
// code, 0x8C0DD9B8-0x8C0E950C). The port has no modem and no online mode
// (docs/roadmap-and-design-targets.md), and nothing outside the modem library
// reads those objects: nothing to construct. If lifted code ever reaches for
// one, the modem function it calls stops as unregistered.
void modem_static_object(rt::Context&) {}

// 0x8C0D0DC0, where the peripherals' set-up goes on here (Shinobi 1.71's
// extension check, which Toy Commander's Shinobi doesn't have): it reads the
// modem's identity through the G2 bus (0xA0600000) and looks for devices at
// three addresses of the expansion port (0xA1000400, 0xA1000800,
// 0xA1001400), keeping what it finds in words from 0x8C0FD388, then marks
// itself done (0x8C0FD384 = -1) and returns 0; called again it returns -1.
// The port has no modem and nothing on the expansion port (no online mode:
// docs/roadmap-and-design-targets.md): what it finds is nothing, which
// leaves those words as they are.
void shinobi_extension_check(rt::Context& c) {
    if (rt::ld32(0x8C0FD384u) != 0) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st32(0x8C0FD384u, 0xFFFFFFFFu);
    c.r[0] = 0;
}

// The screen mode switch (0x8C01AC4E): the cameras widened for the screen,
// not for the game's option (game.cpp's widen_to).
void screen_mode_switch(rt::Context&) { dreamcast::game::widen_to(dreamcast::video::screen_widening()); }

const rt::Provided kNatives[] = {
    {0x8C01AC4Eu, "screen_mode_switch", screen_mode_switch},
    {0x8C04DE3Cu, "struct_copy_even", struct_copy_even},
    {0x8C04DE48u, "struct_copy_odd", struct_copy_odd},
    {0x8C0C54DAu, "runtime_init_lists", runtime_init_lists},
    {0x8C0C552Cu, "runtime_exit_lists", runtime_exit_lists},
    {0x8C0D15AAu, "modem_static_init_8C0D15AA", modem_static_object},
    {0x8C0DD9B8u, "modem_static_init_8C0DD9B8", modem_static_object},
    {0x8C0DDBBCu, "modem_static_init_8C0DDBBC", modem_static_object},
    {0x8C0DFEECu, "modem_static_init_8C0DFEEC", modem_static_object},
    {0x8C0E950Cu, "modem_static_init_8C0E950C", modem_static_object},
    {0x8C0D15BCu, "modem_static_exit_8C0D15BC", modem_static_object},
    {0x8C0DFEFAu, "modem_static_exit_8C0DFEFA", modem_static_object},
    {0x8C0D0DC0u, "shinobi_extension_check", shinobi_extension_check},
    {0x8C04D7E0u, "shinobi_timer_interrupt_a", shinobi_timer_interrupt_a},
    {0x8C04D916u, "shinobi_timer_interrupt_b", shinobi_timer_interrupt_b},
    {0x8C0D0AF8u, "flash_info", flash_info},
    {0x8C0D0B0Cu, "flash_read", flash_read},
    {0x8C0D0B20u, "flash_write", flash_write},
    {0x8C0D0B34u, "flash_delete", flash_delete},
    {0x8C0C7D04u, "run_browser", run_browser},
    {0x8C0D0C80u, "boot_rom_exit", boot_rom_exit},
    {0x8C0CAB2Eu, "shinobi_peripherals_exit", shinobi_peripherals_exit},
    {0x8C0D0F40u, "extension_found", extension_found},
    {0x8C0D116Cu, "modem_handlers", modem_handlers},
    {0x8C0D1184u, "modem_poll", modem_poll},
    {0x8C0D636Cu, "expansion_callback_add", expansion_callback_add},
    {0x8C0D6464u, "expansion_callback_remove", expansion_callback_remove},
    {0x8C09A900u, "debug_adapter_check", debug_adapter_check},
    {0x8C0C9144u, "shinobi_record_init_8C0C9144", shinobi_record_init_8C0C9144},
};

}  // namespace

void provide_natives() { rt::provide(kNatives); }

}  // namespace toyracer
