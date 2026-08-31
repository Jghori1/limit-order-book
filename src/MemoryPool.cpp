//===----------------------------------------------------------------------===//
// MemoryPool.cpp
//
// MemoryPool<T> is a class template, so its definitions must be visible to
// every translation unit that instantiates it -- they live in the header by
// necessity, not by preference. This file holds only the parts that do NOT
// depend on T:
//
//   1. Compile-time validation of the pool's layout assumptions against the
//      concrete payload type the engine actually uses (OrderNode, defined in
//      LimitOrderBook.h since Phase 5 -- it is engine-owned, the pool is
//      generic storage underneath it).
//   2. Explicit instantiation, which forces the compiler to emit the full
//      class here, catching template errors in one place.
//===----------------------------------------------------------------------===//

#include "MemoryPool.h"
#include "LimitOrderBook.h"

namespace lob {

//===----------------------------------------------------------------------===//
// Layout assertions
//
// These are the memory-locality claims the design rests on. Asserting them
// means a careless field addition to Order or OrderNode fails the BUILD
// instead of silently degrading cache behaviour at runtime.
//===----------------------------------------------------------------------===//

static_assert(sizeof(Order) <= 40,
              "Order grew; every byte costs cache-line density in the FIFO");

static_assert(sizeof(OrderNode) <= 48,
              "OrderNode must stay at or below 48 bytes. An equivalent "
              "std::list node would cost sizeof(Order) + two 8-byte "
              "pointers = 56; pooled indices are half that per link.");

static_assert(alignof(OrderNode) <= 8,
              "Over-alignment would pad the slab and waste cache lines");

static_assert(std::is_trivially_copyable_v<OrderNode>,
              "Nodes must be memcpy-able for snapshotting and replay");

static_assert(std::is_trivially_destructible_v<OrderNode>,
              "Pool teardown skips destructors by design");

//===----------------------------------------------------------------------===//
// Explicit instantiation
//
// OrderNode is the type actually used by LimitOrderBook. Order itself is
// instantiated too since it is a valid, smaller payload some callers (or
// tests) may want to pool directly without the intrusive-list overhead.
//===----------------------------------------------------------------------===//

template class MemoryPool<Order>;
template class MemoryPool<OrderNode>;

}  // namespace lob