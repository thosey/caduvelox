#pragma once

#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <liburing.h>
#include <memory>

// Forward declarations
struct io_uring_sqe;
struct io_uring_cqe;

namespace caduvelox {

// Forward declarations
class BufferRingCoordinator;
class AffinityWorkerPool;
class IoJob;

/**
 * Modern job-based server architecture with direct registration.
 * 
 * Jobs register themselves directly with the server to get SQEs and submit operations.
 * Parent jobs create and submit child jobs in their completion handlers for natural chaining.
 * Object pools (lock-free) manage job lifecycle for high performance.
 * 
 * Key Benefits:
 * - Direct job registration: server.register(job) returns SQE
 * - Natural operation chaining in completion handlers
 * - Zero allocation during hot paths (connection handling)
 * - Lock-free object pools for job reuse
 * - Immediate submission - no batching overhead
 * - Simple mental model matching io_uring flow
 * 
 * Usage:
 *   // In job completion handler:
 *   auto child_job = job_pool.acquire();
 *   struct io_uring_sqe* sqe = server.register(child_job);
 *   child_job->prepareSqe(sqe);
 *   server.submit();
 */
class Server {
public:
    Server();
    ~Server();

    // Non-copyable, non-movable (for now)
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /**
     * Initialize the server with io_uring parameters
     */
    bool init(unsigned queue_depth = 256);

    /**
     * Start the event loop (blocking)
     */
    void run();

    /**
     * Stop the event loop
     */
    void stop();

    /**
     * Register a job for io_uring processing using direct pointer (lock-free pools).
     * Modern API that stores job pointer directly in user_data for zero overhead.
     * Jobs manage their own lifecycle using pool allocation/deallocation.
     * @param job The job to register (from pool allocation)
     * @return SQE to configure, or nullptr if ring is full
     */
    struct io_uring_sqe* registerJob(IoJob* job);

    /**
     * Submit all queued operations to io_uring for processing.
     * @return Number of operations submitted, or negative error code
     */
    int submit();

    /**
     * Get the buffer group ID for recv operations with buffer selection.
     * Jobs can use this to set sqe->buf_group for zero-copy operations.
     */
    int getBufferGroupId() const;

    /**
     * Get the buffer ring coordinator for zero-copy operations
     */
    std::shared_ptr<BufferRingCoordinator> getBufferRingCoordinator() const;

    /**
     * Get the io_uring instance (for buffer ring setup)
     */
    struct io_uring* getRing() { return &ring_; }

    /**
     * Set the affinity worker pool for multi-threaded processing.
     * When set, jobs can dispatch CPU-intensive work to worker threads
     * while keeping io_uring operations on the main thread.
     */
    void setAffinityWorkerPool(std::shared_ptr<AffinityWorkerPool> pool);

    /**
     * Get the affinity worker pool
     */
    std::shared_ptr<AffinityWorkerPool> getAffinityWorkerPool() const;

private:
    void processCompletions();
    void drainCompletions();
    void processAvailableCompletions();  // Helper to process all ready completions
    void handleCompletion(struct io_uring_cqe* cqe);

    // Core io_uring state (stack allocated like original Server)
    struct io_uring ring_;
    bool running_;
    
    // Buffer ring for zero-copy operations
    std::shared_ptr<BufferRingCoordinator> buffer_ring_coordinator_;
    
    // Affinity worker pool for multi-threaded processing
    std::shared_ptr<AffinityWorkerPool> affinity_worker_pool_;
};

} // namespace caduvelox