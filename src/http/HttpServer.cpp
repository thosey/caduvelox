#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/jobs/KTLSJob.hpp"
#include "caduvelox/jobs/KTLSContextHelper.hpp"
#include "caduvelox/jobs/MultishotRecvJob.hpp"
#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include "caduvelox/util/ProvidedBufferToken.hpp"
#include "caduvelox/Config.hpp"
#include "LockFreeMemoryPool.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <algorithm>

namespace {
    void cleanupHttpConnectionJob(caduvelox::IoJob* job) {
        lfmemorypool::lockfree_pool_free_fast<caduvelox::HttpConnectionJob>(
            static_cast<caduvelox::HttpConnectionJob*>(job)
        );
    }
}

namespace caduvelox {

HttpServer::HttpServer(Server& job_server, std::shared_ptr<AffinityWorkerPool> worker_pool)
    : job_server_(job_server)
    , router_()
    , server_fd_(-1)
    , running_(false)
    , ktls_enabled_(false)
    , ssl_ctx_(nullptr)
    , worker_pool_(std::move(worker_pool))
{
    // Create default AffinityWorkerPool if not provided
    if (!worker_pool_) {
        worker_pool_ = std::make_shared<AffinityWorkerPool>(std::thread::hardware_concurrency());
    }
    
    // Register worker pool with Server for MultishotRecvJob access
    job_server_.setAffinityWorkerPool(worker_pool_);
    
    // setupWorkerEventFd() is called in listen() / listenKTLS(), not here
}

HttpServer::~HttpServer() {
    stop();
    
    // Close worker eventfd - this should happen AFTER io_uring loop has stopped
    // No completion will be delivered because the loop is not running
    if (worker_event_fd_) {
        worker_event_fd_.reset();
    }
    
    // If eventfd_monitor_job_ still exists, it means the error callback never ran
    // Free it manually to prevent pool leak
    if (eventfd_monitor_job_) {
        EventFdMonitorJob::freePoolAllocated(eventfd_monitor_job_);
        eventfd_monitor_job_ = nullptr;
    }
    
    if (ssl_ctx_) {
        KTLSContextHelper::freeContext(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
}

void HttpServer::addRoute(const std::string& method, const std::string& pathRegex, HttpHandler handler) {
    router_.addRoute(method, pathRegex, std::move(handler));
}

void HttpServer::addRouteWithCaptures(const std::string& method, const std::string& pathRegex, HttpHandlerWithCaptures handler) {
    router_.addRouteWithCaptures(method, pathRegex, std::move(handler));
}

bool HttpServer::listen(int port, const std::string& bind_addr) {
    if (running_) {
        Logger::getInstance().logError("HttpServer: Server is already running");
        return false;
    }

    // Validate port range
    if (port < 0 || port > 65535) {
        Logger::getInstance().logError("HttpServer: Invalid port " + std::to_string(port) + 
                                     " (must be between 0 and 65535)");
        return false;
    }

    // Create and configure server socket
    server_fd_ = createServerSocket(port, bind_addr);
    if (server_fd_ < 0) {
        return false;
    }

    running_ = true;
    ktls_enabled_ = false;
    Logger::getInstance().logMessage("HttpServer: Listening on " + bind_addr + ":" + std::to_string(port));

    // Setup eventfd for worker notifications if we have a worker pool
    if (worker_pool_ && !worker_event_fd_) {
        setupWorkerEventFd();
    }

    // Start accepting connections
    startAccepting();
    
    return true;
}

bool HttpServer::listenKTLS(int port, const std::string& cert_path, const std::string& key_path, 
                              const std::string& bind_addr) {
    if (running_) {
        Logger::getInstance().logError("HttpServer: Server is already running");
        return false;
    }

    // Validate port range
    if (port < 0 || port > 65535) {
        Logger::getInstance().logError("HttpServer: Invalid port " + std::to_string(port) + 
                                     " (must be between 0 and 65535)");
        return false;
    }

    // Create SSL context for KTLS
    ssl_ctx_ = KTLSContextHelper::createServerContext(cert_path, key_path);
    if (!ssl_ctx_) {
        Logger::getInstance().logError("HttpServer: Failed to create SSL context for KTLS");
        return false;
    }

    // Create and configure server socket
    server_fd_ = createServerSocket(port, bind_addr);
    if (server_fd_ < 0) {
        KTLSContextHelper::freeContext(ssl_ctx_);
        ssl_ctx_ = nullptr;
        return false;
    }

    running_ = true;
    ktls_enabled_ = true;
    Logger::getInstance().logMessage("HttpServer: KTLS listening on " + bind_addr + ":" + std::to_string(port));

    // Setup eventfd for worker notifications if we have a worker pool
    if (worker_pool_ && !worker_event_fd_) {
        setupWorkerEventFd();
    }

    // Start accepting connections
    startAccepting();
    
    return true;
}

void HttpServer::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    
    // Close server socket to stop accepting new connections
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    
    // Note: Do NOT close worker_event_fd_ here!
    // It will be closed in the destructor after io_uring loop has fully stopped.

    Logger::getInstance().logMessage("HttpServer: Server stopped");
}

void HttpServer::startAccepting() {
    if (!running_) {
        return;
    }

    auto accept_job = AcceptJob::create(
        server_fd_,
        [this](int client_fd, const sockaddr* addr, socklen_t addrlen) {
            handleNewConnection(client_fd, addr, addrlen);
        },
        [this](int error) {
            Logger::getInstance().logError("HttpServer: Accept error: " + std::to_string(error));
            // Continue accepting unless we're stopping
            if (running_) {
                startAccepting();
            }
        }
    );

    // Guard against pool exhaustion
    if (!accept_job) {
        Logger::getInstance().logError("HttpServer: Failed to allocate AcceptJob (pool exhausted?)");
        // Try again after a brief delay - this is a critical operation
        // In production, you might want exponential backoff or alerting
        return;
    }

    struct io_uring_sqe* sqe = job_server_.registerJob(accept_job);
    if (sqe) {
        accept_job->prepareSqe(sqe);
        job_server_.submit();
    } else {
        Logger::getInstance().logError("HttpServer: Failed to register AcceptJob");
        // Free the job since we couldn't register it
        delete accept_job;
    }
}

void HttpServer::handleNewConnection(int client_fd, const sockaddr* addr, socklen_t addrlen) {
    // Log connection with [ACCESS] prefix for easy filtering
    std::string access_log = "[ACCESS] CONNECT fd=" + std::to_string(client_fd);
    
    if (addr) {
        char ip_str[INET6_ADDRSTRLEN];
        int port = 0;
        
        if (addr->sa_family == AF_INET) {
            auto* addr4 = (const sockaddr_in*)addr;
            inet_ntop(AF_INET, &addr4->sin_addr, ip_str, sizeof(ip_str));
            port = ntohs(addr4->sin_port);
        } else if (addr->sa_family == AF_INET6) {
            auto* addr6 = (const sockaddr_in6*)addr;
            inet_ntop(AF_INET6, &addr6->sin6_addr, ip_str, sizeof(ip_str));
            port = ntohs(addr6->sin6_port);
        } else {
            strcpy(ip_str, "unknown");
        }
        
        access_log += " from " + std::string(ip_str) + ":" + std::to_string(port);
    }
    
    Logger::getInstance().logMessage(access_log);
    
    if (ktls_enabled_) {
        // Start KTLS handshake for this connection
        auto ktls_job = KTLSJob::createFromPool(
            client_fd,
            ssl_ctx_,
            [this](int fd, SSL* ssl) {
                handleKTLSReady(fd, ssl);
            },
            [this](int fd, int error) {
                handleKTLSError(fd, error);
            }
        );

        // Guard against pool exhaustion
        if (!ktls_job) {
            Logger::getInstance().logError("HttpServer: Failed to allocate KTLSJob (pool exhausted?), closing connection");
            close(client_fd);
            return;
        }

        // Register and start the kTLS job
        struct io_uring_sqe* sqe = job_server_.registerJob(ktls_job);
        if (sqe) {
            // Let the job prepare its own SQE
            ktls_job->prepareSqe(sqe);
            job_server_.submit();
        } else {
            Logger::getInstance().logError("HttpServer: Failed to register KTLS job");
            KTLSJob::freePoolAllocated(ktls_job);
            close(client_fd);
        }
    } else {
        // Regular HTTP connection
        createConnectionHandler(client_fd);
    }
    
    // DO NOT call startAccepting() here!
    // AcceptJob is multishot and automatically continues accepting.
    // It only calls handleNewConnection when a new connection arrives.
}

void HttpServer::createConnectionHandler(int client_fd) {
    // Create connection from pool (returns raw pointer - no heap allocation!)
    auto* connection_job = HttpConnectionJob::createFromPool(client_fd, job_server_, router_, this);
    
    if (!connection_job) {
        Logger::getInstance().logError("HttpServer: Failed to allocate HttpConnectionJob from pool");
        close(client_fd);
        return;
    }
    
    // Start the connection job
    // Lifetime managed via atomic state machine (AVAILABLE/WORKING/DISCARDED)
    // Truly lock-free - no mutexes, no heap allocations!
    connection_job->start();
    
    // Job stays alive until closeConnection() calls tryDiscard()
    // Worker threads use tryAcquire()/tryRelease() for safe access
}

void HttpServer::handleKTLSReady(int client_fd, SSL* ssl) {
    Logger::getInstance().logMessage("HttpServer: KTLS ready for fd=" + std::to_string(client_fd));
    
    // KTLS handshake completed successfully!
    // At this point, the connection is encrypted and kernel TLS is enabled.
    // We can now treat it as a regular HTTP connection since the kernel
    // will handle TLS encryption/decryption transparently.
    
    // Note: The SSL* object is not needed for further operations since
    // kTLS allows us to use regular TCP read/write operations.
    
    createConnectionHandler(client_fd);
}

void HttpServer::handleKTLSError(int client_fd, int error) {
    Logger::getInstance().logError("HttpServer: KTLS handshake failed for fd=" + 
                                  std::to_string(client_fd) + ", error=" + std::to_string(error));
    close(client_fd);
}

int HttpServer::createServerSocket(int port, const std::string& bind_addr) {
    // Additional port validation (should have been caught earlier, but defensive programming)
    if (port < 0 || port > 65535) {
        Logger::getInstance().logError("HttpServer: Invalid port " + std::to_string(port) + 
                                     " in createServerSocket (must be between 0 and 65535)");
        return -1;
    }

    // Create server socket
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        Logger::getInstance().logError("HttpServer: Failed to create socket: " + std::string(strerror(errno)));
        return -1;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
        close(socket_fd);
        return -1;
    }

    // Bind to address
    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<uint16_t>(port));
    
