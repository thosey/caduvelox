#pragma once

#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/MultishotRecvJob.hpp"
#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/jobs/EventFdMonitorJob.hpp"
#include "caduvelox/http/HttpRouter.hpp"
#include "caduvelox/http/HttpTypes.hpp"
#include "caduvelox/http/HttpParser.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/threading/AffinityWorkerPool.hpp"
#include "caduvelox/ring_buffer/VyukovRingBuffer.hpp"
#include "caduvelox/util/EventFd.hpp"
#include "caduvelox/util/WorkerResponse.hpp"
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
class HttpServer {
public:
    explicit HttpServer(Server& job_server, std::shared_ptr<AffinityWorkerPool> worker_pool = nullptr);
    ~HttpServer();

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
     * Set affinity worker pool for multi-threaded HTTP processing
     */
    void setAffinityWorkerPool(std::shared_ptr<AffinityWorkerPool> pool);
    
    /**
     * Get the response queue for worker threads to post responses
     */
    VyukovRingBuffer<WorkerResponse, 16384>& getResponseQueue() { return response_queue_; }
    
    /**
     * Signal that a worker has posted a response
     */
    void signalWorkerResponse();

private:
    Server& job_server_;
    HttpRouter router_;
    int server_fd_;
    bool running_;
    bool ktls_enabled_;
    SSL_CTX* ssl_ctx_;  // For KTLS support
    
    // Worker thread support
    std::shared_ptr<AffinityWorkerPool> worker_pool_;
    VyukovRingBuffer<WorkerResponse, 16384> response_queue_;  // Lock-free MPMC queue: Worker threads -> io_uring thread (16K slots)
    std::unique_ptr<EventFd> worker_event_fd_;  // For waking up io_uring thread
    EventFdMonitorJob* eventfd_monitor_job_ = nullptr; // Pool-allocated job to monitor eventfd

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
    
    /**
     * Setup eventfd monitoring for worker responses
     */
    void setupWorkerEventFd();
    
    /**
     * Process pending worker responses
     */
    void processWorkerResponses();
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
    // Allow HttpServer to call sendResponse for worker responses
    friend class HttpServer;
    
public:
    /**
     * Create HTTP connection handler using lock-free pool allocation.
     * Returns raw pointer - managed by pool lifecycle via cleanup callbacks.
     * @param client_fd Client socket file descriptor
     * @param job_server Reference to the io_uring server
     * @param router HTTP router for request handling
     * @param http_server Pointer to HTTP server (for worker response queue)
     * @param max_request_size Maximum request size in bytes
     * @return Pointer to pool-allocated HttpConnectionJob, or nullptr if pool exhausted
     */
    static HttpConnectionJob* createFromPool(
        int client_fd, 
        Server& job_server,
        const HttpRouter& router,
        HttpServer* http_server = nullptr,
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
                     HttpServer* http_server, size_t max_request_size);

    // Public for stateless callback handlers (called by MultishotRecvJob)
    void handleDataReceived(const char* data, ssize_t len);
    void handleDataReceivedOnWorker(const char* data, ssize_t len);
    void handleReadError(int error);
    int getClientFd() const { return client_fd_; }

private:

    void startReading();
    void postResponseFromWorker(HttpResponse response);  // Takes by value for proper move semantics
    void processHttpRequests();
    void processHttpRequestsOnWorker();
    void handleHttpRequest(const HttpRequest& request);
    void sendResponse(const HttpResponse& response);
    void closeConnection();
    bool shouldKeepAlive(const HttpRequest& request) const;
    void continueReading();

    int client_fd_;
    Server& job_server_;
    HttpRouter router_;
    HttpServer* http_server_;  // For posting worker responses
    std::string request_buffer_;
    size_t max_request_size_;
    bool reading_active_;
    bool keep_alive_;  // Track if connection should remain open
};

} // namespace caduvelox

// Type alias for templated MultishotRecvJob (avoids comma issues in macro)
using HttpMultishotRecvJob = caduvelox::MultishotRecvJob<caduvelox::HttpConnectionRecvHandler>;

// Define lock-free pools for HTTP server jobs at global scope
// Large pool for HTTP connection jobs since we can have many concurrent connections
DEFINE_LOCKFREE_POOL(caduvelox::HttpConnectionJob, 10000);

// Pool for templated MultishotRecvJob with HttpConnectionRecvHandler
DEFINE_LOCKFREE_POOL(HttpMultishotRecvJob, 10000);