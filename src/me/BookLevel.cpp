#include "BookLevel.h"
#include "Order.h"
#include <ranges>

BookLevel::BookLevel() : total_quantity(0), price(0), total_orders(0), valid_orders(0) {}
BookLevel::BookLevel(std::shared_ptr<OrderManager>& order_manager, int price) : total_quantity(0), price(price), total_orders(0), valid_orders(0), order_manager(order_manager) {}

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

order_id_t BookLevel::addOrder(OrderSide side, OrderType type, int price, unsigned int quantity) {
    order_id_t order_id = order_manager->add_order(side, type, quantity, price);
    total_quantity += quantity;
    total_orders += 1;
    valid_orders += 1;
    orders.push_back(order_id);
    return orders.back();
}

std::optional<order_id_t> BookLevel::modifyOrder(order_id_t order_id, int new_quantity, int new_price) {
    if (!order_manager->valid(order_id)) return std::nullopt;

    unsigned int current_quantity = order_manager->getQuantity(order_id).value();
    if(new_quantity <= current_quantity) {
        total_quantity -= (current_quantity - new_quantity);
        order_manager->modify_order(order_id, new_quantity, new_price);
        return order_id;
    }
    order_id_t modified_order_id = addOrder(order_manager->getSide(order_id).value(), order_manager->getType(order_id).value(), new_price, new_quantity);
    // Return value is ignored because here we know order_id corresponds to a valid order.
    cancelOrder(order_id);

    // Handling case where order is modified to have 0 quantity
    if(!order_manager->valid(modified_order_id)) valid_orders -= 1;

    return modified_order_id;
}

bool BookLevel::cancelOrder(order_id_t order_id) {
    if (!order_manager->valid(order_id)) return false;
    total_quantity -= order_manager->getQuantity(order_id).value();
    valid_orders -= 1;

    order_manager->cancel_order(order_id);
    // Run check to remove cancelled/filled orders
    compact();
    return true;
}

int BookLevel::fillOrders(int qty) {
    int rem_qty = qty;
    for(order_id_t order_id: orders) {
        if(!order_manager->valid(order_id)) continue;
        rem_qty = order_manager->fulfill_order(order_id, rem_qty);
        if(!order_manager->valid(order_id)) valid_orders -= 1;
    }
    total_quantity = std::max(0, total_quantity - qty);
    // Run check to remove cancelled/filled orders
    compact();
    return rem_qty;
}

void BookLevel::compact() {
    if (valid_orders > 0.2*total_orders) return;

    auto valid_orders_v = orders | std::views::filter([&](order_id_t order_id){return order_manager->valid(order_id);});
    std::vector<order_id_t> valid_orders;
    for(order_id_t order_id: valid_orders_v) {
        valid_orders.push_back(order_id);
    }
    orders = std::move(valid_orders);
}
