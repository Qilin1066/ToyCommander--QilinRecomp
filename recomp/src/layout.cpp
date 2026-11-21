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
    {0x8C0BAB98, 0x8C0BAFDC, "Shinobi", Plan::replace},
    {0x8C0BAFDC, 0x8C0BCFE0, "8 KB buffer (zero-filled; the middleware uses it)", Plan::data},
    {0x8C0BCFE0, 0x8C0BD264, "Shinobi", Plan::replace},
    {0x8C0BD264, 0x8C0BDB14, "Shinobi's gdd (the GD-ROM driver, over the boot ROM's system calls)", Plan::lift},
    {0x8C0BDB14, 0x8C0BE9AC, "Shinobi", Plan::replace},
    {0x8C0BE9AC, 0x8C0C1078, "Shinobi's Maple driver: the bus's DMA and frames; bu's device layer", Plan::lift},
    {0x8C0C1078, 0x8C0C1C0C, "Shinobi", Plan::replace},
    {0x8C0C1C0C, 0x8C0C3C24, "8 KB stack (zero-filled; its top is referenced)", Plan::data},
    {0x8C0C3C24, 0x8C0C3CA4, "Shinobi", Plan::replace},
    {0x8C0C3CA4, 0x8C0C3D04, "Shinobi's exception handler's data", Plan::data},
    {0x8C0C3D04, 0x8C0C44B4, "Shinobi", Plan::replace},
    {0x8C0C44B4, 0x8C0C4774, "Shinobi's Maple driver: its system bus set-up", Plan::lift},
    {0x8C0C4774, 0x8C0C558C, "Shinobi", Plan::replace},
    {0x8C0C558C, 0x8C0C568C, "Shinobi: the G2 bus's register helpers (for the Maple driver)", Plan::lift},
    {0x8C0C568C, 0x8C0C5690, "Shinobi", Plan::replace},
    {0x8C0C5690, 0x8C0C6B9C, "Shinobi's initialised data and version strings", Plan::data},
};

constexpr ReplacedFunction kUsaReplaced[] = {
    {0x8C04EDF4, "memset", "the C runtime's fill through the store queues (docs/code-analysis.md)"},
    {0x8C04EEE0, "memclr", "the C runtime's clear through the store queues (docs/code-analysis.md)"},
    {0x8C085E16, "kamui_install_interrupt_handler", "puts Kamui 2's handler in the exception table (VBR)"},
};

constexpr std::string_view kUsaSha = "ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd";

}  // namespace

std::span<const Region> layout(std::string_view sha256_hex) {
    if (sha256_hex == kUsaSha) return kUsa;
    return {};
}

std::span<const ReplacedFunction> replaced_functions(std::string_view sha256_hex) {
    if (sha256_hex == kUsaSha) return kUsaReplaced;
    return {};
}

}  // namespace qilin
