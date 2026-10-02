#include "BookLevel.h"
#include "Order.h"

// 2 KB of ids per level. This was 512 ids when order_id_t was 32 bits; it was
// halved when ids widened to 64 so the per-level footprint stayed the same —
// bench_add's NewPriceLevel, which opens a level per order, doubled its RSS and
// slowed down accordingly at 512.
static constexpr size_t kReservedOrdersPerLevel = 256;

BookLevel::BookLevel() : total_quantity(0), price(0), live_orders(0), head(0) {
    orders.reserve(kReservedOrdersPerLevel);
}
BookLevel::BookLevel(std::shared_ptr<OrderManager>& order_manager, int price) : total_quantity(0), price(price), live_orders(0), head(0), order_manager(order_manager) {
    orders.reserve(kReservedOrdersPerLevel);
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
    for(size_t i = head; i < orders.size(); ++i) {
        if(order_manager->valid(orders[i])) validOrders.push_back(orders[i]);
    }
    return validOrders;
}

std::vector<order_id_t> BookLevel::getAllOrders() const {
    return {orders.begin() + static_cast<std::ptrdiff_t>(head), orders.end()};
}

int BookLevel::getPrice() const {
    return price;
}

LevelView BookLevel::getView() const {
    return {price, total_quantity};
}

void BookLevel::reset(int new_price) {
    price = new_price;
    total_quantity = 0;
    live_orders = 0;
    head = 0;
    orders.clear();
}

order_id_t BookLevel::addOrder(OrderSide side, OrderType type, int price, int quantity) {
    order_id_t order_id = order_manager->add_order(side, type, quantity, price);
    total_quantity += quantity;
    // Reclaim dead slots instead of letting push_back reallocate.
    if(orders.size()==orders.capacity() && live_orders < orders.size()) compact();
    orders.push_back(order_id);
    live_orders += 1;
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
        // Modify-to-zero retires the order; its id stays as a dead entry.
        if(new_quantity==0) live_orders -= 1;
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
    // The id is left in place as a dead entry; fills skip it and compact() drops it.
    live_orders -= 1;
    return true;
}

int BookLevel::fillOrders(int qty, std::vector<TradeEvent>& filled_orders, order_id_t counter_order_id) {
    int rem_qty = qty;
    // Orders fill strictly front to back, so every entry that is dead or gets
    // fully filled sits at `head`: advancing it retires them without a scan.
    while(rem_qty > 0 && head < orders.size()) {
        const order_id_t order_id = orders[head];
        const auto order = order_manager->getView(order_id);
        if(!order) [[unlikely]] { head += 1; continue; }

        const int orig_qty = rem_qty;
        rem_qty = order_manager->fulfill_order(order_id, rem_qty);
        const int filled_qty = orig_qty - rem_qty;

        // A zero-quantity resting order retires without trading.
        if(filled_qty > 0) [[likely]] {
            // 0 is a transient value for order ID.
            order_id_t buy_order_id = 0, sell_order_id = 0;
            if(order->side==OrderSide::BUY) {buy_order_id = order->orderId; sell_order_id = counter_order_id;}
            else {sell_order_id = order->orderId; buy_order_id = counter_order_id;}

            filled_orders.emplace_back(buy_order_id, sell_order_id, order->price, filled_qty);
        }

        // A partial fill leaves the head order resting, and nothing behind it can trade.
        if(filled_qty < order->quantity) break;
        head += 1;
        live_orders -= 1;
    }
    // Fully drained: reset without releasing capacity.
    if(head==orders.size()) {
        orders.clear();
        head = 0;
    }
    total_quantity = std::max(0, total_quantity - qty);
    return rem_qty;
}

void BookLevel::compact() {
    // Single in-place pass: live ids move down over consumed and dead slots,
    // keeping FIFO order. Shrinking resize() never allocates.
    size_t out = 0;
    for(size_t i = head; i < orders.size(); ++i) {
        if(order_manager->valid(orders[i])) orders[out++] = orders[i];
    }
    orders.resize(out);
    head = 0;
    live_orders = out;
}
