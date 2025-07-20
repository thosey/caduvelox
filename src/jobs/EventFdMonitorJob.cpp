#include "caduvelox/jobs/EventFdMonitorJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/Config.hpp"
#include "LockFreeMemoryPool.h"
#include <liburing.h>
#include <cstring>

// Define lock-free pool for EventFdMonitorJob
// Smaller pool since we typically have only one per worker pool
DEFINE_LOCKFREE_POOL(caduvelox::EventFdMonitorJob, 100);

namespace caduvelox {

EventFdMonitorJob::EventFdMonitorJob(int fd)
    : fd_(fd), counter_buffer_(0) {
}

EventFdMonitorJob::~EventFdMonitorJob() {
    // No cleanup needed - buffer is inline
}

EventFdMonitorJob* EventFdMonitorJob::createFromPool(int fd,
                                                     SignalCallback on_signal,
                                                     ErrorCallback on_error) {
    EventFdMonitorJob* job = lfmemorypool::lockfree_pool_alloc_fast<EventFdMonitorJob>(fd);
    if (!job) {
        return nullptr; // Pool exhausted
    }
    
    job->on_signal_ = std::move(on_signal);
    job->on_error_ = std::move(on_error);
    return job;
}

void EventFdMonitorJob::freePoolAllocated(EventFdMonitorJob* job) {
    if (job) {
        lfmemorypool::lockfree_pool_free_fast<EventFdMonitorJob>(job);
    }
}

void EventFdMonitorJob::prepareSqe(struct io_uring_sqe* sqe) {
    // Single-shot read of 8-byte eventfd counter
    // This will be manually resubmitted by callback (persistent pattern)
    io_uring_prep_read(sqe, fd_, &counter_buffer_, sizeof(counter_buffer_), 0);
}

std::optional<IoJob::CleanupCallback> EventFdMonitorJob::handleCompletion(Server& server, struct io_uring_cqe* cqe) {
    ssize_t result = cqe->res;
    
    CADUVELOX_DEBUG_LOG("DEBUG: EventFdMonitorJob::handleCompletion fd=" << fd_ 
                       << " result=" << result);
    
    // Sanity check
    if (fd_ < 0 || fd_ > 65536) {
        CADUVELOX_DEBUG_LOG("DEBUG: EventFdMonitorJob invalid fd=" << fd_);
        // Don't free - this is a persistent job managed externally
        return std::nullopt;
    }
    
    if (result < 0) {
        // Error occurred (likely fd closed during shutdown)
        CADUVELOX_DEBUG_LOG("DEBUG: EventFdMonitorJob error: " << result);
        if (on_error_) {
            on_error_(fd_, -result);
        }
        // Errors are fatal for eventfd - job will be freed by owner
        return std::nullopt;
    }
    
    if (result == 0) {
        // EOF - eventfd closed
        CADUVELOX_DEBUG_LOG("DEBUG: EventFdMonitorJob EOF");
        if (on_error_) {
            on_error_(fd_, 0);
        }
        // EOF is fatal - job will be freed by owner
        return std::nullopt;
    }
    
    // Success - eventfd was signaled!
    CADUVELOX_DEBUG_LOG("DEBUG: EventFdMonitorJob received counter=" << counter_buffer_);
    
    if (on_signal_) {
        // Invoke callback with counter value
        // Callback is responsible for:
        // 1. Processing the signal (e.g., drain response queue)
        // 2. Resubmitting this job to continue monitoring
        on_signal_(fd_, counter_buffer_);
    }
    
    // Note: This job is NEVER freed here - it's persistent
    // The callback must resubmit, or the owner must explicitly free on shutdown
    return std::nullopt;
}

} // namespace caduvelox
