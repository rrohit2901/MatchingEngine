#pragma once

#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <chrono>
#include <variant>

#include "Events.h"

// Turning events into log lines.
//
// This used to be a virtual push_to_file() on each event. It moved here because
// it is logger-only work -- ostringstream, iomanip, wall-clock conversion -- and
// leaving it on the event types forced a vptr into every ring buffer slot and
// dragged <fstream>/<sstream> into the matching thread's include graph via
// Events.h. Nothing in me/ includes this header.
//
// Dispatch is std::visit over EventVariant, so adding an alternative without
// adding its overload below is a compile error, exactly as forgetting to
// override a pure virtual was.

// UTC, microsecond resolution: "2026-08-05 10:31:44.812305". Seconds-only
// timestamps are useless for ordering events in a matching engine.
//
// Formatting happens here, on the logger thread, rather than where the event was
// stamped: ostringstream and put_time are far more expensive than the clock read
// itself, and none of that belongs on the matching path.
inline std::string format_event_time(event_time_t when) {
    const auto since_epoch = when.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch - secs);

    const std::time_t as_time_t = std::chrono::system_clock::to_time_t(when);
    std::tm utc{};
    gmtime_r(&as_time_t, &utc);

    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(6) << std::setfill('0') << micros.count();
    return out.str();
}

// Every event line is "<timestamp>" followed by the body the overload below
// builds. The line is assembled first and written with a single <<, so
// concurrent writers cannot interleave halves of a record.
class EventWriter {
    public:
        explicit EventWriter(std::shared_ptr<std::ofstream>& output_file)
            : output_file(output_file) {}

        void operator()(const TradeEvent& event) const {
            std::ostringstream body;
            body << " | INFO | TRADE_EVENT"
                 << " | BUY_ID: " << event.buy_id
                 << " | SELL_ID: " << event.sell_id
                 << " | TRADE_QTY: " << event.trade_qty
                 << " | TRADE_PRICE: " << event.trade_price;
            write_line(event, body.str());
        }

        void operator()(const LimitOrderAdd& event) const {
            std::ostringstream body;
            body << " | INFO | LIMIT_ORDER_ADDED"
                 << " | ORDER_ID: " << event.order_id
                 << " | SIDE: " << to_string(event.side)
                 << " | QTY: " << event.qty
                 << " | PRICE: " << event.price;
            write_line(event, body.str());
        }

        void operator()(const MarketOrderAdd& event) const {
            std::ostringstream body;
            body << " | INFO | MARKET_ORDER_ADDED"
                 << " | ORDER_ID: " << event.order_id
                 << " | SIDE: " << to_string(event.side)
                 << " | QTY: " << event.qty;
            write_line(event, body.str());
        }

        void operator()(const OrderModified& event) const {
            std::ostringstream body;
            body << " | INFO | ORDER_MODIFIED"
                 << " | ORDER_ID: " << event.order_id
                 << " | NEW_ORDER_ID: " << event.new_order_id
                 << " | SIDE: " << to_string(event.side)
                 << " | NEW_QTY: " << event.new_qty
                 << " | NEW_PRICE: " << event.new_price;
            write_line(event, body.str());
        }

        void operator()(const OrderCancelled& event) const {
            std::ostringstream body;
            body << " | INFO | ORDER_CANCELLED"
                 << " | ORDER_ID: " << event.order_id
                 << " | SIDE: " << to_string(event.side);
            write_line(event, body.str());
        }

        void operator()(const OrderRejected& event) const {
            std::ostringstream body;
            body << " | WARN | ORDER_REJECTED"
                 << " | SIDE: " << to_string(event.side)
                 << " | TYPE: " << (event.type == OrderType::MARKET ? "MARKET" : "LIMIT")
                 << " | QTY: " << event.qty
                 << " | PRICE: " << event.price
                 << " | REASON: " << event.reason;
            write_line(event, body.str());
        }

        void operator()(const OrderModifyRejected& event) const {
            std::ostringstream body;
            body << " | WARN | ORDER_MODIFY_REJECTED"
                 << " | ORDER_ID: " << event.order_id
                 << " | SIDE: " << to_string(event.side)
                 << " | NEW_QTY: " << event.new_qty
                 << " | NEW_PRICE: " << event.new_price
                 << " | REASON: " << event.reason;
            write_line(event, body.str());
        }

        void operator()(const SessionOpen& event) const {
            write_line(event, " | INFO | SESSION_OPEN");
        }

        void operator()(const SessionClose& event) const {
            write_line(event, " | INFO | SESSION_CLOSE");
        }

        // The sentinel is not a log record. Logger::readWriteLogs() breaks out of
        // its loop before ever reaching here; this overload exists so the visit
        // is exhaustive.
        void operator()(const Shutdown&) const {}

    private:
        std::shared_ptr<std::ofstream>& output_file;

        void write_line(const Event& event, const std::string& body) const {
            (*output_file) << format_event_time(event.event_time) << body << '\n';
        }
};

inline void write_event(const EventVariant& event, std::shared_ptr<std::ofstream>& output_file) {
    std::visit(EventWriter{output_file}, event);
}
