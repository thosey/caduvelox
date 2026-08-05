#pragma once

#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/jobs/CancelJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/ProvidedBufferToken.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include <liburing.h>
#include <errno.h>
#include <concepts>

namespace caduvelox {

/**
 * Concept: ResponseHandler for MultishotRecvJob
 *
 * A valid handler must provide:
 * - onDataToken(ProvidedBufferToken& token) for zero-copy processing
 * - onError(int error) for error handling
 * - shouldRearmRecv() to decide whether a kernel-terminated multishot is revived
 */
template<typename H>
concept ResponseHandler = requires(H handler, int error, ProvidedBufferToken& token) {
    // Required: zero-copy token processing (passed by reference, lifetime managed by caller)
    { handler.onDataToken(token) } -> std::same_as<void>;

    // Required: error handling
    { handler.onError(error) } -> std::same_as<void>;

    // Required: asked after the kernel ends the multishot (IORING_CQE_F_MORE
    // cleared) to decide whether the read stream should be re-armed. Returns
    // false when the owner is closing or already gone, in which case the job
    // reports termination via onError() and frees itself.
    { handler.shouldRearmRecv() } -> std::same_as<bool>;
};

/**
 * MultishotRecvJob - Zero-cost abstraction for persistent multishot socket reads with inline processing
 * 
 * Template-based policy design for optimal performance:
 * - Handler encapsulates context + callbacks (no void* casting)
 * - Direct member calls inline perfectly (no function pointer overhead)
 * - Fully type-safe (compiler checks all types)
 * - Zero heap allocation
 * - Zero-copy buffer token passing for inline processing
 * 
 * Handler Requirements:
 * - void onDataToken(ProvidedBufferToken token)
 * - void onError(int error)
 * - bool shouldRearmRecv()
 *
 * Example Handler:
 * struct HttpConnectionHandler {
 *     HttpConnectionJob* connection;
 *     void onDataToken(ProvidedBufferToken token) {
 *         connection->handleDataReceivedToken(std::move(token));
 *     }
 *     void onError(int error) {
 *         connection->handleReadError(error);
 *     }
 *     bool shouldRearmRecv() {
 *         return connection->wantsContinuedRead();
 *     }
 * };
 *
 * Usage:
 *   HttpConnectionHandler handler{this};
 *   auto* job = PoolManager::allocate<MultishotRecvJob<HttpConnectionHandler>>(fd, handler);
 *
 * Lifecycle:
 * - Created once per connection (pool-allocated per handler type)
 * - Kernel normally continues the multishot until the connection closes or errors
 * - The kernel may also END the multishot on its own (buffer-ring pressure,
 *   internal rearm failure) by clearing IORING_CQE_F_MORE on an otherwise
 *   successful completion. That CQE is the LAST one for this job, so the job
 *   must either re-arm itself or free itself right there — see handleCompletion().
 * - The job is freed only on a completion with IORING_CQE_F_MORE cleared; while
 *   the flag is set the kernel still owns this pointer as SQE user_data.
 */
template<ResponseHandler Handler>
class MultishotRecvJob : public IoJob {
public:
    ~MultishotRecvJob() = default;

    // IoJob interface
    void prepareSqe(struct io_uring_sqe* sqe) override;
    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;

    /**
     * Submit an io_uring cancel SQE targeting this recv operation.
     * Called by the ring-local shutdown sweep for idle keep-alive connections
     * that would otherwise never hit a natural control boundary.
     */
    void requestShutdownCancel(Server& server) override;

    // Constructor must be public for pool allocation
    MultishotRecvJob(int fd, Handler handler, BufferRingCoordinator& coordinator);

private:
    /**
     * Re-submit this job's multishot recv in place after the kernel ended it.
     * Reusing the same object keeps the owner's tracking pointer (and any
     * cancel targeting this job's user_data) valid across the re-arm.
     * @return true if a fresh multishot is armed, false if no SQE was available.
     */
    bool rearm(Server& server);

