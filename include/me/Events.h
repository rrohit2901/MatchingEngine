#pragma once

#include <memory>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <ctime>

#include "Order.h"

using event_id_t = unsigned int;

enum class EventTypes {
    TRADE_DONE,
    LIMIT_ORDER_ADDED,
    MARKET_ORDER_ADDED,
    ORDER_MODIFIED,
    ORDER_CANCELLED,
    SESSION_CLOSE,
    SESSION_OPEN,
};

inline const char* to_string(OrderSide side) {
    return side == OrderSide::BUY ? "BUY" : "SELL";
}

class Event {
    public:
        EventTypes event_type;
        std::chrono::time_point<std::chrono::system_clock> event_time;

        // event_time defaults to now(), so call sites that only care about the
        // type can construct with one argument. Taken by value rather than by
        // reference so a temporary (including the default) can bind.
        Event(EventTypes event_type,
              std::chrono::time_point<std::chrono::system_clock> event_time = std::chrono::system_clock::now())
            : event_type(event_type), event_time(event_time) {}
        virtual ~Event() = default;

        virtual void push_to_file(std::shared_ptr<std::ofstream>& output_file) = 0;

    protected:
        // UTC, microsecond resolution: "2026-08-05 10:31:44.812305". Seconds-only
        // timestamps are useless for ordering events in a matching engine.
        std::string formatted_time() const {
            const auto since_epoch = event_time.time_since_epoch();
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch - secs);

            const std::time_t as_time_t = std::chrono::system_clock::to_time_t(event_time);
            std::tm utc{};
            gmtime_r(&as_time_t, &utc);

            std::ostringstream out;
            out << std::put_time(&utc, "%Y-%m-%d %H:%M:%S")
                << '.' << std::setw(6) << std::setfill('0') << micros.count();
            return out.str();
        }

        // Every event line is "<timestamp>" followed by the body each subclass
        // builds, so the timestamp formatting lives here rather than in each one.
        // The line is assembled first and written with a single <<, so concurrent
        // writers cannot interleave halves of a record.
        void write_line(std::shared_ptr<std::ofstream>& output_file, const std::string& body) const {
            (*output_file) << formatted_time() << body << '\n';
        }
};

class TradeEvent: public Event {
    public:
        order_id_t buy_id, sell_id;
        int trade_price;
        int trade_qty;

        TradeEvent(order_id_t buy_order_id, order_id_t sell_order_id, int trade_price, int trade_qty)
            : Event(EventTypes::TRADE_DONE), buy_id(buy_order_id), sell_id(sell_order_id),
              trade_price(trade_price), trade_qty(trade_qty) {}

        void push_to_file(std::shared_ptr<std::ofstream>& output_file) override {
            std::ostringstream body;
            body << " | INFO | TRADE_EVENT"
                 << " | BUY_ID: " << buy_id
                 << " | SELL_ID: " << sell_id
                 << " | TRADE_QTY: " << trade_qty
                 << " | TRADE_PRICE: " << trade_price;
            write_line(output_file, body.str());
        }
};

class LimitOrderAdd: public Event {
    public:
        order_id_t order_id;
        int price;
        int qty;
        OrderSide side;

        LimitOrderAdd(order_id_t order_id, int price, int qty, OrderSide side)
            : Event(EventTypes::LIMIT_ORDER_ADDED), order_id(order_id), price(price), qty(qty), side(side) {}

        void push_to_file(std::shared_ptr<std::ofstream>& output_file) override {
            std::ostringstream body;
            body << " | INFO | LIMIT_ORDER_ADDED"
                 << " | ORDER_ID: " << order_id
                 << " | SIDE: " << to_string(side)
                 << " | QTY: " << qty
                 << " | PRICE: " << price;
            write_line(output_file, body.str());
        }
};

class MarketOrderAdd: public Event {
    public:
        order_id_t order_id;
        int qty;
        OrderSide side;

        // No price: a market order takes whatever the book offers.
        MarketOrderAdd(order_id_t order_id, int qty, OrderSide side)
            : Event(EventTypes::MARKET_ORDER_ADDED), order_id(order_id), qty(qty), side(side) {}

        void push_to_file(std::shared_ptr<std::ofstream>& output_file) override {
            std::ostringstream body;
            body << " | INFO | MARKET_ORDER_ADDED"
                 << " | ORDER_ID: " << order_id
                 << " | SIDE: " << to_string(side)
                 << " | QTY: " << qty;
            write_line(output_file, body.str());
        }
};

class OrderModified: public Event {
    public:
        order_id_t order_id;
        order_id_t new_order_id;
        int new_price;
        int new_qty;
        OrderSide side;

        // Both ids are recorded because a reprice or a grow in quantity retires
        // the original order and books a replacement under a fresh id; a log
        // holding only one of them cannot reconstruct the order's history.
        OrderModified(order_id_t order_id, order_id_t new_order_id, int new_price, int new_qty, OrderSide side)
            : Event(EventTypes::ORDER_MODIFIED), order_id(order_id), new_order_id(new_order_id),
              new_price(new_price), new_qty(new_qty), side(side) {}

        void push_to_file(std::shared_ptr<std::ofstream>& output_file) override {
            std::ostringstream body;
            body << " | INFO | ORDER_MODIFIED"
                 << " | ORDER_ID: " << order_id
                 << " | NEW_ORDER_ID: " << new_order_id
                 << " | SIDE: " << to_string(side)
                 << " | NEW_QTY: " << new_qty
                 << " | NEW_PRICE: " << new_price;
            write_line(output_file, body.str());
        }
};

class OrderCancelled: public Event {
    public:
        order_id_t order_id;
        OrderSide side;

        OrderCancelled(order_id_t order_id, OrderSide side)
            : Event(EventTypes::ORDER_CANCELLED), order_id(order_id), side(side) {}

        void push_to_file(std::shared_ptr<std::ofstream>& output_file) override {
            std::ostringstream body;
            body << " | INFO | ORDER_CANCELLED"
                 << " | ORDER_ID: " << order_id
                 << " | SIDE: " << to_string(side);
            write_line(output_file, body.str());
        }
};

class SessionOpen: public Event {
    public:
        SessionOpen(): Event(EventTypes::SESSION_OPEN) {}

        void push_to_file(std::shared_ptr<std::ofstream>& output_file) override {
            write_line(output_file, " | INFO | SESSION_OPEN");
        }
};

class SessionClose: public Event {
    public:
        SessionClose(): Event(EventTypes::SESSION_CLOSE) {}

        void push_to_file(std::shared_ptr<std::ofstream>& output_file) override {
            write_line(output_file, " | INFO | SESSION_CLOSE");
        }
};
