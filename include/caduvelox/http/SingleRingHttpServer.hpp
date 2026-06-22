#pragma once

#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/CancelJob.hpp"
#include "caduvelox/jobs/MultishotRecvJob.hpp"
#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/http/HttpRouter.hpp"
#include "caduvelox/http/HttpTypes.hpp"
#include "caduvelox/http/HttpParser.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <openssl/ssl.h>
#include <memory>
#include <unordered_map>
#include <string>

namespace caduvelox {

class IdleTimeoutJob;

/**
 * Pure job-based HTTP server - no IConsumer abstraction needed!
 * 
 * This server directly manages HTTP connections using jobs:
 * 1. AcceptJob accepts new connections
 * 2. HttpConnectionJob manages each connection's lifecycle
 * 3. ReadJob/WriteJob handle I/O for each connection
 * 4. HttpRouter processes requests
 * 
 * Benefits over IConsumer approach:
 * - Direct job management, no abstraction layers
 * - Simpler lifetime management
 * - No awkward dynamic_cast or interface juggling
 * - Clean separation of concerns
 * - Better performance (fewer virtual calls)
 */
class SingleRingHttpServer {
public:
    /**
     * Create HTTP server on a specific Server instance
     * @param job_server The io_uring server to use
     */
    explicit SingleRingHttpServer(Server& job_server);
    ~SingleRingHttpServer();

    /**
     * Add HTTP route
     */
    void addRoute(const std::string& method, const std::string& pathRegex, HttpHandler handler);

    /**
     * Add HTTP route with regex capture group support (avoids double regex execution)
     */
    void addRouteWithCaptures(const std::string& method, const std::string& pathRegex, HttpHandlerWithCaptures handler);

    /**
     * Start listening on the given port
     */
    bool listen(int port, const std::string& bind_addr = "0.0.0.0");

    /**
     * Start listening with KTLS support (HTTPS)
     * @param port Port to listen on
     * @param cert_path Path to SSL certificate file (.pem)
     * @param key_path Path to SSL private key file (.pem)
     * @param bind_addr Address to bind to
     */
    bool listenKTLS(int port, const std::string& cert_path, const std::string& key_path, 
                    const std::string& bind_addr = "0.0.0.0");

    /**
     * Stop the server
     */
    void stop();

    /**
     * Set the router (for multi-ring server where router is shared)
     */
    void setRouter(const HttpRouter& router);

    /**
     * Set KTLS context (for multi-ring setup)
     * @param ssl_ctx The SSL context to use
     * @param take_ownership If true, this instance will free the context on destruction
     */
    void setKTLSContext(SSL_CTX* ssl_ctx, bool take_ownership = true);

    /**
     * Override the per-step TLS handshake timeout (default 5000 ms).
     * Must be called before listenKTLS() / listenOnFd().
     */
    void setKtlsHandshakeTimeoutMs(unsigned ms) { ktls_handshake_timeout_ms_ = ms; }

    /**
     * Override the keep-alive idle timeout (default 60000 ms). A connection that
     * sits idle waiting for the next request for longer than this is closed.
     * Pass 0 to disable the idle timeout entirely.
     * Must be called before listen() / listenKTLS() / listenOnFd().
     */
    void setIdleTimeoutMs(unsigned ms) { idle_timeout_ms_ = ms; }

    /**
     * Start listening on an existing socket fd (for multi-ring with SO_REUSEPORT)
     * @param server_fd Pre-created listening socket
     * @return true on success
     */
    bool listenOnFd(int server_fd);

private:
    Server& job_server_;
    HttpRouter router_;
    int server_fd_;
    bool running_;
    AcceptJob* accept_job_ = nullptr;
    bool ktls_enabled_;
    SSL_CTX* ssl_ctx_;  // For KTLS support
    bool owns_ssl_ctx_;  // Whether this instance should free ssl_ctx_
    unsigned ktls_handshake_timeout_ms_{5000};  // Per-step handshake timeout
    unsigned idle_timeout_ms_{60000};  // Keep-alive idle timeout (0 = disabled)

    /**
     * Start accepting connections
     */
    void startAccepting();

    /**
     * Handle new connection from AcceptJob
     */
    void handleNewConnection(int client_fd, const sockaddr* addr = nullptr, socklen_t addrlen = 0);

