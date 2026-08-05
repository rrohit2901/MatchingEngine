#include "OrderManager.h"
#include <memory>

OrderManager::OrderManager(): order_id_counter(1) {};

order_id_t OrderManager::add_order(OrderSide order_side, OrderType order_type, unsigned int qty, unsigned int price) {
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

bool OrderManager::modify_order(order_id_t order_id, unsigned int new_qty, unsigned int new_price) {
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        return iter->second->modify(new_qty, new_price);
    }
    return false;
}

int OrderManager::fulfill_order(order_id_t order_id, int qty) {
    int rem_qty = qty;
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        rem_qty = iter->second->fulfill(qty);
        if(!iter->second->valid()) orders.erase(order_id);
    }
    return rem_qty;
}

std::optional<OrderSide> OrderManager::getSide(order_id_t order_id) const {
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        return iter->second->getSide();
    }
    return std::nullopt;
}

std::optional<OrderType> OrderManager::getType(order_id_t order_id) const {
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        return iter->second->getType();
    }
    return std::nullopt;
}

std::optional<int> OrderManager::getPrice(order_id_t order_id) const {
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        return iter->second->getPrice();
    }
    return std::nullopt;
}

std::optional<int> OrderManager::getQuantity(order_id_t order_id) const {
    if(auto iter = orders.find(order_id); iter!=orders.end() && iter->second->valid()) {
        return iter->second->getQuantity();
    }
    return std::nullopt;
}

bool OrderManager::valid(order_id_t order_id) const {
    auto iter = orders.find(order_id);
    return iter!=orders.end() && iter->second->valid();
}
