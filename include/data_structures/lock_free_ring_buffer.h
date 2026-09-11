#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <thread>
#include <utility>

// Single-producer / single-consumer lock-free ring buffer.
//
// CONTRACT: exactly one thread may call the push family, exactly one (other)
// thread may call the pop family. Two producers will both see space and write
// the same slot. This is not a general MPMC queue and must not be used as one.
//
// Design notes:
//   * _head and _tail are monotonically increasing counters, masked only when
//     used as an index. That makes "full" and "empty" distinguishable without
//     sacrificing a slot, so the usable capacity is the full N.
//   * Each index has exactly one writer, so no read-modify-write is needed
//     anywhere -- a release store and an acquire load carry all the ordering.
//   * Each thread keeps a private cached copy of the *other* thread's index and
//     only refreshes it when the buffer looks full (producer) or empty
//     (consumer). In steady state neither core touches the other's cache line.
//   * The two index/cache pairs sit on separate cache lines so that the
//     producer's writes do not invalidate the consumer's line and vice versa.
template<typename T, size_t N>
class RingBuffer {
    static_assert(N > 0, "RingBuffer capacity must be non-zero");
    static_assert((N & (N - 1)) == 0,
                  "RingBuffer capacity must be a power of two (index wrapping "
                  "uses a mask, and it keeps the counters exact across "
                  "size_t overflow)");

    private:
        // std::hardware_destructive_interference_size is the right value but is
        // not available everywhere, and GCC warns about its ABI stability when
        // it is used in a class layout. 64 is correct on x86-64 and arm64.
        static constexpr size_t CACHE_LINE = 64;
        static constexpr size_t MASK = N - 1;

        // Written by the producer only; read by the consumer.
        alignas(CACHE_LINE) std::atomic<size_t> _head{0};
        size_t _cached_tail{0};   // producer-private view of _tail

        // Written by the consumer only; read by the producer.
        alignas(CACHE_LINE) std::atomic<size_t> _tail{0};
        size_t _cached_head{0};   // consumer-private view of _head

        alignas(CACHE_LINE) std::array<T, N> _arr{};

        static void spin_pause() noexcept {
        #if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
        #elif defined(__aarch64__)
            __asm__ __volatile__("yield");
        #else
            std::this_thread::yield();
        #endif
        }

        // Producer side: is there room to publish index `next`? Refreshes the
        // cached tail only when the buffer looks full, which is the only time
        // the consumer's cache line has to be touched.
        bool has_space(size_t next) noexcept {
            if (next - _cached_tail <= N) return true;
            _cached_tail = _tail.load(std::memory_order_acquire);
            return next - _cached_tail <= N;
        }

        // Consumer side: is slot `tail` populated? Refreshes the cached head
        // only when the buffer looks empty.
        bool has_item(size_t tail) noexcept {
            if (tail != _cached_head) return true;
            _cached_head = _head.load(std::memory_order_acquire);
            return tail != _cached_head;
        }

    public:
        RingBuffer() = default;

        // The atomics already make these ill-formed; say so explicitly.
        RingBuffer(const RingBuffer&) = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;

        // ---- producer ----

        // Spins until a slot is free. Only safe when a consumer is running on
        // another thread; single-threaded this is an infinite loop.
        void push(T data) {
            const size_t head = _head.load(std::memory_order_relaxed);
            const size_t next = head + 1;
            while (!has_space(next)) spin_pause();
            _arr[head & MASK] = std::move(data);
            // Release: the slot write above must be visible to any consumer
            // that observes this new head.
            _head.store(next, std::memory_order_release);
        }

        // Non-blocking form. Returns false (leaving `data` untouched) if full.
        bool try_push(T data) {
            const size_t head = _head.load(std::memory_order_relaxed);
            const size_t next = head + 1;
            if (!has_space(next)) return false;
            _arr[head & MASK] = std::move(data);
            _head.store(next, std::memory_order_release);
            return true;
        }

        // ---- consumer ----

        void wait_and_pop(T& result) {
            const size_t tail = _tail.load(std::memory_order_relaxed);
            while (!has_item(tail)) spin_pause();
            result = std::move(_arr[tail & MASK]);
            // Release: the read above must complete before the producer can
            // observe the free slot and overwrite it.
            _tail.store(tail + 1, std::memory_order_release);
        }

        std::shared_ptr<T> wait_and_pop() {
            const size_t tail = _tail.load(std::memory_order_relaxed);
            while (!has_item(tail)) spin_pause();
            // Constructed before _tail advances: if the allocation throws, the
            // slot is still owned by the queue and nothing is lost.
            std::shared_ptr<T> res = std::make_shared<T>(std::move(_arr[tail & MASK]));
            _tail.store(tail + 1, std::memory_order_release);
            return res;
        }

        bool try_pop(T& result) {
            const size_t tail = _tail.load(std::memory_order_relaxed);
            if (!has_item(tail)) return false;
            result = std::move(_arr[tail & MASK]);
            _tail.store(tail + 1, std::memory_order_release);
            return true;
        }

        std::shared_ptr<T> try_pop() {
            const size_t tail = _tail.load(std::memory_order_relaxed);
            if (!has_item(tail)) return std::shared_ptr<T>();
            std::shared_ptr<T> res = std::make_shared<T>(std::move(_arr[tail & MASK]));
            _tail.store(tail + 1, std::memory_order_release);
            return res;
        }

        // ---- observers ----
        //
        // Exact for the owning thread (the consumer for empty(), either thread
        // when quiescent); for anyone else they are a snapshot that may be stale
        // the instant they return. _tail is loaded first on purpose: it can only
        // ever catch up to _head, so reading it first keeps the subtraction from
        // wrapping if the producer advances mid-call.

        bool empty() const noexcept {
            const size_t tail = _tail.load(std::memory_order_acquire);
            return _head.load(std::memory_order_acquire) == tail;
        }

        size_t size() const noexcept {
            const size_t tail = _tail.load(std::memory_order_acquire);
            return _head.load(std::memory_order_acquire) - tail;
        }

        static constexpr size_t capacity() noexcept { return N; }
};
