# Order Book — Preliminary Design Note (Stage 1)

## Scope

- Single-instrument central limit order book.
- Operations: add, cancel, modify (qty only), best bid / best ask / spread / mid,
  total open qty, (optional) depth-at-level.
- No matching in this stage. Orders rest as-is even if they would cross.

## Project constraints (inherited)

- Single symbol in the core.
- Single-threaded matching core; concurrency only at the boundaries.
  → The book is owned and mutated by exactly one thread. All book operations,
    including compaction, run serially on that thread.
- C++20.
- Fixed-point integers for all prices/quantities. No floats.

---

## Data structures

### `Order`

Fields:
- `price`
- `direction` (buy / sell)
- `quantity`
- `isValid` (tombstone flag for soft-delete)
- `orderID`

Methods:
- `cancel()` → set `isValid = false`. **O(1)**
- `modifyQty(newQty)` → change quantity. **O(1)** (see Modify semantics below)

> Memory layout: TODO — decide field ordering / packing deliberately.

### `BookLevel` (one price level, single-sided)

- `orders` → `vector<Order>`
  - Chose a **vector over a linked list** for cache friendliness (contiguous
    storage, predictable iteration).
  - To keep deletes O(1) without shifting, deletion is a soft-delete via the
    `isValid` tombstone on the order rather than an erase.
- `totalQty` → running sum of valid quantity at this level.
  - Maintained incrementally on add / cancel / (later) fill, **not** recomputed
    on query. Keeping it also lets me delete a level cleanly when it hits 0.

Methods:
- `addOrder(order)` → append to the back of the vector, bump `totalQty`. **O(1)**
  - Append order = arrival order = **time priority** within the level (FIFO).
- `getQty()` → return `totalQty`. **O(1)**
- `getOrders()` → iterate, skipping tombstoned orders.

### `OrderBook`

- **Two maps, one per side** (book is single-sided per level):
  - `bids` → `map<price, BookLevel>` ordered so best (highest) bid is reachable in O(1)
  - `asks` → `map<price, BookLevel>` ordered so best (lowest) ask is reachable in O(1)
  - Chose a tree-based map to maintain a **dynamic, sorted set of price levels**
    (levels appear/disappear arbitrarily; I need sorted top-of-book).
- `order_map` → `unordered_map<orderID, OrderLocation>`
  - `OrderLocation = { index_into_level_vector, stable_level_ref }`
  - The level reference is stable because it's a reference to a `map` mapped value
    (it survives insertion/erasure of *other* levels).
  - The index is stable across vector growth (reallocation preserves indices);
    it is **not** stable across compaction — see Compaction.
- `get_new_order_id()` (private) → mint internal order IDs.

---

## Operation walkthroughs

### Add
1. Create the order. **O(1)**
2. Insert into `order_map`. **O(1)**
3. Find/create the price level in the side's map. **O(log L)** (L = #levels)
4. Append to the level's vector + bump `totalQty`. **O(1)**

### Cancel (by ID)
1. Look up in `order_map` → get order location + level ref. **O(1)**
2. Flip `isValid = false`. **O(1)**
3. Decrement the level's `totalQty`. **O(1)**
4. If level `totalQty == 0`, delete the level. **O(log L)** in that case.

### Modify (qty only) — branch on direction of change
- **Quantity decrease** → modify the order in place, keep its queue position
  (retains time priority). Adjust `totalQty` by the delta. **O(1)**
- **Quantity increase** → cancel + re-add (re-add to the back of the level),
  which loses time priority. Same `orderID` preserved.

### Best bid / ask / spread / mid
- Best bid = top of `bids`, best ask = top of `asks`. **O(1)**
- Spread / mid computed from those two.

### Total open qty / depth
- Per-level qty = `getQty()`. **O(1)**
- (Optional) depth-at-level query: TODO.

---

## Tombstones & compaction

- Cancels and (later) fills leave tombstoned entries in the level vector.
- Reclaim space with **load-factor-triggered compaction**: when a level's dead
  ratio crosses a threshold, sweep the vector, drop tombstones, and shift the
  survivors down to close gaps (preserving FIFO order).
- Compaction shifts surviving orders, so their indices change → I must
  **reindex** the affected `order_map` entries during the sweep.
- Compaction runs on the single matching thread, serially with other book ops
  (no concurrency concern at this stage).

---

## Order ID handling

- This is an exchange-side matching engine, so the engine **assigns order IDs**
  via `get_new_order_id()`.

---

## Complexity summary

| Operation        | Target          |
|------------------|-----------------|
| add              | O(log L)        |
| cancel           | O(1)*           |
| modify (qty down)| O(1)            |
| modify (qty up)  | O(1) + re-add   |
| best bid/ask     | O(1)            |
| level qty        | O(1)            |

\* amortized; periodic compaction is O(n) over the affected level.

---

## Open questions to resolve before committing

- **Order handle vs. compaction:** index-into-vector is stable across growth but
  not across compaction (which is why I reindex). Confirm the reindex cost is
  acceptable and document cancel as "O(1) amortized with periodic O(n) bursts."
- **Compaction parameters:** what dead-ratio threshold? Per-level or global? Add
  an absolute floor (don't reindex tiny levels)? Hysteresis to avoid thrash?
- **When does compaction run?** Piggyback on an operation that already touches
  the level, or a separate pass? What about cold, tombstone-heavy levels?
- **`order_map` hygiene:** when an order dies and/or a level is deleted, how/when
  are its `order_map` entries removed so no stale level handle survives?
- **Modify edge cases:** modify-to-zero (= cancel?), modify-to-same-qty (no-op?),
  and is price-changing modify in scope at all for Stage 1?
- **Replay (Stage 3) ID reconciliation:** the replay feed carries the exchange's
  own order reference numbers. How do engine-generated IDs reconcile with the
  feed's IDs so subsequent execute/cancel/delete messages map to the right order?
- **`Order` memory layout:** field ordering / packing decision + rationale.
- **Data-structure trade-offs:** capture the tree-map cost (allocation, cache)
  for the Stage 6 optimization write-up.

- Optimization 1: Create a order manager which maintains a list of valid ids and hence cleans up the memory as we go. It will talk to the BookLevel and OrderBook, to optimize some operations. Hence, all book levels need to store is the order id and not the pointer object.