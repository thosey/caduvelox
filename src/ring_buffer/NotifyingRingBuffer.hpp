#pragma once

#include "MPMCRingBuffer.hpp"
#include <atomic>

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
            // Notify waiting consumers that data is available
            has_data_.store(true, std::memory_order_release);
            has_data_.notify_one();
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
            // Notify waiting consumers that data is available
            has_data_.store(true, std::memory_order_release);
            has_data_.notify_one();
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
        if (ring_.dequeue(item)) {
            return true;
        }
        
        while (!shutdown_.load(std::memory_order_acquire)) {
            has_data_.wait(false, std::memory_order_acquire);
            
            if (shutdown_.load(std::memory_order_acquire)) {
                return false;
            }
            if (ring_.dequeue(item)) {
                return true;
            }
            has_data_.store(false, std::memory_order_release);
        }
        
        return false; // Shutdown signaled
    }
    
    /**
     * Signal shutdown to all waiting consumers.
     * This will cause all blocking dequeue operations to return false.
     */
    void shutdown() {
        shutdown_.store(true, std::memory_order_release);
        notify_all();
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
        has_data_.store(true, std::memory_order_release);
        has_data_.notify_all();
    }

private:
    MPMCRingBuffer<T, N> ring_;
    std::atomic<bool> has_data_{false};
    std::atomic<bool> shutdown_{false};
};
