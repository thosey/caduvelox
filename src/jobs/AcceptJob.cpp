#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/CancelJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <liburing.h>
#include <sys/socket.h>

// AcceptJob is multishot: one job services all connections on a listening socket.
// Pool size 2 covers the brief error-recovery window where on_error_ allocates a
// fresh AcceptJob before the old one's cleanup callback has fired.
template<>
size_t caduvelox::PoolCapacityConfig<caduvelox::AcceptJob>::capacity = 2;

namespace {
    void cleanupAcceptJob(caduvelox::IoJob* job) {
        caduvelox::PoolManager::deallocate(static_cast<caduvelox::AcceptJob*>(job));
    }
}

namespace caduvelox {

AcceptJob::AcceptJob(int server_fd)
    : server_fd_(server_fd) {
}


// Pool-allocated, despite this having been plain new/delete for a long time
// while accept_pool_size, PoolCapacityConfig<AcceptJob> and the
// [POOL_EXHAUSTED] branch in startAccepting() all configured and reported on a
// pool nothing used. `new` never returns null, so that branch was unreachable.
//
// The pools are thread-local, so this must be called on the ring thread that
// will service the accept -- otherwise the job comes from one thread's pool and
// is freed into another's. SingleRingHttpServer arms the accept from
// Server::setStartupFn() for exactly that reason. Allocating on the wrong thread
// is also what leaked: HttpServer::listenKTLS() built every ring's accept up
// front on the main thread, and a job stranded there had no pool to return to.
AcceptJob* AcceptJob::create(int server_fd,
                            ConnectionCallback on_connection,
                            ErrorCallback on_error) {
    AcceptJob* job = PoolManager::allocate<AcceptJob>(server_fd);
    if (!job) {
        return nullptr;  // Pool exhausted -- now actually reachable.
    }
    job->on_connection_ = std::move(on_connection);
    job->on_error_ = std::move(on_error);
    return job;
}

void AcceptJob::freePoolAllocated(AcceptJob* job) {
    if (job) {
        PoolManager::deallocate<AcceptJob>(job);
    }
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

        // Unrecoverable error, or any error during shutdown — free the heap-allocated job.
        clearOwnerSlot();
        return cleanupAcceptJob;
    }

    // For multishot accept, result==0 can indicate setup completion, not a real connection.
    if (result == 0) {
        // If IORING_CQE_F_MORE is not set the multishot has terminated.
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            if (shutting_down) {
                // Do not re-arm the accept during shutdown.
                clearOwnerSlot();
                return cleanupAcceptJob;
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
            clearOwnerSlot();
            return cleanupAcceptJob;
        }
        resubmitAccept(server);
        return std::nullopt;
    }

    return std::nullopt; // Continue multishot.
}

void AcceptJob::clearOwnerSlot() {
    // Only clear if the owner still tracks this job — the error path may have
    // already replaced it with a fresh AcceptJob via on_error_ → startAccepting().
    if (owner_slot_ && *owner_slot_ == this) {
        *owner_slot_ = nullptr;
    }
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
