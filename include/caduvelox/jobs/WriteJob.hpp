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
 * All WriteJobs are pool-allocated for performance. Once start() has been called
 * the job owns itself and frees itself; freePoolAllocated() is only for a job
 * that was created and then never started.
 *
 * SIGPIPE: this job issues IORING_OP_WRITE, which -- like write(2) -- raises SIGPIPE
 * when the fd is a socket whose peer has closed, killing the process under the default
 * disposition. Server::init() ignores SIGPIPE once per process so the condition surfaces
 * as a normal -EPIPE through on_error_ instead. A caller that drives a WriteJob without
 * ever calling Server::init() is responsible for its own SIGPIPE disposition.
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
     * Free a WriteJob that was created but never started.
     *
     * Only valid in that window. After start() the job frees itself on every
     * path -- see the ownership note there -- so calling this on a started job
     * is a double free.
     * @param job Pointer to pool-allocated job
     */
    static void freePoolAllocated(WriteJob* job);

    // IoJob interface
    void prepareSqe(struct io_uring_sqe* sqe) override;

    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;

    /**
     * Submit the write.
     *
     * Ownership passes to this call. On return the job is either queued with
     * the kernel -- in which case handleCompletion() will free it -- or it has
     * already been deallocated and its error callback run. The caller must not
     * touch the pointer afterwards, and must not free it: doing so is a double
     * free, and not doing so is not a leak.
     */
    void start(Server& server);

    WriteJob(int fd, bool owns_data);

private:
    // `what` names the attempt in log lines; the initial submission and the
    // resubmission of a partial write differ in nothing else.
    void submitWrite(Server& server, const char* what);

    // Give up on a job that was never queued: free it, then notify.
    void abandon(const std::string& message, int error);

    int fd_;
    bool owns_data_;
    std::unique_ptr<char[]> owned_data_;
    const char* data_ptr_;
    size_t total_length_;
    size_t bytes_written_;
    
    CompletionCallback on_complete_;
    ErrorCallback on_error_;
};

} // namespace caduvelox