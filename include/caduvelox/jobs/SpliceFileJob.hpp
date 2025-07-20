#pragma once

#include "caduvelox/jobs/IoJob.hpp"
#include <functional>
#include <memory>

namespace caduvelox {

/**
 * Zero-copy file transfer job using splice(2) via io_uring.
 * 
 * Implements the pattern recommended by Jens Axboe (io_uring author):
 * 1. Create a pipe
 * 2. splice(file_fd → pipe[1]) - file to pipe write end
 * 3. splice(pipe[0] → socket_fd) - pipe read end to socket
 * 
 * This provides true zero-copy transfers without userspace buffers.
 * The pipe acts as a small kernel buffer that's constantly filled/drained.
 */
class SpliceFileJob : public IoJob, public std::enable_shared_from_this<SpliceFileJob> {
public:
    using CompletionCallback = std::function<void(int client_fd, size_t bytes_transferred)>;
    using ErrorCallback = std::function<void(int client_fd, int error)>;

    /**
     * Create splice file job using lock-free pool allocation.
     * @param client_fd Socket file descriptor to send to
     * @param file_fd File descriptor to send from
     * @param offset Starting offset in file
     * @param length Number of bytes to transfer (0 = until EOF)
     * @param on_complete Callback when transfer completes successfully
     * @param on_error Callback when transfer fails
     * @return Pointer to pool-allocated SpliceFileJob, or nullptr if pool exhausted
     */
    static SpliceFileJob* createFromPool(
        int client_fd, 
        int file_fd, 
        uint64_t offset, 
        uint64_t length,
        CompletionCallback on_complete = nullptr,
        ErrorCallback on_error = nullptr
    );

    // IoJob interface
    void prepareSqe(struct io_uring_sqe* sqe) override;
    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;

    /**
     * Start the splice operation
     */
    void start(Server& server);

    ~SpliceFileJob();

    // Public constructor for pool allocation
    SpliceFileJob(int client_fd, int file_fd, uint64_t offset, uint64_t length);

private:
    enum State {
        CreatingPipe,     // Creating the intermediate pipe
        SplicingFileToPipe,  // splice(file_fd → pipe[1])
        SplicingPipeToSocket // splice(pipe[0] → socket_fd)
    };

    void createPipe();
    void startLinkedSplice(Server& server);
    void drainPipeToSocket(Server& server);  // Drain remaining bytes from partial write
    void resubmit(Server& server);
    void cleanup();

    State state_;
    int client_fd_;
    int file_fd_;
    uint64_t offset_;
    uint64_t remaining_;
    size_t total_transferred_;
    bool is_pool_allocated_ = false;  // Track if this job came from pool
    
    int pipe_fds_[2];  // pipe[0] = read end, pipe[1] = write end
    size_t bytes_in_pipe_;  // Track how much data is currently in the pipe
    int pending_operations_;  // Track linked operations
    size_t current_chunk_size_;  // Size of current chunk being transferred
    
    CompletionCallback on_complete_;
    ErrorCallback on_error_;
    
    static constexpr size_t SPLICE_CHUNK_SIZE = 64 * 1024; // 64KB chunks
};

} // namespace caduvelox