    int fd_;
    Handler handler_;
    BufferRingCoordinator& coordinator_;
};

// ============================================================================
// Implementation (header-only for templates)
// ============================================================================

/**
 * Cleanup callback returned by handleCompletion() on every terminal path.
 * Server invokes it after the job's completion handler has returned, which is
 * the only point at which the job is safe to return to the pool.
 */
template<ResponseHandler Handler>
void cleanupMultishotRecvJob(IoJob* job) {
    PoolManager::deallocate(static_cast<MultishotRecvJob<Handler>*>(job));
}

template<ResponseHandler Handler>
MultishotRecvJob<Handler>::MultishotRecvJob(int fd, Handler handler, BufferRingCoordinator& coordinator)
    : fd_(fd)
    , handler_(std::move(handler))
    , coordinator_(coordinator)
{
}

template<ResponseHandler Handler>
void MultishotRecvJob<Handler>::prepareSqe(struct io_uring_sqe* sqe) {
    io_uring_prep_recv_multishot(sqe, fd_, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    io_uring_sqe_set_data(sqe, this);
}

template<ResponseHandler Handler>
void MultishotRecvJob<Handler>::requestShutdownCancel(Server& server) {
    // Submit a targeted cancel for this specific recv operation.
    // The user_data on the original SQE was set to `this` in prepareSqe().
    auto* cancel_job = PoolManager::allocate<CancelJob>(reinterpret_cast<uint64_t>(this));
    if (!cancel_job) {
        return; // Pool exhausted — recv will terminate naturally when the fd closes.
    }
    struct io_uring_sqe* sqe = server.registerJob(cancel_job);
    if (sqe) {
        cancel_job->prepareSqe(sqe);
        server.submit();
    } else {
        PoolManager::deallocate(cancel_job);
    }
}

template<ResponseHandler Handler>
bool MultishotRecvJob<Handler>::rearm(Server& server) {
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (!sqe) {
        return false;
    }
    prepareSqe(sqe);
    sqe->buf_group = coordinator_.getBufferGroupId();
    server.submit();
    return true;
}

template<ResponseHandler Handler>
std::optional<IoJob::CleanupCallback> MultishotRecvJob<Handler>::handleCompletion(
    Server& server, struct io_uring_cqe* cqe) {

    const int result = cqe->res;

    // Capture before invoking any handler: the handler can synchronously tear the
    // connection down, but this flag describes the CQE being handled right now.
    // IORING_CQE_F_MORE set => the kernel still owns `this` as SQE user_data and
    // will deliver further completions, so the job must NOT be freed.
    const bool multishot_continues = (cqe->flags & IORING_CQE_F_MORE) != 0;

    // Check for errors or connection close
    if (result <= 0) {
        // Error or EOF - call error handler (direct call, fully inlineable!)
        handler_.onError(result);
        if (multishot_continues) {
            // Rare: an error CQE that does not end the multishot. The owner has
            // been told, but more completions are still coming for this job.
            return std::nullopt;
        }
        return &cleanupMultishotRecvJob<Handler>;
    }

    // Successful read
    unsigned int buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

    void* buffer_ptr = coordinator_.getBufferPtr(buffer_id);
    if (!buffer_ptr) {
        handler_.onError(-EINVAL);
        if (multishot_continues) {
            return std::nullopt;
        }
        return &cleanupMultishotRecvJob<Handler>;
    }

    {
        ProvidedBufferToken token(&coordinator_, buffer_id, static_cast<char*>(buffer_ptr), result);

        // Call token handler (direct call, fully inlineable!)
        handler_.onDataToken(token);
        // Token destructor recycles the buffer back to the ring here, before any
        // re-arm below, so the fresh multishot sees a replenished buffer ring.
    }

    if (multishot_continues) {
        // Multishot continues automatically (no cleanup callback)
        return std::nullopt;
    }

    // The kernel ended the multishot even though this completion carried data
    // (buffer-ring pressure, internal rearm failure, ...). No further CQEs will
    // arrive for this job, so doing nothing here would stall the read stream and
    // strand this pool slot forever.
    if (handler_.shouldRearmRecv() && rearm(server)) {
        // in_flight_ accounting stays balanced: rearm()'s registerJob() increments,
        // and Server::handleCompletion() decrements for this F_MORE-cleared CQE.
        return std::nullopt;
    }

    // Not re-armed — either the owner is closing/gone, or no SQE was available.
    // Report termination so the owner drops its pointer to this job (otherwise it
    // would later cancel a dead operation and wait forever on a completion that
    // can never arrive), then free ourselves.
    handler_.onError(-ECANCELED);
    return &cleanupMultishotRecvJob<Handler>;
}

} // namespace caduvelox
