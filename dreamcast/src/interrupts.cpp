#include "interrupts.hpp"

#include <bit>
#include <chrono>
#include <thread>

#include "clock.hpp"
#include "game.hpp"
#include "guest.hpp"
#include "maple.hpp"
#include "pvr.hpp"
#include "video.hpp"
#include "system.hpp"

namespace dreamcast {

namespace rt = qilin::rt;

namespace {

// VGA timing: 525 lines a frame, 60 frames a second; the vertical blank
// begins after line 480 (normal event 3) and ends at the frame's start
// (normal event 4).
constexpr uint64_t kFrameNs = 1'000'000'000ull / 60;
uint64_t g_frames = 0;  // frames whose blank has been raised

// The level-6 interrupt: the graphics chip's and the system bus's events
// enabled in IML6 (normal 0x005F6930, external 0x005F6934, errors
// 0x005F6938). Kamui 2's handler takes one event per call and acknowledges it.
uint32_t level6_pending() {
    return (pvr_register(0x005F6900u) & pvr_register(0x005F6930u)) | (pvr_register(0x005F6904u) & pvr_register(0x005F6934u)) |
           (pvr_register(0x005F6908u) & pvr_register(0x005F6938u));
}

void raise_blanks() {
    const uint64_t due = guest_ns() / kFrameNs;
    if (due > g_frames) {
        // Frames the guest didn't take are gone, as on the console: one blank.
        g_frames = due;
        raise_event(3);
        raise_event(4);
        maple::vblank();
        video::vblank();
    }
}

// The level-4 interrupt: the system bus's normal events 12-20 enabled in
// IML4 (0x005F6920) and its external events 0-3 enabled in 0x005F6924
// (Shinobi's chains set the bits, chain.cpp), which Shinobi's level-4
// handler takes: it acknowledges each and runs the handlers chained on its
// event (0xA00 + 0x20 * (bit - 12); 0xB00 + 0x20 * bit). The Maple
// driver's DMA-end handlers (event 0xA00) run there, bu's scheduler pass
// among them, and the audio manager's handler of the sound chip's interrupt
// (0xB20). Returns whether one was held back by IMASK.
bool level4(rt::Context& c) {
    for (int n = 0; n < 64; ++n) {
        const uint32_t normal = pvr_register(0x005F6900u) & pvr_register(0x005F6920u) & 0x001FF000u;
        const uint32_t external = pvr_register(0x005F6904u) & pvr_register(0x005F6924u) & 0xFu;
        if (!normal && !external) return false;
        if (((c.sr >> 4) & 15u) >= 4) return true;
        if (normal) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(normal));
            set_pvr_register(0x005F6900u, 1u << bit);
            game::raise_bus_event(c, 0xA00u + 0x20u * (bit - 12));
        } else {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(external));
            set_pvr_register(0x005F6904u, 1u << bit);
            game::raise_bus_event(c, 0xB00u + 0x20u * bit);
        }
    }
    return false;
}

void service(rt::Context& c) {
    raise_blanks();
    rt::g_irq_waiting = false;
    const uint32_t handler = game::interrupt_handler(0x224u);
    for (int n = 0; n < 64 && level6_pending(); ++n) {
        if (((c.sr >> 4) & 15u) >= 6 || handler == 0) {
            rt::g_irq_waiting = true;
            break;
        }
        const rt::Context saved = c;
        c.sr = (c.sr & ~0xF0u) | 0x60u;
        rt::call_guest(c, handler);
        c = saved;
    }
    // Level 6 first, as the console's priorities have it; then level 4.
    if (level4(c)) rt::g_irq_waiting = true;
}

}  // namespace

void interrupt_pending() { rt::g_irq_pending.store(true, std::memory_order_relaxed); }

void start_interrupts() {
    rt::g_irq_service = service;
    std::thread([] {
        auto next = std::chrono::steady_clock::now();
        for (;;) {
            next += std::chrono::nanoseconds(kFrameNs);
            std::this_thread::sleep_until(next);
            interrupt_pending();
        }
    }).detach();
}

}  // namespace dreamcast
