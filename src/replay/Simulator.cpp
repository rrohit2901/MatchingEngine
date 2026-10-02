#include "Simulator.h"
#include "ReplayValidator.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

constexpr uint64_t NEVER = std::numeric_limits<uint64_t>::max();

bool toSide(uint8_t side, OrderSide& out) {
    if (side == 'B') { out = OrderSide::BUY; return true; }
    if (side == 'A') { out = OrderSide::SELL; return true; }
    return false;
}

OrderSide opposite(OrderSide side) {
    return side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
}

}  // namespace

const char* to_string(StrategyOrderStatus status) {
    switch (status) {
        case StrategyOrderStatus::PENDING:   return "PENDING";
        case StrategyOrderStatus::OPEN:      return "OPEN";
        case StrategyOrderStatus::FILLED:    return "FILLED";
        case StrategyOrderStatus::CANCELLED: return "CANCELLED";
        case StrategyOrderStatus::REJECTED:  return "REJECTED";
    }
    return "UNKNOWN";
}

const char* to_string(FillSource source) {
    switch (source) {
        case FillSource::AGGRESSIVE:     return "AGGRESSIVE";
        case FillSource::AHEAD_IN_QUEUE: return "AHEAD_IN_QUEUE";
        case FillSource::SWEEP:          return "SWEEP";
        case FillSource::CROSSING_ADD:   return "CROSSING_ADD";
    }
    return "UNKNOWN";
}

Simulator::Simulator(SimConfig cfg) : config(cfg), risk(cfg.risk) {
    if (config.timer_interval_ns == 0) throw std::invalid_argument("timer_interval_ns must be > 0");
    if (config.pnl_sample_interval_ns == 0) throw std::invalid_argument("pnl_sample_interval_ns must be > 0");
    if (config.price_increment <= 0) throw std::invalid_argument("price_increment must be > 0");
    venue.reserve(size_t{1} << 18);
    queue_scratch.reserve(4096);
    trade_scratch.reserve(256);
}

const SimStats& Simulator::getStats() const {
    return stats;
}

// --- book helpers ------------------------------------------------------------

int Simulator::liveQty(order_id_t engine_id) const {
    if (engine_id == 0) return 0;
    const auto view = book.getOrderView(engine_id);
    return view ? view->quantity : 0;
}

int Simulator::reduce(order_id_t engine_id, int qty) {
    if (engine_id == 0 || qty <= 0) return 0;
    const auto view = book.getOrderView(engine_id);
    if (!view) return 0;
    const int take = std::min(qty, view->quantity);
    if (take == view->quantity) {
        book.cancelOrder(engine_id);
    } else {
        // Shrinking in place keeps the id and the queue position.
        book.modifyOrder(engine_id, view->quantity - take, view->price, view->side, view->type);
    }
    return take;
}

StrategyOrder* Simulator::strategyAt(order_id_t engine_id) {
    const auto it = strategy_by_engine.find(engine_id);
    if (it == strategy_by_engine.end()) return nullptr;
    return &strategy_orders.at(it->second);
}

void Simulator::recordFill(StrategyOrder& order, int qty, int price, bool maker, FillSource source) {
    if (qty <= 0) return;
    const int64_t q = qty;
    const int64_t value = static_cast<int64_t>(price) * q;
    if (order.side == OrderSide::BUY) {
        pos += q;
        cash -= value;
        open_buy_qty -= q;
    } else {
        pos -= q;
        cash += value;
        open_sell_qty -= q;
    }
    fee_total += static_cast<double>(q) * (maker ? config.maker_fee : config.taker_fee);
    order.filled += qty;
    fill_log.push_back({clock, order.client_id, order.side, price, qty, maker, source});
    stats.fills += 1;
    (maker ? stats.maker_qty : stats.taker_qty) += q;
    if (order.filled >= order.quantity) finish(order, StrategyOrderStatus::FILLED);
}

void Simulator::finish(StrategyOrder& order, StrategyOrderStatus status) {
    const int64_t unfilled = order.quantity - order.filled;
    if (status != StrategyOrderStatus::FILLED && unfilled > 0) {
        (order.side == OrderSide::BUY ? open_buy_qty : open_sell_qty) -= unfilled;
    }
    if (order.engine_id != 0) {
        strategy_by_engine.erase(order.engine_id);
        order.engine_id = 0;
    }
    order.status = status;
    live_ids.erase(order.client_id);
}

