#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/jobs/KTLSContextHelper.hpp"
#include "caduvelox/jobs/KTLSJob.hpp"
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <thread>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace caduvelox {

HttpServer::HttpServer(const ServerConfig& cfg)
    : config_(cfg),
      ssl_ctx_(nullptr),
      state_(ServerState::Stopped) {

    // Resolve num_rings=0 (auto-detect) before anything else.
    if (config_.num_rings <= 0) {
        config_.num_rings = static_cast<int>(std::thread::hardware_concurrency());
    }

    // Apply pool capacities from config before any ring threads are started.
    // Thread-local pools are initialised on first access, so these writes are
    // visible to every ring thread created during listenKTLS().
    PoolManager::setCapacity<KTLSJob>(config_.ktls_pool_size);
    PoolManager::setCapacity<AcceptJob>(config_.accept_pool_size);
    PoolManager::setCapacity<WriteJob>(config_.write_pool_size);
    PoolManager::setCapacity<HTTPFileJob>(config_.file_job_pool_size);
    PoolManager::setCapacity<HttpConnectionJob>(config_.connection_pool_size);
    PoolManager::setCapacity<HttpMultishotRecvJob>(config_.connection_pool_size);

    // Log startup resource footprint.
    const size_t buf_mb = (static_cast<size_t>(config_.buffer_ring_count) *
                           config_.buffer_size_bytes) / (1024 * 1024);
    Logger::getInstance().logMessage("HttpServer: Creating server with " +
                                     std::to_string(config_.num_rings) + " service rings");
    Logger::getInstance().logMessage("HttpServer: Buffer ring: " +
                                     std::to_string(config_.buffer_ring_count) + " x " +
                                     std::to_string(config_.buffer_size_bytes) + " B = " +
                                     std::to_string(buf_mb) + " MB per ring");
    Logger::getInstance().logMessage("HttpServer: kTLS pool: " +
                                     std::to_string(config_.ktls_pool_size) + " slots");
    Logger::getInstance().logMessage("HttpServer: Connection pool: " +
                                     std::to_string(config_.connection_pool_size) + " slots");
}

HttpServer::HttpServer(int num_rings, unsigned queue_depth)
    : HttpServer([&]{
        ServerConfig cfg;
        cfg.num_rings    = num_rings;
        cfg.queue_depth  = queue_depth;
        return cfg;
    }()) {
}

HttpServer::~HttpServer() {
    stop();
    
    if (ssl_ctx_) {
        KTLSContextHelper::freeContext(ssl_ctx_);
    }
}

void HttpServer::addRoute(const std::string& method, const std::string& path_pattern,
                                    HttpHandler handler) {
    router_.addRoute(method, path_pattern, std::move(handler));
}

bool HttpServer::listenKTLS(int port, const std::string& cert_path, 
                                      const std::string& key_path,
                                      const std::string& bind_addr) {
    if (!isStopped()) {
        Logger::getInstance().logError("HttpServer: Server is not in Stopped state");
        return false;
    }

    // Create SSL context for KTLS
    ssl_ctx_ = KTLSContextHelper::createServerContext(cert_path, key_path);
    if (!ssl_ctx_) {
        Logger::getInstance().logError("HttpServer: Failed to create SSL context");
        return false;
    }

    // Create service rings - each will have its own listening socket
    service_rings_.reserve(config_.num_rings);
    http_servers_.reserve(config_.num_rings);

    for (int i = 0; i < config_.num_rings; ++i) {
        // Create service ring with CPU affinity and runtime buffer config
        int cpu_id = i % static_cast<int>(std::thread::hardware_concurrency());
        auto ring = std::make_unique<ServiceRing>(i, cpu_id, config_.queue_depth,
                                                  config_.buffer_ring_count,
                                                  config_.buffer_size_bytes);
        
        if (!ring->init()) {
            Logger::getInstance().logError("HttpServer: Failed to initialize service ring " + 
                                          std::to_string(i));
            return false;
        }
        
        // Create listening socket with SO_REUSEPORT for this ring
        int server_fd = createServerSocket(port, bind_addr);
        if (server_fd < 0) {
            Logger::getInstance().logError("HttpServer: Failed to create socket for ring " + 
                                          std::to_string(i));
            return false;
        }
        
        // Create internal SingleRingHttpServer for this ring
        // HTTP processing happens inline on io_uring thread for maximum performance
        auto http_server = std::make_unique<SingleRingHttpServer>(ring->getServer());
        
        // Share the router (read-only after setup)
        http_server->setRouter(router_);
        
        // Set KTLS context (borrowed reference - parent HttpServer owns it)
        http_server->setKTLSContext(ssl_ctx_, false);

        // Apply per-ring runtime config
        http_server->setKtlsHandshakeTimeoutMs(config_.ktls_handshake_timeout_ms);
        
        // Start listening on this ring's socket
        // We manually set the socket since we created it with SO_REUSEPORT
        // HttpServer will do the accept multishot on this fd
        if (!http_server->listenOnFd(server_fd)) {
            Logger::getInstance().logError("HttpServer: Failed to start listening on ring " + 
                                          std::to_string(i));
            close(server_fd);
            return false;
        }
        
        // Share the canonical server state so ring-local jobs can observe it.
        ring->bindToServerState(&state_);

        service_rings_.push_back(std::move(ring));
        http_servers_.push_back(std::move(http_server));
        
        Logger::getInstance().logMessage("HttpServer: Ring " + std::to_string(i) + 
                                        " initialized and listening");
    }

    state_.store(ServerState::Running, std::memory_order_release);
    
    Logger::getInstance().logMessage("HttpServer: All " + std::to_string(config_.num_rings) + 
                                    " rings listening on " + bind_addr + ":" + std::to_string(port));
    
    return true;
}

void HttpServer::run() {
    if (!isRunning()) {
        Logger::getInstance().logError("HttpServer: Server not initialized");
        return;
    }

    // Start all service ring threads - each runs its own accept + processing loop
    for (auto& ring : service_rings_) {
        ring->start();
    }
    
    Logger::getInstance().logMessage("HttpServer: All service rings started");

    // Wait for all service rings to finish
    for (auto& ring : service_rings_) {
        ring->join();
    }

    state_.store(ServerState::Stopped, std::memory_order_release);
    
    Logger::getInstance().logMessage("HttpServer: All service rings stopped");
}

void HttpServer::stop() {
    ServerState expected = ServerState::Running;
    if (!state_.compare_exchange_strong(expected, ServerState::Stopping,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        return;
    }

    // Stop all HttpServers (which stops accepting)
    for (auto& http_server : http_servers_) {
        if (http_server) {
            http_server->stop();
        }
    }

    // Stop all service rings
    for (auto& ring : service_rings_) {
        if (ring) {
            ring->stop();
        }
    }

    // If run() has not started the ring threads yet, or if they have already
    // fully unwound, there is no join path left to transition us to Stopped.
    bool any_ring_running = false;
    for (const auto& ring : service_rings_) {
        if (ring && ring->isRunning()) {
            any_ring_running = true;
            break;
        }
    }

    if (!any_ring_running) {
        state_.store(ServerState::Stopped, std::memory_order_release);
    }

    Logger::getInstance().logMessage("HttpServer: Server stopped");
}

int HttpServer::createServerSocket(int port, const std::string& bind_addr) {
    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        Logger::getInstance().logError("HttpServer: Failed to create socket: " + 
                                      std::string(strerror(errno)));
        return -1;
    }

    // Set SO_REUSEADDR and SO_REUSEPORT - critical for multi-ring!
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to set SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to set SO_REUSEPORT");
        close(server_fd);
        return -1;
    }

    // Bind to address
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int inet_result = inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr);
    if (inet_result == 0) {
        Logger::getInstance().logError("HttpServer: Invalid IPv4 address format: " + bind_addr);
        close(server_fd);
        return -1;
    } else if (inet_result < 0) {
        Logger::getInstance().logError("HttpServer: inet_pton failed for address " + bind_addr +
                                       ": " + std::string(strerror(errno)));
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to bind to port " + 
                                      std::to_string(port) + ": " + std::string(strerror(errno)));
        close(server_fd);
        return -1;
    }

    // Start listening
    if (listen(server_fd, SOMAXCONN) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to listen: " + 
                                      std::string(strerror(errno)));
        close(server_fd);
        return -1;
    }

    return server_fd;
}

} // namespace caduvelox
