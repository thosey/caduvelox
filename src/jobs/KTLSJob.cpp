#include "caduvelox/jobs/KTLSJob.hpp"
#include "caduvelox/Server.hpp"
#include "LockFreeMemoryPool.h"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>

// Define lock-free pool for KTLSJob
// Medium pool since KTLS operations are used for secure connections
DEFINE_LOCKFREE_POOL(caduvelox::KTLSJob, 1000);

namespace {
    void cleanupKTLSJob(caduvelox::IoJob* job) {
        lfmemorypool::lockfree_pool_free_fast<caduvelox::KTLSJob>(
            static_cast<caduvelox::KTLSJob*>(job)
        );
    }
}

namespace caduvelox {

KTLSJob* KTLSJob::createFromPool(
    int client_fd,
    SSL_CTX* ssl_ctx,
    SuccessCallback on_success,
    ErrorCallback on_error
) {
    KTLSJob* job = lfmemorypool::lockfree_pool_alloc_fast<KTLSJob>(client_fd, ssl_ctx, std::move(on_success), std::move(on_error));
    if (job) {
        job->is_pool_allocated_ = true;
    }
    return job;
}

void KTLSJob::freePoolAllocated(KTLSJob* job) {
    if (job) {
        lfmemorypool::lockfree_pool_free_fast<KTLSJob>(job);
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
    if (!SSL_set_fd(ssl_, client_fd_)) {
        logger_.logError("KTLSJob: Failed to set SSL file descriptor");
        SSL_free(ssl_);
        ssl_ = nullptr;
        return false;
    }

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
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (sqe) {
        io_uring_prep_poll_add(sqe, client_fd_, events);
        server.submit();
    } else {
        state_ = State::ERROR_STATE;
        if (on_error_) {
            on_error_(client_fd_, -ENOMEM);
        }
    }
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
        io_uring_prep_poll_add(sqe, client_fd_, POLLIN);
        break;

    case SSL_ERROR_WANT_WRITE:
        io_uring_prep_poll_add(sqe, client_fd_, POLLOUT);
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
    if (state_ == State::ERROR_STATE || state_ == State::KTLS_READY) {
        // Job finished (success or error), return cleanup function
        if (is_pool_allocated_) {
            return cleanupKTLSJob;
        }
        return std::nullopt;
    }

    // We're in HANDSHAKING state
    if (cqe->res < 0) {
        logger_.logError("KTLSJob: Poll operation failed for fd=" + std::to_string(client_fd_) + 
                        ", error: " + std::string(strerror(-cqe->res)));
        state_ = State::ERROR_STATE;
        if (on_error_) {
            on_error_(client_fd_, cqe->res);
        }
        // Return cleanup for error state
        if (is_pool_allocated_) {
            return cleanupKTLSJob;
        }
        return std::nullopt;
    }

    // Continue handshake process
    if (!performHandshakeStep(server)) {
        // Error occurred or job completed - return cleanup if needed
        if ((state_ == State::ERROR_STATE || state_ == State::KTLS_READY) && is_pool_allocated_) {
            return cleanupKTLSJob;
        }
    }

    // Continue processing - another poll operation was submitted
    return std::nullopt;
}

} // namespace caduvelox
