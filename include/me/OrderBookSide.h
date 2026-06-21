#pragma once

#include "BookLevel.h"
#include "Order.h"

#include <map>
#include <unordered_map>
#include <type_traits>
#include <limits>

template<OrderSide side>
class OrderBookSide {
    private:
        int marketPrice = (side == OrderSide::BUY) ? std::numeric_limits<int>::max() : std::numeric_limits<int>::min();
        using Comparator = std::conditional_t<side == OrderSide::BUY, std::greater<int>, std::less<int>>;
        std::map<int, std::shared_ptr<BookLevel>, Comparator> priceLevels; 
        std::unordered_map<int, std::pair<std::shared_ptr<BookLevel>, std::shared_ptr<Order>>> orderIdMap;

        std::shared_ptr<Order> addLimitOrder(int orderId, int price, int quantity) {
            if(priceLevels.find(price) == priceLevels.end()) {
                priceLevels[price] = std::make_shared<BookLevel>(price);
            }
            auto order = priceLevels[price]->addOrder(orderId, side, OrderType::LIMIT, price, quantity);
            orderIdMap[orderId] = {priceLevels[price], order};
            return order;
        }

        std::shared_ptr<Order> addMarketOrder(int orderId, int quantity) {
            int price = marketPrice;
            if(priceLevels.find(price) == priceLevels.end()) {
                priceLevels[price] = std::make_shared<BookLevel>(price);
            }
            auto order = priceLevels[price]->addOrder(orderId, side, OrderType::MARKET, price, quantity);
            orderIdMap[orderId] = {priceLevels[price], order};
            return order;
        }
    public:
        OrderBookSide() {};
        ~OrderBookSide() = default;
        OrderBookSide(const OrderBookSide&) = default;
        OrderBookSide& operator=(const OrderBookSide&) = default;
        OrderBookSide(OrderBookSide&&) noexcept = default;
        OrderBookSide& operator=(OrderBookSide&&) noexcept = default;

        OrderSide getSide() const {
            return side;
        }

        auto getLevels() const {
            std::vector<std::shared_ptr<BookLevel>> levels;
            for (const auto& [price, level] : priceLevels) {
                levels.push_back(level);
            }
            return levels;
        }

        bool isOrderIdExist(int orderId) const {
            return orderIdMap.find(orderId) != orderIdMap.end();
        }

        const BookLevel& getLevel(int price) const {
            if (priceLevels.find(price) == priceLevels.end()) {
                throw std::runtime_error("Price level does not exist");
            }
            return *priceLevels.at(price);
        }

        std::vector<std::shared_ptr<BookLevel>> getBookSideView(int numLevels = 1) const {
            std::vector<std::shared_ptr<BookLevel>> levels;
            for (const auto& [price, level] : priceLevels) {
                levels.push_back(level);
                if (static_cast<int>(levels.size()) >= numLevels) {
                    break;
                }
            }
            return levels;
        }

        std::shared_ptr<Order> addOrder(int orderId, OrderType type, int price, int quantity) {
            if (type == OrderType::LIMIT) {
                return addLimitOrder(orderId, price, quantity);
            } else if (type == OrderType::MARKET) {
                return addMarketOrder(orderId, quantity);
            }
            return nullptr;
        }

        bool cancelOrder(int orderId) {
            if (orderIdMap.find(orderId) == orderIdMap.end()) {
                return false;
            }
            auto& [level, order] = orderIdMap[orderId];
            bool isCancelled = level->cancelOrder(order);
            if (level->getTotalQuantity() == 0) {
                priceLevels.erase(order->getPrice());
            }
            if(isCancelled){
                orderIdMap.erase(orderId);
            }
            return isCancelled;
        }

        std::shared_ptr<Order> modifyOrder(int orderId, int newQuantity, int newPrice) {
            if (orderIdMap.find(orderId) == orderIdMap.end()) {
                return nullptr;
            }
            auto& [level, order] = orderIdMap[orderId];
            std::shared_ptr<Order> modifiedOrder;

            if(newPrice == order->getPrice()) {
                modifiedOrder = level->modifyOrder(order, newQuantity, newPrice);
            } else {
                level->cancelOrder(order);
                if (level->getTotalQuantity() == 0) {
                    priceLevels.erase(order->getPrice());
                }
                if(priceLevels.find(newPrice) == priceLevels.end()) {
                    priceLevels[newPrice] = std::make_shared<BookLevel>(newPrice);
                }
                modifiedOrder = priceLevels[newPrice]->addOrder(orderId, side, order->getType(), newPrice, newQuantity);
            }
            if(modifiedOrder) {
                orderIdMap[orderId] = {priceLevels[newPrice], modifiedOrder};
            }
            
            return modifiedOrder;
        }

        Order getOrder(int orderId) const {
            if (orderIdMap.find(orderId) == orderIdMap.end()) {
                throw std::runtime_error("Order ID does not exist");
            }
            return *(orderIdMap.at(orderId).second);
        }
};
