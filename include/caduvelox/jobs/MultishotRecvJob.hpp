#pragma once

#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/ProvidedBufferToken.hpp"
#include "LockFreeMemoryPool.h"
#include <liburing.h>
#include <memory>
#include <errno.h>
#include <concepts>

namespace caduvelox {

/**
 * Concept: ResponseHandler for MultishotRecvJob
 * 
 * A valid handler must provide:
 * - onDataToken(std::shared_ptr<ProvidedBufferToken> token, void* worker_pool_ptr) for zero-copy processing
 * - onError(int error) for error handling
 */
template<typename H>
concept ResponseHandler = requires(H handler, 
                                   std::shared_ptr<ProvidedBufferToken> token,
                                   void* worker_pool_ptr,
                                   int error) {
    // Required: zero-copy token processing
    { handler.onDataToken(token, worker_pool_ptr) } -> std::same_as<void>;
    
    // Required: error handling
    { handler.onError(error) } -> std::same_as<void>;
};

/**
 * MultishotRecvJob - Zero-cost abstraction for persistent multishot socket reads with affinity workers
 * 
 * Template-based policy design for optimal performance:
 * - Handler encapsulates context + callbacks (no void* casting)
 * - Direct member calls inline perfectly (no function pointer overhead)
 * - Fully type-safe (compiler checks all types)
 * - Zero heap allocation
 * - Zero-copy buffer token passing to affinity workers
 * 
 * Handler Requirements:
 * - void onDataToken(std::shared_ptr<ProvidedBufferToken> token, void* worker_pool_ptr)
 * - void onError(int error)
 * 
 * Example Handler:
 * struct HttpConnectionHandler {
 *     HttpConnectionJob* connection;
 *     void onDataToken(std::shared_ptr<ProvidedBufferToken> token, void* worker_pool_ptr) {
 *         connection->handleDataReceivedToken(token, worker_pool_ptr);
 *     }
 *     void onError(int error) {
 *         connection->handleReadError(error);
 *     }
 * };
 * 
 * Usage:
 *   HttpConnectionHandler handler{this};
 *   auto* job = MultishotRecvJob<HttpConnectionHandler>::createFromPool(fd, handler, worker_pool.get());
 * 
 * Lifecycle:
 * - Created once per connection (pool-allocated per handler type)
 * - Kernel automatically continues until connection closes or error
 * - Returns cleanup callback when connection terminates
 */
template<ResponseHandler Handler>
class MultishotRecvJob : public IoJob {
public:
    ~MultishotRecvJob() = default;

    /**
     * Create pool-allocated MultishotRecvJob with zero-copy token callback
     * Buffer lifetime extends until token is destroyed (for worker threads).
     * 
     * @param fd File descriptor to read from
     * @param handler Handler instance (contains context + callbacks)
     * @param worker_pool_ptr Pointer to worker pool (required for zero-copy dispatch)
     * @return Pool-allocated job or nullptr if pool exhausted
     */
    static MultishotRecvJob* createFromPool(int fd, 
                                           Handler handler,
                                           void* worker_pool_ptr);

    /**
     * Free pool-allocated job (for error cleanup before registration)
     */
    static void freePoolAllocated(MultishotRecvJob* job);

    // IoJob interface
    void prepareSqe(struct io_uring_sqe* sqe) override;
    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;

    // Constructor must be public for pool allocation
    MultishotRecvJob(int fd, Handler handler, void* worker_pool_ptr);

private:
    int fd_;
    Handler handler_;              // Handler encapsulates context + callbacks
    void* worker_pool_ptr_;        // For zero-copy worker dispatch

    static void cleanupMultishotRecvJob(IoJob* job);
};

// ============================================================================
// Implementation (header-only for templates)
// ============================================================================

template<ResponseHandler Handler>
MultishotRecvJob<Handler>::MultishotRecvJob(int fd, Handler handler, void* worker_pool_ptr)
    : fd_(fd)
    , handler_(std::move(handler))
    , worker_pool_ptr_(worker_pool_ptr)
{
}

template<ResponseHandler Handler>
MultishotRecvJob<Handler>* MultishotRecvJob<Handler>::createFromPool(
    int fd, Handler handler, void* worker_pool_ptr) {
    auto* job = lfmemorypool::lockfree_pool_alloc_fast<MultishotRecvJob<Handler>>(
        fd, std::move(handler), worker_pool_ptr);
    if (!job) {
        return nullptr;
    }
    return job;
}

template<ResponseHandler Handler>
void MultishotRecvJob<Handler>::freePoolAllocated(MultishotRecvJob* job) {
    lfmemorypool::lockfree_pool_free_fast<MultishotRecvJob<Handler>>(job);
}

template<ResponseHandler Handler>
void MultishotRecvJob<Handler>::prepareSqe(struct io_uring_sqe* sqe) {
    io_uring_prep_recv_multishot(sqe, fd_, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    io_uring_sqe_set_data(sqe, this);
}

template<ResponseHandler Handler>
std::optional<IoJob::CleanupCallback> MultishotRecvJob<Handler>::handleCompletion(
    Server& server, struct io_uring_cqe* cqe) {
    
    int result = cqe->res;
    
    // Check for errors or connection close
    if (result <= 0) {
        // Error or EOF - call error handler (direct call, fully inlineable!)
        handler_.onError(result);
        return cleanupMultishotRecvJob;
    }
    
    // Successful read
    unsigned int buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    
    // Get buffer coordinator and buffer pointer
    auto buffer_coordinator = server.getBufferRingCoordinator();
    if (!buffer_coordinator) {
        handler_.onError(-EINVAL);
        return cleanupMultishotRecvJob;
    }
    
    void* buffer_ptr = buffer_coordinator->getBufferPtr(buffer_id);
    if (!buffer_ptr) {
        handler_.onError(-EINVAL);
        return cleanupMultishotRecvJob;
    }
    
    // Zero-copy path: create token for worker thread processing
    auto token = std::make_shared<ProvidedBufferToken>(
        [buffer_coordinator](unsigned buf_id) {
            buffer_coordinator->recycleBuffer(buf_id);
        },
        buffer_id,
        static_cast<char*>(buffer_ptr),
        result
    );
    
    // Call token handler (direct call, fully inlineable!)
    handler_.onDataToken(std::move(token), worker_pool_ptr_);
    
    // Multishot continues automatically (no cleanup callback)
    return std::nullopt;
}

template<ResponseHandler Handler>
void MultishotRecvJob<Handler>::cleanupMultishotRecvJob(IoJob* job) {
    freePoolAllocated(static_cast<MultishotRecvJob<Handler>*>(job));
}

} // namespace caduvelox
