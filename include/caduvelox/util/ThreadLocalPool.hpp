#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

namespace caduvelox {

/**
 * ThreadLocalPool - Fast slab allocator with intrusive free list
 * 
 * This pool uses a single contiguous slab of memory with an intrusive
 * free list. Each slot is a union that contains either a pointer to the
 * next free slot, or storage for an object of type T.
 * 
 * Benefits:
 * - Single contiguous allocation (better cache locality)
 * - Zero allocation overhead per object (intrusive free list)
 * - O(1) allocation and deallocation (pointer manipulation only)
 * - No atomic operations (thread-local)
 * 
 * Usage:
 *   thread_local ThreadLocalPool<MyType> pool(1000);
 *   auto* obj = pool.allocate(arg1, arg2);
 *   pool.deallocate(obj);
 */
template<typename T>
class ThreadLocalPool {
private:
    // Union: either points to next free slot, or holds object storage
    union Slot {
        Slot* next;  // When free - intrusive linked list pointer
        alignas(T) std::byte storage[sizeof(T)];  // When allocated - holds T
    };

public:
    explicit ThreadLocalPool(size_t capacity)
        : capacity_(capacity)
        , slab_(nullptr)
        , free_head_(nullptr)
    {
        if (capacity == 0) {
            return;
        }

        // Allocate aligned slab
        constexpr size_t alignment = alignof(Slot);
        size_t size = capacity * sizeof(Slot);
        
        // aligned_alloc requires size to be multiple of alignment
        size = (size + alignment - 1) & ~(alignment - 1);
        
        slab_ = static_cast<Slot*>(std::aligned_alloc(alignment, size));
        if (!slab_) {
            throw std::bad_alloc();
        }
        
        // Initialize intrusive free list - link all slots together
        for (size_t i = 0; i < capacity - 1; ++i) {
            (slab_ + i)->next = slab_ + i + 1;
        }
        // Terminate the list
        (slab_ + capacity - 1)->next = nullptr;
        free_head_ = slab_;
    }

    ~ThreadLocalPool() {
        if (slab_) {
            std::free(slab_);
        }
    }

    // Non-copyable, non-movable
    ThreadLocalPool(const ThreadLocalPool&) = delete;
    ThreadLocalPool& operator=(const ThreadLocalPool&) = delete;

    /**
     * Allocate an object from the pool with perfect forwarding
     */
    template<typename... Args>
    T* allocate(Args&&... args) {
        if (!free_head_) {
            return nullptr; // Pool exhausted
        }

        // Pop from free list
        Slot* slot = free_head_;
        free_head_ = slot->next;

        // Construct object in slot's storage
        T* ptr = reinterpret_cast<T*>(slot->storage);
        new (ptr) T(std::forward<Args>(args)...);
        
        return ptr;
    }

    /**
     * Return an object to the pool
     */
    void deallocate(T* ptr) {
        if (!ptr) return;

        // Destroy the object
        ptr->~T();
        
        // Push back to free list (intrusive)
        Slot* slot = reinterpret_cast<Slot*>(ptr);
        slot->next = free_head_;
        free_head_ = slot;
    }

    /**
     * Get current number of available objects
     */
    size_t available() const {
        size_t count = 0;
        Slot* current = free_head_;
        while (current) {
            ++count;
            current = current->next;
        }
        return count;
    }

    /**
     * Get total capacity
     */
    size_t capacity() const {
        return capacity_;
    }

    /**
     * Get number of allocated objects
     */
    size_t allocated() const {
        return capacity_ - available();
    }

private:
    size_t capacity_;
    Slot* slab_;        
    Slot* free_head_;   
};

} // namespace caduvelox
