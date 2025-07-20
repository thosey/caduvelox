#pragma once

#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <liburing.h>
#include <memory>
#include <functional>
#include <string>

namespace caduvelox {

// Forward declarations
class Server;

/**
 * KTLSJob - Performs TLS handshake and enables kernel TLS (kTLS) for a socket.
 * 
 * This job encapsulates the complete TLS handshake process and automatic kTLS setup.
 * It uses OpenSSL with SSL_OP_ENABLE_KTLS to automatically transition to kernel TLS
 * after the handshake completes.
 * 
 * Key Features:
 * - Automatic TLS 1.2 handshake with AES128-GCM-SHA256 cipher
 * - Seamless kTLS enablement for high-performance data transfer
 * - Non-blocking handshake using io_uring poll operations
 * - Proper SSL resource management and cleanup
 * 
 * Usage:
 *   auto ktls_job = KTLSJob::create(
 *       client_fd, ssl_ctx,
 *       [](int fd, SSL* ssl) { 
 *           // kTLS ready, use raw TCP ops 
 *       },
 *       [](int fd, int error) { 
 *           // handshake failed 
 *       }
 *   );
 */
class KTLSJob : public IoJob, public std::enable_shared_from_this<KTLSJob> {
public:
    // TLS handshake states
    enum class State {
        HANDSHAKING,        // Performing TLS handshake steps
        KTLS_READY,         // kTLS enabled, ready for data transfer
        ERROR_STATE         // Error occurred during handshake
    };

    // Completion callbacks
    using SuccessCallback = std::function<void(int client_fd, SSL* ssl)>;
    using ErrorCallback = std::function<void(int client_fd, int error_code)>;

    /**
     * Create KTLS job using lock-free pool allocation.
     * @param client_fd Socket file descriptor for the client connection
     * @param ssl_ctx OpenSSL context configured for kTLS (TLS 1.2, AES128-GCM-SHA256)
     * @param on_success Called when kTLS is ready for data transfer
     * @param on_error Called if handshake fails
     * @return Pointer to pool-allocated KTLSJob, or nullptr if pool exhausted
     */
    static KTLSJob* createFromPool(
        int client_fd,
        SSL_CTX* ssl_ctx,
        SuccessCallback on_success,
        ErrorCallback on_error
    );

    /**
     * Free a pool-allocated KTLSJob (for error cleanup before submission).
     */
    static void freePoolAllocated(KTLSJob* job);

    ~KTLSJob() override;

    // IoJob interface
    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;
    void prepareSqe(struct io_uring_sqe* sqe) override;

    // Job state inspection
    State getState() const { return state_; }
    int getClientFd() const { return client_fd_; }
    SSL* getSSL() const { return ssl_; }
    bool isKTLSEnabled() const { return ktls_enabled_; }

    // Public constructor for pool allocation
    KTLSJob(int client_fd, SSL_CTX* ssl_ctx, SuccessCallback on_success, ErrorCallback on_error);

private:

    // Core handshake logic
    bool initializeSSL();
    bool performHandshakeStep(Server& server);
    bool completeHandshake();  // Common logic for successful SSL_accept
    bool enableKTLS();
    void cleanup();
    void submitPollOperation(Server& server, short events);

    // Job state
    int client_fd_;
    SSL_CTX* ssl_ctx_;          // SSL context (not owned)
    SSL* ssl_;                  // SSL connection (owned)
    State state_;
    bool ktls_enabled_;
    bool is_pool_allocated_ = false;  // Track if this job came from pool
    Logger& logger_;

    // Callbacks
    SuccessCallback on_success_;
    ErrorCallback on_error_;

    // Handshake state tracking
    bool ssl_initialized_;
    int last_ssl_error_;
};

} // namespace caduvelox