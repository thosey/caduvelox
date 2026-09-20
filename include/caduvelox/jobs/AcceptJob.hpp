#pragma once

#include "IoJob.hpp"
#include <functional>
#include <memory>
#include <sys/socket.h>

namespace caduvelox {

/**
 * Job for accepting new connections using multishot accept.
 * Always uses io_uring_prep_multishot_accept for continuous connection acceptance.
 *
 * Pool-allocated, and the pools are thread-local: create() must be called on the
 * ring thread that will service the accept, or the job is taken from one
 * thread's pool and freed into another's. SingleRingHttpServer therefore arms
 * the accept from Server::setStartupFn(), which runs on the ring thread.
 */
class AcceptJob : public IoJob {
public:
    using ConnectionCallback = std::function<void(int client_fd, const sockaddr* addr, socklen_t addrlen)>;
    using ErrorCallback = std::function<void(int error)>;

    /**
     * Create a multishot accept job from the lock-free pool.
     *
     * Must be called on the ring thread that will service the accept -- see the
     * note on the class.
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
    void requestShutdownCancel(Server& server) override;

    // Start the job (submit initial operation)
    void start(Server& server);

    /**
     * Bind the owner's tracking pointer (e.g. SingleRingHttpServer::accept_job_).
     * When this job terminates and is about to be deleted, it nulls the slot —
     * but only if the slot still points at this job — so the owner (and its
     * shutdown sweep) never dereferences a freed AcceptJob. The slot must
     * outlive the job.
     */
    void bindOwnerSlot(AcceptJob** slot) { owner_slot_ = slot; }

    AcceptJob(int server_fd);

private:
    void submitAccept(Server& server);
    void resubmitAccept(Server& server);  // For internal re-submission when already managed
    // Null the bound owner slot if it still points at this job. Called on every
    // path that returns a delete-cleanup from handleCompletion.
    void clearOwnerSlot();

    int server_fd_;
    AcceptJob** owner_slot_ = nullptr;

    ConnectionCallback on_connection_;
    ErrorCallback on_error_;
};

} // namespace caduvelox
