#pragma once

#include "caduvelox/util/ThreadLocalPool.hpp"
#include <cassert>
#include <memory>

namespace caduvelox {

/**
 * Per-type pool capacity configuration.
 *
 * The primary template provides a default of 1000.
 * Type-specific defaults are defined as explicit specializations near each type
 * (see KTLSJob.cpp, AcceptJob.cpp, WriteJob.cpp, etc.).
 * HttpServer overrides these at runtime via PoolManager::setCapacity<T>() before
 * starting ring threads.
 */
template<typename T>
struct PoolCapacityConfig {
    static size_t capacity;
};

// Primary template default — explicit specializations override this.
template<typename T>
inline size_t PoolCapacityConfig<T>::capacity = 1000;

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
     * Invoke callback(T&) for every live object in the thread-local pool for T.
     * Must only be called on the ring thread that owns the pool.
     * The callback must not allocate or deallocate T from this pool.
     */
    template<typename T, typename Callback>
    static void sweepLive(Callback&& callback) {
        getPool<T>().sweepLive(std::forward<Callback>(callback));
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

    /**
     * Get the current capacity for pool type T.
     * Useful for tests that need to save and restore capacity values.
     */
    template<typename T>
    static size_t getCapacity() {
        return PoolCapacityConfig<T>::capacity;
    }

    /**
     * Override the pool capacity for type T.
     * Must be called before the first allocation on any ring thread that should
     * use the new value — thread-local pools are sized on first access.
     * Typical caller: HttpServer constructor, before starting ring threads.
     */
    template<typename T>
    static void setCapacity(size_t cap) {
        assert(cap > 0 && "PoolManager::setCapacity: capacity must be > 0");
        PoolCapacityConfig<T>::capacity = cap;
    }

private:
    template<typename T>
    static ThreadLocalPool<T>& getPool() {
        // Thread-local pool per type.
        // Capacity is read once per thread on first access — set PoolCapacityConfig<T>::capacity
        // before starting ring threads so they pick up the right value.
        thread_local ThreadLocalPool<T> pool(PoolCapacityConfig<T>::capacity);
        return pool;
    }
};

// Specializations are defined near the type definitions.
// See: SingleRingHttpServer.hpp, CancelJob.hpp, WriteJob.cpp, KTLSJob.cpp, etc.

} // namespace caduvelox
