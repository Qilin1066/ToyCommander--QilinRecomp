#include "clock.hpp"

#include <chrono>

namespace toycommander {

uint64_t guest_ns() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

}  // namespace toycommander