int Simulator::cross(OrderSide incoming, int price, int qty, StrategyOrder* aggressor) {
    const OrderSide resting = opposite(incoming);
    const auto best = book.getBestPrice(resting);
    if (!best) return qty;
    const bool crosses = incoming == OrderSide::BUY ? *best <= price : *best >= price;
    if (!crosses) return qty;

    trade_scratch.clear();
    const int left = book.fillOrders(resting, price, qty, trade_scratch, 0);
    for (const TradeEvent& trade : trade_scratch) {
        const order_id_t resting_id = resting == OrderSide::BUY ? trade.buy_id : trade.sell_id;
        if (aggressor) {
            recordFill(*aggressor, trade.trade_qty, trade.trade_price, false, FillSource::AGGRESSIVE);
        } else if (StrategyOrder* maker = strategyAt(resting_id)) {
            recordFill(*maker, trade.trade_qty, trade.trade_price, true, FillSource::CROSSING_ADD);
        }
    }
    return left;
}

// --- venue records -----------------------------------------------------------

void Simulator::venueAdd(uint64_t venue_id, OrderSide side, int price, int size) {
    if (const auto it = venue.find(venue_id); it != venue.end()) [[unlikely]] {
        stats.venue.duplicate_add += 1;
        if (it->second.engine_id) book.cancelOrder(it->second.engine_id);
        venue.erase(it);
    }
    // The venue's own book never crosses, so anything this add crosses is the
    // strategy's or an orphan, and it trades as it would have on arrival.
    const int left = cross(side, price, size, nullptr);
    stats.crossing_add_qty += size - left;
    // Without passive impact the venue order rests as recorded, whatever it filled here.
    const int rests = config.passive_impact ? left : size;
    const order_id_t engine_id = rests > 0 ? book.addOrder(price, rests, OrderType::LIMIT, side) : 0;
    venue.emplace(venue_id, VenueOrder{engine_id, side, price, size, 0});
    stats.venue.adds += 1;
}

void Simulator::venueFill(uint64_t venue_id, int size) {
    stats.venue.fills += 1;
    const auto it = venue.find(venue_id);
    if (it == venue.end()) [[unlikely]] {
        stats.venue.unknown_order += 1;
        return;
    }
    VenueOrder& v = it->second;
    v.pending_exec += size;

    // Snapshot the queue: reducing orders below can retire the level.
    const auto queue = book.getLevelQueue(v.side, v.price);
    queue_scratch.assign(queue.begin(), queue.end());
    const auto named = v.engine_id ? std::find(queue_scratch.begin(), queue_scratch.end(), v.engine_id)
                                   : queue_scratch.end();

    int left = size;
    // 1. Strategy orders queued ahead of the named order would have been hit first,
    //    up to the size of the execution.
    if (named != queue_scratch.end()) {
        int budget = size;
        for (auto q = queue_scratch.begin(); q != named && budget > 0; ++q) {
            StrategyOrder* order = strategyAt(*q);
            if (!order) continue;
            const int price = order->price;
            const int take = reduce(*q, budget);
            recordFill(*order, take, price, true, FillSource::AHEAD_IN_QUEUE);
            stats.ahead_fill_qty += take;
            budget -= take;
        }
        // With passive impact the strategy's share comes out of the venue's fill;
        // without it the named order is still filled as recorded.
        if (config.passive_impact) left = budget;
    }
    // 2. The named order itself.
    if (left > 0 && v.engine_id) {
        left -= reduce(v.engine_id, left);
        if (liveQty(v.engine_id) == 0) v.engine_id = 0;
    }
    // 3. It had less left than the venue filled (the strategy took some): the real
    //    aggressor carried on down the queue at this price.
    if (left > 0) {
        const auto from = named != queue_scratch.end() ? named + 1 : queue_scratch.begin();
        for (auto q = from; q != queue_scratch.end() && left > 0; ++q) {
            if (StrategyOrder* order = strategyAt(*q)) {
                const int price = order->price;
                const int take = reduce(*q, left);
                recordFill(*order, take, price, true, FillSource::SWEEP);
                stats.sweep_qty += take;
                left -= take;
            } else {
                const int take = reduce(*q, left);
                stats.sweep_qty += take;
                left -= take;
            }
        }
    }
    stats.unfilled_venue_qty += left;
}

