#pragma once
#include "Order.h"
#include <vector>
#include <memory>


class BookLevel {
    private:
        int totalQuantity;
        int price;
        std::vector<std::shared_ptr<Order>> orders;
    public:
        BookLevel();
        BookLevel(int price);
        ~BookLevel();
        BookLevel(const BookLevel&);
        BookLevel& operator=(const BookLevel&);
        BookLevel(BookLevel&&) noexcept;
        BookLevel& operator=(BookLevel&&) noexcept;

        int getTotalQuantity() const;
        std::vector<std::shared_ptr<Order>> getOrders() const;
        std::vector<std::shared_ptr<Order>> getAllOrders() const;
        double getPrice() const;

        std::shared_ptr<Order> addOrder(int orderId, OrderSide side, OrderType type, int price, int quantity);
        std::shared_ptr<Order> modifyOrder(std::shared_ptr<Order>& order, int newQuantity, int newPrice);
        bool cancelOrder(std::shared_ptr<Order>& order);
        int fillOrders(int qty);
};
