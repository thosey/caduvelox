#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/http/HttpResponseWriter.hpp"
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
    Logger::info("HttpServer: Listening on " + bind_addr + ":" + std::to_string(port));

    installRingLocalHooks();

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
    Logger::info("HttpServer: KTLS listening on " + bind_addr + ":" + std::to_string(port));

    installRingLocalHooks();

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

    Logger::info("HttpServer: Server stopped");
}

// Both hooks this ring needs, installed by every listen path.
//
// The shutdown sweep cancels idle keep-alive connections (and the accept) when
// the server enters Stopping/Aborting, so a quiet connection does not hold the
// ring open.
//
// The accept is armed from the startup function rather than here, because
// Server::run() calls that on the ring thread. AcceptJob is pool-allocated and
// the pools are thread-local, so arming it from the caller's thread -- which is
// where listen() runs, before any ring thread exists -- would take the job from
// the caller's pool and free it into the ring's. That is also how the job came
// to leak: HttpServer::listenKTLS() creates every ring's accept up front on the
// main thread, and a job stranded there has no pool to return to.
//
// This used to be three identical copies, one per listen path.
void SingleRingHttpServer::installRingLocalHooks() {
    // Setup that arrives after run() has begun is simply never seen: the startup
    // function below is read once, at the top of the loop. Silently never
    // accepting is a poor way to find that out.
    if (job_server_.isLoopRunning()) {
        Logger::getInstance().logError(
            "HttpServer: listen() was called after the event loop started. The accept "
            "cannot be armed from this thread, and the startup hook that would arm it "
            "has already been passed, so this ring will not accept connections. Call "
            "listen() before run().");
    }

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

    job_server_.setStartupFn([this](Server&) {
        startAccepting();
    });
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

    // Guard against pool exhaustion
    if (!accept_job) {
        Logger::getInstance().logError("[POOL_EXHAUSTED] type=AcceptJob capacity=" +
            std::to_string(PoolManager::getCapacity<AcceptJob>()));
        // Try again after a brief delay - this is a critical operation
        // In production, you might want exponential backoff or alerting
        return;
    }

    struct io_uring_sqe* sqe = job_server_.registerJob(accept_job);
    if (sqe) {
        accept_job->prepareSqe(sqe);
        // Track the job only once it is actually armed. The job nulls this
        // slot itself when it terminates and self-deletes, so the shutdown
        // sweep never touches a freed AcceptJob.
        accept_job->bindOwnerSlot(&accept_job_);
        accept_job_ = accept_job;
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
    
    Logger::info(access_log);
    
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
            Logger::getInstance().logError("[POOL_EXHAUSTED] type=KTLSJob capacity=" +
                std::to_string(PoolManager::getCapacity<KTLSJob>()) +
                " fd=" + std::to_string(client_fd));
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
        Logger::getInstance().logError("[POOL_EXHAUSTED] type=HttpConnectionJob capacity=" +
            std::to_string(PoolManager::getCapacity<HttpConnectionJob>()) +
            " fd=" + std::to_string(client_fd));
        close(client_fd);
        return;
    }
    
    // Start the connection job - it will manage itself via cleanup callbacks
    connection_job->start();
    
    // The HttpConnectionJob will stay alive until closeConnection() is called,
    // at which point the cleanup callback returns it to the pool
}

