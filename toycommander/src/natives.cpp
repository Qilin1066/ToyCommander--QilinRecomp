// Native replacements for guest functions (rt::provide). Each one does what
// its guest code does, as analysis of that code shows (docs/code-analysis.md),
// with what reaches hardware done the host's way.
#include "natives.hpp"

#include <cstdint>
#include <cstring>

#include <map>

#include "heap.hpp"
#include "pvr.hpp"
#include "sh4.hpp"
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

// 0x8C04EDF4, the C runtime's fill: r6 bytes of r5's low byte from r4, r4
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
// I/O port: a USA console (the disc's area), a VGA cable (640x480
// progressive, what a PC shows best), and the language and sound mode the
// settings file gives (toycommander.ini: set_console).
struct Console {
    uint32_t area = '1';       // the factory record's byte 10: '0' Japan, '1' the Americas, '2' Europe
    uint32_t broadcast = '0';  // its byte 12: '0' NTSC, '1' PAL, '2' PAL-M, '3' PAL-N
    uint32_t language = 1;     // the user's: 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish, 5 Italian
    uint32_t sound = 0;        // the user's: 0 stereo, 1 mono
    uint32_t cable = 0;        // the port's bits 9-8: 0 VGA, 2 RGB, 3 composite
} g_console;
}  // namespace

void set_console(unsigned language, bool stereo) {
    g_console.language = language;
    g_console.sound = stereo ? 0 : 1;
}

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
void shinobi_sound_mode(rt::Context& c) { put_setting(c, g_console.sound); }      // 0x8C0BABEA
void shinobi_area(rt::Context& c) { put_setting(c, g_console.area); }             // 0x8C0BAC38
void shinobi_language(rt::Context& c) { put_setting(c, g_console.language); }     // 0x8C0BAC68
void shinobi_broadcast(rt::Context& c) { put_setting(c, g_console.broadcast); }   // 0x8C0BACF0
// 0x8C0B0758: the cable, from the I/O port's bits 9-8, returned in r0.
void shinobi_cable(rt::Context& c) { c.r[0] = g_console.cable; }

// 0x8C0B4840, Shinobi's hardware set-up, called by the middleware's system
// initialisation, which doesn't use its result. Of what it programs, the
// port's hardware needs what lifted code reads back: timer 0, which
// 0x8C0B4958 makes a free-running counter (Kamui 2 times its waits with it).
// The interrupt controller and DMA (0x8C0B48E0, 0x8C0BE520, 0x8C0BE6F8) are
// the port's own business.
void shinobi_hardware_init(rt::Context&) { start_free_running_timer0(); }

// 0x8C0B3E10, Shinobi's allocator set-up: the heap is r5 bytes from r4, both
// rounded to 32 bytes (the middleware's system initialisation passes the
// end of the program's data and the rest of RAM). The caller doesn't use r0.
void shinobi_heap_init(rt::Context& c) {
    rt::check(c.r[4], c.r[5]);
    heap::init(c.r[4], c.r[5]);
}

// The allocator's entry points jump through the operations table at
// 0x8C117DBC. Only its set-up writes it, with the default table, so anything
// else there is something new: stop rather than guess.
void check_heap_table(uint32_t entry) {
    if (rt::ld32(heap::kTablePointer) != heap::kDefaultTable)
        throw rt::Stop{rt::Stop::Unsupported, entry, rt::ld32(heap::kTablePointer)};
}
// 0x8C0B3EDE and 0x8C0B3EE8 (two names for one call): malloc(r4).
void shinobi_malloc(rt::Context& c) {
    check_heap_table(0x8C0B3EDEu);
    c.r[0] = heap::alloc(c.r[4]);
}
// 0x8C0B3FAC and 0x8C0B3FB6: free(r4).
void shinobi_free(rt::Context& c) {
    check_heap_table(0x8C0B3FACu);
    heap::release(c.r[4]);
}
// 0x8C0B3E6A, the allocator's shut-down: an rts.
void shinobi_heap_exit(rt::Context&) {}