    // More specific inet_pton error handling
    int inet_result = inet_pton(AF_INET, bind_addr.c_str(), &server_addr.sin_addr);
    if (inet_result == 0) {
        Logger::getInstance().logError("HttpServer: Invalid IPv4 address format: " + bind_addr);
        close(socket_fd);
        return -1;
    } else if (inet_result < 0) {
        Logger::getInstance().logError("HttpServer: inet_pton failed for address " + bind_addr + 
                                     ": " + std::string(strerror(errno)));
        close(socket_fd);
        return -1;
    }

    if (bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to bind to " + bind_addr + ":" + 
                                     std::to_string(port) + ": " + std::string(strerror(errno)));
        close(socket_fd);
        return -1;
    }

    // Start listening
    if (::listen(socket_fd, 128) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to listen on socket: " + std::string(strerror(errno)));
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

// HttpConnectionJob implementation
// Pool-allocated only - managed via cleanup callbacks (no shared_ptr needed)

HttpConnectionJob* HttpConnectionJob::createFromPool(
    int client_fd, 
    Server& job_server,
    const HttpRouter& router,
    HttpServer* http_server,
    size_t max_request_size) {
    
    HttpConnectionJob* job = lfmemorypool::lockfree_pool_alloc_fast<HttpConnectionJob>(
        client_fd, job_server, router, http_server, max_request_size);
    if (!job) {
        return nullptr; // Pool exhausted
    }
    
    // Increment generation counter to detect ABA problem
    job->incrementGeneration();
    
    // Job starts in AVAILABLE state (set in constructor)
    // No heap allocations - truly lock-free!
    return job;
}

HttpConnectionJob::HttpConnectionJob(int client_fd, Server& job_server, const HttpRouter& router, 
                                   HttpServer* http_server, size_t max_request_size)
    : client_fd_(client_fd)
    , job_server_(job_server)
    , router_(router)
    , http_server_(http_server)
    , request_buffer_()
    , max_request_size_(max_request_size)
    , reading_active_(false)
    , keep_alive_(true)  // Default to keep-alive for HTTP/1.1
    , state_()  // JobStateMachine initializes to AVAILABLE
    , generation_(0)  // Initialize generation counter
{
    
    request_buffer_.reserve(8192);
    Logger::getInstance().logMessage("HttpConnectionJob: Created for fd=" + std::to_string(client_fd_));
}

void HttpConnectionJob::prepareSqe(struct io_uring_sqe* sqe) {
    // This job doesn't submit its own operations - it manages ReadJob/WriteJob instead
    (void)sqe;
}

void HttpConnectionJob::start() {
    // Job starts in AVAILABLE state, ready for async operations
    // State machine will handle lifetime via atomic CAS guards
    startReading();
}

std::optional<IoJob::CleanupCallback> HttpConnectionJob::handleCompletion(Server& server, struct io_uring_cqe* cqe) {
    // This job doesn't handle completions directly - ReadJob/WriteJob do
    // Jobs manage their own lifecycle with pools now
    (void)server;
    (void)cqe;
    return cleanupHttpConnectionJob;
}

void HttpConnectionJob::startReading() {
    if (reading_active_ || client_fd_ < 0) {
        return;
    }

    reading_active_ = true;
    
    // Pass raw pointer to handler - worker will use tryAcquire/tryRelease for safety
    HttpConnectionRecvHandler handler{this};
    
    // Check if we have affinity workers for zero-copy processing
    auto worker_pool = job_server_.getAffinityWorkerPool();
    
    if (!worker_pool) {
        Logger::getInstance().logError("HttpConnectionJob: AffinityWorkerPool required for MultishotRecvJob");
        reading_active_ = false;
        return;
    }
    
    // Zero-copy path: use token-based MultishotRecvJob for worker thread processing
    // Template policy pattern - type-safe, fully inlineable callbacks
    auto* read_job = MultishotRecvJob<HttpConnectionRecvHandler>::createFromPool(
        client_fd_,
        handler,              // Handler instance (no void* casting!)
        worker_pool.get()     // void* worker_pool_ptr for zero-copy dispatch
    );
    
    if (!read_job) {
        Logger::getInstance().logError("HttpConnectionJob: Failed to allocate MultishotRecvJob from pool");
        reading_active_ = false;
        return;
    }
    
    struct io_uring_sqe* sqe = job_server_.registerJob(read_job);
    if (sqe) {
        // Configure the SQE for the ReadJob
        read_job->prepareSqe(sqe);
        
        // Set buffer group for buffer selection
        if (auto buffer_coordinator = job_server_.getBufferRingCoordinator()) {
            sqe->buf_group = buffer_coordinator->getBufferGroupId();
        }
        
        // Submit the job
        job_server_.submit();
    } else {
        Logger::getInstance().logError("HttpConnectionJob: Failed to register ReadJob");
        reading_active_ = false;
    }
}

void HttpConnectionJob::handleDataReceived(const char* data, ssize_t len) {
    if (len == 0) {
        // EOF - client disconnected
        Logger::getInstance().logMessage("HttpConnectionJob: Client disconnected fd=" + std::to_string(client_fd_));
        closeConnection();
        return;
    }

    if (len < 0) {
        Logger::getInstance().logError("HttpConnectionJob: Read error fd=" + std::to_string(client_fd_) + 
                                     ", error=" + std::to_string(-len));
        closeConnection();
        return;
    }

    // Check buffer size limit
    if (request_buffer_.size() + len > max_request_size_) {
        Logger::getInstance().logError("HttpConnectionJob: Request too large, closing connection");
        closeConnection();
        return;
    }

    // Append data to request buffer
    request_buffer_.append(data, len);
    
    // Process any complete HTTP requests
    processHttpRequests();
}

void HttpConnectionJob::handleDataReceivedOnWorker(const char* data, ssize_t len) {
    // Try to acquire job for processing (AVAILABLE -> WORKING)
    if (!tryAcquire()) {
        // Job was discarded, don't process
        Logger::getInstance().logMessage("HttpConnectionJob: Skipping worker processing, job discarded fd=" + std::to_string(client_fd_));
        return;
    }
    
    // RAII-style release using scope guard
    struct ReleaseGuard {
        HttpConnectionJob* job;
        ~ReleaseGuard() {
            if (!job->tryRelease()) {
                // Job was discarded while we were processing
                // We're the last one - cleanup now
                Logger::getInstance().logMessage("HttpConnectionJob: Worker detected discard, cleaning up fd=" + std::to_string(job->client_fd_));
                lfmemorypool::lockfree_pool_free_fast<HttpConnectionJob>(job);
            }
        }
    } guard{this};
    
    if (len == 0) {
        // EOF - client disconnected, cleanup handled by connection state machine
        Logger::getInstance().logMessage("HttpConnectionJob: Client disconnected on worker fd=" + std::to_string(client_fd_));
        return;
    }

    if (len < 0) {
        Logger::getInstance().logError("HttpConnectionJob: Read error on worker fd=" + std::to_string(client_fd_) + 
                                     ", error=" + std::to_string(-len));
        return;
    }

    // Check buffer size limit
    if (request_buffer_.size() + len > max_request_size_) {
        Logger::getInstance().logError("HttpConnectionJob: Request too large on worker, closing connection");
        return;
    }

    // Append data to request buffer (thread-safe since each connection has affinity to one worker)
    request_buffer_.append(data, len);
    
    // Process HTTP requests on worker thread
    processHttpRequestsOnWorker();
}

void HttpConnectionJob::handleReadError(int error) {
    Logger::getInstance().logError("HttpConnectionJob: Read error fd=" + std::to_string(client_fd_) + 
                                 ", error=" + std::to_string(error));
    closeConnection();
}

void HttpConnectionJob::processHttpRequests() {
    while (!request_buffer_.empty()) {
        HttpRequest request;
        size_t bytes_consumed = 0;
        
        auto result = HttpParser::parse_request(
            request_buffer_, 
            request, 
            bytes_consumed
        );

        if (result == HttpParser::ParseResult::Success) {
            // Complete request parsed successfully
            request_buffer_.erase(0, bytes_consumed);
            handleHttpRequest(request);
        } else if (result == HttpParser::ParseResult::Incomplete) {
            // Need more data - wait for next read
            return;
        } else {
            // BadRequest - malformed input, close connection
            Logger::getInstance().logError("HttpConnectionJob: Malformed HTTP request, closing connection fd=" + 
                                         std::to_string(client_fd_));
            closeConnection();
            return;
        }
    }
}

void HttpConnectionJob::processHttpRequestsOnWorker() {
    while (!request_buffer_.empty()) {
        HttpRequest request;
        size_t bytes_consumed = 0;
        
        auto result = HttpParser::parse_request(
            request_buffer_, 
            request, 
            bytes_consumed
        );

        if (result == HttpParser::ParseResult::Success) {
            // Complete request parsed on worker thread
            request_buffer_.erase(0, bytes_consumed);
            
            Logger::getInstance().logMessage("HttpConnectionJob: Processing on worker " + request.method + " " + request.path);
            
            // Determine if we should keep connection alive after this response
            keep_alive_ = shouldKeepAlive(request);
            
            // Process HTTP request directly on worker thread
            HttpResponse response;
            router_.dispatch(request, response);
            
            Logger::getInstance().logMessage("HttpConnectionJob: Response generated, status=" + std::to_string(response.status_code));
            
            // Post response back to main thread for io_uring operations
            postResponseFromWorker(response);
        } else if (result == HttpParser::ParseResult::Incomplete) {
            // Need more data - wait for next read
            return;
        } else {
            // BadRequest - malformed input
            Logger::getInstance().logError("HttpConnectionJob: Malformed HTTP request on worker, closing connection fd=" + 
                                         std::to_string(client_fd_));
            
            // Fatal error - force connection close and clear poisoned buffer
            keep_alive_ = false;
            request_buffer_.clear();  // Drop invalid bytes to prevent reprocessing
            
            // Post 400 Bad Request response back to main thread
            HttpResponse error_response;
            error_response.status_code = 400;
            error_response.status_text = "Bad Request";
            error_response.setHeader("content-type", "text/plain");
            error_response.setHeader("connection", "close");
            error_response.body = "Bad Request";
            
            postResponseFromWorker(error_response);
            return;
        }
    }
}

void HttpConnectionJob::handleHttpRequest(const HttpRequest& request) {
    Logger::getInstance().logMessage("HttpConnectionJob: Processing " + request.method + " " + request.path);
    
    // Determine if we should keep connection alive after this response
    keep_alive_ = shouldKeepAlive(request);
    
    // This method is called when NOT using affinity workers (fallback)
    HttpResponse response;
    router_.dispatch(request, response);
    
    Logger::getInstance().logMessage("HttpConnectionJob: Response generated, status=" + std::to_string(response.status_code));
    
    sendResponse(response);
}

void HttpConnectionJob::postResponseFromWorker(const HttpResponse& response) {
    if (!http_server_) {
        Logger::getInstance().logError("HttpConnectionJob: No HttpServer reference, cannot post worker response");
        return;
    }
    
    Logger::getInstance().logMessage("HttpConnectionJob: Posting response from worker thread, status=" + 
                                   std::to_string(response.status_code));
    
    // Create WorkerResponse with raw pointer - io_uring thread will use tryAcquire/tryRelease
    WorkerResponse wr(client_fd_, response, keep_alive_, this);
    
    if (!http_server_->getResponseQueue().try_push(std::move(wr))) {
        Logger::getInstance().logError("HttpConnectionJob: Worker response queue full! Dropping response for fd=" + 
                                     std::to_string(client_fd_));
        return;
    }
    
    // Signal the io_uring thread to process responses
    http_server_->signalWorkerResponse();
    
    Logger::getInstance().logMessage("HttpConnectionJob: Worker response posted successfully");
}

void HttpConnectionJob::sendResponse(const HttpResponse& response) {
    Logger::getInstance().logMessage("HttpConnectionJob: Sending response, status=" + std::to_string(response.status_code));
    
    // Check if this is a file serving response (internal flag only, not sent to client)
    if (!response.file_path.empty()) {
        // This is a file serving request - use HTTPFileJob for zero-copy transfer
        Logger::getInstance().logMessage("HttpConnectionJob: Using HTTPFileJob for file: " + response.file_path);
        
        // CRITICAL: HTTPFileJob needs to keep HttpConnectionJob alive during file transfer
        // to prevent ABA problem (memory reuse while callbacks are pending).
        // The caller (worker thread) MUST keep this job acquired (WORKING state) and
        // NOT release until HTTPFileJob callbacks complete. The callbacks will release.
        HttpConnectionJob* conn_ptr = this;
        uint64_t expected_generation = generation_.load(std::memory_order_relaxed);
        int expected_fd = client_fd_;  // Capture fd for sanity check
        
        HttpResponse response_copy = response;
        // Use per-connection keep_alive_ flag (not response header)
        // This is set based on HTTP/1.1 Connection header from the REQUEST
        if (!keep_alive_) {
            response_copy.setHeader("connection", "close");
        }

        std::string file_path = response.file_path;
        bool keep_alive_copy = keep_alive_;

        auto http_file_job = HTTPFileJob::createFromPool(
            client_fd_,
            file_path,
            std::move(response_copy), // Pass the response for any custom headers
            [conn_ptr, keep_alive_copy, expected_generation, expected_fd](int fd, size_t bytes_sent) {
                // Sanity check: fd should match
                if (fd != expected_fd) {
                    Logger::getInstance().logError("HttpConnectionJob: FD mismatch in callback, expected=" + 
                                                 std::to_string(expected_fd) + ", got=" + std::to_string(fd));
                    return;
                }
                
                // Check generation to detect ABA problem (memory reuse)
                // NOTE: This is still potentially unsafe if memory was reused, but combined
                // with FD check it's much more likely to catch issues
                uint64_t current_gen = conn_ptr->generation_.load(std::memory_order_relaxed);
                if (current_gen != expected_generation) {
                    Logger::getInstance().logMessage("HttpConnectionJob: Detected stale callback (ABA), expected_gen=" + 
                                                   std::to_string(expected_generation) + ", current_gen=" + std::to_string(current_gen));
                    return;
                }
                
                // Job is already acquired by caller (worker thread)
                // We just use it and release when done
                struct ReleaseGuard {
                    HttpConnectionJob* job_;
                    ~ReleaseGuard() {
                        if (!job_->tryRelease()) {
                            lfmemorypool::lockfree_pool_free_fast<HttpConnectionJob>(job_);
                        }
                    }
                } guard{conn_ptr};
                
                Logger::getInstance().logMessage("HttpConnectionJob: File transfer complete fd=" + 
                                               std::to_string(fd) + ", bytes=" + std::to_string(bytes_sent));
                if (keep_alive_copy) {
                    conn_ptr->startReading();
                } else {
                    conn_ptr->closeConnection();
                }
            },
            [conn_ptr, expected_generation, expected_fd](int fd, int error) {
                // Sanity check: fd should match
                if (fd != expected_fd) {
                    Logger::getInstance().logError("HttpConnectionJob: FD mismatch in error callback, expected=" + 
                                                 std::to_string(expected_fd) + ", got=" + std::to_string(fd));
                    return;
                }
                
                // Check generation to detect ABA problem (memory reuse)
                uint64_t current_gen = conn_ptr->generation_.load(std::memory_order_relaxed);
                if (current_gen != expected_generation) {
                    Logger::getInstance().logMessage("HttpConnectionJob: Detected stale error callback (ABA), expected_gen=" + 
                                                   std::to_string(expected_generation) + ", current_gen=" + std::to_string(current_gen));
                    return;
                }
                
                // Job is already acquired by caller (worker thread)
                // We just use it and release when done
                struct ReleaseGuard {
                    HttpConnectionJob* job_;
                    ~ReleaseGuard() {
                        if (!job_->tryRelease()) {
                            lfmemorypool::lockfree_pool_free_fast<HttpConnectionJob>(job_);
                        }
                    }
                } guard{conn_ptr};
                
                Logger::getInstance().logError("HttpConnectionJob: File transfer error fd=" + 
                                             std::to_string(fd) + ", error=" + std::to_string(error));
                conn_ptr->closeConnection();
            }
        );
        
        if (http_file_job) {
            Logger::getInstance().logMessage("HttpConnectionJob: HTTPFileJob allocated, starting...");
            // HTTPFileJob is a composite job - start it directly (it creates child jobs for io_uring)
            http_file_job->start(job_server_);
            Logger::getInstance().logMessage("HttpConnectionJob: HTTPFileJob started");
        } else {
            Logger::getInstance().logError("HttpConnectionJob: Failed to allocate HTTPFileJob from pool");
            closeConnection();
        }
        return;
    }
    
    // Regular response - use WriteJob
    std::ostringstream oss;
    oss << "HTTP/1.1 " << response.status_code << " " << response.status_text << "\r\n";

    // Add headers
    bool hasCL = response.headers.find("content-length") != response.headers.end();
    for (const auto &[k, v] : response.headers) {
        oss << k << ": " << v << "\r\n";
    }
    if (!hasCL) {
        oss << "Content-Length: " << response.body.size() << "\r\n";
    }
    oss << "\r\n" << response.body;
    
    std::string response_str = oss.str();
    
    // Capture raw pointer - callbacks will use tryAcquire/tryRelease
    HttpConnectionJob* conn_ptr = this;
    bool keep_alive_copy = keep_alive_;
    
    // Create owned data for WriteJob
    auto response_data = std::make_unique<char[]>(response_str.size());
    std::memcpy(response_data.get(), response_str.data(), response_str.size());

    auto write_job = WriteJob::createFromPoolWithOwnedData(
        client_fd_,
        std::move(response_data),
        response_str.size(),
        [conn_ptr, keep_alive_copy](int fd, size_t bytes_written) {
            if (!conn_ptr->tryAcquire()) {
                Logger::getInstance().logMessage("HttpConnectionJob: Connection closed during write completion");
                return;
            }
            struct ReleaseGuard {
                HttpConnectionJob* job_;
                ~ReleaseGuard() {
                    if (!job_->tryRelease()) {
                        lfmemorypool::lockfree_pool_free_fast<HttpConnectionJob>(job_);
                    }
                }
            } guard{conn_ptr};
            
            Logger::getInstance().logMessage("HttpConnectionJob: Response sent fd=" + std::to_string(fd) + 
                                           ", bytes=" + std::to_string(bytes_written));
            // Check if we should keep the connection alive for more requests
            if (keep_alive_copy) {
                conn_ptr->continueReading();
            } else {
                conn_ptr->closeConnection();
            }
        },
        [conn_ptr](int fd, int error) {
            if (!conn_ptr->tryAcquire()) {
                Logger::getInstance().logMessage("HttpConnectionJob: Connection closed during write error");
                return;
            }
            struct ReleaseGuard {
                HttpConnectionJob* job_;
                ~ReleaseGuard() {
                    if (!job_->tryRelease()) {
                        lfmemorypool::lockfree_pool_free_fast<HttpConnectionJob>(job_);
                    }
                }
            } guard{conn_ptr};
            
            Logger::getInstance().logError("HttpConnectionJob: Write error fd=" + std::to_string(fd) + 
                                         ", error=" + std::to_string(error));
            conn_ptr->closeConnection();
        }
    );

    if (write_job) {
        Logger::getInstance().logMessage("HttpConnectionJob: WriteJob allocated, registering...");
        // Register and prepare the write job
        struct io_uring_sqe* sqe = job_server_.registerJob(write_job);
        if (sqe) {
            write_job->prepareSqe(sqe);
            job_server_.submit();
            Logger::getInstance().logMessage("HttpConnectionJob: WriteJob registered and submitted");
        } else {
            Logger::getInstance().logError("HttpConnectionJob: Failed to get SQE for WriteJob");
            WriteJob::freePoolAllocated(write_job);
            closeConnection();
        }
    } else {
        Logger::getInstance().logError("HttpConnectionJob: Failed to allocate WriteJob from pool");
        closeConnection();
    }
}

void HttpConnectionJob::closeConnection() {
    if (client_fd_ >= 0) {
        Logger::getInstance().logMessage("[ACCESS] DISCONNECT fd=" + std::to_string(client_fd_));
        close(client_fd_);
        client_fd_ = -1;
    }
    reading_active_ = false;
    
    // Try to discard this job
    if (tryDiscard()) {
        // We successfully transitioned AVAILABLE -> DISCARDED
        // No worker is processing, safe to return to pool immediately
        lfmemorypool::lockfree_pool_free_fast<HttpConnectionJob>(this);
    }
    // else: Worker is processing (WORKING state)
    // We marked it as DISCARDED, worker will cleanup when done
}

bool HttpConnectionJob::shouldKeepAlive(const HttpRequest& request) const {
    // Check Connection header - HTTP/1.1 defaults to keep-alive
    auto conn_header = request.headers.find("connection");
    if (conn_header != request.headers.end()) {
        std::string conn_value = conn_header->second;
        // Case-insensitive comparison
        std::transform(conn_value.begin(), conn_value.end(), conn_value.begin(), ::tolower);
        if (conn_value == "close") {
            return false;
        }
        if (conn_value == "keep-alive") {
            return true;
        }
    }
    
    // HTTP/1.1 defaults to keep-alive, HTTP/1.0 defaults to close
    return request.version.find("1.1") != std::string::npos;
}

void HttpConnectionJob::continueReading() {
    // Resume reading for next request on persistent connection
    Logger::getInstance().logMessage("HttpConnectionJob: Continuing reading for keep-alive connection fd=" + 
                                   std::to_string(client_fd_));
    reading_active_ = false;
    startReading();
}

// ============================================================================
// HttpServer Worker Response Handling
// ============================================================================

void HttpServer::setupWorkerEventFd() {
    try {
        // Create eventfd for worker notifications
        worker_event_fd_ = std::make_unique<EventFd>(false, true);  // Not semaphore, nonblocking
        
        Logger::getInstance().logMessage("HttpServer: Worker eventfd created, fd=" + 
                                       std::to_string(worker_event_fd_->fd()));
        
        // Create a pool-allocated EventFdMonitorJob to monitor the eventfd
        eventfd_monitor_job_ = EventFdMonitorJob::createFromPool(
            worker_event_fd_->fd(),
            [this](int fd, uint64_t counter) {
                // EventFd was signaled - process all pending worker responses
                Logger::getInstance().logMessage("HttpServer: EventFD signaled, counter=" + std::to_string(counter));
                processWorkerResponses();
                
                // Resubmit the read to continue monitoring
                if (eventfd_monitor_job_ && worker_event_fd_) {
                    struct io_uring_sqe* sqe = job_server_.registerJob(eventfd_monitor_job_);
                    if (sqe) {
                        eventfd_monitor_job_->prepareSqe(sqe);
                        job_server_.submit();
                    }
                }
            },
            [this](int fd, int error) {
                // Error callback - eventfd closed or error occurred
                if (error != 0) {
                    Logger::getInstance().logError("HttpServer: Worker eventfd error: " + std::to_string(error));
                }
                // Free the job before clearing pointer to prevent pool leak
                if (eventfd_monitor_job_) {
                    EventFdMonitorJob::freePoolAllocated(eventfd_monitor_job_);
                    eventfd_monitor_job_ = nullptr;
                }
            }
        );
        
        if (!eventfd_monitor_job_) {
            Logger::getInstance().logError("HttpServer: Failed to allocate EventFdMonitorJob from pool");
            return;
        }
        
        // Start monitoring (will be triggered when workers signal)
        struct io_uring_sqe* sqe = job_server_.registerJob(eventfd_monitor_job_);
        if (sqe) {
            // EventFdMonitorJob handles setup (single-shot read of 8-byte counter)
            eventfd_monitor_job_->prepareSqe(sqe);
            job_server_.submit();
            
            Logger::getInstance().logMessage("HttpServer: Worker eventfd monitoring started");
        } else {
            Logger::getInstance().logError("HttpServer: Failed to register eventfd monitor job");
        }
    } catch (const std::exception& e) {
        Logger::getInstance().logError("HttpServer: Failed to setup worker eventfd: " + std::string(e.what()));
    }
}

void HttpServer::signalWorkerResponse() {
    if (worker_event_fd_) {
        try {
            worker_event_fd_->signal();
        } catch (const std::exception& e) {
            Logger::getInstance().logError("HttpServer: Failed to signal eventfd: " + std::string(e.what()));
        }
    }
}

void HttpServer::processWorkerResponses() {
    int processed = 0;
    
    // Process all pending responses from workers
    while (auto maybe_response = response_queue_.try_pop()) {
        WorkerResponse& wr = *maybe_response;
        
        HttpConnectionJob* job = wr.connection_job;
        if (!job) {
            Logger::getInstance().logMessage("HttpServer: Null connection job for fd=" + 
                                           std::to_string(wr.client_fd));
            continue;
        }
        
        // Acquire the job to ensure it's still alive
        if (!job->tryAcquire()) {
            Logger::getInstance().logMessage("HttpServer: Job already discarded for fd=" + 
                                           std::to_string(wr.client_fd));
            continue;
        }
        
        // IMPORTANT: For file responses, sendResponse creates HTTPFileJob which will
        // take ownership of the acquisition. The HTTPFileJob callbacks will release.
        // For non-file responses, sendResponse creates WriteJob with callbacks that
        // will acquire/release independently. So we release here for non-file responses.
        
        bool is_file_response = !wr.response.file_path.empty();
        
        // Send the response
        job->sendResponse(wr.response);
        
        // Release ONLY if not a file response
        // File responses: HTTPFileJob owns the acquisition and will release in callbacks
        // Non-file responses: WriteJob has its own acquire/release in callbacks
        if (!is_file_response) {
            if (!job->tryRelease()) {
                // Job was discarded while we were processing
                lfmemorypool::lockfree_pool_free_fast<HttpConnectionJob>(job);
            }
        }
        // else: HTTPFileJob callbacks will release
        
        processed++;
    }
    
    if (processed > 0) {
        Logger::getInstance().logMessage("HttpServer: Processed " + std::to_string(processed) + 
                                       " worker responses");
    }
    
    // Consume the eventfd counter
    if (worker_event_fd_) {
        worker_event_fd_->try_consume();
    }
}

void HttpServer::setAffinityWorkerPool(std::shared_ptr<AffinityWorkerPool> pool) {
    worker_pool_ = std::move(pool);
    job_server_.setAffinityWorkerPool(worker_pool_);
}


} // namespace caduvelox
