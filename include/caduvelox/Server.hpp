#pragma once

#include "caduvelox/ServerState.hpp"
#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <liburing.h>
#include <atomic>
#include <functional>
#include <memory>

// Forward declarations
struct io_uring_sqe;
struct io_uring_cqe;

namespace caduvelox {

// Forward declarations
class BufferRingCoordinator;
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
     * @param queue_depth   SQE/CQE ring depth
     * @param buf_count     Number of pre-allocated zero-copy recv buffers
     * @param buf_size      Size of each recv buffer in bytes
     */
    bool init(unsigned queue_depth = 256, unsigned buf_count = 512, size_t buf_size = 16384);

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
     * Redirect state queries to an external atomic owned by a parent component.
     * By default Server owns its own state; call this to share state across
     * multiple Server instances so they all observe the same transitions.
     * Must be called before the event loop starts.
     */
    void bindToServerState(std::atomic<ServerState>* state);

    /**
     * Query the lifecycle state.
     */
    ServerState getServerState() const;
    bool isStopping() const;
    bool isAborting() const;

    /**
     * Register a function that sweeps ring-local job pools during shutdown.
     * Called by SingleRingHttpServer so it can cancel idle multishot recv jobs
     * without Server needing to know about specific job types.
     * Must be installed before the event loop starts.
     */
    void setShutdownSweepFn(std::function<void(Server&)> fn);

    /**
     * Invoke the registered shutdown sweep (if any).
     * Called by StopIoJob when the shutdown wake-up NOP completes on the ring thread.
     */
    void sweepLiveJobsForShutdown();

private:
    void processCompletions();
    void drainCompletions();
    void processAvailableCompletions();  // Helper to process all ready completions
    void handleCompletion(struct io_uring_cqe* cqe);

    // Core io_uring state (stack allocated like original Server)
    struct io_uring ring_;
    std::atomic<bool> running_;

    // Owned local state used when no external atomic has been installed.
    std::atomic<ServerState> local_state_{ServerState::Running};

    // Points to the active state atomic — either local_state_ (default) or an
    // external atomic shared across multiple Server instances.
    std::atomic<ServerState>* server_state_;

    // Optional ring-local shutdown sweep installed by SingleRingHttpServer.
    std::function<void(Server&)> shutdown_sweep_fn_;

    // Buffer ring for zero-copy operations
    std::shared_ptr<BufferRingCoordinator> buffer_ring_coordinator_;
};

} // namespace caduvelox