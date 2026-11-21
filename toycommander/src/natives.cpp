// Native replacements for guest functions (rt::provide). Each one does what
// its guest code does, as analysis of that code shows (docs/code-analysis.md),
// with what reaches hardware done the host's way.
#include "natives.hpp"

#include <cstdint>
#include <cstring>

#include "heap.hpp"
#include "system.hpp"

namespace toycommander {

namespace rt = qilin::rt;

namespace {

// 0x8C04EEE0, the C runtime's clear: zeroes r5 bytes from r4 and returns r4
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

// 0x8C0B068A, Shinobi's initialisation, and 0x8C0B06E2, its exit: they walk
// the library handles between the "Lib Handle Start" and "Lib Handle End"
// markers at r4 + 48 (one entry in this build, at 0x8C010040) and register
// each in a table of Shinobi's own (0x8C0B05A0), or remove it (0x8C0B05F0),
// with interrupts masked around it. Only Shinobi reads that table, and the
// port replaces Shinobi: nothing to do. Both return 0 (-1 if the start marker
// is missing).
void shinobi_library_init(rt::Context& c) { c.r[0] = 0; }
void shinobi_library_exit(rt::Context& c) { c.r[0] = 0; }

// 0x8C0BAB98: keeps a work area's address (r4) in a Shinobi global, the word
// its literal pool names (0x8C0BAD80); 0, or -1 for a null address. The
// global is written in guest RAM as the guest code writes it.
void shinobi_set_work_area(rt::Context& c) {
    if (c.r[4] == 0) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st32(rt::ld32(0x8C0BAD80u), c.r[4]);
    c.r[0] = 0;
}

// The console's settings, which Shinobi reads from its flash memory and an
// I/O port: a USA console (the disc's area), English, stereo, a VGA cable
// (640x480 progressive, what a PC shows best). The language will come from
// the settings file (toycommander.ini, docs/naming.md).
struct Console {
    uint32_t area = '1';       // the factory record's byte 10: '0' Japan, '1' the Americas, '2' Europe
    uint32_t broadcast = '0';  // its byte 12: '0' NTSC, '1' PAL, '2' PAL-M, '3' PAL-N
    uint32_t language = 1;     // the user's: 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish, 5 Italian
    uint32_t sound = 0;        // the user's: 0 stereo, 1 mono
    uint32_t cable = 0;        // the port's bits 9-8: 0 VGA, 2 RGB, 3 composite
} g_console;

// Each query writes its value through r4 and returns 0 (-1 for a null r4).
void put_setting(rt::Context& c, uint32_t value) {
    if (c.r[4] == 0) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st32(c.r[4], value);
    c.r[0] = 0;
}
void shinobi_sound_mode(rt::Context& c) { put_setting(c, g_console.sound); }      // 0x8C0BABEA
void shinobi_area(rt::Context& c) { put_setting(c, g_console.area); }             // 0x8C0BAC38
void shinobi_language(rt::Context& c) { put_setting(c, g_console.language); }     // 0x8C0BAC68
void shinobi_broadcast(rt::Context& c) { put_setting(c, g_console.broadcast); }   // 0x8C0BACF0
// 0x8C0B0758: the cable, from the I/O port's bits 9-8, returned in r0.
void shinobi_cable(rt::Context& c) { c.r[0] = g_console.cable; }

// 0x8C0B4840, Shinobi's hardware set-up (the interrupt controller, the
// timers, DMA, through 0x8C0B48E0, 0x8C0BE520 and 0x8C0BE6F8), called by the
// middleware's system initialisation, which doesn't use its result. The
// hardware is the port's: nothing to do.
void shinobi_hardware_init(rt::Context&) {}

Heap g_heap;

// 0x8C0B3E10, Shinobi's allocator set-up: the heap is r5 bytes from r4, both
// rounded to 32 bytes (the middleware's system initialisation passes the
// end of the program's data and the rest of RAM). The caller doesn't use r0.
void shinobi_heap_init(rt::Context& c) {
    rt::check(c.r[4], c.r[5]);
    g_heap.init(c.r[4], c.r[5]);
}

const rt::Provided kNatives[] = {
    {0x8C04EEE0u, "memclr", memclr},
    {0x8C0B068Au, "shinobi_library_init", shinobi_library_init},
    {0x8C0B0758u, "shinobi_cable", shinobi_cable},
    {0x8C0B3E10u, "shinobi_heap_init", shinobi_heap_init},
    {0x8C0B4840u, "shinobi_hardware_init", shinobi_hardware_init},
    {0x8C0BAB98u, "shinobi_set_work_area", shinobi_set_work_area},
    {0x8C0BABEAu, "shinobi_sound_mode", shinobi_sound_mode},
    {0x8C0BAC38u, "shinobi_area", shinobi_area},
    {0x8C0BAC68u, "shinobi_language", shinobi_language},
    {0x8C0BACF0u, "shinobi_broadcast", shinobi_broadcast},
    {0x8C0B06E2u, "shinobi_library_exit", shinobi_library_exit},
};

}  // namespace

void provide_natives() { rt::provide(kNatives); }

}  // namespace toycommander
