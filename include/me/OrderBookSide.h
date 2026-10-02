#pragma once

#include "BookLevel.h"
#include "Order.h"
#include "OrderManager.h"
#include "Events.h"

#include <algorithm>
#include <optional>
#include <vector>

template<OrderSide side>
class OrderBookSide {
    private:
        // Levels are kept sorted worst -> best, so the best price sits at back().
        // Most activity is at the top of the book, which makes adds, fills and
        // level removals there touch the tail of the vector and shift little.
        //   BUY:  ascending prices (highest bid at back)
        //   SELL: descending prices (lowest ask at back)
        static constexpr bool isBetter(int a, int b) {
            return side == OrderSide::BUY ? a > b : a < b;
        }

        // Capacity reserved up front for both vectors below; past it they grow normally.
        static constexpr size_t kReservedLevels = 1024;

        std::vector<BookLevel> levels;
        // Emptied levels parked for reuse. Erasing a level would destroy its order
        // vector, and the next new level would allocate a fresh one.
        std::vector<BookLevel> spare_levels;
        std::shared_ptr<OrderManager> order_manager;

        using LevelIter = typename std::vector<BookLevel>::iterator;

        // First level whose price is at least as good as `price`.
        template<typename Levels>
        static auto lowerBound(Levels& lvls, int price) {
            return std::lower_bound(lvls.begin(), lvls.end(), price,
                [](const BookLevel& level, int p) { return isBetter(p, level.getPrice()); });
        }

        LevelIter findLevel(int price) {
            auto it = lowerBound(levels, price);
            return (it != levels.end() && it->getPrice() == price) ? it : levels.end();
        }

        LevelIter findOrInsertLevel(int price) {
            auto it = lowerBound(levels, price);
            if (it != levels.end() && it->getPrice() == price) return it;
            if (spare_levels.empty()) {
                return levels.emplace(it, order_manager, price);
            }
            BookLevel level = std::move(spare_levels.back());
            spare_levels.pop_back();
            level.reset(price);
            return levels.insert(it, std::move(level));
        }

        void removeLevel(LevelIter it) {
            // A full pool lets the level go rather than growing past the reservation.
            if (spare_levels.size() < spare_levels.capacity()) {
                spare_levels.push_back(std::move(*it));
            }
            levels.erase(it);
        }

    public:
        OrderBookSide(std::shared_ptr<OrderManager>& order_manager): order_manager(order_manager) {
            levels.reserve(kReservedLevels);
            spare_levels.reserve(kReservedLevels);
        };
        ~OrderBookSide() = default;
        OrderBookSide(const OrderBookSide&) = default;
        OrderBookSide& operator=(const OrderBookSide&) = default;
        OrderBookSide(OrderBookSide&&) noexcept = default;
        OrderBookSide& operator=(OrderBookSide&&) noexcept = default;

        OrderSide getSide() const {
            return side;
        }

        // Every level, best price first.
        std::vector<LevelView> getLevels() const {
            return getBookSideView(static_cast<unsigned>(levels.size()));
        }

        bool isOrderIdExist(order_id_t orderId) const {
            const auto order = order_manager->getView(orderId);
            return order.has_value() && order->side==side;
        }

        std::optional<LevelView> getLevel(int price) const {
            const auto it = lowerBound(levels, price);
            if (it == levels.end() || it->getPrice() != price) return std::nullopt;
            return it->getView();
        }

        // Top `numLevels` levels, best price first.
        std::vector<LevelView> getBookSideView(unsigned numLevels = 1) const {
            const size_t n = std::min<size_t>(numLevels, levels.size());
            std::vector<LevelView> view;
            view.reserve(n);
            for (auto it = levels.rbegin(); it != levels.rbegin() + static_cast<std::ptrdiff_t>(n); ++it) {
                view.push_back(it->getView());
            }
            return view;
        }

        order_id_t addOrder(OrderType type, int price, int quantity) {
            return findOrInsertLevel(price)->addOrder(side, type, price, quantity);
        }

        bool cancelOrder(order_id_t orderId) {
            // One lookup replaces isOrderIdExist()'s check plus the price fetch.
            const auto order = order_manager->getView(orderId);
            if (!order || order->side!=side) {
                return false;
            }
            auto level = findLevel(order->price);
            if (level == levels.end()) [[unlikely]] return false;
            const bool is_cancelled = level->cancelOrder(orderId);

            if(level->getTotalQuantity()==0) {
                removeLevel(level);
            }
            return is_cancelled;
        }

        order_id_t modifyOrder(order_id_t orderId, int newQuantity, int newPrice) {
            // Was five lookups: isOrderIdExist (valid + side), price twice, type.
            const auto order = order_manager->getView(orderId);
            if (!order || order->side!=side) {
                return 0;
            }

            if(newPrice != order->price) {
                // addOrder may insert a level and shift the vector, so the old
                // level is looked up afresh inside cancelOrder; it also drops the
                // old level if this emptied it.
                const order_id_t modifiedOrder = addOrder(order->type, newPrice, newQuantity);
                // Return value is not relevant since we know that orderId is valid.
                cancelOrder(orderId);
                return modifiedOrder;
            }

            auto level = findLevel(order->price);
            if (level == levels.end()) [[unlikely]] return 0;
            const order_id_t modifiedOrder = level->modifyOrder(orderId, newQuantity, newPrice).value();
            if(level->getTotalQuantity()==0) removeLevel(level);
            return modifiedOrder;
        }

        int fillOrders(int target_price, int qty, std::vector<TradeEvent>& filled_orders, order_id_t counter_order_id) {
            // The best level is always at back(), so matching walks the tail and
            // removing a drained level is a pop from the end.
            while (qty > 0 && !levels.empty()) {
                BookLevel& best = levels.back();
                const bool isCrossed = (side==OrderSide::BUY) ? (target_price<=best.getPrice()) : (target_price>=best.getPrice());
                if (!isCrossed) break;

                qty = best.fillOrders(qty, filled_orders, counter_order_id);
                if (best.getTotalQuantity()==0) {
                    removeLevel(levels.end() - 1);
                }
            }
            return qty;
        }

        std::optional<int> getTopPrice() const {
            if (levels.empty()) return std::nullopt;
            return levels.back().getPrice();
        }
};
