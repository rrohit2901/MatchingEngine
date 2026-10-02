// bench/bench_add.cpp
//
// Latency micro-benchmarks for OrderBook::addOrder. All timings in nanoseconds.
//
// Run:   ./build/bench/bench_add
//        ./build/bench/bench_add --benchmark_filter=NewPriceLevel
//        ./build/bench/bench_add --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
//
// Build in Release for meaningful numbers (the project defaults to Release):
//   cmake -S . -B build && cmake --build build --target bench_add

#include <benchmark/benchmark.h>

#include "Order.h"
#include "OrderBook.h"

namespace {

constexpr double kBasePrice = 100.0;
constexpr int kQuantity = 10;
constexpr int kPriceLevels = 100;  // bounded spread to keep the level tree at realistic depth

// Every add lands on a brand-new price level: exercises BookLevel creation + map insert.
void BM_Add_NewPriceLevel(benchmark::State& state) {
    OrderBook book;
    int i = 0;
    for (auto _ : state) {
        const double price = kBasePrice + i++;  // strictly increasing -> always a new level
        order_id_t id = book.addOrder(price, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Add_NewPriceLevel)->Unit(benchmark::kNanosecond);

// Every add targets one fixed price: appends to the same level's order vector (FIFO tail).
void BM_Add_ExistingPriceLevel(benchmark::State& state) {
    OrderBook book;
    for (auto _ : state) {
        order_id_t id = book.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Add_ExistingPriceLevel)->Unit(benchmark::kNanosecond);

// Adds spread across kPriceLevels distinct prices: realistic mix of new-level and append paths.
void BM_Add_SpreadAcrossLevels(benchmark::State& state) {
    OrderBook book;
    int i = 0;
    for (auto _ : state) {
        const double price = kBasePrice + (i++ % kPriceLevels);
        order_id_t id = book.addOrder(price, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Add_SpreadAcrossLevels)->Unit(benchmark::kNanosecond);

// Market-order add. The sentinel-price handling for market orders is still unimplemented —
// OrderBookSide::addOrder ignores its OrderType and files the order at the price passed in —
// so this currently measures the same path as BM_Add_ExistingPriceLevel. It is kept as the
// placeholder for when market orders land.
void BM_Add_MarketOrder(benchmark::State& state) {
    OrderBook book;
    for (auto _ : state) {
        order_id_t id = book.addOrder(kBasePrice, kQuantity, OrderType::MARKET, OrderSide::BUY);
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Add_MarketOrder)->Unit(benchmark::kNanosecond);

// Buy and sell paths are symmetric (mirror-image comparators); benchmarking BUY is representative.

}  // namespace

BENCHMARK_MAIN();
