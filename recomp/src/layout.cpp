#include "layout.hpp"

namespace qilin {

std::string_view name(Plan p) {
    switch (p) {
    case Plan::lift: return "lift";
    case Plan::replace: return "replace";
    case Plan::undecided: return "undecided";
    case Plan::data: return "data";
    }
    return "?";
}

namespace {

// USA, MK-57020, V1.022. The evidence for each boundary is in
// docs/code-analysis.md.
constexpr Region kUsa[] = {
    {0x8C010000, 0x8C010030, "start-up (cache set-up, jump to syStart)", Plan::replace},
    {0x8C010030, 0x8C010080, "\"Lib Handle Start\" / \"Lib Handle End\" markers", Plan::data},
    {0x8C010080, 0x8C0495B8, "game", Plan::lift},
    {0x8C0495B8, 0x8C09F5B6, "middleware: No Cliche's engine, the audio manager, Kamui 2, kmdk, C runtime", Plan::undecided},
    {0x8C09F5B6, 0x8C0B0340, "initialised data of the game and the middleware", Plan::data},
    {0x8C0B0340, 0x8C0B0780, "Shinobi", Plan::replace},
    {0x8C0B0780, 0x8C0B13D4, "Shinobi's bu (the memory card's file system): its API", Plan::lift},
    {0x8C0B13D4, 0x8C0B3288, "Shinobi's gdFs (the GD-ROM file system, over gdd)", Plan::lift},
    // Shinobi's peripheral library (pd), its memory card file system (bu) and
    // their Maple bus driver are lifted, over the Maple bus the port provides:
    // their objects are spread through Shinobi, so each run of them is a
    // region of its own (docs/runtime.md).
    {0x8C0B3288, 0x8C0B37F0, "Shinobi's pd (peripherals)", Plan::lift},
    {0x8C0B37F0, 0x8C0B394C, "Shinobi", Plan::replace},
    {0x8C0B394C, 0x8C0B399C, "Shinobi: the interrupt chains' tables cleared (for pd's set-up)", Plan::lift},
    {0x8C0B399C, 0x8C0B486A, "Shinobi", Plan::replace},
    {0x8C0B486A, 0x8C0B487A, "Shinobi: the peripherals' set-up", Plan::lift},
    {0x8C0B487A, 0x8C0B4D04, "Shinobi", Plan::replace},
    {0x8C0B4D04, 0x8C0B5BD6, "Shinobi's Maple driver: the bus's servers, the devices' drivers", Plan::lift},
    {0x8C0B5BD6, 0x8C0BAB98, "Shinobi's Maple devices' drivers and bu's core", Plan::lift},
    {0x8C0BAB98, 0x8C0BAE40, "Shinobi", Plan::replace},
    {0x8C0BAE40, 0x8C0BAF20, "Shinobi: the peripherals' vibration and screen calls", Plan::lift},
    {0x8C0BAF20, 0x8C0BAFDC, "Shinobi", Plan::replace},
    {0x8C0BAFDC, 0x8C0BCFE0, "8 KB buffer (zero-filled; the middleware uses it)", Plan::data},
    // Software only (no hardware, no system calls): lifted.
    {0x8C0BCFE0, 0x8C0BD264, "Shinobi: the boot's next program's name and number; the GD-ROM's TOC and BCD helpers", Plan::lift},
    {0x8C0BD264, 0x8C0BDB14, "Shinobi's gdd (the GD-ROM driver, over the boot ROM's system calls)", Plan::lift},
    {0x8C0BDB14, 0x8C0BE9AC, "Shinobi", Plan::replace},
    {0x8C0BE9AC, 0x8C0C1078, "Shinobi's Maple driver: the bus's DMA and frames; bu's device layer", Plan::lift},
    {0x8C0C1078, 0x8C0C10BC, "Shinobi: the boot ROM's font calls", Plan::replace},
    {0x8C0C10BC, 0x8C0C1520, "Shinobi: character code conversions for the boot ROM's font (software only)", Plan::lift},
    {0x8C0C1520, 0x8C0C17C4, "Shinobi: the boot ROM's GD-ROM and flash calls", Plan::replace},
    {0x8C0C17C4, 0x8C0C1C0C, "Shinobi's vibration (Jump Pack) and screen (memory card) drivers", Plan::lift},
    {0x8C0C1C0C, 0x8C0C3C24, "8 KB stack (zero-filled; its top is referenced)", Plan::data},
    {0x8C0C3C24, 0x8C0C3CA4, "Shinobi", Plan::replace},
    {0x8C0C3CA4, 0x8C0C3D04, "Shinobi's exception handler's data", Plan::data},
    {0x8C0C3D04, 0x8C0C44B4, "Shinobi", Plan::replace},
    {0x8C0C44B4, 0x8C0C4774, "Shinobi's Maple driver: its system bus set-up", Plan::lift},
    // bu's thread library is lifted but for its two switches, which run guest
    // threads on host threads (toycommander/src/threads.cpp).
    {0x8C0C4774, 0x8C0C4A3A, "Shinobi: bu's thread library", Plan::lift},
    {0x8C0C4A3A, 0x8C0C4A78, "Shinobi: bu's thread library: a thread's yield (host threads)", Plan::replace},
    {0x8C0C4A78, 0x8C0C4A7C, "Shinobi: bu's thread library", Plan::lift},
    {0x8C0C4A7C, 0x8C0C4B4A, "Shinobi: bu's thread library: the scheduler's pass (host threads)", Plan::replace},
    {0x8C0C4B4A, 0x8C0C558C, "Shinobi", Plan::replace},
    {0x8C0C558C, 0x8C0C568C, "Shinobi: the G2 bus's register helpers (for the Maple driver)", Plan::lift},
    {0x8C0C568C, 0x8C0C5690, "Shinobi", Plan::replace},
    {0x8C0C5690, 0x8C0C6B9C, "Shinobi's initialised data and version strings", Plan::data},
};

constexpr ReplacedFunction kUsaReplaced[] = {
    {0x8C04EDF4, "memset", "the C runtime's fill through the store queues (docs/code-analysis.md)"},
    {0x8C04EEE0, "memclr", "the C runtime's clear through the store queues (docs/code-analysis.md)"},
    {0x8C085E16, "kamui_install_interrupt_handler", "puts Kamui 2's handler in the exception table (VBR)"},
    {0x8C085ED4, "kamui_remove_interrupt_handler", "takes Kamui 2's handler out of the exception table (VBR) again"},
    {0x8C04F0D0, "read_dbr", "the C runtime's read of the debug base register (stc dbr): a debugger's"},
    {0x8C099470, "debug_break", "a debugger's break (0x003B ; rts ; nop), which the engine's checks call when they fail", 6},
    {0x8C09947C, "debug_trap", "a debugger's trap (trapa #2 ; rts), the C runtime's default handler at 0x8C0994E8", 4},
};

// Hand-written code's spare entries and small functions nothing refers to
// (docs/code-analysis.md, "Every byte"): found by reading the code, lifted
// with the rest.
constexpr KnownEntry kUsaEntries[] = {
    {0x8C0613AC, "the engine's matrix code: an identity load into the back bank, falling into its neighbour's"},
    {0x8C06145C, "the engine's matrix code: a vector load from r4, falling into the code at 0x8C061464"},
    {0x8C06169C, "the engine's matrix code: the shared call into 0x8C0616A0 with its own routine"},
    {0x8C0616B8, "the engine's matrix code: 0x8C0615C8, then 0x8C06169C"},
    {0x8C0616C4, "the engine's matrix code: 0x8C0616A0 with its routine 0x8C0613D4"},
    {0x8C0616D0, "the engine's matrix code: 0x8C0616A0 with its routine 0x8C0613FC"},
    {0x8C0616DC, "the engine's matrix code: 0x8C0616A0 with its routine 0x8C0614A0"},
    {0x8C0616E8, "the engine's matrix code: 0x8C0616A0 with its routine 0x8C0614C8"},
    {0x8C0616F4, "the engine's matrix code: PR saved, then 0x8C0616F6 (where 0x8C061700's call returns with PR already in r4)"},
    {0x8C061DB8, "the engine's matrix code: a look-at rotation from the back bank's vector"},
    {0x8C061DC4, "the engine's matrix code: the same from a vector at r4"},
    {0x8C061ECC, "the engine's matrix code: the look-at rotation called with PR set by hand"},
    {0x8C061EE0, "the engine's matrix code: the look-at rotation, then 0x8C061EEC"},
    {0x8C099478, "the C runtime's stack pointer (rts ; mov r15,r0), beside the debugger stubs"},
    {0x8C0C400C, "Shinobi's exception stub (replaced: the port takes interrupts itself)"},
};

// Data inside code regions (docs/code-analysis.md, "Every byte").
constexpr DataBlock kUsaData[] = {
    {0x8C099564, 0x8C099568, "the middle word of an 8-byte constant at 0x8C099560 a literal pool points at"},
    {0x8C09D18C, 0x8C09D190, "the TrueMotion decoder's mask table between its block handlers, read with indexed loads"},
    {0x8C09D198, 0x8C09D19C, "the TrueMotion decoder's mask table (continued)"},
    {0x8C09D278, 0x8C09D288, "the TrueMotion decoder's mask table before the handler at 0x8C09D288"},
    {0x8C09D464, 0x8C09D468, "the TrueMotion decoder's mask table"},
    {0x8C09D71C, 0x8C09D720, "the TrueMotion decoder's mask table"},
    {0x8C09D728, 0x8C09D738, "the TrueMotion decoder's mask table before the handler at 0x8C09D738"},
    {0x8C09F324, 0x8C09F326, "the movie dispatcher's offsets the decoder patches (0x8C09F320-0x8C09F333)"},
    {0x8C09F328, 0x8C09F334, "the movie dispatcher's offsets (continued)"},
    {0x8C0C10B4, 0x8C0C10BC, "Shinobi's 8-character build tag, before 0x8C0C10BC"},
    {0x8C0C3FF8, 0x8C0C3FFC, "a literal nothing loads (0xAC004000) in Shinobi's exception code"},
};

// Toy Racer, Europe, MK-5114950, V1.050. The evidence for each boundary is in
// docs/toyracer/code-analysis.md.
constexpr Region kEurope[] = {
    {0x8C010000, 0x8C010030, "start-up (cache set-up, jump to syStart)", Plan::replace},
    {0x8C010030, 0x8C010080, "\"Lib Handle Start\" / \"Lib Handle End\" markers", Plan::data},
    {0x8C010080, 0x8C04CA58, "game", Plan::lift},
    {0x8C04CA58, 0x8C0A5B02,
     "middleware: No Cliche's engine, the audio manager, Kamui 2, kmdk, the network libraries, C runtime",
     Plan::undecided},
    {0x8C0A5B02, 0x8C0C5440, "initialised data of the game and the middleware", Plan::data},
    {0x8C0C5440, 0x8C0C5924, "Shinobi", Plan::replace},
    {0x8C0C5924, 0x8C0C5AFC, "Shinobi's pdKbd (the keyboard, over the Maple driver)", Plan::lift},
    {0x8C0C5AFC, 0x8C0C6530, "Shinobi", Plan::replace},
    {0x8C0C6530, 0x8C0C7184, "Shinobi's bu (the memory card's file system): its API", Plan::lift},
    {0x8C0C7184, 0x8C0C78E4, "Shinobi's flash memory functions (software over the boot ROM's flash calls, which are natives)", Plan::lift},
    {0x8C0C78E4, 0x8C0C7A66, "Shinobi: the system's settings (natives)", Plan::replace},
    {0x8C0C7A66, 0x8C0C7D04, "Shinobi: helpers of the network libraries (byte order, checksums; software only)", Plan::lift},
    {0x8C0C7D04, 0x8C0C7D30, "Shinobi: runs the disc's web browser (another program: stops)", Plan::replace},
    {0x8C0C7D30, 0x8C0C9BE8, "Shinobi's gdFs (the GD-ROM file system, over gdd)", Plan::lift},
    // Shinobi's peripheral library (pd), its memory card file system (bu) and
    // their Maple bus driver are lifted, over the Maple bus the port provides,
    // as in Toy Commander; the regions were found by matching its functions
    // (docs/runtime.md).
    {0x8C0C9BE8, 0x8C0CA150, "Shinobi's pd (peripherals)", Plan::lift},
    {0x8C0CA150, 0x8C0CA2AC, "Shinobi", Plan::replace},
    {0x8C0CA2AC, 0x8C0CA2FC, "Shinobi: the interrupt chains' tables cleared (for pd's set-up)", Plan::lift},
    {0x8C0CA2FC, 0x8C0CAB1A, "Shinobi", Plan::replace},
    {0x8C0CAB1A, 0x8C0CAB2E, "Shinobi: the peripherals' set-up", Plan::lift},
    {0x8C0CAB2E, 0x8C0CAC28, "Shinobi", Plan::replace},
    {0x8C0CAC28, 0x8C0D0B84, "Shinobi's Maple driver: the bus's servers, the devices' drivers, bu's core", Plan::lift},
    {0x8C0D0B84, 0x8C0D0CEC, "Shinobi", Plan::replace},
    {0x8C0D0CEC, 0x8C0D0DC0, "Shinobi: the peripherals' vibration and screen calls", Plan::lift},
    {0x8C0D0DC0, 0x8C0D16DC, "Shinobi (with the extension check and part of the modem library)", Plan::replace},
    {0x8C0D16DC, 0x8C0D18B4, "Shinobi: the GD-ROM's TOC and BCD helpers (software only, as Toy Commander's 0x8C0BD08C-0x8C0BD264)", Plan::lift},
    {0x8C0D18B4, 0x8C0D216C, "Shinobi's gdd (the GD-ROM driver, over the boot ROM's system calls)", Plan::lift},
    {0x8C0D216C, 0x8C0D2F50, "Shinobi", Plan::replace},
    {0x8C0D2F50, 0x8C0D58B4, "Shinobi's Maple driver: its system bus set-up, the bus's DMA and frames; bu's device layer", Plan::lift},
    {0x8C0D58B4, 0x8C0D58BC, "Shinobi's build tag", Plan::replace},
    {0x8C0D58BC, 0x8C0D5D20, "Shinobi: character code conversions for the boot ROM's font (software only, as Toy Commander's)", Plan::lift},
    {0x8C0D5D20, 0x8C0D5E24, "Shinobi (the boot ROM's system-call wrappers)", Plan::replace},
    {0x8C0D5E24, 0x8C0D62A0, "Shinobi's vibration (Jump Pack) and screen (memory card) drivers", Plan::lift},
    {0x8C0D62A0, 0x8C0DFFEC, "Shinobi and Sega's modem library", Plan::replace},
    {0x8C0DFFEC, 0x8C0E00A0, "Shinobi: the boot's next program's name and number (software only, as Toy Commander's 0x8C0BCFE0-0x8C0BD08C)", Plan::lift},
    {0x8C0E00A0, 0x8C0E08CC, "Shinobi", Plan::replace},
    {0x8C0E08CC, 0x8C0E0BC8, "Shinobi: the G2 bus's register helpers (for the Maple driver)", Plan::lift},
    // bu's thread library is lifted but for its two switches, which run guest
    // threads on host threads (toyracer/src/threads.cpp).
    {0x8C0E0BC8, 0x8C0E0E8E, "Shinobi: bu's thread library", Plan::lift},
    {0x8C0E0E8E, 0x8C0E0ECC, "Shinobi: bu's thread library: a thread's yield (host threads)", Plan::replace},
    {0x8C0E0ECC, 0x8C0E0ED0, "Shinobi: bu's thread library", Plan::lift},
    {0x8C0E0ED0, 0x8C0E0F9E, "Shinobi: bu's thread library: the scheduler's pass (host threads)", Plan::replace},
    {0x8C0E0F9E, 0x8C0F9BB4, "Sega's modem library", Plan::replace},
    {0x8C0F9BB4, 0x8C0FD624, "Shinobi's and the modem library's initialised data and version strings", Plan::data},
};

constexpr ReplacedFunction kEuropeReplaced[] = {
    {0x8C05E01C, "memset", "the C runtime's fill through the store queues (docs/code-analysis.md)"},
    {0x8C04DE3C, "struct_copy_even", "the compiler's structure copy: a jump through a table into an unrolled run"},
    {0x8C04DE48, "struct_copy_odd", "the same, for an odd number of words"},
    {0x8C04D758, "shinobi_timer_start", "Shinobi's timer 0 set-up (linked among the middleware here)"},
    {0x8C04D77E, "shinobi_timer_count", "timer 0's count so far"},
    {0x8C04D788, "shinobi_timer_diff", "the ticks between two counts"},
    {0x8C04DB04, "shinobi_cable", "Shinobi's cable query (linked among the middleware here): reads an I/O port"},
    {0x8C05E108, "memclr", "the C runtime's clear through the store queues (docs/code-analysis.md)"},
    {0x8C090AC6, "kamui_install_interrupt_handler", "puts Kamui 2's handler in the exception table (VBR)"},
    {0x8C090B84, "kamui_remove_interrupt_handler", "takes Kamui 2's handler out of the exception table (VBR) again"},
    {0x8C05E2F8, "read_dbr", "the C runtime's read of the debug base register (stc dbr): a debugger's"},
    {0x8C04D7E0, "shinobi_timer_interrupt_a", "Shinobi's timer interrupt set-up (linked among the middleware here): VBR"},
    {0x8C04D916, "shinobi_timer_interrupt_b", "another of Shinobi's timer interrupt set-ups: VBR"},
    {0x8C0D0AF8, "flash_info", "the boot ROM's flash call 0 (through the vector at 0x8C0000B8)"},
    {0x8C0D0B0C, "flash_read", "the boot ROM's flash call 1"},
    {0x8C0D0B20, "flash_write", "the boot ROM's flash call 2"},
    {0x8C0D0B34, "flash_delete", "the boot ROM's flash call 3"},
    {0x8C0A1DD4, "debug_break", "a debugger's break (0x003B ; rts ; nop), which the engine's checks call when they fail", 6},
    {0x8C0A1DE0, "debug_trap", "a debugger's trap (trapa #2 ; rts), the C runtime's default handler", 4},
    {0x8C09A900, "debug_adapter_check", "breaks (0x003B) if the development kit's debug adapter's name is at 0x8C004020", 0x18},
};

// What references can't show (docs/code-analysis.md, "Every byte"), as in
// Toy Commander's build: the engine's matrix code is the same code 0xCE34
// bytes on, byte for byte.
constexpr KnownEntry kEuropeEntries[] = {
    {0x8C06E1E0, "the engine's matrix code: an identity load into the back bank, falling into its neighbour's"},
    {0x8C06E290, "the engine's matrix code: a vector load from r4, falling into the code after it"},
    {0x8C06E4D0, "the engine's matrix code: the shared call into 0x8C06E4D4 with its own routine"},
    {0x8C06E4EC, "the engine's matrix code: 0x8C06E3FC, then 0x8C06E4D0"},
    {0x8C06E4F8, "the engine's matrix code: 0x8C06E4D4 with its routine"},
    {0x8C06E504, "the engine's matrix code: 0x8C06E4D4 with its routine"},
    {0x8C06E510, "the engine's matrix code: 0x8C06E4D4 with its routine"},
    {0x8C06E51C, "the engine's matrix code: 0x8C06E4D4 with its routine"},
    {0x8C06E528, "the engine's matrix code: PR saved, then the code after it"},
    {0x8C06EBEC, "the engine's matrix code: a look-at rotation from the back bank's vector"},
    {0x8C06EBF8, "the engine's matrix code: the same from a vector at r4"},
    {0x8C06ED00, "the engine's matrix code: the look-at rotation called with PR set by hand"},
    {0x8C06ED14, "the engine's matrix code: the look-at rotation, then the code after it"},
    {0x8C0A1DDC, "the C runtime's stack pointer (rts ; mov r15,r0), beside the debugger stubs"},
    {0x8C0E0424, "Shinobi's exception stub (replaced: the port takes interrupts itself)"},
    // Registered as an interrupt chain handler (event 0x900, by 0x8C082140):
    // r4 set, then the function at 0x8C082210. A guess there runs into that
    // function's start, so only the layout can name it.
    {0x8C08220E, "an interrupt chain handler: r4 set, then 0x8C082210"},
};
constexpr DataBlock kEuropeData[] = {
    {0x8C09A918, 0x8C09A934, "the debug adapter check's literal and the name it compares (read with mova)"},
    {0x8C09B8C8, 0x8C09B8D4, "literal pool entries nothing loads"},
    {0x8C09B8D8, 0x8C09B8DC, "a literal pool entry nothing loads (a code address)"},
    {0x8C09B8E0, 0x8C09B8F0, "literal pool entries nothing loads (code and data addresses)"},
    {0x8C09B964, 0x8C09B96C, "literal pool entries nothing loads (before an unreferenced delay loop)"},
    {0x8C0A1EC8, 0x8C0A1ECC, "the middle word of an 8-byte constant at 0x8C0A1EC4 a literal pool points at"},
    {0x8C0D58B4, 0x8C0D58BC, "Shinobi's 8-character build tag"},
    {0x8C0E0410, 0x8C0E0414, "a literal nothing loads in Shinobi's exception code"},
};

constexpr std::string_view kEuropeSha = "da29a065f9db779f0c95d778de36c019848b211ad93a667244bf58e9a10ee5d4";

constexpr std::string_view kUsaSha = "ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd";

}  // namespace

std::span<const Region> layout(std::string_view sha256_hex) {
    if (sha256_hex == kUsaSha) return kUsa;
    if (sha256_hex == kEuropeSha) return kEurope;
    return {};
}

namespace {
// The movie decoder's block handlers (docs/code-analysis.md): its two block
// loops turn on 64-bit transfers and call through tables the decoder fills
// from these, so the handlers run with SZ = 1 (and copy pixels with 8-byte
// fmov through the XD registers).
constexpr IndirectTargets kUsaIndirect[] = {
    {0x8C09F2AC, 0x8C09F2F6, 0x8C0A3CCC, 20, "the TrueMotion decoder's block handlers, called with SZ = 1"},
    // Its set-up (0x8C09B080, 0x8C09BFB6) also puts these in the tables the
    // dispatcher calls through: two pools of literals.
    {0x8C09F2AC, 0x8C09F2F6, 0x8C09B148, 4, "more of the decoder's block handlers (a literal pool)"},
    {0x8C09F2AC, 0x8C09F2F6, 0x8C09BFE0, 1, "one more of the decoder's block handlers (a literal)"},
};
}  // namespace

std::span<const IndirectTargets> indirect_targets(std::string_view sha256_hex) {
    if (sha256_hex == kUsaSha) return kUsaIndirect;
    return {};
}

std::span<const ReplacedFunction> replaced_functions(std::string_view sha256_hex) {
    if (sha256_hex == kUsaSha) return kUsaReplaced;
    if (sha256_hex == kEuropeSha) return kEuropeReplaced;
    return {};
}

std::span<const KnownEntry> known_entries(std::string_view sha256_hex) {
    if (sha256_hex == kUsaSha) return kUsaEntries;
    if (sha256_hex == kEuropeSha) return kEuropeEntries;
    return {};
}

std::span<const DataBlock> data_blocks(std::string_view sha256_hex) {
    if (sha256_hex == kUsaSha) return kUsaData;
    if (sha256_hex == kEuropeSha) return kEuropeData;
    return {};
}

}  // namespace qilin
