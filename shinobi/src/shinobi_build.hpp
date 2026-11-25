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
    Natives natives;
};

}  // namespace shinobi
