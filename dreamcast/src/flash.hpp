// The console's flash memory, as the boot ROM's flash calls give it to
// Shinobi (its syFlash functions, lifted over these): 128 KB in five
// partitions, kept as a file in the data folder (flash.bin), erased (all
// ones) until something writes it. Writing programs bits (a 1 can become 0,
// not back), as flash memory does; deleting a partition erases it.
// Nothing here is the console's own data: a game that finds a partition
// unformatted formats it itself, with its own code.
#pragma once

#include <cstdint>

namespace dreamcast::flash {

// Call 0: partition `partition`'s place and size, or false for one there isn't.
bool info(uint32_t partition, uint32_t& offset, uint32_t& size);
// Call 1: `n` bytes from `offset` to guest address `to`: `n`, or -1 out of range.
int32_t read(uint32_t offset, uint32_t to, uint32_t n);
// Call 2: `n` bytes from guest address `from` programmed at `offset`: `n`, or -1.
int32_t write(uint32_t offset, uint32_t from, uint32_t n);
// Call 3: the partition that starts at `offset` erased: 0, or -1 for no partition there.
int32_t erase(uint32_t offset);

}  // namespace dreamcast::flash
