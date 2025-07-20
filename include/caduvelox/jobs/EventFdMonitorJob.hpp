#pragma once

#include "IoJob.hpp"
#include <functional>
#include <vector>

namespace caduvelox {

/**
 * EventFD monitoring job for inter-thread signaling.
 * Persistent single-shot read that resubmits itself after each signal.
 * Designed specifically for eventfd file descriptors, not sockets.
 * 
 * Unlike ReadJob (multishot socket recv), this:
 * - Uses a fixed 8-byte buffer (eventfd counter)
 * - Never frees itself (persistent lifecycle)
 * - Resubmits in callback (manual persistence)
 * - No buffer ring (direct buffer pointer)
 */
class EventFdMonitorJob : public IoJob {
public:
    using SignalCallback = std::function<void(int fd, uint64_t counter)>;
    using ErrorCallback = std::function<void(int fd, int error)>;

    ~EventFdMonitorJob();

    /**
     * Pool-based factory: Create an eventfd monitor using lock-free pool allocation.
     * @param fd EventFD file descriptor to monitor
     * @param on_signal Callback when eventfd is signaled (receives counter value)
     * @param on_error Callback for errors (e.g., fd closed during shutdown)
     * @return Pointer to pool-allocated job, or nullptr if pool exhausted
     */
    static EventFdMonitorJob* createFromPool(int fd,
                                             SignalCallback on_signal,
                                             ErrorCallback on_error = nullptr);

    /**
     * Free a pool-allocated job (for error cleanup or shutdown)
     */
    static void freePoolAllocated(EventFdMonitorJob* job);

    // IoJob interface
    void prepareSqe(struct io_uring_sqe* sqe) override;
    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;

    /**
     * Get pointer to internal 8-byte buffer for eventfd counter
     */
    char* getBufferPtr() { return reinterpret_cast<char*>(&counter_buffer_); }

    EventFdMonitorJob(int fd);

private:
    int fd_;
    uint64_t counter_buffer_;  // EventFD always reads 8-byte counter
    
    SignalCallback on_signal_;
    ErrorCallback on_error_;
};

} // namespace caduvelox