void Simulator::venueCancel(uint64_t venue_id, int size) {
    const auto it = venue.find(venue_id);
    if (it == venue.end()) [[unlikely]] {
        stats.venue.unknown_order += 1;
        return;
    }
    VenueOrder& v = it->second;

    // The C that follows a fill removes the filled size; venueFill already took it.
    const int exec_part = std::min(size, v.pending_exec);
    v.pending_exec -= exec_part;
    const int cancel_part = size - exec_part;
    v.venue_qty -= size;
    if (v.venue_qty < 0) [[unlikely]] {
        stats.venue.cancel_oversized += 1;
        v.venue_qty = 0;
    }

    if (v.venue_qty == 0) {
        stats.venue.cancels_full += 1;
        const int ours = liveQty(v.engine_id);
        if (cancel_part > 0) {
            // The owner cancelled what it had left: all of it goes, whatever we hold.
            if (ours > 0) book.cancelOrder(v.engine_id);
        } else if (ours > 0) {
            // Filled out at the venue, but the strategy absorbed some of those fills:
            // the rest would still be resting. Nothing will mention it again.
            stats.orphaned_orders += 1;
            stats.orphaned_qty += ours;
            orphan_ids.push_back(v.engine_id);
        }
        venue.erase(it);
        return;
    }

    stats.venue.cancels_partial += 1;
    if (cancel_part > 0) {
        const int ours = liveQty(v.engine_id);
        if (cancel_part > ours) stats.clamped_cancels += 1;
        reduce(v.engine_id, cancel_part);
        if (liveQty(v.engine_id) == 0) v.engine_id = 0;
    }
}

void Simulator::venueModify(uint64_t venue_id, OrderSide side, int price, int size) {
    const auto it = venue.find(venue_id);
    if (it == venue.end()) {
        stats.venue.modify_unknown += 1;
        if (size > 0) venueAdd(venue_id, side, price, size);
        return;
    }
    stats.venue.modifies += 1;
    VenueOrder& v = it->second;
    if (size <= 0) {
        if (v.engine_id) book.cancelOrder(v.engine_id);
        venue.erase(it);
        return;
    }
    if (side == v.side && price == v.price && size <= v.venue_qty) {
        // Shrinks in place, keeping priority.
        const int cut = v.venue_qty - size;
        if (cut > liveQty(v.engine_id)) stats.clamped_cancels += 1;
        reduce(v.engine_id, cut);
        if (liveQty(v.engine_id) == 0) v.engine_id = 0;
        v.venue_qty = size;
        return;
    }
    // Reprice, resize up or side change: loses priority, as a new order.
    if (v.engine_id) book.cancelOrder(v.engine_id);
    const int left = cross(side, price, size, nullptr);
    stats.crossing_add_qty += size - left;
    const int rests = config.passive_impact ? left : size;
    v = VenueOrder{rests > 0 ? book.addOrder(price, rests, OrderType::LIMIT, side) : order_id_t{0}, side, price, size, 0};
}

void Simulator::venueClear() {
    for (const auto& [venue_id, v] : venue) {
        if (v.engine_id) book.cancelOrder(v.engine_id);
    }
    venue.clear();
    for (order_id_t id : orphan_ids) book.cancelOrder(id);   // no-op for ones already gone
    orphan_ids.clear();
    stats.venue.clears += 1;
}

