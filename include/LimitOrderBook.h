#pragma once

//===----------------------------------------------------------------------===//
// LimitOrderBook.h
//
// Price-time priority matching engine.
//
// PHASE 5 CHANGE: the time-priority FIFO at each price level is no longer a
// std::list. It is an intrusive doubly linked list threaded through slots of
// a MemoryPool<OrderNode> -- "intrusive" meaning the prev/next links live
// INSIDE the node itself rather than in separately allocated list machinery.
// This removes the last per-order heap allocation from the hot path: adding
// an order is now a pool slot grab (no malloc) plus two pointer -- er,
// index -- writes.
//
// Container contract:
//   Price levels ....... std::map               (Red-Black tree, ordered)
//   Time priority ...... intrusive list in pool  (FIFO, zero allocation)
//   Order storage ....... MemoryPool<OrderNode>  (contiguous slab)
//   Cancel directory ... std::unordered_map      (OrderId -> location record)
//
// Complexity summary (M = distinct price levels, N = resting orders):
//   best bid / best ask ............ O(1)          map::begin()
//   locate / create price level .... O(log M)      RB-tree descent
//   append at level (time prio) .... O(1)          pool.allocate + link
//   cancel by OrderId .............. O(1) avg      hash lookup + unlink
//   erase an emptied price level ... amortized O(1) we hold the map iterator
//   match one aggressive order ..... O(log M + K)  K = orders consumed
//
// Iterator/index stability is the load-bearing invariant: std::map keeps
// iterators valid across insertion and unrelated erasure, and a MemoryPool
// slot's index stays valid for the slot's entire lifetime -- until THAT
// slot is explicitly deallocated. That is the only reason the O(1)
// cancellation directory is sound.
//===----------------------------------------------------------------------===//

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "MemoryPool.h"

namespace lob {

//===----------------------------------------------------------------------===//
// Primitive types
//===----------------------------------------------------------------------===//

// Prices are integral ticks internally. double appears ONLY at the public API
// boundary, where it is converted once; it never reaches the comparison path.
using Price     = std::int64_t;
using Quantity  = std::uint64_t;   // 64-bit: level aggregates sum many orders
using OrderId   = std::uint64_t;
using Sequence  = std::uint64_t;   // monotonic arrival counter = time priority

enum class Side : std::uint8_t {
    BUY  = 0,
    SELL = 1
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::BUY ? Side::SELL : Side::BUY;
}

//===----------------------------------------------------------------------===//
// Order
//
// Trivially copyable and tightly packed: these are the payload of every pool
// slot, and every byte of padding is a byte of cache line not spent on the
// next order in the FIFO queue. Widest-first layout avoids interior padding.
//===----------------------------------------------------------------------===//

struct Order {
    OrderId  id       {0};
    Price    price    {0};   // ticks
    Quantity quantity {0};   // remaining (unfilled) quantity
    Sequence sequence {0};   // arrival order; ties are impossible by construction
    Side     side     {Side::BUY};
    // 7 bytes trailing padding reserved for order-type flags (IOC/FOK).
};

// A single fill. Accumulated internally rather than returned by value so the
// hot path performs no per-call heap allocation.
struct Trade {
    OrderId  aggressorId  {0};
    OrderId  restingId    {0};
    Price    price        {0};   // always the RESTING order's price
    Quantity quantity     {0};
    Side     aggressorSide{Side::BUY};
};

//===----------------------------------------------------------------------===//
// OrderNode
//
// The intrusive list node. Lives inside a MemoryPool slot -- there is no
// separate list allocation. next/prev are POOL INDICES, not pointers: 4
// bytes instead of 8, and indices survive relocation / serialise trivially,
// which raw pointers into a std::list never could.
//===----------------------------------------------------------------------===//

struct OrderNode {
    Order     order;
    PoolIndex next {NULL_INDEX};
    PoolIndex prev {NULL_INDEX};
};

//===----------------------------------------------------------------------===//
// PriceLevel
//
// No std::list member anymore. head/tail are pool indices bounding the
// intrusive FIFO; NULL_INDEX in both means the level is empty.
//===----------------------------------------------------------------------===//

struct PriceLevel {
    PoolIndex head {NULL_INDEX};       // oldest -- first to fill
    PoolIndex tail {NULL_INDEX};       // newest -- append here
    Quantity  aggregateQuantity{0};    // maintained incrementally -> O(1) depth

    [[nodiscard]] bool empty() const noexcept { return head == NULL_INDEX; }
};

//===----------------------------------------------------------------------===//
// Book maps
//
// Bids use std::greater -> begin() is the HIGHEST bid.
// Asks use std::less    -> begin() is the LOWEST  ask.
// Top-of-book is therefore begin() in O(1) on both sides, and the matching
// loop is a single forward iteration regardless of side.
//===----------------------------------------------------------------------===//

using BidBook = std::map<Price, PriceLevel, std::greater<Price>>;
using AskBook = std::map<Price, PriceLevel, std::less<Price>>;

//===----------------------------------------------------------------------===//
// OrderLocation / LevelHandle
//
// Value type of the cancellation directory, and the reason cancel is O(1).
//
// A pool index alone is not enough. After erasing the last order at a price
// we must also erase the now-empty std::map node -- and if all we had cached
// was the PRICE, finding that node would cost an O(log M) re-descent, which
// would make the whole operation O(log M) and defeat the directory. So we
// cache the MAP ITERATOR itself; std::map guarantees it stays valid for the
// lifetime of that node.
//
// Bids and asks are different map types (different comparators), so the two
// iterator types are unrelated. `side` is the discriminator; the union
// stores whichever one applies with zero space overhead and zero indirection.
//===----------------------------------------------------------------------===//

union LevelHandle {
    BidBook::iterator bid;
    AskBook::iterator ask;