void SingleRingHttpServer::handleKTLSReady(int client_fd, SSL* ssl) {
    Logger::debug("HttpServer: KTLS ready for fd=" + std::to_string(client_fd));
    
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
    , response_in_flight_(false)
    , close_pending_(false)
    , active_read_job_(nullptr)
    , read_cancel_pending_(false)
    , deferred_close_fd_(-1)
    , active_idle_timeout_job_(nullptr)
    , idle_cancel_pending_(false)
{
    
    request_buffer_.reserve(8192);
    Logger::debug("HttpConnectionJob: Created for fd=" + std::to_string(client_fd_));
    
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

    // Create handler instance (encapsulates context + callbacks).
    // Capture this connection's pool generation so late recv completions
    // arriving after this connection is freed/recycled are dropped.
    HttpConnectionRecvHandler handler{this, PoolManager::generation<HttpConnectionJob>(this)};

    // Zero-copy path: use token-based MultishotRecvJob for inline processing on io_uring thread
    // Template policy pattern - type-safe, fully inlineable callbacks
    auto* read_job = PoolManager::allocate<MultishotRecvJob<HttpConnectionRecvHandler>>(
        client_fd_,
        handler,
        *job_server_.getBufferRingCoordinator()
    );

    if (!read_job) {
        Logger::getInstance().logError("[POOL_EXHAUSTED] type=MultishotRecvJob capacity=" +
            std::to_string(PoolManager::getCapacity<HttpMultishotRecvJob>()) +
            " fd=" + std::to_string(client_fd_));
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
        Logger::debug("HttpConnectionJob: Client disconnected fd=" + std::to_string(client_fd_));
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
        // Answer rather than close silently, and say which half is too big: if the
        // blank line ending the header section has not arrived, it is the headers,
        // otherwise it is the body. Best effort -- a terminator could straddle the
        // boundary between what is buffered and what just arrived -- but it is
        // right in every case that matters, and either answer beats none.
        if (request_buffer_.find("\r\n\r\n") == std::string::npos) {
            sendErrorAndClose(431, "Request Header Fields Too Large");
        } else {
            sendErrorAndClose(413, "Content Too Large");
        }
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
        Logger::debug("HttpConnectionJob: Recv cancelled (shutdown) fd=" +
                                         std::to_string(client_fd_));
    } else {
        Logger::getInstance().logError("HttpConnectionJob: Read error fd=" + std::to_string(client_fd_) +
                                       ", error=" + std::to_string(error));
    }
    closeConnection();
}

void HttpConnectionJob::processHttpRequests() {
    // Handles at most ONE request per call.
    //
    // This used to loop over every complete request in the buffer, issuing one
    // WriteJob per response. Those SQEs are independent, and io_uring gives no
    // ordering guarantee between them: if one write blocks and is punted to an
    // async worker while the next proceeds inline, the responses interleave on
    // the socket and the HTTP stream is corrupt. The pipelining tests passed
    // only because small writes to a healthy socket complete inline — which is
    // an observation about the happy path, not a contract.
    //
    // Now one response is in flight at a time and the next pipelined request is
    // not parsed until the current one is on the wire. onResponseComplete()
    // calls back in to pick up whatever is left in the buffer.
    if (response_in_flight_ || client_fd_ < 0 || request_buffer_.empty()) {
        return;
    }

    HttpRequest request;
    size_t bytes_consumed = 0;

    auto result = HttpParser::parse_request(
        request_buffer_,
        request,
        bytes_consumed
    );

    if (result == HttpParser::ParseResult::Success) {
        // Complete request parsed successfully.
        request_buffer_.erase(0, bytes_consumed);
        // handleHttpRequest() can close and deallocate this connection on a
        // pool-exhaustion path, so nothing may touch a member after it returns.
        // (The old loop did exactly that, and re-read request_buffer_ on freed
        // memory when a response failed to allocate.)
        handleHttpRequest(request);
        return;
    }

    if (result == HttpParser::ParseResult::Incomplete) {
        // Need more data - wait for next read
        return;
    }

    // BadRequest - malformed input. Answer 400 before closing: this used to be a
    // bare close, which to a client is indistinguishable from a network fault.
    sendErrorAndClose(400, "Bad Request");
}

// Answer a request being refused, then close the connection.
//
// Parse failures used to log and close with nothing sent. Since the parser became
// strict that is the response to a great many deliberate rejections -- a field
// line without a colon, whitespace before one, an obs-fold continuation, a bare
// LF in a value, a bad version, a contradictory Content-Length, any
// Transfer-Encoding -- and every one of them looked like the network breaking.
// RFC 9112 section 3 asks for the status first.
//
// This response is the last thing on the connection, by necessity rather than
// choice: once framing is untrustworthy the stream is not known to be at a
// request boundary, so anything already buffered is dropped instead of parsed --
// an attacker would otherwise choose what came next. Clearing keep_alive_ makes
// sendResponse() advertise "connection: close" and onResponseComplete() tear the
// connection down once the bytes are away.
void HttpConnectionJob::sendErrorAndClose(int status_code, const std::string& reason) {
    Logger::getInstance().logError("HttpConnectionJob: refusing request fd=" +
                                   std::to_string(client_fd_) + ": " +
                                   std::to_string(status_code) + " " + reason);

    keep_alive_ = false;
    advertise_keep_alive_ = false;
    request_buffer_.clear();

    if (client_fd_ < 0 || response_in_flight_) {
        // Nothing can be written right now, so skip straight to teardown.
        closeConnection();
        return;
    }

    HttpResponse res;
    res.setStatus(status_code, reason);
    res.setHeader("content-type", "text/plain");
    res.setBody(reason);
    sendResponse(std::move(res));
}

void HttpConnectionJob::onResponseComplete(bool keep_alive) {
    response_in_flight_ = false;

    // A close requested while the response was on the wire runs now.
    if (close_pending_) {
        closeConnection();
        return;
    }

    if (!keep_alive || job_server_.isStopping()) {
        closeConnection();
        return;
    }

    // Pipelined requests already sitting in the buffer are answered straight
    // away; arming an idle timer we would cancel on the next byte is pointless.
    if (!request_buffer_.empty()) {
        processHttpRequests();
        return;
    }

    continueReading();
}

void HttpConnectionJob::handleHttpRequest(const HttpRequest& request) {
    Logger::debug("HttpConnectionJob: Processing " + request.method + " " + request.path);
    
    // Determine if we should keep connection alive after this response
    keep_alive_ = shouldKeepAlive(request);
    advertise_keep_alive_ = request.version != "HTTP/1.1";
    
    // This method is called when NOT using affinity workers (fallback)
    HttpResponse response;
    router_.dispatch(request, response);
    
    Logger::debug("HttpConnectionJob: Response generated, status=" + std::to_string(response.status_code));
    
    sendResponse(std::move(response));
}

void HttpConnectionJob::sendResponse(HttpResponse response) {
    Logger::debug("HttpConnectionJob: Sending response, status=" + std::to_string(response.status_code));

    // Decide once whether this connection outlives the response, then state it
    // on the response -- every kind of response, not just file responses, which
    // were the only ones that used to say "connection: close". A body response
    // to "Connection: close" closed correctly but told an HTTP/1.1 client the
    // connection persisted.
    //
    // A handler may close a connection the client wanted kept (it sets
    // "connection: close"), but it cannot claim persistence the server is not
    // going to give: whatever a handler put in the field is replaced by the
    // decision. A server that is already stopping will close after this
    // response, so it says so.
    if (HttpParser::list_contains_token(response.getHeader("connection"), "close") ||
        job_server_.isStopping()) {
        keep_alive_ = false;
    }
    if (!keep_alive_) {
        response.setHeader("connection", "close");
    } else if (advertise_keep_alive_) {
        response.setHeader("connection", "keep-alive");
    } else {
        // HTTP/1.1 persistence is the default and needs no field. Anything a
        // handler wrote here (other than close, handled above) is not ours to
        // promise -- this server negotiates no connection options.
        response.headers.erase("connection");
    }
    
    // Check if this is a file serving response (internal flag only, not sent to client)
    if (!response.file_path.empty()) {
        // This is a file serving request - use HTTPFileJob for zero-copy transfer
        Logger::debug("HttpConnectionJob: Using HTTPFileJob for file: " + response.file_path);
        
        // The connection field was already set above, for this path and the body
        // path alike.
        HttpResponse response_copy = response;

        std::string file_path = response.file_path;

        // Claim the connection's single response slot for the file transfer.
        response_in_flight_ = true;

        auto http_file_job = HTTPFileJob::createFromPool(
            client_fd_,
            file_path,
            std::move(response_copy), // Pass the response for any custom headers
            [this, keep_alive = keep_alive_](int fd, size_t bytes_sent) {
                Logger::debug("HttpConnectionJob: File transfer complete fd=" + 
                                               std::to_string(fd) + ", bytes=" + std::to_string(bytes_sent));

                onResponseComplete(keep_alive);
            },
            [this](int fd, int error) {
                Logger::getInstance().logError("HttpConnectionJob: File transfer error fd=" + 
                                             std::to_string(fd) + ", error=" + std::to_string(error));

                // Release the response slot; the connection is going away anyway.
                response_in_flight_ = false;
                closeConnection();
            }
        );
        
        if (http_file_job) {
            Logger::debug("HttpConnectionJob: HTTPFileJob allocated, starting...");
            // HTTPFileJob is a composite job - start it directly (it creates child jobs for io_uring)
            http_file_job->start(job_server_);
            Logger::debug("HttpConnectionJob: HTTPFileJob started");
        } else {
            Logger::getInstance().logError("[POOL_EXHAUSTED] type=HTTPFileJob capacity=" +
                std::to_string(PoolManager::getCapacity<HTTPFileJob>()) +
                " fd=" + std::to_string(client_fd_));
            response_in_flight_ = false;  // Release the slot since allocation failed
            closeConnection();
        }
        return;
    }
    
    // Regular response - use WriteJob
    //
    // build_response_head() is the one place a response head becomes bytes (see
    // HttpResponseWriter.hpp). This used to write the map verbatim and add its own
    // Content-Length only when an exact-case "content-length" key was missing --
    // so a handler's "Content-Length" went out alongside a second one, and a
    // correctly-cased one went out even when it did not match the body.
    std::string response_str;
    if (build_response_head(response, response.body.size(), response_str)) {
        response_str += response.body;
    } else {
        Logger::getInstance().logError(
            "HttpConnectionJob: response cannot be written safely, sending 500 fd=" +
            std::to_string(client_fd_));
        response_str = fallback_error_response(!keep_alive_);
    }
    
    // Create owned data for WriteJob
    auto response_data = std::make_unique<char[]>(response_str.size());
    std::memcpy(response_data.get(), response_str.data(), response_str.size());

    // Claim the connection's single response slot BEFORE creating the WriteJob.
    response_in_flight_ = true;
    
    auto write_job = WriteJob::createFromPoolWithOwnedData(
        client_fd_,
        std::move(response_data),
        response_str.size(),
        // keep_alive is captured by value: it is THIS request's decision. Reading
        // the keep_alive_ member at completion time was wrong under pipelining,
        // where a later request had already overwritten it.
        [this, keep_alive = keep_alive_](int fd, size_t bytes_written) {
            Logger::debug("HttpConnectionJob: Response sent fd=" + std::to_string(fd) + 
                                           ", bytes=" + std::to_string(bytes_written));

            onResponseComplete(keep_alive);
        },
        [this](int fd, int error) {
            Logger::getInstance().logError("HttpConnectionJob: Write error fd=" + std::to_string(fd) + 
                                         ", error=" + std::to_string(error));

            // Release the response slot; the connection is going away anyway.
            response_in_flight_ = false;
            closeConnection();
        }
    );

    if (!write_job) {
        Logger::getInstance().logError("[POOL_EXHAUSTED] type=WriteJob capacity=" +
            std::to_string(PoolManager::getCapacity<WriteJob>()) +
            " fd=" + std::to_string(client_fd_));
        response_in_flight_ = false;  // Release the slot since allocation failed
        closeConnection();
        return;
    }

    // start() owns write_job from here: it either queues the write or frees the
    // job and runs the error callback above, which releases the response slot
    // and closes the connection -- the same teardown this branch used to
    // open-code. It also retries after flushing a full submission queue, which
    // the open-coded version did not. This connection may be gone on return.
    Logger::debug("HttpConnectionJob: WriteJob allocated, submitting...");
    write_job->start(job_server_);
}

void HttpConnectionJob::closeConnection() {
    // Defer if a response is still on the wire. onResponseComplete() sees
    // close_pending_ and calls back in once the socket is quiet.
    if (response_in_flight_) {
        close_pending_ = true;
        Logger::debug("HttpConnectionJob: Close deferred, response in flight fd=" +
                                         std::to_string(client_fd_));
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
                // the timer will fire naturally later, and its pool-generation check
                // on this (by then freed) connection makes it a true no-op.
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
                // close our fd reference now. Note: this does NOT terminate the armed
                // multishot recv; an in-flight io_uring op holds its own reference to
                // the socket, so it stays parked until the peer sends data, closes, or
                // the ring shuts down. When its completion eventually fires, the recv
                // handler's pool-generation check sees this connection is gone and
                // drops it, and the recv job frees itself on the terminal completion.
                Logger::getInstance().logError("HttpConnectionJob: Failed to submit recv cancel, closing fd immediately");
                active_read_job_ = nullptr;
            }
        }
        if (active_read_job_ != nullptr) {
            waiting = true;
        }
    }

    if (waiting) {
        Logger::debug("HttpConnectionJob: Close deferred pending cancel completion(s) fd=" +
                                         std::to_string(deferred_close_fd_ >= 0 ? deferred_close_fd_ : client_fd_));
        return;
    }

    // Perform the actual close.
    int fd_to_close = (deferred_close_fd_ >= 0) ? deferred_close_fd_ : client_fd_;
    if (fd_to_close >= 0) {
        Logger::info("[ACCESS] DISCONNECT fd=" + std::to_string(fd_to_close));
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
        Logger::getInstance().logError("[POOL_EXHAUSTED] type=IdleTimeoutJob capacity=" +
            std::to_string(PoolManager::getCapacity<IdleTimeoutJob>()) +
            " fd=" + std::to_string(client_fd_));
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

    Logger::debug("HttpConnectionJob: Idle timeout fd=" +
                                     std::to_string(client_fd_ >= 0 ? client_fd_ : deferred_close_fd_) +
                                     ", res=" + std::to_string(res));
    closeConnection();
}

