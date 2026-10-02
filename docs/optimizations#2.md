# Optimizations #2: Order book hot path

Goal: lower peak (tail) latency in the order book by removing hot-path heap
allocations, avoiding O(n) shifts and scans where possible, and making the book
cache-friendly.

This change builds on the earlier work in this round: `getBestPrice()` replacing
the view-based risk check, and the `[[likely]]`/`[[unlikely]]` hints.

---

## 1. `BookLevel`: order queue with a head index and dead entries left in place

Files: `include/me/BookLevel.h`, `src/me/BookLevel.cpp`

### Before
- **Fill:** after every fill, `std::erase_if` scanned the whole level and did a
  hash lookup for each resting order. That made every fill O(depth), even a
  1-lot fill. (The earlier `compact()` design rebuilt the vector into a newly
  allocated one.)
- **Cancel:** `lower_bound` + `vector::erase` moved every order behind the
  cancelled one.

### After
- New members: `head` (a `size_t`) and `live_orders`.
- `orders` holds IDs in FIFO order. IDs are allocated in increasing order, so
  this is also ascending ID order.
  - Entries before `head` were consumed by fills.
  - Dead entries at or after `head` are cancelled orders, or orders modified to
    quantity 0. They are skipped lazily.
- **Fill:** walks from `head` and stops at the first partial fill. A fully
  filled or dead order is retired by moving `head` forward. Fills never touch
  orders they don't trade with. When the level drains, `orders.clear()` resets
  it and keeps the capacity.
- **Cancel:** cancels the order in `OrderManager` and decrements `live_orders`.
  The ID stays in place, so there is no search and no shift.
- **`compact()`:** runs only when `push_back` would otherwise reallocate
  (`size() == capacity()`) and dead entries exist. It is a single in-place pass;
  the shrinking `resize()` never allocates. It only runs at a point where the old
  code would have paid an allocation plus a full copy anyway, so it adds no new
  latency spike.
- `getOrders()` and `getAllOrders()` iterate from `head`. `getAllOrders()` may
  include dead entries that haven't been compacted yet.
- **Zero-fill guard:** no trade is emitted when `filled_qty == 0`. This only
  happens for a 0-quantity resting order. See *Behaviour notes*.

---

## 2. `OrderBookSide`: sorted `std::vector<BookLevel>` instead of `std::map<int, shared_ptr<BookLevel>>`

File: `include/me/OrderBookSide.h`

### Why
- Every `map` node and every `make_shared<BookLevel>` was a heap allocation.
- Node-based containers are cache-unfriendly to traverse.
- Each `shared_ptr` copy in a view is an atomic refcount operation.

### Layout
- Levels are stored by value and sorted **worst → best**, so the best price is
  at `back()`:
  - BUY: ascending prices (highest bid at `back()`)
  - SELL: descending prices (lowest ask at `back()`)
- Most activity is at the top of the book, so adds, fills and level removals
  there touch the tail of the vector and shift little.
- `std::lower_bound` with an `isBetter` comparator finds or inserts a level.

### No allocations from level churn
- `levels.reserve(kReservedLevels)`, where `kReservedLevels = 1024`.
- **Level pool (`spare_levels`, also reserved to 1024):**
  - Erasing a level would destroy its order vector, and the next new level would
    allocate a new 4 KB vector (`reserve(512)`).
  - Instead, emptied levels are moved into the pool. A new price takes one back
    out and calls `BookLevel::reset(price)`, which keeps the order capacity.
  - When the pool is full, the emptied level is freed rather than growing the
    pool.
- **`fillOrders`** works directly on `levels.back()` and pops a drained level
  off the end. It no longer builds a `getCandidateLevels` vector on every match.
- **`cancelOrder` and `modifyOrder`** look up the level with a binary search and
  return the level to the pool when it empties.
- **`modifyOrder` with a price change:** `addOrder` may insert a level, which
  invalidates iterators. The old level is therefore looked up again inside
  `cancelOrder`.

### Removed
- `std::map`, `shared_ptr<BookLevel>`, `getCandidateLevels`, and the unused
  `marketPrice` member.

---

## 3. Views return price/quantity values

