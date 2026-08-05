#include "OrderManager.h"
#include <memory>

OrderManager::OrderManager(): order_id_counter(1) {};

order_id_t OrderManager::add_order(OrderSide order_side, OrderType order_type, int qty, int price) {
    orders.insert({order_id_counter, std::make_unique<Order>(order_id_counter, order_side, order_type, price, qty)});
    return order_id_counter++;
}

bool OrderManager::cancel_order(order_id_t order_id) {
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        bool cancelled = iter->second->cancel();
        orders.erase(iter);
        return cancelled;
    }
    return false;
}

bool OrderManager::modify_order(order_id_t order_id, int new_qty, int new_price) {
    auto iter = orders.find(order_id);
    if(iter==orders.end() || !iter->second->valid()) return false;

    bool modified = iter->second->modify(new_qty, new_price);
    // A modify down to zero quantity retires the order. Drop it here the way
    // cancel_order and fulfill_order do, otherwise the dead entry stays in the
    // map forever — invisible through getView(), but still holding memory.
    if(!iter->second->valid()) orders.erase(iter);
    return modified;
}

int OrderManager::fulfill_order(order_id_t order_id, int qty) {
    int rem_qty = qty;
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        rem_qty = iter->second->fulfill(qty);
        if(!iter->second->valid()) orders.erase(iter);
    }
    return rem_qty;
}

std::optional<OrderView> OrderManager::getView(order_id_t order_id) const {
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        return iter->second->getView();
    }
    return std::nullopt;
}

bool OrderManager::valid(order_id_t order_id) const {
    auto iter = orders.find(order_id);
    return iter!=orders.end() && iter->second->valid();
}
