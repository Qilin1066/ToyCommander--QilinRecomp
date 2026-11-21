// The start-up, natively. The entry code at 0x8C010000 sets the cache
// control register and jumps to Shinobi's start-up at 0x8C0C5440, which, in
// the supported build (docs/code-analysis.md):
//   - fills 0x8C00C000-0x8C00F400 with the word 0x41474553 ("SEGA"): the
//     stack, whose top is where the boot ROM leaves r15;
//   - clears the zero-initialised data and copies the initialised data, as
//     SHC's section initialisation does (0x8C0C5480), from four tables of
//     start and end addresses in the image;
//   - calls 0x8C0C5856, Shinobi's initialisation, with r4 = the word at 0x8C0F9BB4;
//   - calls the game's main, 0x8C0204E4;
//   - goes on to 0x8C0C58AE, Shinobi's exit, with r4 = the same word.
// The calls go through the function table, so Shinobi's functions reach the
// registry of native replacements, and stop naming any that has none yet.
#include "boot.hpp"

#include <cstring>

#include "bootfile.hpp"
#include "system.hpp"

namespace toyracer {

namespace rt = qilin::rt;

namespace {

uint32_t peek32(uint32_t a) { return rt::ld32(a); }

// SHC's section initialisation, from Shinobi's start-up (0x8C0C5480): the
// addresses of each table's start and end words.
void init_sections() {
    // Zero-initialised data, cleared a word at a time.
    for (uint32_t a = peek32(0x8C0A5B1Cu), end = peek32(0x8C0A5B28u); a < end; a += 4) rt::st32(a, 0);
    // Initialised data, copied a byte at a time from its image.
    for (uint32_t d = peek32(0x8C0A5B0Cu), s = peek32(0x8C0A5B04u), end = peek32(0x8C0A5B18u); d < end; ++d, ++s) rt::st8(d, rt::ld8(s));
    // Shinobi's own: zero-initialised, then initialised.
    for (uint32_t a = peek32(0x8C0F9BDCu), end = peek32(0x8C0F9BE8u); a < end; ++a) rt::st8(a, 0);
    for (uint32_t d = peek32(0x8C0F9BCCu), s = peek32(0x8C0F9BC4u), end = peek32(0x8C0F9BD8u); d < end; ++d, ++s) rt::st8(d, rt::ld8(s));
}

}  // namespace

void boot(rt::Context& c, std::span<const uint8_t> boot_file) {
    rt::check(qilin::kBootFileBase, static_cast<uint32_t>(boot_file.size()));
    std::memcpy(rt::g_mem + qilin::kBootFileBase, boot_file.data(), boot_file.size());
    c = rt::Context{};
    c.r[15] = 0x8C00F400u;  // the top of the stack the start-up fills
    for (uint32_t a = 0x8C00C000u; a < 0x8C00F400u; a += 4) rt::st32(a, 0x41474553u);
    init_sections();
    c.r[4] = peek32(0x8C0F9BB4u);
    rt::call_guest(c, 0x8C0C5856u);
    rt::call_guest(c, 0x8C0204E4u);
    c.r[4] = peek32(0x8C0F9BB4u);
    rt::call_guest(c, 0x8C0C58AEu);
}

}  // namespace toyracer
