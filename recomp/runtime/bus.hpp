// Accesses outside the fast path of RAM (guest.hpp): RAM's other views, the
// SH-4's store queues, and the devices the runtime maps (the hardware the
// port provides in place of the console's). Anything nothing handles stops,
// naming the address. Every access here is rare next to RAM's.
//
// Addresses below 0xE0000000 are decoded as the SH-4 decodes them: the low 29
// bits are the physical address, whatever the region (P0, P1, P2, P3), and
// RAM is area 3, 0x0C000000-0x0FFFFFFF, its 16 MB seen four times. From
// 0xE0000000 (P4) addresses are taken as they are.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>

namespace qilin::rt {

struct Device {
    uint32_t lo, hi;  // [lo, hi): physical addresses, or P4 addresses
    const char* name;
    uint32_t (*read)(uint32_t a, unsigned size);
    void (*write)(uint32_t a, uint32_t v, unsigned size);
    // 32 bytes from a store queue (or a block from DMA), when the device
    // takes them whole; otherwise they come as 4-byte writes.
    void (*write_block)(uint32_t a, const uint8_t* data, unsigned size) = nullptr;
};

// Maps a device. Overlapping another is a mistake and stops.
void map(const Device& d);

// Logs every access the bus takes, to stderr (QILIN_TRACE_BUS=1 sets it).
void trace_bus(bool on);

// The address a store queue sends its 32 bytes to, from QACR0/QACR1 (which
// lifted code sets through the bus, as Kamui 2 does).
uint32_t store_queue_target(uint32_t sq_address);

}  // namespace qilin::rt
