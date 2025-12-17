#include "caduvelox/jobs/KTLSJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>

// Pool capacity specialization for KTLSJob
#ifndef CDV_KTLS_POOL_SIZE
#define CDV_KTLS_POOL_SIZE 5000
#endif
template<>
constexpr size_t caduvelox::PoolManager::getPoolCapacity<caduvelox::KTLSJob>() {
    return CDV_KTLS_POOL_SIZE; // HTTPS handshake operations
}

namespace {
    void cleanupKTLSJob(caduvelox::IoJob* job) {
        caduvelox::PoolManager::deallocate(static_cast<caduvelox::KTLSJob*>(job));
    }
}

namespace caduvelox {

KTLSJob* KTLSJob::createFromPool(
    int client_fd,
    SSL_CTX* ssl_ctx,
    SuccessCallback on_success,
    ErrorCallback on_error
) {
    KTLSJob* job = PoolManager::allocate<KTLSJob>(client_fd, ssl_ctx, std::move(on_success), std::move(on_error));
    if (job) {
        job->is_pool_allocated_ = true;
    }
    return job;
}

void KTLSJob::freePoolAllocated(KTLSJob* job) {
    if (job) {
        PoolManager::deallocate<KTLSJob>(job);
    }
}

KTLSJob::KTLSJob(int client_fd, SSL_CTX* ssl_ctx, SuccessCallback on_success, ErrorCallback on_error)
    : client_fd_(client_fd)
    , ssl_ctx_(ssl_ctx)
    , ssl_(nullptr)
    , state_(State::HANDSHAKING)
    , ktls_enabled_(false)
    , logger_(Logger::getInstance())
    , on_success_(std::move(on_success))
    , on_error_(std::move(on_error))
    , ssl_initialized_(false)
    , last_ssl_error_(0)
    , pending_operations_(0)
{
    // Set socket to non-blocking for handshake
    int flags = fcntl(client_fd_, F_GETFL, 0);
    if (flags != -1) {
        fcntl(client_fd_, F_SETFL, flags | O_NONBLOCK);
    }
}

KTLSJob::~KTLSJob() {
    cleanup();
}

void KTLSJob::cleanup() {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    ssl_initialized_ = false;
}

bool KTLSJob::initializeSSL() {
    if (ssl_initialized_) {
        return true;
    }

    if (!ssl_ctx_) {
        logger_.logError("KTLSJob: Invalid SSL context");
        return false;
    }

    // Create SSL connection
    ssl_ = SSL_new(ssl_ctx_);
    if (!ssl_) {
        logger_.logError("KTLSJob: Failed to create SSL connection");
        return false;
    }

    // Enable kTLS option to let OpenSSL automatically use kernel TLS
    SSL_set_options(ssl_, SSL_OP_ENABLE_KTLS);
    logger_.logMessage("KTLSJob: Enabled SSL_OP_ENABLE_KTLS for automatic kTLS support");

    // Set the socket file descriptor
    logger_.logMessage("KTLSJob: Setting SSL fd=" + std::to_string(client_fd_));
    if (!SSL_set_fd(ssl_, client_fd_)) {
        logger_.logError("KTLSJob: Failed to set SSL file descriptor for fd=" + std::to_string(client_fd_));
        SSL_free(ssl_);
        ssl_ = nullptr;
        return false;
    }
    logger_.logMessage("KTLSJob: SSL fd set successfully, ssl_initialized=true");

    ssl_initialized_ = true;
    return true;
}

bool KTLSJob::completeHandshake() {
    // Handshake completed successfully!
    logger_.logMessage("KTLSJob: TLS handshake completed for fd=" + std::to_string(client_fd_));

    // Enable kTLS - this is required for io_uring TLS operations
    if (enableKTLS()) {
        state_ = State::KTLS_READY;
        ktls_enabled_ = true;
        logger_.logMessage("KTLSJob: kTLS enabled for fd=" + std::to_string(client_fd_));
        if (on_success_) {
            on_success_(client_fd_, ssl_);
        }
        return true;
    } else {
        logger_.logError("KTLSJob: Failed to enable kTLS for fd=" + std::to_string(client_fd_) + 
                       " - kTLS is required for io_uring TLS operations");
        state_ = State::ERROR_STATE;
        if (on_error_) {
            on_error_(client_fd_, -1);
        }
        return false;
    }
}

bool KTLSJob::performHandshakeStep(Server& server) {
    if (!ssl_initialized_ && !initializeSSL()) {
        return false;
    }

    int ret = SSL_accept(ssl_);
    
    if (ret == 1) {
        return completeHandshake();
    }

    // Handshake needs more I/O
    int ssl_error = SSL_get_error(ssl_, ret);
    last_ssl_error_ = ssl_error;

    switch (ssl_error) {
    case SSL_ERROR_WANT_READ:
        submitPollOperation(server, POLLIN);
        return true;  // Continue processing

    case SSL_ERROR_WANT_WRITE:
        submitPollOperation(server, POLLOUT);
        return true;  // Continue processing

    default:
        // Real error occurred
        logger_.logError("KTLSJob: SSL handshake error for fd=" + std::to_string(client_fd_) + 
                       ", SSL error: " + std::to_string(ssl_error));
        state_ = State::ERROR_STATE;
        if (on_error_) {
            on_error_(client_fd_, ssl_error);
        }
        return false;
    }
}

bool KTLSJob::enableKTLS() {
    // Check if OpenSSL has automatically set up kTLS
    BIO* bio = SSL_get_wbio(ssl_);
    if (bio != nullptr && BIO_get_ktls_send(bio) && BIO_get_ktls_recv(bio)) {
        logger_.logMessage("KTLSJob: kTLS fully enabled automatically by OpenSSL for fd=" +
                          std::to_string(client_fd_));
        return true;
    }

    // Get information about why kTLS might not be available
    const char* version = SSL_get_version(ssl_);
    const char* cipher = SSL_get_cipher_name(ssl_);
    
    logger_.logError("KTLSJob: kTLS not available for fd=" + std::to_string(client_fd_) + 
                    " - TLS version: " + (version ? version : "unknown") + 
                    ", cipher: " + (cipher ? cipher : "unknown") + 
                    ". kTLS requires specific cipher/version combinations.");
    
    return false;
}

void KTLSJob::submitPollOperation(Server& server, short events) {
    // Add a poll request linked with a timeout to prevent handshake hangs from
    // exhausting the KTLSJob pool. If the peer never progresses the TLS
    // handshake, the link timeout will fire and we'll error out, returning
    // this job to the pool.

    // Default 5000ms; overridable via -DCDV_KTLS_HANDSHAKE_TIMEOUT_MS
#ifndef CDV_KTLS_HANDSHAKE_TIMEOUT_MS
#define CDV_KTLS_HANDSHAKE_TIMEOUT_MS 5000
#endif
    constexpr unsigned HANDSHAKE_TIMEOUT_MS = CDV_KTLS_HANDSHAKE_TIMEOUT_MS; // per step

    struct io_uring_sqe* sqe = server.registerJob(this);
    if (!sqe) {
        state_ = State::ERROR_STATE;
        if (on_error_) {
            on_error_(client_fd_, -ENOMEM);
        }
        return;
    }

    // Prepare the poll with a linked timeout
    io_uring_prep_poll_add(sqe, client_fd_, events);
    sqe->flags |= IOSQE_IO_LINK; // next SQE is a linked timeout

    // Prepare the link timeout SQE
    struct io_uring_sqe* tsqe = server.registerJob(this);
    if (!tsqe) {
        // If we cannot enqueue the timeout, still submit the poll to avoid stalling
        // Increment pending for the poll operation we just added
        pending_operations_++;
        server.submit();
        return;
    }

    __kernel_timespec ts{};
    ts.tv_sec = HANDSHAKE_TIMEOUT_MS / 1000;
    ts.tv_nsec = (HANDSHAKE_TIMEOUT_MS % 1000) * 1000000ULL;
    io_uring_prep_link_timeout(tsqe, &ts, 0);

    // We submitted 2 linked operations (POLL + TIMEOUT)
    // Both will deliver completions, so increment counter by 2
    pending_operations_ += 2;

    server.submit();
}

void KTLSJob::prepareSqe(struct io_uring_sqe* sqe) {
    // This method is called when the job is first registered
    // We'll start the handshake process
    if (!ssl_initialized_) {
        if (!initializeSSL()) {
            // If we can't initialize SSL, set error state and prepare nop for cleanup
            state_ = State::ERROR_STATE;
            if (on_error_) {
                on_error_(client_fd_, -1);
            }
            io_uring_prep_nop(sqe);
            return;
        }
    }

    // Try the handshake step
    int ret = SSL_accept(ssl_);
    
    if (ret == 1) {
        // Handshake completed!
        completeHandshake();
        // Prepare nop to get completion for cleanup
        io_uring_prep_nop(sqe);
        return;
    }

    // Handshake needs more I/O
    int ssl_error = SSL_get_error(ssl_, ret);
    last_ssl_error_ = ssl_error;

    switch (ssl_error) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        // Defer to performHandshakeStep() which will post poll with a
        // linked timeout using submitPollOperation(). We prepare a NOP
        // here to get an immediate completion and drive the state machine.
        io_uring_prep_nop(sqe);
        break;

    default:
        // Error occurred, set error state and prepare nop for cleanup
        logger_.logError("KTLSJob: SSL handshake error for fd=" + std::to_string(client_fd_) + 
                       ", SSL error: " + std::to_string(ssl_error));
        state_ = State::ERROR_STATE;
        if (on_error_) {
            on_error_(client_fd_, ssl_error);
        }
        io_uring_prep_nop(sqe);
        break;
    }
}

