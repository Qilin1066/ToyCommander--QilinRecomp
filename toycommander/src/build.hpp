// Toy Commander's build (USA, MK-57020, V1.022: 1GUTH.BIN): where the parts
// the shared Shinobi replacements work on are (shinobi/src/shinobi_build.hpp).
#pragma once

#include "natives.hpp"
#include "shinobi_build.hpp"

namespace shinobi {

inline constexpr Build kBuild{
    .game_sections = 0x8C09F5B8u,
    .shinobi_sections = 0x8C0C56A0u,
    .shinobi_init = 0x8C0B068Au,
    .game_main = 0x8C02796Au,
    .shinobi_exit = 0x8C0B06E2u,
    .shinobi_data = 0x8C117DBCu,
    .heap_default_table = 0x8C0C5AC8u,
    .chain_records = 0x8C116ABCu,
    .kamui_set_900 = 0x8C087E34u,
    .kamui_set_920 = 0x8C087E1Cu,
    .rtc_month_days = 0,
    .thread_current = 0x8C124280u,
    .thread_exit = 0x8C0C4972u,
    .thread_cleanup = 0x8C0C48E6u,
    .work_area = 0x8C0C6B68u,  // named by the literal at 0x8C0BAD80 (the code never writes it)
    .kamui_state = 0x8C1135F4u,
    .own_stack_top = 0x8C0C3C20u,
    .console_area = '1',
    .console_broadcast = '0',
    .natives = {
        .chain_add = 0x8C0B399Cu,
        .chain_remove = 0x8C0B3CA4u,
        .chain_dispatch_900 = 0x8C0B3B6Eu,
        .chain_dispatch_920 = 0x8C0B3BB8u,
        .gd_send_command = 0x8C0C1520u,
        .gd_check_command = 0x8C0C1534u,
        .gd_main_loop = 0x8C0C1548u,
        .gd_init = 0x8C0C155Cu,
        .gd_check_drive = 0x8C0C1570u,
        .gd_dma_end = 0x8C0C1584u,
        .gd_dma_transfer = 0x8C0C1598u,
        .gd_dma_progress = 0x8C0C15ACu,
        .gd_abort = 0x8C0C15C0u,
        .gd_reset = 0x8C0C15D4u,
        .rtc_refresh = 0x8C0B427Cu,
        .rtc_get_date = 0x8C0B42A6u,
        .rtc_date_to_count = 0,
        .thread_yield = 0x8C0C4A3Au,
        .thread_scheduler_pass = 0x8C0C4A7Cu,
        .memset = 0x8C04EDF4u,
        .memclr = 0x8C04EEE0u,
        .library_init = 0x8C0B068Au,
        .library_exit = 0x8C0B06E2u,
        .set_work_area = 0x8C0BAB98u,
        .clear_work_area = 0x8C0BABAAu,
        .set_sound_mode = 0x8C0BABB4u,
        .sound_mode = 0x8C0BABEAu,
        .area = 0x8C0BAC38u,
        .language = 0x8C0BAC68u,
        .broadcast = 0x8C0BACF0u,
        .cable = 0x8C0B0758u,
        .hardware_init = 0x8C0B4840u,
        .heap_init = 0x8C0B3E10u,
        .heap_exit = 0x8C0B3E6Au,
        .malloc = 0x8C0B3EDEu,
        .malloc_2 = 0x8C0B3EE8u,
        .free = 0x8C0B3FACu,
        .free_2 = 0x8C0B3FB6u,
        .rtc_init = 0x8C0B4244u,
        .rtc_exit = 0x8C0B426Eu,
        .cache_invalidate = 0x8C0BDB94u,
        .cache_purge = 0x8C0BDDF2u,
        .cache_write_back = 0x8C0BE046u,
        .timer_start = 0x8C0B4958u,
        .timer_count = 0x8C0B497Eu,
        .timer_diff = 0x8C0B4988u,
        .copy = 0x8C0BAF20u,
        .clear = 0x8C0BAF74u,
        .call_on_own_stack = 0x8C0C3C28u,
        .kamui_install_interrupt_handler = 0x8C085E16u,
        .kamui_remove_interrupt_handler = 0x8C085ED4u,
        .read_dbr = 0x8C04F0D0u,
        .debug_break = 0x8C099470u,
        .debug_trap = 0x8C09947Cu,
    },
};

// The game's own natives (natives.cpp), registered with the shared ones.
inline void provide_game_natives() { toycommander::provide_natives(); }

}  // namespace shinobi
