#ifndef BLAZERULES_ARENA_H
#define BLAZERULES_ARENA_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

class BatchArena {
public:
    explicit BatchArena(size_t capacity)
        : block_(static_cast<uint8_t*>(::operator new(capacity, std::align_val_t(64)))),
          capacity_(capacity), bump_pos_(0), alloc_count_(0) {}

    ~BatchArena() { if (block_) ::operator delete(block_, std::align_val_t(64)); }

    BatchArena(const BatchArena&) = delete;
    BatchArena& operator=(const BatchArena&) = delete;

    BatchArena(BatchArena&& o) noexcept
        : block_(o.block_), capacity_(o.capacity_), bump_pos_(o.bump_pos_), alloc_count_(o.alloc_count_) {
        o.block_ = nullptr; o.capacity_ = 0; o.bump_pos_ = 0; o.alloc_count_ = 0;
    }

    void* allocate(size_t size) {
        size_t aligned = (bump_pos_ + 63) & ~static_cast<size_t>(63);
        if (aligned + size > capacity_) return nullptr;
        bump_pos_ = aligned + size;
        ++alloc_count_;
        return block_ + aligned;
    }

    uint8_t* allocate_bitmask(int n_records) {
        size_t bytes = static_cast<size_t>((n_records + 7) / 8);
        auto* p = static_cast<uint8_t*>(allocate(bytes));
        if (p) std::memset(p, 0, bytes);
        return p;
    }

    void reset() { bump_pos_ = 0; alloc_count_ = 0; }

    size_t capacity() const { return capacity_; }
    size_t used_bytes() const { return bump_pos_; }
    size_t allocations_since_reset() const { return alloc_count_; }

private:
    uint8_t* block_;
    size_t capacity_;
    size_t bump_pos_;
    size_t alloc_count_;
};

#endif //BLAZERULES_ARENA_H
