#pragma once

#include "caduvelox/util/ThreadLocalPool.hpp"
#include <memory>

namespace caduvelox {

/**
 * PoolManager - Provides thread-local object pools for per-ring architecture
 * 
 * This manager provides access to thread-local pools for various job types.
 * Each ring thread gets its own independent pools with zero synchronization.
 * 
 * Usage in SingleRingHttpServer:
 *   auto* job = PoolManager::allocate<HttpConnectionJob>(args...);
 *   PoolManager::deallocate(job);
 */
class PoolManager {
public:
    /**
     * Allocate an object from the thread-local pool
     */
    template<typename T, typename... Args>
    static T* allocate(Args&&... args) {
        auto& pool = getPool<T>();
        return pool.allocate(std::forward<Args>(args)...);
    }

    /**
     * Deallocate an object back to the thread-local pool
     */
    template<typename T>
    static void deallocate(T* ptr) {
        auto& pool = getPool<T>();
        pool.deallocate(ptr);
    }

    /**
     * Get stats for a specific pool type
     */
    template<typename T>
    static size_t available() {
        return getPool<T>().available();
    }

    template<typename T>
    static size_t allocated() {
        return getPool<T>().allocated();
    }

private:
    template<typename T>
    static ThreadLocalPool<T>& getPool() {
        // Thread-local pool per type
        // Capacity tuned based on expected usage
        constexpr size_t capacity = getPoolCapacity<T>();
        thread_local ThreadLocalPool<T> pool(capacity);
        return pool;
    }

    // Pool capacity specializations based on usage patterns
    // Default capacity - can be specialized near type definitions
    template<typename T>
    static constexpr size_t getPoolCapacity() {
        return 1000; // Default capacity for unknown types
    }
};

// Specializations should be defined near the type definitions
// See: SingleRingHttpServer.hpp, WriteJob.cpp, KTLSJob.cpp, etc.

} // namespace caduvelox
