#include "MatchingEngine.h"
#include "lock_queue.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

// MatchingEngine is OrderBook plus automatic matching: every addOrder and
// modifyOrder runs the Matcher, so a crossing order trades on arrival instead of
// resting. It exposes no per-order accessors, so these tests observe the book
// through the level views.
//
// It also publishes an event per operation into the container it is constructed
// with. Tests hand it a real LockQueue and drain that queue to assert on what
// was published; nothing here writes to a file.

namespace {

using Levels = std::vector<std::shared_ptr<BookLevel>>;
using EventQueue = LockQueue<EventVariant>;

int restingQuantity(const Levels& levels) {
    int total = 0;
    for (const auto& level : levels) {
        total += level->getTotalQuantity();
    }
    return total;
}

std::size_t restingOrders(const Levels& levels) {
    std::size_t count = 0;
    for (const auto& level : levels) {
        count += level->getOrders().size();
    }
    return count;
}

class MatchingEngineTest : public ::testing::Test {
  protected:
    std::shared_ptr<EventQueue> queue{std::make_shared<EventQueue>()};
    MatchingEngine<LockQueue> me{queue};

    // Pops everything published so far. The engine emits SESSION_OPEN from its
    // constructor, so that is always the first entry.
    std::vector<EventVariant> drainEvents() {
        std::vector<EventVariant> events;
        while (auto popped = queue->try_pop()) {
            events.push_back(std::move(*popped));
        }
        return events;
    }

    // event_type() reads the variant's active alternative; it replaced the
    // event_type member the old polymorphic base carried.
    std::vector<EventTypes> drainEventTypes() {
        std::vector<EventTypes> types;
        for (const auto& event : drainEvents()) {
            types.push_back(event_type(event));
        }
        return types;
    }

    std::vector<TradeEvent> drainTrades() {
        std::vector<TradeEvent> trades;
        for (const auto& event : drainEvents()) {
            if (const auto* trade = std::get_if<TradeEvent>(&event)) {
                trades.push_back(*trade);
            }
        }
        return trades;
    }
};

} // namespace

TEST_F(MatchingEngineTest, EmptyBook) {
    EXPECT_TRUE(me.getBuySideView(5).empty());
    EXPECT_TRUE(me.getSellSideView(5).empty());

    const auto [bids, asks] = me.getOrderBookView(5);
    EXPECT_TRUE(bids.empty());
    EXPECT_TRUE(asks.empty());
}

TEST_F(MatchingEngineTest, NonCrossingOrdersRest) {
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(101, 5, OrderType::LIMIT, OrderSide::SELL);

    const auto bids = me.getBuySideView(5);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids.front()->getPrice(), 100);
    EXPECT_EQ(bids.front()->getTotalQuantity(), 10);

    const auto asks = me.getSellSideView(5);
    ASSERT_EQ(asks.size(), 1u);
    EXPECT_EQ(asks.front()->getPrice(), 101);
    EXPECT_EQ(asks.front()->getTotalQuantity(), 5);
}

TEST_F(MatchingEngineTest, CrossingOrderTradesOnArrival) {
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // Both sides are fully consumed, so no quantity and no live order remains.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 0);
    EXPECT_EQ(restingOrders(me.getBuySideView(5)), 0u);
    EXPECT_EQ(restingOrders(me.getSellSideView(5)), 0u);
}

TEST_F(MatchingEngineTest, PartialFillLeavesRemainderResting) {
    me.addOrder(100, 4, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // The 4-lot ask is consumed; 6 of the buy's 10 stay on the bid at 100.
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 6);
    EXPECT_EQ(restingOrders(me.getBuySideView(5)), 1u);
}

TEST_F(MatchingEngineTest, RestingOrderAbsorbsSmallerAggressor) {
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 4, OrderType::LIMIT, OrderSide::BUY);

    // The aggressing buy is fully filled; the ask keeps its 6 unfilled lots.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 6);
}

