#include "OrderManager.h"

#include <algorithm>
#include <stdexcept>

OrderManager::OrderManager(size_t capacity): free_head(NIL) {
    // resize, not reserve: constructing every slot now touches every page now, so
    // the first orders into a fresh slab do not take page faults on the hot path.
    slots.resize(capacity == 0 ? 1 : capacity);
    link_free(0);
}

void OrderManager::link_free(size_t first) {
    // Push in reverse so the lowest index ends up on top of the free list.
    for(size_t i = slots.size(); i-- > first;) {
        slots[i].next_free = free_head;
        free_head = static_cast<uint32_t>(i);
    }
}

void OrderManager::grow() {
    const size_t old_size = slots.size();
    // The index is 32 bits and NIL is reserved.
    if(old_size >= NIL) [[unlikely]] throw std::length_error("OrderManager slab exhausted");
    slots.resize(std::min<size_t>(old_size * 2, NIL));
    link_free(old_size);
}

OrderManager::Slot* OrderManager::resolve(order_id_t id) {
    const uint32_t index = index_of(id);
    if(index >= slots.size()) [[unlikely]] return nullptr;
    Slot& slot = slots[index];
    // Equal generations alone are not enough: a forged id with an even
    // generation could match a free slot. Odd means the slot is live.
    if(slot.generation != generation_of(id) || (slot.generation & 1u) == 0) [[unlikely]] return nullptr;
    return &slot;
}

const OrderManager::Slot* OrderManager::resolve(order_id_t id) const {
    return const_cast<OrderManager*>(this)->resolve(id);
}

void OrderManager::release(Slot& slot) {
    // Even again: every id minted for this occupancy is now stale.
    slot.generation += 1;
    slot.next_free = free_head;
    free_head = static_cast<uint32_t>(&slot - slots.data());
}

order_id_t OrderManager::add_order(OrderSide order_side, OrderType order_type, int qty, int price) {
    if(free_head == NIL) [[unlikely]] grow();
    const uint32_t index = free_head;
    Slot& slot = slots[index];
    free_head = slot.next_free;

    // Odd: live. Skipping 0 on wrap is unnecessary — a wrapped generation is
    // still odd, so an id can never be 0.
    slot.generation += 1;
    const order_id_t id = make_id(index, slot.generation);
    slot.order = Order(id, order_side, order_type, price, qty);
    return id;
}

bool OrderManager::cancel_order(order_id_t order_id) {
    Slot* slot = resolve(order_id);
    if(!slot) return false;
    const bool cancelled = slot->order.cancel();
    release(*slot);
    return cancelled;
}

bool OrderManager::modify_order(order_id_t order_id, int new_qty, int new_price) {
    Slot* slot = resolve(order_id);
    if(!slot) return false;

    const bool modified = slot->order.modify(new_qty, new_price);
    // A modify down to zero quantity retires the order. Release it here the way
    // cancel_order and fulfill_order do, otherwise the slot stays occupied
    // forever — invisible through getView(), but never reused.
    if(!slot->order.valid()) release(*slot);
    return modified;
}

int OrderManager::fulfill_order(order_id_t order_id, int qty) {
    Slot* slot = resolve(order_id);
    if(!slot) return qty;
    const int rem_qty = slot->order.fulfill(qty);
    if(!slot->order.valid()) release(*slot);
    return rem_qty;
}

std::optional<OrderView> OrderManager::getView(order_id_t order_id) const {
    if(const Slot* slot = resolve(order_id)) return slot->order.getView();
    return std::nullopt;
}

bool OrderManager::valid(order_id_t order_id) const {
    return resolve(order_id) != nullptr;
}
