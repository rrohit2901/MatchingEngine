#pragma once

#include "BookLevel.h"
#include "Order.h"
#include "OrderManager.h"

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
        std::shared_ptr<OrderManager> order_manager;
    public:
        OrderBookSide(std::shared_ptr<OrderManager>& order_manager): order_manager(order_manager) {};
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

        bool isOrderIdExist(order_id_t orderId) const {
            const auto order = order_manager->getView(orderId);
            return order.has_value() && order->side==side;
        }

        std::optional<const BookLevel> getLevel(int price) const {
            if (priceLevels.find(price) == priceLevels.end()) {
                return std::nullopt;
            }
            return *priceLevels.at(price);
        }

        std::vector<std::shared_ptr<BookLevel>> getBookSideView(unsigned numLevels = 1) const {
            std::vector<std::shared_ptr<BookLevel>> levels;
            for (const auto& [price, level] : priceLevels) {
                if (levels.size() >= numLevels) {
                    break;
                }
                levels.push_back(level);
            }
            return levels;
        }

        order_id_t addOrder(OrderType type, int price, int quantity) {
            if(priceLevels.find(price)==priceLevels.end()) {
                priceLevels[price] = std::make_shared<BookLevel> (order_manager, price);
            }
            return priceLevels[price]->addOrder(side, type, price, quantity);
        }

        bool cancelOrder(order_id_t orderId) {
            // One lookup replaces isOrderIdExist()'s check plus the price fetch.
            const auto order = order_manager->getView(orderId);
            if (!order || order->side!=side) {
                return false;
            }
            const int order_price = order->price;
            std::shared_ptr<BookLevel> book_level = priceLevels[order_price];
            bool is_cancelled = book_level->cancelOrder(orderId);

            if(book_level->getTotalQuantity()==0) {
                priceLevels.erase(order_price);
            }
            return is_cancelled;
        }

        order_id_t modifyOrder(order_id_t orderId, int newQuantity, int newPrice) {
            // Was five lookups: isOrderIdExist (valid + side), price twice, type.
            const auto order = order_manager->getView(orderId);
            if (!order || order->side!=side) {
                return 0;
            }
            const int order_price = order->price;
            std::shared_ptr<BookLevel> book_level = priceLevels[order_price];

            order_id_t modifiedOrder;

            if(newPrice == order_price) {
                modifiedOrder = book_level->modifyOrder(orderId, newQuantity, newPrice).value();
            } else {
                modifiedOrder = addOrder(order->type, newPrice, newQuantity);
                // Return value is not relevant since we know that orderId is valid.
                cancelOrder(orderId);
            }
            if(auto price_level = priceLevels.find(order_price); price_level!=priceLevels.end() && price_level->second->getTotalQuantity()==0) priceLevels.erase(order_price);
            
            return modifiedOrder;
        }

        std::vector<std::shared_ptr<BookLevel>> getCandidateLevels (int curr_price) {
            std::vector<std::shared_ptr<BookLevel>> candidateLevels;
            for(auto& [price, bookLevel]: priceLevels) {
                bool isCrossed = (side==OrderSide::BUY) ? (curr_price<=price) : (curr_price>=price);
                if(isCrossed) {candidateLevels.push_back(bookLevel);}
                else {break;}
            }
            return candidateLevels;
        }

        int fillOrders(int target_price, int qty) {
            std::vector<std::shared_ptr<BookLevel>> candidates = getCandidateLevels(target_price);
            for(std::shared_ptr<BookLevel>& candidate: candidates) {
                qty = candidate->fillOrders(qty);
                if(candidate->getTotalQuantity()==0) {
                    priceLevels.erase(candidate->getPrice());
                }
            }
            return qty;
        }
};
