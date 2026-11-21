#include "heap.hpp"

#include <iterator>

namespace toycommander {

void Heap::init(uint32_t begin, uint32_t size) {
    begin_ = (begin + 31) & ~31u;
    end_ = (begin + size) & ~31u;
    free_.clear(), used_.clear();
    if (end_ > begin_) free_[begin_] = end_ - begin_;
}

uint32_t Heap::alloc(uint32_t size) {
    const uint64_t want = (uint64_t(size ? size : 1) + 31) & ~uint64_t(31);
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < want) continue;
        const uint32_t block = it->first, rest = it->second - static_cast<uint32_t>(want);
        free_.erase(it);
        if (rest) free_[block + static_cast<uint32_t>(want)] = rest;
        used_[block] = static_cast<uint32_t>(want);
        return block;
    }
    return 0;
}

bool Heap::free(uint32_t block) {
    const auto u = used_.find(block);
    if (u == used_.end()) return false;
    uint32_t start = block, size = u->second;
    used_.erase(u);
    auto next = free_.lower_bound(start);
    if (next != free_.end() && start + size == next->first) size += next->second, next = free_.erase(next);
    if (next != free_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) start = prev->first, size += prev->second, free_.erase(prev);
    }
    free_[start] = size;
    return true;
}

uint32_t Heap::size_of(uint32_t block) const {
    const auto u = used_.find(block);
    return u == used_.end() ? 0 : u->second;
}

uint32_t Heap::free_bytes() const {
    uint32_t n = 0;
    for (const auto& [start, size] : free_) n += size;
    return n;
}

}  // namespace toycommander
