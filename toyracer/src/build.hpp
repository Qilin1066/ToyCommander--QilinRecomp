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
    },
};

// The game's own natives (natives.cpp), registered with the shared ones.
inline void provide_game_natives() { toyracer::provide_natives(); }

}  // namespace shinobi
