#include "gtest/gtest.h"
#include "Order.h"
#include "OrderBook.h"
#include "Matcher.h"
#include "lock_queue.h"

#include <memory>

// Matcher is now a template over the event container, and it needs an
// EventManager to publish the trades it produces. Tests wire it to a real
// LockQueue and drain the queue to assert on what was published.
//
// OrderBook::getOrder() is gone; order state is read through the optional
// accessors, and an order that has left the book reports IsOrderValid() == false.
//
// Fill semantics under test:
//   - an order that is completely filled leaves the book;
//   - an order that is only partially filled keeps resting with the quantity
//     that did NOT fill.

namespace {

using EventQueue = LockQueue<EventVariant>;

::testing::AssertionResult IsGone(OrderBook& book, order_id_t order_id) {
    if (!book.IsOrderValid(order_id)) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "order " << order_id << " still in book with quantity "
           << book.getOrderQuantity(order_id).value_or(-1);
}

class MatcherTest : public ::testing::Test {
  protected:
    std::shared_ptr<OrderBook> order_book{std::make_shared<OrderBook>()};
    std::shared_ptr<EventQueue> queue{std::make_shared<EventQueue>()};
    std::shared_ptr<EventManager<LockQueue>> event_manager{
        std::make_shared<EventManager<LockQueue>>(queue)};
    Matcher<LockQueue> matcher{order_book, event_manager};

    // Every trade the matcher published, popped back off the queue. Events are
    // values in a variant now, so picking the trades out is get_if rather than
    // the dynamic_cast the old polymorphic hierarchy needed.
    std::vector<TradeEvent> drainTrades() {
        std::vector<TradeEvent> trades;
        while (auto popped = queue->try_pop()) {
            if (const auto* trade = std::get_if<TradeEvent>(&*popped)) {
                trades.push_back(*trade);
            }
        }
        return trades;
    }
};

} // namespace

TEST_F(MatcherTest, FullMatchEqualQuantity) {
    const order_id_t sell_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_TRUE(matcher.tryMatch(buy_id)); // incoming buy fully filled
    EXPECT_TRUE(IsGone(*order_book, buy_id));
    EXPECT_TRUE(IsGone(*order_book, sell_id));

    const auto trades = drainTrades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_id, buy_id);
    EXPECT_EQ(trades[0].sell_id, sell_id);
    EXPECT_EQ(trades[0].trade_qty, 10);
    EXPECT_EQ(trades[0].trade_price, 100);
}

TEST_F(MatcherTest, PartialMatchIncomingLarger) {
    const order_id_t sell_id = order_book->addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_FALSE(matcher.tryMatch(buy_id)); // 5 remaining, not fully filled
    EXPECT_TRUE(IsGone(*order_book, sell_id)); // resting sell consumed

    // The buy rests with its UNFILLED remainder, not the amount that traded.
    ASSERT_TRUE(order_book->IsOrderValid(buy_id))
        << "partially filled buy must keep resting in the book";
    EXPECT_EQ(order_book->getOrderQuantity(buy_id).value(), 5);

    const auto trades = drainTrades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].trade_qty, 5); // only what actually traded
}

TEST_F(MatcherTest, PartialMatchRestingLarger) {
    const order_id_t sell_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 5, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_TRUE(matcher.tryMatch(buy_id)); // incoming buy fully filled
    EXPECT_TRUE(IsGone(*order_book, buy_id));

    ASSERT_TRUE(order_book->IsOrderValid(sell_id))
        << "partially filled sell must keep resting in the book";
    EXPECT_EQ(order_book->getOrderQuantity(sell_id).value(), 5);

    const auto trades = drainTrades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].trade_qty, 5);
}

TEST_F(MatcherTest, NoMatchWhenPriceDoesNotCross) {
    const order_id_t sell_id = order_book->addOrder(101, 10, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_FALSE(matcher.tryMatch(buy_id)); // buy below best ask, nothing crosses

    ASSERT_TRUE(order_book->IsOrderValid(buy_id))
        << "unmatched buy must keep resting in the book";
    EXPECT_EQ(order_book->getOrderQuantity(buy_id).value(), 10);

    ASSERT_TRUE(order_book->IsOrderValid(sell_id))
        << "untouched sell must keep resting in the book";
    EXPECT_EQ(order_book->getOrderQuantity(sell_id).value(), 10);

    EXPECT_TRUE(drainTrades().empty()); // no trade means no trade event
}

TEST_F(MatcherTest, IncomingSellCrossesBuyBook) {
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    const order_id_t sell_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);

    EXPECT_TRUE(matcher.tryMatch(sell_id)); // incoming sell fully filled against buy book
    EXPECT_TRUE(IsGone(*order_book, sell_id));
    EXPECT_TRUE(IsGone(*order_book, buy_id));

    // Sides are reported from the resting order's perspective either way round.
    const auto trades = drainTrades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_id, buy_id);
    EXPECT_EQ(trades[0].sell_id, sell_id);
}

