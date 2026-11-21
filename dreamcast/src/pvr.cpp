// The PowerVR2's and the system bus's registers, 0x005F6800-0x005F9FFF
// (physical). For now a register file: what is written is kept and read
// back, but for the registers that identify the hardware. Registers with
// behaviour (starting a render, the list initialisation, the display, the
// interrupt status, DMA) get it as the bring-up reaches them.
#include "pvr.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "bus.hpp"
#include "guest.hpp"
#include "interrupts.hpp"
#include "maple.hpp"
#include "sh4.hpp"
#include "ta.hpp"
#include "video.hpp"

namespace dreamcast {

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

void vram_block(uint32_t a, const uint8_t* data, unsigned n);

// TA_LIST_INIT (0x005F8144): bit 31 starts a new scene's lists.
// STARTRENDER (0x005F8014): renders the scene the lists built. For now the
// renderer (video.cpp) draws the scene and the render ends at once: the
// ISP's, the TSP's and the video's ends (normal events 1, 2 and 0) are raised.
constexpr uint32_t kTaListInit = 0x005F8144u, kStartRender = 0x005F8014u;
void start_render() {
    video::render();
    raise_event(0), raise_event(1), raise_event(2);
}

// Channel 2's DMA, from RAM to the graphics chip: C2DSTAT (0x005F6800) is
// where to, C2DLEN (0x005F6804) how many bytes, and writing 1 to C2DST
// (0x005F6808) starts it, with the SH-4's channel 2 programmed for 32-byte
// blocks from SAR2 (CHCR2 = 0x12C1, DMAOR's DME set). The transfer is done
// at once; then C2DST reads 0, channel 2 shows it ended, and the normal
// event 19 (channel 2's DMA end) is raised.
constexpr uint32_t kC2Dstat = 0x005F6800u, kC2Dlen = 0x005F6804u, kC2Dst = 0x005F6808u;
void dma_to_chip() {
    const Dma2 ch = dma_channel2();
    const uint32_t to = reg(kC2Dstat), bytes = reg(kC2Dlen);
    if (ch.chcr != 0x12C1u || !(ch.dmaor & 1u) || bytes == 0 || bytes % 32 || ch.dmatcr * 32 != bytes)
        throw rt::Stop{rt::Stop::Unsupported, 0, ch.chcr};
    // Where it goes: 0x10000000-0x10FFFFFF the tile accelerator (its
    // parameter stream, then the YUV converter), 0x11000000-0x11FFFFFF
    // texture memory through the 64-bit path, then both again.
    const uint32_t area = to & 0x03800000u;
    const bool to_ta = area == 0, to_texture = (area & 0x03000000u) == 0x01000000u;
    if (!to_ta && !to_texture) throw rt::Stop{rt::Stop::Unsupported, 0, to};
    if (to_texture && (reg(0x005F6884u) & 1u)) throw rt::Stop{rt::Stop::Unsupported, 0, to};  // LMMODE0: the 32-bit path
    uint8_t block[32];
    for (uint32_t k = 0; k < bytes; k += 32) {
        for (uint32_t i = 0; i < 32; i += 4) {
            const uint32_t w = rt::ld32(ch.sar + k + i);
            std::memcpy(block + i, &w, 4);
        }
        if (to_ta) ta::feed(block, 32);
        else vram_block(0x04000000u | ((to + k) & 0x00FFFFFFu), block, 32);
    }
    end_dma_channel2(bytes);
    reg(kC2Dst) = 0;
    raise_event(19);
}

void write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    switch (a) {
    case kC2Dst:
        reg(a) = v & 1u;
        if (v & 1u) dma_to_chip();
        return;
    case kTaListInit:
        reg(a) = v & 0x7FFFFFFFu;  // reads back without the start bit
        if (v & 0x80000000u) ta::list_init();
        return;
    case 0x005F6C18u:  // MDST: the Maple bus's DMA (maple.cpp)
        reg(a) = v & 1u;
        if (v & 1u) maple::start();
        return;
    case kStartRender:
        reg(a) = v;
        start_render();
        return;
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
// When each 4 KB page was last written (vram64_generation).
constexpr uint32_t kPageShift = 12;
uint64_t g_page_written[sizeof g_vram >> kPageShift];
uint64_t g_writes = 0;
void vram_write(uint32_t a, uint32_t v, unsigned n) {
    const uint32_t at = vram_offset(a, n);
    std::memcpy(g_vram + at, &v, n);
    g_page_written[at >> kPageShift] = ++g_writes;
}
void vram_block(uint32_t a, const uint8_t* data, unsigned n) {
    ++g_writes;
    for (unsigned k = 0; k < n; k += 4) {
        const uint32_t at = vram_offset(a + k, 4);
        std::memcpy(g_vram + at, data + k, 4);
        g_page_written[at >> kPageShift] = g_writes;
    }
}

}  // namespace

void map_pvr() {
    rt::map({kLo, kHi, "PowerVR2 and system bus registers", read, write});
    rt::map({0x04000000u, 0x08000000u, "video memory", vram_read, vram_write, vram_block});
}

const uint8_t* vram() { return g_vram; }
// The texture hardware's addresses wrap at the end of the 8 MB: a texture
// declared taller than what is used (a 640-wide stride texture 512 lines
// high near the end of video memory, at the title) reads on from the start,
// as on the console.
void vram64_read(uint32_t offset, uint8_t* out, uint32_t n) {
    if (offset >= sizeof g_vram || n > sizeof g_vram) throw rt::Stop{rt::Stop::Unsupported, 0, offset};
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t off = (offset + k) & (sizeof g_vram - 1);
        out[k] = g_vram[((off & 4u) << 20) | ((off >> 1) & 0x003FFFFCu) | (off & 3u)];
    }
}
uint64_t vram64_generation(uint32_t offset, uint32_t n) {
    if (offset >= sizeof g_vram || n > sizeof g_vram) throw rt::Stop{rt::Stop::Unsupported, 0, offset};
    if (n > sizeof g_vram - offset) {  // wraps (vram64_read)
        const uint32_t first_part = static_cast<uint32_t>(sizeof g_vram) - offset;
        return std::max(vram64_generation(offset, first_part), vram64_generation(0, n - first_part));
    }
    // The range is half as long in each bank.
    const uint32_t first = (offset >> 1) >> kPageShift, last = ((offset + n + 7u) >> 1) >> kPageShift;
    constexpr uint32_t kBankPages = (4u << 20) >> kPageShift;
    uint64_t newest = 0;
    for (uint32_t p = first; p <= last && p < kBankPages; ++p) newest = std::max({newest, g_page_written[p], g_page_written[kBankPages + p]});
    return newest;
}

uint32_t pvr_register(uint32_t a) { return reg(a & 0x1FFFFFFFu); }
const uint32_t* pvr_palette() { return &reg(0x005F9000u); }
void set_pvr_register(uint32_t a, uint32_t v) { write(a & 0x1FFFFFFFu, v, 4); }
void raise_event(unsigned bit) {
    reg(kStatusNormal) |= 1u << bit;
    interrupt_pending();
}
void raise_external_event(unsigned bit) {
    reg(kStatusExternal) |= 1u << bit;
    interrupt_pending();
}

}  // namespace dreamcast