TEST_F(MatchingEngineTest, IncomingSellCrossesRestingBid) {
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);

    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 0);
}

TEST_F(MatchingEngineTest, OrderDoesNotTradeThroughItsLimit) {
    me.addOrder(102, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // 100 does not reach a 102 ask, so both orders rest untouched.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 10);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 10);
}

TEST_F(MatchingEngineTest, AggressorSweepsBestPriceFirst) {
    me.addOrder(102, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(102, 5, OrderType::LIMIT, OrderSide::BUY);

    // The buy can pay up to 102 but only needs 5 lots, so it takes the 100 ask
    // and leaves the 102 ask alone.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);

    const auto asks = me.getSellSideView(5);
    EXPECT_EQ(restingQuantity(asks), 5);
    ASSERT_FALSE(asks.empty());
    EXPECT_EQ(asks.back()->getPrice(), 102);
}

TEST_F(MatchingEngineTest, AggressorSweepsMultipleLevels) {
    me.addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(101, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(101, 8, OrderType::LIMIT, OrderSide::BUY);

    // 8 lots clear the 100 level entirely and 3 of the 101 level.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 2);
}

TEST_F(MatchingEngineTest, CancelOrder) {
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).value();

    EXPECT_TRUE(me.cancelOrder(id));
    EXPECT_TRUE(me.getBuySideView(5).empty());
    EXPECT_FALSE(me.cancelOrder(id)); // already gone
}

TEST_F(MatchingEngineTest, CancelUnknownOrderFails) {
    EXPECT_FALSE(me.cancelOrder(9999));
}

TEST_F(MatchingEngineTest, ModifyOrderRequotesToNewPrice) {
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).value();

    const auto requoted = me.modifyOrder(id, 10, 99, OrderSide::BUY, OrderType::LIMIT);
    ASSERT_TRUE(requoted.has_value());

    const auto bids = me.getBuySideView(5);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids.front()->getPrice(), 99);
    EXPECT_EQ(bids.front()->getTotalQuantity(), 10);
}

TEST_F(MatchingEngineTest, ModifyUnknownOrderReturnsNullopt) {
    EXPECT_FALSE(me.modifyOrder(9999, 10, 100, OrderSide::BUY, OrderType::LIMIT).has_value());
}

TEST_F(MatchingEngineTest, ModifyIntoACrossTrades) {
    me.addOrder(101, 10, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).value();
    ASSERT_EQ(restingQuantity(me.getSellSideView(5)), 10); // no cross yet

    // Repricing the bid up to the ask must match, not just move the order.
    const auto requoted = me.modifyOrder(id, 10, 101, OrderSide::BUY, OrderType::LIMIT);
    ASSERT_TRUE(requoted.has_value());
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
}

