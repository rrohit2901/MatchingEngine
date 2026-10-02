#pragma once

#include "MarketReplayer.h"
#include "MboEvents.h"
#include "OrderBook.h"
#include "RiskManager.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Phase 2: one strategy trading into a replayed Nasdaq book.
//
// The venue's orders and the strategy's share one OrderBook, so the strategy
// takes queue positions behind real orders and can take real liquidity. The
// venue, of course, never saw the strategy, so its later records have to be
// reconciled against a book the strategy has changed:
//
//  * A venue fill (F) names the resting order it hit. If strategy orders were
//    queued ahead of that order at its price, the real aggressor would have
//    hit them first: they are filled from the venue fill's quantity, and the
//    named order is reduced only by what is left.
//  * If the named order has less than that left (because the strategy took
//    some of it), the remainder comes from the orders queued behind it at the
//    same price, as the real aggressor would have continued there.
//  * The cancel that follows every fill (measured in Phase 0: T, F, then a C of
//    exactly the filled size) is matched to the fill and does not reduce the
//    order again. A plain cancel is clamped to what the order has left.
//  * A venue order the strategy kept alive (because the strategy absorbed the
//    fills that would have consumed it) becomes an orphan once the venue thinks
//    it is gone: it keeps resting, but no venue record will mention it again.
//  * A venue add that crosses resting liquidity (only the strategy's orders or
//    orphans can be crossed) trades with it first.
//
// With no strategy orders all of this reduces to Phase 1: the book matches
// Nasdaq's mbp-1 exactly (checked by validateWith on the real data).
//
// Time. One clock, the venue's ts_recv. Three streams merge in time order:
// strategy actions arriving at the venue, venue records, and the strategy
// timer. At equal times an arriving action goes first, then venue records,
// then the timer, so a timer at T sees everything up to and including T.
// Market-data latency is folded into the action delay: a strategy that sees
// the book of time T at T + md_latency acts then, and its order reaches the
// venue at T + md_latency + order_latency.

enum class StrategyOrderStatus : uint8_t { PENDING, OPEN, FILLED, CANCELLED, REJECTED };

const char* to_string(StrategyOrderStatus status);

struct SimConfig {
    uint64_t order_latency_ns = 0;
    uint64_t md_latency_ns = 0;
    uint64_t timer_interval_ns = 10'000'000;   // 10 ms
    // The strategy is called, and may trade, only inside [trade_start, trade_end)
    // (UTC ns). At trade_end every open strategy order is cancelled and the
    // replay stops.
    uint64_t trade_start_ns = 0;
    uint64_t trade_end_ns = UINT64_MAX;
    // Strategy order prices must be a multiple of this many ticks (1e-4 $):
    // 100 = one cent, Nasdaq's minimum increment for prices of $1 and up.
    int price_increment = 100;
    // 0 = no limit. Otherwise an order is rejected if, filled in full together
    // with every open order on the same side, it would take |position| past this.
    int64_t max_position = 0;
    // Dollars; 0 = no limit. The capital the strategy has deployed on one side:
    // the shares it holds, valued at the current mid, plus every open order
    // that adds to that side, valued at its limit price. An order is rejected
    // (CAPITAL_LIMIT) if, together with all of that, it would take the side past
    // this. Orders that reduce the position (selling a long, buying back a
    // short) lower the figure, so they are never blocked by it.
    double max_capital = 0.0;
    RiskParams risk{};
    // Per share, in dollars; negative is a rebate.
    double maker_fee = 0.0;
    double taker_fee = 0.0;
    // How the strategy's passive fills affect the replayed venue orders.
    //  true (default): the execution's quantity is conserved. What the strategy
    //    received is taken out of the venue's fill, so the venue order behind it
    //    keeps that size, as it would have in reality. Once the venue retires
    //    that order, the size stays in the book as an orphan that no later record
    //    removes (counted in SimStats; ~570k shares by the close on AAPL with the
    //    example strategy).
    //  false: no impact. The execution fills the strategy order AND still
    //    happens to the venue orders exactly as recorded, so the venue's side of
    //    the book stays identical to Nasdaq's, at the cost of counting the
    //    execution twice.
    // Aggressive strategy orders always take venue liquidity, whichever is chosen.
    bool passive_impact = true;
    // Nasdaq lets an order trade with a resting order of the same firm unless
    // the firm opts in to self-match prevention. false (default): self-trades
    // execute, and both sides are booked as fills (source SELF_TRADE). true:
    // an incoming order that would cross one of the strategy's own resting
    // orders is rejected with SELF_TRADE.
    bool self_trade_prevention = false;
    // Equity sample spacing for the PnL curve.
    uint64_t pnl_sample_interval_ns = 1'000'000'000;   // 1 s
};