    /**
     * Handle completed KTLS handshake
     */
    void handleKTLSReady(int client_fd, SSL* ssl);

    /**
     * Handle KTLS handshake error
     */
    void handleKTLSError(int client_fd, int error);

    /**
     * Create connection handler job for a client
     */
    void createConnectionHandler(int client_fd);

    /**
     * Common socket setup helper
     */
    int createServerSocket(int port, const std::string& bind_addr);
};

/**
 * Job that manages the lifecycle of a single HTTP connection.
 * 
 * This job:
 * - Starts a ReadJob for incoming data
 * - Parses HTTP requests as data arrives
 * - Routes complete requests through HttpRouter
 * - Sends responses via WriteJob
 * - Handles connection cleanup
 * 
 * Note: This class uses shared_ptr for lifetime management due to complex async
 * operation dependencies. Pool factory exists but is currently unused.
 */
/**
 * HTTP connection handler (pool-allocated)
 * Manages HTTP request/response lifecycle for a single client connection
 */
class HttpConnectionJob {

public:
    /**
     * Start reading from the connection
     * Must be called after the object is created
     */
    void start();

    // Constructor needs to be public for pool allocation
    HttpConnectionJob(int client_fd, Server& job_server, const HttpRouter& router,
                     size_t max_request_size, unsigned idle_timeout_ms);

    // Public for stateless callback handlers (called by MultishotRecvJob)
    void handleDataReceived(const char* data, ssize_t len);
    void handleReadError(int error);
    int getClientFd() const { return client_fd_; }

    // Public for IdleTimeoutJob's completion callback. `job` identifies which
    // timer instance is completing, so a stale completion from a timer that
    // has already been superseded by a fresher one can be safely ignored.
    void handleIdleTimeout(IdleTimeoutJob* job, int res);

private:

    void startReading();
    void processHttpRequests();
    void handleHttpRequest(const HttpRequest& request);
    void sendResponse(const HttpResponse& response);
    void closeConnection();
    bool shouldKeepAlive(const HttpRequest& request) const;
    void continueReading();
    // Submit an io_uring cancel SQE targeting active_read_job_.
    // Returns true if the cancel was successfully submitted.
    bool submitRecvCancel();
    // Arm a fresh idle-wait timeout. No-op if idle_timeout_ms_ == 0.
    void armIdleTimeout();
    // Submit an io_uring cancel SQE targeting active_idle_timeout_job_.
    // Returns true if the cancel was successfully submitted.
    bool submitIdleTimeoutCancel();

    int client_fd_;
    Server& job_server_;
    const HttpRouter& router_;
    std::string request_buffer_;
    size_t max_request_size_;
    unsigned idle_timeout_ms_;
    bool reading_active_;
    bool keep_alive_;  // Track if connection should remain open
    int pending_writes_;  // Track pending write operations to prevent use-after-free
    bool close_pending_;  // Track if close was requested while writes were pending

    // Multishot recv cancellation support for deferred close during shutdown.
    MultishotRecvJob<HttpConnectionRecvHandler>* active_read_job_{nullptr};  // currently armed recv job, or nullptr
    bool read_cancel_pending_{false};  // cancel SQE already submitted for active_read_job_
    int  deferred_close_fd_{-1};      // real fd to close once the recv terminates

    // Idle-wait timeout: closes connections that sit idle between keep-alive requests.
    IdleTimeoutJob* active_idle_timeout_job_{nullptr};  // currently armed timer, or nullptr
    bool idle_cancel_pending_{false};  // cancel SQE already submitted for active_idle_timeout_job_
};

} // namespace caduvelox

// Pool capacity configurations for HTTP jobs
template<>
inline size_t caduvelox::PoolCapacityConfig<caduvelox::HttpConnectionJob>::capacity = 10000;

// Type alias for templated MultishotRecvJob (avoids comma issues in macro)
using HttpMultishotRecvJob = caduvelox::MultishotRecvJob<caduvelox::HttpConnectionRecvHandler>;

// Pool capacity for MultishotRecvJob with HttpConnectionRecvHandler
template<>
inline size_t caduvelox::PoolCapacityConfig<HttpMultishotRecvJob>::capacity = 10000;

// Thread-local pools are managed by PoolManager
// Each ring thread gets its own independent pools with zero synchronization
