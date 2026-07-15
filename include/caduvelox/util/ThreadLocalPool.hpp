#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <utility>
#include <vector>

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
 * Generation tracking:
 * Each slot carries a generation counter that is incremented on deallocate.
 * Code that stores a raw pointer to a pool object across an async boundary
 * (e.g. an io_uring completion that may arrive after the object was freed)
 * can capture generationOf(ptr) alongside the pointer and later check
 * isLive(ptr, gen) before dereferencing. A recycled slot has a different
 * generation, so stale references are detected instead of causing
 * use-after-free / ABA bugs.
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
        , live_(capacity, 0)
        , generations_(capacity, 0)
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
            // Destroy any objects still live so their resources (fds, owned
            // buffers, SSL contexts) are released. Objects live at pool
            // destruction indicate a leak upstream; warn via stderr because
            // the Logger may already be torn down during thread exit.
            size_t leaked = 0;
            for (size_t i = 0; i < capacity_; ++i) {
                if (live_[i]) {
                    reinterpret_cast<T*>(slab_[i].storage)->~T();
                    ++leaked;
                }
            }
            if (leaked > 0) {
                std::fprintf(stderr,
                             "ThreadLocalPool: destroyed %zu object(s) still live at pool teardown\n",
                             leaked);
            }
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

        // Mark slot as live
        live_[static_cast<size_t>(slot - slab_)] = 1;

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

        // Mark slot as free and invalidate any outstanding (ptr, generation)
        // references held across async boundaries.
        Slot* slot = reinterpret_cast<Slot*>(ptr);
        size_t index = static_cast<size_t>(slot - slab_);
        live_[index] = 0;
        ++generations_[index];

        // Push back to free list (intrusive)
        slot->next = free_head_;
        free_head_ = slot;
    }

    /**
     * Get the current generation of the slot holding ptr.
     * Capture this alongside the pointer when storing it across an async
     * boundary; validate later with isLive(ptr, generation).
     * ptr must be a pointer previously returned by allocate() on this pool.
     */
    uint64_t generationOf(const T* ptr) const {
        assert(ownsPointer(ptr) && "generationOf: pointer not from this pool");
        return generations_[slotIndex(ptr)];
    }

    /**
     * Check whether ptr still refers to the same live object it did when
     * generation was captured. Returns false if the slot was freed (or freed
     * and recycled for a new object) since then, or if ptr is not from this
     * pool's slab.
     */
    bool isLive(const T* ptr, uint64_t generation) const {
        if (!ownsPointer(ptr)) {
            return false;
        }
        size_t index = slotIndex(ptr);
        return live_[index] != 0 && generations_[index] == generation;
    }

    /**
     * Iterate every live (currently allocated) object and invoke callback(T&).
     * Must only be called on the thread that owns this pool.
     * The callback must not allocate or deallocate from this pool.
     */
    template<typename Callback>
    void sweepLive(Callback&& callback) {
        for (size_t i = 0; i < capacity_; ++i) {
            if (live_[i]) {
                callback(*reinterpret_cast<T*>(slab_[i].storage));
            }
        }
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
    size_t slotIndex(const T* ptr) const {
        return static_cast<size_t>(reinterpret_cast<const Slot*>(ptr) - slab_);
    }

    bool ownsPointer(const T* ptr) const {
        if (!slab_ || !ptr) {
            return false;
        }
        const Slot* slot = reinterpret_cast<const Slot*>(ptr);
        return slot >= slab_ && slot < slab_ + capacity_;
    }

    size_t capacity_;
    Slot* slab_;
    Slot* free_head_;
    std::vector<uint8_t> live_;        // 1 = slot is live; 0 = slot is free
    std::vector<uint64_t> generations_; // bumped on deallocate; detects stale refs
};

} // namespace caduvelox
