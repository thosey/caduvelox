#pragma once

#include <atomic>
#include <cassert>
#include <vector>

/**
 * Reference: "Bounded MPMC queue" by Dmitry Vyukov
 * http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
 */

constexpr size_t fast_mod(size_t index, size_t capacity) {
    return index & (capacity - 1);
}

constexpr bool is_power_of_two(size_t n) {
    return (n & (n - 1)) == 0;
}

template <typename T>
struct Slot {
    std::atomic<size_t> sequence;
    T data;                     

    explicit Slot(size_t initial_sequence = 0) : sequence(initial_sequence), data{} {}

    Slot(Slot&& other) noexcept
        : sequence(other.sequence.load(std::memory_order_relaxed)), data(std::move(other.data)) {}

    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    Slot& operator=(Slot&&) = delete;
};

template <typename T, size_t Size>
class MPMCRingBuffer {
   private:
    static_assert(Size > 0 && is_power_of_two(Size), "Size must be a power of 2");
    
    Slot<T> slots_[Size];        
    std::atomic<size_t> head_;   
    std::atomic<size_t> tail_;   

   public:
    MPMCRingBuffer() : head_(0), tail_(0) {
        for(size_t i = 0; i < Size; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool enqueue(T&& item) {
        size_t pos = head_.load(std::memory_order_relaxed);

        while(true) {
            Slot<T>& slot = slots_[fast_mod(pos, Size)];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if(diff == 0) {
                if(head_.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
                    slot.data = std::move(item);
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if(diff < 0) {
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * Enqueue operation for producers (copy version)
     * @param item Item to enqueue (copied into the buffer)
     * @return true if successfully enqueued, false if buffer full
     */
    bool enqueue(const T& item) {
        T copy = item;
        return enqueue(std::move(copy));
    }

    bool dequeue(T& item) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        while(true) {
            Slot<T>& slot = slots_[fast_mod(pos, Size)];
            size_t seq = slot.sequence.load(std::memory_order_acquire);

            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if(diff == 0) {
                if(tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
                    item = std::move(slot.data);
                    slot.sequence.store(pos + Size, std::memory_order_release);
                    return true;
                }
            } else if(diff < 0) {
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }
};