#include "aica.hpp"

#include <cstring>

#include "bus.hpp"
#include "guest.hpp"

namespace toycommander {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kRamLo = 0x00800000u, kRamBytes = 2u << 20;
constexpr uint32_t kRegLo = 0x00700000u, kRegBytes = 0x10000u;
uint8_t g_ram[kRamBytes];
uint8_t g_regs[kRegBytes];

uint32_t ram_read(uint32_t a, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    uint32_t v = 0;
    std::memcpy(&v, g_ram + off, n);
    return v;
}
void ram_write(uint32_t a, uint32_t v, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    std::memcpy(g_ram + off, &v, n);
}
void ram_block(uint32_t a, const uint8_t* data, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    std::memcpy(g_ram + off, data, n);
}

// The registers are 32-bit words on the G2 bus, of which the chip uses the
// low 16 bits; accesses are 4 bytes wide.
uint32_t reg_read(uint32_t a, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    uint32_t v;
    std::memcpy(&v, g_regs + (a - kRegLo), 4);
    return v;
}
// The ARM's reset (0x00702C00, bit 0: held in reset while set). Releasing
// it starts the driver; the port's stand-in does what the driver's start-up
// does that the audio manager can see: it writes where the sound RAM it
// doesn't use begins (0x30040) to 0xB408 and to 0xEC, which the audio
// manager's install waits on (docs/runtime.md, the audio manager).
constexpr uint32_t kArmReset = 0x00702C00u;
void start_driver() {
    const uint32_t free_from = 0x30040u;
    std::memcpy(g_ram + 0xB408u, &free_from, 4);
    std::memcpy(g_ram + 0xECu, &free_from, 4);
}

void reg_write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    uint32_t old;
    std::memcpy(&old, g_regs + (a - kRegLo), 4);
    std::memcpy(g_regs + (a - kRegLo), &v, 4);
    if (a == kArmReset && (old & 1u) && !(v & 1u)) start_driver();
}

}  // namespace

void map_aica() {
    rt::map({kRamLo, kRamLo + kRamBytes, "sound RAM", ram_read, ram_write, ram_block});
    rt::map({kRegLo, kRegLo + kRegBytes, "sound chip registers", reg_read, reg_write});
}

uint8_t* sound_ram() { return g_ram; }

}  // namespace toycommander
