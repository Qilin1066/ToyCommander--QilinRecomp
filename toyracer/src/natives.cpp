// Native replacements for guest functions (rt::provide). Each one does what
// its guest code does, as analysis of that code shows (docs/code-analysis.md),
// with what reaches hardware done the host's way.
#include "natives.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <map>

#include "flash.hpp"
#include "heap.hpp"
#include "pvr.hpp"
#include "sh4.hpp"
#include "video.hpp"
#include "system.hpp"

namespace toyracer {

using namespace dreamcast;

namespace rt = qilin::rt;

namespace {

// 0x8C05E108, the C runtime's clear: zeroes r5 bytes from r4 and returns r4
// in r0. The guest code clears the 32-byte blocks through the store queues
// (zeros written to them with 64-bit fmov from fr0/fr1, then pref), which
// leaves fr0 and fr1 zero when it takes that path: when, after the bytes up to
// the next 32-byte boundary (a whole 32 when r4 is aligned), 32 or more remain.
// The store queues reach RAM because Shinobi's set-up points them there.
void memclr(rt::Context& c) {
    const uint32_t dest = c.r[4], n = c.r[5];
    c.r[0] = dest;
    if (n == 0) return;
    rt::check(dest, n);
    std::memset(rt::g_mem + dest, 0, n);
    const uint32_t head = 32 - (dest & 31);
    if (n >= 32 && n - head >= 32) c.fr[0] = c.fr[1] = 0.0f;
}

// 0x8C05E01C, the C runtime's fill: r6 bytes of r5's low byte from r4, r4
// returned in r0. A zero byte goes to memclr (with the count moved to r5).
// The guest code fills 32-byte blocks through the store queues, keeping r8 in
// FPUL meanwhile, which leaves FPUL holding r8 when it takes that path (on
// the same condition as memclr's).
void memset_sq(rt::Context& c) {
    if (c.r[5] == 0) {  // the guest tests the whole register
        c.r[5] = c.r[6];
        memclr(c);
        return;
    }
    const uint32_t dest = c.r[4], n = c.r[6];
    c.r[0] = dest;
    if (n == 0) return;
    rt::check(dest, n);
    std::memset(rt::g_mem + dest, static_cast<int>(c.r[5] & 0xFFu), n);
    const uint32_t head = 32 - (dest & 31);
    if (n >= 32 && n - head >= 32) c.fpul = c.r[8];
}

// 0x8C0C5856, Shinobi's initialisation, and 0x8C0C58AE, its exit: they walk
// the library handles between the "Lib Handle Start" and "Lib Handle End"
// markers at r4 + 48 (one entry in this build, at 0x8C010040) and register
// each in a table of Shinobi's own (0x8C0C576C), or remove it (0x8C0C57BC),
// with interrupts masked around it. Only Shinobi reads that table, and the
// port replaces Shinobi: nothing to do. Both return 0 (-1 if the start marker
// is missing).
void shinobi_library_init(rt::Context& c) { c.r[0] = 0; }
void shinobi_library_exit(rt::Context& c) { c.r[0] = 0; }

// 0x8C0C78E4: keeps a work area's address (r4) in a Shinobi global
// (0x8C0FD1F8); 0, or -1 for a null address. The global is written in guest
// RAM as the guest code writes it.
void shinobi_set_work_area(rt::Context& c) {
    if (c.r[4] == 0) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st32(0x8C0FD1F8u, c.r[4]);
    c.r[0] = 0;
}

// The console's settings, which Shinobi reads from its flash memory and an
// I/O port: a European console (the disc's area; PAL, as those are), English,
// stereo, a VGA cable (640x480 progressive at 60 Hz, what a PC shows best). The language will come from
// the settings file (toyracer.ini, docs/naming.md).
struct Console {
    uint32_t area = '2';       // the factory record's byte 10: '0' Japan, '1' the Americas, '2' Europe
    uint32_t broadcast = '1';  // its byte 12: '0' NTSC, '1' PAL, '2' PAL-M, '3' PAL-N
    uint32_t language = 1;     // the user's: 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish, 5 Italian
    uint32_t sound = 0;        // the user's: 0 stereo, 1 mono
    uint32_t cable = 0;        // the port's bits 9-8: 0 VGA, 2 RGB, 3 composite
} g_console;
void (*g_save_sound_mode)(bool) = nullptr;
}  // namespace

void set_console(unsigned language, bool stereo) {
    g_console.language = language;
    g_console.sound = stereo ? 0 : 1;
}
void on_sound_mode(void (*save)(bool stereo)) { g_save_sound_mode = save; }

namespace {

// Each query writes its value through r4 and returns 0 (-1 for a null r4).
void put_setting(rt::Context& c, uint32_t value) {
    if (c.r[4] == 0) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st32(c.r[4], value);
    c.r[0] = 0;
}
void shinobi_sound_mode(rt::Context& c) { put_setting(c, g_console.sound); }      // 0x8C0C7936
void shinobi_area(rt::Context& c) { put_setting(c, g_console.area); }             // 0x8C0C7984
void shinobi_language(rt::Context& c) { put_setting(c, g_console.language); }     // 0x8C0C79B4
void shinobi_broadcast(rt::Context& c) { put_setting(c, g_console.broadcast); }   // 0x8C0C7A3C
// 0x8C04DB04: the cable, from the I/O port's bits 9-8, returned in r0.
void shinobi_cable(rt::Context& c) { c.r[0] = g_console.cable; }

// 0x8C0CAAF0, Shinobi's hardware set-up, called by the middleware's system
// initialisation, which doesn't use its result. Of what it programs, the
// port's hardware needs what lifted code reads back: timer 0, which
// 0x8C04D758 makes a free-running counter (Kamui 2 times its waits with it).
// The interrupt controller and DMA (0x8C0CAB9A, 0x8C0D2C0C, 0x8C0D2DE4) are
// the port's own business.
void shinobi_hardware_init(rt::Context&) { start_free_running_timer0(); }

// 0x8C0C5AFC, Shinobi's allocator set-up: the heap is r5 bytes from r4, both
// rounded to 32 bytes (the middleware's system initialisation passes the
// end of the program's data and the rest of RAM). The caller doesn't use r0.
void shinobi_heap_init(rt::Context& c) {
    rt::check(c.r[4], c.r[5]);
    heap::init(c.r[4], c.r[5]);
}

// The allocator's entry points jump through the operations table at
// 0x8C1BCAC0. Only its set-up writes it, with the default table, so anything
// else there is something new: stop rather than guess.
void check_heap_table(uint32_t entry) {
    if (rt::ld32(heap::kTablePointer) != heap::kDefaultTable)
        throw rt::Stop{rt::Stop::Unsupported, entry, rt::ld32(heap::kTablePointer)};
}
// 0x8C0C5BCA and 0x8C0C5BD4 (two names for one call): malloc(r4).
void shinobi_malloc(rt::Context& c) {
    check_heap_table(0x8C0C5BCAu);
    c.r[0] = heap::alloc(c.r[4]);
}
// 0x8C0C5C98 and 0x8C0C5CA2: free(r4).
void shinobi_free(rt::Context& c) {
    check_heap_table(0x8C0C5C98u);
    heap::release(c.r[4]);
}
// 0x8C0C5B56, the allocator's shut-down: an rts.
void shinobi_heap_exit(rt::Context&) {}

// 0x8C0D1620, Shinobi's copy: r6 bytes from r5 to r4, in units of the largest
// size (8, 4, 2 or 1) that all three are multiples of; 8-byte units go
// through fr0/fr1 with 64-bit fmov. Through the bus, since either side can be
// a device (Kamui 2 copies to video memory with it). The registers end as the
// guest code leaves them. A count of 0 would make the guest loop wrap round: stop.
void shinobi_copy(rt::Context& c) {
    const uint32_t dst = c.r[4], src = c.r[5], n = c.r[6], all = dst | src | n;
    const uint32_t unit = (all & 7u) == 0 ? 8u : (all & 3u) == 0 ? 4u : (all & 1u) == 0 ? 2u : 1u;
    if (n == 0) throw rt::Stop{rt::Stop::Unsupported, 0x8C0D1620u, 0};
    uint32_t last = 0;
    for (uint32_t k = 0; k < n; k += unit) {
        switch (unit) {
        case 8: {
            const uint32_t hi = rt::ld32(src + k), lo = rt::ld32(src + k + 4);
            rt::st32(dst + k, hi), rt::st32(dst + k + 4, lo);
            c.fr[0] = rt::flt(hi), c.fr[1] = rt::flt(lo);
            break;
        }
        case 4: last = rt::ld32(src + k), rt::st32(dst + k, last); break;
        case 2: last = rt::ld16s(src + k), rt::st16(dst + k, last); break;
        default: last = rt::ld8s(src + k), rt::st8(dst + k, last); break;
        }
    }
    c.r[0] = unit == 8 ? all : last;
    c.r[4] = dst + n, c.r[5] = src + n, c.r[6] = 0, c.t = 1;
}

// The interrupt handlers code installs in Shinobi's table at VBR + offset:
// the runtime calls them when it delivers that interrupt.
std::map<uint32_t, uint32_t> g_vector;

}  // namespace

uint32_t interrupt_handler(uint32_t offset) {
    const auto it = g_vector.find(offset);
    return it == g_vector.end() ? 0 : it->second;
}

namespace {

// 0x8C090AC6, Kamui 2's interrupt set-up, with r4 its handler. With interrupts masked it saves IMASK and the level-9 interrupt
// masks (0xA05F6930 normal events, 0xA05F6938 errors) in its state at
// 0x8C1B9024 (+8, +12, +16), puts its handler in the table entry at
// VBR + 0x224 if it isn't there already (keeping the old one at +40 and
// itself at +44), clears the masks and every pending event, and enables
// the events it wants (0x280FEC normal, 0xE errors: +24, +28).
void kamui_install_interrupt_handler(rt::Context& c) {
    constexpr uint32_t kState = 0x8C1B9024u, kSlot = 0x224u;
    rt::st32(kState + 8, (c.sr >> 4) & 15u);
    rt::st32(kState + 12, pvr_register(0xA05F6930u));
    rt::st32(kState + 16, pvr_register(0xA05F6938u));
    const uint32_t old = g_vector[kSlot];
    if (old != c.r[4]) {
        rt::st32(kState + 40, old), rt::st32(kState + 44, c.r[4]);
        g_vector[kSlot] = c.r[4];
    }
    set_pvr_register(0xA05F6930u, 0), set_pvr_register(0xA05F6938u, 0);
    set_pvr_register(0xA05F6900u, 0xFFFFFFFFu), set_pvr_register(0xA05F6908u, 0xFFFFFFFFu);
    rt::st32(kState + 24, 0x00280FECu), rt::st32(kState + 28, 14);
    set_pvr_register(0xA05F6930u, 0x00280FECu), set_pvr_register(0xA05F6938u, 14);
}

// 0x8C090B84, Kamui 2's interrupt removal, the other half of 0x8C090AC6 (the
// same code as Toy Commander's 0x8C085ED4): with interrupts masked it saves
// IMASK and the level-9 masks in its state again, puts the handler it kept
// (+40) back in the table entry at VBR + 0x224 unless the entry still holds
// the one at +44, clears the masks and every pending event, and leaves its
// own masks (+24, +28) at 0.
void kamui_remove_interrupt_handler(rt::Context& c) {
    constexpr uint32_t kState = 0x8C1B9024u, kSlot = 0x224u;
    rt::st32(kState + 8, (c.sr >> 4) & 15u);
    rt::st32(kState + 12, pvr_register(0xA05F6930u));
    rt::st32(kState + 16, pvr_register(0xA05F6938u));
    if (rt::ld32(kState + 44) != g_vector[kSlot]) g_vector[kSlot] = rt::ld32(kState + 40);
    set_pvr_register(0xA05F6930u, 0), set_pvr_register(0xA05F6938u, 0);
    set_pvr_register(0xA05F6900u, 0xFFFFFFFFu), set_pvr_register(0xA05F6908u, 0xFFFFFFFFu);
    rt::st32(kState + 24, 0), rt::st32(kState + 28, 0);
}

// 0x8C05E2F8: the debug base register (stc dbr), a debugger's (Toy
// Commander's 0x8C04F0D0). Nothing refers to it; the port has no debugger.
void read_dbr(rt::Context&) { throw rt::Stop{rt::Stop::Unsupported, 0x8C05E2F8u, 0}; }

// 0x8C04D7E0 (entered with r7 = 2 through 0x8C04D7DC) and 0x8C04D916:
// Shinobi's timer interrupts, linked among the middleware in this build:
// each puts a handler in the exception table through VBR and enables a
// timer's underflow interrupt. Nothing refers to either; the port's timer
// unit raises no interrupts: stop if one is ever reached.
void shinobi_timer_interrupt_a(rt::Context&) { throw rt::Stop{rt::Stop::Unsupported, 0x8C04D7E0u, 0}; }
void shinobi_timer_interrupt_b(rt::Context&) { throw rt::Stop{rt::Stop::Unsupported, 0x8C04D916u, 0}; }

// 0x8C0E00A0 (Toy Commander's 0x8C0C3C28, the same code): calls r4 with r5
// and r6 as its arguments on Shinobi's own stack (top 0x8C1D9D60, the
// caller's kept at 0x8C1D9D64), with PR and SZ cleared in FPSCR and the
// current bank, MACH, MACL, FPSCR and PR as they were afterwards (Kamui 2's
// interrupt handler calls the game's callbacks this way).
void shinobi_call_on_own_stack(rt::Context& c) {
    const rt::Context saved = c;
    rt::st32(0x8C1D9D64u, c.r[15]);
    c.r[15] = 0x8C1D9D60u - 4 * 20;  // where the guest code's pushes leave it
    c.fpscr &= ~0x00180000u;
    c.r[4] = saved.r[5], c.r[5] = saved.r[6];
    rt::call_guest(c, saved.r[4]);
    std::memcpy(c.fr, saved.fr, sizeof c.fr);
    c.mach = saved.mach, c.macl = saved.macl, c.fpscr = saved.fpscr, c.pr = saved.pr;
    c.r[15] = saved.r[15];
}

// 0x8C0C5F5A (Toy Commander's 0x8C0B426E), the real-time clock's exit:
// clears the count it keeps (0x8C1BCAF4) and takes its refresh handler out
// of the interrupt chain. The port's set-up (0x8C0C5F30) added no handler,
// so only the count is cleared.
void shinobi_rtc_exit(rt::Context& c) {
    rt::st32(0x8C1BCAF4u, 0);
    c.r[0] = 0;
}

// 0x8C0C78F6 (Toy Commander's 0x8C0BABAA): forgets the work area Shinobi was
// given (0x8C0FD1F8; Shinobi's exit).
void shinobi_clear_work_area(rt::Context& c) {
    rt::st32(0x8C0FD1F8u, 0);
    c.r[0] = 0;
}

// 0x8C0C7900 (Toy Commander's 0x8C0BABB4): sets the console's sound mode, r4
// (0 stereo, 1 mono): the boot ROM's copy at 0x8C00007E, then the console's
// flash memory through the work area (0x8C0D0B84). Fails (-1) without a work
// area or for another value. The port's flash memory is the settings file.
void shinobi_set_sound_mode(rt::Context& c) {
    if (rt::ld32(0x8C0FD1F8u) == 0 || c.r[4] > 1) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st8(0x8C00007Eu, static_cast<uint8_t>(c.r[4]));
    g_console.sound = c.r[4];
    if (g_save_sound_mode) g_save_sound_mode(c.r[4] == 0);
    c.r[0] = 0;
}

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

// 0x8C0A1DD4: a debugger's break (0x003B), which the engine's checks call
// when one fails; 0x8C0A1DE0: trapa #2, the C runtime's default handler. On
// a console without a debugger either ends the game: stop, naming the caller.
void debug_break(rt::Context& c) { throw rt::Stop{rt::Stop::Trap, c.pr - 4, 0x003B}; }
void debug_trap(rt::Context& c) { throw rt::Stop{rt::Stop::Trap, c.pr - 4, 2}; }

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

// 0x8C0D1674, Shinobi's clear: r5 bytes from r4, in units as its copy picks
// them (8-byte ones are fr0/fr1 zeroed and stored with 64-bit fmov). Through
// the bus; the registers end as the guest leaves them; a count of 0 stops.
void shinobi_clear(rt::Context& c) {
    const uint32_t dst = c.r[4], n = c.r[5], all = dst | n;
    const uint32_t unit = (all & 7u) == 0 ? 8u : (all & 3u) == 0 ? 4u : (all & 1u) == 0 ? 2u : 1u;
    if (n == 0) throw rt::Stop{rt::Stop::Unsupported, 0x8C0D1674u, 0};
    for (uint32_t k = 0; k < n; k += unit) {
        if (unit >= 4) {
            for (uint32_t w = 0; w < unit; w += 4) rt::st32(dst + k + w, 0);
        } else if (unit == 2) {
            rt::st16(dst + k, 0);
        } else {
            rt::st8(dst + k, 0);
        }
    }
    if (unit == 8) c.fr[0] = c.fr[1] = 0.0f, c.r[0] = all;
    else c.r[0] = 0;
    c.r[4] = dst + n, c.r[5] = 0, c.t = 1;
}


// 0x8C0C5F30, the real-time clock's initialisation: reads the AICA's clock
// (0xA0710000, two 16-bit halves of seconds since 1950, read until two reads
// agree) into 0x8C1BCAF4, clears the flags at 0x8C1BCAF8 and adds a refresh
// handler (0x8C0C5F68) to the interrupt chain for event 0x900 at priority 92,
// keeping the handle at 0x8C1BCAFC. Only Shinobi reads any of that (its
// date functions at 0x8C0C5F92 and 0x8C0C6088, which get natives over the
// host's clock when something calls them), so nothing to do here. Returns 0.
void shinobi_rtc_init(rt::Context& c) { c.r[0] = 0; }

// 0x8C0C9144: sets up a three-word record at r4 as {0, r5, 0} and returns
// r4 (the middleware gives it a 0x400-byte block it just allocated). r6 ends 0.
void shinobi_record_init_8C0C9144(rt::Context& c) {
    rt::st32(c.r[4], 0), rt::st32(c.r[4] + 4, c.r[5]), rt::st32(c.r[4] + 8, 0);
    c.r[6] = 0, c.r[0] = c.r[4];
}

// 0x8C04D77E: timer 0's count so far, 0xFFFFFFFF - TCNT0 (timer 0 counts
// down from 0xFFFFFFFF: shinobi_hardware_init). r2 and r3 end as the guest
// code leaves them.
void shinobi_timer_count(rt::Context& c) {
    c.r[2] = 0xFFD8000Cu;
    c.r[3] = rt::ld32(0xFFD8000Cu);
    c.r[0] = 0xFFFFFFFFu - c.r[3];
}
// 0x8C04D788: the ticks between two counts, r5 - r4.
void shinobi_timer_diff(rt::Context& c) { c.r[0] = c.r[5] - c.r[4]; }

// syCache over r5 bytes from r4: 0x8C0D228A invalidates the operand cache
// (ocbi, after a DMA into memory), 0x8C0D24E6 purges it (ocbp, before one
// out of memory), 0x8C0D2736 writes it back (ocbwb). The port has no cache
// between the CPU and its devices: nothing to do.
void shinobi_cache_invalidate(rt::Context&) {}
void shinobi_cache_purge(rt::Context&) {}
void shinobi_cache_write_back(rt::Context&) {}

// 0x8C04D758: (re)starts timer 0 as a free-running count down from
// 0xFFFFFFFF at Pphi/4, as Shinobi's hardware set-up does.
void shinobi_timer_start(rt::Context&) { start_free_running_timer0(); }

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

// The screen mode switch (0x8C01AC4E), as the game's but for the screen
// rather than the option (game.hpp), as Toy Commander's: the game's scales
// the cameras' view (0x8C02BAD0 with the ratio in fr4), and one more value
// when 0x8C0ADDC0's bit 2 is set, by 4/3 when the option is 16:9 and back by
// 3/4, and keeps the factor the projection reads (0x8C0ADDD0) and a state
// word (0x8C0ADDBC). This one widens them as much as the screen is wider than
// 4:3, whatever the option, and the renderer draws the picture that much
// wider (dreamcast/src/renderer.hpp, Source::widen).
void screen_mode_switch(rt::Context& c) {
    constexpr uint32_t kState = 0x8C0ADDBCu, kFlags = 0x8C0ADDC0u, kFactor = 0x8C0ADDD0u, kScaler = 0x8C02BAD0u;
    constexpr uint32_t kSometimes = 0x8C1045D0u;
    const float target = dreamcast::video::screen_widening(), applied = rt::flt(rt::ld32(kFactor));
    if (target == applied || !(applied > 0.0f)) return;
    const float ratio = target / applied;
    rt::st32(kState, target > 1.0f ? 1u : 0u);
    rt::st32(kFactor, rt::bits(target));
    const uint32_t pr = c.pr;
    c.fr[4] = ratio;
    rt::call_guest(c, kScaler);
    c.pr = pr;
    if (rt::ld32(kFlags) & 4u) rt::st32(kSometimes, rt::bits(rt::flt(rt::ld32(kSometimes)) * ratio));
}

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
    {0x8C05E01Cu, "memset", memset_sq},
    {0x8C05E108u, "memclr", memclr},
    {0x8C0C5856u, "shinobi_library_init", shinobi_library_init},
    {0x8C090AC6u, "kamui_install_interrupt_handler", kamui_install_interrupt_handler},
    {0x8C090B84u, "kamui_remove_interrupt_handler", kamui_remove_interrupt_handler},
    {0x8C05E2F8u, "read_dbr", read_dbr},
    {0x8C04D7E0u, "shinobi_timer_interrupt_a", shinobi_timer_interrupt_a},
    {0x8C04D916u, "shinobi_timer_interrupt_b", shinobi_timer_interrupt_b},
    {0x8C0E00A0u, "shinobi_call_on_own_stack", shinobi_call_on_own_stack},
    {0x8C0C5F5Au, "shinobi_rtc_exit", shinobi_rtc_exit},
    {0x8C0C78F6u, "shinobi_clear_work_area", shinobi_clear_work_area},
    {0x8C0C7900u, "shinobi_set_sound_mode", shinobi_set_sound_mode},
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
    {0x8C0A1DD4u, "debug_break", debug_break},
    {0x8C0A1DE0u, "debug_trap", debug_trap},
    {0x8C09A900u, "debug_adapter_check", debug_adapter_check},
    {0x8C04DB04u, "shinobi_cable", shinobi_cable},
    {0x8C0C5AFCu, "shinobi_heap_init", shinobi_heap_init},
    {0x8C0C5B56u, "shinobi_heap_exit", shinobi_heap_exit},
    {0x8C0C5BCAu, "shinobi_malloc", shinobi_malloc},
    {0x8C0C5BD4u, "shinobi_malloc_2", shinobi_malloc},
    {0x8C0C5C98u, "shinobi_free", shinobi_free},
    {0x8C0C5CA2u, "shinobi_free_2", shinobi_free},
    {0x8C0C9144u, "shinobi_record_init_8C0C9144", shinobi_record_init_8C0C9144},
    {0x8C0C5F30u, "shinobi_rtc_init", shinobi_rtc_init},
    {0x8C0CAAF0u, "shinobi_hardware_init", shinobi_hardware_init},
    {0x8C0D228Au, "shinobi_cache_invalidate", shinobi_cache_invalidate},
    {0x8C0D24E6u, "shinobi_cache_purge", shinobi_cache_purge},
    {0x8C0D2736u, "shinobi_cache_write_back", shinobi_cache_write_back},
    {0x8C04D758u, "shinobi_timer_start", shinobi_timer_start},
    {0x8C04D77Eu, "shinobi_timer_count", shinobi_timer_count},
    {0x8C04D788u, "shinobi_timer_diff", shinobi_timer_diff},
    {0x8C0C78E4u, "shinobi_set_work_area", shinobi_set_work_area},
    {0x8C0D1620u, "shinobi_copy", shinobi_copy},
    {0x8C0D1674u, "shinobi_clear", shinobi_clear},
    {0x8C0C7936u, "shinobi_sound_mode", shinobi_sound_mode},
    {0x8C0C7984u, "shinobi_area", shinobi_area},
    {0x8C0C79B4u, "shinobi_language", shinobi_language},
    {0x8C0C7A3Cu, "shinobi_broadcast", shinobi_broadcast},
    {0x8C0C58AEu, "shinobi_library_exit", shinobi_library_exit},
};

}  // namespace

void provide_natives() { rt::provide(kNatives); }

}  // namespace toyracer
