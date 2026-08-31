//===----------------------------------------------------------------------===//
// LimitOrderBook.cpp
//
// Phase 5: engine internals migrated from std::list to an intrusive doubly
// linked list threaded through MemoryPool<OrderNode> slots. Every order that
// rests on the book now lives in a pool slot; there is no std::list, and
// after construction there is no call to `new` anywhere on the hot path.
//
// Invariants (unchanged in spirit from Phase 2, restated for the pool):
//   (I1) directory_ contains an entry for EXACTLY the set of orders currently
//        resting in bids_/asks_.
//   (I2) PriceLevel::aggregateQuantity == sum of its orders' quantities.
//   (I3) No price level with an empty intrusive list (head == NULL_INDEX) is
//        ever left in a std::map.
//   (I4) NEW: every live OrderNode is reachable from exactly one PriceLevel's
//        head/tail chain, and its pool slot is never touched after
//        deallocate() until the slot is reused by a future allocate().
//===----------------------------------------------------------------------===//

#include "LimitOrderBook.h"

#include <algorithm>
#include <utility>

namespace lob {

//===----------------------------------------------------------------------===//
// addOrder
//===----------------------------------------------------------------------===//

void LimitOrderBook::addOrder(OrderId  orderId,
                              Side     side,
                              double   price,
                              std::uint32_t quantity)
{
    tradeLog_.clear();
    if (quantity == 0) return;

    Order incoming;
    incoming.id       = orderId;
    incoming.price    = toTicks(price);
    incoming.quantity = quantity;
    incoming.sequence = ++sequence_;
    incoming.side     = side;

    // 1. Aggress against the opposite book. Mutates incoming.quantity down.
    match(incoming);

    // 2. Rest the residual, if any.
    //
    // IMPLEMENTATION NOTE -- pool exhaustion policy:
    // restOrder() can fail if the pool has no free slots. When that happens
    // the fills already applied in match() stand (they are real trades that
    // already happened and cannot be undone), but the unfilled residual is
    // silently dropped rather than rested. This mirrors how a real gateway
    // would behave -- reject the resting leg, never leave the engine in a
    // state where an order is neither filled nor tracked. A production
    // system would instead surface this as an explicit reject status to the
    // caller; addOrder's void return does not currently carry one.
    if (incoming.quantity > 0) {
        (void) restOrder(incoming);
    }
}

//===----------------------------------------------------------------------===//
// match / matchAgainst
//
// Unchanged in structure from Phase 2 -- only the queue traversal underneath
// changed, from std::list iteration to walking pool indices via node.next.
//===----------------------------------------------------------------------===//

void LimitOrderBook::match(Order& incomingOrder)
{
    if (incomingOrder.side == Side::BUY) {
        matchAgainst(asks_, incomingOrder);
    } else {
        matchAgainst(bids_, incomingOrder);
    }
}

template <typename BookT>
void LimitOrderBook::matchAgainst(BookT& book, Order& aggressor)
{
    const auto& crosses = book.key_comp();
    auto levelIt = book.begin();

    while (aggressor.quantity > 0 &&
           levelIt != book.end() &&
           !crosses(aggressor.price, levelIt->first))
    {
        PriceLevel& level = levelIt->second;
        PoolIndex   cur   = level.head;   // O(1): FIFO head is the oldest order

        while (aggressor.quantity > 0 && cur != NULL_INDEX) {
            OrderNode& node    = pool_[cur];   // O(1): index -> slot, no chase
            Order&     resting = node.order;

            const Quantity fill = std::min(aggressor.quantity, resting.quantity);

            aggressor.quantity      -= fill;
            resting.quantity        -= fill;
            level.aggregateQuantity -= fill;          // (I2)

            // Trade prints at the RESTING order's price -- the passive side
            // set the price and earns the spread.
            tradeLog_.push_back(Trade{aggressor.id,
                                      resting.id,
                                      levelIt->first,
                                      fill,
                                      aggressor.side});

            if (resting.quantity == 0) {
                // Fully filled. Capture the successor BEFORE the slot is
                // freed -- once deallocate() runs, this slot may be handed
                // back out by the very next allocate() anywhere in the
                // engine, so node.next must not be read afterward.   (I4)
                const PoolIndex next = node.next;

                directory_.erase(resting.id);          // O(1) avg   (I1)

                // Unlink from the intrusive list. Same shape as removing a
                // std::list node, just addressed by index instead of pointer.
                if (node.prev != NULL_INDEX) pool_[node.prev].next = node.next;
                else                         level.head = node.next;
                if (node.next != NULL_INDEX) pool_[node.next].prev = node.prev;
                else                         level.tail = node.prev;

                pool_.deallocate(cur);                 // O(1) -- slot reusable now

                cur = next;
            }
            // Partial fill: aggressor.quantity is now guaranteed 0 (fill was
            // the minimum of the two), so the outer while condition ends the
            // loop on the next check. The resting node keeps its position
            // and its time priority untouched.
        }

        // Memory cleanup: never leave an empty level in the tree.   (I3)
        if (level.head == NULL_INDEX) {
            levelIt = book.erase(levelIt);   // amortised O(1), no re-descent
        } else {
            ++levelIt;
        }
    }
}

//===----------------------------------------------------------------------===//
// restOrder
//
// Complexity: O(log M) to find-or-create the level, O(1) to allocate a slot
// and link it in, O(1) average to index. Returns false without mutating the
// book if the pool has no free slot.
//===----------------------------------------------------------------------===//

bool LimitOrderBook::restOrder(const Order& order)
{
    bool ok = false;

    auto insertLevel = [&](auto& book) {
        // C++20 parenthesized aggregate initialization: OrderNode has no
        // user-declared constructor, so allocate()'s placement-new T(args...)
        // aggregate-initializes {order, next, prev} directly in the slot --
        // no temporary, no extra copy.
        const PoolIndex node = pool_.allocate(order, NULL_INDEX, NULL_INDEX);
        if (node == NULL_INDEX) {
            return;   // pool exhausted; ok stays false, nothing was mutated
        }

        auto [levelIt, /*inserted*/ _] = book.try_emplace(order.price);
        PriceLevel& level = levelIt->second;

        // Append to the tail: O(1), no traversal.
        pool_[node].prev = level.tail;
        pool_[node].next = NULL_INDEX;
        if (level.tail != NULL_INDEX) pool_[level.tail].next = node;
        else                          level.head = node;
        level.tail = node;
        level.aggregateQuantity += order.quantity;    // (I2)

        LevelHandle handle;
        if constexpr (std::is_same_v<std::decay_t<decltype(book)>, BidBook>) {
            handle = LevelHandle::fromBid(levelIt);
        } else {
            handle = LevelHandle::fromAsk(levelIt);
        }

        directory_.emplace(order.id, OrderLocation{node, handle, order.side}); // (I1)
        ok = true;
    };

    if (order.side == Side::BUY) insertLevel(bids_);
    else                         insertLevel(asks_);

    return ok;
}

//===----------------------------------------------------------------------===//
// unlink
//
// Removes one node from its intrusive list and, if that emptied the price
// level, removes the level from the tree. Every step takes an index/iterator
// -- no container performs a search.
//===----------------------------------------------------------------------===//

template <typename BookT, typename LevelIt>
void LimitOrderBook::unlink(BookT& book, LevelIt levelIt, PoolIndex node)
{
    PriceLevel& level = levelIt->second;
    OrderNode&  n      = pool_[node];

    level.aggregateQuantity -= n.order.quantity;      // (I2)

    if (n.prev != NULL_INDEX) pool_[n.prev].next = n.next;
    else                      level.head = n.next;
    if (n.next != NULL_INDEX) pool_[n.next].prev = n.prev;
    else                      level.tail = n.prev;

    pool_.deallocate(node);                            // O(1)

    if (level.head == NULL_INDEX) {
        book.erase(levelIt);                            // (I3)
    }
}

//===----------------------------------------------------------------------===//
// cancelOrder / doCancel
//
// O(1) average, end to end -- identical shape to Phase 3, now unlinking a
// pool node instead of a std::list node.
//===----------------------------------------------------------------------===//

void LimitOrderBook::cancelOrder(OrderId orderId)
{
    (void) doCancel(orderId);
}

bool LimitOrderBook::doCancel(OrderId orderId)
{
    auto dirIt = directory_.find(orderId);              // O(1) avg
    if (dirIt == directory_.end()) return false;

    const OrderLocation loc = dirIt->second;             // copy before erase

    if (loc.side == Side::BUY) {
        unlink(bids_, loc.level.bid, loc.node);
    } else {
        unlink(asks_, loc.level.ask, loc.node);
    }

    directory_.erase(dirIt);                             // O(1) avg   (I1)
    return true;
}

//===----------------------------------------------------------------------===//
// reduceQuantity
//===----------------------------------------------------------------------===//

bool LimitOrderBook::reduceQuantity(OrderId orderId, std::uint32_t newQuantity)
{
    auto dirIt = directory_.find(orderId);
    if (dirIt == directory_.end()) return false;

    const OrderLocation& loc  = dirIt->second;
    Order&                ord = pool_[loc.node].order;   // O(1): direct index

    if (newQuantity >= ord.quantity) return false;   // increase => cancel-replace
    if (newQuantity == 0)            return doCancel(orderId);

    const Quantity delta = ord.quantity - newQuantity;
    ord.quantity = newQuantity;

    if (loc.side == Side::BUY) loc.level.bid->second.aggregateQuantity -= delta;
    else                       loc.level.ask->second.aggregateQuantity -= delta;

    return true;                                                        // (I2)
}

//===----------------------------------------------------------------------===//
// quantityAt
//===----------------------------------------------------------------------===//

Quantity LimitOrderBook::quantityAt(Side side, double price) const noexcept
{
    const Price ticks = toTicks(price);

    if (side == Side::BUY) {
        auto it = bids_.find(ticks);
        return it == bids_.end() ? 0 : it->second.aggregateQuantity;
    }
    auto it = asks_.find(ticks);
    return it == asks_.end() ? 0 : it->second.aggregateQuantity;
}

//===----------------------------------------------------------------------===//
// Explicit instantiation of the templated internals for both book types.
//===----------------------------------------------------------------------===//

template void LimitOrderBook::matchAgainst<BidBook>(BidBook&, Order&);
template void LimitOrderBook::matchAgainst<AskBook>(AskBook&, Order&);
template void LimitOrderBook::unlink<BidBook, BidBook::iterator>(BidBook&, BidBook::iterator, PoolIndex);
template void LimitOrderBook::unlink<AskBook, AskBook::iterator>(AskBook&, AskBook::iterator, PoolIndex);

}  // namespace lob