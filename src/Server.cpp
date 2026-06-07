#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include <liburing.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace caduvelox {

Server::Server()
    : ring_{}, running_(false),
      server_state_(&local_state_),
      buffer_ring_coordinator_(std::make_shared<BufferRingCoordinator>()) {
    ring_.ring_fd = -1;  // sentinel: ring not yet initialized; distinguishes uninit from fd 0 (stdin)
}

Server::~Server() {
    stop();
    if (ring_.ring_fd >= 0) {
        io_uring_queue_exit(&ring_);
    }
}

bool Server::init(unsigned queue_depth, unsigned buf_count, size_t buf_size) {
    int ret = io_uring_queue_init(queue_depth, &ring_, 0);
    if (ret < 0) {
        throw std::runtime_error("Failed to initialize io_uring: " + std::string(strerror(-ret)));
    }

    // Create and set up the buffer ring with the requested dimensions.
    buffer_ring_coordinator_ = std::make_shared<BufferRingCoordinator>(buf_count, buf_size);
    if (!buffer_ring_coordinator_->setupBufferRing(&ring_)) {
        throw std::runtime_error("Failed to setup buffer ring - this requires a recent kernel with buffer ring support");
    }

    Logger::getInstance().logMessage("Server initialized with queue depth " + std::to_string(queue_depth));
    return true;
}

void Server::run() {
    running_.store(true, std::memory_order_relaxed);

    if (startup_fn_) {
        startup_fn_(*this);
    }

    while (running_.load(std::memory_order_relaxed)) {
        processCompletions();
    }

    // Drain remaining completions after the event loop exits to ensure
    // pool-managed jobs release their resources on this thread.
    drainCompletions();
    Logger::getInstance().logMessage("Server: Stopped with all jobs drained");
}

void Server::stop() {
    running_.store(false, std::memory_order_relaxed);
    
    // Early return if ring was never initialized (init() failed or never called)
    if (ring_.ring_fd < 0) {
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
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0);  // user_data=0 signals the shutdown sweep
        io_uring_submit(&ring_);
    } else {
        Logger::getInstance().logError("Server: Unable to acquire SQE for shutdown wakeup");
    }

    Logger::getInstance().logMessage("Server: Stop requested");
}

struct io_uring_sqe* Server::registerJob(IoJob* job) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        return nullptr;
    }
    ++in_flight_;
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uintptr_t>(job));
    return sqe;
}

int Server::submit() {
    return io_uring_submit(&ring_);
}

int Server::getBufferGroupId() const {
    return buffer_ring_coordinator_->getBufferGroupId();
}

void Server::drainCompletions() {
    Logger::getInstance().logMessage("Server: Starting drain, in_flight=" + std::to_string(in_flight_));

    while (in_flight_ > 0) {
        struct io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            Logger::getInstance().logError("Server: drain wait failed: " + std::string(strerror(-ret)));
            break;
        }
        processAvailableCompletions();
    }

    Logger::getInstance().logMessage("Server: Drain complete");
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
        // Shutdown NOP — trigger the ring-local sweep and return.
        // Not counted in in_flight_ since it bypasses registerJob.
        sweepLiveJobsForShutdown();
        return;
    }

    IoJob* job = reinterpret_cast<IoJob*>(user_data);
    auto cleanup = job->handleCompletion(*this, cqe);

    // Decrement only when the operation is fully done (no further CQEs expected).
    // Multishot operations set IORING_CQE_F_MORE on all but their final completion.
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
        --in_flight_;
    }

    if (cleanup) {
        (*cleanup)(job);
    }
}

std::shared_ptr<caduvelox::BufferRingCoordinator> Server::getBufferRingCoordinator() const {
    return buffer_ring_coordinator_;
}

void Server::bindToServerState(std::atomic<ServerState>* state) {
    server_state_ = state;
}

ServerState Server::getServerState() const {
    return server_state_->load(std::memory_order_acquire);
}

bool Server::isStopping() const {
    return getServerState() == ServerState::Stopping;
}

bool Server::isAborting() const {
    return getServerState() == ServerState::Aborting;
}

void Server::setStartupFn(std::function<void(Server&)> fn) {
    startup_fn_ = std::move(fn);
}

void Server::setShutdownSweepFn(std::function<void(Server&)> fn) {
    shutdown_sweep_fn_ = std::move(fn);
}

void Server::sweepLiveJobsForShutdown() {
    if (shutdown_sweep_fn_) {
        shutdown_sweep_fn_(*this);
    }
}

} // namespace caduvelox
