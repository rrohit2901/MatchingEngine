#include "Simulator.h"
#include "ReplayValidator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <vector>

// Phase 2: a strategy trading into the replayed book. Each test writes a short
// venue tape with explicit timestamps and a strategy that acts on the timer.
// Prices are 1e-4 $ ticks; 1'000'000 = $100.00, and strategy prices must sit on
// the one-cent grid (multiples of 100).

namespace {

constexpr int P = 1'000'000;      // $100.00
constexpr int CENT = 100;

struct Tape {
    std::vector<uint64_t> ts, oid;
    std::vector<uint8_t> action, side, flags;
    std::vector<int32_t> price;
    std::vector<uint32_t> size, seq;

    Tape& rec(uint64_t t, char a, char s, int32_t px, uint32_t sz, uint64_t id, bool last = true) {
        ts.push_back(t);
        seq.push_back(static_cast<uint32_t>(seq.size()));
        action.push_back(static_cast<uint8_t>(a));
        side.push_back(static_cast<uint8_t>(s));
        price.push_back(px);
        size.push_back(sz);
        oid.push_back(id);
        flags.push_back(last ? mbo::F_LAST : uint8_t{0});
        return *this;
    }
    // A venue execution of `qty` against resting order `id`: T, F, then the C that removes it.
    Tape& exec(uint64_t t, char aggressor, char resting, int32_t px, uint32_t qty, uint64_t id) {
        rec(t, 'T', aggressor, px, qty, 0, false);
        rec(t, 'F', resting, px, qty, id, false);
        return rec(t, 'C', resting, px, qty, id, true);
    }
    MboEvents events() const { return {ts, action, side, price, size, oid, flags, seq}; }
};

SimConfig config(uint64_t timer_ns = 100, uint64_t start_ns = 0) {
    SimConfig c;
    c.timer_interval_ns = timer_ns;
    c.trade_start_ns = start_ns;
    c.risk.max_price_book_top_deviation = 1'000'000;   // tests are not about fat fingers
    return c;
}

// Runs the tape; `act` is called once, at the first timer at or after `at`.
Simulator runWith(const Tape& tape, SimConfig cfg, uint64_t at, const std::function<void(Simulator&)>& act) {
    Simulator sim(cfg);
    bool done = false;
    sim.run(tape.events(), [&](Simulator& s) {
        if (!done && s.now() >= at) { done = true; act(s); }
    });
    return sim;
}

int restingAt(const Simulator& sim, OrderSide side, int price) {
    for (const auto& level : side == OrderSide::BUY ? sim.getBook().getBuySideView(100) : sim.getBook().getSellSideView(100)) {
        if (level.price == price) return level.quantity;
    }
    return 0;
}

const StrategyOrder& order(const Simulator& sim, uint64_t id) { return sim.orders().at(id); }

}  // namespace

TEST(Simulator, WithoutStrategyMatchesThePlainReplay) {
    Tape tape;
    tape.rec(10, 'A', 'A', P, 10, 1).rec(20, 'A', 'A', P, 5, 2).rec(30, 'A', 'B', P - CENT, 7, 3)
        .exec(40, 'B', 'A', P, 10, 1)    // takes order 1...
        .exec(40, 'B', 'A', P, 2, 2)     // ...and 2 of order 2
        .rec(50, 'C', 'B', P - CENT, 3, 3);
    Simulator sim(config());
    MarketReplayer plain;
    const auto events = tape.events();
    for (size_t i = 0, j = 0; i < events.count();) {
        i = sim.applyEvent(events, i);
        j = plain.applyEvent(events, j);
        EXPECT_EQ(sim.getBook().getBestPrice(OrderSide::BUY), plain.getBook().getBestPrice(OrderSide::BUY));
        EXPECT_EQ(sim.getBook().getBestPrice(OrderSide::SELL), plain.getBook().getBestPrice(OrderSide::SELL));
        EXPECT_EQ(restingAt(sim, OrderSide::SELL, P), plain.getBook().getTopLevel(OrderSide::SELL) ? plain.getBook().getTopLevel(OrderSide::SELL)->quantity : 0);
    }
    EXPECT_EQ(restingAt(sim, OrderSide::SELL, P), 3);
    EXPECT_EQ(sim.getStats().venue.unknown_order, 0u);
}