struct StrategyOrder {
    uint64_t client_id;
    OrderSide side;
    int price;
    int quantity;
    int filled = 0;
    StrategyOrderStatus status = StrategyOrderStatus::PENDING;
    const char* reject_reason = "";
    uint64_t ts_sent = 0;
    uint64_t ts_arrival = 0;
    order_id_t engine_id = 0;   // while resting
};

// How a strategy fill came about.
enum class FillSource : uint8_t {
    AGGRESSIVE,     // the strategy's order crossed resting liquidity
    AHEAD_IN_QUEUE, // a venue fill hit an order queued behind the strategy's
    SWEEP,          // a venue fill ran past an order the strategy had taken
    CROSSING_ADD,   // a venue add crossed the strategy's resting order
    SELF_TRADE,     // the strategy's order traded with its own resting order (both sides)
};

const char* to_string(FillSource source);

struct StrategyFill {
    uint64_t ts;
    uint64_t client_id;
    OrderSide side;
    int price;
    int quantity;
    bool maker;
    FillSource source;
};

struct EquitySample {
    uint64_t ts;
    int64_t position;
    int64_t cash_ticks;   // 1e-4 $
    int mid_x2;           // bid + ask, in ticks; 0 if either side is empty
    double fees;
};

struct SimStats {
    ReplayStats venue;                    // venue records, as counted by the Phase 1 replay
    // Strategy
    uint64_t timer_calls = 0;
    uint64_t orders_submitted = 0;
    uint64_t orders_rejected = 0;
    uint64_t orders_cancelled = 0;
    uint64_t fills = 0;
    int64_t maker_qty = 0;
    int64_t taker_qty = 0;
    // Reconciliation between the venue's records and the changed book
    int64_t ahead_fill_qty = 0;           // venue fill quantity given to strategy orders queued ahead
    int64_t sweep_qty = 0;                // venue fill quantity taken from behind a depleted order
    int64_t unfilled_venue_qty = 0;       // venue fill quantity with no liquidity left at the level
    uint64_t clamped_cancels = 0;         // venue cancels larger than what the order had left
    uint64_t orphaned_orders = 0;         // venue orders kept alive after the venue retired them
    int64_t orphaned_qty = 0;
    int64_t crossing_add_qty = 0;         // venue add quantity that traded on arrival
    int64_t self_trade_qty = 0;           // strategy quantity that traded with itself
};

class Simulator {
    public:
        using TimerFn = std::function<void(Simulator&)>;

        explicit Simulator(SimConfig config);

        // --- the replay -----------------------------------------------------
        // Replays `events` in full, calling on_timer every timer interval inside
        // the trading window. Can be called once.
        void run(const MboEvents& events, const TimerFn& on_timer);
        // Applies venue records from i through the next F_LAST and returns the
        // index after it. Exposed so the replay can be validated without a
        // strategy (validateWith); run() is the normal entry point.
        size_t applyEvent(const MboEvents& events, size_t i);

        // --- strategy actions (from the timer callback) ----------------------
        // Returns the client order id. The order reaches the venue after the
        // configured latency; until then its status is PENDING.
        // A limit order: whatever does not trade on arrival rests until filled or cancelled.
        uint64_t submit(OrderSide side, int price, int quantity);
        // Requests a cancel; it also travels with the latency. Returns false if
        // the order is unknown or already done.
        bool cancel(uint64_t client_id);
        void cancelAll();

