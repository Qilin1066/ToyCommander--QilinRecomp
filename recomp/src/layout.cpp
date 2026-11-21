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
    {0x8C0B0340, 0x8C0BAFDC, "Shinobi", Plan::replace},
    {0x8C0BAFDC, 0x8C0BCFE0, "8 KB buffer (zero-filled; the middleware uses it)", Plan::data},
    {0x8C0BCFE0, 0x8C0C1C0C, "Shinobi", Plan::replace},
    {0x8C0C1C0C, 0x8C0C3C24, "8 KB stack (zero-filled; its top is referenced)", Plan::data},
    {0x8C0C3C24, 0x8C0C3CA4, "Shinobi", Plan::replace},
    {0x8C0C3CA4, 0x8C0C3D04, "Shinobi's exception handler's data", Plan::data},
    {0x8C0C3D04, 0x8C0C5690, "Shinobi", Plan::replace},
    {0x8C0C5690, 0x8C0C6B9C, "Shinobi's initialised data and version strings", Plan::data},
};

}  // namespace

std::span<const Region> layout(std::string_view sha256_hex) {
    if (sha256_hex == "ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd") return kUsa;
    return {};
}

}  // namespace qilin