SimConfig withImpact() {   // the default, spelled out where a test depends on it
    SimConfig c = config();
    c.passive_impact = true;
    return c;
}

SimConfig withoutImpact() {
    SimConfig c = config();
    c.passive_impact = false;
    return c;
}

Tape aheadTape() {
    Tape tape;
    tape.rec(10, 'A', 'A', P, 10, 1)      // X: ahead of the strategy
        .rec(200, 'A', 'A', P, 10, 2)     // Y: behind the strategy
        .exec(300, 'B', 'A', P, 10, 1)    // X filled out
        .exec(300, 'B', 'A', P, 4, 2);    // 4 hit Y: the strategy is ahead of Y
    return tape;
}

TEST(Simulator, OrderAheadInQueueIsFilledWithoutTouchingTheVenueOrders) {
    uint64_t id = 0;
    const Simulator sim = runWith(aheadTape(), withoutImpact(), 100, [&](Simulator& s) { id = s.submit(OrderSide::SELL, P, 3); });
    EXPECT_EQ(order(sim, id).filled, 3);
    EXPECT_EQ(sim.fills()[0].source, FillSource::AHEAD_IN_QUEUE);
    // Without passive impact Y still loses all 4, exactly as the venue recorded.
    EXPECT_EQ(restingAt(sim, OrderSide::SELL, P), 6);
    EXPECT_EQ(sim.getStats().orphaned_orders, 0u);
}

TEST(Simulator, AheadFillsShareOneExecution) {
    // Two strategy orders ahead of Y split the 4-lot execution; they never get more.
    uint64_t a = 0, b = 0;
    const Simulator sim = runWith(aheadTape(), config(), 100, [&](Simulator& s) {
        a = s.submit(OrderSide::SELL, P, 3);
        b = s.submit(OrderSide::SELL, P, 3);
    });
    EXPECT_EQ(order(sim, a).filled, 3);
    EXPECT_EQ(order(sim, b).filled, 1);
}

TEST(Simulator, OrderAheadInQueueIsFilledByTheVenueFill) {
    uint64_t id = 0;
    const Simulator sim = runWith(aheadTape(), withImpact(), 100, [&](Simulator& s) { id = s.submit(OrderSide::SELL, P, 3); });

    EXPECT_EQ(order(sim, id).filled, 3);
    EXPECT_EQ(order(sim, id).status, StrategyOrderStatus::FILLED);
    EXPECT_EQ(sim.position(), -3);
    EXPECT_EQ(sim.cashTicks(), 3LL * P);
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_EQ(sim.fills()[0].source, FillSource::AHEAD_IN_QUEUE);
    EXPECT_TRUE(sim.fills()[0].maker);
    // Y lost only 1 (4 filled, 3 went to the strategy): 9 rest, the venue thinks 6.
    EXPECT_EQ(restingAt(sim, OrderSide::SELL, P), 9);
    EXPECT_EQ(sim.getStats().ahead_fill_qty, 3);
}

TEST(Simulator, OrderBehindTheFilledOrderIsNotFilled) {
    Tape tape;
    tape.rec(10, 'A', 'A', P, 10, 1).exec(300, 'B', 'A', P, 4, 1);
    uint64_t id = 0;
    const Simulator sim = runWith(tape, config(), 100, [&](Simulator& s) { id = s.submit(OrderSide::SELL, P, 3); });
    EXPECT_EQ(order(sim, id).filled, 0);
    EXPECT_TRUE(sim.fills().empty());
    // The venue fill took 4 from the order ahead; the strategy's 3 were cancelled at the end.
    EXPECT_EQ(restingAt(sim, OrderSide::SELL, P), 6);
    EXPECT_EQ(order(sim, id).status, StrategyOrderStatus::CANCELLED);
}

