#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/CancelJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <liburing.h>
#include <sys/socket.h>

// Default pool capacity for AcceptJob (overridable at runtime via ServerConfig).
template<>
size_t caduvelox::PoolCapacityConfig<caduvelox::AcceptJob>::capacity = 1000;

namespace caduvelox {

AcceptJob::AcceptJob(int server_fd)
    : server_fd_(server_fd) {
}


// New pool-based factory method
AcceptJob* AcceptJob::create(int server_fd,
                            ConnectionCallback on_connection,
                            ErrorCallback on_error) {
    auto* job = new AcceptJob(server_fd);
    job->on_connection_ = std::move(on_connection);
    job->on_error_ = std::move(on_error);
    return job;
}

void AcceptJob::freePoolAllocated(AcceptJob* job) {
    delete job;
}

std::optional<IoJob::CleanupCallback> AcceptJob::handleCompletion(Server& server, struct io_uring_cqe* cqe) {
    int result = cqe->res;
    bool shutting_down = server.isStopping() || server.isAborting();

    if (result < 0) {
        // During shutdown, listener-close and related errors are expected — suppress the callback.
        if (!shutting_down && on_error_) {
            on_error_(-result);
        }

        // Only retry recoverable errors when the server is still running.
        if (!shutting_down && (-result == EMFILE || -result == ENFILE || -result == ENOBUFS)) {
            resubmitAccept(server);
            return std::nullopt;
        }

        // Unrecoverable error, or any error during shutdown — return job to pool.
        return [](IoJob* job) {
            delete static_cast<AcceptJob*>(job);
        };
    }

    // For multishot accept, result==0 can indicate setup completion, not a real connection.
    if (result == 0) {
        // If IORING_CQE_F_MORE is not set the multishot has terminated.
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            if (shutting_down) {
                // Do not re-arm the accept during shutdown.
                return [](IoJob* job) {
                    PoolManager::deallocate(static_cast<AcceptJob*>(job));
                };
            }
            resubmitAccept(server);
            return std::nullopt;
        }
        return std::nullopt; // Continue multishot.
    }

    // Successfully accepted a connection — deliver it regardless of shutdown state;
    // HttpConnectionJob will respect the server state at its own control boundaries.
    int client_fd = result;

    sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    if (getpeername(client_fd, (sockaddr*)&client_addr, &addr_len) == 0) {
        if (on_connection_) {
            on_connection_(client_fd, (const sockaddr*)&client_addr, addr_len);
        }
    } else {
        if (on_connection_) {
            on_connection_(client_fd, nullptr, 0);
        }
    }

    // If IORING_CQE_F_MORE is not set the multishot has terminated.
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
        if (shutting_down) {
            return [](IoJob* job) {
                PoolManager::deallocate(static_cast<AcceptJob*>(job));
            };
        }
        resubmitAccept(server);
        return std::nullopt;
    }

    return std::nullopt; // Continue multishot.
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

void AcceptJob::requestShutdownCancel(Server& server) {
    auto* cancel_job = PoolManager::allocate<CancelJob>(reinterpret_cast<uint64_t>(this));
    if (!cancel_job) return;
    struct io_uring_sqe* sqe = server.registerJob(cancel_job);
    if (sqe) {
        cancel_job->prepareSqe(sqe);
        server.submit();
    } else {
        PoolManager::deallocate(cancel_job);
    }
}

void AcceptJob::resubmitAccept(Server& server) {
    // Do not re-arm during shutdown. Callers in handleCompletion are responsible
    // for returning a cleanup callback when resubmission is skipped.
    if (server.isStopping() || server.isAborting()) {
        return;
    }
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
