// Toy Racer's build (Europe, MK-5114950, V1.050: 1TOYRACE.BIN; Shinobi
// 1.71): where the parts the shared Shinobi replacements work on are
// (shinobi/src/shinobi_build.hpp).
#pragma once

#include "natives.hpp"
#include "shinobi_build.hpp"

namespace shinobi {

inline constexpr Build kBuild{
    .game_sections = 0x8C0A5B04u,
    .shinobi_sections = 0x8C0F9BC4u,
    .shinobi_init = 0x8C0C5856u,
    .game_main = 0x8C0204E4u,
    .shinobi_exit = 0x8C0C58AEu,
    .shinobi_data = 0x8C1BCAC0u,
    .heap_default_table = 0x8C0F9E48u,
    .chain_records = 0x8C1BD17Cu,
    .kamui_set_900 = 0x8C0929E0u,
    .kamui_set_920 = 0x8C0929C8u,
    .rtc_month_days = 0x8C0F9E8Cu,
    .thread_current = 0x8C1CD1FCu,
    .thread_exit = 0x8C0E0DC6u,
    .thread_cleanup = 0x8C0E0D3Au,
    .work_area = 0x8C0FD1F8u,
    .kamui_state = 0x8C1B9024u,
    .own_stack_top = 0x8C1D9D60u,
    .console_area = '2',
    .console_broadcast = '1',
    .natives = {
        .chain_add = 0x8C0CA2FCu,
        .chain_remove = 0x8C0CA604u,
        .chain_dispatch_900 = 0x8C0CA4CEu,
        .chain_dispatch_920 = 0x8C0CA518u,
        .gd_send_command = 0x8C0D5D20u,
        .gd_check_command = 0x8C0D5D34u,
        .gd_main_loop = 0x8C0D5D48u,
        .gd_init = 0x8C0D5D5Cu,
        .gd_check_drive = 0x8C0D5D70u,
        .gd_dma_end = 0x8C0D5D84u,
        .gd_dma_transfer = 0x8C0D5D98u,
        .gd_dma_progress = 0x8C0D5DACu,
        .gd_abort = 0x8C0D5DC0u,
        .gd_reset = 0x8C0D5DD4u,
        .rtc_refresh = 0x8C0C5F68u,
        .rtc_get_date = 0x8C0C5F92u,
        .rtc_date_to_count = 0x8C0C6088u,
        .thread_yield = 0x8C0E0E8Eu,
        .thread_scheduler_pass = 0x8C0E0ED0u,
        .memset = 0x8C05E01Cu,
        .memclr = 0x8C05E108u,
        .library_init = 0x8C0C5856u,
        .library_exit = 0x8C0C58AEu,
        .set_work_area = 0x8C0C78E4u,
        .clear_work_area = 0x8C0C78F6u,
        .set_sound_mode = 0x8C0C7900u,
        .sound_mode = 0x8C0C7936u,
        .area = 0x8C0C7984u,
        .language = 0x8C0C79B4u,
        .broadcast = 0x8C0C7A3Cu,
        .cable = 0x8C04DB04u,
        .hardware_init = 0x8C0CAAF0u,
        .heap_init = 0x8C0C5AFCu,
        .heap_exit = 0x8C0C5B56u,
        .malloc = 0x8C0C5BCAu,
        .malloc_2 = 0x8C0C5BD4u,
        .free = 0x8C0C5C98u,
        .free_2 = 0x8C0C5CA2u,
        .rtc_init = 0x8C0C5F30u,
        .rtc_exit = 0x8C0C5F5Au,
        .cache_invalidate = 0x8C0D228Au,
        .cache_purge = 0x8C0D24E6u,
        .cache_write_back = 0x8C0D2736u,
        .timer_start = 0x8C04D758u,
        .timer_count = 0x8C04D77Eu,
        .timer_diff = 0x8C04D788u,
        .copy = 0x8C0D1620u,
        .clear = 0x8C0D1674u,
        .call_on_own_stack = 0x8C0E00A0u,
        .kamui_install_interrupt_handler = 0x8C090AC6u,
        .kamui_remove_interrupt_handler = 0x8C090B84u,
        .read_dbr = 0x8C05E2F8u,
        .debug_break = 0x8C0A1DD4u,
        .debug_trap = 0x8C0A1DE0u,
    },
};

// The game's own natives (natives.cpp), registered with the shared ones.
inline void provide_game_natives() { toyracer::provide_natives(); }

}  // namespace shinobi
