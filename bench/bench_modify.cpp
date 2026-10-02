// bench/bench_modify.cpp
//
// Latency micro-benchmarks for OrderBook::modifyOrder. All timings in nanoseconds.
//
// modifyOrder mutates book state, so each iteration first rests a fresh order under
// Pause/ResumeTiming and only the modify itself is inside the timed window.
//
// Run:   ./build/bench/bench_modify
//        ./build/bench/bench_modify --benchmark_filter=ChangeSide
//
// Build in Release for meaningful numbers (the project defaults to Release):
//   cmake -S . -B build && cmake --build build --target bench_modify

#include <benchmark/benchmark.h>

#include "Order.h"
#include "OrderBook.h"

namespace {

constexpr double kBasePrice = 100.0;
constexpr double kAltPrice = 105.0;
constexpr int kQuantity = 10;

// Same side, same price, change quantity only: in-place BookLevel::modifyOrder path.
void BM_Modify_QuantityOnly(benchmark::State& state) {
    OrderBook book;
    int newQty = kQuantity;
    for (auto _ : state) {
        state.PauseTiming();
        order_id_t id = book.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        state.ResumeTiming();

        // Same price/side as the resting order; only quantity differs.
        bool ok = book.modifyOrder(id, ++newQty, kBasePrice, OrderSide::BUY, OrderType::LIMIT).has_value();
        benchmark::DoNotOptimize(ok);

        state.PauseTiming();
        book.cancelOrder(id);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Modify_QuantityOnly)->Unit(benchmark::kNanosecond);

// Same side, different price: internally cancels from the old level and re-adds to the new level.
void BM_Modify_ChangePriceLevel(benchmark::State& state) {
    OrderBook book;
    for (auto _ : state) {
        state.PauseTiming();
        order_id_t id = book.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        state.ResumeTiming();

        auto modified = book.modifyOrder(id, kQuantity, kAltPrice, OrderSide::BUY, OrderType::LIMIT);
        benchmark::DoNotOptimize(modified);

        state.PauseTiming();
        // A reprice mints a new id, so cleaning up `id` would leak the order and
        // let the book grow without bound across iterations.
        book.cancelOrder(modified.value_or(id));
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Modify_ChangePriceLevel)->Unit(benchmark::kNanosecond);

// Cross-side modify (buy -> sell): cancels from the buy side and adds to the sell side.
void BM_Modify_ChangeSide(benchmark::State& state) {
    OrderBook book;
    for (auto _ : state) {
        state.PauseTiming();
        order_id_t id = book.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        state.ResumeTiming();

        auto modified = book.modifyOrder(id, kQuantity, kAltPrice, OrderSide::SELL, OrderType::LIMIT);
        benchmark::DoNotOptimize(modified);

        state.PauseTiming();
        // Switching sides re-books the order under a new id; see above.
        book.cancelOrder(modified.value_or(id));
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Modify_ChangeSide)->Unit(benchmark::kNanosecond);

}  // namespace

BENCHMARK_MAIN();
