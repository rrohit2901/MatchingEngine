#pragma once

#include <chrono>
#include <type_traits>
#include <variant>

#include "Order.h"

using event_id_t = unsigned int;

// Spelled once: it appears in the Event base, in Shutdown, and in the logger's
// formatter signature.
using event_time_t = std::chrono::time_point<std::chrono::system_clock>;

enum class EventTypes {
    TRADE_DONE,
    LIMIT_ORDER_ADDED,
    MARKET_ORDER_ADDED,
    ORDER_MODIFIED,
    ORDER_CANCELLED,
    ORDER_REJECTED,
    ORDER_MODIFY_REJECTED,
    SESSION_CLOSE,
    SESSION_OPEN,
    SHUTDOWN,
};

inline const char* to_string(OrderSide side) {
    return side == OrderSide::BUY ? "BUY" : "SELL";
}

// Every event carries nothing but a tick count from the base.
//
// Deliberately NOT polymorphic. Events travel through the SPSC ring by value, so
// a vptr would cost 8 bytes in every slot and -- worse -- would make EventVariant
// non-trivially-copyable, turning each slot write from a memcpy into a real
// move-assignment with a destroy-then-construct branch on the alternative. The
// formatting that used to be a virtual push_to_file() now lives in the logger
// (event_handler/EventFormatter.h), which is the only code that ever needed it.
//
// The timestamp is taken at construction, on the matching thread. That is a
// clock_gettime through the vDSO -- tens of nanoseconds -- and it is deliberate:
// ordering in the log comes from the queue (FIFO, single producer), so the
// timestamp only has to say roughly when something happened. Stamping a raw
// cycle counter here and converting on the logger thread would shave that call
// off the hot path, at the cost of calibration, clock-drift caveats and a
// platform-specific header. Not worth it at this size.
struct Event {
    event_time_t event_time;

    explicit Event(event_time_t event_time = std::chrono::system_clock::now()) noexcept
        : event_time(event_time) {}
};

// kType on each event is what the old `event_type` member was, moved to compile
// time: the variant's active alternative already *is* the tag, so storing it
// again would cost 8 bytes a slot after padding and could disagree with reality.
// event_type() below recovers it.

struct TradeEvent: Event {
    static constexpr EventTypes kType = EventTypes::TRADE_DONE;

    order_id_t buy_id, sell_id;
    int trade_price;
    int trade_qty;

    TradeEvent(order_id_t buy_order_id, order_id_t sell_order_id, int trade_price, int trade_qty) noexcept
        : buy_id(buy_order_id), sell_id(sell_order_id),
          trade_price(trade_price), trade_qty(trade_qty) {}
};

struct LimitOrderAdd: Event {
    static constexpr EventTypes kType = EventTypes::LIMIT_ORDER_ADDED;

    order_id_t order_id;
    int price;
    int qty;
    OrderSide side;

    LimitOrderAdd(order_id_t order_id, int price, int qty, OrderSide side) noexcept
        : order_id(order_id), price(price), qty(qty), side(side) {}
};

struct MarketOrderAdd: Event {
    static constexpr EventTypes kType = EventTypes::MARKET_ORDER_ADDED;

    order_id_t order_id;
    int qty;
    OrderSide side;

    // No price: a market order takes whatever the book offers.
    MarketOrderAdd(order_id_t order_id, int qty, OrderSide side) noexcept
        : order_id(order_id), qty(qty), side(side) {}
};

struct OrderModified: Event {
    static constexpr EventTypes kType = EventTypes::ORDER_MODIFIED;

    order_id_t order_id;
    order_id_t new_order_id;
    int new_price;
    int new_qty;
    OrderSide side;

    // Both ids are recorded because a reprice or a grow in quantity retires
    // the original order and books a replacement under a fresh id; a log
    // holding only one of them cannot reconstruct the order's history.
    OrderModified(order_id_t order_id, order_id_t new_order_id, int new_price, int new_qty, OrderSide side) noexcept
        : order_id(order_id), new_order_id(new_order_id),
          new_price(new_price), new_qty(new_qty), side(side) {}
};

