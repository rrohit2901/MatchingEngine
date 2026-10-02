#pragma once

#include "Order.h"
#include "Events.h"
#include "OrderManager.h"

#include <vector>
#include <memory>
#include <span>

// What readers of the book get per level: a value snapshot, not a handle onto
// the live level.
struct LevelView {
    int price;
    int quantity;
};

class BookLevel {
    private:
        int total_quantity;
        int price;
        // Live ids in FIFO (arrival) order, interleaved with dead ones. Ids are
        // not ascending — OrderManager recycles slots — so position is the only
        // record of time priority.
        // Entries before `head` were consumed by fills; dead entries at or after
        // `head` are cancelled / modified-to-zero orders, skipped lazily. Neither
        // path erases from the vector, so no hot-path op shifts or allocates.
        size_t live_orders;
        size_t head;
        std::shared_ptr<OrderManager> order_manager;
        std::vector<order_id_t> orders;

        // In-place removal of dead ids; runs only when a push_back would
        // otherwise grow the vector.
        void compact();

    public:
        BookLevel();
        BookLevel(std::shared_ptr<OrderManager>& order_manager, int price);
        ~BookLevel();
        BookLevel(const BookLevel&);
        BookLevel& operator=(const BookLevel&);
        BookLevel(BookLevel&&) noexcept;
        BookLevel& operator=(BookLevel&&) noexcept;

        int getTotalQuantity() const;
        std::vector<order_id_t> getOrders() const;
        std::vector<order_id_t> getAllOrders() const;
        int getPrice() const;
        LevelView getView() const;
        // Ids in time priority, front of the queue first. May include dead
        // (cancelled or filled) ids, which callers skip; valid only until the
        // level next changes.
        std::span<const order_id_t> queue() const {
            return {orders.data() + head, orders.size() - head};
        }

        // Re-arms an emptied level for a new price, keeping the order vector's
        // capacity so a recycled level costs no allocation.
        void reset(int new_price);

        order_id_t addOrder(OrderSide side, OrderType type, int price, int quantity);
        std::optional<order_id_t> modifyOrder(order_id_t order, int new_quantity, int new_price);
        bool cancelOrder(order_id_t order_id);
        int fillOrders(int qty, std::vector<TradeEvent>& filled_orders, order_id_t counter_order_id);
};