TEST(Simulator, AggressiveOrderTakesLiquidityAndLaterVenueRecordsReconcile) {
    Tape tape;
    tape.rec(10, 'A', 'A', P, 10, 1)       // X
        .rec(20, 'A', 'A', P, 10, 2)       // Y, behind X
        .exec(300, 'B', 'A', P, 4, 1)      // venue fills 4 of X, but the strategy already took X
        .rec(400, 'C', 'A', P, 6, 1);      // X's owner cancels the rest
    uint64_t id = 0;
    const Simulator sim = runWith(tape, config(), 100, [&](Simulator& s) { id = s.submit(OrderSide::BUY, P, 10); });

    ASSERT_EQ(order(sim, id).filled, 10);
    EXPECT_EQ(sim.fills()[0].source, FillSource::AGGRESSIVE);
    EXPECT_FALSE(sim.fills()[0].maker);
    EXPECT_EQ(sim.position(), 10);
    // The venue's fill of X swept on to Y (4), and X's final cancel had nothing left to remove.
    EXPECT_EQ(sim.getStats().sweep_qty, 4);
    EXPECT_EQ(restingAt(sim, OrderSide::SELL, P), 6);
    EXPECT_EQ(sim.getStats().venue.unknown_order, 0u);
}

TEST(Simulator, VenueAddCrossingAStrategyQuoteTradesWithIt) {
    Tape tape;
    tape.rec(10, 'A', 'A', P + 2 * CENT, 10, 1)   // venue ask at 100.02
        .rec(300, 'A', 'B', P + CENT, 5, 2);      // venue bid at 100.01 meets the strategy's 100.01 ask
    uint64_t id = 0;
    const Simulator sim = runWith(tape, withoutImpact(), 100, [&](Simulator& s) { id = s.submit(OrderSide::SELL, P + CENT, 3); });
    EXPECT_EQ(order(sim, id).filled, 3);
    EXPECT_EQ(sim.fills()[0].source, FillSource::CROSSING_ADD);
    EXPECT_EQ(restingAt(sim, OrderSide::BUY, P + CENT), 5);   // without impact: the add rests as recorded
    EXPECT_EQ(sim.getStats().crossing_add_qty, 3);

    const Simulator impact = runWith(tape, withImpact(), 100, [&](Simulator& s) { s.submit(OrderSide::SELL, P + CENT, 3); });
    EXPECT_EQ(restingAt(impact, OrderSide::BUY, P + CENT), 2);   // only the add's remainder rests
}

TEST(Simulator, LatencyDecidesWhetherTheOrderMakesIt) {
    Tape tape;
    tape.rec(10, 'A', 'A', P, 10, 1)
        .exec(150, 'B', 'A', P, 10, 1);   // the liquidity is gone at t=150
    auto taken = [&](uint64_t latency) {
        SimConfig c = config();
        c.order_latency_ns = latency;
        uint64_t id = 0;
        const Simulator sim = runWith(tape, c, 100, [&](Simulator& s) { id = s.submit(OrderSide::BUY, P, 5); });
        return order(sim, id).filled;
    };
    EXPECT_EQ(taken(0), 5);      // arrives at 100, before the venue execution
    EXPECT_EQ(taken(100), 0);    // arrives at 200: nothing left to take, so it just rests
}

