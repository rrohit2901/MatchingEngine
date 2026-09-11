#pragma once

#include <cstddef>
#include <memory>

#include "Events.h"
#include "lock_free_ring_buffer.h"

// The queue that carries events from the matching thread to the logger thread.
//
// This is the one place the capacity is chosen. 128 slots of
// std::unique_ptr<Event> is 1 KiB -- sixteen cache lines, so the whole ring
// stays resident in L1 while the logger is keeping up. It only has to absorb
// the burst a single matching operation can emit (a marketable order publishes
// one event per fill it sweeps), not the whole session; the logger drains
// continuously behind it.
//
// The trade-off versus LockQueue: this queue is *bounded*. When the logger
// falls behind -- a slow disk, a large flush -- the matching thread spins in
// push() instead of allocating another node. That is backpressure rather than
// unbounded memory growth, and it is the behaviour you want on a matching path,
// but it does mean file I/O can now stall the producer. Raise the capacity if
// the logger's worst-case pause is longer than the burst this absorbs.
inline constexpr size_t kEventQueueCapacity = 128;

// RingBuffer takes <typename, size_t>, but Logger/MatchingEngine/EventManager
// all take a `template<typename> class` container. This alias binds the
// capacity so the ring buffer can be passed as that template template argument.
template<typename T>
using EventRingBuffer = RingBuffer<T, kEventQueueCapacity>;

static_assert(sizeof(EventRingBuffer<std::unique_ptr<Event>>) <= 4096,
              "event queue should stay small enough to be cache resident");
