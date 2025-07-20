#pragma once

#include "IoJob.hpp"
#include <functional>
#include <memory>
#include <vector>

namespace caduvelox {

/**
 * Job for writing data to a file descriptor.
 * Handles partial writes automatically by continuing until all data is sent.
 * 
 * All WriteJobs are pool-allocated for performance. Use freePoolAllocated() for manual cleanup.
 */
class WriteJob : public IoJob {
public:
    using CompletionCallback = std::function<void(int fd, size_t bytes_written)>;
    using ErrorCallback = std::function<void(int fd, int error)>;

    /**
     * Create write job with owned data using lock-free pool allocation.
     * @param fd File descriptor to write to
     * @param data Unique pointer to data (ownership transferred to job)
     * @param length Number of bytes to write
     * @param on_complete Callback for successful completion
     * @param on_error Callback for errors
     * @return Pointer to pool-allocated WriteJob, or nullptr if pool exhausted
     */
    static WriteJob* createFromPoolWithOwnedData(int fd, std::unique_ptr<char[]> data, size_t length,
                                                 CompletionCallback on_complete = nullptr,
                                                 ErrorCallback on_error = nullptr);

    /**
     * Create write job with borrowed data using lock-free pool allocation.
     * @param fd File descriptor to write to
     * @param data Pointer to data (caller must ensure lifetime)
     * @param length Number of bytes to write
     * @param on_complete Callback for successful completion
     * @param on_error Callback for errors
     * @return Pointer to pool-allocated WriteJob, or nullptr if pool exhausted
     */
    static WriteJob* createFromPoolWithBorrowedData(int fd, const char* data, size_t length,
                                                    CompletionCallback on_complete = nullptr,
                                                    ErrorCallback on_error = nullptr);

    /**
     * Create write job from string using lock-free pool allocation.
     * @param fd File descriptor to write to
     * @param data String data (will be copied into job-owned buffer)
     * @param on_complete Callback for successful completion
     * @param on_error Callback for errors
     * @return Pointer to pool-allocated WriteJob, or nullptr if pool exhausted
     */
    static WriteJob* createFromPoolFromString(int fd, const std::string& data,
                                              CompletionCallback on_complete = nullptr,
                                              ErrorCallback on_error = nullptr);

    /**
     * Free a pool-allocated WriteJob.
     * @param job Pointer to pool-allocated job
     */
    static void freePoolAllocated(WriteJob* job);

    // IoJob interface
    void prepareSqe(struct io_uring_sqe* sqe) override;

    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;

    // Start the job (submit initial operation)
    void start(Server& server);

    WriteJob(int fd, bool owns_data);

private:
    void submitWrite(Server& server);
    void resubmitWrite(Server& server);  // For internal re-submission when already managed

    int fd_;
    bool owns_data_;
    bool is_pool_allocated_ = false;  // Track if this job came from pool
    std::unique_ptr<char[]> owned_data_;
    const char* data_ptr_;
    size_t total_length_;
    size_t bytes_written_;
    
    CompletionCallback on_complete_;
    ErrorCallback on_error_;
};

} // namespace caduvelox