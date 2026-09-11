#pragma once

#include <cstddef>

#include "Events.h"
#include "lock_free_ring_buffer.h"

// The queue that carries events from the matching thread to the logger thread.
//
// This is the one place the capacity is chosen. Slots hold an EventVariant by
// value (40 bytes) rather than the unique_ptr<Event> they used to, so 128 slots
// is 5 KiB instead of 1 KiB -- still comfortably inside a 32 KiB L1d, and now the
// ring really is the whole event, not 128 pointers to scattered heap blocks.
//
// The capacity stayed at 128 through that change on purpose: it is sized by the
// burst a single matching operation can emit (a marketable order publishes one
// event per fill it sweeps), and shrinking it to keep the old byte count would
// have narrowed that headroom for no benefit.
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

static_assert(sizeof(EventRingBuffer<EventVariant>) <= 8192,
              "event queue should stay small enough to be cache resident");
