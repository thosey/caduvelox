#pragma once

#include <memory>
#include <string>
#include <optional>

// Forward declaration to avoid including liburing.h in header
struct io_uring_cqe;
struct io_uring_sqe;

namespace caduvelox {

// Forward declarations
class Server;
class IoJob;

/**
 * Base interface for all io_uring operations.
 * Each job encapsulates all data and logic needed for a specific operation.
 */
class IoJob {
public:
    using CleanupCallback = void(*)(IoJob*);

    virtual ~IoJob() = default;

    /**
     * Prepare the submission queue entry for this job.
     * The job configures the SQE with its specific operation and data.
     * @param sqe The SQE to configure for this job
     */
    virtual void prepareSqe(struct io_uring_sqe* sqe) = 0;

    /**
     * Handle completion of the io_uring operation.
     * Job manages its own lifecycle - may return a cleanup callback for deferred cleanup.
     * @param server Reference to the job server for posting follow-up jobs  
     * @param cqe The completion queue entry from io_uring
     * @return Optional cleanup function pointer to be executed after handleCompletion returns
     */
    virtual std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) = 0;
};

} // namespace caduvelox