struct OrderCancelled: Event {
    static constexpr EventTypes kType = EventTypes::ORDER_CANCELLED;

    order_id_t order_id;
    OrderSide side;

    OrderCancelled(order_id_t order_id, OrderSide side) noexcept
        : order_id(order_id), side(side) {}
};

// A rejected order carries no order id: it was turned away before reaching the
// book, so no id was ever minted for it. The reason stays text so that Events
// stays independent of the risk layer's enum -- but it is a `const char*`, not a
// std::string. Every producer passes a string literal or RiskManager's
// to_string(RejectReason), all of which have static storage duration, so there is
// nothing to own. A std::string here would have been a malloc on the matching
// thread and a free on the logger thread, which is the whole cost this queue was
// restructured to remove.
//
// CONTRACT: `reason` must outlive the log line. Static storage only; never point
// it at a local buffer.
struct OrderRejected: Event {
    static constexpr EventTypes kType = EventTypes::ORDER_REJECTED;

    const char* reason;
    int price;
    int qty;
    OrderSide side;
    OrderType type;

    OrderRejected(int price, int qty, OrderSide side, OrderType type, const char* reason) noexcept
        : reason(reason), price(price), qty(qty), side(side), type(type) {}
};

// A rejected modify DOES have an order id: the order it targeted is still
// resting, untouched, under that id. Same static-storage contract on `reason`.
struct OrderModifyRejected: Event {
    static constexpr EventTypes kType = EventTypes::ORDER_MODIFY_REJECTED;

    const char* reason;
    order_id_t order_id;
    int new_price;
    int new_qty;
    OrderSide side;

    OrderModifyRejected(order_id_t order_id, int new_price, int new_qty, OrderSide side, const char* reason) noexcept
        : reason(reason), order_id(order_id), new_price(new_price),
          new_qty(new_qty), side(side) {}
};

struct SessionOpen: Event {
    static constexpr EventTypes kType = EventTypes::SESSION_OPEN;
};

struct SessionClose: Event {
    static constexpr EventTypes kType = EventTypes::SESSION_CLOSE;
};

// The logger's shutdown sentinel, and the role nullptr played when the queue
// carried unique_ptr<Event>. It is the *first* alternative so that a
// default-constructed EventVariant is the sentinel: Logger::stop() still just
// pushes T{}, and RingBuffer's value-initialised slot array needs no special
// casing. Its timestamp is left at the epoch rather than read from the clock:
// it is never printed, and default construction happens once per ring slot at
// construction and once per Logger loop variable.
struct Shutdown: Event {
    static constexpr EventTypes kType = EventTypes::SHUTDOWN;

    Shutdown() noexcept : Event(event_time_t{}) {}
};

using EventVariant = std::variant<
    Shutdown,
    TradeEvent,
    LimitOrderAdd,
    MarketOrderAdd,
    OrderModified,
    OrderCancelled,
    OrderRejected,
    OrderModifyRejected,
    SessionOpen,
    SessionClose>;

// The two properties the hot path actually depends on. If either breaks, a slot
// write has quietly stopped being a memcpy.
static_assert(std::is_trivially_copyable_v<EventVariant>,
              "EventVariant must stay trivially copyable: the SPSC ring copies "
              "events by value, and a non-trivial alternative (a virtual, a "
              "std::string, any owning member) turns every slot write into a "
              "move-assign and puts allocation back on the matching thread");
// 48, up from 40, when order_id_t widened to 64 bits to carry OrderManager's
// slot generation: OrderModified holds two ids and sets the high-water mark.
static_assert(sizeof(EventVariant) <= 48,
              "EventVariant got fatter -- check what was added and re-check the "
              "ring buffer's footprint in event_handler/EventQueue.h");

inline bool is_shutdown(const EventVariant& event) noexcept {
    return std::holds_alternative<Shutdown>(event);
}

inline EventTypes event_type(const EventVariant& event) noexcept {
    return std::visit(
        []<typename E>(const E&) noexcept { return E::kType; }, event);
}
