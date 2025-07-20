#pragma once

#include "IoJob.hpp"
#include <functional>
#include <memory>
#include <sys/socket.h>

namespace caduvelox {

/**
 * Job for accepting new connections using multishot accept.
 * Always uses io_uring_prep_multishot_accept for continuous connection acceptance.
 */
class AcceptJob : public IoJob, public std::enable_shared_from_this<AcceptJob> {
public:
    using ConnectionCallback = std::function<void(int client_fd, const sockaddr* addr, socklen_t addrlen)>;
    using ErrorCallback = std::function<void(int error)>;

    /**
     * Create a multishot accept job from lock-free pool 
     * @param server_fd The listening socket file descriptor
     * @param on_connection Callback for new connections (optional)
     * @param on_error Callback for errors (optional)
     * @return New AcceptJob allocated from pool, or nullptr if pool exhausted
     */
    static AcceptJob* create(int server_fd,
                            ConnectionCallback on_connection = nullptr,
                            ErrorCallback on_error = nullptr);

    /**
     * Free a pool-allocated AcceptJob (for error cleanup)
     * @param job The job to free
     */
    static void freePoolAllocated(AcceptJob* job);

    // IoJob interface
    void prepareSqe(struct io_uring_sqe* sqe) override;
    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;

    // Start the job (submit initial operation)
    void start(Server& server);

    AcceptJob(int server_fd);

private:
    void submitAccept(Server& server);
    void resubmitAccept(Server& server);  // For internal re-submission when already managed

    int server_fd_;
    
    ConnectionCallback on_connection_;
    ErrorCallback on_error_;
};

} // namespace caduvelox
