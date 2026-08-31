#pragma once

//===----------------------------------------------------------------------===//
// MemoryPool.h
//
// Fixed-capacity, index-addressed object pool for the matching engine's hot
// path. Zero dynamic allocation after construction, O(1) allocate and free.
//
// Design in one line: one contiguous slab of slots, plus a free list threaded
// through the bytes of the slots that are currently free.
//
// The free list costs nothing extra. A free slot's payload bytes are dead
// anyway, so we reuse the first four of them to store the index of the next
// free slot. No side table, no bitmap, no per-slot overhead.
//
//   slots_:  [ Order ][ Order ][ free->7 ][ Order ][ free->2 ] ...
//                                  |                    |
//              freeHead_ = 4 ------+--------------------+--> 7 --> NULL
//
// Complexity:
//   allocate()   O(1) -- pop the free-list head, or bump the high-water mark
//   deallocate() O(1) -- push onto the free-list head
//   operator[]   O(1) -- base pointer + (index * sizeof(Slot))
//===----------------------------------------------------------------------===//

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace lob {

using PoolIndex = std::uint32_t;

// Sentinel for "no slot". Chosen as the max uint32 so it is never a valid
// index, which caps capacity at 2^32-2 -- roughly 4.29 billion orders, some
// orders of magnitude beyond any real book.
inline constexpr PoolIndex NULL_INDEX = 0xFFFFFFFFu;

template <typename T>
class MemoryPool {
public:
    // A slot is raw storage, not a T. We do not want the pool to
    // default-construct a million Orders at startup; slots hold either a live
    // T or a free-list link, and the pool tracks which.
    struct alignas(alignof(T) > alignof(PoolIndex) ? alignof(T) : alignof(PoolIndex)) Slot {
        std::byte storage[sizeof(T) > sizeof(PoolIndex) ? sizeof(T) : sizeof(PoolIndex)];
    };

    static_assert(sizeof(T) >= sizeof(PoolIndex),
                  "Payload must be large enough to overlay a free-list link");
    static_assert(std::is_trivially_destructible_v<T>,
                  "Pool skips destructor calls on teardown; keep payloads trivial");

    explicit MemoryPool(std::size_t capacity)
        : slots_(capacity)
    {
        assert(capacity > 0 && capacity < NULL_INDEX);
        // The vector's constructor already touched every page, so the slab is
        // resident before the first order arrives. Page faults are a latency
        // spike we pay at startup, deliberately, instead of mid-session.
    }

    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&)                 = default;
    MemoryPool& operator=(MemoryPool&&)      = default;

    //--- Allocation --------------------------------------------------------//

    // O(1). Returns NULL_INDEX when exhausted -- no exception, no allocation,
    // no growth. A full pool is a capacity planning error, and the caller
    // (risk/gateway layer) must reject the order rather than stall the engine.
    template <typename... Args>
    [[nodiscard]] PoolIndex allocate(Args&&... args) noexcept
    {
        PoolIndex idx;

        if (freeHead_ != NULL_INDEX) {
            // Reuse. LIFO on purpose: the most recently freed slot is the one
            // most likely to still be resident in L1/L2, so churn stays warm.
            idx       = freeHead_;
            freeHead_ = readLink(idx);
        } else if (highWater_ < slots_.size()) {
            // Virgin slot. The bump pointer means construction does not have
            // to walk a million slots wiring up an initial free list.
            idx = static_cast<PoolIndex>(highWater_++);
        } else {
            return NULL_INDEX;                       // exhausted
        }

        // Placement new: construct in memory we already own. No malloc, no
        // lock, no syscall -- the entire point of the exercise.
        ::new (static_cast<void*>(&slots_[idx])) T(std::forward<Args>(args)...);
        ++live_;
        return idx;
    }

    // O(1). Push onto the free-list head.
    void deallocate(PoolIndex idx) noexcept
    {
        assert(idx < highWater_ && "index outside the allocated region");
        assert(live_ > 0 && "deallocate on an empty pool");

        if constexpr (!std::is_trivially_destructible_v<T>) {
            operator[](idx).~T();
        }

        writeLink(idx, freeHead_);
        freeHead_ = idx;
        --live_;
    }

    //--- Access ------------------------------------------------------------//

    // O(1) and branchless: base + idx * sizeof(Slot). This is the operation
    // that replaces pointer chasing, and it is why indices beat pointers here.
    [[nodiscard]] T& operator[](PoolIndex idx) noexcept {
        assert(idx < highWater_);
        return *std::launder(reinterpret_cast<T*>(&slots_[idx]));
    }

    [[nodiscard]] const T& operator[](PoolIndex idx) const noexcept {
        assert(idx < highWater_);
        return *std::launder(reinterpret_cast<const T*>(&slots_[idx]));
    }

    //--- Introspection -----------------------------------------------------//

    [[nodiscard]] std::size_t capacity()  const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t live()      const noexcept { return live_; }
    [[nodiscard]] std::size_t available() const noexcept { return slots_.size() - live_; }
    [[nodiscard]] bool        empty()     const noexcept { return live_ == 0; }

    // Bytes of contiguous slab. Useful for sizing against L3 and for deciding
    // whether the slab warrants huge pages.
    [[nodiscard]] std::size_t bytes() const noexcept { return slots_.size() * sizeof(Slot); }

private:
    // Free-list links live inside the free slots themselves. memcpy rather
    // than a reinterpret_cast store: the slot's stored type changes between
    // T and PoolIndex, and memcpy is the well-defined way to say that.
    [[nodiscard]] PoolIndex readLink(PoolIndex idx) const noexcept {
        PoolIndex next;
        __builtin_memcpy(&next, &slots_[idx], sizeof(PoolIndex));
        return next;
    }

    void writeLink(PoolIndex idx, PoolIndex next) noexcept {
        __builtin_memcpy(&slots_[idx], &next, sizeof(PoolIndex));
    }

    std::vector<Slot> slots_;                 // the slab; never resized
    PoolIndex         freeHead_ {NULL_INDEX}; // head of the free list
    std::size_t       highWater_{0};          // slots never yet handed out
    std::size_t       live_     {0};          // currently allocated
};

}  // namespace lob