Files: `include/me/BookLevel.h`, `include/me/OrderBookSide.h`,
`include/me/OrderBook.h`, `src/me/OrderBook.cpp`, `include/me/MatchingEngine.h`

- New `struct LevelView { int price; int quantity; }` and
  `BookLevel::getView()`.
- These functions now return `std::vector<LevelView>`, best price first:
  `getBookSideView`, `getLevels`, `getBuySideView`, `getSellSideView` and
  `getOrderBookView`.
- `getLevel(price)` returns `std::optional<LevelView>` and uses binary search.
- A view is a snapshot of values, not a live handle. Building a view still
  allocates its result vector, but views are off the hot path. The risk check
  uses `getBestPrice()`.

### Knock-on changes
- **Python** (`src/python/module.cpp`, `python/matching_engine/_core.pyi`,
  `tests/python/test_bindings.py`):
  - `PriceLevel` is now `LevelView` itself.
  - **`PriceLevel.order_ids` was removed (API change).**
  - The `snapshot()` helper is gone, and `__repr__` no longer prints an order
    count.
- **`src/main.cpp`:** `printSide` prints only price and quantity per level.
- **Tests:**
  - `->getPrice()` / `->getTotalQuantity()` became `.price` / `.quantity`.
  - `restingOrders` (orders per level) was removed from
    `test_matching_engine.cpp`. Its checks were replaced with level-count checks,
    which are weaker.
- **Compile fix:** the earlier ranges version of `getBookSideView` lacked
  `std::views::common` and didn't compile. That code is now replaced entirely.

---

## Behaviour notes
- **Repricing an order to quantity 0** (`OrderBookSide::modifyOrder` with a new
  price and `newQuantity == 0`) still books a 0-quantity order and level, as the
  old `map` code did.
  - The engine's risk manager rejects quantity 0 before this point, so it is
    only reachable by calling `OrderBookSide` directly.
  - Matching retires such an order without emitting a trade.
- **Copying an `OrderBook`** now deep-copies its levels. Before, the copies
  shared levels through `shared_ptr`. Both copies still share one
  `OrderManager`, as before.

---

## Verification
- The Debug build is clean apart from older warnings (`-Wshadow` in
  `Events.h`/`BookLevel.cpp`, and the `int`→`unsigned` warnings in
  `OrderBook.cpp`). All 8 C++ test suites pass.
- **Randomized tests, built with ASan, UBSan and `_GLIBCXX_ASSERTIONS`:**
  - **`BookLevel` against a FIFO reference model:** 200 rounds × 3,000 ops,
    about 193k trades, all matching.
    - With few fills, so that `compact()` runs (4 times), it still matches.
  - **`OrderBookSide` against a `std::map` reference model, buy and sell:**
    2 × 50 rounds × 4,000 ops, about 110k trades.
    - After every op it compared each trade's ID, price and quantity, the
      remaining quantity, every level's price and total, and the top price.
    - Repricing to quantity 0 was excluded from this run; see *Behaviour notes*.
- **Allocation counts:**
  - Cancel and fill: **0**.
  - Add, in steady state: only the two allocations `OrderManager` makes for each
    order (the `Order` and its map node), plus a rare allocation that looks like
    `unordered_map` rehashing (not confirmed).
- **Not verified:**
  - The Python module wasn't compiled (pybind11 not found), so the Python
    tests weren't run.
  - The `bench/` latency benchmarks weren't run.

---

## Follow-ups
- **`Matcher::tryMatch`** creates a new `std::vector<TradeEvent>` on every match.
  This is the next hot-path allocation; a reusable member buffer would remove it.
- **`OrderManager`:** the `unordered_map` rehash, and the `make_unique<Order>`
  for every order. (The rehash is being looked into separately.)
- **Level search:** benchmark binary search against a linear scan from `back()`.
  Most inserts land within a few levels of the top. Storing prices in a separate
  contiguous array would also make the search more cache-friendly.
- **`BookLevel`:** holds a `shared_ptr<OrderManager>` (16 bytes). A raw pointer
  or reference would make each level smaller, which shrinks the data moved when
  levels shift.
- **Benchmarks:** run `bench_latency` in `build-release` to compare tail latency
  before and after.
