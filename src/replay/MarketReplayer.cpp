#include "MarketReplayer.h"

namespace {

// B and A are Databento's sides; anything else (N) cannot rest in a book.
bool toSide(uint8_t side, OrderSide& out) {
    if (side == 'B') { out = OrderSide::BUY; return true; }
    if (side == 'A') { out = OrderSide::SELL; return true; }
    return false;
}

}  // namespace

MarketReplayer::MarketReplayer(size_t expected_orders) {
    ids.reserve(expected_orders);
}

order_id_t MarketReplayer::engineId(uint64_t venue_id) const {
    const auto it = ids.find(venue_id);
    return it == ids.end() ? order_id_t{0} : it->second;
}

void MarketReplayer::add(uint64_t venue_id, OrderSide side, int price, int size) {
    if (const auto it = ids.find(venue_id); it != ids.end()) [[unlikely]] {
        // The venue reused a live id. Trust the newer record.
        stats.duplicate_add += 1;
        book.cancelOrder(it->second);
        ids.erase(it);
    }
    ids.emplace(venue_id, book.addOrder(price, size, OrderType::LIMIT, side));
    stats.adds += 1;
}

void MarketReplayer::cancel(uint64_t venue_id, int size) {
    const auto it = ids.find(venue_id);
    if (it == ids.end()) [[unlikely]] {
        stats.unknown_order += 1;
        return;
    }
    const auto order = book.getOrderView(it->second);
    if (!order) [[unlikely]] {
        // Mapped but gone from the book: the map is stale. Drop the entry.
        stats.unknown_order += 1;
        ids.erase(it);
        return;
    }

    // A cancel's size is the amount removed, not the amount left (measured in
    // Phase 0: no cancel on a clean day is larger than its order).
    if (size >= order->quantity) {
        if (size > order->quantity) [[unlikely]] stats.cancel_oversized += 1;
        book.cancelOrder(it->second);
        ids.erase(it);
        stats.cancels_full += 1;
        return;
    }
    // Partial: shrinking an order in place keeps its id and its queue position.
    const auto kept = book.modifyOrder(it->second, order->quantity - size, order->price, order->side, order->type);
    if (kept) it->second = *kept;
    stats.cancels_partial += 1;
}

void MarketReplayer::modify(uint64_t venue_id, OrderSide side, int price, int size) {
    const auto it = ids.find(venue_id);
    if (it == ids.end()) {
        // Databento's own book examples treat a modify of an unknown order as an add.
        stats.modify_unknown += 1;
        if (size > 0) add(venue_id, side, price, size);
        return;
    }
    stats.modifies += 1;
    if (size <= 0) {
        book.cancelOrder(it->second);
        ids.erase(it);
        return;
    }
    // OrderBook decides priority: same price and smaller size keeps the id; a
    // price change, a size increase or a side change re-queues under a new id.
    const auto modified = book.modifyOrder(it->second, size, price, side, OrderType::LIMIT);
    if (modified) {
        it->second = *modified;
    } else [[unlikely]] {
        stats.unknown_order += 1;
        ids.erase(it);
    }
}

void MarketReplayer::clear() {
    for (const auto& [venue_id, engine_id] : ids) {
        book.cancelOrder(engine_id);
    }
    ids.clear();
    stats.clears += 1;
}

void MarketReplayer::apply(const MboEvents& events, size_t i) {
    const uint8_t action = events.action[i];
    const uint64_t venue_id = events.order_id[i];
    const int size = static_cast<int>(events.size[i]);
    const int price = events.price[i];

    stats.records += 1;
    if (events.flags[i] & mbo::F_LAST) stats.events += 1;

    OrderSide side = OrderSide::BUY;
    switch (action) {
        case 'A':
            if (!toSide(events.side[i], side)) [[unlikely]] { stats.bad_side += 1; return; }
            if (price == mbo::PRICE_UNDEF) [[unlikely]] { stats.bad_price += 1; return; }
            add(venue_id, side, price, size);
            return;
        case 'C':
            cancel(venue_id, size);
            return;
        case 'M':
            if (!toSide(events.side[i], side)) [[unlikely]] { stats.bad_side += 1; return; }
            if (price == mbo::PRICE_UNDEF) [[unlikely]] { stats.bad_price += 1; return; }
            modify(venue_id, side, price, size);
            return;
        case 'R':
            clear();
            return;
        case 'T':
            stats.trades += 1;
            return;
        case 'F':
            // The fill itself changes nothing; the C that follows it removes the size.
            stats.fills += 1;
            if (!ids.contains(venue_id)) [[unlikely]] stats.unknown_order += 1;
            return;
        case 'N':
            stats.nones += 1;
            return;
        default:
            stats.unknown_action += 1;
            return;
    }
}

size_t MarketReplayer::applyEvent(const MboEvents& events, size_t i) {
    const size_t n = events.count();
    while (i < n) {
        const bool last = events.flags[i] & mbo::F_LAST;
        apply(events, i);
        ++i;
        if (last) break;
    }
    return i;
}