TEST_F(MatcherTest, MatchesBestPriceFirst) {
    const order_id_t best_ask = order_book->addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t worse_ask = order_book->addOrder(105, 5, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(105, 5, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_TRUE(matcher.tryMatch(buy_id)); // 5 units fill against the best ask only
    EXPECT_TRUE(IsGone(*order_book, buy_id));
    EXPECT_TRUE(IsGone(*order_book, best_ask)); // best (lowest) ask consumed

    ASSERT_TRUE(order_book->IsOrderValid(worse_ask)) << "worse ask must be untouched";
    EXPECT_EQ(order_book->getOrderQuantity(worse_ask).value(), 5);

    const auto trades = drainTrades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, best_ask);
    EXPECT_EQ(trades[0].trade_price, 100); // traded at the resting order's price
}

TEST_F(MatcherTest, MatchAgainstUnknownOrderIsANoOp) {
    EXPECT_FALSE(matcher.tryMatch(9999));
    EXPECT_TRUE(drainTrades().empty());
}

// --- Fast path and the reused trade buffer --------------------------------
// tryMatch returns early when the opposite best does not cross, skips rewriting
// the incoming order when nothing filled, and keeps its trade vector across
// calls. These pin down that none of that changes what is observable.

TEST_F(MatcherTest, NoMatchAgainstAnEmptyOppositeSide) {
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_FALSE(matcher.tryMatch(buy_id));
    EXPECT_EQ(order_book->getOrderQuantity(buy_id), 10);
    EXPECT_TRUE(drainTrades().empty());
}

TEST_F(MatcherTest, PriceExactlyAtOppositeBestCrosses) {
    // The early exit must use the same inclusive rule as fillOrders on both
    // sides; an off-by-one here would silently stop at-the-touch orders filling.
    const order_id_t sell_id = order_book->addOrder(100, 4, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 4, OrderType::LIMIT, OrderSide::BUY);
    EXPECT_TRUE(matcher.tryMatch(buy_id));
    EXPECT_TRUE(IsGone(*order_book, sell_id));

    const order_id_t bid_id = order_book->addOrder(90, 4, OrderType::LIMIT, OrderSide::BUY);
    const order_id_t ask_id = order_book->addOrder(90, 4, OrderType::LIMIT, OrderSide::SELL);
    EXPECT_TRUE(matcher.tryMatch(ask_id));
    EXPECT_TRUE(IsGone(*order_book, bid_id));

    EXPECT_EQ(drainTrades().size(), 2u);
}

TEST_F(MatcherTest, TradesFromAnEarlierMatchAreNotPublishedAgain) {
    // First sweep: two resting sells, one incoming buy takes both.
    order_book->addOrder(100, 3, OrderType::LIMIT, OrderSide::SELL);
    order_book->addOrder(101, 3, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t first = order_book->addOrder(101, 6, OrderType::LIMIT, OrderSide::BUY);
    EXPECT_TRUE(matcher.tryMatch(first));
    EXPECT_EQ(drainTrades().size(), 2u);

    // A passive order in between takes the early exit and must publish nothing.
    const order_id_t passive = order_book->addOrder(90, 1, OrderType::LIMIT, OrderSide::BUY);
    EXPECT_FALSE(matcher.tryMatch(passive));
    EXPECT_TRUE(drainTrades().empty());

    // Second sweep reuses the buffer: exactly its own single trade comes out.
    const order_id_t sell_id = order_book->addOrder(102, 2, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t second = order_book->addOrder(102, 2, OrderType::LIMIT, OrderSide::BUY);
    EXPECT_TRUE(matcher.tryMatch(second));
    const auto trades = drainTrades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_id, second);
    EXPECT_EQ(trades[0].sell_id, sell_id);
    EXPECT_EQ(trades[0].trade_qty, 2);
}
