#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/jobs/KTLSJob.hpp"
#include "caduvelox/jobs/KTLSContextHelper.hpp"
#include "caduvelox/jobs/MultishotRecvJob.hpp"
#include "caduvelox/jobs/IdleTimeoutJob.hpp"
#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include "caduvelox/util/ProvidedBufferToken.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/Config.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <algorithm>

namespace caduvelox {

SingleRingHttpServer::SingleRingHttpServer(Server& job_server)
    : job_server_(job_server)
    , router_()
    , server_fd_(-1)
    , running_(false)
    , ktls_enabled_(false)
    , ssl_ctx_(nullptr)
    , owns_ssl_ctx_(true)  // By default, owns the SSL context
{
    // HTTP processing happens inline on io_uring thread (no thread ping-pong)
}

SingleRingHttpServer::~SingleRingHttpServer() {
    stop();
    
    // Only free SSL context if we own it
    if (ssl_ctx_ && owns_ssl_ctx_) {
        KTLSContextHelper::freeContext(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
}

void SingleRingHttpServer::addRoute(const std::string& method, const std::string& pathRegex, HttpHandler handler) {
    router_.addRoute(method, pathRegex, std::move(handler));
}

void SingleRingHttpServer::addRouteWithCaptures(const std::string& method, const std::string& pathRegex, HttpHandlerWithCaptures handler) {
    router_.addRouteWithCaptures(method, pathRegex, std::move(handler));
}

bool SingleRingHttpServer::listen(int port, const std::string& bind_addr) {
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

    // Install the ring-local shutdown sweep so idle keep-alive connections are
    // cancelled promptly when the server enters Stopping/Aborting state.
    job_server_.setShutdownSweepFn([this](Server& server) {
        PoolManager::sweepLive<HttpMultishotRecvJob>([&](HttpMultishotRecvJob& job) {
            job.requestShutdownCancel(server);
        });
        PoolManager::sweepLive<IdleTimeoutJob>([&](IdleTimeoutJob& job) {
            job.requestShutdownCancel(server);
        });
        if (accept_job_) {
            accept_job_->requestShutdownCancel(server);
            accept_job_ = nullptr;
        }
    });

    // Start accepting connections
    startAccepting();

    return true;
}

bool SingleRingHttpServer::listenKTLS(int port, const std::string& cert_path, const std::string& key_path, 
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

    job_server_.setShutdownSweepFn([this](Server& server) {
        PoolManager::sweepLive<HttpMultishotRecvJob>([&](HttpMultishotRecvJob& job) {
            job.requestShutdownCancel(server);
        });
        PoolManager::sweepLive<IdleTimeoutJob>([&](IdleTimeoutJob& job) {
            job.requestShutdownCancel(server);
        });
        if (accept_job_) {
            accept_job_->requestShutdownCancel(server);
            accept_job_ = nullptr;
        }
    });

    // Start accepting connections
    startAccepting();

    return true;
}

void SingleRingHttpServer::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    
    // Close server socket to stop accepting new connections
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }

    Logger::getInstance().logMessage("HttpServer: Server stopped");
}

void SingleRingHttpServer::startAccepting() {
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
            if (running_) {
                startAccepting();
            }
        }
    );
    accept_job_ = accept_job;

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
        AcceptJob::freePoolAllocated(accept_job);
    }
}

