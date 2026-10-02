#include "BookLevel.h"
#include "Order.h"
#include <ranges>

BookLevel::BookLevel() : total_quantity(0), price(0) {
    orders.reserve(512);
}
BookLevel::BookLevel(std::shared_ptr<OrderManager>& order_manager, int price) : total_quantity(0), price(price), order_manager(order_manager) {
    orders.reserve(512);
}

BookLevel::~BookLevel() = default;
BookLevel::BookLevel(const BookLevel& other) = default;
BookLevel& BookLevel::operator=(const BookLevel& other) = default;
BookLevel::BookLevel(BookLevel&& other) noexcept = default;
BookLevel& BookLevel::operator=(BookLevel&& other) noexcept = default;

int BookLevel::getTotalQuantity() const {
    return total_quantity;
}

std::vector<order_id_t> BookLevel::getOrders() const {
    std::vector<order_id_t> validOrders;
    for(order_id_t order_id: orders) {
        if(order_manager->valid(order_id)) validOrders.push_back(order_id);
    } 
    return validOrders;
}

std::vector<order_id_t> BookLevel::getAllOrders() const {
    return orders;
}

int BookLevel::getPrice() const {
    return price;
}

order_id_t BookLevel::addOrder(OrderSide side, OrderType type, int price, int quantity) {
    order_id_t order_id = order_manager->add_order(side, type, quantity, price);
    total_quantity += quantity;
    orders.push_back(order_id);
    return order_id;
}

std::optional<order_id_t> BookLevel::modifyOrder(order_id_t order_id, int new_quantity, int new_price) {
    // One lookup covers the validity check and every field used below.
    const auto order = order_manager->getView(order_id);
    if (!order) [[unlikely]] return std::nullopt;

    // Signed comparison: current_quantity used to be unsigned, which turned a
    // negative new_quantity into a huge value and took the wrong branch.
    const int current_quantity = order->quantity;
    if(new_quantity <= current_quantity) {
        total_quantity -= (current_quantity - new_quantity);
        order_manager->modify_order(order_id, new_quantity, new_price);
        return order_id;
    }
    order_id_t modified_order_id = addOrder(order->side, order->type, new_price, new_quantity);
    // Return value is ignored because here we know order_id corresponds to a valid order.
    cancelOrder(order_id);

    return modified_order_id;
}

bool BookLevel::cancelOrder(order_id_t order_id) {
    const auto order = order_manager->getView(order_id);
    if (!order) return false;
    total_quantity -= order->quantity;
    order_manager->cancel_order(order_id);
    auto it = lower_bound(orders.begin(), orders.end(), order_id);
    if (it != orders.end() && *it == order_id) [[likely]] orders.erase(it);
    return true;
}

int BookLevel::fillOrders(int qty, std::vector<TradeEvent>& filled_orders, order_id_t counter_order_id) {
    int rem_qty = qty;
    for(order_id_t order_id: orders) {
        // Nothing left to match against this level's remaining orders.
        if(rem_qty==0) break;

        int orig_qty = rem_qty;
        const auto order = order_manager->getView(order_id);
        if(!order) [[unlikely]] continue;

        rem_qty = order_manager->fulfill_order(order_id, rem_qty);

        int price = order->price;
        int filled_qty = orig_qty - rem_qty;
        if(filled_qty==0) continue;

        // 0 is a transient value for order ID.
        order_id_t buy_order_id = 0, sell_order_id = 0;
        if(order->side==OrderSide::BUY) {buy_order_id = order->orderId; sell_order_id = counter_order_id;}
        else {sell_order_id = order->orderId; buy_order_id = counter_order_id;}

        filled_orders.emplace_back(buy_order_id, sell_order_id, price, filled_qty);
    }
    std::erase_if(orders, [&](order_id_t order_id) { return !order_manager->valid(order_id); });
    total_quantity = std::max(0, total_quantity - qty);
    return rem_qty;
}
