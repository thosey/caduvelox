#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include <liburing.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
class StopIoJob final : public caduvelox::IoJob {
public:
    void prepareSqe(struct io_uring_sqe* sqe) override {
        io_uring_prep_nop(sqe);
    }

    std::optional<CleanupCallback> handleCompletion(caduvelox::Server&, struct io_uring_cqe*) override {
        // No-op sentinel to wake the event loop during shutdown.
        return std::nullopt;
    }
};

StopIoJob g_stop_job;
}

namespace caduvelox {

Server::Server() 
    : ring_{}, running_(false),
      buffer_ring_coordinator_(std::make_shared<BufferRingCoordinator>()) {
}

Server::~Server() {
    stop();
    if (ring_.ring_fd != 0) {
        io_uring_queue_exit(&ring_);
    }
}

bool Server::init(unsigned queue_depth) {
    int ret = io_uring_queue_init(queue_depth, &ring_, 0);
    if (ret < 0) {
        throw std::runtime_error("Failed to initialize io_uring: " + std::string(strerror(-ret)));
    }

    // Set up buffer ring for zero-copy operations
    // Use the direct io_uring overload to avoid reinterpret_cast issues
    if (!buffer_ring_coordinator_->setupBufferRing(&ring_)) {
        throw std::runtime_error("Failed to setup buffer ring - this requires a recent kernel with buffer ring support");
    }

    Logger::getInstance().logMessage("Server initialized with queue depth " + std::to_string(queue_depth));
    return true;
}

void Server::run() {
    running_ = true;

    while (running_) {
        processCompletions();
    }

    // Drain remaining completions after the event loop exits to ensure
    // pool-managed jobs release their resources on this thread.
    drainCompletions();
    Logger::getInstance().logMessage("Server: Stopped with all jobs drained");
}

void Server::stop() {
    running_ = false;
    
    // Early return if ring was never initialized (init() failed or never called)
    if (ring_.ring_fd <= 0) {
        Logger::getInstance().logMessage("Server: Stop requested (ring not initialized)");
        return;
    }
    
    // Wake the event loop in case it's blocked in io_uring_wait_cqe.
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        // Attempt to flush outstanding submissions to free an SQE.
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }

    if (sqe) {
        g_stop_job.prepareSqe(sqe);
        io_uring_sqe_set_data64(sqe, reinterpret_cast<uintptr_t>(&g_stop_job));
        io_uring_submit(&ring_);
    } else {
        Logger::getInstance().logError("Server: Unable to acquire SQE for shutdown wakeup");
    }

    Logger::getInstance().logMessage("Server: Stop requested");
}

struct io_uring_sqe* Server::registerJob(IoJob* job) {
    // Get an SQE from io_uring
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        return nullptr; // Ring is full
    }
    
    io_uring_sqe_set_data64(sqe,  reinterpret_cast<uintptr_t>(job));
    return sqe;
}

int Server::submit() {
    return io_uring_submit(&ring_);
}

int Server::getBufferGroupId() const {
    return buffer_ring_coordinator_->getBufferGroupId();
}

void Server::drainCompletions() {
    struct io_uring_cqe* cqe;
    int max_iterations = 100; // Prevent infinite loops
    int iterations = 0;
    
    Logger::getInstance().logMessage("Server: Starting drain (pool-based jobs manage own lifecycle)");
    
    // Process remaining completions without waiting
    while (iterations < max_iterations) {
        int ret = io_uring_peek_cqe(&ring_, &cqe);
        if (ret < 0) {
            break; // No more completions available
        }
        
        // Process all available completions
        processAvailableCompletions();
        iterations++;
    }
    
    Logger::getInstance().logMessage("Server: Drain complete after " + std::to_string(iterations) + " iterations");
}

void Server::processCompletions() {
    struct io_uring_cqe* cqe;
    
    // Wait for at least one completion
    int ret = io_uring_wait_cqe(&ring_, &cqe);
    if (ret < 0) {
        if (ret != -EINTR) {
            Logger::getInstance().logError("io_uring_wait_cqe failed: " + std::string(strerror(-ret)) + " (code: " + std::to_string(ret) + ")");
        }
        return;
    }

    // Process this completion and any others that are ready
    processAvailableCompletions();
}

void Server::processAvailableCompletions() {
    struct io_uring_cqe* cqe;
    unsigned unused_head;
    unsigned handledCount = 0;
    
    io_uring_for_each_cqe(&ring_, unused_head, cqe) {
        handleCompletion(cqe);
        handledCount++;
    }
    
    io_uring_cq_advance(&ring_, handledCount);
}

void Server::handleCompletion(struct io_uring_cqe* cqe) {
    uint64_t user_data = io_uring_cqe_get_data64(cqe);
    if (user_data == 0) {
        Logger::getInstance().logError("Server: Completion with null user_data");
        return;
    }

    IoJob* job = (IoJob*)user_data;
    auto cleanup = job->handleCompletion(*this, cqe);
    
    // Execute cleanup function pointer if returned
    if (cleanup) {
        (*cleanup)(job);
    }
}

std::shared_ptr<caduvelox::BufferRingCoordinator> Server::getBufferRingCoordinator() const {
    return buffer_ring_coordinator_;
}

} // namespace caduvelox