TEST(Simulator, LimitOrderRestsWhatItCannotFill) {
    Tape tape;
    tape.rec(10, 'A', 'A', P, 3, 1).rec(1'000, 'N', 'N', 0, 0, 0);
    uint64_t id = 0;
    StrategyOrderStatus after_arrival{};
    int resting = 0;
    Simulator sim(config());
    sim.run(tape.events(), [&](Simulator& s) {
        if (s.now() == 100) id = s.submit(OrderSide::BUY, P, 5);
        if (s.now() == 200) {
            after_arrival = s.orders().at(id).status;
            resting = restingAt(s, OrderSide::BUY, P);
        }
    });
    EXPECT_EQ(after_arrival, StrategyOrderStatus::OPEN);
    EXPECT_EQ(order(sim, id).filled, 3);   // took the whole offer...
    EXPECT_EQ(resting, 2);                 // ...and rested the other 2 as the best bid
    EXPECT_EQ(order(sim, id).status, StrategyOrderStatus::CANCELLED);   // at the end of trading
}

TEST(Simulator, MarketDataLatencyAlsoDelaysTheOrder) {
    Tape tape;
    tape.rec(10, 'A', 'A', P, 10, 1).exec(150, 'B', 'A', P, 10, 1);
    SimConfig c = config();
    c.md_latency_ns = 100;
    uint64_t id = 0;
    const Simulator sim = runWith(tape, c, 100, [&](Simulator& s) { id = s.submit(OrderSide::BUY, P, 5); });
    EXPECT_EQ(order(sim, id).filled, 0);
    EXPECT_EQ(order(sim, id).ts_arrival, 200u);
}

TEST(Simulator, OrderKeptAliveByTheStrategyBecomesAnOrphan) {
    Tape tape;
    tape.rec(10, 'A', 'A', P, 10, 1)       // X
        .rec(200, 'A', 'A', P, 4, 2)       // Y, behind the strategy
        .exec(300, 'B', 'A', P, 4, 2)      // fills Y out, but the strategy is ahead of Y
        .rec(400, 'C', 'A', P, 10, 1);     // X cancelled
    uint64_t id = 0;
    const Simulator sim = runWith(tape, withImpact(), 100, [&](Simulator& s) { id = s.submit(OrderSide::SELL, P, 4); });
    EXPECT_EQ(order(sim, id).filled, 4);
    // With passive impact, Y never traded here, so it would still be resting.
    EXPECT_EQ(sim.getStats().orphaned_orders, 1u);
    EXPECT_EQ(sim.getStats().orphaned_qty, 4);
    EXPECT_EQ(restingAt(sim, OrderSide::SELL, P), 4);
}

TEST(Simulator, GatewayAndVenueRejections) {
    Tape tape;
    tape.rec(10, 'A', 'A', P + CENT, 10, 1).rec(1'000, 'N', 'N', 0, 0, 0);
    SimConfig c = config();
    c.max_position = 5;
    std::vector<uint64_t> ids;
    const Simulator sim = runWith(tape, c, 100, [&](Simulator& s) {
        ids.push_back(s.submit(OrderSide::BUY, P + 50, 1));        // off the cent grid
        ids.push_back(s.submit(OrderSide::BUY, P, 6));             // past the position limit
        ids.push_back(s.submit(OrderSide::BUY, P, 3));             // fine: rests
    });
    EXPECT_STREQ(order(sim, ids[0]).reject_reason, "PRICE_INCREMENT");
    EXPECT_STREQ(order(sim, ids[1]).reject_reason, "POSITION_LIMIT");
    EXPECT_EQ(order(sim, ids[2]).status, StrategyOrderStatus::CANCELLED);   // cancelled at end of trading
    EXPECT_EQ(sim.getStats().orders_rejected, 2u);
}

namespace {
// The strategy rests a bid at 100.00, then sends a sell at 100.00 that crosses it.
Simulator selfCross(SimConfig c, uint64_t& bid, uint64_t& sell) {
    Tape tape;
    tape.rec(10, 'A', 'A', P + CENT, 10, 1).rec(1'000, 'N', 'N', 0, 0, 0);
    c.maker_fee = -0.002;
    c.taker_fee = 0.003;
    int calls = 0;
    Simulator sim(c);
    sim.run(tape.events(), [&](Simulator& s) {
        ++calls;
        if (calls == 1) bid = s.submit(OrderSide::BUY, P, 3);
        if (calls == 2) sell = s.submit(OrderSide::SELL, P, 2);
    });
    return sim;
}
}  // namespace

TEST(Simulator, SelfTradeExecutesByDefault) {
    uint64_t bid = 0, sell = 0;
    const Simulator sim = selfCross(config(), bid, sell);

    EXPECT_EQ(order(sim, sell).status, StrategyOrderStatus::FILLED);
    EXPECT_EQ(order(sim, bid).filled, 2);
    ASSERT_EQ(sim.fills().size(), 2u);
    EXPECT_EQ(sim.fills()[0].source, FillSource::SELF_TRADE);
    EXPECT_FALSE(sim.fills()[0].maker);
    EXPECT_EQ(sim.fills()[1].source, FillSource::SELF_TRADE);
    EXPECT_TRUE(sim.fills()[1].maker);
    // Bought and sold 2 at the same price: flat, no cash moved, both fees paid.
    EXPECT_EQ(sim.position(), 0);
    EXPECT_EQ(sim.cashTicks(), 0);
    EXPECT_NEAR(sim.fees(), 2 * (0.003 - 0.002), 1e-12);
    EXPECT_EQ(sim.getStats().self_trade_qty, 2);
}

TEST(Simulator, SelfTradePreventionIsOptIn) {
    SimConfig c = config();
    c.self_trade_prevention = true;
    uint64_t bid = 0, sell = 0;
    const Simulator sim = selfCross(c, bid, sell);

    EXPECT_STREQ(order(sim, sell).reject_reason, "SELF_TRADE");
    EXPECT_EQ(order(sim, bid).filled, 0);
    EXPECT_TRUE(sim.fills().empty());
}

TEST(Simulator, CancelTravelsWithLatency) {
    Tape tape;
    tape.rec(10, 'A', 'B', P, 10, 1).rec(5'000, 'N', 'N', 0, 0, 0);
    SimConfig c = config(1'000);
    c.order_latency_ns = 50;
    uint64_t id = 0;
    int calls = 0;
    Simulator sim(c);
    sim.run(tape.events(), [&](Simulator& s) {
        ++calls;
        if (calls == 1) id = s.submit(OrderSide::SELL, P + CENT, 2);
        if (calls == 2) {
            EXPECT_EQ(s.orders().at(id).status, StrategyOrderStatus::OPEN);
            EXPECT_TRUE(s.cancel(id));
            EXPECT_EQ(s.orders().at(id).status, StrategyOrderStatus::OPEN);   // still in flight
        }
        if (calls == 3) { EXPECT_EQ(s.orders().at(id).status, StrategyOrderStatus::CANCELLED); }
    });
    EXPECT_GE(calls, 3);
}

TEST(Simulator, TradingWindowAndMarkToMarket) {
    Tape tape;
    tape.rec(10, 'A', 'B', P - CENT, 10, 1).rec(10, 'A', 'A', P + CENT, 10, 2)
        .rec(5'000, 'N', 'N', 0, 0, 0);
    SimConfig c = config(1'000, 2'000);
    c.trade_end_ns = 4'000;
    std::vector<uint64_t> seen;
    Simulator sim(c);
    sim.run(tape.events(), [&](Simulator& s) {
        seen.push_back(s.now());
        if (seen.size() == 1) s.submit(OrderSide::BUY, P + CENT, 4);   // lift the offer
    });
    EXPECT_EQ(seen, (std::vector<uint64_t>{2'000, 3'000}));
    EXPECT_EQ(sim.position(), 4);
    // Bought 4 at 100.01, marked at the 100.00 mid: -0.04.
    EXPECT_NEAR(sim.markToMarket(), -0.04, 1e-9);
}

TEST(Simulator, MarkToMarketUsesTheCurrentMidBetweenSamples) {
    Tape tape;
    tape.rec(10, 'A', 'B', P - CENT, 10, 1).rec(10, 'A', 'A', P + CENT, 10, 2)
        .rec(1'500, 'C', 'A', P + CENT, 6, 2)            // the strategy took 4; the rest leaves
        .rec(1'500, 'A', 'A', P + 5 * CENT, 10, 3)       // the ask moves up: mid 100.02
        .rec(5'000, 'N', 'N', 0, 0, 0);
    SimConfig c = config(1'000, 1'000);
    c.pnl_sample_interval_ns = 1'000'000;                // one equity sample, at the start
    std::vector<double> pnl;
    Simulator sim(c);
    sim.run(tape.events(), [&](Simulator& s) {
        if (s.now() == 1'000) s.submit(OrderSide::BUY, P + CENT, 4);
        pnl.push_back(s.markToMarket());
    });
    ASSERT_GE(pnl.size(), 2u);
    // Bought 4 at 100.01; at 2'000 the mid is 100.02, not the 100.00 of the last sample.
    EXPECT_NEAR(pnl[1], 0.04, 1e-9);
}

TEST(Simulator, NonIntegerQuantityIsRecordedAsRejected) {
    Tape tape;
    tape.rec(10, 'A', 'B', P - CENT, 10, 1).rec(10, 'A', 'A', P + CENT, 10, 2).rec(5'000, 'N', 'N', 0, 0, 0);
    uint64_t id = 0;
    const Simulator sim = runWith(tape, config(1'000), 0, [&](Simulator& s) {
        id = s.rejectNonIntegerQuantity(OrderSide::BUY, P + CENT, 4);
    });
    EXPECT_EQ(order(sim, id).status, StrategyOrderStatus::REJECTED);
    EXPECT_STREQ(order(sim, id).reject_reason, "QUANTITY_NOT_INTEGER");
    EXPECT_EQ(sim.position(), 0);
    EXPECT_EQ(sim.getStats().orders_rejected, 1u);
    EXPECT_TRUE(sim.liveOrderIds().empty());
}

TEST(Simulator, ValidatorRunsOnTheSimulator) {
    Tape tape;
    tape.rec(10, 'A', 'B', P, 10, 1).rec(20, 'A', 'A', P + CENT, 4, 2).exec(30, 'B', 'A', P + CENT, 4, 2);
    std::vector<uint64_t> ts{10, 20, 30};
    std::vector<uint32_t> seq{0, 1, 4}, bsz{10, 10, 10}, asz{0, 4, 0};
    std::vector<uint8_t> flags{mbo::F_LAST, mbo::F_LAST, mbo::F_LAST};
    std::vector<int32_t> bpx{P, P, P}, apx{mbo::PRICE_UNDEF, P + CENT, mbo::PRICE_UNDEF};
    const auto report = validateSimulatorAgainstMbp1(tape.events(), {ts, seq, flags, bpx, apx, bsz, asz});
    EXPECT_EQ(report.compared, 3u);
    EXPECT_EQ(report.mismatched, 0u);
}

TEST(Simulator, CapitalLimitCountsOpenOrdersAtTheirPrice) {
    Tape tape;
    tape.rec(10, 'A', 'B', P - CENT, 10, 1).rec(10, 'A', 'A', P + CENT, 10, 2).rec(1'000, 'N', 'N', 0, 0, 0);
    SimConfig c = config();
    c.max_capital = 1'000.0;   // dollars
    std::vector<uint64_t> ids;
    const Simulator sim = runWith(tape, c, 100, [&](Simulator& s) {
        ids.push_back(s.submit(OrderSide::BUY, P, 9));          // $900 resting: fine
        ids.push_back(s.submit(OrderSide::BUY, P, 2));          // +$200 = $1,100: over
        ids.push_back(s.submit(OrderSide::SELL, P + CENT, 5));  // the sell side has its own $1,000
    });
    EXPECT_NE(order(sim, ids[0]).status, StrategyOrderStatus::REJECTED);
    EXPECT_STREQ(order(sim, ids[1]).reject_reason, "CAPITAL_LIMIT");
    EXPECT_NE(order(sim, ids[2]).status, StrategyOrderStatus::REJECTED);
}

TEST(Simulator, CapitalLimitValuesThePositionAtTheCurrentMid) {
    Tape tape;
    tape.rec(10, 'A', 'B', P - CENT, 10, 1).rec(10, 'A', 'A', P + CENT, 10, 2)
        // The market moves up $2: both sides requote.
        .rec(150, 'C', 'B', P - CENT, 10, 1, false).rec(150, 'C', 'A', P + CENT, 10, 2, false)
        .rec(150, 'A', 'B', P + 199 * CENT, 10, 3, false).rec(150, 'A', 'A', P + 201 * CENT, 10, 4, true)
        .rec(1'000, 'N', 'N', 0, 0, 0);
    SimConfig c = config();
    c.max_capital = 1'010.0;
    int calls = 0;
    uint64_t buy = 0, more = 0, sell = 0;
    double deployed_before = 0, deployed_after = 0;
    Simulator sim(c);
    sim.run(tape.events(), [&](Simulator& s) {
        ++calls;
        if (calls == 2) buy = s.submit(OrderSide::BUY, P + CENT, 8);   // t=100: lifts 8 at 100.01
        if (calls == 2) deployed_before = s.capitalDeployed(OrderSide::BUY);
        if (calls == 3) {                                              // t=200: mid is now 102.00
            deployed_after = s.capitalDeployed(OrderSide::BUY);
            // At the old 100.00 mid this would be 800 + 202 = $1,002, inside the limit;
            // at the current mid it is 816 + 202 = $1,018.
            more = s.submit(OrderSide::BUY, P + 101 * CENT, 2);
            sell = s.submit(OrderSide::SELL, P + 201 * CENT, 8);       // reduces the long: never blocked
        }
    });
    EXPECT_EQ(order(sim, buy).filled, 8);
    EXPECT_NEAR(deployed_before, 800.08, 1e-9);  // in flight: counted at its limit price, 8 x 100.01...
    EXPECT_NEAR(deployed_after, 816.0, 1e-9);    // ...and once filled, 8 shares at the 102.00 mid
    EXPECT_STREQ(order(sim, more).reject_reason, "CAPITAL_LIMIT");
    EXPECT_NE(order(sim, sell).status, StrategyOrderStatus::REJECTED);
}
