#include "clock.hpp"

#include <atomic>
#include <chrono>

namespace dreamcast {

namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();
std::atomic<int64_t> g_paused_total{0};  // ns spent paused, before the current pause
std::atomic<int64_t> g_paused_at{-1};    // when the current pause began, or -1
int64_t since_start() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - g_start).count(); }
}  // namespace

uint64_t guest_ns() {
    const int64_t at = g_paused_at.load(std::memory_order_acquire);
    const int64_t now = at >= 0 ? at : since_start();
    return static_cast<uint64_t>(now - g_paused_total.load(std::memory_order_acquire));
}

void pause_guest_time(bool paused) {
    if (paused) {
        if (g_paused_at.load() < 0) g_paused_at.store(since_start(), std::memory_order_release);
    } else if (const int64_t at = g_paused_at.load(); at >= 0) {
        g_paused_total.fetch_add(since_start() - at, std::memory_order_acq_rel);
        g_paused_at.store(-1, std::memory_order_release);
    }
}

}  // namespace dreamcast
