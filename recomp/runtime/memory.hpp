// The guest's 32-bit address space, reserved flat in the host's 64-bit one:
// guest address a is host address base() + a (rt::g_mem). Only committed
// ranges are backed; anything else faults, and in a build with
// QILIN_CHECKED_MEMORY the access stops first, naming the instruction.
//
// Runtime code: no std::format (macOS 10.15, see CMakeLists.txt).
#pragma once

#include <cstdint>

namespace qilin::rt {

class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    bool reserve();                                // 4 GB of address space, nothing backed
    bool commit(uint32_t address, uint32_t size);  // back a range with zeroed, writable memory
    uint8_t* base() const { return base_; }

private:
    uint8_t* base_ = nullptr;
};

}  // namespace qilin::rt
