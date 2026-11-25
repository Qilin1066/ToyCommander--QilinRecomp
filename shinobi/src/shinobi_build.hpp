// Where a game's build has the parts of Shinobi and of its start-up that the
// shared replacements (shinobi/src) work on. Both games are built on the same
// libraries, linked at other addresses: the code here is the same for both,
// and each game gives its addresses in its own table, `shinobi::kBuild` in
// <game>/src/build.hpp. Each game's library compiles the shared sources with
// its own table, so the addresses are constants, as when each game had its
// own copy. The comments in shinobi/src give Toy Commander's addresses.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>

namespace shinobi {

// The guest functions replaced natively by the shared code (each is a native
// registered at that address). tools/check/check_natives.py reads them from
// each game's build.hpp: keep them in this block, one literal each.
struct Natives {
    // The interrupt chains (chain.cpp): add, remove, and the two dispatchers Kamui 2 calls back.
    uint32_t chain_add, chain_remove, chain_dispatch_900, chain_dispatch_920;
    // The boot ROM's GD-ROM system calls, one wrapper each (gdrom_bios.cpp): calls 0 to 9.
    uint32_t gd_send_command, gd_check_command, gd_main_loop, gd_init, gd_check_drive, gd_dma_end, gd_dma_transfer, gd_dma_progress,
        gd_abort, gd_reset;
    // The real-time clock (rtc.cpp). rtc_date_to_count: 0 where lifted code never reaches it (Toy Commander).
    uint32_t rtc_refresh, rtc_get_date, rtc_date_to_count;
    // bu's thread library (threads.cpp): a thread's yield and the scheduler's pass.
    uint32_t thread_yield, thread_scheduler_pass;
    // The C runtime's, Shinobi's and Kamui 2's functions both games link in
    // the same form (library.cpp). The C runtime's fill and clear:
    uint32_t memset, memclr;
    // Shinobi's initialisation and exit, the work area, the console's settings:
    uint32_t library_init, library_exit, set_work_area, clear_work_area, set_sound_mode, sound_mode, area, language, broadcast, cable;
    // Its hardware set-up, allocator (two names each for malloc and free), real-time clock set-up and exit:
    uint32_t hardware_init, heap_init, heap_exit, malloc, malloc_2, free, free_2, rtc_init, rtc_exit;
    // The cache calls, timer 0, copy and clear, the call on Shinobi's own stack:
    uint32_t cache_invalidate, cache_purge, cache_write_back, timer_start, timer_count, timer_diff, copy, clear, call_on_own_stack;
    // Kamui 2's interrupt set-up and removal; the debugger's register, break and trap:
    uint32_t kamui_install_interrupt_handler, kamui_remove_interrupt_handler, read_dbr, debug_break, debug_trap;
};

struct Build {
    // The start-up (boot.cpp). SHC's section initialisation reads a table
    // for the game's sections and one for Shinobi's, each laid out alike: +0
    // where the initialised data's image is, +8 where it goes, +0x14 its end,
    // +0x18 the area to zero, +0x24 its end. Shinobi's initialisation and
    // exit take the word 0x10 before Shinobi's table.
    uint32_t game_sections, shinobi_sections;
    uint32_t shinobi_init, game_main, shinobi_exit;
    // Shinobi's data block, alike in both: +0 the allocator's operations
    // table (heap.hpp), +4 the free list's base, +0x24 its rover, +0x28 the
    // heap's start, +0x2C its size, +0x30 the start again; +0x34 the clock's
    // count of seconds, +0x38 its flags (rtc.cpp).
    uint32_t shinobi_data;
    uint32_t heap_default_table;  // the allocator's operations table as linked
    uint32_t chain_records;       // 256 records of 16 bytes; the lists' heads 0x1000 after them
    uint32_t kamui_set_900, kamui_set_920;  // Kamui 2's functions that set its callbacks for events 0x900 and 0x920
    uint32_t rtc_month_days;      // the days before each month, read by rtc_date_to_count (0 without it)
    uint32_t thread_current;      // the scheduler running now; the one before it at +4
    uint32_t thread_exit;         // where a killed thread's saved PR is pointed
    uint32_t thread_cleanup;      // frees a dead thread's slot (lifted)
    // library.cpp's: the global Shinobi keeps the work area's address in;
    // Kamui 2's interrupt state (IMASK and the masks at +8.., its handlers at
    // +40, +44); the top of the stack Shinobi calls callbacks on (the
    // caller's stack pointer kept 4 above it); and the console's area and
    // broadcast standard, the disc's (the factory record's bytes 10 and 12).
    uint32_t work_area, kamui_state, own_stack_top;
    uint32_t console_area, console_broadcast;
    Natives natives;
};

}  // namespace shinobi
