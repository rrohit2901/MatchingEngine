#include "MatchingEngine.h"
#include <iostream>

int main() {
    MatchingEngine me;

    // Add some orders
    int orderId1 = me.addOrder(100.5, 10, OrderType::LIMIT, OrderSide::BUY);
    int orderId2 = me.addOrder(101.0, 5, OrderType::LIMIT, OrderSide::SELL);

    int orderId3 = me.addOrder(100.7, 10, OrderType::LIMIT, OrderSide::BUY);
    int orderId4 = me.addOrder(101.3, 5, OrderType::LIMIT, OrderSide::SELL);

    int orderId5 = me.addOrder(100.5, 10, OrderType::LIMIT, OrderSide::BUY);
    int orderId6 = me.addOrder(102.5, 5, OrderType::LIMIT, OrderSide::SELL);

    int orderId7 = me.addOrder(102.5, 10, OrderType::LIMIT, OrderSide::BUY);

    // Modify an order
    // orderBook.modifyOrder(orderId1, 15, 100.5, OrderSide::BUY, OrderType::LIMIT);
    // orderBook.modifyOrder(orderId3, 15, 100.5, OrderSide::SELL, OrderType::LIMIT);

    // // Cancel an order
    // orderBook.cancelOrder(orderId2);

    // Get order book view
    auto [buyLevels, sellLevels] = me.getOrderBookView(5);
    std::cout << "Buy Levels:" << std::endl;
    for (const auto& level : buyLevels) {
        std::cout << "Price: " << level->getPrice() << ", Quantity: " << level->getTotalQuantity() << std::endl;
        std::cout <<"---------"<<std::endl;
        for(const auto& order : level->getOrders()) {
            if (order->valid()) {
                std::cout << "Order ID: " << order->getOrderId() << ", Quantity: " << order->getQuantity() << std::endl;
            }
        }
        std::cout <<"---------"<<std::endl;
    }
    std::cout << "Sell Levels:" << std::endl;
    for (const auto& level : sellLevels) {
        std::cout << "Price: " << level->getPrice() << ", Quantity: " << level->getTotalQuantity() << std::endl;
        std::cout <<"---------"<<std::endl;
        for(const auto& order : level->getOrders()) {
            if (order->valid()) {
                std::cout << "Order ID: " << order->getOrderId() << ", Quantity: " << order->getQuantity() << std::endl;
            }
        }
        std::cout <<"---------"<<std::endl;
    }

    return 0;
}
