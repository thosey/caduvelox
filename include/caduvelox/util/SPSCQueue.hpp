#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <array>

namespace caduvelox {

/**
 * Lock-free Single Producer Single Consumer (SPSC) queue
 * Optimized for cross-thread communication between worker threads and io_uring thread
 * 
 * Features:
 * - Wait-free for producer (worker thread)
 * - Lock-free for consumer (io_uring thread)
 * - Cache-line padding to prevent false sharing
 * - Power-of-2 size for fast modulo operations
 */
template<typename T, size_t Size = 1024>
class SPSCQueue {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    
public:
    SPSCQueue() : head_(0), tail_(0) {
        // Note: Slots may not be cache-line sized for large T
        // Cache-line alignment on head_/tail_ still prevents false sharing
    }
    
    ~SPSCQueue() = default;
    
    // Non-copyable, non-movable
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    
    /**
     * Producer: Try to enqueue an item
     * Returns true if successful, false if queue is full
     */
    bool try_push(T&& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) & (Size - 1);
        
        // Check if queue is full
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Queue full
        }
        
        // Store item
        slots_[head].data = std::move(item);
        
        // Publish the new head (release semantics)
        head_.store(next_head, std::memory_order_release);
        return true;
    }
    
    /**
     * Consumer: Try to dequeue an item
     * Returns the item if available, std::nullopt if queue is empty
     */
    std::optional<T> try_pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        
        // Check if queue is empty
        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Queue empty
        }
        
        // Get item
        T item = std::move(slots_[tail].data);
        
        // Update tail (release semantics)
        const size_t next_tail = (tail + 1) & (Size - 1);
        tail_.store(next_tail, std::memory_order_release);
        
        return item;
    }
    
    /**
     * Check if queue is empty (approximate, may be stale)
     */
    bool empty() const {
        return tail_.load(std::memory_order_relaxed) == 
               head_.load(std::memory_order_relaxed);
    }
    
    /**
     * Get approximate number of items in queue
     */
    size_t size() const {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail) & (Size - 1);
    }
    
private:
    // Cache line size (typical for x86-64)
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    // Slot with cache-line padding to prevent false sharing
    struct alignas(CACHE_LINE_SIZE) Slot {
        T data;
    };
    
    // Head and tail on separate cache lines to prevent false sharing
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
    alignas(CACHE_LINE_SIZE) std::array<Slot, Size> slots_;
};

} // namespace caduvelox
