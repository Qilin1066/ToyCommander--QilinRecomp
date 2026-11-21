// The guest heap Shinobi's allocator managed: blocks of guest RAM handed to
// the game and the middleware, 32-byte aligned as Shinobi aligns them. The
// bookkeeping is the host's (the guest's allocator kept it in the heap
// itself; nothing outside the allocator reads it), so only the addresses
// handed out are guest-visible.
#pragma once

#include <cstdint>
#include <map>

namespace toycommander {

class Heap {
public:
    // The heap: [begin, begin + size), both rounded inward to 32 bytes.
    void init(uint32_t begin, uint32_t size);
    // A block of at least `size` bytes, or 0 if there is no room. First fit, lowest address.
    uint32_t alloc(uint32_t size);
    // Gives a block back; false if it isn't one alloc handed out.
    bool free(uint32_t block);
    // The size alloc gave a block (rounded up to 32), or 0 if it isn't one.
    uint32_t size_of(uint32_t block) const;
    uint32_t free_bytes() const;
    bool ready() const { return end_ != 0; }

private:
    uint32_t begin_ = 0, end_ = 0;
    std::map<uint32_t, uint32_t> free_;  // start -> size, coalesced
    std::map<uint32_t, uint32_t> used_;  // start -> size
};

}  // namespace toycommander
