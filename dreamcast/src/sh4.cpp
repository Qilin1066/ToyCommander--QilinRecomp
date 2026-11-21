// The timer unit, from the SH-4's description of it: three 32-bit
// down-counters (TCNT0-2), each reloaded from its constant (TCOR) when it
// underflows, which sets UNF (bit 8) in its control register (TCR), whose
// bits 2-0 (TPSC) pick the count rate from the peripheral clock, Pφ (50 MHz
// on the Dreamcast): /4, /16, /64, /256, /1024. TSTR's bits 0-2 start them.
// Counting follows the guest clock. An underflow interrupt (TCR's UNIE,
// bit 5), the other clock sources and input capture aren't supported yet:
// programming them stops.
#include "sh4.hpp"

#include "bus.hpp"
#include "clock.hpp"
#include "guest.hpp"

namespace dreamcast {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kBase = 0xFFD80000u;
constexpr uint64_t kPeripheralHz = 50'000'000u;

struct Timer {
    uint32_t tcor = 0xFFFFFFFFu, tcr = 0;
    uint32_t count = 0xFFFFFFFFu;  // TCNT when counting (re)started
    uint64_t since = 0;            // guest ns then
    bool underflowed = false;
};
Timer g_timer[3];
uint8_t g_tocr = 0, g_tstr = 0;

uint64_t rate(const Timer& t) {
    static constexpr uint32_t kDiv[5] = {4, 16, 64, 256, 1024};
    return kPeripheralHz / kDiv[t.tcr & 7u];
}
uint64_t ticks_since(const Timer& t, uint64_t now) { return (now - t.since) / 1000u * rate(t) / 1'000'000u; }

// The counter now, and whether it has underflowed since it was set.
uint32_t current(int n, uint64_t now) {
    Timer& t = g_timer[n];
    if (!(g_tstr & (1u << n))) return t.count;
    const uint64_t elapsed = ticks_since(t, now);
    if (elapsed <= t.count) return t.count - static_cast<uint32_t>(elapsed);
    t.underflowed = true;
    const uint64_t period = uint64_t(t.tcor) + 1, e = elapsed - t.count - 1;
    return t.tcor - static_cast<uint32_t>(e % period);
}
// Restart counting from the current value (on any change of how it counts).
void rebase(int n) {
    const uint64_t now = guest_ns();
    g_timer[n].count = current(n, now);
    g_timer[n].since = now;
}

[[noreturn]] void unsupported(uint32_t a, uint32_t v) { throw rt::Stop{rt::Stop::Unsupported, 0, (a & 0xFFu) << 24 | (v & 0xFFFFFFu)}; }

uint32_t read(uint32_t a, unsigned) {
    const uint32_t off = a - kBase;
    if (off == 0x00) return g_tocr;
    if (off == 0x04) return g_tstr;
    if (off >= 0x08 && off < 0x2C) {
        const int n = static_cast<int>((off - 0x08) / 12);
        switch ((off - 0x08) % 12) {
        case 0: return g_timer[n].tcor;
        case 4: return current(n, guest_ns());
        case 8: {
            (void)current(n, guest_ns());
            return (g_timer[n].tcr & ~0x100u) | (g_timer[n].underflowed ? 0x100u : 0u);
        }
        }
    }
    unsupported(a, 0);
}

void write(uint32_t a, uint32_t v, unsigned) {
    const uint32_t off = a - kBase;
    if (off == 0x00) {
        g_tocr = static_cast<uint8_t>(v);
        return;
    }
    if (off == 0x04) {
        for (int n = 0; n < 3; ++n) rebase(n);
        g_tstr = static_cast<uint8_t>(v & 7u);
        for (int n = 0; n < 3; ++n) g_timer[n].since = guest_ns();
        return;
    }
    if (off >= 0x08 && off < 0x2C) {
        const int n = static_cast<int>((off - 0x08) / 12);
        Timer& t = g_timer[n];
        switch ((off - 0x08) % 12) {
        case 0: t.tcor = v; return;
        case 4: t.count = v, t.since = guest_ns(); return;
        case 8:
            if ((v & 7u) > 4 || (v & 0x20u) || (n == 2 && (v & 0xC0u))) unsupported(a, v);
            rebase(n);
            t.tcr = v & 0xFFu & ~0x100u;
            if (!(v & 0x100u)) t.underflowed = false;  // UNF is cleared by writing 0
            return;
        }
    }
    unsupported(a, v);
}

// The DMA controller (DMAC), 0xFFA00000: four channels of SAR, DAR, DMATCR
// and CHCR, 16 bytes apart, and DMAOR at 0x40. Only channel 2 is used, by
// Kamui 2, in the one way the Dreamcast uses it: from RAM to the graphics
// chip in 32-byte blocks, started by the system bus's C2DST (pvr.cpp). The
// registers are kept as written; what a channel does is the bus's.
constexpr uint32_t kDmacBase = 0xFFA00000u;
uint32_t g_dmac[0x44 / 4];

uint32_t dmac_read(uint32_t a, unsigned n) {
    if (n != 4 || (a & 3u)) unsupported(a, 0);
    return g_dmac[(a - kDmacBase) / 4];
}
void dmac_write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4 || (a & 3u)) unsupported(a, v);
    const uint32_t off = a - kDmacBase;
    // Channels 0, 1 and 3 are never started by the code: one being enabled is new.
    if (off < 0x40 && (off & 0xFu) == 0xCu && off != 0x2Cu && (v & 1u)) unsupported(a, v);
    g_dmac[off / 4] = v;
}

// The operand cache's address array (0xF4000000-0xF4FFFFFF): code writes
// a line's tag with U = V = 0 to invalidate it (the movie player clears the
// whole cache that way after a DMA into memory). The port has no cache:
// such writes do nothing; anything else (a read, a valid or dirty tag) is
// new and stops.
void oc_address_write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4 || (v & 3u)) unsupported(a, v);
}

}  // namespace

void map_sh4_peripherals() {
    rt::map({0xF4000000u, 0xF5000000u, "SH-4 operand cache address array", nullptr, oc_address_write});
    rt::map({kBase, kBase + 0x30u, "SH-4 timer unit", read, write});
    rt::map({kDmacBase, kDmacBase + 0x44u, "SH-4 DMA controller", dmac_read, dmac_write});
}

Dma2 dma_channel2() { return {g_dmac[0x20 / 4], g_dmac[0x28 / 4], g_dmac[0x2C / 4], g_dmac[0x40 / 4]}; }

void end_dma_channel2(uint32_t bytes) {
    g_dmac[0x20 / 4] += bytes;  // SAR2 has moved on
    g_dmac[0x28 / 4] = 0;       // DMATCR2: nothing left
    g_dmac[0x2C / 4] |= 2u;     // CHCR2.TE: transfer ended
}

void start_free_running_timer0() {
    write(kBase + 0x00, 0, 1);
    write(kBase + 0x04, g_tstr & ~1u, 1);
    write(kBase + 0x10, 2, 2);
    write(kBase + 0x08, 0xFFFFFFFFu, 4);
    write(kBase + 0x0C, 0xFFFFFFFFu, 4);
    write(kBase + 0x04, g_tstr | 1u, 1);
}

uint32_t timer0_count() { return current(0, guest_ns()); }

}  // namespace dreamcast