// 0x8C0BAF20, Shinobi's copy: r6 bytes from r5 to r4, in units of the largest
// size (8, 4, 2 or 1) that all three are multiples of; 8-byte units go
// through fr0/fr1 with 64-bit fmov. Through the bus, since either side can be
// a device (Kamui 2 copies to video memory with it). The registers end as the
// guest code leaves them. A count of 0 would make the guest loop wrap round: stop.
void shinobi_copy(rt::Context& c) {
    const uint32_t dst = c.r[4], src = c.r[5], n = c.r[6], all = dst | src | n;
    const uint32_t unit = (all & 7u) == 0 ? 8u : (all & 3u) == 0 ? 4u : (all & 1u) == 0 ? 2u : 1u;
    if (n == 0) throw rt::Stop{rt::Stop::Unsupported, 0x8C0BAF20u, 0};
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

// 0x8C085E16, Kamui 2's interrupt set-up, with r4 its handler (0x8C085C78 or
// 0x8C085B8C). With interrupts masked it saves IMASK and the level-9 interrupt
// masks (0xA05F6930 normal events, 0xA05F6938 errors) in its state at
// 0x8C1135F4 (+8, +12, +16), puts its handler in the table entry at
// VBR + 0x224 if it isn't there already (keeping the old one at +40 and
// itself at +44), clears the masks and every pending event, and enables
// the events it wants (0x280FEC normal, 0xE errors: +24, +28).
void kamui_install_interrupt_handler(rt::Context& c) {
    constexpr uint32_t kState = 0x8C1135F4u, kSlot = 0x224u;
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

// 0x8C0BAF74, Shinobi's clear: r5 bytes from r4, in units as its copy picks
// them (8-byte ones are fr0/fr1 zeroed and stored with 64-bit fmov). Through
// the bus; the registers end as the guest leaves them; a count of 0 stops.
void shinobi_clear(rt::Context& c) {
    const uint32_t dst = c.r[4], n = c.r[5], all = dst | n;
    const uint32_t unit = (all & 7u) == 0 ? 8u : (all & 3u) == 0 ? 4u : (all & 1u) == 0 ? 2u : 1u;
    if (n == 0) throw rt::Stop{rt::Stop::Unsupported, 0x8C0BAF74u, 0};
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


// 0x8C0B4244, the real-time clock's initialisation: reads the AICA's clock
// (0xA0710000, two 16-bit halves of seconds since 1950, read until two reads
// agree) into 0x8C117DF0, clears the flags at 0x8C117DF4 and adds a refresh
// handler (0x8C0B427C) to the interrupt chain for event 0x900 at priority 92,
// keeping the handle at 0x8C117DF8. Only Shinobi reads any of that (its
// date functions at 0x8C0B42A6 and 0x8C0B439C, which get natives over the
// host's clock when something calls them), so nothing to do here. Returns 0.
void shinobi_rtc_init(rt::Context& c) { c.r[0] = 0; }

// 0x8C0B27E4: sets up a three-word record at r4 as {0, r5, 0} and returns
// r4 (the middleware gives it a 0x400-byte block it just allocated). r6 ends 0.
void shinobi_record_init_8C0B27E4(rt::Context& c) {
    rt::st32(c.r[4], 0), rt::st32(c.r[4] + 4, c.r[5]), rt::st32(c.r[4] + 8, 0);
    c.r[6] = 0, c.r[0] = c.r[4];
}

// 0x8C0B497E: timer 0's count so far, 0xFFFFFFFF - TCNT0 (timer 0 counts
// down from 0xFFFFFFFF: shinobi_hardware_init). r2 and r3 end as the guest
// code leaves them.
void shinobi_timer_count(rt::Context& c) {
    c.r[2] = 0xFFD8000Cu;
    c.r[3] = rt::ld32(0xFFD8000Cu);
    c.r[0] = 0xFFFFFFFFu - c.r[3];
}
// 0x8C0B4988: the ticks between two counts, r5 - r4.
void shinobi_timer_diff(rt::Context& c) { c.r[0] = c.r[5] - c.r[4]; }

// syCache over r5 bytes from r4: 0x8C0BDB94 invalidates the operand cache
// (ocbi, after a DMA into memory), 0x8C0BDDF2 purges it (ocbp, before one
// out of memory), 0x8C0BE046 writes it back (ocbwb). The port has no cache
// between the CPU and its devices: nothing to do.
void shinobi_cache_invalidate(rt::Context&) {}
void shinobi_cache_purge(rt::Context&) {}
void shinobi_cache_write_back(rt::Context&) {}

// 0x8C0B4958: (re)starts timer 0 as a free-running count down from
// 0xFFFFFFFF at Pphi/4, as Shinobi's hardware set-up does.
void shinobi_timer_start(rt::Context&) { start_free_running_timer0(); }

const rt::Provided kNatives[] = {
    {0x8C04EDF4u, "memset", memset_sq},
    {0x8C04EEE0u, "memclr", memclr},
    {0x8C0B068Au, "shinobi_library_init", shinobi_library_init},
    {0x8C085E16u, "kamui_install_interrupt_handler", kamui_install_interrupt_handler},
    {0x8C0B0758u, "shinobi_cable", shinobi_cable},
    {0x8C0B3E10u, "shinobi_heap_init", shinobi_heap_init},
    {0x8C0B3E6Au, "shinobi_heap_exit", shinobi_heap_exit},
    {0x8C0B3EDEu, "shinobi_malloc", shinobi_malloc},
    {0x8C0B3EE8u, "shinobi_malloc_2", shinobi_malloc},
    {0x8C0B3FACu, "shinobi_free", shinobi_free},
    {0x8C0B3FB6u, "shinobi_free_2", shinobi_free},
    {0x8C0B27E4u, "shinobi_record_init_8C0B27E4", shinobi_record_init_8C0B27E4},
    {0x8C0B4244u, "shinobi_rtc_init", shinobi_rtc_init},
    {0x8C0B4840u, "shinobi_hardware_init", shinobi_hardware_init},
    {0x8C0BDB94u, "shinobi_cache_invalidate", shinobi_cache_invalidate},
    {0x8C0BDDF2u, "shinobi_cache_purge", shinobi_cache_purge},
    {0x8C0BE046u, "shinobi_cache_write_back", shinobi_cache_write_back},
    {0x8C0B4958u, "shinobi_timer_start", shinobi_timer_start},
    {0x8C0B497Eu, "shinobi_timer_count", shinobi_timer_count},
    {0x8C0B4988u, "shinobi_timer_diff", shinobi_timer_diff},
    {0x8C0BAB98u, "shinobi_set_work_area", shinobi_set_work_area},
    {0x8C0BAF20u, "shinobi_copy", shinobi_copy},
    {0x8C0BAF74u, "shinobi_clear", shinobi_clear},
    {0x8C0BABEAu, "shinobi_sound_mode", shinobi_sound_mode},
    {0x8C0BAC38u, "shinobi_area", shinobi_area},
    {0x8C0BAC68u, "shinobi_language", shinobi_language},
    {0x8C0BACF0u, "shinobi_broadcast", shinobi_broadcast},
    {0x8C0B06E2u, "shinobi_library_exit", shinobi_library_exit},
};

}  // namespace

void provide_natives() { rt::provide(kNatives); }

}  // namespace toycommander
