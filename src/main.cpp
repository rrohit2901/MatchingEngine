#include "MatchingEngine.h"

#include <iostream>
#include <vector>

namespace {

// Prices are plain integers (ticks) — there is no floating point in the engine,
// so the old `100.5` literals silently truncated. BookLevel hands back order ids
// rather than Order objects now, so a level can only print ids; per-order detail
// would need MatchingEngine to forward OrderBook's getOrder*() accessors.
void printSide(const char* label, const std::vector<std::shared_ptr<BookLevel>>& levels) {
    std::cout << label << ":\n";
    for (const auto& level : levels) {
        std::cout << "  price " << level->getPrice()
                  << ", quantity " << level->getTotalQuantity() << '\n';
        // getOrders() already filters out cancelled/filled ids, so no valid() check here.
        for (order_id_t order_id : level->getOrders()) {
            std::cout << "    order " << order_id << '\n';
        }
    }
}

} // namespace

int main() {
    MatchingEngine me;

    const order_id_t buy1 = me.addOrder(1005, 10, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(1010, 5, OrderType::LIMIT, OrderSide::SELL);

    const order_id_t buy2 = me.addOrder(1007, 10, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(1013, 5, OrderType::LIMIT, OrderSide::SELL);

    me.addOrder(1005, 10, OrderType::LIMIT, OrderSide::BUY);
    const order_id_t sell3 = me.addOrder(1025, 5, OrderType::LIMIT, OrderSide::SELL);

    me.addOrder(1025, 10, OrderType::LIMIT, OrderSide::BUY);

    // modifyOrder reports the surviving order id, or nullopt when the order is
    // not in the book. A requote can change the id, so keep the returned value.
    if (const auto requoted = me.modifyOrder(buy1, 15, 1005, OrderSide::BUY, OrderType::LIMIT)) {
        std::cout << "order " << buy1 << " requoted as " << *requoted << '\n';
    } else {
        std::cout << "order " << buy1 << " could not be modified\n";
    }

    std::cout << std::boolalpha;
    std::cout << "cancel " << sell3 << ": " << me.cancelOrder(sell3) << '\n';
    std::cout << "cancel " << buy2 << ": " << me.cancelOrder(buy2) << '\n';

    const auto [buyLevels, sellLevels] = me.getOrderBookView(5);
    printSide("Buy levels", buyLevels);
    printSide("Sell levels", sellLevels);

    return 0;
}
