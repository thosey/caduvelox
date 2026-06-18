#pragma once

#include "IoJob.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <liburing.h>

namespace caduvelox {

class HttpConnectionJob;

/**
 * One-shot io_uring TIMEOUT operation used to close keep-alive HTTP connections
 * that sit idle (waiting for the next request) for too long.
 *
 * Armed by HttpConnectionJob every time it starts waiting for data (startReading()).
 * Cancelled via a targeted CancelJob (same pattern as MultishotRecvJob's cancellation)
 * when data arrives or the connection closes for another reason. If it fires
 * naturally (-ETIME), it tells the owning HttpConnectionJob to close.
 *
 * Pool-allocated; callers use PoolManager::allocate<IdleTimeoutJob>(owner, timeout_ms).
 */
class IdleTimeoutJob final : public IoJob {
public:
    IdleTimeoutJob(HttpConnectionJob* owner, unsigned timeout_ms);

    void prepareSqe(struct io_uring_sqe* sqe) override;
    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;
    void requestShutdownCancel(Server& server) override;

private:
    HttpConnectionJob* owner_;
    // Must remain valid until the request completes or is cancelled — the kernel
    // reads it when the SQE is processed and may hold a reference until then.
    __kernel_timespec ts_{};
};

} // namespace caduvelox

// Pool capacity for IdleTimeoutJob: one outstanding timer per live connection at most.
template<>
inline size_t caduvelox::PoolCapacityConfig<caduvelox::IdleTimeoutJob>::capacity = 10000;
