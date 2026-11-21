// Shinobi's allocator (syMalloc), natively, over the same guest RAM and with
// the same state as the guest code, so every block lands where it would on
// the console: a circular free list in address order, 32-byte units, a
// 32-byte header before each block (+0 the next free block, +4 the size in
// units, header included), searched first fit from a rover, and a block cut
// from the top of the free block that fits. The guest's own words:
//   0x8C1BCAC0  the operations table (the default: 0x8C0F9E48)
//   0x8C1BCAC4  the list's base, a header of size 0
//   0x8C1BCAE4  the rover
//   0x8C1BCAE8  the heap's start (also at 0x8C1BCAF0), 0x8C1BCAEC its size
#pragma once

#include <cstdint>

namespace toyracer::heap {

inline constexpr uint32_t kTablePointer = 0x8C1BCAC0u;
inline constexpr uint32_t kDefaultTable = 0x8C0F9E48u;

// 0x8C0C5AFC: r5 bytes from r4, both rounded to 32 bytes.
void init(uint32_t begin, uint32_t size);
// 0x8C0C5BDE: a block of at least `bytes`, or 0.
uint32_t alloc(uint32_t bytes);
// 0x8C0C5CAC: gives a block back (0 is ignored, as the guest ignores it).
void release(uint32_t block);
// The free list's total in bytes, headers excluded (what 0x8C0C5B5A reports).
uint32_t free_bytes();

}  // namespace toyracer::heap
