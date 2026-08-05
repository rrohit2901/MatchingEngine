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

        order_id_t add_order(OrderSide order_side, OrderType order_type, unsigned int qty, unsigned int price);
        bool cancel_order(order_id_t order_id);
        bool modify_order(order_id_t order_id, unsigned int new_qty, unsigned int new_price);
        int fulfill_order(order_id_t order_id, int qty);

        std::optional<OrderSide> getSide(order_id_t order_id) const;
        std::optional<OrderType> getType(order_id_t order_id) const;
        std::optional<int> getPrice(order_id_t order_id) const;
        std::optional<int> getQuantity(order_id_t order_id) const;
        bool valid(order_id_t order_id) const;
};