    LevelHandle() noexcept : bid() {}

    // Deliberately factories, not overloaded constructors. On libstdc++ and
    // libc++ the comparator is NOT part of the iterator's type, so
    // BidBook::iterator and AskBook::iterator are the SAME type and a pair of
    // overloaded ctors is ill-formed (redeclaration). Named factories compile
    // whether the two types coincide or not.
    [[nodiscard]] static LevelHandle fromBid(BidBook::iterator i) noexcept {
        LevelHandle h; h.bid = i; return h;
    }
    [[nodiscard]] static LevelHandle fromAsk(AskBook::iterator i) noexcept {
        LevelHandle h; h.ask = i; return h;
    }
};

static_assert(std::is_trivially_copyable_v<BidBook::iterator>,
              "Cached map iterator must be a trivial value type");
static_assert(std::is_trivially_destructible_v<BidBook::iterator>,
              "Union member must not require a destructor call");
static_assert(std::is_trivially_copyable_v<AskBook::iterator>,
              "Cached map iterator must be a trivial value type");
static_assert(std::is_trivially_destructible_v<AskBook::iterator>,
              "Union member must not require a destructor call");

struct OrderLocation {
    PoolIndex   node;              // slot holding this order's OrderNode
    LevelHandle level;             // the owning std::map node
    Side        side {Side::BUY};  // discriminates the union
};

using OrderDirectory = std::unordered_map<OrderId, OrderLocation>;

//===----------------------------------------------------------------------===//
// LimitOrderBook
//===----------------------------------------------------------------------===//

class LimitOrderBook {
public:
    static constexpr double TICK_SIZE   = 0.01;
    static constexpr double TICKS_PER_1 = 1.0 / TICK_SIZE;

    // `capacity` bounds two things at once: the pool's slab size (max
    // CONCURRENTLY resting orders -- freed slots are reused, so this is not
    // a lifetime total) and the directory's pre-reserved bucket count.
    explicit LimitOrderBook(std::size_t capacity = 1u << 16)
        : pool_(capacity)
    {
        directory_.reserve(capacity);
        directory_.max_load_factor(0.5f);   // trade memory for shorter probes
        tradeLog_.reserve(1u << 12);
    }

    // Non-copyable: the directory and the intrusive list both hold indices
    // into pool_, and bids_/asks_ hold iterators into themselves. A shallow
    // copy would alias all of that into the clone.
    LimitOrderBook(const LimitOrderBook&)            = delete;
    LimitOrderBook& operator=(const LimitOrderBook&) = delete;
    LimitOrderBook(LimitOrderBook&&)                 = default;
    LimitOrderBook& operator=(LimitOrderBook&&)      = default;

    //--- Tick conversion (API boundary only) ------------------------------//

    [[nodiscard]] static Price toTicks(double price) noexcept {
        return static_cast<Price>(std::llround(price * TICKS_PER_1));
    }

    [[nodiscard]] static double toDouble(Price ticks) noexcept {
        return static_cast<double>(ticks) * TICK_SIZE;
    }

    //--- Mutating operations ----------------------------------------------//

    // Aggress against the opposite book, then rest any residual quantity.
    // O(log M + K), K = resting orders fully or partially consumed.
    // If the pool is exhausted when resting a residual, the fill already
    // applied stands and the residual is DROPPED rather than partially
    // recorded -- see addOrder's implementation note for the rationale.
    void addOrder(OrderId orderId, Side side, double price, std::uint32_t quantity);

    // O(1) average: hash probe, intrusive unlink, then level teardown if
    // emptied.
    void cancelOrder(OrderId orderId);

    // Quantity DECREASE in place retains time priority: O(1) average.
    bool reduceQuantity(OrderId orderId, std::uint32_t newQuantity);

    //--- Read-only queries -------------------------------------------------//

    [[nodiscard]] std::optional<Price> bestBid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    [[nodiscard]] std::optional<Price> bestAsk() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    [[nodiscard]] std::optional<Price> spread() const noexcept {
        if (bids_.empty() || asks_.empty()) return std::nullopt;
        return asks_.begin()->first - bids_.begin()->first;
    }

    [[nodiscard]] Quantity quantityAt(Side side, double price) const noexcept;

    [[nodiscard]] bool contains(OrderId id) const noexcept {
        return directory_.find(id) != directory_.end();
    }

    [[nodiscard]] std::size_t orderCount() const noexcept { return directory_.size(); }
    [[nodiscard]] std::size_t bidLevels()  const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t askLevels()  const noexcept { return asks_.size(); }

    // Remaining free slots in the order pool -- the hard capacity ceiling.
    [[nodiscard]] std::size_t poolAvailable() const noexcept { return pool_.available(); }
    [[nodiscard]] std::size_t poolCapacity()  const noexcept { return pool_.capacity(); }

    [[nodiscard]] const std::vector<Trade>& lastTrades() const noexcept { return tradeLog_; }

private:
    void match(Order& incomingOrder);

    template <typename BookT>
    void matchAgainst(BookT& book, Order& aggressor);

    // Returns false (and rests nothing) if the pool is exhausted.
    bool restOrder(const Order& order);

    bool doCancel(OrderId orderId);

    // Unlinks the node from its intrusive list and deallocates its slot;
    // tears down the price level if that emptied it. Takes iterators/indices
    // only -- no container performs a search.
    template <typename BookT, typename LevelIt>
    void unlink(BookT& book, LevelIt levelIt, PoolIndex node);

    BidBook               bids_;
    AskBook               asks_;
    OrderDirectory        directory_;
    MemoryPool<OrderNode> pool_;       // backs every resting order, zero malloc
    std::vector<Trade>    tradeLog_;
    Sequence              sequence_{0};
};

}  // namespace lob