size_t Simulator::applyEvent(const MboEvents& events, size_t i) {
    const size_t n = events.count();
    while (i < n) {
        const uint8_t action = events.action[i];
        const uint8_t flags = events.flags[i];
        const uint64_t venue_id = events.order_id[i];
        const int size = static_cast<int>(events.size[i]);
        const int price = events.price[i];
        clock = std::max(clock, events.ts_recv[i]);
        stats.venue.records += 1;
        if (flags & mbo::F_LAST) stats.venue.events += 1;

        OrderSide side = OrderSide::BUY;
        switch (action) {
            case 'A':
                if (!toSide(events.side[i], side)) [[unlikely]] { stats.venue.bad_side += 1; break; }
                if (price == mbo::PRICE_UNDEF) [[unlikely]] { stats.venue.bad_price += 1; break; }
                venueAdd(venue_id, side, price, size);
                break;
            case 'C': venueCancel(venue_id, size); break;
            case 'M':
                if (!toSide(events.side[i], side)) [[unlikely]] { stats.venue.bad_side += 1; break; }
                if (price == mbo::PRICE_UNDEF) [[unlikely]] { stats.venue.bad_price += 1; break; }
                venueModify(venue_id, side, price, size);
                break;
            case 'R': venueClear(); break;
            case 'T': stats.venue.trades += 1; break;   // hidden executions have no F and change nothing
            case 'F': venueFill(venue_id, size); break;
            case 'N': stats.venue.nones += 1; break;
            default: stats.venue.unknown_action += 1; break;
        }
        ++i;
        if (flags & mbo::F_LAST) break;
    }
    return i;
}

// --- strategy ------------------------------------------------------------------

uint64_t Simulator::submit(OrderSide side, int price, int quantity) {
    StrategyOrder order{};
    order.client_id = next_client_id++;
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    order.ts_sent = clock;
    stats.orders_submitted += 1;

    // Checks the strategy's own gateway would make before sending.
    const char* reason = nullptr;
    if (clock < config.trade_start_ns || clock >= config.trade_end_ns) reason = "OUTSIDE_TRADING_WINDOW";
    else if (quantity <= 0) reason = "QUANTITY_BELOW_MIN";
    else if (price <= 0 || price % config.price_increment != 0) reason = "PRICE_INCREMENT";
    else if (config.max_position > 0) {
        const int64_t worst = side == OrderSide::BUY ? pos + open_buy_qty + quantity
                                                      : -(pos - open_sell_qty - quantity);
        if (worst > config.max_position) reason = "POSITION_LIMIT";
    }

    auto [it, inserted] = strategy_orders.emplace(order.client_id, order);
    StrategyOrder& stored = it->second;
    if (reason) {
        stored.reject_reason = reason;
        stored.status = StrategyOrderStatus::REJECTED;
        stats.orders_rejected += 1;
        return stored.client_id;
    }
    (side == OrderSide::BUY ? open_buy_qty : open_sell_qty) += quantity;
    live_ids.insert(stored.client_id);
    in_flight.push_back({clock + config.md_latency_ns + config.order_latency_ns, ActionKind::SUBMIT, stored.client_id});
    return stored.client_id;
}

bool Simulator::cancel(uint64_t client_id) {
    const auto it = strategy_orders.find(client_id);
    if (it == strategy_orders.end()) return false;
    const auto status = it->second.status;
    if (status != StrategyOrderStatus::PENDING && status != StrategyOrderStatus::OPEN) return false;
    in_flight.push_back({clock + config.md_latency_ns + config.order_latency_ns, ActionKind::CANCEL, client_id});
    return true;
}

void Simulator::cancelAll() {
    for (const uint64_t client_id : live_ids) {
        in_flight.push_back({clock + config.md_latency_ns + config.order_latency_ns, ActionKind::CANCEL, client_id});
    }
}

void Simulator::arrive(const Action& action) {
    const auto it = strategy_orders.find(action.client_id);
    if (it == strategy_orders.end()) return;
    if (action.kind == ActionKind::SUBMIT) arriveSubmit(it->second);
    else arriveCancel(it->second);
}

