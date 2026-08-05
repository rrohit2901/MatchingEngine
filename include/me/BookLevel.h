#pragma once

#include "Order.h"
#include "OrderManager.h"

#include <vector>
#include <memory>


class BookLevel {
    private:
        int total_quantity;
        int price;
        int total_orders;
        int valid_orders;
        std::shared_ptr<OrderManager> order_manager;
        std::vector<order_id_t> orders;

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

        order_id_t addOrder(OrderSide side, OrderType type, int price, int quantity);
        std::optional<order_id_t> modifyOrder(order_id_t order, int new_quantity, int new_price);
        bool cancelOrder(order_id_t order_id);
        int fillOrders(int qty);
};
