#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "LockFreeMemoryPool.h"
#include <liburing.h>
#include <sys/socket.h>

// Define lock-free pool for AcceptJob at global scope
// Small pool since usually only 1-2 active accept jobs per listening socket
DEFINE_LOCKFREE_POOL(caduvelox::AcceptJob, 100);

namespace caduvelox {

AcceptJob::AcceptJob(int server_fd)
    : server_fd_(server_fd) {
}


// New pool-based factory method
AcceptJob* AcceptJob::create(int server_fd,
                            ConnectionCallback on_connection,
                            ErrorCallback on_error) {
    AcceptJob* job = lfmemorypool::lockfree_pool_alloc_fast<AcceptJob>(server_fd);
    if (job) {
        job->on_connection_ = std::move(on_connection);
        job->on_error_ = std::move(on_error);
    }
    return job;
}

// Free pool-allocated job (for error cleanup)
void AcceptJob::freePoolAllocated(AcceptJob* job) {
    if (job) {
        lfmemorypool::lockfree_pool_free_fast<AcceptJob>(job);
    }
}

std::optional<IoJob::CleanupCallback> AcceptJob::handleCompletion(Server& server, struct io_uring_cqe* cqe) {
    int result = cqe->res;
    
    if (result < 0) {
        if (on_error_) {
            on_error_(-result);
        }
        
        // For multishot, try to resubmit on recoverable errors
        if (-result == EMFILE || -result == ENFILE || -result == ENOBUFS) {
            // Resource exhaustion - wait a bit and retry
            resubmitAccept(server);
            return std::nullopt; // Continue operation
        }
        
        // Unrecoverable error - job is complete
        return std::nullopt; // End job on unrecoverable error
    }
    
    // For multishot accept, result==0 can indicate setup completion, not a real connection
    if (result == 0) {
        // Check if multishot is continuing - if IORING_CQE_F_MORE is NOT set,
        // the multishot accept has terminated and we need to resubmit
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            // Multishot terminated - resubmit to continue accepting
            resubmitAccept(server);
            return std::nullopt; // Continue with resubmitted job
        }
        return std::nullopt; // Continue multishot
    }
    
    // Successfully accepted a connection
    int client_fd = result;
    
    if (on_connection_) {
        // For now, pass null addr info - could be enhanced to capture client address
        on_connection_(client_fd, nullptr, 0);
    }
    
    // Check if multishot is continuing - if IORING_CQE_F_MORE is NOT set,
    // the multishot accept has terminated and we need to resubmit
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
        // Multishot terminated - resubmit to continue accepting
        resubmitAccept(server);
        return std::nullopt; // Continue with resubmitted job
    }
    
    return std::nullopt; // Continue multishot
}

void AcceptJob::start(Server& server) {
    submitAccept(server);
}

void AcceptJob::prepareSqe(struct io_uring_sqe* sqe) {
    // Ensure accepted sockets are non-blocking and close-on-exec
    io_uring_prep_multishot_accept(sqe, server_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

void AcceptJob::submitAccept(Server& server) {
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (sqe) {
        prepareSqe(sqe);
        server.submit();
    } else if (on_error_) {
        // Ring SQE temporarily unavailable
        on_error_(EAGAIN);
    }
}

void AcceptJob::resubmitAccept(Server& server) {
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (sqe) {
        prepareSqe(sqe);
        server.submit();
    } else if (on_error_) {
        // Ring SQE temporarily unavailable
        on_error_(EAGAIN);
    }
}

} // namespace caduvelox
