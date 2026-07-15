#include "caduvelox/jobs/IdleTimeoutJob.hpp"
#include "caduvelox/jobs/CancelJob.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/Server.hpp"

namespace caduvelox {

namespace {
    void cleanupIdleTimeoutJob(IoJob* job) {
        PoolManager::deallocate(static_cast<IdleTimeoutJob*>(job));
    }
}

IdleTimeoutJob::IdleTimeoutJob(HttpConnectionJob* owner, unsigned timeout_ms)
    : owner_(owner)
    , owner_gen_(PoolManager::generation<HttpConnectionJob>(owner))
{
    ts_.tv_sec = timeout_ms / 1000;
    ts_.tv_nsec = (timeout_ms % 1000) * 1000000ULL;
}

void IdleTimeoutJob::prepareSqe(struct io_uring_sqe* sqe) {
    io_uring_prep_timeout(sqe, &ts_, 0, 0);
}

std::optional<IoJob::CleanupCallback> IdleTimeoutJob::handleCompletion(Server&, struct io_uring_cqe* cqe) {
    // Only deliver the completion if the owning connection is still the same
    // live object it was at arm time. If it was freed (or its pool slot
    // recycled for a new connection) while this timer stayed armed, drop the
    // completion instead of dereferencing stale memory.
    if (PoolManager::isValid(owner_, owner_gen_)) {
        owner_->handleIdleTimeout(this, cqe->res);
    }
    return cleanupIdleTimeoutJob;
}

void IdleTimeoutJob::requestShutdownCancel(Server& server) {
    auto* cancel_job = PoolManager::allocate<CancelJob>(reinterpret_cast<uint64_t>(this));
    if (!cancel_job) {
        return; // Pool exhausted — timer will fire naturally and no-op against a closed connection.
    }
    struct io_uring_sqe* sqe = server.registerJob(cancel_job);
    if (sqe) {
        cancel_job->prepareSqe(sqe);
        server.submit();
    } else {
        PoolManager::deallocate(cancel_job);
    }
}

} // namespace caduvelox
