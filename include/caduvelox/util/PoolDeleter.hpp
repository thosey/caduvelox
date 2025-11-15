#pragma once

#include "LockFreeMemoryPool.h"

namespace caduvelox {

/**
 * Custom deleter for shared_ptr that returns objects to lock-free pool
 * instead of calling delete. This allows shared_ptr to use pool-allocated
 * memory while maintaining reference counting safety.
 * 
 * Usage:
 *   auto obj = std::shared_ptr<MyType>(
 *       lfmemorypool::lockfree_pool_alloc_fast<MyType>(...),
 *       PoolDeleter<MyType>()
 *   );
 */
template<typename T>
struct PoolDeleter {
    void operator()(T* ptr) const {
        if (ptr) {
            // Return to pool instead of delete
            lfmemorypool::lockfree_pool_free_fast<T>(ptr);
        }
    }
};

/**
 * Helper to create shared_ptr from pool with proper deleter
 * 
 * Usage:
 *   auto obj = makeSharedFromPool<MyType>(arg1, arg2, ...);
 */
template<typename T, typename... Args>
std::shared_ptr<T> makeSharedFromPool(Args&&... args) {
    T* ptr = lfmemorypool::lockfree_pool_alloc_fast<T>(std::forward<Args>(args)...);
    if (!ptr) {
        return nullptr; // Pool exhausted
    }
    return std::shared_ptr<T>(ptr, PoolDeleter<T>());
}

} // namespace caduvelox
