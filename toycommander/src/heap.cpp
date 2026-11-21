#include "heap.hpp"

#include "guest.hpp"

namespace toycommander::heap {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kBase = 0x8C117DC0u, kRover = 0x8C117DE0u;
constexpr uint32_t kStart = 0x8C117DE4u, kSize = 0x8C117DE8u, kStartCopy = 0x8C117DECu;

uint32_t next(uint32_t h) { return rt::ld32(h); }
uint32_t units(uint32_t h) { return rt::ld32(h + 4); }
void set_next(uint32_t h, uint32_t v) { rt::st32(h, v); }
void set_units(uint32_t h, uint32_t v) { rt::st32(h + 4, v); }

}  // namespace

void init(uint32_t begin, uint32_t size) {
    begin = (begin + 31) & ~31u, size &= ~31u;
    rt::st32(kStartCopy, begin), rt::st32(kStart, begin), rt::st32(kSize, size);
    rt::st32(kRover, kBase), set_next(kBase, kBase), set_units(kBase, 0);
    rt::st32(kTablePointer, kDefaultTable);
    set_units(begin, size >> 5);
    release(begin + 32);
}

uint32_t alloc(uint32_t bytes) {
    const uint32_t want = ((bytes + 31) >> 5) + 1;
    const uint32_t rover = rt::ld32(kRover);
    for (uint32_t prev = rover, p = next(rover);; prev = p, p = next(p)) {
        if (units(p) >= want) {
            if (units(p) == want) {
                set_next(prev, next(p));
            } else {
                set_units(p, units(p) - want);
                p += units(p) * 32;
                set_units(p, want);
            }
            rt::st32(kRover, prev);
            return p + 32;
        }
        if (p == rover) return 0;
    }
}

void release(uint32_t block) {
    if (block == 0) return;
    const uint32_t b = block - 32;
    uint32_t p = rt::ld32(kRover);
    while (!(b > p && b < next(p))) {
        if (p >= next(p) && (b > p || b < next(p))) break;
        p = next(p);
    }
    if (b + units(b) * 32 == next(p)) {
        set_units(b, units(b) + units(next(p)));
        set_next(b, next(next(p)));
    } else {
        set_next(b, next(p));
    }
    if (p + units(p) * 32 == b) {
        set_units(p, units(p) + units(b));
        set_next(p, next(b));
    } else {
        set_next(p, b);
    }
    rt::st32(kRover, p);
}

uint32_t free_bytes() {
    uint32_t n = 0;
    const uint32_t base = rt::ld32(kRover);
    uint32_t p = base;
    do {
        if (units(p) > 0) n += (units(p) - 1) * 32;
        p = next(p);
    } while (p != base);
    return n;
}

}  // namespace toycommander::heap