void Simulator::arriveSubmit(StrategyOrder& order) {
    if (order.status != StrategyOrderStatus::PENDING) return;
    order.ts_arrival = clock;

    // The venue's pre-trade checks, against the book as it is on arrival.
    const RejectReason reason = risk.checkOrder(order.price, order.quantity, book.getBestPrice(order.side));
    if (reason != RejectReason::NONE) {
        order.reject_reason = to_string(reason);
        stats.orders_rejected += 1;
        finish(order, StrategyOrderStatus::REJECTED);
        return;
    }
    // Self-trade prevention: never cross a resting order of our own.
    for (const uint64_t id : live_ids) {
        const StrategyOrder& other = strategy_orders.at(id);
        if (other.status != StrategyOrderStatus::OPEN || other.side == order.side) continue;
        const bool would_cross = order.side == OrderSide::BUY ? other.price <= order.price : other.price >= order.price;
        if (would_cross) {
            order.reject_reason = "SELF_TRADE";
            stats.orders_rejected += 1;
            finish(order, StrategyOrderStatus::REJECTED);
            return;
        }
    }

    const int left = cross(order.side, order.price, order.quantity, &order);
    if (order.status != StrategyOrderStatus::PENDING) return;   // filled in full
    // The rest of a limit order rests at its price.
    order.engine_id = book.addOrder(order.price, left, OrderType::LIMIT, order.side);
    strategy_by_engine.emplace(order.engine_id, order.client_id);
    order.status = StrategyOrderStatus::OPEN;
}

void Simulator::arriveCancel(StrategyOrder& order) {
    if (order.status == StrategyOrderStatus::OPEN) {
        book.cancelOrder(order.engine_id);
    } else if (order.status != StrategyOrderStatus::PENDING) {
        return;   // already done; the cancel misses
    }
    stats.orders_cancelled += 1;
    finish(order, StrategyOrderStatus::CANCELLED);
}

void Simulator::endOfTrading() {
    // finish() erases from live_ids, so walk a copy.
    const std::vector<uint64_t> live(live_ids.begin(), live_ids.end());
    for (const uint64_t client_id : live) {
        StrategyOrder& order = strategy_orders.at(client_id);
        if (order.status == StrategyOrderStatus::OPEN) book.cancelOrder(order.engine_id);
        stats.orders_cancelled += 1;
        finish(order, StrategyOrderStatus::CANCELLED);
    }
    in_flight.clear();
    recordEquity();
}

void Simulator::recordEquity() {
    const auto bid = book.getBestPrice(OrderSide::BUY);
    const auto ask = book.getBestPrice(OrderSide::SELL);
    if (bid && ask) last_mid_x2 = *bid + *ask;
    equity.push_back({clock, pos, cash, last_mid_x2, fee_total});
}

double Simulator::markToMarket() const {
    // cash and position x mid are both in 1e-4 $; mid_x2 is twice the mid.
    const double value_ticks = static_cast<double>(cash) + static_cast<double>(pos) * last_mid_x2 / 2.0;
    return value_ticks / 10'000.0 - fee_total;
}

void Simulator::run(const MboEvents& events, const TimerFn& on_timer) {
    if (ran) throw std::logic_error("Simulator::run can be called once");
    ran = true;
    if (!events.consistent()) throw std::invalid_argument("Simulator::run: columns of different lengths");

    const size_t n = events.count();
    size_t i = 0;
    uint64_t next_timer = config.trade_start_ns;
    next_sample = config.trade_start_ns;

    while (true) {
        const uint64_t t_event = i < n ? events.ts_recv[i] : NEVER;
        const uint64_t t_action = in_flight.empty() ? NEVER : in_flight.front().arrival;
        if (t_event == NEVER && t_action == NEVER) {
            // Out of data: nothing can happen after this.
            endOfTrading();
            return;
        }
        const uint64_t t = std::min({t_event, t_action, next_timer});
        if (t >= config.trade_end_ns) {
            clock = std::max(clock, config.trade_end_ns);
            endOfTrading();
            return;
        }
        // Equal times: an arriving action, then venue records, then the timer.
        if (t_action == t) {
            clock = std::max(clock, t_action);
            const Action action = in_flight.front();
            in_flight.pop_front();
            arrive(action);
        } else if (t_event == t) {
            i = applyEvent(events, i);
        } else {
            clock = std::max(clock, next_timer);
            if (clock >= next_sample) {
                recordEquity();
                while (next_sample <= clock) next_sample += config.pnl_sample_interval_ns;
            }
            stats.timer_calls += 1;
            on_timer(*this);
            next_timer += config.timer_interval_ns;
        }
    }
}

ValidationReport validateSimulatorAgainstMbp1(const MboEvents& mbo, const Mbp1Events& mbp, size_t max_examples) {
    Simulator sim(SimConfig{});
    ValidationReport report = validateWith(sim, mbo, mbp, max_examples);
    report.replay = sim.getStats().venue;
    return report;
}
