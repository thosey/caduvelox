#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/jobs/KTLSContextHelper.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <thread>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace caduvelox {

HttpServer::HttpServer(int num_rings, unsigned queue_depth)
    : num_rings_(num_rings > 0 ? num_rings : std::thread::hardware_concurrency()),
      queue_depth_(queue_depth),
      ssl_ctx_(nullptr),
      running_(false) {
    
    Logger::getInstance().logMessage("HttpServer: Creating server with " + 
                                    std::to_string(num_rings_) + " service rings");
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
    if (running_) {
        Logger::getInstance().logError("HttpServer: Server is already running");
        return false;
    }

    // Create SSL context for KTLS
    ssl_ctx_ = KTLSContextHelper::createServerContext(cert_path, key_path);
    if (!ssl_ctx_) {
        Logger::getInstance().logError("HttpServer: Failed to create SSL context");
        return false;
    }

    // Create service rings - each will have its own listening socket
    service_rings_.reserve(num_rings_);
    http_servers_.reserve(num_rings_);
    
    for (int i = 0; i < num_rings_; ++i) {
        // Create service ring with CPU affinity
        int cpu_id = i % std::thread::hardware_concurrency();
        auto ring = std::make_unique<ServiceRing>(i, cpu_id, queue_depth_);
        
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
        
        // Create internal SingleRingHttpServer for this ring WITHOUT worker pool
        // HTTP processing happens inline on io_uring thread for maximum performance
        auto http_server = std::make_unique<SingleRingHttpServer>(ring->getServer(), nullptr);
        
        // Share the router (read-only after setup)
        http_server->setRouter(router_);
        
        // Set KTLS context
        http_server->setKTLSContext(ssl_ctx_);
        
        // Start listening on this ring's socket
        // We manually set the socket since we created it with SO_REUSEPORT
        // HttpServer will do the accept multishot on this fd
        if (!http_server->listenOnFd(server_fd)) {
            Logger::getInstance().logError("HttpServer: Failed to start listening on ring " + 
                                          std::to_string(i));
            close(server_fd);
            return false;
        }
        
        service_rings_.push_back(std::move(ring));
        http_servers_.push_back(std::move(http_server));
        
        Logger::getInstance().logMessage("HttpServer: Ring " + std::to_string(i) + 
                                        " initialized and listening");
    }

    running_ = true;
    
    Logger::getInstance().logMessage("HttpServer: All " + std::to_string(num_rings_) + 
                                    " rings listening on " + bind_addr + ":" + std::to_string(port));
    
    return true;
}

void HttpServer::run() {
    if (!running_) {
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
    
    Logger::getInstance().logMessage("HttpServer: All service rings stopped");
}

void HttpServer::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

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
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) <= 0) {
        Logger::getInstance().logError("HttpServer: Invalid bind address: " + bind_addr);
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