        // --- state ------------------------------------------------------------
        uint64_t now() const { return clock; }
        const OrderBook& getBook() const { return book; }
        int64_t position() const { return pos; }
        int64_t cashTicks() const { return cash; }
        double fees() const { return fee_total; }
        // cash + position at mid, minus fees, in dollars. Needs both sides of the book.
        double markToMarket() const;
        const std::vector<StrategyFill>& fills() const { return fill_log; }
        const std::unordered_map<uint64_t, StrategyOrder>& orders() const { return strategy_orders; }
        // Client ids of PENDING and OPEN orders, oldest first.
        const std::set<uint64_t>& liveOrderIds() const { return live_ids; }
        const std::vector<EquitySample>& equityCurve() const { return equity; }
        const SimStats& getStats() const;
        const SimConfig& getConfig() const { return config; }
        // Capital deployed on one side, in dollars, as max_capital measures it
        // (position at the current mid, plus that side's open orders).
        double capitalDeployed(OrderSide side) const;

    private:
        struct VenueOrder {
            order_id_t engine_id;   // 0 once nothing of it rests in our book
            OrderSide side;
            int price;
            int venue_qty;          // what the venue believes is left
            int pending_exec;       // filled by F, not yet removed by the matching C
        };

        enum class ActionKind : uint8_t { SUBMIT, CANCEL };
        struct Action {
            uint64_t arrival;
            ActionKind kind;
            uint64_t client_id;
        };

        SimConfig config;
        RiskManager risk;
        OrderBook book;
        uint64_t clock = 0;

        std::unordered_map<uint64_t, VenueOrder> venue;          // venue order id -> state
        std::unordered_map<order_id_t, uint64_t> strategy_by_engine;
        std::unordered_map<uint64_t, StrategyOrder> strategy_orders;
        std::set<uint64_t> live_ids;
        std::deque<Action> in_flight;     // constant latency keeps this in arrival order
        uint64_t next_client_id = 1;

        int64_t pos = 0;
        int64_t cash = 0;
        double fee_total = 0.0;
        int64_t open_buy_qty = 0;         // resting + in flight, for the position limit
        int64_t open_sell_qty = 0;
        int64_t open_buy_value = 0;       // the same orders at their limit prices, 1e-4 $ x shares,
        int64_t open_sell_value = 0;      // for the capital limit

        std::vector<StrategyFill> fill_log;
        std::vector<EquitySample> equity;
        SimStats stats;

        std::vector<order_id_t> orphan_ids;   // venue orders only the replay still holds
        int last_mid_x2 = 0;
        uint64_t next_sample = 0;
        bool ran = false;

        std::vector<order_id_t> queue_scratch;
        std::vector<TradeEvent> trade_scratch;

        // venue records
        void venueAdd(uint64_t venue_id, OrderSide side, int price, int size);
        void venueCancel(uint64_t venue_id, int size);
        void venueModify(uint64_t venue_id, OrderSide side, int price, int size);
        void venueFill(uint64_t venue_id, int size);
        void venueClear();

        // strategy
        void arrive(const Action& action);
        void arriveSubmit(StrategyOrder& order);
        void arriveCancel(StrategyOrder& order);
        void finish(StrategyOrder& order, StrategyOrderStatus status);
        void endOfTrading();

        // book helpers
        int liveQty(order_id_t engine_id) const;
        // Removes up to `qty` from a resting order as an execution, keeping its
        // queue position if anything is left. Returns how much was removed.
        int reduce(order_id_t engine_id, int qty);
        // Books a fill of `order`: position, cash, fees, the fill log, and the
        // order's status once nothing is left. Does not touch the book.
        void recordFill(StrategyOrder& order, int qty, int price, bool maker, FillSource source);
        // The strategy order resting under this engine id, or nullptr.
        StrategyOrder* strategyAt(order_id_t engine_id);
        // Matches incoming quantity against the opposite side up to `price` and
        // returns what is left. `aggressor` is the strategy's order, or nullptr
        // for a venue add (then any strategy order it hits is the maker).
        int cross(OrderSide incoming, int price, int qty, StrategyOrder* aggressor);
        void recordEquity();
        // Twice the current mid in ticks; the last known one if a side is empty, 0 if never known.
        int currentMidX2() const;
        // capitalDeployed in 1e-4 $, with the position valued at `fallback_price` when no mid is known.
        double capitalTicks(OrderSide side, int fallback_price) const;
};

// Replays MBO through Simulator with no strategy and validates the book against
// mbp-1 exactly like validateAgainstMbp1 does for MarketReplayer.
struct ValidationReport;
ValidationReport validateSimulatorAgainstMbp1(const MboEvents& mbo, const Mbp1Events& mbp, size_t max_examples = 20);
