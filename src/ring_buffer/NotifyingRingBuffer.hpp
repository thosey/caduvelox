#pragma once

#include "MPMCRingBuffer.hpp"
#include <atomic>
#include <chrono>
#include <semaphore>

/**
 * A decorator around MPMCRingBuffer that adds efficient blocking/notification
 * for producers and consumers, eliminating busy-waiting.
 * 
 * Uses C++20 atomic wait/notify for optimal performance:
 * - Producers notify waiting consumers when data is available
 * - Consumers block efficiently instead of busy-waiting
 * - Maintains the lock-free properties of the underlying MPMCRingBuffer
 * 
 * Template parameters:
 * @param T - Type of elements stored in the buffer
 * @param N - Size of the ring buffer (must be power of 2)
 */
template<typename T, size_t N>
class NotifyingRingBuffer {
public:
    static_assert((N & (N - 1)) == 0, "Buffer size must be a power of 2");

    // Queue depth at which a producer signals the consumer. Below it, the
    // consumer's bounded park picks the items up instead. See wakeIfWorthIt().
    static constexpr size_t NOTIFY_WATERMARK = 16;

    // Longest a consumer parks before looking again. This is the latency bound on
    // an item that never reaches the watermark -- a single log line on an
    // otherwise idle server.
    static constexpr std::chrono::milliseconds MAX_PARK{1};
    
    NotifyingRingBuffer() = default;
    
    // Non-copyable, non-movable (like MPMCRingBuffer)
    NotifyingRingBuffer(const NotifyingRingBuffer&) = delete;
    NotifyingRingBuffer& operator=(const NotifyingRingBuffer&) = delete;
    NotifyingRingBuffer(NotifyingRingBuffer&&) = delete;
    NotifyingRingBuffer& operator=(NotifyingRingBuffer&&) = delete;
    
    /**
     * Enqueue an item (move version).
     * If successful, notifies waiting consumers.
     * 
     * @param item - Item to enqueue (will be moved)
     * @return true if enqueued successfully, false if buffer is full
     */
    bool enqueue(T&& item) {
        bool success = ring_.enqueue(std::move(item));
        if (success) {
            wakeIfWorthIt();
        }
        return success;
    }
    
    /**
     * Enqueue an item (copy version).
     * If successful, notifies waiting consumers.
     * 
     * @param item - Item to enqueue (will be copied)
     * @return true if enqueued successfully, false if buffer is full
     */
    bool enqueue(const T& item) {
        bool success = ring_.enqueue(item);
        if (success) {
            wakeIfWorthIt();
        }
        return success;
    }
    
    /**
     * Dequeue an item with efficient blocking.
     * If no data is available, blocks until notified by a producer.
     * 
     * @param item - Reference to store the dequeued item
     * @return true if dequeued successfully, false if shutdown was signaled
     */
    bool dequeue(T& item) {
        for (;;) {
            // Drain without sleeping for as long as there is anything to take, so
            // one wake-up serves a whole batch.
            if (ring_.dequeue(item)) {
                return true;
            }
            if (shutdown_.load(std::memory_order_acquire)) {
                return false;
            }

            // Park, but only for a bounded time. Producers deliberately do not
            // signal every item (see wakeIfWorthIt), so the timeout is what
            // collects the last few of a batch -- and it means a missed signal
            // costs latency, never a lost item.
            sem_.try_acquire_for(MAX_PARK);

            if (shutdown_.load(std::memory_order_acquire)) {
                // Take anything still queued before reporting shutdown; the
                // caller drains with try_dequeue afterwards either way.
                return ring_.dequeue(item);
            }
        }
    }
    
    /**
     * Signal shutdown to all waiting consumers.
     * This will cause all blocking dequeue operations to return false.
     */
    void shutdown() {
        shutdown_.store(true, std::memory_order_release);
        notify_all();  // wake the consumer immediately; do not make it wait out MAX_PARK
    }
    
    /**
     * Non-blocking dequeue attempt.
     * Returns immediately whether data is available or not.
     * 
     * @param item - Reference to store the dequeued item
     * @return true if dequeued successfully, false if buffer is empty
     */
    bool try_dequeue(T& item) {
        return ring_.dequeue(item);
    }
    
    /**
     * Wake all waiting consumers.
     * Useful for shutdown scenarios where you want to wake sleeping threads.
     */
    void notify_all() {
        sem_.release();
    }

private:
    /**
     * Wake the consumer only when a batch has built up.
     *
     * Signalling every item is what made this expensive: the consumer drains in
     * well under a microsecond, so it was parked again by the time the next item
     * arrived, and every item cost a futex wake plus a futex wait. Measured
     * through AsyncLogger at seven log lines per HTTP request, that was +19 us of
     * CPU per request -- more than logging synchronously.
     *
     * Waking once per batch trades a little latency for those syscalls, and the
     * consumer's bounded park (MAX_PARK) is what bounds the trade: an item that
     * does not reach the watermark waits at most that long. Semaphore tokens also
     * persist, unlike a condition-variable signal, so a token released just as the
     * consumer decides to park is still there when it does.
     */
    void wakeIfWorthIt() {
        if (ring_.size_approx() >= NOTIFY_WATERMARK) {
            sem_.release();
        }
    }

    MPMCRingBuffer<T, N> ring_;
    // Tokens can accumulate when producers outrun the consumer; a surplus token
    // only costs one extra trip around the drain loop.
    std::counting_semaphore<> sem_{0};
    std::atomic<bool> shutdown_{false};
};