TEST_F(MatchingEngineTest, BookViewIsPriceOrderedAndCapped) {
    me.addOrder(98, 1, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(100, 1, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(99, 1, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(103, 1, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(101, 1, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(102, 1, OrderType::LIMIT, OrderSide::SELL);

    const auto [bids, asks] = me.getOrderBookView(3);
    ASSERT_EQ(bids.size(), 3u);
    ASSERT_EQ(asks.size(), 3u);
    EXPECT_EQ(bids[0]->getPrice(), 100); // best bid is the highest
    EXPECT_EQ(bids[1]->getPrice(), 99);
    EXPECT_EQ(bids[2]->getPrice(), 98);
    EXPECT_EQ(asks[0]->getPrice(), 101); // best ask is the lowest
    EXPECT_EQ(asks[1]->getPrice(), 102);
    EXPECT_EQ(asks[2]->getPrice(), 103);

    EXPECT_EQ(me.getBuySideView(2).size(), 2u);
    EXPECT_EQ(me.getSellSideView(1).size(), 1u);
}

TEST_F(MatchingEngineTest, FullyFilledAggressorLeavesNoEmptyLevel) {
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // A level with nothing resting on it must not occupy a slot in the depth
    // view, otherwise getOrderBookView(n) reports phantom depth.
    EXPECT_TRUE(me.getSellSideView(5).empty());
    EXPECT_TRUE(me.getBuySideView(5).empty());
}

// --- event publishing ------------------------------------------------------
// The engine's whole reason for holding a queue: every operation must leave a
// record for the logger thread, without the engine ever touching a file.

TEST_F(MatchingEngineTest, ConstructionPublishesSessionOpen) {
    const auto types = drainEventTypes();
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], EventTypes::SESSION_OPEN);
}

TEST_F(MatchingEngineTest, DestructionPublishesSessionClose) {
    std::vector<EventTypes> types;
    {
        auto scoped_queue = std::make_shared<EventQueue>();
        { MatchingEngine<LockQueue> scoped{scoped_queue}; }  // open, then close
        while (auto popped = scoped_queue->try_pop()) {
            types.push_back(event_type(*popped));
        }
    }
    ASSERT_EQ(types.size(), 2u);
    EXPECT_EQ(types[0], EventTypes::SESSION_OPEN);
    EXPECT_EQ(types[1], EventTypes::SESSION_CLOSE);
}

TEST_F(MatchingEngineTest, LimitOrderPublishesLimitAddEvent) {
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    const auto types = drainEventTypes();
    ASSERT_EQ(types.size(), 2u);
    EXPECT_EQ(types[1], EventTypes::LIMIT_ORDER_ADDED);
}

TEST_F(MatchingEngineTest, MarketOrderPublishesMarketAddEvent) {
    me.addOrder(100, 10, OrderType::MARKET, OrderSide::BUY);

    // The add event reflects the order type rather than always saying LIMIT.
    const auto types = drainEventTypes();
    ASSERT_EQ(types.size(), 2u);
    EXPECT_EQ(types[1], EventTypes::MARKET_ORDER_ADDED);
}

TEST_F(MatchingEngineTest, CancelPublishesCancelEvent) {
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).value();
    ASSERT_TRUE(me.cancelOrder(id));

    const auto types = drainEventTypes();
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[2], EventTypes::ORDER_CANCELLED);
}

TEST_F(MatchingEngineTest, FailedCancelPublishesNothing) {
    EXPECT_FALSE(me.cancelOrder(9999));

    // Only SESSION_OPEN; a cancel that did nothing must not be logged as one.
    const auto types = drainEventTypes();
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], EventTypes::SESSION_OPEN);
}

TEST_F(MatchingEngineTest, ModifyPublishesModifyEvent) {
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).value();
    ASSERT_TRUE(me.modifyOrder(id, 5, 99, OrderSide::BUY, OrderType::LIMIT).has_value());

    const auto types = drainEventTypes();
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[2], EventTypes::ORDER_MODIFIED);
}

TEST_F(MatchingEngineTest, ModifyForUnknownOrderPublishesRejectEvent) {
    EXPECT_FALSE(me.modifyOrder(9999, 10, 100, OrderSide::BUY, OrderType::LIMIT).has_value());

    // Passes risk, but there is no such order: still a rejected request, and it
    // must be logged rather than silently dropped.
    const auto types = drainEventTypes();
    ASSERT_EQ(types.size(), 2u);
    EXPECT_EQ(types[1], EventTypes::ORDER_MODIFY_REJECTED);
}

TEST_F(MatchingEngineTest, MatchPublishesTradeEvent) {
    const order_id_t sell_id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL).value();
    const order_id_t buy_id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).value();

    const auto trades = drainTrades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_id, buy_id);
    EXPECT_EQ(trades[0].sell_id, sell_id);
    EXPECT_EQ(trades[0].trade_qty, 10);
    EXPECT_EQ(trades[0].trade_price, 100);
}