std::optional<IoJob::CleanupCallback> KTLSJob::handleCompletion(Server& server, struct io_uring_cqe* cqe) {
    // Decrement pending operations counter for this completion
    if (pending_operations_ > 0) {
        pending_operations_--;
    }
    
    if (state_ == State::ERROR_STATE || state_ == State::KTLS_READY) {
        // Job finished (success or error), but check if all operations completed
        if (pending_operations_ == 0 && is_pool_allocated_) {
            return cleanupKTLSJob;
        }
        // Still have pending operations, don't cleanup yet
        return std::nullopt;
    }

    // We're in HANDSHAKING state
    if (cqe->res < 0) {
            // -ECANCELED indicates the linked timeout was canceled because the poll
            // completed normally earlier. We ignore these and continue.
            if (cqe->res == -ECANCELED) {
                // Timeout was canceled, check if we should cleanup now
                if ((state_ == State::ERROR_STATE || state_ == State::KTLS_READY) && 
                    pending_operations_ == 0 && is_pool_allocated_) {
                    return cleanupKTLSJob;
                }
                return std::nullopt;
            }

        logger_.logError("KTLSJob: Poll operation failed for fd=" + std::to_string(client_fd_) + 
                        ", error: " + std::string(strerror(-cqe->res)));
            logger_.logError("KTLSJob: Poll/timeout failure for fd=" + std::to_string(client_fd_) +
                             ", error: " + std::string(strerror(-cqe->res)) +
                             (cqe->res == -ETIME ? " (handshake step timed out)" : ""));
            state_ = State::ERROR_STATE;
        if (on_error_) {
            on_error_(client_fd_, cqe->res);
        }
        // Return cleanup only if all operations completed
        if (pending_operations_ == 0 && is_pool_allocated_) {
            return cleanupKTLSJob;
        }
        return std::nullopt;
    }

    // Continue handshake process
    if (!performHandshakeStep(server)) {
        // Error occurred or job completed - return cleanup only if all operations done
        if ((state_ == State::ERROR_STATE || state_ == State::KTLS_READY) && 
            pending_operations_ == 0 && is_pool_allocated_) {
            return cleanupKTLSJob;
        }
    }

    // Continue processing - another poll operation was submitted
    return std::nullopt;
}

} // namespace caduvelox
