#pragma once

#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/MultishotRecvJob.hpp"
#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/http/HttpRouter.hpp"
#include "caduvelox/http/HttpTypes.hpp"
#include "caduvelox/http/HttpParser.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "LockFreeMemoryPool.h"
#include <openssl/ssl.h>
#include <memory>
#include <unordered_map>
#include <string>

namespace caduvelox {

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
     * Set KTLS context (for multi-ring server)
     */
    void setKTLSContext(SSL_CTX* ssl_ctx);

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
    bool ktls_enabled_;
    SSL_CTX* ssl_ctx_;  // For KTLS support

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
class HttpConnectionJob : public IoJob {
    
public:
    /**
     * Create HTTP connection handler using lock-free pool allocation.
     * Returns raw pointer - managed by pool lifecycle via cleanup callbacks.
     * @param client_fd Client socket file descriptor
     * @param job_server Reference to the io_uring server
     * @param router HTTP router for request handling
     * @param http_server Pointer to HTTP server (currently unused, kept for API compatibility)
     * @param max_request_size Maximum request size in bytes
     * @return Pointer to pool-allocated HttpConnectionJob, or nullptr if pool exhausted
     */
    static HttpConnectionJob* createFromPool(
        int client_fd, 
        Server& job_server,
        const HttpRouter& router,
        SingleRingHttpServer* http_server = nullptr,
        size_t max_request_size = 1024 * 1024
    );

    /**
     * Start reading from the connection
     * Must be called after the object is created and in a shared_ptr
     */
    void start();

    // IoJob interface
    void prepareSqe(struct io_uring_sqe* sqe) override;
    std::optional<CleanupCallback> handleCompletion(Server& server, struct io_uring_cqe* cqe) override;

    // Constructor needs to be public for pool allocation
    HttpConnectionJob(int client_fd, Server& job_server, const HttpRouter& router, 
                     SingleRingHttpServer* http_server, size_t max_request_size);

    // Public for stateless callback handlers (called by MultishotRecvJob)
    void handleDataReceived(const char* data, ssize_t len);
    void handleReadError(int error);
    int getClientFd() const { return client_fd_; }

private:

    void startReading();
    void processHttpRequests();
    void handleHttpRequest(const HttpRequest& request);
    void sendResponse(const HttpResponse& response);
    void closeConnection();
    bool shouldKeepAlive(const HttpRequest& request) const;
    void continueReading();

    int client_fd_;
    Server& job_server_;
    HttpRouter router_;
    SingleRingHttpServer* http_server_;  // Currently unused, kept for API compatibility
    std::string request_buffer_;
    size_t max_request_size_;
    bool reading_active_;
    bool keep_alive_;  // Track if connection should remain open
};

} // namespace caduvelox

// Type alias for templated MultishotRecvJob (avoids comma issues in macro)
using HttpMultishotRecvJob = caduvelox::MultishotRecvJob<caduvelox::HttpConnectionRecvHandler>;

// Define cache-aligned lock-free pools for HTTP server jobs (hot paths)
// Large pool for HTTP connection jobs since we can have many concurrent connections
// Cache alignment prevents false sharing between worker threads
DEFINE_LOCKFREE_POOL_CACHE_ALIGNED(caduvelox::HttpConnectionJob, 10000);

// Pool for templated MultishotRecvJob with HttpConnectionRecvHandler (also hot)
DEFINE_LOCKFREE_POOL_CACHE_ALIGNED(HttpMultishotRecvJob, 10000);