TEST_F(MatchingEngineTest, SweepPublishesOneTradePerLevelConsumed) {
    me.addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(101, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(101, 8, OrderType::LIMIT, OrderSide::BUY);

    // 8 lots clear the 100 level and take 3 from the 101 level: two fills.
    const auto trades = drainTrades();
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].trade_price, 100);
    EXPECT_EQ(trades[0].trade_qty, 5);
    EXPECT_EQ(trades[1].trade_price, 101);
    EXPECT_EQ(trades[1].trade_qty, 3);
}

TEST_F(MatchingEngineTest, NonCrossingOrderPublishesNoTrade) {
    me.addOrder(102, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_TRUE(drainTrades().empty());
}

// --- risk checks -----------------------------------------------------------
// addOrder/modifyOrder return nullopt when the pre-trade risk layer rejects,
// and a rejected order must leave no trace: no book change and no event.

TEST_F(MatchingEngineTest, RejectsZeroQuantityOrder) {
    EXPECT_FALSE(me.addOrder(100, 0, OrderType::LIMIT, OrderSide::BUY).has_value());
    EXPECT_TRUE(me.getBuySideView(5).empty());
}

TEST_F(MatchingEngineTest, RejectsNegativeQuantityOrder) {
    // Previously this booked a level with total_quantity == -5.
    EXPECT_FALSE(me.addOrder(100, -5, OrderType::LIMIT, OrderSide::BUY).has_value());
    EXPECT_TRUE(me.getBuySideView(5).empty());
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
}

TEST_F(MatchingEngineTest, RejectsQuantityAboveLimit) {
    EXPECT_FALSE(me.addOrder(100, 1000000, OrderType::LIMIT, OrderSide::BUY).has_value());
    EXPECT_TRUE(me.getBuySideView(5).empty());
}

TEST_F(MatchingEngineTest, RejectsPriceFarFromTopOfBook) {
    ASSERT_TRUE(me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).has_value());

    // Default deviation is 1000; 100 + 5000 is well outside it.
    EXPECT_FALSE(me.addOrder(5100, 10, OrderType::LIMIT, OrderSide::BUY).has_value());
    EXPECT_EQ(me.getBuySideView(5).size(), 1u);
}

TEST_F(MatchingEngineTest, FirstOrderIsNotPriceCheckedAgainstAnEmptyBook) {
    // No top of book yet, so there is nothing to deviate from.
    EXPECT_TRUE(me.addOrder(9999999, 10, OrderType::LIMIT, OrderSide::BUY).has_value());
}

TEST_F(MatchingEngineTest, RejectedOrderPublishesRejectNotAdd) {
    ASSERT_FALSE(me.addOrder(100, 0, OrderType::LIMIT, OrderSide::BUY).has_value());

    // A rejected order is logged as a reject, never as an add.
    const auto types = drainEventTypes();
    ASSERT_EQ(types.size(), 2u);
    EXPECT_EQ(types[1], EventTypes::ORDER_REJECTED);
}

TEST_F(MatchingEngineTest, RejectedModifyLeavesOrderUntouched) {
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).value();

    EXPECT_FALSE(me.modifyOrder(id, 0, 100, OrderSide::BUY, OrderType::LIMIT).has_value());

    const auto bids = me.getBuySideView(5);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids.front()->getTotalQuantity(), 10); // unchanged
}

TEST_F(MatchingEngineTest, CustomRiskParamsAreApplied) {
    auto q = std::make_shared<EventQueue>();
    RiskParams params;
    params.max_allowed_quantity_quote = 50;
    MatchingEngine<LockQueue> strict{q, params};

    EXPECT_TRUE(strict.addOrder(100, 50, OrderType::LIMIT, OrderSide::BUY).has_value());
    EXPECT_FALSE(strict.addOrder(100, 51, OrderType::LIMIT, OrderSide::BUY).has_value());
}

// --- reject events ---------------------------------------------------------
// A reject is the only record that a request ever arrived, so it has to carry
// enough to act on: what was asked for, and why it was refused.