bool HttpConnectionJob::shouldKeepAlive(const HttpRequest& request) const {
    // Connection is a comma-separated list of options (RFC 9110 section 7.6.1).
    // This compared the whole value against exactly "close" or "keep-alive", so
    // "Connection: close, TE" matched neither and fell through to the HTTP/1.1
    // default -- and the server went on to answer requests pipelined after a
    // close, which RFC 9112 section 9.6 says it must not process.
    const std::string options = request.getHeader("connection");

    // close wins over everything, including a keep-alive in the same list.
    if (HttpParser::list_contains_token(options, "close")) {
        return false;
    }
    if (request.version == "HTTP/1.1") {
        return true;
    }
    // HTTP/1.0 closes unless the client asked otherwise.
    return request.version == "HTTP/1.0" && HttpParser::list_contains_token(options, "keep-alive");
}

void HttpConnectionJob::continueReading() {
    // The multishot recv armed by start()/startReading() persists automatically
    // across requests — it keeps delivering completions until cancelled or it
    // hits an error/EOF, so there's nothing to re-arm here. (Re-arming would
    // orphan the original recv job: it would keep holding a kernel reference
    // on client_fd_ that nothing tracks or cancels, deferring the real socket
    // teardown indefinitely once the connection eventually closes.)
    // Just start a fresh idle-wait window for the next request.
    Logger::debug("HttpConnectionJob: Continuing reading for keep-alive connection fd=" +
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

    Logger::info("HttpServer: Listening on fd=" + std::to_string(server_fd));

    installRingLocalHooks();

    return true;
}

} // namespace caduvelox
