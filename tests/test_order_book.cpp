#include "gtest/gtest.h"
#include "Order.h"
#include "OrderBook.h"

// OrderBook no longer exposes getOrder(): Order objects live in the OrderManager
// and are reached through the optional-returning accessors below. An id that is
// unknown, cancelled or fully filled yields nullopt rather than a null pointer.
//
// Prices are plain ints — there is no PRICE_MULTIPLIER and no floating point, so
// a price goes in and comes back out unchanged.

TEST(OrderBook, AddOrder) {
    OrderBook book;

    const order_id_t buy_id = book.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    EXPECT_TRUE(book.IsOrderValid(buy_id));
    EXPECT_EQ(book.getOrderSide(buy_id).value(), OrderSide::BUY);
    EXPECT_EQ(book.getOrderType(buy_id).value(), OrderType::LIMIT);
    EXPECT_EQ(book.getOrderPrice(buy_id).value(), 100);
    EXPECT_EQ(book.getOrderQuantity(buy_id).value(), 10);

    const order_id_t sell_id = book.addOrder(101, 5, OrderType::LIMIT, OrderSide::SELL);
    EXPECT_NE(sell_id, buy_id);
    EXPECT_EQ(book.getOrderSide(sell_id).value(), OrderSide::SELL);
    EXPECT_EQ(book.getOrderPrice(sell_id).value(), 101);
    EXPECT_EQ(book.getOrderQuantity(sell_id).value(), 5);

    const order_id_t market_id = book.addOrder(99, 20, OrderType::MARKET, OrderSide::BUY);
    EXPECT_EQ(book.getOrderType(market_id).value(), OrderType::MARKET);
    EXPECT_EQ(book.getOrderQuantity(market_id).value(), 20);
}

TEST(OrderBook, AccessorsOnUnknownOrder) {
    OrderBook book;
    EXPECT_FALSE(book.IsOrderValid(9999));
    EXPECT_FALSE(book.getOrderSide(9999).has_value());
    EXPECT_FALSE(book.getOrderType(9999).has_value());
    EXPECT_FALSE(book.getOrderPrice(9999).has_value());
    EXPECT_FALSE(book.getOrderQuantity(9999).has_value());
}

TEST(OrderBook, OrderBookView) {
    OrderBook book;
    book.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    book.addOrder(99, 10, OrderType::LIMIT, OrderSide::BUY);
    book.addOrder(101, 5, OrderType::LIMIT, OrderSide::SELL);
    book.addOrder(102, 5, OrderType::LIMIT, OrderSide::SELL);

    const auto [bids, asks] = book.getOrderBookView(2);
    ASSERT_EQ(bids.size(), 2u);
    ASSERT_EQ(asks.size(), 2u);
    EXPECT_EQ(bids[0]->getPrice(), 100); // best bid is the highest
    EXPECT_EQ(bids[1]->getPrice(), 99);
    EXPECT_EQ(asks[0]->getPrice(), 101); // best ask is the lowest
    EXPECT_EQ(asks[1]->getPrice(), 102);

    EXPECT_EQ(book.getBuySideView(1).size(), 1u);
    EXPECT_EQ(book.getSellSideView(1).size(), 1u);
}

TEST(OrderBook, CancelOrder) {
    OrderBook book;
    const order_id_t id = book.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_TRUE(book.cancelOrder(id));
    EXPECT_FALSE(book.IsOrderValid(id));
    EXPECT_FALSE(book.getOrderQuantity(id).has_value());
    EXPECT_TRUE(book.getBuySideView(1).empty());
}

TEST(OrderBook, CancelUnknownOrderFails) {
    OrderBook book;
    EXPECT_FALSE(book.cancelOrder(9999));
}

TEST(OrderBook, ModifyOrderSamePriceAndSide) {
    OrderBook book;
    const order_id_t id = book.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    const auto modified = book.modifyOrder(id, 5, 100, OrderSide::BUY, OrderType::LIMIT);
    ASSERT_TRUE(modified.has_value());
    EXPECT_EQ(book.getOrderQuantity(modified.value()).value(), 5);
    EXPECT_EQ(book.getOrderPrice(modified.value()).value(), 100);
    EXPECT_EQ(book.getOrderSide(modified.value()).value(), OrderSide::BUY);
}

TEST(OrderBook, ModifyOrderNewPrice) {
    OrderBook book;
    const order_id_t id = book.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    const auto modified = book.modifyOrder(id, 20, 101, OrderSide::BUY, OrderType::LIMIT);
    ASSERT_TRUE(modified.has_value());
    EXPECT_EQ(book.getOrderQuantity(modified.value()).value(), 20);
    EXPECT_EQ(book.getOrderPrice(modified.value()).value(), 101);

    const auto bids = book.getBuySideView(5);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids.front()->getPrice(), 101);
}

TEST(OrderBook, ModifyOrderSwitchesSide) {
    OrderBook book;
    const order_id_t id = book.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // Flipping the side re-books the order on the other book, so the id changes.
    const auto modified = book.modifyOrder(id, 20, 101, OrderSide::SELL, OrderType::LIMIT);
    ASSERT_TRUE(modified.has_value());
    EXPECT_EQ(book.getOrderSide(modified.value()).value(), OrderSide::SELL);
    EXPECT_EQ(book.getOrderQuantity(modified.value()).value(), 20);
    EXPECT_EQ(book.getOrderPrice(modified.value()).value(), 101);

    EXPECT_TRUE(book.getBuySideView(5).empty());
    EXPECT_EQ(book.getSellSideView(5).size(), 1u);
}

TEST(OrderBook, ModifyUnknownOrderReturnsNullopt) {
    OrderBook book;
    EXPECT_FALSE(book.modifyOrder(9999, 10, 100, OrderSide::BUY, OrderType::LIMIT).has_value());
}

TEST(OrderBook, FillOrders) {
    OrderBook book;
    const order_id_t ask = book.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);

    // A buyer paying 100 consumes the resting ask outright.
    EXPECT_EQ(book.fillOrders(OrderSide::SELL, 100, 10), 0);
    EXPECT_FALSE(book.IsOrderValid(ask));
}

TEST(OrderBook, FillOrdersReturnsUnfilledRemainder) {
    OrderBook book;
    book.addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    EXPECT_EQ(book.fillOrders(OrderSide::SELL, 100, 8), 3);
}
