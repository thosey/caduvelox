#pragma once

#include "IoJob.hpp"
#include <memory>

// Forward declaration to avoid including liburing.h in header
struct io_uring_sqe;

namespace caduvelox {

/**
 * Interface for transaction objects that batch job submissions.
 * Thread-safe and provides atomic submission of multiple jobs.
 * Perfect for linked operations and batched I/O.
 */
class JobTransaction {
public:
    virtual ~JobTransaction() = default;

    /**
     * Add a job to this transaction.
     * The transaction will provide an SQE and let the job configure it.
     * Thread-safe and can be called from multiple threads.
     * @param job The job to add - the job will configure its own SQE
     */
    virtual void addJob(JobPtr job) = 0;

    /**
     * Create an SQE for preparing operations within this transaction.
     * Thread-safe.
     * @return New SQE for job preparation, or nullptr if none available
     */
    virtual struct io_uring_sqe* createSqe() = 0;

    /**
     * Atomically submit all jobs in this transaction.
     * Thread-safe - only one thread can commit at a time.
     * @return Number of operations submitted, or negative error code
     */
    virtual int commit() = 0;

    /**
     * Check if this transaction has any pending jobs.
     * Thread-safe.
     */
    virtual bool isEmpty() const = 0;

    /**
     * Get the number of jobs in this transaction.
     * Thread-safe.
     */
    virtual size_t size() const = 0;

    /**
     * Clear all pending jobs without submitting them.
     * Thread-safe.
     */
    virtual void clear() = 0;
};

/**
 * Smart pointer type for transactions
 */
using JobTransactionPtr = std::unique_ptr<JobTransaction>;

} // namespace caduvelox