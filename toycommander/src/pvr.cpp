// The PowerVR2's and the system bus's registers, 0x005F6800-0x005F9FFF
// (physical). For now a register file: what is written is kept and read
// back, but for the registers that identify the hardware. Registers with
// behaviour (starting a render, the list initialisation, the display, the
// interrupt status, DMA) get it as the bring-up reaches them.
#include "pvr.hpp"

#include <cstdint>
#include <cstring>

#include "bus.hpp"
#include "guest.hpp"

namespace toycommander {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kLo = 0x005F6800u, kHi = 0x005FA000u;
uint32_t g_regs[(kHi - kLo) / 4];

constexpr uint32_t kId = 0x005F8000u;        // PVR: ID, the retail chip's
constexpr uint32_t kRevision = 0x005F8004u;  // PVR: revision, the retail chip's (17)

uint32_t& reg(uint32_t a) { return g_regs[(a - kLo) / 4]; }

// The system bus's interrupt status: normal events (0x005F6900), external
// (0x005F6904) and errors (0x005F6908). The hardware sets their bits; writing
// ones clears them.
constexpr uint32_t kStatusNormal = 0x005F6900u, kStatusExternal = 0x005F6904u, kStatusError = 0x005F6908u;

uint32_t read(uint32_t a, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    switch (a) {
    case kId: return 0x17FD11DBu;
    case kRevision: return 0x11u;
    default: return reg(a);
    }
}

void write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    switch (a) {
    case kStatusNormal:
    case kStatusExternal:
    case kStatusError: reg(a) &= ~v; return;
    default: reg(a) = v; return;
    }
}

// Video memory: 8 MB as two 4 MB banks. The 32-bit view (0x05000000, and
// 0x07000000) has the banks one after the other; the 64-bit view
// (0x04000000, and 0x06000000), which the texture hardware reads, has them
// interleaved every 4 bytes. Kept in the 32-bit view's order.
uint8_t g_vram[8u << 20];
uint32_t vram_offset(uint32_t a, unsigned n) {
    const uint32_t off = a & 0x007FFFFFu;
    uint32_t linear;
    if (a & 0x01000000u) linear = off;  // the 32-bit view
    else linear = ((off & 4u) << 20) | ((off >> 1) & 0x003FFFFCu) | (off & 3u);  // the 64-bit view
    if ((linear & 0x3FFFFFu) + n > 0x400000u && !(a & 0x01000000u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    if (linear + n > sizeof g_vram) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    return linear;
}
uint32_t vram_read(uint32_t a, unsigned n) {
    uint32_t v = 0;
    std::memcpy(&v, g_vram + vram_offset(a, n), n);
    return v;
}
void vram_write(uint32_t a, uint32_t v, unsigned n) { std::memcpy(g_vram + vram_offset(a, n), &v, n); }
void vram_block(uint32_t a, const uint8_t* data, unsigned n) {
    for (unsigned k = 0; k < n; k += 4) std::memcpy(g_vram + vram_offset(a + k, 4), data + k, 4);
}

}  // namespace

void map_pvr() {
    rt::map({kLo, kHi, "PowerVR2 and system bus registers", read, write});
    rt::map({0x04000000u, 0x08000000u, "video memory", vram_read, vram_write, vram_block});
}

const uint8_t* vram() { return g_vram; }

uint32_t pvr_register(uint32_t a) { return reg(a & 0x1FFFFFFFu); }
void set_pvr_register(uint32_t a, uint32_t v) { write(a & 0x1FFFFFFFu, v, 4); }
void raise_event(unsigned bit) { reg(kStatusNormal) |= 1u << bit; }

}  // namespace toycommander
