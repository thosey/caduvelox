#pragma once

#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <liburing.h>
#include <cstdint>

namespace caduvelox {

/**
 * CancelJob — submits an io_uring async-cancel SQE targeting a specific in-flight operation.
 *
 * The target is identified by the uint64_t user_data value that was set on the original SQE.
 * If the target operation has already completed, io_uring returns -ENOENT or -EALREADY for
 * the cancel completion; both are treated as benign and the job is simply returned to the pool.
 *
 * Pool-allocated; callers use PoolManager::allocate<CancelJob>(target_user_data).
 */
class CancelJob final : public IoJob {
public:
    explicit CancelJob(uint64_t target) : target_(target) {}

    void prepareSqe(struct io_uring_sqe* sqe) override {
        io_uring_prep_cancel64(sqe, target_, 0);
    }

    std::optional<CleanupCallback> handleCompletion(Server&, struct io_uring_cqe*) override {
        // Cancel completed (success, -ENOENT, or -EALREADY — all are benign).
        return [](IoJob* job) {
            PoolManager::deallocate(static_cast<CancelJob*>(job));
        };
    }

private:
    uint64_t target_;
};

} // namespace caduvelox

// Pool capacity for CancelJob: one per active connection during shutdown sweep.
template<>
inline size_t caduvelox::PoolCapacityConfig<caduvelox::CancelJob>::capacity = 10000;