void SingleRingHttpServer::handleNewConnection(int client_fd, const sockaddr* addr, socklen_t addrlen) {
    // Log connection with [ACCESS] prefix for easy filtering
    std::string access_log = "[ACCESS] CONNECT fd=" + std::to_string(client_fd);
    
    if (addr) {
        // Only AF_INET is reachable here: every listening socket in this codebase
        // is created with socket(AF_INET, ...), so getpeername() on an accepted
        // connection can never return AF_INET6.
        char ip_str[INET_ADDRSTRLEN];
        int port = 0;

        if (addr->sa_family == AF_INET) {
            auto* addr4 = (const sockaddr_in*)addr;
            inet_ntop(AF_INET, &addr4->sin_addr, ip_str, sizeof(ip_str));
            port = ntohs(addr4->sin_port);
        } else {
            strcpy(ip_str, "unknown");
        }
        
        access_log += " from " + std::string(ip_str) + ":" + std::to_string(port);
    }
    
    Logger::getInstance().logMessage(access_log);
    
    if (ktls_enabled_) {
        // Start KTLS handshake for this connection
        auto ktls_job = PoolManager::allocate<KTLSJob>(
            client_fd,
            ssl_ctx_,
            ktls_handshake_timeout_ms_,
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
            PoolManager::deallocate(ktls_job);
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

void SingleRingHttpServer::createConnectionHandler(int client_fd) {
    auto connection_job = PoolManager::allocate<HttpConnectionJob>(client_fd, job_server_, router_, 1024 * 1024, idle_timeout_ms_);
    
    if (!connection_job) {
        Logger::getInstance().logError("HttpServer: Failed to allocate HttpConnectionJob from pool");
        close(client_fd);
        return;
    }
    
    // Start the connection job - it will manage itself via cleanup callbacks
    connection_job->start();
    
    // The HttpConnectionJob will stay alive until closeConnection() is called,
    // at which point the cleanup callback returns it to the pool
}

void SingleRingHttpServer::handleKTLSReady(int client_fd, SSL* ssl) {
    Logger::getInstance().logMessage("HttpServer: KTLS ready for fd=" + std::to_string(client_fd));
    
    // KTLS handshake completed successfully!
    // At this point, the connection is encrypted and kernel TLS is enabled.
    // We can now treat it as a regular HTTP connection since the kernel
    // will handle TLS encryption/decryption transparently.
    
    // Note: The SSL* object is not needed for further operations since
    // kTLS allows us to use regular TCP read/write operations.
    
    createConnectionHandler(client_fd);
}

void SingleRingHttpServer::handleKTLSError(int client_fd, int error) {
    Logger::getInstance().logError("HttpServer: KTLS handshake failed for fd=" + 
                                  std::to_string(client_fd) + ", error=" + std::to_string(error));
    close(client_fd);
}

int SingleRingHttpServer::createServerSocket(int port, const std::string& bind_addr) {
    // Additional port validation (should have been caught earlier, but defensive programming)
    if (port < 0 || port > 65535) {
        Logger::getInstance().logError("HttpServer: Invalid port " + std::to_string(port) + 
                                     " in createServerSocket (must be between 0 and 65535)");
        return -1;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        Logger::getInstance().logError("HttpServer: Failed to create socket: " + std::string(strerror(errno)));
        return -1;
    }

    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
        close(socket_fd);
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to set SO_REUSEPORT: " + std::string(strerror(errno)));
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

    if (::listen(socket_fd, SOMAXCONN) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to listen on socket: " + std::string(strerror(errno)));
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

// HttpConnectionJob implementation
// Pool-allocated only - managed via cleanup callbacks (no shared_ptr needed)

HttpConnectionJob::HttpConnectionJob(int client_fd, Server& job_server, const HttpRouter& router,
                                   size_t max_request_size, unsigned idle_timeout_ms)
    : client_fd_(client_fd)
    , job_server_(job_server)
    , router_(router)
    , request_buffer_()
    , max_request_size_(max_request_size)
    , idle_timeout_ms_(idle_timeout_ms)
    , reading_active_(false)
    , keep_alive_(true)  // Default to keep-alive for HTTP/1.1
    , pending_writes_(0)
    , close_pending_(false)
    , active_read_job_(nullptr)
    , read_cancel_pending_(false)
    , deferred_close_fd_(-1)
    , active_idle_timeout_job_(nullptr)
    , idle_cancel_pending_(false)
{
    
    request_buffer_.reserve(8192);
    Logger::getInstance().logMessage("HttpConnectionJob: Created for fd=" + std::to_string(client_fd_));
    
    // Don't start reading here - must be called after object is in shared_ptr
}

void HttpConnectionJob::start() {
    startReading();
}

void HttpConnectionJob::startReading() {
    if (reading_active_ || client_fd_ < 0) {
        return;
    }

    reading_active_ = true;

    // Create handler instance (encapsulates context + callbacks)
    HttpConnectionRecvHandler handler{this};

    // Zero-copy path: use token-based MultishotRecvJob for inline processing on io_uring thread
    // Template policy pattern - type-safe, fully inlineable callbacks
    auto* read_job = PoolManager::allocate<MultishotRecvJob<HttpConnectionRecvHandler>>(
        client_fd_,
        handler,
        *job_server_.getBufferRingCoordinator()
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

        // Track the active recv so closeConnection() and the shutdown sweep can cancel it.
        active_read_job_ = read_job;

        // Submit the job
        job_server_.submit();

        // Arm the idle-wait timeout: closes the connection if no data arrives
        // before the next request within idle_timeout_ms_.
        armIdleTimeout();
    } else {
        Logger::getInstance().logError("HttpConnectionJob: Failed to register ReadJob");
        // CRITICAL: Free the pool-allocated job to prevent leak
        PoolManager::deallocate(read_job);
        reading_active_ = false;
    }
}

void HttpConnectionJob::handleDataReceived(const char* data, ssize_t len) {
    // If closeConnection() was already called and set client_fd_ = -1, discard
    // any remaining recv completions that arrive before the cancel fires.
    if (client_fd_ < 0) {
        return;
    }

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

    // Real data arrived — the idle wait that armIdleTimeout() started is over.
    if (active_idle_timeout_job_ != nullptr && !idle_cancel_pending_) {
        if (submitIdleTimeoutCancel()) {
            idle_cancel_pending_ = true;
        }
    }

    // Append data to request buffer
    request_buffer_.append(data, len);
    
    // Process any complete HTTP requests
    processHttpRequests();
}

void HttpConnectionJob::handleReadError(int error) {
    active_read_job_ = nullptr;

    // If closeConnection() already ran its final path (both fds cleared), this is
    // a stale completion from a recv that outlived the connection close (e.g. after
    // a failed cancel submission). Nothing left to do.
    if (client_fd_ < 0 && deferred_close_fd_ < 0) {
        return;
    }

    if (error == -ECANCELED) {
        Logger::getInstance().logMessage("HttpConnectionJob: Recv cancelled (shutdown) fd=" +
                                         std::to_string(client_fd_));
    } else {
        Logger::getInstance().logError("HttpConnectionJob: Read error fd=" + std::to_string(client_fd_) +
                                       ", error=" + std::to_string(error));
    }
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

void HttpConnectionJob::sendResponse(const HttpResponse& response) {
    Logger::getInstance().logMessage("HttpConnectionJob: Sending response, status=" + std::to_string(response.status_code));
    
    // Check if this is a file serving response (internal flag only, not sent to client)
    if (!response.file_path.empty()) {
        // This is a file serving request - use HTTPFileJob for zero-copy transfer
        Logger::getInstance().logMessage("HttpConnectionJob: Using HTTPFileJob for file: " + response.file_path);
        
        HttpResponse response_copy = response;
        // Use per-connection keep_alive_ flag (not response header)
        // This is set based on HTTP/1.1 Connection header from the REQUEST
        if (!keep_alive_) {
            response_copy.setHeader("connection", "close");
        }

        std::string file_path = response.file_path;

        // Increment pending writes for the file transfer
        pending_writes_++;

        auto http_file_job = HTTPFileJob::createFromPool(
            client_fd_,
            file_path,
            std::move(response_copy), // Pass the response for any custom headers
            [this, keep_alive = keep_alive_](int fd, size_t bytes_sent) {
                Logger::getInstance().logMessage("HttpConnectionJob: File transfer complete fd=" + 
                                               std::to_string(fd) + ", bytes=" + std::to_string(bytes_sent));
                
                // Decrement pending writes
                pending_writes_--;
                
                // If close was pending, execute it now
                if (close_pending_ && pending_writes_ == 0) {
                    closeConnection();
                    return;
                }
                
                if (keep_alive && !job_server_.isStopping()) {
                    continueReading();
                } else {
                    closeConnection();
                }
            },
            [this](int fd, int error) {
                Logger::getInstance().logError("HttpConnectionJob: File transfer error fd=" + 
                                             std::to_string(fd) + ", error=" + std::to_string(error));
                
                // Decrement pending writes
                pending_writes_--;
                
                closeConnection();
            }
        );
        
        if (http_file_job) {
            Logger::getInstance().logMessage("HttpConnectionJob: HTTPFileJob allocated, starting...");
            // HTTPFileJob is a composite job - start it directly (it creates child jobs for io_uring)
            http_file_job->start(job_server_);
            Logger::getInstance().logMessage("HttpConnectionJob: HTTPFileJob started");
        } else {
            Logger::getInstance().logError("HttpConnectionJob: Failed to allocate HTTPFileJob from pool");
            pending_writes_--;  // Decrement since allocation failed
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
    
    // Create owned data for WriteJob
    auto response_data = std::make_unique<char[]>(response_str.size());
    std::memcpy(response_data.get(), response_str.data(), response_str.size());

    // Increment pending writes BEFORE creating the WriteJob
    pending_writes_++;
    
    auto write_job = WriteJob::createFromPoolWithOwnedData(
        client_fd_,
        std::move(response_data),
        response_str.size(),
        [this](int fd, size_t bytes_written) {
            Logger::getInstance().logMessage("HttpConnectionJob: Response sent fd=" + std::to_string(fd) + 
                                           ", bytes=" + std::to_string(bytes_written));
            
            // Decrement pending writes
            pending_writes_--;
            
            // If close was pending, execute it now
            if (close_pending_ && pending_writes_ == 0) {
                closeConnection();
                return;
            }
            
            // Check if we should keep the connection alive for more requests
            if (keep_alive_ && !job_server_.isStopping()) {
                continueReading();
            } else {
                closeConnection();
            }
        },
        [this](int fd, int error) {
            Logger::getInstance().logError("HttpConnectionJob: Write error fd=" + std::to_string(fd) + 
                                         ", error=" + std::to_string(error));
            
            // Decrement pending writes
            pending_writes_--;
            
            closeConnection();
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
            pending_writes_--;  // Decrement since job failed to submit
            closeConnection();
        }
    } else {
        Logger::getInstance().logError("HttpConnectionJob: Failed to allocate WriteJob from pool");
        pending_writes_--;  // Decrement since allocation failed
        closeConnection();
    }
}

void HttpConnectionJob::closeConnection() {
    // Defer if writes are still in flight.
    if (pending_writes_ > 0) {
        close_pending_ = true;
        Logger::getInstance().logMessage("HttpConnectionJob: Close deferred, pending writes=" +
                                         std::to_string(pending_writes_) + " fd=" + std::to_string(client_fd_));
        return;
    }

    close_pending_ = true;
    bool waiting = false;

    // Cancel the idle-wait timer if one is armed. Submitted independently of the
    // recv cancel below — both legs may be outstanding at once.
    if (active_idle_timeout_job_ != nullptr) {
        if (!idle_cancel_pending_) {
            if (submitIdleTimeoutCancel()) {
                idle_cancel_pending_ = true;
            } else {
                // Cancel submission failed (pool or SQE exhausted) — fall through;
                // the timer will fire naturally later and no-op against a closed fd.
                Logger::getInstance().logError("HttpConnectionJob: Failed to submit idle-timeout cancel");
                active_idle_timeout_job_ = nullptr;
            }
        }
        if (active_idle_timeout_job_ != nullptr) {
            waiting = true;
        }
    }

    // If a multishot recv is still active and no cancel has been submitted yet,
    // cancel it and defer the actual fd close until the cancel completes.
    // Set client_fd_ = -1 immediately so any buffered recv completions arriving
    // before the cancel fires are discarded by handleDataReceived().
    if (active_read_job_ != nullptr) {
        if (!read_cancel_pending_) {
            deferred_close_fd_ = client_fd_;
            client_fd_ = -1;
            if (submitRecvCancel()) {
                read_cancel_pending_ = true;
            } else {
                // Cancel submission failed (pool or SQE exhausted) — fall through and
                // close the fd immediately; the recv will terminate naturally with EBADF.
                Logger::getInstance().logError("HttpConnectionJob: Failed to submit recv cancel, closing fd immediately");
                active_read_job_ = nullptr;
            }
        }
        if (active_read_job_ != nullptr) {
            waiting = true;
        }
    }

    if (waiting) {
        Logger::getInstance().logMessage("HttpConnectionJob: Close deferred pending cancel completion(s) fd=" +
                                         std::to_string(deferred_close_fd_ >= 0 ? deferred_close_fd_ : client_fd_));
        return;
    }

    // Perform the actual close.
    int fd_to_close = (deferred_close_fd_ >= 0) ? deferred_close_fd_ : client_fd_;
    if (fd_to_close >= 0) {
        Logger::getInstance().logMessage("[ACCESS] DISCONNECT fd=" + std::to_string(fd_to_close));
        close(fd_to_close);
    }

    client_fd_ = -1;
    deferred_close_fd_ = -1;
    reading_active_ = false;
    close_pending_ = false;
    read_cancel_pending_ = false;
    idle_cancel_pending_ = false;
    active_read_job_ = nullptr;

    // Do not access 'this' after this point.
    PoolManager::deallocate(this);
}

bool HttpConnectionJob::submitRecvCancel() {
    auto* cancel_job = PoolManager::allocate<CancelJob>(
        reinterpret_cast<uint64_t>(active_read_job_));
    if (!cancel_job) {
        return false;
    }
    struct io_uring_sqe* sqe = job_server_.registerJob(cancel_job);
    if (sqe) {
        cancel_job->prepareSqe(sqe);
        job_server_.submit();
        return true;
    }
    PoolManager::deallocate(cancel_job);
    return false;
}

void HttpConnectionJob::armIdleTimeout() {
    if (idle_timeout_ms_ == 0 || client_fd_ < 0) {
        return;
    }

    auto* timeout_job = PoolManager::allocate<IdleTimeoutJob>(this, idle_timeout_ms_);
    if (!timeout_job) {
        Logger::getInstance().logError("HttpConnectionJob: Failed to allocate IdleTimeoutJob from pool");
        return; // Non-fatal: this wait cycle simply has no idle timeout.
    }

    struct io_uring_sqe* sqe = job_server_.registerJob(timeout_job);
    if (!sqe) {
        Logger::getInstance().logError("HttpConnectionJob: Failed to register IdleTimeoutJob");
        PoolManager::deallocate(timeout_job);
        return;
    }

    timeout_job->prepareSqe(sqe);
    active_idle_timeout_job_ = timeout_job;
    idle_cancel_pending_ = false;
    job_server_.submit();
}

bool HttpConnectionJob::submitIdleTimeoutCancel() {
    auto* cancel_job = PoolManager::allocate<CancelJob>(
        reinterpret_cast<uint64_t>(active_idle_timeout_job_));
    if (!cancel_job) {
        return false;
    }
    struct io_uring_sqe* sqe = job_server_.registerJob(cancel_job);
    if (sqe) {
        cancel_job->prepareSqe(sqe);
        job_server_.submit();
        return true;
    }
    PoolManager::deallocate(cancel_job);
    return false;
}

void HttpConnectionJob::handleIdleTimeout(IdleTimeoutJob* job, int res) {
    if (job != active_idle_timeout_job_) {
        // Stale completion from a timer that was already superseded by a
        // fresher one (its cancel was submitted, but this completion arrived
        // after a new timer was armed). The current timer is still live and
        // responsible for the connection's fate — ignore this one.
        return;
    }
    active_idle_timeout_job_ = nullptr;

    if (res == -ECANCELED) {
        // Either activity resumed (data arrived) or the connection is closing for
        // another reason. If we're mid-close, re-enter to check whether the other
        // leg (recv cancel) has also drained.
        if (close_pending_) {
            closeConnection();
        }
        return;
    }

    if (client_fd_ < 0 && deferred_close_fd_ < 0) {
        // Stale completion after the connection already fully closed.
        return;
    }

    Logger::getInstance().logMessage("HttpConnectionJob: Idle timeout fd=" +
                                     std::to_string(client_fd_ >= 0 ? client_fd_ : deferred_close_fd_) +
                                     ", res=" + std::to_string(res));
    closeConnection();
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
    // The multishot recv armed by start()/startReading() persists automatically
    // across requests — it keeps delivering completions until cancelled or it
    // hits an error/EOF, so there's nothing to re-arm here. (Re-arming would
    // orphan the original recv job: it would keep holding a kernel reference
    // on client_fd_ that nothing tracks or cancels, deferring the real socket
    // teardown indefinitely once the connection eventually closes.)
    // Just start a fresh idle-wait window for the next request.
    Logger::getInstance().logMessage("HttpConnectionJob: Continuing reading for keep-alive connection fd=" +
                                   std::to_string(client_fd_));
    armIdleTimeout();
}

// ============================================================================
// HttpServer Configuration
// ============================================================================

void SingleRingHttpServer::setRouter(const HttpRouter& router) {
    router_ = router;
}

void SingleRingHttpServer::setKTLSContext(SSL_CTX* ssl_ctx, bool take_ownership) {
    ssl_ctx_ = ssl_ctx;
    owns_ssl_ctx_ = take_ownership;
}

bool SingleRingHttpServer::listenOnFd(int server_fd) {
    server_fd_ = server_fd;
    running_ = true;
    ktls_enabled_ = true;  // Assume KTLS if using this method

    Logger::getInstance().logMessage("HttpServer: Listening on fd=" + std::to_string(server_fd));

    job_server_.setShutdownSweepFn([this](Server& server) {
        PoolManager::sweepLive<HttpMultishotRecvJob>([&](HttpMultishotRecvJob& job) {
            job.requestShutdownCancel(server);
        });
        PoolManager::sweepLive<IdleTimeoutJob>([&](IdleTimeoutJob& job) {
            job.requestShutdownCancel(server);
        });
        if (accept_job_) {
            accept_job_->requestShutdownCancel(server);
            accept_job_ = nullptr;
        }
    });

    // Start accepting connections
    startAccepting();

    return true;
}

} // namespace caduvelox
