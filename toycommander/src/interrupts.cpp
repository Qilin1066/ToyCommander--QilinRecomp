#include "interrupts.hpp"

#include <chrono>
#include <thread>

#include "clock.hpp"
#include "guest.hpp"
#include "natives.hpp"
#include "pvr.hpp"
#include "video.hpp"
#include "system.hpp"

namespace toycommander {

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
    }
}

void service(rt::Context& c) {
    raise_blanks();
    video::poll();
    rt::g_irq_waiting = false;
    const uint32_t handler = interrupt_handler(0x224u);
    for (int n = 0; n < 64 && level6_pending(); ++n) {
        if (((c.sr >> 4) & 15u) >= 6 || handler == 0) {
            rt::g_irq_waiting = true;
            return;
        }
        const rt::Context saved = c;
        c.sr = (c.sr & ~0xF0u) | 0x60u;
        rt::call_guest(c, handler);
        c = saved;
    }
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

}  // namespace toycommander
