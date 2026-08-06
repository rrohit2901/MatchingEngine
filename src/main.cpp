#include "MatchingEngine.h"
#include "Logger.h"
#include "lock_queue.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

using EventQueue = LockQueue<std::unique_ptr<Event>>;

constexpr const char* kLogFile = "logs/matching_engine.log";

// Prices are plain integers (ticks) — there is no floating point in the engine.
// BookLevel hands back order ids rather than Order objects, so a level can only
// print ids; per-order detail would need MatchingEngine to forward OrderBook's
// getOrder*() accessors.
void printSide(const char* label, const std::vector<std::shared_ptr<BookLevel>>& levels) {
    std::cout << label << ":\n";
    for (const auto& level : levels) {
        std::cout << "  price " << level->getPrice()
                  << ", quantity " << level->getTotalQuantity() << '\n';
        // getOrders() already filters out cancelled/filled ids, so no valid() check here.
        for (order_id_t order_id : level->getOrders()) {
            std::cout << "    order " << order_id << '\n';
        }
    }
}

// Everything the matching thread does. Kept in its own scope so the engine is
// destroyed — and SESSION_CLOSE published — before the sentinel is queued.
void runMatchingEngine(const std::shared_ptr<EventQueue>& events) {
    MatchingEngine<LockQueue> me{events};

    const order_id_t buy1 = me.addOrder(1005, 10, OrderType::LIMIT, OrderSide::BUY).value();
    me.addOrder(1010, 5, OrderType::LIMIT, OrderSide::SELL);

    const order_id_t buy2 = me.addOrder(1007, 10, OrderType::LIMIT, OrderSide::BUY).value();
    me.addOrder(1013, 5, OrderType::LIMIT, OrderSide::SELL);

    me.addOrder(1005, 10, OrderType::LIMIT, OrderSide::BUY);
    const order_id_t sell3 = me.addOrder(1025, 5, OrderType::LIMIT, OrderSide::SELL).value();

    // Crosses the resting asks at 1010 and 1013: this is what produces trades.
    me.addOrder(1025, 10, OrderType::LIMIT, OrderSide::BUY);

    // modifyOrder reports the surviving order id, or nullopt when the order is
    // not in the book. A requote can change the id, so keep the returned value.
    if (const auto requoted = me.modifyOrder(buy1, 15, 1005, OrderSide::BUY, OrderType::LIMIT)) {
        std::cout << "order " << buy1 << " requoted as " << *requoted << '\n';
    } else {
        std::cout << "order " << buy1 << " could not be modified\n";
    }

    std::cout << std::boolalpha;
    std::cout << "cancel " << sell3 << ": " << me.cancelOrder(sell3) << '\n';
    std::cout << "cancel " << buy2 << ": " << me.cancelOrder(buy2) << '\n';

    const auto [buyLevels, sellLevels] = me.getOrderBookView(5);
    printSide("Buy levels", buyLevels);
    printSide("Sell levels", sellLevels);
}

} // namespace

int main() {
    // The queue is the only thing the two threads share. The engine publishes
    // into it and never touches a file; the logger drains it and does all the
    // I/O, so no write ever lands on the matching path.
    // ofstream will not create missing directories, so make the log directory
    // exist before the logger thread tries to open a file inside it.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{kLogFile}.parent_path(), ec);
    if (ec) {
        std::cerr << "cannot create log directory: " << ec.message() << '\n';
        return 1;
    }

    auto events = std::make_shared<EventQueue>();
    Logger<LockQueue> logger{events, kLogFile};

    std::thread logger_thread{[&logger] { logger.readWriteLogs(); }};

    runMatchingEngine(events);

    // The engine is destroyed by now, so SESSION_CLOSE is already queued. The
    // sentinel goes in behind it and the logger drains everything before
    // returning, so the join below cannot truncate the log.
    logger.stop();
    logger_thread.join();

    std::cout << "events written to " << kLogFile << '\n';
    return 0;
}
