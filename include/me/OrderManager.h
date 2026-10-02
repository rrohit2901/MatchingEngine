#pragma once

#include<cstddef>
#include<cstdint>
#include<optional>
#include<vector>

#include "Order.h"

// Owns every Order in a pre-allocated slab: one contiguous vector of slots,
// recycled through an intrusive free list. An order id encodes where its order
// lives, so a lookup is a bounds check and an index — no hashing, no probing, and
// no allocation on add or erase.
//
// Id layout (order_id_t is 64 bits):
//   [ 63 .......... 32 | 31 ........... 0 ]
//   [    generation    |    slot index    ]
//
// Every slot carries a generation counter, bumped on each allocate AND each
// release, so it is odd while the slot holds a live order and even while it is
// free. An id is minted with the (odd) generation the slot had at allocation, so
// once that order dies the slot's generation moves on and the old id stops
// matching — even after the slot is reused for a new order. That is what keeps a
// late or duplicate cancel from hitting whichever order now lives in the slot.
//
// Ids are unique but NOT sequential: a freed slot is reused before any fresh one,
// so arrival order must come from somewhere else (BookLevel's FIFO vector).
// Because live generations are odd, no id is ever 0, which stays free as a
// not-found sentinel.
class OrderManager{
    private:
        static constexpr uint32_t NIL = UINT32_MAX;

        struct Slot {
            Order order{0, OrderSide::BUY, OrderType::LIMIT, 0, 0};
            uint32_t generation = 0;    // odd = live, even = free
            uint32_t next_free = NIL;   // meaningful only while free
        };
        // Two slots per cache line. Not a correctness requirement — just a
        // tripwire so that fattening Order is a deliberate decision.
        static_assert(sizeof(Slot) <= 32, "Slot outgrew half a cache line");

        std::vector<Slot> slots;
        // Top of the free list. LIFO on purpose: the most recently freed slot is
        // the one most likely to still be in cache.
        uint32_t free_head;

        static uint32_t index_of(order_id_t id) { return static_cast<uint32_t>(id); }
        static uint32_t generation_of(order_id_t id) { return static_cast<uint32_t>(id >> 32); }
        static order_id_t make_id(uint32_t index, uint32_t generation) {
            return (static_cast<order_id_t>(generation) << 32) | index;
        }

        // The single liveness check: in range, and the slot is live under the
        // same generation the id was minted with. nullptr for unknown, dead, or
        // stale ids.
        Slot* resolve(order_id_t id);
        const Slot* resolve(order_id_t id) const;

        // Threads slots [first, slots.size()) onto the free list.
        void link_free(size_t first);
        // Doubles the slab. Runs only when every slot is live; size the
        // constructor's capacity to the expected high-water mark to keep it off
        // the hot path entirely.
        void grow();
        void release(Slot& slot);

    public:
        static constexpr size_t DEFAULT_CAPACITY = size_t{1} << 16;

        explicit OrderManager(size_t capacity = DEFAULT_CAPACITY);
        ~OrderManager() = default;
        OrderManager(const OrderManager&) = delete;
        OrderManager& operator=(const OrderManager&) = delete;

        order_id_t add_order(OrderSide order_side, OrderType order_type, int qty, int price);
        bool cancel_order(order_id_t order_id);
        bool modify_order(order_id_t order_id, int new_qty, int new_price);
        int fulfill_order(order_id_t order_id, int qty);

        // Single lookup for every field a caller needs. Prefer this over asking
        // for fields one at a time — the per-field accessors were removed so the
        // one-lookup-per-field pattern cannot creep back in.
        // Returns nullopt when the id is unknown or the order is no longer live.
        std::optional<OrderView> getView(order_id_t order_id) const;
        bool valid(order_id_t order_id) const;

        size_t capacity() const { return slots.size(); }
};