TEST_F(MatchingEngineTest, RejectEventCarriesRequestAndReason) {
    ASSERT_FALSE(me.addOrder(100, 0, OrderType::LIMIT, OrderSide::BUY).has_value());

    const auto events = drainEvents();
    ASSERT_EQ(events.size(), 2u);
    const auto* reject = std::get_if<OrderRejected>(&events[1]);
    ASSERT_NE(reject, nullptr);
    EXPECT_EQ(reject->price, 100);
    EXPECT_EQ(reject->qty, 0);
    EXPECT_EQ(reject->side, OrderSide::BUY);
    EXPECT_EQ(reject->type, OrderType::LIMIT);
    EXPECT_STREQ(reject->reason, "QUANTITY_BELOW_MIN");
}

TEST_F(MatchingEngineTest, RejectReasonDistinguishesQuantityFromPrice) {
    ASSERT_FALSE(me.addOrder(100, 1000000, OrderType::LIMIT, OrderSide::BUY).has_value());
    {
        const auto events = drainEvents();
        ASSERT_EQ(events.size(), 2u);
        const auto* reject = std::get_if<OrderRejected>(&events[1]);
        ASSERT_NE(reject, nullptr);
        EXPECT_STREQ(reject->reason, "QUANTITY_ABOVE_MAX");
    }

    ASSERT_TRUE(me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).has_value());
    ASSERT_FALSE(me.addOrder(9100, 10, OrderType::LIMIT, OrderSide::BUY).has_value());
    {
        const auto events = drainEvents();
        const auto* reject = std::get_if<OrderRejected>(&events.back());
        ASSERT_NE(reject, nullptr);
        EXPECT_STREQ(reject->reason, "PRICE_TOO_FAR_FROM_TOP");
    }
}

TEST_F(MatchingEngineTest, MarketOrderRejectRecordsItsType) {
    ASSERT_FALSE(me.addOrder(100, 0, OrderType::MARKET, OrderSide::SELL).has_value());

    const auto events = drainEvents();
    const auto* reject = std::get_if<OrderRejected>(&events.back());
    ASSERT_NE(reject, nullptr);
    EXPECT_EQ(reject->type, OrderType::MARKET);
    EXPECT_EQ(reject->side, OrderSide::SELL);
}

TEST_F(MatchingEngineTest, ModifyRejectCarriesTargetOrderAndReason) {
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).value();
    ASSERT_FALSE(me.modifyOrder(id, 0, 100, OrderSide::BUY, OrderType::LIMIT).has_value());

    const auto events = drainEvents();
    const auto* reject = std::get_if<OrderModifyRejected>(&events.back());
    ASSERT_NE(reject, nullptr);
    // The targeted order is still resting under this id, untouched.
    EXPECT_EQ(reject->order_id, id);
    EXPECT_EQ(reject->new_qty, 0);
    EXPECT_EQ(reject->new_price, 100);
    EXPECT_STREQ(reject->reason, "QUANTITY_BELOW_MIN");
}

TEST_F(MatchingEngineTest, ModifyRejectForUnknownOrderSaysNotFound) {
    ASSERT_FALSE(me.modifyOrder(9999, 10, 100, OrderSide::BUY, OrderType::LIMIT).has_value());

    const auto events = drainEvents();
    const auto* reject = std::get_if<OrderModifyRejected>(&events.back());
    ASSERT_NE(reject, nullptr);
    EXPECT_EQ(reject->order_id, 9999u);
    EXPECT_STREQ(reject->reason, "ORDER_NOT_FOUND");
}

TEST_F(MatchingEngineTest, AcceptedOrderPublishesNoReject) {
    ASSERT_TRUE(me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY).has_value());

    for (const auto& event : drainEvents()) {
        EXPECT_NE(event_type(event), EventTypes::ORDER_REJECTED);
        EXPECT_NE(event_type(event), EventTypes::ORDER_MODIFY_REJECTED);
    }
}
