#pragma once

#include<unordered_map>
#include<memory>
#include<optional>
#include "Order.h"

class OrderManager{
    private:
        order_id_t order_id_counter;
        std::unordered_map<order_id_t, std::unique_ptr<Order>> orders;
    public:
        OrderManager();
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
};
