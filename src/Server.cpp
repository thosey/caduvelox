#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include "caduvelox/util/EventFd.hpp"
#include <liburing.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace caduvelox {

Server::Server()
    : ring_{},
      server_state_(&local_state_),
      buffer_ring_coordinator_(std::make_shared<BufferRingCoordinator>()) {
    ring_.ring_fd = -1;  // sentinel: ring not yet initialized; distinguishes uninit from fd 0 (stdin)
}

// Out-of-line so the header only needs a forward declaration of EventFd.
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

    // Cross-thread stop channel. Created here rather than in the constructor so
    // that a Server whose init() failed has no half-usable wake-up path.
    stop_signal_ = std::make_unique<EventFd>(/*semaphore=*/false, /*nonblocking=*/true);

    Logger::getInstance().logMessage("Server initialized with queue depth " + std::to_string(queue_depth));
    return true;
}

void Server::armStopSignal() {
    if (!stop_signal_ || ring_.ring_fd < 0) {
        return;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);  // flush to free an SQE
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe) {
        Logger::getInstance().logError("Server: Unable to acquire SQE for stop signal poll");
        return;
    }

    // Single-shot is enough: the first signal ends the event loop.
    io_uring_prep_poll_add(sqe, stop_signal_->fd(), POLLIN);
    io_uring_sqe_set_data64(sqe, STOP_SIGNAL_USER_DATA);
    io_uring_submit(&ring_);
}

void Server::run() {
    // Arm before anything else can block: a stop() that already happened is not
    // lost, because the eventfd counter persists and the poll completes as soon
    // as it is armed.
    armStopSignal();

    if (startup_fn_) {
        startup_fn_(*this);
    }

    while (server_state_->load(std::memory_order_acquire) == ServerState::Running) {
        processCompletions();
    }

    // Drain remaining completions after the event loop exits to ensure
    // pool-managed jobs release their resources on this thread.
    drainCompletions();
    Logger::getInstance().logMessage("Server: Stopped with all jobs drained");
}

void Server::stop() {
    // Transition to Stopping so the run() loop exits on next iteration.
    // In the multi-ring HttpServer case, HttpServer::stop() has already CAS'd
    // the shared state_ to Stopping before calling ring->stop(), so this CAS
    // fails harmlessly — the important effect is the wakeup below.
    ServerState expected = ServerState::Running;
    server_state_->compare_exchange_strong(expected, ServerState::Stopping,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);

    // Early return if init() never ran (or failed): there is nothing to wake.
    if (!stop_signal_) {
        Logger::getInstance().logMessage("Server: Stop requested (ring not initialized)");
        return;
    }

    // Wake the event loop in case it is blocked in io_uring_wait_cqe().
    //
    // This must NOT go through the submission queue. stop() runs on a foreign
    // thread — HttpServer::stop() -> ServiceRing::stop(), and tests call it from
    // main while the loop runs — and liburing's SQ is not thread-safe. The ring
    // thread may be inside registerJob()/submit() from a completion handler at
    // the same moment; two threads racing on io_uring_get_sqe() can be handed the
    // same SQE or corrupt the SQ tail, so a job's user_data could be overwritten
    // or a half-written SQE submitted.
    //
    // A write(2) to an eventfd is thread-safe, and run() armed a poll on it from
    // the ring thread, which turns this into an ordinary CQE. If the poll is not
    // armed yet the signal is still not lost: the eventfd counter persists, so
    // the poll completes immediately once armed.
    try {
        stop_signal_->signal();
    } catch (const std::system_error& e) {
        // Never propagate: stop() is called from ~Server().
        Logger::getInstance().logError(std::string("Server: stop wakeup failed: ") + e.what());
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
    if (user_data == STOP_SIGNAL_USER_DATA) {
        // A foreign thread called stop(). Drain the counter so the fd is left
        // quiescent, then run the ring-local sweep.
        // Not counted in in_flight_ since armStopSignal() bypasses registerJob.
        if (stop_signal_) {
            stop_signal_->try_consume();
        }
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
